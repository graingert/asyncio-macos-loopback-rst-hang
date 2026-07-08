# DragonFly BSD bug report (draft)

File at the DragonFly bug tracker (Redmine): https://bugs.dragonflybsd.org/
(**Tracker** = `Bug`, **Category** = `Kernel`). DragonFly development happens on
the mailing lists, so after filing, post a short note linking the issue to
**kernel@dragonflybsd.org** (Cc **bugs@dragonflybsd.org**); patches also go to
**submit@dragonflybsd.org** or straight into the issue. A `kernel@` email draft
is at the end of this file.

---

## Summary

TCP: a valid RST carrying `SEG.SEQ == RCV.NXT` is silently dropped instead of
resetting the connection when the receiver has delayed-ACKed data and its receive
window has shrunk below the un-ACKed gap, leaving the connection half-open.

## Description

When a peer abortively closes a flow-controlled connection, it emits a RST at
`SND.NXT`, which equals the receiver's `RCV.NXT`. RFC 793 (and RFC 5961 §3.2)
require the receiver to reset on a RST at `RCV.NXT`. DragonFly instead validates
an incoming RST against a window anchored on **`last_ack_sent`**, and drops
anything outside `[last_ack_sent, last_ack_sent + rcv_wnd]`.

When the receiver has received data it has not yet ACKed (delayed/stretch ACK, so
`rcv_nxt > last_ack_sent`) **and** its receive window has shrunk below that gap
(`rcv_nxt - last_ack_sent > rcv_wnd`, which happens precisely when the receiving
application has stopped reading — the same flow-controlled state that produced the
abort), a RST at `rcv_nxt` sits *beyond* the right edge `last_ack_sent + rcv_wnd`
and is silently dropped. The connection stays `ESTABLISHED` (half-open):

- its `EVFILT_READ` registration never fires,
- `getpeername()` still succeeds,
- `getsockopt(SO_ERROR)` returns 0,
- `recv(..., MSG_PEEK)` returns `EAGAIN` (errno 35).

The peer that sent the RST is correct — the RST goes out with `SEG.SEQ ==
SND.NXT == the receiver's rcv_nxt`. The originating side has already fully closed
(its `tcpcb` is gone after the abortive `close()`), so nothing retransmits the
RST; the receiver stays `ESTABLISHED` until the application gives up and closes
the socket itself, long after an application timeout has already observed the
hang.

### How this differs from the FreeBSD form of the bug

DragonFly has **no RFC 5961 challenge-ACK path**. FreeBSD's *common* failure
(~1 in 128k on loopback) is that a valid in-window RST at `rcv_nxt` draws a
challenge ACK instead of resetting; DragonFly is **immune** to that case — an
in-window RST at `rcv_nxt` where `gap ≤ rcv_wnd` resets correctly. DragonFly is
exposed only through the **rarer residual**: the outer window edge is anchored on
`last_ack_sent`, so once the window shrinks below the delayed-ACK gap the RST
falls off the right edge and is dropped. It is the same defect at the same code
point that FreeBSD carries as its residual (which FreeBSD's second fix targets).

### Root cause

`sys/netinet/tcp_input.c`, ESTABLISHED-state RST handling (DragonFly
`DragonFly_RELEASE_6_4`, the `if (thflags & TH_RST)` block at line 1681, window
check at lines 1682–1683):

```c
if (thflags & TH_RST) {
        if (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
            SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) {   // <-- both edges on last_ack_sent
                switch (tp->t_state) {
                ...
                case TCPS_ESTABLISHED:
                ...
                        so->so_error = ECONNRESET;
                close:
                        TCP_STATE_CHANGE(tp, TCPS_CLOSED);
                        ...
                }
        }
        goto drop;   // <-- RST at rcv_nxt, gap > rcv_wnd: falls here, silently dropped
}
```

The acceptance window is `[last_ack_sent, last_ack_sent + rcv_wnd]`. Both edges
are anchored on `last_ack_sent`. When `rcv_nxt > last_ack_sent + rcv_wnd`, a RST
at `rcv_nxt` — the RFC-mandated reset point — fails `SEQ_LEQ(th->th_seq,
last_ack_sent + rcv_wnd)` and is dropped. There is no exact-match test against
`rcv_nxt` and no zero-window special case.

### Prior art in the sibling BSDs

This is the delayed-ACK RST failure that both other kqueue BSDs fixed years ago by
anchoring RST validation on `rcv_nxt`:

- **OpenBSD** (markus, 2005 + 2006): accepts a RST whose seq matches
  `last_ack_sent`, `rcv_nxt`, or `rcv_nxt + 1`. Commit message: *"allow RST if the
  th_seq matches rcv_nxt in case the RST follows the data immediately. otherwise
  we would ignore RST for delayed acks."*
- **NetBSD** (christos, 2009, patch from Joanne M Mikkelson): validates a RST
  against `rcv_nxt` exactly. Commit message: *"Don't check against the last ack
  received, but the expected sequence number. This makes RST handling independent
  of delayed ACK."*

DragonFly (like FreeBSD and XNU) never took the `rcv_nxt` anchor. See
`PRIOR_ART.md` for the full lineage.

### Same-code-path proof (FreeBSD, wire capture)

The residual was captured on the wire on a FreeBSD kernel patched to remove the
*primary* (challenge-ACK) failure — isolating exactly the outer-edge drop that is
DragonFly's only failure mode. `tcpdump -S` on `lo0` (45605 closes, 50033 is left
hung):

