# TCP RST at rcv_nxt gets a challenge ACK instead of a reset: abortive close of a loopback connection

A minimal, dependency-free reproducer for a **BSD/kqueue-family kernel bug** in
RFC 5961 RST handling. When a loopback TCP connection is **abortively closed** —
`setsockopt(SO_LINGER, {1, 0})` then `close()` — the RST is emitted correctly and
put on the wire, but if the peer is holding delayed-ACKed data the peer answers
with a **challenge ACK instead of resetting**, and the connection is left
half-open.

The RST carries `SEG.SEQ == SND.NXT == the peer's rcv_nxt` (RFC-correct — RFC 5961
says a RST at `RCV.NXT` MUST reset). The receiver declines to reset because its
RFC 5961 exact-match test compares `th_seq` against `last_ack_sent` instead of
`rcv_nxt`; when a delayed/stretch ACK leaves `rcv_nxt > last_ack_sent`, the RST is
treated as in-window-but-not-exact and gets a challenge ACK. The originating side
has already fully closed (nothing retransmits the RST), so the connection stays
`ESTABLISHED` until the application gives up and closes it — seconds later, long
after an application timeout has seen the hang. See [Root cause](#root-cause) for
the code, a DTrace trace of the receiver's control block at the drop, and the
wire capture.

Found via `asyncio`, but it is **not** asyncio-specific: it reproduces with a
plain `selectors` loop, from Rust, and from pure C. It is **not macOS-specific**
either — it reproduces on **FreeBSD** (the original kqueue platform) too, so it
is a **BSD/kqueue-family** bug that macOS 26 makes dramatically more frequent.
Never observed on Linux (`epoll`).

**Reported upstream:** Apple Feedback Assistant **FB23590387** · CPython
[python/cpython#153117](https://github.com/python/cpython/issues/153117).

## Symptom

The peer is left with a socket that stays `ESTABLISHED` (until the application
gives up and closes it — well past any reasonable app timeout):

- its registered reader callback never fires,
- `getpeername()` still succeeds,
- `SO_ERROR` is `0`,
- a `MSG_PEEK` `recv()` returns `EWOULDBLOCK`.

The closing side is textbook-correct: at `close()` time it is still connected,
`SO_LINGER` is `{1, 0}`, and `close()` emits a RST at `SND.NXT`. Its file
descriptor is then released cleanly (verified with `lsof`: no lingering fd). A
packet capture confirms the **RST is on the wire** — so it is not "lost". The
peer receives it and, instead of resetting, sends a challenge ACK.

## Root cause

RFC 5961 §3.2 requires that a RST with `SEG.SEQ == RCV.NXT` resets the connection,
and that an in-window RST that is *not* `RCV.NXT` draws a challenge ACK. FreeBSD's
check (`sys/netinet/tcp_input.c`, ~line 2160 in 15.1-RELEASE; Darwin's `xnu` shares this lineage)
does the exact-match test against `last_ack_sent`, not `rcv_nxt`:

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
```

The code comment above the test even records the incomplete choice: *"Note: to
take into account delayed ACKs, we should test against last_ack_sent instead of
rcv_nxt."* It tests `last_ack_sent` **only**. With the default
`net.inet.tcp.insecure_rst = 0`, the connection resets **iff**
`last_ack_sent == th_seq`; when `rcv_nxt > last_ack_sent` (delayed ACK), a RST at
`rcv_nxt` is challenge-ACKed instead of resetting.

**DTrace at the moment of the drop** (`dtrace/rst_drop.d`, hooking
`fbt::tcp_do_segment:entry`), one real hang on FreeBSD 15.1/amd64:

```
th_seq=1888935822 rcv_nxt=1888935822 last_ack_sent=1888919490 rcv_wnd=16640
  seq-rcv_nxt        =  0     # RST is exactly at rcv_nxt -> MUST reset per RFC 5961
  seq-last_ack_sent  = 16332  # last_ack_sent lags one full segment (delayed ACK)
  (last_ack_sent+rcv_wnd)-seq = +308   # RST is INSIDE the window -> not a window drop
```

`seq - rcv_nxt = 0` and a positive window margin prove this is the inner
exact-match, not a window/right-edge drop. The matching wire capture (same
connection; 52374 closes, 22467 is left hung):

```
52374->22467  . seq 1888919490:1888935822, length 16332   # closer's final full segment
52374->22467  R. seq 1888935822                            # the RST, seq == rcv_nxt
22467->52374  . ack 1888935822, win 175                    # CHALLENGE ACK -- not a reset
   ... 3.07 s later ...
22467->52374  F. seq 1850944913, ack 1888935822            # app gives up, closes (FIN)
```

The receiver-side fix has **three** required parts — accept `rcv_nxt` as an exact
match in the reset test, anchor the outer window clause's right edge at
`rcv_nxt + rcv_wnd`, **and** accept `rcv_nxt` in the zero-window arm — written up
in [`FREEBSD_BUG.md`](FREEBSD_BUG.md).

**Validated on a patched kernel.** Built into FreeBSD 15.1-RELEASE-p1 GENERIC
kernels under QEMU/KVM, with distinct idents (`uname -i`-verified) so each result
is unambiguously attributable:

| kernel (`uname -i`) | result |
| --- | --- |
| stock `GENERIC` | hangs — **15 / 1,919,209** (~1 in 128k); DTrace: every hang is the challenge-ACK case |
| `RSTINNER` (inner fix only) | primary hang gone, **residual persists** (first hang at 287k / 19.9M connections) |
| `RSTBOTH` (both fixes) | **0 / 31,651,215** |

The inner fix alone is *not* enough: a residual persists, and a `tcpdump` capture
on `RSTINNER` proves it is the **outer-clause drop** the second change targets
(RST at `rcv_nxt`, `last_ack_sent` lagging one 16332-byte segment, `rcv_wnd≈308`,
so `seq ≫ last_ack_sent + rcv_wnd` → silently dropped). Changes 1 + 2 together
eliminate the *organic* hang (0 across 31.65M connections, where `RSTINNER` hit it
within ~20M). The residual is a timing-sensitive Heisenbug — bursty, and perturbed
by `fbt` DTrace on the hot path, so the wire capture uses `tcpdump`'s BPF tap; see
[`FREEBSD_BUG.md`](FREEBSD_BUG.md).

The **third part** (accept `rcv_nxt` in the zero-window arm) covers a case an
organic sender never reaches — hence `RSTBOTH`'s 0/31.65M above — but crafted
packets do. Two GENERIC kernels differing only by that one line, `RSTBOTH`
(changes 1 + 2) and `RSTARMB` (changes 1 + 2 + 3), give a clean red/green on a
deterministic packetdrill test (`tests/rst-rcvnxt-zero-window.pkt`, RST at
`rcv_nxt` with `rcv_wnd == 0` under a delayed ACK): `RSTBOTH` fails it
(`read` → `EAGAIN`), `RSTARMB` passes it (`ECONNRESET`), both pass the other two
tests.

## Run

```console
$ python3 repro.py [SECONDS]                    # asyncio, deferred close
$ python3 repro_selectors.py [SECONDS]          # plain selectors, immediate close
$ python3 repro_selectors_deferred.py [SECONDS] # plain selectors, deferred close
$ python3 repro_kqueue.py [SECONDS]             # raw select.kqueue, deferred close
```

Default 300s. Exit status is non-zero if any undelivered (half-open) close is
observed. Each occurrence prints diagnostics proving the peer is still
established:

```
  [418733] UNDELIVERED: getpeername=('127.0.0.1', 49187) SO_ERROR=0 MSG_PEEK=EAGAIN
```

## Observations

Undelivered / total, from a single ~10-minute CI run per cell (rates are low and
noisy — an occasional `0` does **not** mean "cannot reproduce"):

| variant | macOS 14 | macOS 15 | macOS 26 | Linux |
| --- | --- | --- | --- | --- |
| `repro.py` — asyncio (deferred close) | **19 / 1.46M** | **4 / 0.93M** | **596 / 1,291** | 0 / 1.1M |
| `repro_selectors.py` — plain `selectors`, immediate close | **5 / 1.55M** | 0 / 1.16M | **597 / 1,484** | 0 / 1.3M |
| `repro_selectors_deferred.py` — plain `selectors`, deferred close | **21 / 1.89M** | **1 / 1.27M** | **597 / 1,553** | 0 / 1.3M |
| `repro_kqueue.py` — raw `select.kqueue`, deferred close | **9 / 1.93M** | 0 / 1.36M | **597 / 1,469** | n/a (no kqueue) |

On macOS 26 the failure is no longer rare: **28–46% of connections hang**. The
undelivered counts sit at ~597 for every variant because they are wall-clock
saturated — each hang consumes the full 1s detection timeout, so a 600s run
tops out at ~600 hangs regardless of how many fast iterations fit in between.

It is **not macOS-specific** — the same `c/repro.c` reproduces on **FreeBSD**
(the original kqueue platform), on **`amd64`** (every macOS run above is arm64,
so it is not architecture-specific either):

| platform (`c/repro.c`, deferred close) | undelivered / total |
| --- | --- |
| macOS 26.4 (arm64) | **596 / 2,113** (~28%) |
| macOS 14 (arm64) | ~1 in 10^5 |
| **FreeBSD 15.1-RELEASE (amd64)** | **312 / 10.34M** (~1 in 33k) |
| Linux (epoll) | 0 / many millions |

So this is a **BSD/kqueue-family** RST-rejection bug, present on FreeBSD at a low
rate and amplified enormously on macOS 26.

## What this shows

- The hang reproduces with **pure `selectors` and no asyncio** (immediate close,
  macOS 14), and with the **raw `select.kqueue`** API directly (no `selectors`,
  no `asyncio`). So it is a **kernel-level RST-handling bug** (RFC 5961 exact-match
  test), not an asyncio bug.
- It also reproduces with **no Python at all** — a pure **Rust** program
  (`rust/`, raw `kqueue`/`kevent` via `libc`: macOS 14 **20 / 1.89M**, macOS 26
  **597 / 1,831**) and a pure **C** program (`c/repro.c`, raw `kqueue`/`kevent`,
  no libraries: macOS 26 **596 / 2,113**, FreeBSD 15.1 amd64 **312 / 10.34M**).
  This is a language-agnostic, cross-architecture **BSD/kqueue-family kernel**
  bug — not unique to macOS or to Apple Silicon.
- The **deferred close amplifies it** on macOS 14/15 — roughly 3–4× more
  frequent, and it is what surfaces the bug on macOS 15. On macOS 26 the
  amplification has all but vanished: immediate close hangs ~40% of connections
  too. "Deferred close" means a `select()` /
  kqueue poll cycle runs **between** unregistering the fd from the selector and
  abortively closing it:

  ```
  unregister(fd)  ->  select()  ->  setsockopt(SO_LINGER {1,0}); close(fd)
  ```

  The immediate-close variant does `unregister(fd)` then `close(fd)` with no poll
  in between, and hits the bug much less often. asyncio always inserts that poll
  cycle, because it defers the `close()` to a later loop iteration via
  `call_soon`; that is why asyncio programs hit this most.
- Reproduces on **FreeBSD** too (`c/repro.c`, 15.1-RELEASE amd64: 312 / 10.34M),
  and the buggy `last_ack_sent`-based exact-match test is present verbatim in
  FreeBSD's own `sys/netinet/tcp_input.c` — so the root cause is BSD-family, not
  something Apple added to XNU. macOS 26 just makes it enormously more frequent.
  On FreeBSD the `freebsd` CI job proves it natively at three levels for the same
  connection: the symptom, the wire capture, and a **DTrace** trace of the
  receiver's `rcv_nxt` / `last_ack_sent` / `rcv_wnd` at the drop (`dtrace/rst_drop.d`).
- Never observed on **Linux** (`epoll`) across many millions of iterations.

## Each iteration

Using only raw sockets plus the low-level fd APIs:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes drive the server's receive
   window down and fill the client's send buffer (and let a final segment sit
   received-but-unacked — the `rcv_nxt > last_ack_sent` gap the bug needs).
3. The client fd is registered with the selector/loop and filled until `send()`
   blocks.
4. The client is unregistered and abortively closed (`SO_LINGER {1,0}` +
   `close()`) — either immediately or after one poll cycle.
5. The server fd is registered; it drains the buffered data, then waits for the
   disconnect (a `recv()` raising `ECONNRESET`, or EOF).
6. If the disconnect never arrives within the timeout, the reset was lost.
