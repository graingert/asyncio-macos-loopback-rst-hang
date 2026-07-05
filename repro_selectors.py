#!/usr/bin/env python3
"""
Same reproducer as ``repro.py`` but using only the ``selectors`` module (no
``asyncio``), to show whether the lost RST is specific to asyncio or lives one
layer lower, in the ``selectors`` / kqueue registration pattern.

A single ``selectors.DefaultSelector`` (``KqueueSelector`` on macOS) drives a
hand-rolled event loop. Each iteration:

  1. Establish a loopback client + accepted server socket, both non-blocking.
  2. Register the client for read+write; fill via write-readiness until
     ``send()`` blocks against the non-reading server (window -> zero), then
     drop the write interest.
  3. Unregister the client and abortively close it: ``SO_LINGER {1,0}`` +
     ``close()``.
  4. Register the server for read; drain the buffered data, then wait for the
     disconnect (EOF / errno).
  5. If the disconnect never arrives within TIMEOUT, the reset was not
     delivered -- the bug. Diagnostics proving the socket is still ESTABLISHED
     are printed.

Usage:
    python3 repro_selectors.py [SECONDS]      # default 300s
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

    # Abortively close the client.
    sel.unregister(client)
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, LINGER_ZERO)
    client.close()

    # Register the server; drain buffered data, then wait for the disconnect.
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
    print(f"run={run_seconds}s timeout={TIMEOUT}s\n")

    sel = selectors.DefaultSelector()
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

    total = delivered + undelivered
    print(f"\ndelivered   = {delivered}/{total}")
    print(f"UNDELIVERED = {undelivered}/{total}")
    sys.exit(1 if undelivered else 0)


if __name__ == "__main__":
    main()
