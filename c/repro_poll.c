/*
 * Portable (poll-based) reproducer for the lost TCP reset (RST) under a delayed
 * ACK. Uses only poll(2) + sockets, so this one source file builds and runs
 * unchanged on Linux and FreeBSD (no kqueue) -- handy as a side-by-side
 * control: FreeBSD
 * (and its downstreams, XNU, DragonFly) can lose the RST; Linux (rcv_nxt-
 * anchored RST validation) never does.
 *
 * The bug: when a flow-controlled (small/zero receive window) TCP connection is
 * abortively closed -- setsockopt(SO_LINGER {1,0}) then close() -- the peer that
 * has delayed-ACKed the final data segment (so rcv_nxt > last_ack_sent) may
 * never see the RST. Its socket stays ESTABLISHED forever: it never becomes
 * readable, getpeername() still succeeds, SO_ERROR == 0, and a MSG_PEEK recv()
 * returns EWOULDBLOCK. The RST carried SEG.SEQ == the peer's rcv_nxt, which
 * FreeBSD/XNU/DragonFly validate against last_ack_sent instead, so it is
 * answered with a challenge ACK (FreeBSD) or silently dropped, never a reset.
 *
 * This mirrors the "deferred close" pattern that reproduces most often: one
 * poll cycle runs between filling the client and abortively closing it -- the
 * same call_soon-deferred close asyncio performs (python/cpython#153117).
 *
 * Each iteration:
 *   1. Establish a loopback client + accepted server socket, both non-blocking.
 *   2. poll() the client for writability and send() until the send buffer stays
 *      full against the non-reading server (its receive window has reached 0).
 *   3. Run one poll cycle, then abortively close the client (SO_LINGER {1,0}).
 *   4. poll() the server for readability; drain any buffered data, then wait for
 *      the disconnect (EOF / ECONNRESET).
 *   5. If the disconnect never arrives within TIMEOUT, the reset was lost.
 *
 * Build:  cc -O2 -o repro_poll repro_poll.c
 * Run:    ./repro_poll [SECONDS] [stop] [-jN]
 *           SECONDS: how long to run (default 300)
 *           stop:    exit at the first lost RST (for packet capture)
 *           -jN:     run N worker processes in parallel (default 1)
 *
 * Exit status is non-zero if any lost RST is observed. On Linux it should
 * always report 0 undelivered; on FreeBSD it reports a small nonzero count.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

#define TIMEOUT_SEC 1.0

static int g_stop_on_hang = 0;

/* Shared across worker processes (mmap MAP_SHARED|MAP_ANON) so -jN workers
 * aggregate their counts and honour a single stop-on-hang. */
struct shared {
    unsigned long long delivered;
    unsigned long long undelivered;
    volatile int stop;
};
static struct shared *g_sh;

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

/* poll a single fd for `events`; returns the revents (0 on timeout). */
static short poll_one(int fd, short events, double timeout) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    int ms = (int)(timeout * 1000.0);
    if (ms < 0)
        ms = 0;
    int n = poll(&pfd, 1, ms);
    return n > 0 ? pfd.revents : 0;
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

