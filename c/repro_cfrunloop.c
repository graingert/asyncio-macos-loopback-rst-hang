/*
 * CoreFoundation reproducer for the lost TCP reset (RST) on macOS loopback,
 * using CFRunLoop + CFSocket -- Apple's own framework-level readiness API --
 * instead of kqueue/kevent or poll(2).
 *
 * The application never calls kevent() or poll(): each socket is wrapped in a
 * CFSocket (CFSocketCreateWithNative), turned into a CFRunLoopSource, and the
 * thread blocks in CFRunLoopRunInMode() waiting for read/write callbacks.
 * Internally CFSocket services all registered sockets from a single hidden
 * manager thread running select(2), so this variant demonstrates that the
 * hang (a) is reachable through the API Cocoa/CFRunLoop applications use, and
 * (b) is not a kqueue artifact -- consistent with c/repro_poll.c.
 *
 * Mirrors c/repro.c's "deferred close" pattern: a run-loop cycle runs between
 * invalidating the client's CFSocket and abortively closing the fd. (With
 * CFSocket the deferral is partly inherent: invalidation is delivered to the
 * select() manager thread asynchronously.)
 *
 * Each iteration:
 *   1. Establish a loopback client + accepted server socket, both non-blocking.
 *   2. Wrap the client in a CFSocket (read + write callbacks); on the write
 *      callback, fill until send() blocks against the non-reading server.
 *   3. Invalidate the client's CFSocket, run one run-loop cycle, then
 *      abortively close it: setsockopt(SO_LINGER {1,0}) + close().
 *   4. Wrap the server in a CFSocket (read callback); drain the buffered
 *      data, then wait for the disconnect (EOF / errno).
 *   5. If the disconnect never arrives within TIMEOUT, the reset was lost.
 *
 * Build:  cc -O2 -framework CoreFoundation -o repro_cfrunloop repro_cfrunloop.c
 * Run:    ./repro_cfrunloop [SECONDS] [stop] [-jN]
 *           SECONDS: how long to run (default 300)
 *           stop:    if present, exit at the first lost RST (for packet capture)
 *           -jN:     run N worker THREADS in parallel (default 1). Threads, not
 *                    fork(): CoreFoundation initializes at load time and is not
 *                    fork-safe, and a per-thread CFRunLoop is the native idiom.
 *
 * macOS-only (CoreFoundation). Exit status is non-zero if any lost RST is
 * observed.
 */
#include <CoreFoundation/CoreFoundation.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_SEC 1.0

static int g_stop_on_hang = 0;

/* Shared across worker threads. */
struct shared {
    unsigned long long delivered;
    unsigned long long undelivered;
    volatile int stop;
};
static struct shared g_sh;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void die(const char *msg) {
    perror(msg);
    exit(2);
}

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        die("fcntl(O_NONBLOCK)");
}

static void make_pair(int *client_out, int *server_out) {
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0)
        die("socket(listen)");
    int one = 1;
    if (setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        die("setsockopt(SO_REUSEADDR)");
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lsock, (struct sockaddr *)&a, sizeof(a)) < 0)
        die("bind");
    if (listen(lsock, 1) < 0)
        die("listen");

    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    if (getsockname(lsock, (struct sockaddr *)&bound, &blen) < 0)
        die("getsockname");

    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0)
        die("socket(client)");
    if (connect(client, (struct sockaddr *)&bound, sizeof(bound)) < 0)
        die("connect");
    int server = accept(lsock, NULL, NULL);
    if (server < 0)
        die("accept");
    close(lsock);

    set_nonblocking(client);
    set_nonblocking(server);
    *client_out = client;
    *server_out = server;
}

static int local_port(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    if (getsockname(fd, (struct sockaddr *)&a, &l) == 0)
        return ntohs(a.sin_port);
    return -1;
}

static int peer_port(int fd) {
    struct sockaddr_in a;
    socklen_t l = sizeof(a);
    if (getpeername(fd, (struct sockaddr *)&a, &l) == 0)
        return ntohs(a.sin_port);
    return -1;
}

