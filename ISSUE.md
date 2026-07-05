# Title

macOS: abortive close (`SO_LINGER {1,0}`) of a flow-controlled loopback socket occasionally loses the RST, hanging asyncio peers

# Body

## Summary

On macOS, when a flow-controlled (zero receive window) loopback TCP connection is **abortively closed** — `setsockopt(SO_LINGER, {1, 0})` then `close()` — the reset (RST) is occasionally **never delivered** to the peer. The peer's socket stays `ESTABLISHED` forever: its registered reader never fires, `getpeername()` still succeeds, `SO_ERROR == 0`, and `recv(..., MSG_PEEK)` returns `EWOULDBLOCK`. The connection is left half-open.

The closing side is textbook-correct at `close()` time (still connected, `SO_LINGER` set to `{1,0}`, unsent bytes in the send buffer), and its fd is released cleanly (`lsof` confirms no lingering fd). So per POSIX/BSD semantics `close()` must emit a RST — but the peer never sees it, nor a FIN.

**This surfaces as a permanent, silent hang of otherwise-correct asyncio programs on macOS.** It is ultimately a macOS kernel lost-RST (it reproduces with plain `selectors`, no asyncio), but asyncio's normal teardown pattern makes it markedly more frequent — see below.

## Reproducer

Full runnable reproducers + red CI on macOS: **https://github.com/graingert/asyncio-macos-loopback-rst-hang**

Three variants, all stdlib-only:
- `repro.py` — asyncio (`loop.add_reader`/`add_writer` + a `call_soon`-deferred abortive close)
- `repro_selectors.py` — plain `selectors`, immediate close
- `repro_selectors_deferred.py` — plain `selectors`, close deferred by one poll cycle

Each iteration: establish a loopback pair; the server never reads so the client's writes drive the window to zero and fill its send buffer; register the client fd, fill until `send()` blocks; unregister and abortively close the client; register the server and wait for the disconnect. If the disconnect never arrives within a timeout, the RST was lost.

## Results (single ~10-minute CI run per cell; undelivered / total)

| variant | macOS 14 | macOS 15 | Linux |
| --- | --- | --- | --- |
| asyncio (deferred close) | **19 / 1.46M** | **4 / 0.93M** | 0 / 1.1M |
| `selectors`, immediate close | **5 / 1.55M** | 0 / 1.16M | 0 / 1.3M |
| `selectors`, deferred close | **21 / 1.89M** | **1 / 1.27M** | 0 / 1.3M |

Rates are low (~1–13 per million) and noisy, so an occasional `0` does not mean "cannot reproduce."

## Analysis

1. **It is not an asyncio logic bug** — it reproduces with a plain `selectors` (`KqueueSelector`) loop and no asyncio (macOS 14, immediate close). The underlying lost RST is macOS kernel behavior.
2. **The deferred close amplifies it ~3–4×.** "Deferred close" = a `select()`/kqueue poll cycle runs *between* unregistering the fd and abortively closing it:
   ```
   unregister(fd)  ->  select()  ->  setsockopt(SO_LINGER {1,0}); close(fd)
   ```
   The immediate-close variant (`unregister(fd); close(fd)` with no poll between) hits it much less often.
3. **asyncio always inserts that poll cycle**, because a transport's `close()`/abort is deferred to a later loop iteration via `call_soon`. That is why asyncio programs on macOS hit this most, and hang permanently when they do (`loop.sock_recv` / a registered reader that never fires).
4. Never observed on Linux (`epoll`) across many millions of iterations.

## Environment

- macOS 14 and macOS 15 (GitHub-hosted `macos-14` / `macos-15` runners, Apple Silicon), CPython 3.13.
- Reproduces on the default `SelectorEventLoop` (`KqueueSelector`).
- Does not reproduce on Linux.

## Not a duplicate of

- #77444 — "Asyncio server enters an invalid state after a request with SO_LINGER" (server-side accept/state; not a lost RST).
- #71573 — "Asyncio server hang when clients connect and immediately disconnect" (server-side hang; different mechanism).

Those are server-side; this is a client-side abortive close whose RST is lost, leaving the *peer* half-open.