/* returns 1 if the abort was NOT delivered (the hang), 0 if delivered. */
static int one_iteration(char *buf, size_t buflen, char *desc, size_t desclen) {
    int client, server;
    make_pair(&client, &server);
    /* Contents are irrelevant; we only need bytes to fill the send buffer. */
    static char chunk[65536];

    /* Fill: wait for the client to be writable, then send until send() blocks
     * against the non-reading server -- the client's send buffer is full and
     * the server's receive window is driven toward 0. One drain matches the
     * kqueue reproducer (fast; the bug is rare, so iterations/sec matters). */
    int filled = 0;
    double fill_deadline = now_sec() + TIMEOUT_SEC;
    while (!filled && now_sec() < fill_deadline) {
        short re = poll_one(client, POLLOUT, 0.5);
        if (re & POLLOUT) {
            while (send(client, chunk, sizeof(chunk), 0) > 0) {
            }
            filled = 1;
        }
    }
    if (!filled) {
        close(client);
        close(server);
        return 0;
    }

    /* Deferred abort: run one poll cycle (the call_soon-deferred-close analog),
     * then abortively close the client so it emits a RST at SND.NXT, which is
     * the server's rcv_nxt. */
    (void)poll(NULL, 0, 0); /* the poll cycle between "unwatch" and close */
    struct linger lg = {1, 0};
    if (setsockopt(client, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg)) < 0)
        die("setsockopt(SO_LINGER)");
    int lport = local_port(server);
    int pport = peer_port(server);
    close(client);

    /* Drain buffered data on the server, then wait for the disconnect. */
    int hung = 1;
    double deadline = now_sec() + TIMEOUT_SEC;
    for (;;) {
        double rem = deadline - now_sec();
        if (rem <= 0) {
            hung = 1;
            break;
        }
        short re = poll_one(server, POLLIN, rem);
        if (re & (POLLIN | POLLHUP | POLLERR)) {
            ssize_t r = recv(server, buf, buflen, 0);
            if (r <= 0) { /* ECONNRESET / EOF */
                hung = 0;
                break;
            }
            /* else: drained buffered data; keep waiting for the disconnect. */
        }
    }

    if (hung) {
        int so = 0;
        socklen_t sl = sizeof(so);
        getsockopt(server, SOL_SOCKET, SO_ERROR, &so, &sl);
        char pk[32];
        ssize_t r = recv(server, buf, 1, MSG_PEEK);
        if (r < 0)
            snprintf(pk, sizeof(pk), "errno %d", errno);
        else if (r == 0)
            snprintf(pk, sizeof(pk), "EOF");
        else
            snprintf(pk, sizeof(pk), "%zdbytes", r);
        snprintf(desc, desclen, "%d<->%d SO_ERROR=%d MSG_PEEK=%s", lport, pport,
                 so, pk);
        if (g_stop_on_hang) {
            /* Idle so the hung connection's abort window is isolated by a clear
             * multi-second gap of silence in a packet capture. */
            sleep(2);
        }
    }
    close(server);
    return hung;
}

/* One worker process: runs the loop until the deadline or the shared stop flag,
 * accumulating counts into the shared struct. */
static void run_worker(double run_seconds) {
    size_t buflen = 1 << 20;
    char *buf = malloc(buflen);
    if (!buf)
        die("malloc");
    char desc[128];
    double start = now_sec();

    while (now_sec() - start < run_seconds && !g_sh->stop) {
        if (one_iteration(buf, buflen, desc, sizeof(desc))) {
            unsigned long long idx = __sync_add_and_fetch(&g_sh->undelivered, 1);
            printf("  [%llu] UNDELIVERED: %s\n", idx + g_sh->delivered, desc);
            fflush(stdout);
            if (g_stop_on_hang) {
                g_sh->stop = 1;
                break;
            }
        } else {
            __sync_add_and_fetch(&g_sh->delivered, 1);
        }
    }
    free(buf);
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

    printf("c repro (portable): raw poll(2) + sockets (no kqueue)\n");
    printf("run=%.0fs timeout=%.0fs workers=%d (deferred close)%s\n\n",
           run_seconds, TIMEOUT_SEC, workers,
           stop_on_hang ? " [stop at first hang]" : "");
    fflush(stdout);

    g_sh = mmap(NULL, sizeof(*g_sh), PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANON, -1, 0);
    if (g_sh == MAP_FAILED)
        die("mmap");
    g_sh->delivered = g_sh->undelivered = 0;
    g_sh->stop = 0;

    if (workers > 256)
        workers = 256;
    for (int i = 0; i < workers; i++) {
        pid_t p = fork();
        if (p < 0)
            die("fork");
        if (p == 0) {
            run_worker(run_seconds);
            _exit(0);
        }
    }
    while (wait(NULL) > 0)
        ;

    unsigned long long delivered = g_sh->delivered;
    unsigned long long undelivered = g_sh->undelivered;
    unsigned long long total = delivered + undelivered;
    printf("\ndelivered   = %llu/%llu\n", delivered, total);
    printf("UNDELIVERED = %llu/%llu\n", undelivered, total);
    return undelivered > 0 ? 1 : 0;
}
