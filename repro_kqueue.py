#!/usr/bin/env python3
"""
Same reproducer as the others, but using the raw ``select.kqueue`` API directly
-- no ``selectors``, no ``asyncio``. This is the most minimal, kernel-level form:
register/unregister file descriptors with ``kevent`` and poll with
``kqueue.control``.

It mirrors the deferred-close pattern (which is the one that reproduces most
often): a ``kqueue.control`` poll runs between deleting the client fd's filters
and abortively closing it.

  1. Establish a loopback client + accepted server socket, both non-blocking.
  2. Register the client (EVFILT_READ + EVFILT_WRITE); fill via write-readiness
     until ``send()`` blocks against the non-reading server (window -> zero).
  3. Delete the client's filters, run one ``kqueue.control`` poll cycle, then
     abortively close it: ``SO_LINGER {1,0}`` + ``close()``.
  4. Register the server (EVFILT_READ); drain the buffered data, then wait for
     the disconnect (EOF / errno).
  5. If the disconnect never arrives within TIMEOUT, the reset was lost.

Only runs on platforms with ``select.kqueue`` (macOS / *BSD); elsewhere it exits
0 without doing anything.

Usage:
    python3 repro_kqueue.py [SECONDS]      # default 300s
"""
import errno
import platform
import select
import socket
import struct
import sys
import time

TIMEOUT = 1.0
LINGER_ZERO = struct.pack("ii", 1, 0)


def make_pair():
    lsock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    lsock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    lsock.bind(("127.0.0.1", 0))
    lsock.listen(1)
    client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client.connect(lsock.getsockname())
    server, _ = lsock.accept()
    lsock.close()
    client.setblocking(False)
    server.setblocking(False)
    return client, server


def describe(sock):
    try:
        peer = f"getpeername={sock.getpeername()}"
    except OSError as e:
        peer = f"getpeername->{errno.errorcode.get(e.args[0], e.args[0])}"
    try:
        so = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
    except OSError as e:
        so = f"err{e.args[0]}"
    try:
        d = sock.recv(1, socket.MSG_PEEK)
        peek = "EOF" if d == b"" else f"{len(d)}bytes"
    except OSError as e:
        peek = errno.errorcode.get(e.args[0], str(e.args[0]))
    return f"{peer} SO_ERROR={so} MSG_PEEK={peek}"


def kev(fd, filt, flags):
    return select.kevent(fd, filter=filt, flags=flags)


def one_iteration(kq):
    client, server = make_pair()
    cfd, sfd = client.fileno(), server.fileno()
    chunk = b"x" * 65536

    # Register the client for read+write and fill until send() blocks.
    kq.control(
        [
            kev(cfd, select.KQ_FILTER_READ, select.KQ_EV_ADD),
            kev(cfd, select.KQ_FILTER_WRITE, select.KQ_EV_ADD),
        ],
        0,
        0,
    )
    filled = False
    fill_deadline = time.monotonic() + TIMEOUT
    while not filled and time.monotonic() < fill_deadline:
        for ev in kq.control(None, 16, 0.5):
            if ev.ident == cfd and ev.filter == select.KQ_FILTER_WRITE:
                try:
                    while True:
                        client.send(chunk)
                except OSError:
                    kq.control(
                        [kev(cfd, select.KQ_FILTER_WRITE, select.KQ_EV_DELETE)], 0, 0
                    )
                    filled = True
    if not filled:
        kq.control([kev(cfd, select.KQ_FILTER_READ, select.KQ_EV_DELETE)], 0, 0)
        server.close()
        client.close()
        return "fill-timeout"

    # Deferred abort: delete the client's remaining filter, run one poll cycle,
    # then abortively close it.
    kq.control([kev(cfd, select.KQ_FILTER_READ, select.KQ_EV_DELETE)], 0, 0)
    kq.control(None, 16, 0)  # <-- the poll cycle between unregister and close
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, LINGER_ZERO)
    client.close()

    # Register the server; drain buffered data, then wait for the disconnect.
    kq.control([kev(sfd, select.KQ_FILTER_READ, select.KQ_EV_ADD)], 0, 0)
    outcome = None
    deadline = time.monotonic() + TIMEOUT
    while outcome is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            outcome = "UNDELIVERED: " + describe(server)
            break
        for ev in kq.control(None, 16, remaining):
            if ev.ident == sfd and ev.filter == select.KQ_FILTER_READ:
                try:
                    data = server.recv(1 << 20)
                except OSError as e:
                    outcome = "delivered: " + errno.errorcode.get(
                        e.args[0], str(e.args[0])
                    )
                    break
                if data == b"":
                    outcome = "delivered: EOF"
                    break
                # else: drained buffered data; keep waiting

    # Closing the server auto-removes its kevents.
    server.close()
    try:
        client.close()
    except OSError:
        pass
    return outcome


def main():
    if not hasattr(select, "kqueue"):
        print(f"select.kqueue not available on {platform.platform()}; skipping.")
        sys.exit(0)

    run_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
    print(f"platform: {platform.platform()}  python: {sys.version.split()[0]}")
    print(f"api: select.kqueue")
    print(f"run={run_seconds}s timeout={TIMEOUT}s (deferred close)\n")

    kq = select.kqueue()
    delivered = undelivered = 0
    deadline = time.monotonic() + run_seconds
    while time.monotonic() < deadline:
        res = one_iteration(kq)
        if res.startswith("delivered"):
            delivered += 1
        elif res.startswith("UNDELIVERED"):
            undelivered += 1
            print(f"  [{delivered + undelivered}] {res}", flush=True)
        # fill-timeout: ignore
    kq.close()

    total = delivered + undelivered
    print(f"\ndelivered   = {delivered}/{total}")
    print(f"UNDELIVERED = {undelivered}/{total}")
    sys.exit(1 if undelivered else 0)


if __name__ == "__main__":
    main()
