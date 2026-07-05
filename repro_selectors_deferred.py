#!/usr/bin/env python3
"""
Like ``repro_selectors.py`` (pure ``selectors``, no asyncio), but with a
*deferred* abort-close that mirrors what asyncio's ``call_soon`` introduces: a
``select()`` / kqueue poll cycle runs between *unregistering* the client fd and
*closing* it.

``repro_selectors.py`` (immediate close after unregister) reproduces the hang on
macOS 14, but rarely. This variant inserts the poll cycle and reproduces it
markedly more often (and surfaces it on macOS 15 too), isolating that timing as
an amplifier of the underlying macOS lost-RST bug -- the same timing asyncio
introduces via ``call_soon``.

A persistent self-pipe stays registered so ``select()`` always has an fd to poll,
just as asyncio's event loop always has its self-pipe registered.

Usage:
    python3 repro_selectors_deferred.py [SECONDS]      # default 300s
"""
import errno
import platform
import selectors
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


def one_iteration(sel):
    client, server = make_pair()
    chunk = b"x" * 65536

    # Register the client (reader + writer) and fill until send() blocks.
    sel.register(client, selectors.EVENT_READ | selectors.EVENT_WRITE)
    filled = False
    fill_deadline = time.monotonic() + TIMEOUT
    while not filled and time.monotonic() < fill_deadline:
        for key, mask in sel.select(0.5):
            if key.fileobj is client and (mask & selectors.EVENT_WRITE):
                try:
                    while True:
                        client.send(chunk)
                except OSError:
                    sel.modify(client, selectors.EVENT_READ)  # stop writing
                    filled = True
    if not filled:
        sel.unregister(client)
        server.close()
        client.close()
        return "fill-timeout"

    # DEFERRED abort: unregister the client, run one poll cycle (as asyncio's
    # call_soon does), THEN abortively close it and start watching the server.
    sel.unregister(client)
    sel.select(0)  # <-- the poll cycle between unregister and close
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, LINGER_ZERO)
    client.close()

    sel.register(server, selectors.EVENT_READ)
    outcome = None
    deadline = time.monotonic() + TIMEOUT
    while outcome is None:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            outcome = "UNDELIVERED: " + describe(server)
            break
        for key, mask in sel.select(remaining):
            if key.fileobj is server:
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

    try:
        sel.unregister(server)
    except KeyError:
        pass
    server.close()
    try:
        client.close()
    except OSError:
        pass
    return outcome


def main():
    run_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
    print(f"platform: {platform.platform()}  python: {sys.version.split()[0]}")
    print(f"selector: {type(selectors.DefaultSelector()).__name__}")
    print(f"run={run_seconds}s timeout={TIMEOUT}s (deferred close)\n")

    sel = selectors.DefaultSelector()
    # A persistent self-pipe, always registered, so select() has an fd to poll
    # during the deferred-close cycle -- as asyncio's loop always does.
    wake_r, wake_w = socket.socketpair()
    wake_r.setblocking(False)
    sel.register(wake_r, selectors.EVENT_READ)

    delivered = undelivered = 0
    deadline = time.monotonic() + run_seconds
    while time.monotonic() < deadline:
        res = one_iteration(sel)
        if res.startswith("delivered"):
            delivered += 1
        elif res.startswith("UNDELIVERED"):
            undelivered += 1
            print(f"  [{delivered + undelivered}] {res}", flush=True)
        # fill-timeout: ignore
    sel.close()
    wake_r.close()
    wake_w.close()

    total = delivered + undelivered
    print(f"\ndelivered   = {delivered}/{total}")
    print(f"UNDELIVERED = {undelivered}/{total}")
    sys.exit(1 if undelivered else 0)


if __name__ == "__main__":
    main()