/* Per-iteration state handed to the CFSocket callbacks via the context. */
struct iter {
    int client;
    int server;
    int filled; /* client's send buffer + peer window filled */
    int done;   /* server observed the disconnect (EOF / errno) */
    char *buf;
    size_t buflen;
};

static void client_cb(CFSocketRef s, CFSocketCallBackType type, CFDataRef addr,
                      const void *data, void *info) {
    (void)s;
    (void)addr;
    (void)data;
    struct iter *it = info;
    if (type != kCFSocketWriteCallBack || it->filled)
        return;
    /* Contents are irrelevant; we only need bytes to fill the send buffer.
     * Read-only, so safe to share across worker threads. */
    static const char chunk[65536];
    while (send(it->client, chunk, sizeof(chunk), 0) > 0) {
    }
    it->filled = 1;
    CFRunLoopStop(CFRunLoopGetCurrent());
}

static void server_cb(CFSocketRef s, CFSocketCallBackType type, CFDataRef addr,
                      const void *data, void *info) {
    (void)s;
    (void)addr;
    (void)data;
    struct iter *it = info;
    if (type != kCFSocketReadCallBack || it->done)
        return;
    ssize_t r = recv(it->server, it->buf, it->buflen, 0);
    if (r <= 0) { /* ECONNRESET / EOF */
        it->done = 1;
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
    /* else: drained buffered data; the read callback auto-re-enables. */
}

/* Wrap fd in a CFSocket, schedule it on the current run loop's default mode.
 * kCFSocketCloseOnInvalidate is cleared: the reproducer controls close()
 * timing itself (the abortive SO_LINGER close must happen AFTER invalidation
 * plus one run-loop cycle). */
static CFSocketRef watch(int fd, CFOptionFlags cbtypes, CFSocketCallBack cb,
                         struct iter *it, CFRunLoopSourceRef *src_out) {
    CFSocketContext ctx = {0, it, NULL, NULL, NULL};
    CFSocketRef s = CFSocketCreateWithNative(kCFAllocatorDefault, fd, cbtypes,
                                             cb, &ctx);
    if (!s)
        die("CFSocketCreateWithNative");
    CFSocketSetSocketFlags(s, CFSocketGetSocketFlags(s) &
                                  ~(CFOptionFlags)kCFSocketCloseOnInvalidate);
    CFRunLoopSourceRef src =
        CFSocketCreateRunLoopSource(kCFAllocatorDefault, s, 0);
    if (!src)
        die("CFSocketCreateRunLoopSource");
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    *src_out = src;
    return s;
}

/* Invalidate BEFORE close: CFSocket keeps a process-wide registry keyed by
 * native fd, and fd numbers are reused every iteration. */
static void unwatch(CFSocketRef s, CFRunLoopSourceRef src) {
    CFSocketInvalidate(s); /* also removes the source from the run loop */
    CFRelease(src);
    CFRelease(s);
}

/* Run the current run loop until *flag is set or the deadline passes. */
static void run_until(const int *flag, double deadline) {
    while (!*flag) {
        double rem = deadline - now_sec();
        if (rem <= 0)
            break;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, rem, true);
    }
}

