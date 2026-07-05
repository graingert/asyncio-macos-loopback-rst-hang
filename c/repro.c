/*
 * Pure-C reproducer for the lost TCP reset (RST) on macOS loopback, using the
 * raw kqueue/kevent and socket syscalls only (no libraries beyond libc).
 *
 * When a flow-controlled (zero receive window) loopback TCP connection is
 * abortively closed -- setsockopt(SO_LINGER {1,0}) then close() -- the reset is
 * occasionally never delivered to the peer. The peer's socket stays ESTABLISHED
 * forever: its EVFILT_READ never fires, getpeername() still succeeds,
 * SO_ERROR == 0, and a MSG_PEEK recv() returns EWOULDBLOCK.
 *
 * Mirrors the "deferred close" pattern that reproduces most often: a kevent
 * poll cycle runs between deleting the client fd's filters and abortively
 * closing it.
 *
 * Each iteration:
 *   1. Establish a loopback client + accepted server socket, both non-blocking.
 *   2. Register the client (EVFILT_READ + EVFILT_WRITE); fill via write-
 *      readiness until send() blocks against the non-reading server.
 *   3. EV_DELETE the client's filters, run one kevent poll cycle, then
 *      abortively close it.
 *   4. Register the server (EVFILT_READ); drain the buffered data, then wait
 *      for the disconnect (EOF / errno).
 *   5. If the disconnect never arrives within TIMEOUT, the reset was lost.
 *
 * Build:  cc -O2 -o repro repro.c
 * Run:    ./repro [SECONDS] [stop]
 *           SECONDS: how long to run (default 300)
 *           stop:    if present, exit at the first lost RST (for packet capture)
 *
 * Exit status is non-zero if any lost RST is observed.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TIMEOUT_SEC 1.0

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void make_pair(int *client_out, int *server_out) {
    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lsock, (struct sockaddr *)&a, sizeof(a));
    listen(lsock, 1);

    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    getsockname(lsock, (struct sockaddr *)&bound, &blen);

    int client = socket(AF_INET, SOCK_STREAM, 0);
    connect(client, (struct sockaddr *)&bound, sizeof(bound));
    int server = accept(lsock, NULL, NULL);
    close(lsock);

    set_nonblocking(client);
    set_nonblocking(server);
    *client_out = client;
    *server_out = server;
}

static void kev_change(int kq, int fd, int16_t filter, uint16_t flags) {
    struct kevent ev;
    EV_SET(&ev, fd, filter, flags, 0, 0, NULL);
    kevent(kq, &ev, 1, NULL, 0, NULL);
}

static int kev_poll(int kq, struct kevent *evs, int max, double timeout) {
    struct timespec ts;
    ts.tv_sec = (time_t)timeout;
    ts.tv_nsec = (long)((timeout - (double)(time_t)timeout) * 1e9);
    int n = kevent(kq, NULL, 0, evs, max, &ts);
    return n < 0 ? 0 : n;
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
static int one_iteration(int kq, char *buf, size_t buflen, char *desc,
                         size_t desclen) {
    int client, server;
    make_pair(&client, &server);
    static char chunk[65536];
    struct kevent evs[16];

    /* Register the client for read+write; fill until send() blocks. */
    kev_change(kq, client, EVFILT_READ, EV_ADD);
    kev_change(kq, client, EVFILT_WRITE, EV_ADD);
    int filled = 0;
    double fill_deadline = now_sec() + TIMEOUT_SEC;
    while (!filled && now_sec() < fill_deadline) {
        int n = kev_poll(kq, evs, 16, 0.5);
        for (int i = 0; i < n; i++) {
            if ((int)evs[i].ident == client && evs[i].filter == EVFILT_WRITE) {
                while (send(client, chunk, sizeof(chunk), 0) > 0) {
                }
                kev_change(kq, client, EVFILT_WRITE, EV_DELETE);
                filled = 1;
            }
        }
    }
    if (!filled) {
        kev_change(kq, client, EVFILT_READ, EV_DELETE);
        close(client);
        close(server);
        return 0;
    }

    /* Deferred abort: delete the client's remaining filter, run one poll cycle,
     * then abortively close it. */
    kev_change(kq, client, EVFILT_READ, EV_DELETE);
    kev_poll(kq, evs, 16, 0.0); /* the poll cycle between unregister and close */
    struct linger lg = {1, 0};
    setsockopt(client, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    int lport = local_port(server);
    int pport = peer_port(server);
    close(client);

    /* Register the server; drain buffered data, then wait for the disconnect. */
    kev_change(kq, server, EVFILT_READ, EV_ADD);
    int hung = 1;
    double deadline = now_sec() + TIMEOUT_SEC;
    for (;;) {
        double rem = deadline - now_sec();
        if (rem <= 0) {
            hung = 1;
            break;
        }
        int n = kev_poll(kq, evs, 16, rem);
        int got = 0;
        for (int i = 0; i < n; i++) {
            if ((int)evs[i].ident == server && evs[i].filter == EVFILT_READ) {
                ssize_t r = recv(server, buf, buflen, 0);
                if (r <= 0) { /* ECONNRESET / EOF */
                    hung = 0;
                    got = 1;
                    break;
                }
                /* else: drained buffered data; keep waiting */
            }
        }
        if (got)
            break;
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
    }
    close(server);
    return hung;
}

int main(int argc, char **argv) {
    double run_seconds = argc > 1 ? atof(argv[1]) : 300.0;
    int stop_on_hang = argc > 2;

    printf("c repro: raw kqueue/kevent + sockets (no libraries)\n");
    printf("run=%.0fs timeout=%.0fs (deferred close)%s\n\n", run_seconds,
           TIMEOUT_SEC, stop_on_hang ? " [stop at first hang]" : "");
    fflush(stdout);

    int kq = kqueue();
    size_t buflen = 1 << 20;
    char *buf = malloc(buflen);
    unsigned long long delivered = 0, undelivered = 0;
    char desc[128];
    double start = now_sec();

    while (now_sec() - start < run_seconds) {
        if (one_iteration(kq, buf, buflen, desc, sizeof(desc))) {
            undelivered++;
            printf("  [%llu] UNDELIVERED: %s\n", delivered + undelivered, desc);
            fflush(stdout);
            if (stop_on_hang)
                break;
        } else {
            delivered++;
        }
    }
    close(kq);

    unsigned long long total = delivered + undelivered;
    printf("\ndelivered   = %llu/%llu\n", delivered, total);
    printf("UNDELIVERED = %llu/%llu\n", undelivered, total);
    return undelivered > 0 ? 1 : 0;
}
