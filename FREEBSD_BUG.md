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

TCP: RFC 5961 RST check rejects a valid RST (SEG.SEQ == RCV.NXT) when the receiver has received-but-unacknowledged data under flow control, leaving the connection half-open

## Description

The RFC 5961 reset check in `tcp_input.c` computes its acceptance window's right
edge from `last_ack_sent` instead of `rcv_nxt`. When the receiver is flow
controlled (receive buffer nearly full, small `rcv_wnd`) **and** has just
accepted data it has not yet ACKed (delayed ACK, so `rcv_nxt > last_ack_sent`), a
perfectly valid RST whose sequence number equals `rcv_nxt` is rejected. The
connection is left `ESTABLISHED` forever on the side that should have been reset:

- its `EVFILT_READ` registration never fires,
- `getpeername()` still succeeds,
- `getsockopt(SO_ERROR)` returns 0,
- `recv(..., MSG_PEEK)` returns `EAGAIN` (errno 35).

The peer that sent the RST is correct — the RST goes out on the wire with
`SEG.SEQ == SND.NXT`, which is exactly RFC-conformant. It is the *receiver* that
wrongly drops it.

### Root cause

`sys/netinet/tcp_input.c`, RFC 5961 §3.2 handling (FreeBSD 15.1, around line
2131):

```c
if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
     SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||
    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
    ...
    if (V_tcp_insecure_rst ||
        tp->last_ack_sent == th->th_seq) {
        /* reset the connection */
    } else {
        /* challenge ACK */
    }
}
goto drop;
```