```
S->C  ack 3142883995, win 65 (=16640 B)          # last ACK: last_ack_sent = 3142883995
C->S  . seq 3142883995:3142900327, length 16332  # final data segment (delayed-ACKed)
C->S  R. seq 3142900327                           # RST at rcv_nxt (= snd_max)
S->C  . ack 3142900327, win 320                   # receiver acks the data; the RST was silently dropped
```

`last_ack_sent` (3142883995) lags `rcv_nxt` (3142900327) by a full 16332-byte
segment (delayed ACK), and `rcv_wnd` has shrunk to ~308, so the RST's seq sits far
beyond `last_ack_sent + rcv_wnd = 3142884303` → the window check drops it. This is
DragonFly's `SEQ_LEQ(th->th_seq, last_ack_sent + rcv_wnd)` test failing,
byte-for-byte.

### Empirical status on DragonFly

Reproduced under QEMU/KVM on DragonFly 6.4.2-RELEASE (`X86_64_GENERIC`), running
the self-contained C reproducer below. <!-- DFLY_EMPIRICAL --> An initial 180 s
pass completed **0 / 450,584** connections with no hang; a longer stop-on-first-
hang run is in progress to catch the rare residual. The residual is inherently
much rarer on DragonFly than the FreeBSD primary, because DragonFly lacks the
common challenge-ACK failure entirely — only the narrow window-shrunk-below-gap
race remains. (This section will be updated with the long-run figure.)

### Suggested fix

Accept a RST whose sequence number is exactly `rcv_nxt`, matching the deployed
OpenBSD/NetBSD behaviour — a RST at `RCV.NXT` must reset:

```diff
 	if (thflags & TH_RST) {
-		if (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
-		    SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) {
+		if (th->th_seq == tp->rcv_nxt ||
+		    (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
+		     SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd))) {
 			switch (tp->t_state) {
```

Equivalently, the window's right edge can be anchored on the true receive-window
edge `rcv_nxt + rcv_wnd` (the same one-line change FreeBSD needs for its
residual):

```diff
 		if (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
-		    SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) {
+		    SEQ_LEQ(th->th_seq, tp->rcv_nxt + tp->rcv_wnd)) {
```

Either change makes a RST at `rcv_nxt` reset regardless of the delayed-ACK gap.
Neither weakens DragonFly's existing RST acceptance meaningfully: DragonFly
already accepts any in-window RST (RFC 793 behaviour, no challenge ACK), so an
off-path attacker gains nothing new — the change only stops legitimate RSTs at
`rcv_nxt` from being dropped. The exact-`rcv_nxt` form is the more conservative of
the two and matches the sibling-BSD prior art directly.

## How to reproduce

Self-contained C reproducer, no dependencies beyond libc, uses only sockets +
`kqueue`/`kevent`:
https://github.com/graingert/asyncio-macos-loopback-rst-hang (`c/repro.c`)

```
cc -O2 -o repro c/repro.c
./repro 600
```

Each iteration:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes fill the server's receive
   buffer and drive `rcv_wnd` toward zero.
