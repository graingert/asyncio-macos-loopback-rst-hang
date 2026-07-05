# Apple Feedback Assistant report (draft)

Submit at https://feedbackassistant.apple.com (sign in with an Apple ID).
Suggested: **Product** = macOS, **Area** = Networking, **Type** = Incorrect/Unexpected Behavior.

---

## Title

TCP RST from an abortive close (SO_LINGER {1,0}) is occasionally not delivered to a loopback peer, leaving it half-open

## Which area are you seeing an issue with?

Networking (TCP / loopback / kqueue)

## Description

On macOS, abortively closing a flow-controlled (zero receive window) loopback TCP connection — `setsockopt(SO_LINGER, {l_onoff=1, l_linger=0})` followed by `close()` — occasionally does **not** deliver the RST to the peer. The peer's socket remains in `ESTABLISHED` state indefinitely:

- an `EVFILT_READ` registration for it never fires,
- `getpeername()` still succeeds,
- `getsockopt(SO_ERROR)` returns 0,
- `recv(..., MSG_PEEK)` returns `EAGAIN`.

No RST or FIN is delivered; the connection is left half-open forever. The closing side is correct at `close()` time (still connected, `SO_LINGER` set to `{1,0}`, unsent bytes queued in the send buffer), and its file descriptor is released cleanly (no lingering descriptor per `lsof`). Per BSD socket semantics `close()` on such a socket must emit a RST.

The failure is rare (order 1 in 10^5–10^6 connections) and is much more likely when a `kevent` poll runs between deleting the closing fd's filters and calling `close()`.

## Steps to Reproduce

A self-contained C reproducer (no libraries beyond libc) is attached and lives here:
https://github.com/graingert/asyncio-macos-loopback-rst-hang (`c/repro.c`)

```
cc -O2 -o repro c/repro.c
./repro 600
```

Each iteration, using only sockets + `kqueue`/`kevent`:
1. Establish a loopback client + accepted server socket (both non-blocking).
2. The server never reads, so the client's writes drive the receive window to zero and fill the client's send buffer.
3. Register the client fd with kqueue (EVFILT_READ + EVFILT_WRITE); fill until `send()` returns EAGAIN.
4. `EV_DELETE` the client's filters, run one `kevent` poll, then abortively close it (`SO_LINGER {1,0}` + `close()`).
5. Register the server fd (EVFILT_READ); drain buffered data, then wait up to 1s for the disconnect.
6. If the disconnect never arrives, the RST was lost. The program prints the half-open 4-tuple and exits non-zero.

The same behavior reproduces from Python (`selectors` and `asyncio`) and from Rust — it is language-agnostic. Details and CI results: see the repository README.

## Expected Result

Every abortive `close()` results in the peer observing the reset (a `recv()` returning `ECONNRESET`, surfaced as a readable `EVFILT_READ` event). This is what happens on Linux (epoll) across many millions of iterations, and on macOS in the overwhelming majority of iterations.

## Actual Result

Occasionally the peer never observes the disconnect. Its socket stays `ESTABLISHED` — verified by the reproducer via `getpeername()` (succeeds), `getsockopt(SO_ERROR)` == 0, and `recv(..., MSG_PEEK)` == `EAGAIN`.

Packet capture on `lo0` (`tcpdump`, **0 packets dropped by kernel**) shows the difference between a delivered and a hung abortive close:

- **Delivered** (the overwhelming majority): after the SYN handshake, the closing side emits a `[R.]` (RST+ACK, receive window non-zero) followed by one or more bare `[R]` (RST, window 0); the peer resets correctly. Example:

  ```
  IP 127.0.0.1.52872 > 127.0.0.1.52871: Flags [S]
  IP 127.0.0.1.52871 > 127.0.0.1.52872: Flags [S.]
  IP 127.0.0.1.52872 > 127.0.0.1.52871: Flags [R.], seq 245293, ack 1, win 6379
  IP 127.0.0.1.52872 > 127.0.0.1.52871: Flags [R],  seq 838765268, win 0
  ```

- **Hung**: the closing side emits the `[R.]` (RST+ACK), but the peer's socket does **not** transition out of `ESTABLISHED`, and the trailing bare `[R]` (window 0) segments that accompany a clean teardown do not appear. Example (last packets of the capture; this is the hung connection):

  ```
  IP 127.0.0.1.52874 > 127.0.0.1.52873: Flags [S]
  IP 127.0.0.1.52873 > 127.0.0.1.52874: Flags [S.]
  IP 127.0.0.1.52874 > 127.0.0.1.52873: Flags [R.], seq 540205, ack 1, win 6379
  ```

So the reset the closing side sends does not take effect on the peer. The behavior looks related to RST validation while the receiver's window is zero. (Precise per-connection isolation in a busy capture is limited because ephemeral ports recycle within microseconds; the reproducer reliably triggers the condition for capture with kernel-level tooling.)

## Configuration

- macOS 14.8.7 (build 23J520); `Darwin Kernel Version 23.6.0 ... xnu-10063.141.1.712.16~1/RELEASE_ARM64_VMAPPLE arm64`.
- Hardware: Apple Silicon (arm64) — reproduced on GitHub-hosted `macos-14` runners; also seen, more rarely, on `macos-15`.
- Interface: loopback (`lo0`), IPv4 127.0.0.1.
- Not reproducible on Linux.

## Attachments to include

- `c/repro.c` (attached) — the self-contained reproducer.
- A `tcpdump -i lo0` capture (`.pcap`) of a hung connection.
- A `sysdiagnose` captured shortly after a hang (Terminal: `sudo sysdiagnose`), if possible.

## Real-world impact

This surfaces as a permanent, silent hang of otherwise-correct asyncio programs on macOS (a registered reader that never fires), because asyncio defers a transport's `close()` to a later event-loop iteration (`call_soon`) — which reliably places a `kevent` poll between unregistering and closing the fd, the amplifying condition. Reported to CPython at python/cpython#153117.
