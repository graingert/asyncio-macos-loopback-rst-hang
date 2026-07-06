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

TCP: RFC 5961 sends a challenge ACK instead of resetting when a valid RST arrives with SEG.SEQ == RCV.NXT and the receiver has delayed-ACKed data, leaving the connection half-open

## Description

RFC 5961 §3.2 requires that a RST whose sequence number **exactly equals
`RCV.NXT`** resets the connection, and that an in-window RST whose sequence
number is *not* `RCV.NXT` draws a challenge ACK instead. FreeBSD's check
implements the exact-match test against `last_ack_sent` rather than `rcv_nxt`.

When the receiver has received data it has not yet ACKed (delayed/stretch ACK, so
`rcv_nxt > last_ack_sent`), an incoming RST carrying `SEG.SEQ == rcv_nxt` — which
is exactly what a correct peer sends (`SND.NXT`) — matches neither
`last_ack_sent` nor the insecure-RST override, so it is treated as
in-window-but-not-exact and answered with a **challenge ACK instead of a reset**.
The connection stays `ESTABLISHED` (half-open):

- its `EVFILT_READ` registration never fires,
- `getpeername()` still succeeds,
- `getsockopt(SO_ERROR)` returns 0,
- `recv(..., MSG_PEEK)` returns `EAGAIN` (errno 35).

The peer that sent the RST is correct — the RST goes out with `SEG.SEQ ==
SND.NXT == the receiver's rcv_nxt`, exactly RFC-conformant. It is the *receiver*
that declines to reset. The connection is only torn down later, when a
retransmitted RST arrives after the challenge ACK has advanced `last_ack_sent`
up to `rcv_nxt` — seconds later, long after an application timeout has already
observed the hang.

### Root cause

`sys/netinet/tcp_input.c`, RFC 5961 §3.2 handling (FreeBSD 15.1, around line
2131):

```c
if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
     SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||
    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
    ...
    if (V_tcp_insecure_rst ||
        tp->last_ack_sent == th->th_seq) {   // <-- should also accept rcv_nxt
        /* reset the connection */
    } else {
        tcp_send_challenge_ack(tp, th, m);   // <-- taken for a valid RST at rcv_nxt
    }
}
goto drop;
```

The inner exact-match test is `last_ack_sent == th_seq`. The code comment above
it even records the deliberate-but-incomplete choice: *"Note: to take into
account delayed ACKs, we should test against last_ack_sent instead of rcv_nxt."*
It tests `last_ack_sent` **only**, and so misses the RFC 5961-mandated
`RCV.NXT` match. With the default `net.inet.tcp.insecure_rst=0`, the connection
resets **iff** `last_ack_sent == th_seq`; when `rcv_nxt > last_ack_sent`, a RST
at `rcv_nxt` is challenge-ACKed instead of resetting.

### Ground-truth proof (DTrace, at the moment of the drop)

`dtrace/rst_drop.d` hooks `fbt::tcp_do_segment:entry` and prints the receiver's
TCP control block for any RST that fails to reset an ESTABLISHED connection
(`last_ack_sent != th_seq`). One hang, captured natively on FreeBSD
15.1-RELEASE/amd64:

```
HANG-RST th_seq=1888935822 rcv_nxt=1888935822 last_ack_sent=1888919490 rcv_wnd=16640
  | seq-rcv_nxt=0  seq-last_ack_sent=16332  cur_edge(las+wnd)-seq=+308  fix_edge(rcvnxt+wnd)-seq=+16640
```

- `seq - rcv_nxt = 0`: the RST sits **exactly at `rcv_nxt`** — the final data was
  already absorbed; only the ACK was delayed. Per RFC 5961 this MUST reset.
- `seq - last_ack_sent = 16332`: `last_ack_sent` lags `rcv_nxt` by one full
  segment (delayed ACK). Because the code tests `last_ack_sent`, not `rcv_nxt`,
  the reset is declined.
- `cur_edge(las+wnd)-seq = +308`: the RST is **inside** the current window check
  (positive margin) — so this is *not* a window/right-edge drop. The outer clause
  passes; the inner exact-match is what fails.