/* returns 1 if the abort was NOT delivered (the hang), 0 if delivered. */
static int one_iteration(char *buf, size_t buflen, char *desc,
                         size_t desclen) {
    struct iter it;
    memset(&it, 0, sizeof(it));
    it.buf = buf;
    it.buflen = buflen;
    make_pair(&it.client, &it.server);

    /* Watch the client (read + write callbacks, as c/repro.c registers both
     * filters); fill until send() blocks. */
    CFRunLoopSourceRef csrc;
    CFSocketRef cs = watch(it.client,
                           kCFSocketReadCallBack | kCFSocketWriteCallBack,
                           client_cb, &it, &csrc);
    run_until(&it.filled, now_sec() + TIMEOUT_SEC);
    if (!it.filled) {
        unwatch(cs, csrc);
        close(it.client);
        close(it.server);
        return 0;
    }

    /* Deferred abort: invalidate the client's CFSocket, run one run-loop
     * cycle, then abortively close it. (CFSocket removal also has to reach
     * the select() manager thread asynchronously, deferring further.) */
    unwatch(cs, csrc);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0, true);
    struct linger lg = {1, 0};
    if (setsockopt(it.client, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) < 0)
        die("setsockopt(SO_LINGER)");
    int lport = local_port(it.server);
    int pport = peer_port(it.server);
    close(it.client);

    /* Watch the server; drain buffered data, then wait for the disconnect. */
    CFRunLoopSourceRef ssrc;
    CFSocketRef ss = watch(it.server, kCFSocketReadCallBack, server_cb, &it,
                           &ssrc);
    run_until(&it.done, now_sec() + TIMEOUT_SEC);
    int hung = !it.done;

    if (hung) {
        int so = 0;
        socklen_t sl = sizeof(so);
        getsockopt(it.server, SOL_SOCKET, SO_ERROR, &so, &sl);
        char pk[32];
        ssize_t r = recv(it.server, buf, 1, MSG_PEEK);
        if (r < 0)
            snprintf(pk, sizeof(pk), "errno %d", errno);
        else if (r == 0)
            snprintf(pk, sizeof(pk), "EOF");
        else
            snprintf(pk, sizeof(pk), "%zdbytes", r);
        snprintf(desc, desclen, "%d<->%d SO_ERROR=%d MSG_PEEK=%s", lport,
                 pport, so, pk);
        if (g_stop_on_hang) {
            /* Idle so the hung connection's abort window is isolated by a
             * clear multi-second gap of silence in a packet capture. */
            sleep(2);
        }
    }
    unwatch(ss, ssrc);
    close(it.server);
    return hung;
}

/* One worker thread: runs the loop on its own CFRunLoop until the deadline or
 * the shared stop flag, accumulating counts into the shared struct. */
static void *run_worker(void *arg) {
    double run_seconds = *(double *)arg;
    size_t buflen = 1 << 20;
    char *buf = malloc(buflen);
    if (!buf)
        die("malloc");
    char desc[128];
    double start = now_sec();

    while (now_sec() - start < run_seconds && !g_sh.stop) {
        if (one_iteration(buf, buflen, desc, sizeof(desc))) {
            unsigned long long idx =
                __sync_add_and_fetch(&g_sh.undelivered, 1);
            printf("  [%llu] UNDELIVERED: %s\n", idx + g_sh.delivered, desc);
            fflush(stdout);
            if (g_stop_on_hang) {
                g_sh.stop = 1;
                break;
            }
        } else {
            __sync_add_and_fetch(&g_sh.delivered, 1);
        }
    }
    free(buf);
    return NULL;
}

int main(int argc, char **argv) {
    double run_seconds = 300.0;
    int stop_on_hang = 0;
    int workers = 1;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "-j", 2) == 0) {
            workers = atoi(argv[i] + 2);
            if (workers < 1)
                workers = 1;
        } else if (strcmp(argv[i], "stop") == 0) {
            stop_on_hang = 1;
        } else {
            run_seconds = atof(argv[i]);
        }
    }
    g_stop_on_hang = stop_on_hang;

    printf("c repro: CoreFoundation CFRunLoop/CFSocket (no kqueue, no poll "
           "in the app)\n");
    printf("run=%.0fs timeout=%.0fs workers=%d (deferred close)%s\n\n",
           run_seconds, TIMEOUT_SEC, workers,
           stop_on_hang ? " [stop at first hang]" : "");
    fflush(stdout);

    if (workers > 256)
        workers = 256;
    pthread_t tids[256];
    for (int i = 0; i < workers; i++) {
        if (pthread_create(&tids[i], NULL, run_worker, &run_seconds) != 0)
            die("pthread_create");
    }
    for (int i = 0; i < workers; i++)
        pthread_join(tids[i], NULL);

    unsigned long long delivered = g_sh.delivered;
    unsigned long long undelivered = g_sh.undelivered;
    unsigned long long total = delivered + undelivered;
    printf("\ndelivered   = %llu/%llu\n", delivered, total);
    printf("UNDELIVERED = %llu/%llu\n", undelivered, total);
    return undelivered > 0 ? 1 : 0;
}