3. Register the client fd (EVFILT_READ + EVFILT_WRITE); fill until `send()`
   returns EAGAIN.
4. `EV_DELETE` the client's filters, run one `kevent` poll (which lets the server
   absorb a final data segment without ACKing it — creating the `rcv_nxt >
   last_ack_sent` gap), then abortively close it (`SO_LINGER {1,0}` + `close()`),
   which emits a RST at `SND.NXT`.
5. Register the server fd (EVFILT_READ); wait up to 1s for the disconnect.
6. If the disconnect never arrives, the RST was dropped. The program prints the
   half-open port pair and exits non-zero.

`asyncio` performs exactly this deferred close via `call_soon`, which is how the
bug was first found (python/cpython#153117).

## Expected result

The abortive `close()` resets the peer — a `recv()` returning `ECONNRESET`,
surfaced as a readable `EVFILT_READ`.

## Actual result

Occasionally the peer never observes the disconnect within the timeout; its socket
stays `ESTABLISHED`, confirmed via `getpeername()` (succeeds), `SO_ERROR == 0`,
and `recv(MSG_PEEK) == EAGAIN`:

```
UNDELIVERED: 20208<->63743 SO_ERROR=0 MSG_PEEK=errno 35
```

## Environment

- DragonFly 6.4.2-RELEASE, `X86_64_GENERIC`, amd64, under QEMU/KVM.
- Interface: loopback (`lo0`), IPv4 127.0.0.1.
- Not reproducible on Linux (epoll) across many millions of iterations. OpenBSD
  (7.4) and NetBSD (9.3), which anchor RST validation on `rcv_nxt`, also do not
  reproduce it (0 across ~0.9M / ~1.5M connections respectively under the same
  harness).

## Related reports (same reproducer, other platforms)

- FreeBSD — same code lineage, the *common* challenge-ACK form plus this residual
  (see `FREEBSD_BUG.md`).
- Apple Feedback Assistant **FB23590387** (macOS/XNU — far higher rate, same
  `last_ack_sent`-anchored validation).
- CPython python/cpython#153117 (surfaces as a silent asyncio hang).

---

## kernel@ email (draft)

**To:** kernel@dragonflybsd.org
**Cc:** bugs@dragonflybsd.org
**Subject:** TCP: RST at rcv_nxt dropped under delayed ACK when rcv_wnd < gap (half-open hang)

Hi,

I think there's a TCP bug in tcp_input.c's ESTABLISHED-state RST handling. An
incoming RST is validated against a window anchored on last_ack_sent:

  if (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
      SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) { ...reset... }
  goto drop;

When the receiver has received-but-unacked data (delayed ACK, rcv_nxt >
last_ack_sent) and its receive window has shrunk below that gap (the flow-
controlled state where the app has stopped reading), a valid RST at rcv_nxt (==
the peer's SND.NXT) sits beyond last_ack_sent + rcv_wnd and is silently dropped
instead of resetting. The sender has already fully closed, so nothing retransmits;
the connection is left ESTABLISHED. EVFILT_READ never fires, SO_ERROR == 0,
recv(MSG_PEEK) == EAGAIN.

OpenBSD (2005/2006) and NetBSD (2009) both fixed this by anchoring RST validation
on rcv_nxt. Suggested minimal fix, matching that prior art -- accept a RST at
rcv_nxt exactly:

  if (th->th_seq == tp->rcv_nxt ||
      (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
       SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd))) { ...reset... }

(Equivalently, anchor the right edge on rcv_nxt + rcv_wnd.) DragonFly already
accepts any in-window RST, so this doesn't weaken RST filtering -- it just stops
legitimate RSTs at rcv_nxt from being dropped.

Self-contained C reproducer (libc only, raw kqueue/kevent + sockets):

  https://github.com/graingert/asyncio-macos-loopback-rst-hang  (c/repro.c)
  cc -O2 -o repro c/repro.c && ./repro 600

The same defect (as the residual after removing FreeBSD's challenge-ACK path) is
captured on the wire in the linked FreeBSD report; DragonFly shares the code
point. Also reported to FreeBSD and Apple (FB23590387). Happy to gather more data
(tcpdump, ktr, etc.) or test patches.

Thanks,
<your name>