The left edge is deliberately anchored at `last_ack_sent` (the comment says *"to
take into account delayed ACKs"*), but the width is `rcv_wnd`, which is measured
from `rcv_nxt`. So the computed right edge is `last_ack_sent + rcv_wnd`, which is
short of the true window edge `rcv_nxt + rcv_wnd` by exactly
`rcv_nxt - last_ack_sent` — the amount of received-but-unacknowledged data.

RFC 5961 says to test the RST against `RCV.NXT` and to reset when
`SEG.SEQ == RCV.NXT`. Because this code substitutes `last_ack_sent`:

- when `rcv_nxt - last_ack_sent >= rcv_wnd` (nearly-full buffer + a burst of
  unacked data), a RST at `SEG.SEQ == rcv_nxt` is **above** `last_ack_sent +
  rcv_wnd`, so it matches neither the in-window clause nor the zero-window clause
  and is silently dropped;
- even if it were in window, the exact-match reset tests `last_ack_sent ==
  th_seq`, not `rcv_nxt == th_seq`, so a RST at `rcv_nxt` would only draw a
  challenge ACK rather than resetting.

### On-the-wire proof

Captured on `lo0` (macOS, same RFC 5961 code path; FreeBSD reproduces the same
symptom). 49164 is the closing peer, 49163 is the peer wrongly left
`ESTABLISHED`. Absolute sequence numbers (`tcpdump -S`):

```
S->C  ack 3081222923, win 246                        # last ACK S transmitted
C->S  P. seq 3081222923:3081223131, length 208       # C's final data segment
C->S  R. seq 3081223131                              # the RST (dropped by S)
S->C  ack 3081223131, win 243                        # S *did* receive the 208 bytes
```

- `last_ack_sent` = 3081222923 (S's last transmitted ACK).
- S accepts the 208-byte segment, so `rcv_nxt` = 3081223131, but its ACK is
  delayed — `last_ack_sent` still lags at 3081222923.
- The RST's seq = 3081223131 = `rcv_nxt` = C's `SND.NXT`. RFC-correct.
- S's check: `SEQ_LT(3081223131, 3081222923 + rcv_wnd)` is false whenever
  `rcv_wnd <= 208`. With the buffer near full it is, so the RST is dropped.
- S only ACKs 3081223131 *after* the RST — proving it held the data unacked when
  the RST arrived.

### Suggested fix

Anchor the right edge at `rcv_nxt` (per RFC 5961) and accept `rcv_nxt` as an
exact match, while keeping the `last_ack_sent` leniency on the left:

```diff
 	if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
-	    SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||
+	    SEQ_LT(th->th_seq, tp->rcv_nxt + tp->rcv_wnd)) ||
 	    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
 		...
 		if (V_tcp_insecure_rst ||
-		    tp->last_ack_sent == th->th_seq) {
+		    tp->last_ack_sent == th->th_seq ||
+		    tp->rcv_nxt == th->th_seq) {
```

This is strictly *more* RFC 5961-conformant (a RST at `RCV.NXT` MUST reset); it
does not loosen the anti-spoofing intent, since off-window RSTs are still
rejected and in-window/non-exact RSTs still draw a challenge ACK.

*Not yet validated against a patched kernel — the packet capture proves why the
RST is dropped; building a kernel with this change and re-running the reproducer
is the confirming step.*

## How to reproduce

Self-contained C reproducer, no dependencies beyond libc:
https://github.com/graingert/asyncio-macos-loopback-rst-hang (`c/repro.c`)

```
cc -O2 -o repro c/repro.c
./repro 600
```

Each iteration, using only sockets + `kqueue`/`kevent`:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes drive the receive window down
   and fill the client's send buffer.
3. Register the client fd (EVFILT_READ + EVFILT_WRITE); fill until `send()`
   returns EAGAIN.
4. `EV_DELETE` the client's filters, run one `kevent` poll, then abortively close
   it (`SO_LINGER {1,0}` + `close()`), which emits a RST at `SND.NXT`.
5. Register the server fd (EVFILT_READ); wait up to 1s for the disconnect.
6. If the disconnect never arrives, the RST was dropped. The program prints the
   half-open port pair and exits non-zero.

The `kevent` poll in step 4 is what makes the server accept a final data burst
without ACKing it before the RST arrives (maximizing `rcv_nxt - last_ack_sent`),
which is why it raises the hit rate. `asyncio` does exactly this via `call_soon`,
which is how the bug was first found.

## Expected result

The abortive `close()` resets the peer — a `recv()` returning `ECONNRESET`,
surfaced as a readable `EVFILT_READ`. This is what happens on the overwhelming
majority of iterations (and on Linux/epoll across many millions of iterations
with zero failures).

## Actual result

Occasionally the peer never observes the disconnect. Its socket stays
`ESTABLISHED` — the reproducer confirms via `getpeername()` (succeeds),
`SO_ERROR == 0`, and `recv(MSG_PEEK) == EAGAIN`:

```
UNDELIVERED: 20208<->63743 SO_ERROR=0 MSG_PEEK=errno 35
```

**Observed rate on FreeBSD 15.1-RELEASE (amd64):** `312 / 10,336,808`
connections (~1 in 33,000) in a single 600-second run. On macOS (Darwin, the
other kqueue platform, same RFC 5961 code) the rate is far higher — up to
~28–46% on macOS 26 — and the packet capture above was taken there.

## Environment

- FreeBSD 15.1-RELEASE, GENERIC, **amd64** (`FreeBSD 15.1-RELEASE releng/15.1-n283562-96841ea08dcf`), on a GitHub-hosted `vmactions/freebsd-vm` guest.
- Interface: loopback (`lo0`), IPv4 127.0.0.1.
- Not reproducible on Linux (epoll) — the acceptance-window edge is computed
  differently there.

## Related reports (same reproducer, other platforms)

- Apple Feedback Assistant **FB23590387** (macOS — much higher rate, with the
  on-the-wire packet capture shown above).
- CPython python/cpython#153117 (surfaces as a silent asyncio hang, because
  asyncio defers `close()` via `call_soon`, which lets the server take an
  unacked final burst before the RST).

---

## freebsd-net@ email (draft)

Send after filing the PR (or before, if the account is still pending — mention
the PR is forthcoming). Plain text, no HTML.

**To:** freebsd-net@freebsd.org
**Subject:** RFC 5961 RST check drops valid RST (seq == rcv_nxt) under flow control + delayed ACK

Hi,

I think I've found a TCP bug in the base system, in the RFC 5961 reset check in
tcp_input.c, and I have an on-the-wire capture and a suggested fix.

The check computes its acceptance-window right edge as
`last_ack_sent + rcv_wnd`, but `rcv_wnd` is measured from `rcv_nxt`. When the
receiver is flow controlled (small rcv_wnd) and has received-but-unacked data
(delayed ACK, so rcv_nxt > last_ack_sent), the right edge is short of the true
window edge by `rcv_nxt - last_ack_sent`. A valid RST whose seq == rcv_nxt (== the
sender's SND.NXT, RFC-correct) then falls above the computed edge and is silently
dropped, leaving the connection ESTABLISHED forever. EVFILT_READ never fires,
SO_ERROR == 0, recv(MSG_PEEK) == EAGAIN.

Absolute-seq capture of the failing exchange (49164 closes, 49163 is left hung):

  S->C  ack 3081222923, win 246
  C->S  P. seq 3081222923:3081223131, length 208
  C->S  R. seq 3081223131            <- RST, dropped
  S->C  ack 3081223131, win 243      <- S had the 208 bytes unacked when the RST came

Self-contained C reproducer (libc only, raw kqueue/kevent + sockets):

  https://github.com/graingert/asyncio-macos-loopback-rst-hang  (c/repro.c)
  cc -O2 -o repro c/repro.c && ./repro 600

On FreeBSD 15.1-RELEASE (amd64) it reproduces at 312 / 10,336,808 connections
(~1 in 33k) in a 600s run; never on Linux (epoll). The same reproducer hits it
far more often on macOS (up to ~28-46% on macOS 26), which shares this code.

Suggested fix: anchor the right edge at rcv_nxt and accept rcv_nxt as an exact
match (details + diff in PR <NNNNNN>). Also reported to Apple (FB23590387) and
CPython (python/cpython#153117), since asyncio's deferred close makes it surface
as a silent hang.

Happy to test patches or gather more data (tcpdump, dtrace, etc.).

Thanks,
<your name>
