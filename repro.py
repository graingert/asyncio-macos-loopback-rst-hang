#!/usr/bin/env python3
"""
Reproducer for a lost TCP reset (RST) on macOS loopback under asyncio.

Summary
-------
On macOS, when a flow-controlled (zero receive window) loopback TCP connection
whose file descriptor is driven by an asyncio ``SelectorEventLoop``
(``KqueueSelector``) is *abortively* closed -- ``SO_LINGER {1, 0}`` then
``close()`` -- the reset is occasionally never delivered to the peer.

The closing side's socket is textbook-correct at ``close()`` time (still
connected, ``SO_LINGER`` set to ``{1, 0}``, unsent bytes in the send buffer),
so per POSIX/BSD semantics ``close()`` must emit a RST. The closing fd is then
released cleanly (it does not linger). Yet the peer's socket stays
``ESTABLISHED`` forever: its ``add_reader`` callback never fires, ``getpeername``
still succeeds, ``SO_ERROR`` is 0, and a ``MSG_PEEK`` ``recv`` returns
``EWOULDBLOCK``. The connection is left half-open with no RST/FIN delivered.

Each iteration reproduces the scenario using only the low-level event-loop file
descriptor APIs (``add_reader`` / ``add_writer`` / ``remove_reader`` /
``remove_writer``) plus raw blocking/non-blocking sockets:

  1. Establish a loopback client + accepted server socket, both non-blocking.
  2. The server never reads, so as the client writes its receive window goes to
     zero and the client's send buffer fills.
  3. The client is registered with the loop (reader + writer) and fills until
     ``send()`` blocks.
  4. The client is unregistered and abortively closed:
     ``setsockopt(SO_LINGER, {1, 0})`` + ``close()``.
  5. The server is registered with ``add_reader``; its callback drains the
     buffered data and then waits for the disconnect (EOF / errno).
  6. If the server's callback never reports the disconnect within TIMEOUT, the
     reset was not delivered -- the bug. Diagnostics proving the socket is
     still ESTABLISHED are printed.

Observed
--------
- macOS 14 (Kqueue): reproduces, rarely (order 1 in 10^5-10^6 iterations).
- macOS 15 (Kqueue): not observed in ~10^6 iterations (much rarer, if present).
- Linux (epoll): never observed.

Usage
-----
    python3 repro.py [SECONDS]      # default 300s

Exit status is non-zero if any undelivered (half-open) close was observed.
"""
import asyncio
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
    """A connected loopback (client, server) socket pair, both non-blocking."""
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
    """Prove whether the socket still looks like a live, idle connection."""
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


async def one_iteration(loop):
    client, server = make_pair()
    cfd, sfd = client.fileno(), server.fileno()

    # Client is a reader (as a normal connection is) and a writer while it has
    # data to send; fill until the send buffer blocks against the non-reading
    # server (receive window -> zero).
    loop.add_reader(cfd, lambda: None)
    fill_done = loop.create_future()
    chunk = b"x" * 65536

    def do_write():
        try:
            while True:
                client.send(chunk)
        except OSError:
            loop.remove_writer(cfd)
            if not fill_done.done():
                fill_done.set_result(None)

    loop.add_writer(cfd, do_write)
    await fill_done

    # Abortively close the client: unregister it, then reset.
    loop.remove_reader(cfd)
    loop.remove_writer(cfd)
    client.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, LINGER_ZERO)
    client.close()

    # The server starts reading; its callback drains buffered data and then
    # waits for the disconnect.
    got = loop.create_future()

    def on_server_readable():
        try:
            data = server.recv(1 << 20)
        except OSError as e:
            if not got.done():
                got.set_result(errno.errorcode.get(e.args[0], str(e.args[0])))
            return
        if data == b"":
            if not got.done():
                got.set_result("EOF")
        # else: drained buffered data; keep waiting for the disconnect

    loop.add_reader(sfd, on_server_readable)
    try:
        await asyncio.wait_for(got, TIMEOUT)
        outcome = "delivered"
    except asyncio.TimeoutError:
        outcome = "UNDELIVERED: " + describe(server)
    finally:
        try:
            loop.remove_reader(sfd)
        except Exception:
            pass
        server.close()
        try:
            client.close()
        except Exception:
            pass
    return outcome


async def main_coro(run_seconds):
    loop = asyncio.get_running_loop()
    delivered = 0
    undelivered = 0
    deadline = time.monotonic() + run_seconds
    while time.monotonic() < deadline:
        res = await one_iteration(loop)
        if res == "delivered":
            delivered += 1
        else:
            undelivered += 1
            print(f"  [{delivered + undelivered}] {res}", flush=True)
    return delivered, undelivered


def main():
    run_seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 300.0
    print(f"platform: {platform.platform()}  python: {sys.version.split()[0]}")
    print(f"selector: {type(selectors.DefaultSelector()).__name__}")
    print(f"run={run_seconds}s timeout={TIMEOUT}s\n")

    delivered, undelivered = asyncio.run(main_coro(run_seconds))
    total = delivered + undelivered
    print(f"\ndelivered   = {delivered}/{total}")
    print(f"UNDELIVERED = {undelivered}/{total}")
    sys.exit(1 if undelivered else 0)


if __name__ == "__main__":
    main()