### On-the-wire proof (same connection, same run)

`lo0`, absolute sequence numbers (`tcpdump -S`). 52374 is the closing peer,
22467 is the peer left `ESTABLISHED`:

```
52374->22467  . seq 1888919490:1888935822, length 16332   # closer's final full segment
52374->22467  R. seq 1888935822                            # the RST (seq == rcv_nxt)
22467->52374  . ack 1888935822, win 175                    # CHALLENGE ACK -- not a reset
   ... 3.07 s later ...
22467->52374  F. seq 1850944913, ack 1888935822            # app gives up and closes (FIN)
52374->22467  R  seq 1888935822                            # closer's host RSTs the orphan FIN
```

The RST's seq (1888935822) matches the DTrace `th_seq` exactly. The receiver's
reply is a challenge ACK (`ack 1888935822`), not a reset; the connection stays up
for ~3 s until the application closes it.

### Suggested fix

Accept `rcv_nxt` as an exact match in the reset test — the RFC 5961-mandated
point — in addition to the existing `last_ack_sent` leniency:

```diff
 		if (V_tcp_insecure_rst ||
-		    tp->last_ack_sent == th->th_seq) {
+		    tp->last_ack_sent == th->th_seq ||
+		    tp->rcv_nxt == th->th_seq) {
 			/* reset the connection */
```

This is strictly *more* RFC 5961-conformant (a RST at `RCV.NXT` MUST reset) and
does not weaken the anti-spoofing intent: an off-path attacker must still guess
`rcv_nxt` (or `last_ack_sent`) exactly; in-window/non-exact RSTs still draw a
challenge ACK.

A secondary, defensive change covers a rarer sub-case — when the delayed-ACK gap
(`rcv_nxt - last_ack_sent`) exceeds `rcv_wnd`, the *outer* window clause's right
edge (`last_ack_sent + rcv_wnd`) can fall below a RST at `rcv_nxt` and silently
drop it before the inner test is reached. Anchoring that edge at `rcv_nxt`
closes it:

```diff
 		if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
-		    SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||
+		    SEQ_LT(th->th_seq, tp->rcv_nxt + tp->rcv_wnd)) ||
 		    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
```

The captured hang above is fixed by the first (inner) change alone; the second is
belt-and-suspenders. *Not yet validated against a patched kernel — the DTrace
trace proves the mechanism; building a kernel with the fix and re-running the
reproducer is the confirming step.*

## How to reproduce

Self-contained C reproducer, no dependencies beyond libc:
https://github.com/graingert/asyncio-macos-loopback-rst-hang (`c/repro.c`)

```
cc -O2 -o repro c/repro.c
./repro 600
```

Each iteration, using only sockets + `kqueue`/`kevent`:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes fill the server's receive
   buffer and the client's send buffer.
3. Register the client fd (EVFILT_READ + EVFILT_WRITE); fill until `send()`
   returns EAGAIN.
4. `EV_DELETE` the client's filters, run one `kevent` poll, then abortively close
   it (`SO_LINGER {1,0}` + `close()`), which emits a RST at `SND.NXT`.
5. Register the server fd (EVFILT_READ); wait up to 1s for the disconnect.
6. If the disconnect never arrives, the RST was challenge-ACKed instead of
   resetting. The program prints the half-open port pair and exits non-zero.

The `kevent` poll in step 4 lets the server absorb a final data segment without
ACKing it before the RST arrives (creating the `rcv_nxt > last_ack_sent` gap),
which raises the hit rate. `asyncio` does exactly this via `call_soon`, which is
how the bug was first found.

The DTrace probe (`dtrace/rst_drop.d`) can be run alongside to print the
receiver's TCB at the drop.

## Expected result

The abortive `close()` resets the peer — a `recv()` returning `ECONNRESET`,
surfaced as a readable `EVFILT_READ`. This is what happens on the overwhelming
majority of iterations (and on Linux/epoll across many millions of iterations
with zero failures).

## Actual result

Occasionally the peer never observes the disconnect within the timeout. Its
socket stays `ESTABLISHED` — the reproducer confirms via `getpeername()`
(succeeds), `SO_ERROR == 0`, and `recv(MSG_PEEK) == EAGAIN`:

