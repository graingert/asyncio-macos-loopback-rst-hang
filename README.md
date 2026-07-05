# Lost TCP reset (RST) on macOS: abortive close of a flow-controlled loopback connection

A minimal, dependency-free reproducer for a **lost TCP reset (RST) on macOS
loopback**. When a flow-controlled (zero receive window) loopback TCP connection
is **abortively closed** — `setsockopt(SO_LINGER, {1, 0})` then `close()` — the
reset is occasionally **never delivered** to the peer, leaving it half-open.

Found via `asyncio`, but it is **not** asyncio-specific: it reproduces with a
plain `selectors` loop too. Never observed on Linux.

## Symptom

The peer is left with a socket that stays `ESTABLISHED` forever:

- its registered reader callback never fires,
- `getpeername()` still succeeds,
- `SO_ERROR` is `0`,
- a `MSG_PEEK` `recv()` returns `EWOULDBLOCK`.

The closing side is textbook-correct at `close()` time — still connected,
`SO_LINGER` set to `{1, 0}`, with unsent bytes in its send buffer — so per
POSIX/BSD semantics `close()` must emit a RST. Its file descriptor is then
released cleanly (verified with `lsof`: no lingering fd). Yet the peer never
sees the RST or a FIN. The connection is left half-open.

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

| variant | macOS 14 | macOS 15 | Linux |
| --- | --- | --- | --- |
| `repro.py` — asyncio (deferred close) | **19 / 1.46M** | **4 / 0.93M** | 0 / 1.1M |
| `repro_selectors.py` — plain `selectors`, immediate close | **5 / 1.55M** | 0 / 1.16M | 0 / 1.3M |
| `repro_selectors_deferred.py` — plain `selectors`, deferred close | **21 / 1.89M** | **1 / 1.27M** | 0 / 1.3M |
| `repro_kqueue.py` — raw `select.kqueue`, deferred close | **9 / 1.93M** | 0 / 1.36M | n/a (no kqueue) |

## What this shows

- The hang reproduces with **pure `selectors` and no asyncio** (immediate close,
  macOS 14), and with the **raw `select.kqueue`** API directly (no `selectors`,
  no `asyncio`). So it is a **macOS kqueue/kernel-level lost RST**, not an asyncio
  bug.
- The **deferred close amplifies it** — roughly 3–4× more frequent, and it is
  what surfaces the bug on macOS 15. "Deferred close" means a `select()` /
  kqueue poll cycle runs **between** unregistering the fd from the selector and
  abortively closing it:

  ```
  unregister(fd)  ->  select()  ->  setsockopt(SO_LINGER {1,0}); close(fd)
  ```

  The immediate-close variant does `unregister(fd)` then `close(fd)` with no poll
  in between, and hits the bug much less often. asyncio always inserts that poll
  cycle, because it defers the `close()` to a later loop iteration via
  `call_soon`; that is why asyncio programs hit this most.
- Never observed on Linux (`epoll`) across millions of iterations.

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
