# Prior art: RFC 5961 RST validation and the delayed-ACK hang

Context for `FREEBSD_BUG.md`. The bug is that FreeBSD's RFC 5961 §3.2 RST
handling validates an incoming RST against `last_ack_sent` only, and never
against `rcv_nxt`. When a delayed ACK leaves `rcv_nxt > last_ack_sent`, a
legitimate RST carrying `SEG.SEQ == rcv_nxt` fails the exact-match test in one of
three ways, none of which reset the connection: (1) in-window, it is answered
with a **challenge ACK**; (2) once the receive window has shrunk below the gap,
it is **silently dropped** by the outer window check; (3) once the window reaches
**zero**, it is rejected by the zero-window arm (which also tests `last_ack_sent`
only). The peer has already forgotten the connection, so the challenge ACK is
itself answered with a RST at the same seq, and the exchange never converges —
the local side hangs.

The complete fix therefore has **three parts** (accept `rcv_nxt` in the reset
test, anchor the window edge at `rcv_nxt + rcv_wnd`, and accept `rcv_nxt` in the
zero-window arm). Cases 1 and 2 arise organically; case 3 is not reached by a
conforming loopback flow but is directly reachable with crafted packets and is
wire-proven — two GENERIC kernels differing only by the zero-window change give a
clean red/green on a deterministic packetdrill test (`tests/rst-rcvnxt-zero-window.pkt`).
See `FREEBSD_BUG.md` for the diffs and validation.

The fix — anchor RST validation on `rcv_nxt`, not `last_ack_sent` — is **not
novel**. Both sibling BSDs adopted it years ago, each with a commit message that
names the delayed-ACK failure directly. FreeBSD, DragonFly, and XNU never took
it.

## Summary

| Kernel | RST validation anchor | Vulnerable to delayed-ACK hang? | Reference |
| --- | --- | --- | --- |
| OpenBSD | accepts `last_ack_sent`, `rcv_nxt`, `rcv_nxt + 1` | No | markus 2005 / 2006 |
| NetBSD | `rcv_nxt` only (exact) | No | christos 2009 |
| FreeBSD | `last_ack_sent` only (`insecure_rst` toggles RFC 793 window) | **Yes** | Gleb Smirnoff 2014 |
| DragonFly | window `[last_ack_sent, last_ack_sent + rcv_wnd]`, no challenge ACK | **Yes** (silent drop, residual path) | pre-RFC-5961 |
| XNU (Darwin) | `last_ack_sent` or `last_ack_sent - 1` | **Yes** | file:line below |

Hashes below are from the GitHub mirrors (`github.com/openbsd/src`,
`github.com/NetBSD/src`, `github.com/freebsd/freebsd-src`). The OpenBSD and
NetBSD entries are CVS-era commits; the canonical reference is the committer +
date, viewable via each project's cvsweb.

---

## Prior-art fixes (anchor on `rcv_nxt`)

### OpenBSD — 2005-12-01, `markus` — mirror `89ef4ab4d975`
`sys/netinet/tcp_input.c`. Core delayed-ACK fix; reviewed on tech@.

> allow RST if the th_seq matches rcv_nxt in case the RST follows the data
> immediately. otherwise we would ignore RST for delayed acks; ok deraadt,
> dhartmei

### OpenBSD — 2006-12-11, `markus` — mirror `02c0aee39485`
Adds the `rcv_nxt + 1` arm (FIN-consumed / Windows clients). Listed in the
OpenBSD 4.1 changelog (`openbsd.org/plus41.html`).

> allow RST with th_seq incremented (seen from windows tcp clients); ok dhartmei

Resulting OpenBSD test (current `master`, ~L1400):
```c
if (th->th_seq != tp->last_ack_sent &&
    th->th_seq != tp->rcv_nxt &&
    th->th_seq != (tp->rcv_nxt + 1))
        goto drop;
```

### NetBSD — 2009-06-20, `christos` — mirror `8d20d2e95368`
`sys/netinet/tcp_input.c`. Patch from Joanne M Mikkelson. No GNATS PR; it went
straight from the tech-net thread (below) to commit.

> Follow exactly the recommendation of draft-ietf-tcpm-tcpsecure-11.txt: Don't
> check gainst the last ack received, but the expected sequence number. This
> makes RST handling independent of delayed ACK. From Joanne M Mikkelson.

(`draft-ietf-tcpm-tcpsecure-11` is the draft that became RFC 5961.)

Resulting NetBSD test (current `trunk`, ~L2370):
```c
if (tiflags & TH_RST) {
        if (th->th_seq != tp->rcv_nxt)
                goto dropafterack_ratelim;
        /* ... reset ... */
}
```

---

## Bug lineage (anchor on `last_ack_sent`, never fixed)

