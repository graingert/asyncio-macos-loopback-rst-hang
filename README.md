# TCP reset (RST) rejected by the RFC 5961 window check: abortive close of a flow-controlled loopback connection

A minimal, dependency-free reproducer for a **BSD/kqueue-family kernel bug** in
the RFC 5961 RST-acceptance check. When a **flow-controlled** loopback TCP
connection is **abortively closed** — `setsockopt(SO_LINGER, {1, 0})` then
`close()` — the RST is emitted correctly and put on the wire, but the peer's
kernel **rejects a valid RST** and stays half-open.

The RST carries `SEG.SEQ == SND.NXT` (RFC-correct). The receiver drops it because
its RFC 5961 check computes the acceptance-window right edge from `last_ack_sent`
instead of `rcv_nxt`; when the receiver is flow controlled (small `rcv_wnd`) and
holds received-but-unacked data (delayed ACK, so `rcv_nxt > last_ack_sent`), a
RST at `rcv_nxt` falls just past the mis-computed edge and is silently discarded.
See [Root cause](#root-cause) for the code and packet-level proof.

Found via `asyncio`, but it is **not** asyncio-specific: it reproduces with a
plain `selectors` loop, from Rust, and from pure C. It is **not macOS-specific**
either — it reproduces on **FreeBSD** (the original kqueue platform) too, so it
is a **BSD/kqueue-family** bug that macOS 26 makes dramatically more frequent.
Never observed on Linux (`epoll`).

**Reported upstream:** Apple Feedback Assistant **FB23590387** · CPython
[python/cpython#153117](https://github.com/python/cpython/issues/153117).

## Symptom

The peer is left with a socket that stays `ESTABLISHED` forever:

- its registered reader callback never fires,
- `getpeername()` still succeeds,
- `SO_ERROR` is `0`,
- a `MSG_PEEK` `recv()` returns `EWOULDBLOCK`.

The closing side is textbook-correct: at `close()` time it is still connected,
`SO_LINGER` is `{1, 0}`, and `close()` emits a RST at `SND.NXT`. Its file
descriptor is then released cleanly (verified with `lsof`: no lingering fd). A
packet capture confirms the **RST is on the wire** — so it is not "lost". The
peer receives it and drops it in its RFC 5961 check, leaving the connection
half-open.

## Root cause

The RFC 5961 §3.2 reset check (FreeBSD `sys/netinet/tcp_input.c`, ~line 2131;
Darwin's `xnu` shares this lineage) computes its acceptance-window right edge from
`last_ack_sent` rather than `rcv_nxt`:

```c
if ((SEQ_GEQ(th->th_seq, tp->last_ack_sent) &&
     SEQ_LT(th->th_seq, tp->last_ack_sent + tp->rcv_wnd)) ||   // right edge is wrong
    (tp->rcv_wnd == 0 && tp->last_ack_sent == th->th_seq)) {
```

The left edge is deliberately anchored at `last_ack_sent` ("to take into account
delayed ACKs", per the code's own comment), but the width `rcv_wnd` is measured
from `rcv_nxt`. So the right edge is `last_ack_sent + rcv_wnd`, short of the true
window edge `rcv_nxt + rcv_wnd` by exactly `rcv_nxt - last_ack_sent` — the
received-but-unacknowledged bytes. When the receiver is flow controlled (small
`rcv_wnd`) and holds a burst of unacked data, a RST at `SEG.SEQ == rcv_nxt`
(the sender's `SND.NXT` — RFC-correct) lands past the mis-computed edge and is
silently dropped.

Absolute-sequence capture of a real hang on `lo0` (49164 closes, 49163 is left
`ESTABLISHED`):

```
S->C  ack 3081222923, win 246                    # last ACK the receiver sent
C->S  P. seq 3081222923:3081223131, length 208   # closer's final data burst
C->S  R. seq 3081223131                          # the RST — dropped by the peer
S->C  ack 3081223131, win 243                    # peer *had* the 208 bytes unacked
```

`last_ack_sent` = 3081222923, but the peer had already accepted the 208-byte
segment (`rcv_nxt` = 3081223131) without yet ACKing it. The RST's seq =
3081223131 = `rcv_nxt`, which is `>= last_ack_sent + rcv_wnd` whenever
`rcv_wnd <= 208` — so it fails both clauses and is discarded. The peer only ACKs
3081223131 *after* the RST, proving the data was unacked when the RST arrived.

A two-line receiver-side fix (anchor the right edge at `rcv_nxt + rcv_wnd`, and
accept `rcv_nxt` as an exact match) is written up in
[`FREEBSD_BUG.md`](FREEBSD_BUG.md).

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
  no `asyncio`). So it is a **kernel-level RST rejection** (RFC 5961 window
  check), not an asyncio bug.
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
  and the buggy `last_ack_sent + rcv_wnd` check is present verbatim in FreeBSD's
  own `sys/netinet/tcp_input.c` — so the root cause is BSD-family, not something
  Apple added to XNU. macOS 26 just makes it enormously more frequent. (The
  wire-level capture above is from macOS; the `freebsd-capture` CI job gathers the
  equivalent proof natively on FreeBSD.)
- Never observed on **Linux** (`epoll`) across many millions of iterations.

## Each iteration

Using only raw sockets plus the low-level fd APIs:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes drive the receive window to
   zero and fill the client's send buffer.
3. The client fd is registered with the selector/loop and filled until `send()`
   blocks.
4. The client is unregistered and abortively closed (`SO_LINGER {1,0}` +
   `close()`) — either immediately or after one poll cycle.
5. The server fd is registered; it drains the buffered data, then waits for the
   disconnect (a `recv()` raising `ECONNRESET`, or EOF).
6. If the disconnect never arrives within the timeout, the reset was lost.
