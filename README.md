# asyncio loopback abort: lost RST leaves a half-open connection on macOS

A minimal, dependency-free reproducer for a **lost TCP reset (RST) on macOS
loopback** when the connection is driven by an `asyncio` `SelectorEventLoop`
(`KqueueSelector`).

## Symptom

When a flow-controlled (zero receive window) loopback TCP connection is
**abortively closed** by one side — `setsockopt(SO_LINGER, {1, 0})` then
`close()` — the reset is occasionally **never delivered** to the peer. The peer
is left with a socket that is still `ESTABLISHED` forever:

- its `loop.add_reader` callback never fires,
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
$ python3 repro.py             # asyncio variant, 300s by default
$ python3 repro.py 600         # run longer
$ python3 repro_selectors.py   # same scenario using only `selectors` (no asyncio)
```

Exit status is non-zero if any undelivered (half-open) close is observed. Each
occurrence prints diagnostics proving the peer socket is still established:

```
  [418733] UNDELIVERED: getpeername=('127.0.0.1', 49187) SO_ERROR=0 MSG_PEEK=EAGAIN
```

## Expected vs actual

- **Expected:** every abortive `close()` results in the peer's registered reader
  observing the disconnect (a `recv()` raising `ECONNRESET`, or `EOF`), within a
  timeout — as it always does on Linux (`epoll`).
- **Actual (macOS 14):** rarely, the peer's reader never fires and the socket
  stays `ESTABLISHED`.

## Observations

| Platform | Selector | Result |
| --- | --- | --- |
| macOS 14 | `KqueueSelector` | reproduces, rarely — order **1 in 10⁵–10⁶** iterations |
| macOS 15 | `KqueueSelector` | not observed in ~10⁶ iterations |
| Linux | `EpollSelector` | never observed |

GitHub Actions across `macos-14`, `macos-15`, and `ubuntu-latest` is included in
`.github/workflows/ci.yaml`.

## What the reproducer does

Each iteration uses only the low-level event-loop file descriptor APIs
(`add_reader` / `add_writer` / `remove_reader` / `remove_writer`) plus raw
sockets, to stay close to what a selector-based event loop does per connection:

1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes drive the receive window to
   zero and fill the client's send buffer.
3. The client is registered with the loop and fills until `send()` blocks.
4. The client is unregistered and abortively closed (`SO_LINGER {1,0}` +
   `close()`).
5. The server is registered with `add_reader`; its callback drains the buffered
   data, then waits for the disconnect.
6. If the disconnect never arrives within the timeout, the reset was lost.

## Is this CPython or the OS?

The closing side does everything correctly, so the missing RST is ultimately
macOS kernel behavior (a reset that is not delivered to a loopback peer while the
connection is in a zero-window / persist state). It is reported here because it
is readily reproduced through `asyncio` and manifests as a silent, permanent
hang of an otherwise-correct asyncio program on macOS. A pure `selectors` (no
`asyncio`) variant is expected to exhibit it at a similarly low rate.