### FreeBSD — 2014-09-16, `Gleb Smirnoff` — mirror `3220a2121cc9`
Introduced the RFC 5961 challenge-ACK path, in response to `FreeBSD-SA-14:19.tcp`.
This is the origin of both the `last_ack_sent == th->th_seq` inner test and the
comment that acknowledges the delayed-ACK gap yet does nothing about it.

> In 2010 a new technique for mitigation of these attacks was proposed in
> RFC5961 [...] The idea is to send a "challenge ACK" packet to the peer [...]

Current `main`, `sys/netinet/tcp_input.c` ~L2119–2161. Note the in-tree comment:
```c
/*
 * Note: to take into account delayed ACKs, we should
 *   test against last_ack_sent instead of rcv_nxt.
 */
if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
    SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||
    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
        ...
        if (V_tcp_insecure_rst ||
            tp->last_ack_sent == th->th_seq) {   /* reset */ }
        else { tcp_send_challenge_ack(tp, th, m); }   /* <-- hang */
}
```
The comment argues the delayed-ACK case should be handled — the code then anchors
on `last_ack_sent` anyway.

### XNU / Darwin (macOS)
`bsd/netinet/tcp_input.c` (`apple-oss-distributions/xnu`, `main`) ~L4476–4482.
Snapshot-release repo, so per-line blame is not meaningful; cite by file:line.
Inner reset test accepts `last_ack_sent` or `last_ack_sent - 1`, never `rcv_nxt`,
with no `insecure_rst` escape hatch — the strictest of the group and the worst
delayed-ACK behaviour observed.
```c
if (tp->last_ack_sent == th->th_seq || tp->last_ack_sent - 1 == th->th_seq) {
        /* reset */
} else {
        /* challenge ACK / dropafterack */
}
```

### DragonFly BSD
`sys/netinet/tcp_input.c` (`DragonFlyBSD/DragonFlyBSD`, `master`) ~L1681–1683.
No RFC 5961 challenge-ACK path at all, so immune to the *primary* (challenge-ACK)
form — but the in-window check is anchored on `last_ack_sent` at both edges:
```c
if (SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
    SEQ_LEQ(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) {
        /* reset */
}
goto drop;
```
When the delayed-ACK gap exceeds `rcv_wnd`, a RST at `rcv_nxt` falls outside
`[last_ack_sent, last_ack_sent + rcv_wnd]` and is silently dropped — the same
residual hang, via the outer window edge rather than a challenge ACK.

---

## Discussion / advisory trail

- **NetBSD tech-net, June 2009 — "bad interaction between TCP delayed ack and
  RSTs"** (Joanne M Mikkelson).
  Thread start: <https://mail-index.netbsd.org/tech-net/2009/06/16/msg001400.html>
  Reply pointing at SA2004-006 + the tcpsecure draft:
  <https://mail-index.netbsd.org/tech-net/2009/06/17/msg001408.html>
  Describes the exact scenario: delayed ACK, peer (Windows) sends RST/ACK at
  `rcv_nxt`, NetBSD ACKs the RST and drops it. Fixed four days later by the
  christos commit above.
- **NetBSD SA2004-006** — introduced the earlier "respond to RST by ACK"
  behaviour that *caused* the delayed-ACK drop, later corrected by anchoring on
  `rcv_nxt`.
- **FreeBSD-SA-14:19.tcp** — the advisory that motivated FreeBSD's 2014 RFC 5961
  work (`3220a2121cc9`).

## External references

- Lars Snellman, "The many ways of handling TCP RST packets" (2016):
  <https://www.snellman.net/blog/archive/2016-02-01-tcp-rst/>
  Cross-OS archaeology; quotes the FreeBSD `last_ack_sent` comment and catalogues
  how the exact-match rule was loosened per OS (incl. the XNU `± 1` variant).
- FreeBSD `tcp-testsuite`, `rcv-rst-last-ack`:
  <https://github.com/freebsd-net/tcp-testsuite/blob/master/state-event-engine/rcv-rst-last-ack/README.md>
- RFC 5961, "Improving TCP's Robustness to Blind In-Window Attacks":
  <https://www.rfc-editor.org/rfc/rfc5961>

## Argument for freebsd-net@

The `rcv_nxt` anchor is the deployed, reviewed choice in both sibling BSDs:
OpenBSD since 2005, NetBSD since 2009, each landed with a commit message that
names the delayed-ACK RST failure. FreeBSD's own in-tree comment (since 2014) has
acknowledged the same gap without acting on it. The change requested in
`FREEBSD_BUG.md` brings FreeBSD in line with prior art rather than introducing
new behaviour; DragonFly and XNU carry variants of the same unfixed anchor.
