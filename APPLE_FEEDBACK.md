# Apple Feedback Assistant report

Filed as **FB23590387** (https://feedbackassistant.apple.com/feedback/23590387 — visible to the reporter only). **Product** = macOS, **Area** = Networking, **Type** = Incorrect/Unexpected Behavior.

---

## Title

TCP RST from an abortive close (SO_LINGER {1,0}) is occasionally not delivered to a loopback peer, leaving it half-open

## Description

Abortively closing a flow-controlled (zero receive window) loopback TCP connection — `setsockopt(SO_LINGER, {1,0})` then `close()` — occasionally does **not** deliver the RST to the peer. The peer stays `ESTABLISHED` forever: its `EVFILT_READ` never fires, `getpeername()` succeeds, `SO_ERROR` is 0, and `recv(MSG_PEEK)` returns `EAGAIN`. The closing side is correct at `close()` (connected, `SO_LINGER {1,0}`, unsent bytes queued; fd released cleanly per `lsof`), so per BSD semantics `close()` must emit a RST.

Rare on macOS 14/15 (~1 in 10^5–10^6), **frequent on macOS 26** (~1 in 3–20). Much likelier when a `kevent` poll runs between deleting the closing fd's filters and `close()`.

## Steps to Reproduce

Self-contained C reproducer (libc only): https://github.com/graingert/asyncio-macos-loopback-rst-hang (`c/repro.c`)

```
cc -O2 -o repro c/repro.c && ./repro 600
```

Each iteration, using only sockets + `kqueue`/`kevent`:
1. Loopback client + accepted server, both non-blocking.
2. Server never reads → client's writes drive the receive window to zero and fill its send buffer.
3. Register client (READ+WRITE); fill until `send()` = EAGAIN.
4. `EV_DELETE` the client's filters, run one `kevent` poll, then abortively close (`SO_LINGER {1,0}` + `close()`).
5. Register server (READ); drain, then wait 1s for the disconnect.
6. No disconnect → RST lost; prints the half-open 4-tuple and exits non-zero.

Also reproduces from Python (`selectors`, `asyncio`) and Rust — language-agnostic.

## Expected Result

Every abortive `close()` resets the peer (`recv()` → `ECONNRESET`, a readable `EVFILT_READ`). This holds on Linux (epoll) across millions of iterations and on macOS in the overwhelming majority of cases.

## Actual Result

The peer never observes the disconnect. A `lo0` capture shows the closing side **does put the RST on the wire** for the hung connection, yet the peer stays `ESTABLISHED`. On macOS 26.4 the reproducer hangs on its **first** connection, so the whole capture (3 packets, **0 dropped by kernel**) is that one connection:

```
IP 127.0.0.1.49164 > 127.0.0.1.49163: Flags [S], seq 1586118699
IP 127.0.0.1.49163 > 127.0.0.1.49164: Flags [S.], seq 2935135109, ack 1586118700
IP 127.0.0.1.49164 > 127.0.0.1.49163: Flags [R.], seq 393217, ack 1, win 6380
```

Client (49164) sends the RST ~0.4 ms after the handshake, right after filling the zero window; server (49163) stays `ESTABLISHED` (`SO_ERROR` 0, `MSG_PEEK` `errno 35`). A *delivered* abortive close sends an identical `[R.]` and does reset the peer — so the difference is in inbound-RST handling, apparently RST validation while the receive window is zero.

## Configuration

Reproduced on:
- macOS 14.8.7 (23J520), `xnu-10063.141.1.712.16~1` — `macos-14` runner;
- macOS 15.7.7 (24G720), `xnu-11417.140.69.710.16~1` — `macos-15` runner;
- macOS 26.4 (25E246), `xnu-12377.101.15~1` — `macos-26` runner;
- macOS 26.5.1 (25F80), `xnu-12377.121.6~2/RELEASE_ARM64_T8122` — MacBook Pro (Mac15,3, M3), **physical hardware**.

Frequency: **~28%** (596/2,113) in a 600s macOS 26.4 CI run of `c/repro.c`; ~5% (59/1,212) locally on 26.5.1; ~1 in 10^5 (macOS 14) / 10^6 (macOS 15). Rebuilt with a 10s timeout, macOS 26 peers still hung (6/180) — not late delivery. Loopback (`lo0`), IPv4. Not reproducible on Linux.

## Attachments

- `c/repro.c` — the reproducer.
- `lo0.pcap` — the 3-packet macOS 26.4 capture above (CI artifact `capture-macos-26`).
- `sysdiagnose` from the same boot (same artifact).

## Real-world impact

A permanent, silent hang of correct asyncio programs on macOS (a registered reader that never fires): asyncio defers a transport's `close()` via `call_soon`, reliably placing a `kevent` poll between unregister and close — the amplifying condition. Reported to CPython at python/cpython#153117.