```
UNDELIVERED: 20208<->63743 SO_ERROR=0 MSG_PEEK=errno 35
```

**Observed rate on FreeBSD 15.1-RELEASE (amd64):** `312 / 10,336,808`
connections (~1 in 33,000) in a single 600-second run. On macOS (Darwin, the
other kqueue platform, same RFC 5961 lineage) the rate is far higher — up to
~28–46% on macOS 26.

## Environment

- FreeBSD 15.1-RELEASE, GENERIC, **amd64** (`FreeBSD 15.1-RELEASE releng/15.1-n283562-96841ea08dcf`), on a GitHub-hosted `vmactions/freebsd-vm` guest.
- Interface: loopback (`lo0`), IPv4 127.0.0.1.
- `net.inet.tcp.insecure_rst = 0` (default).
- Not reproducible on Linux (epoll) — its RST acceptance is checked against the
  actual next-expected sequence number.

## Related reports (same reproducer, other platforms)

- Apple Feedback Assistant **FB23590387** (macOS — much higher rate, same
  challenge-ACK-instead-of-reset signature on the wire).
- CPython python/cpython#153117 (surfaces as a silent asyncio hang, because
  asyncio defers `close()` via `call_soon`, which lets the server absorb an
  un-ACKed final segment before the RST).

---

## freebsd-net@ email (draft)

Send after filing the PR (or before, if the account is still pending — mention
the PR is forthcoming). Plain text, no HTML.

**To:** freebsd-net@freebsd.org
**Subject:** RFC 5961: challenge ACK instead of reset for a valid RST at rcv_nxt under delayed ACK

Hi,

I think I've found a TCP bug in the base system, in the RFC 5961 reset handling
in tcp_input.c, with an on-the-wire capture, a DTrace trace of the receiver's
TCB at the drop, and a suggested fix.

RFC 5961 resets the connection when an incoming RST has SEG.SEQ == RCV.NXT, and
challenge-ACKs an in-window RST that is not exactly RCV.NXT. The check does the
exact-match test against last_ack_sent, not rcv_nxt. When the receiver has
received-but-unacked data (delayed ACK, rcv_nxt > last_ack_sent), a valid RST at
rcv_nxt (== the sender's SND.NXT) is challenge-ACKed instead of resetting, and
the connection is left ESTABLISHED until a later RST retransmit -- long enough
for an application to see a hang. EVFILT_READ never fires, SO_ERROR == 0,
recv(MSG_PEEK) == EAGAIN.

DTrace at the drop (fbt::tcp_do_segment:entry), one hang:

  th_seq=1888935822 rcv_nxt=1888935822 last_ack_sent=1888919490 rcv_wnd=16640
  seq-rcv_nxt=0  (RST is exactly at rcv_nxt)
  seq-last_ack_sent=16332  (last_ack_sent lags one segment, delayed ACK)
  (last_ack_sent+rcv_wnd)-seq=+308  (RST is inside the window -- not a window drop)

Matching wire capture (52374 closes, 22467 is left hung):

  52374->22467  . seq 1888919490:1888935822, length 16332
  52374->22467  R. seq 1888935822            <- RST, seq == rcv_nxt
  22467->52374  . ack 1888935822             <- challenge ACK, not a reset

Self-contained C reproducer (libc only, raw kqueue/kevent + sockets):

  https://github.com/graingert/asyncio-macos-loopback-rst-hang  (c/repro.c)
  cc -O2 -o repro c/repro.c && ./repro 600

On FreeBSD 15.1-RELEASE (amd64) it reproduces at 312 / 10,336,808 connections
(~1 in 33k) in a 600s run; never on Linux (epoll). The same reproducer hits it
far more often on macOS (up to ~28-46% on macOS 26), which shares this code.

Suggested fix: accept rcv_nxt as an exact match in the reset test (details + diff
in PR <NNNNNN>). Also reported to Apple (FB23590387) and CPython
(python/cpython#153117), since asyncio's deferred close makes it surface as a
silent hang.

Happy to test patches or gather more data (tcpdump, dtrace, etc.).

Thanks,
<your name>
