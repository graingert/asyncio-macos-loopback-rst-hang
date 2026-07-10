# FreeBSD bug report

**Filed as bug 296594:** https://bugs.freebsd.org/bugzilla/show_bug.cgi?id=296594
(Base System / kern). Patch against `main` (-CURRENT):
`freebsd-current-tcp-rst-rcvnxt.patch`.

Next: email a short note linking the PR to **freebsd-net@freebsd.org** (the
network-stack maintainers watch the list, not Bugzilla) — draft at the end of
this file.

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
that declines to reset. The originating side has already fully closed (its
`tcpcb` is gone after the abortive `close()`), so nothing retransmits the RST;
the receiver simply stays `ESTABLISHED` until the application gives up and closes
the socket itself. In the capture below that is a FIN ~3 s later — long after an
application timeout has already observed the hang.

### Root cause

`sys/netinet/tcp_input.c`, RFC 5961 §3.2 handling (FreeBSD 15.1-RELEASE, the
`if (thflags & TH_RST)` block at line 2148, check at line 2160):

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

**Three** changes to the RFC 5961 RST handling are required; each closes a
distinct sub-case, and a kernel missing any one of them still hangs on that
sub-case (see [Validation](#validation-patched-kernel)).

**1. Accept `rcv_nxt` as an exact match in the reset test** — the RFC 5961-mandated
point — in addition to the existing `last_ack_sent` leniency:

```diff
 			if (V_tcp_insecure_rst ||
-			    tp->last_ack_sent == th->th_seq) {
+			    tp->last_ack_sent == th->th_seq ||
+			    tp->rcv_nxt == th->th_seq) {
 				TCPSTAT_INC(tcps_drops);
 				/* Drop the connection. */
```

This is strictly *more* RFC 5961-conformant (a RST at `RCV.NXT` MUST reset) and
does not weaken the anti-spoofing intent: an off-path attacker must still guess
`rcv_nxt` (or `last_ack_sent`) exactly; in-window/non-exact RSTs still draw a
challenge ACK.

**2. Anchor the outer window clause's right edge at `rcv_nxt`.** When the
delayed-ACK gap (`rcv_nxt - last_ack_sent`) exceeds `rcv_wnd`, the current right
edge (`last_ack_sent + rcv_wnd`) falls below a RST at `rcv_nxt`, so the outer
clause silently drops it before the inner test is ever reached. `rcv_nxt +
rcv_wnd` is the true right edge of the receive window:

```diff
 		if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
-		    SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||
+		    SEQ_LT(th->th_seq, tp->rcv_nxt + tp->rcv_wnd)) ||
 		    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
```

**3. Accept `rcv_nxt` in the zero-window arm.** When `rcv_wnd == 0`, the outer
window check (arm A) can never admit a RST at `rcv_nxt` — `SEQ_LT(rcv_nxt,
rcv_nxt + 0)` is false — so acceptance falls to the zero-window arm, which also
tests `last_ack_sent` only. Under a delayed ACK (`rcv_nxt > last_ack_sent`) a
RST at `rcv_nxt` is rejected there too (shown against the post-change-2 tree):

```diff
 		if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
 		    SEQ_LT(th->th_seq, tp->rcv_nxt + tp->rcv_wnd)) ||
-		    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
+		    (tp->rcv_wnd == 0 && (tp->last_ack_sent == th->th_seq ||
+		    tp->rcv_nxt == th->th_seq))) {
```

An organic loopback flow does not reach this arm (a conforming sender will not
push data past a zero window), so changes 1 + 2 alone show 0 hangs across 31.65M
connections; but the state *is* reachable with crafted packets and is wire-proven
below (`rst-rcvnxt-zero-window.pkt`). RFC 5961 requires a RST at `RCV.NXT` to
reset regardless of the advertised window.

### Prior art: the sibling BSDs already anchor on `rcv_nxt`

Anchoring RST validation on `rcv_nxt` is **not novel** — both other kqueue BSDs
adopted it years ago, each landing the change with a commit message that names the
delayed-ACK RST failure directly:

- **OpenBSD** — `markus`, 2005-12-01 and 2006-12-11. Accepts a RST whose seq
  matches `last_ack_sent`, `rcv_nxt`, or `rcv_nxt + 1`. *"allow RST if the th_seq
  matches rcv_nxt in case the RST follows the data immediately. otherwise we would
  ignore RST for delayed acks."*
- **NetBSD** — `christos`, 2009-06-20 (patch from Joanne M Mikkelson). Validates a
  RST against `rcv_nxt` exactly. *"Don't check against the last ack received, but
  the expected sequence number. This makes RST handling independent of delayed
  ACK."* (Following `draft-ietf-tcpm-tcpsecure-11`, which became RFC 5961.)

Running the same `c/repro.c` under QEMU/KVM confirms the anchor is what matters —
the two `rcv_nxt`-anchored BSDs do not reproduce the hang, while the two that
anchor on `last_ack_sent` do:

| kernel | RST validation anchor | 180 s repro run | hangs |
| --- | --- | --- | --- |
| OpenBSD 7.4 | `last_ack_sent` / `rcv_nxt` / `rcv_nxt + 1` | 1,512,615 conns | **0** |
| NetBSD 9.3 | `rcv_nxt` (exact) | 915,874 conns | **0** |
| FreeBSD 15.1 | `last_ack_sent` only | — | hangs (below) |
| DragonFly 6.4.2 | window on `last_ack_sent`, no challenge ACK | 450,584 conns | 0 so far (rare residual only — see `DRAGONFLY_BUG.md`) |

FreeBSD's own in-tree comment has acknowledged the delayed-ACK gap since 2014
without acting on it. The change requested here brings FreeBSD in line with
deployed, reviewed prior art rather than introducing new behaviour. Full lineage
(incl. the DragonFly and XNU/Darwin variants that share the unfixed anchor) is in
`PRIOR_ART.md`.

### Validation (patched kernel)

Built into FreeBSD 15.1-RELEASE-p1 GENERIC kernels under QEMU/KVM and run against
`c/repro.c`. To remove any ambiguity about which kernel produced which result,
the two patched kernels carry distinct idents (verified live with `uname -i`):
`RSTINNER` (change 1 only) and `RSTBOTH` (changes 1 + 2).

| kernel (`uname -i`) | result |
| --- | --- |
| stock `GENERIC` | hangs — e.g. **15 / 1,919,209** (~1 in 128k); DTrace confirmed every hang is the challenge-ACK case (change 1's target) |
| `RSTINNER` (change 1) | primary hang gone, but the **residual persists** — first hang seen at 287,392 / 19,946,282 connections in separate runs |
| `RSTBOTH` (changes 1 + 2) | **0 / 31,651,215** |

**The residual is the outer-clause drop (change 2's target), captured on the
wire** on `RSTINNER` (`tcpdump -S`; 45605 closes, 50033 is left hung):

```
S->C  ack 3142883995, win 65 (=16640 B)          # last ACK: last_ack_sent = 3142883995
C->S  . seq 3142883995:3142900327, length 16332  # final data segment (delayed-ACKed)
C->S  R. seq 3142900327                           # RST at rcv_nxt (= snd_max)
S->C  . ack 3142900327, win 320                   # server acks the data; the RST was silently dropped
```

`last_ack_sent` (3142883995) lags `rcv_nxt` (3142900327) by a full 16332-byte
segment (delayed ACK), and `rcv_wnd` has shrunk to ~308, so the RST's seq sits far
beyond `last_ack_sent + rcv_wnd = 3142884303` → the outer clause drops it before
the inner test. Change 2 (`rcv_nxt + rcv_wnd = 3142900635 > seq`) accepts it, then
change 1 resets. `RSTBOTH` eliminates it (0 across 31.65M connections, where
`RSTINNER` hit it within ~20M).

**Caveat — the residual is a timing-sensitive Heisenbug.** It needs a tight race
(the RST landing after the receiver absorbs the final segment but before its
delayed ACK, with the window already below the gap), so its rate is extremely
bursty (a first hang anywhere from ~290k to >18M connections) and it is perturbed
by `fbt` DTrace on `tcp_do_segment` (which adds hot-path latency); the wire
capture above uses `tcpdump`'s out-of-band BPF tap, which does not disturb it. The
*primary* hang is not timing-fragile (DTrace matched it exactly, 17/17).

**Change 3 (the zero-window arm) is validated separately, via packetdrill,**
because the organic `c/repro.c` run above never reaches it — a conforming sender
will not push data past a zero window, which is why `RSTBOTH` (changes 1 + 2)
shows 0 hangs across 31.65M organic connections. The state *is* reachable with
crafted packets, though. Two GENERIC kernels differing **only** by change 3 —
`RSTBOTH` (changes 1 + 2) and `RSTARMB` (changes 1 + 2 + 3) — booted under
QEMU/KVM with `uname -i` verified, give a clean red/green on the zero-window test
(RST at `rcv_nxt`, `rcv_wnd == 0`, delayed ACK), while both pass the other two:

| test | stock `GENERIC` | `RSTBOTH` (1 + 2) | `RSTARMB` (1 + 2 + 3) |
| --- | --- | --- | --- |
| `rst-rcvnxt-challenge-ack.pkt` (fix 1) | RED | GREEN | GREEN |
| `rst-rcvnxt-window-drop.pkt` (fix 2) | RED | GREEN | GREEN |
| `rst-rcvnxt-zero-window.pkt` (fix 3) | RED | **RED** (`read` → `EAGAIN`) | **GREEN** (`ECONNRESET`) |

The single-line difference between `RSTBOTH` and `RSTARMB` flipping only the
zero-window test proves the arm-B gap is real and the third change closes it.

### Regression tests (packetdrill)

Three deterministic packetdrill scripts, one per fix, live in `tests/`. All use a
non-blocking socket so the buggy case fails fast with `EAGAIN` rather than
blocking. Verified under QEMU/KVM against a stock GENERIC kernel, a kernel with
only fixes 1 + 2 (`RSTBOTH`), and one with all three (`RSTARMB`, i.e. patched with
`freebsd-current-tcp-rst-rcvnxt.patch`):

| test | stock | fixes 1 + 2 | all three |
| --- | --- | --- | --- |
| `rst-rcvnxt-challenge-ack.pkt` (fix 1: RST at rcv_nxt, in-window) | RED — challenge-ACK'd (`read` → EAGAIN) | GREEN — `ECONNRESET` | GREEN |
| `rst-rcvnxt-window-drop.pkt` (fix 2: rcv_wnd shrunk below the gap) | RED — silently dropped (no challenge ACK) | GREEN — `ECONNRESET` | GREEN |
| `rst-rcvnxt-zero-window.pkt` (fix 3: rcv_wnd == 0, delayed ACK) | RED — rejected by the zero-window arm | **RED** — still rejected | **GREEN** — `ECONNRESET` |

The window-drop test uses a small `SO_RCVBUF` (< 2·MSS, so it is not rounded up)
and a sub-MSS delay-ACKed fill to shrink `rcv_wnd` below the gap, so the RST at
`rcv_nxt` lands beyond `last_ack_sent + rcv_wnd` — confirmed live with DTrace on
`tcp_do_segment`. It requires fixes 1 + 2 (a fix-1-only kernel still drops it at
the unfixed outer edge). The zero-window test sizes the delay-ACked fill to bring
`rcv_wnd` to **exactly 0**, so the RST at `rcv_nxt` reaches only the zero-window
arm — it stays RED on a fixes-1+2 kernel and needs fix 3.

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
- Not reproducible on Linux (epoll) across many millions of iterations.

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
rcv_nxt (== the sender's SND.NXT) is challenge-ACKed instead of resetting. The
sender has already fully closed (nothing retransmits the RST), so the connection
is left ESTABLISHED until the application gives up -- long enough to see a hang.
EVFILT_READ never fires, SO_ERROR == 0, recv(MSG_PEEK) == EAGAIN.

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

Anchoring RST validation on rcv_nxt is deployed prior art: OpenBSD (markus,
2005/2006) and NetBSD (christos, 2009) both fixed exactly this delayed-ACK RST
failure that way, each with a commit message that names it. Running the same
reproducer under QEMU/KVM, neither reproduces (OpenBSD 7.4: 0/1,512,615; NetBSD
9.3: 0/915,874), while FreeBSD does.

Suggested fix (three parts: accept rcv_nxt as an exact match in the reset test,
anchor the outer window edge at rcv_nxt + rcv_wnd, and accept rcv_nxt in the
zero-window arm; details + diff in bug 296594). I built these into 15.1p1 GENERIC
kernels under QEMU/KVM: stock hangs at 15/1,919,209, and changes 1 + 2 give
0/31,651,215 organic connections (the inner change alone is not enough:
residual at ~287k). The third change covers the rcv_wnd == 0 case, which an
organic sender does not reach but crafted packets do -- proven with a
deterministic packetdrill test (rst-rcvnxt-zero-window.pkt) that is RED on a
changes-1+2 kernel and GREEN with all three. Also reported to Apple (FB23590387)
and CPython (python/cpython#153117), since asyncio's deferred close makes it
surface as a silent hang.

Happy to test patches or gather more data (tcpdump, dtrace, etc.).

Thanks,
<your name>
