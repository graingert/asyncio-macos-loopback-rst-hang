# FreeBSD bug report (draft)

**First, get a Bugzilla account.** Auto-registration is disabled (AI-spam); email
**bugmeister@FreeBSD.org** from a legitimate address requesting an account, and
wait for the confirmation email before you can log in.

Then file at https://bugs.freebsd.org/bugzilla/ — **Product** = `Base System`,
**Component** = `kern`, **Version** = `15.1-RELEASE`, **Hardware/OS** = `amd64` /
`FreeBSD`. After filing, email a short note linking the PR to
**freebsd-net@freebsd.org** (the network-stack maintainers watch the list, not
Bugzilla).

---

## Summary

TCP: abortive close (SO_LINGER {1,0}) of a zero-window loopback socket occasionally loses the RST, leaving the peer ESTABLISHED

## Description

Abortively closing a flow-controlled (zero receive window) loopback TCP connection — `setsockopt(SO_LINGER, {l_onoff=1, l_linger=0})` then `close()`, which per BSD semantics must emit a RST — occasionally does **not** deliver the RST to the peer. The peer socket stays `ESTABLISHED` indefinitely:

- its `EVFILT_READ` registration never fires,
- `getpeername()` still succeeds,
- `getsockopt(SO_ERROR)` returns 0,
- `recv(..., MSG_PEEK)` returns `EAGAIN` (errno 35).

No RST or FIN is ever observed by the peer; the connection is left half-open forever. The closing side is correct at `close()` time (still connected, `SO_LINGER {1,0}`, unsent bytes queued in the send buffer).

It is rare but reliably reproducible under load, and much likelier when a `kevent` poll runs between deleting the closing fd's filters and calling `close()` ("deferred close").

## How to reproduce

Self-contained C reproducer, no dependencies beyond libc:
https://github.com/graingert/asyncio-macos-loopback-rst-hang (`c/repro.c`)

```
cc -O2 -o repro c/repro.c
./repro 600
```

Each iteration, using only sockets + `kqueue`/`kevent`:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes drive the receive window to zero and fill the client's send buffer.
3. Register the client fd (EVFILT_READ + EVFILT_WRITE); fill until `send()` returns EAGAIN.
4. `EV_DELETE` the client's filters, run one `kevent` poll, then abortively close it (`SO_LINGER {1,0}` + `close()`).
5. Register the server fd (EVFILT_READ); drain buffered data, then wait up to 1s for the disconnect.
6. If the disconnect never arrives, the RST was lost. The program prints the half-open port pair and exits non-zero.

## Expected result

Every abortive `close()` results in the peer observing the reset — a `recv()` returning `ECONNRESET`, surfaced as a readable `EVFILT_READ`. This is what happens on the overwhelming majority of iterations (and on Linux/epoll across many millions of iterations with zero failures).

## Actual result

Occasionally the peer never observes the disconnect. Its socket stays `ESTABLISHED` — the reproducer confirms via `getpeername()` (succeeds), `SO_ERROR == 0`, and `recv(MSG_PEEK) == EAGAIN`:

```
UNDELIVERED: 20208<->63743 SO_ERROR=0 MSG_PEEK=errno 35
```

**Observed rate on FreeBSD 15.1-RELEASE (amd64):** `312 / 10,336,808` connections (~1 in 33,000) in a single 600-second run.

On macOS (Darwin, the other kqueue platform) the same reproducer is far more frequent — up to ~28–46% of connections on macOS 26 — and a packet capture there shows the closing side **does put the RST on the wire** for the hung connection, yet the peer never leaves `ESTABLISHED`. That points at inbound-RST acceptance while the receiver's window is zero, rather than the RST failing to be sent. FreeBSD shows the identical symptom at a lower rate.

## Environment

- FreeBSD 15.1-RELEASE, GENERIC, **amd64** (`FreeBSD 15.1-RELEASE releng/15.1-n283562-96841ea08dcf`), on a GitHub-hosted `vmactions/freebsd-vm` guest.
- Interface: loopback (`lo0`), IPv4 127.0.0.1.
- Not reproducible on Linux (epoll).

## Related reports (same reproducer, other kqueue platforms)

- Apple Feedback Assistant **FB23590387** (macOS — much higher rate, with an on-the-wire packet capture).
- CPython python/cpython#153117 (surfaces as a silent asyncio hang, because asyncio defers `close()` via `call_soon`, inserting the amplifying poll cycle).

---

## freebsd-net@ email (draft)

Send after filing the PR (or before, if the account is still pending — mention
the PR is forthcoming). Plain text, no HTML.

**To:** freebsd-net@freebsd.org
**Subject:** Lost TCP RST on abortive close of a zero-window loopback socket (kern, repro attached)

Hi,

I've hit what looks like a TCP bug in the base system and would appreciate a
pointer to the right area. On an abortive close of a flow-controlled (zero
receive window) loopback connection — setsockopt(SO_LINGER {1,0}) then close(),
which should emit a RST — the RST is occasionally never delivered to the peer.
The peer socket stays ESTABLISHED forever: EVFILT_READ never fires,
getsockopt(SO_ERROR) == 0, recv(MSG_PEEK) == EAGAIN.

I have a self-contained C reproducer (libc only, raw kqueue/kevent + sockets):

  https://github.com/graingert/asyncio-macos-loopback-rst-hang  (c/repro.c)
  cc -O2 -o repro c/repro.c && ./repro 600

On FreeBSD 15.1-RELEASE (amd64) it reproduces at 312 / 10,336,808 connections
(~1 in 33k) in a 600s run. It never reproduces on Linux (epoll). The same
reproducer hits the identical symptom on macOS at a much higher rate (up to
~28-46% on macOS 26); a packet capture there shows the closing side does put the
RST on the wire, yet the peer never leaves ESTABLISHED — which suggests the
issue is in accepting an inbound RST while the receive window is zero, rather
than the RST not being sent.

Filed as PR <NNNNNN> (Base System / kern). Also reported to Apple (FB23590387)
and CPython (python/cpython#153117), since asyncio's deferred close makes it
surface as a silent hang.

Happy to test patches or gather more data (tcpdump, dtrace, etc.).

Thanks,
<your name>
