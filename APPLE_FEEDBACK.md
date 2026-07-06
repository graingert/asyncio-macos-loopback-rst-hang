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

The failure is rare on macOS 14/15 (order 1 in 10^5–10^6 connections) but frequent on macOS 26 (order 1 in 3–20 depending on machine), and is much more likely when a `kevent` poll runs between deleting the closing fd's filters and calling `close()`.

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

A packet capture on `lo0` shows that the closing side **does put a reset on the wire** for the hung connection — an `[R.]` (RST+ACK) — yet the peer's socket does **not** transition out of `ESTABLISHED`. On macOS 26.4 the reproducer hangs on its **first** connection, so the capture is perfectly isolated: the entire capture below (`tcpdump -n -i lo0` filtered to SYN/RST/FIN, **3 packets captured, 0 dropped by kernel**) is the hung connection and nothing else — no port reuse, no other traffic:

```
1783325598.092457 IP 127.0.0.1.49167 > 127.0.0.1.49166: Flags [S], seq 3301250272, win 65535, options [mss 16344,nop,wscale 6,nop,nop,TS val 1345239134 ecr 0,sackOK,eol], length 0
1783325598.092599 IP 127.0.0.1.49166 > 127.0.0.1.49167: Flags [S.], seq 3782955689, ack 3301250273, win 65535, options [mss 16344,nop,wscale 6,nop,nop,TS val 4275974469 ecr 1345239134,sackOK,eol], length 0
1783325598.093571 IP 127.0.0.1.49167 > 127.0.0.1.49166: Flags [R.], seq 393217, ack 1, win 6380, length 0
```

The client (port 49167) sends the RST ~1 ms after the handshake, immediately after filling the server's zero receive window; the server socket (port 49166) nevertheless remains `ESTABLISHED` (`SO_ERROR` 0, `MSG_PEEK` `EAGAIN`). The same pattern was captured earlier on macOS 14 (in a busier run, with per-connection isolation limited by ephemeral-port recycling).

So the reset is transmitted but is not applied to the peer socket. A *delivered* abortive close sends an identical-looking `[R.]` and does reset the peer, so the difference is in how the inbound reset is handled — this looks related to RST validation while the receiver's window is zero.

## Configuration

- Reproduced on all of:
  - macOS 14.8.7 (build 23J520), `xnu-10063.141.1.712.16~1/RELEASE_ARM64_VMAPPLE arm64` (GitHub-hosted `macos-14` runner);
  - macOS 15.7.7 (build 24G720), `xnu-11417.140.69.710.16~1/RELEASE_ARM64_VMAPPLE arm64` (GitHub-hosted `macos-15` runner);
  - macOS 26.4 (build 25E246), `xnu-12377.101.15~1/RELEASE_ARM64_VMAPPLE arm64` (GitHub-hosted `macos-26` runner);
  - macOS 26.5.1 (build 25F80), `xnu-12377.121.6~2/RELEASE_ARM64_T8122 arm64`, MacBook Pro (Mac15,3, Apple M3) — physical hardware, not a VM.
- Hardware: Apple Silicon (arm64) — both virtualized (VMAPPLE CI runners) and physical (M3 MacBook Pro).
- Frequency: **dramatically more frequent on macOS 26** — 596 / 2,113 connections (~28%) in a single 600-second CI run of `c/repro.c` on macOS 26.4, and 59 / 1,212 (~5%) in a 60-second local run on macOS 26.5.1, versus ~1 in 10^5 on macOS 14 and ~1 in 10^6 on macOS 15. On macOS 26 the hung peers remain `ESTABLISHED` past a 10-second wait as well (reproducer rebuilt with `TIMEOUT_SEC 10.0`: 6 / 180), ruling out late delivery. The packet capture above is from macOS 26.4; macOS 14 and 15 show the same behavior but are far rarer to catch.
- Interface: loopback (`lo0`), IPv4 127.0.0.1.
- Not reproducible on Linux.

## Attachments to include

- `c/repro.c` (attached) — the self-contained reproducer.
- A `tcpdump -i lo0` capture (`.pcap`) of a hung connection — the 3-packet macOS 26.4 capture quoted above (CI artifact `capture-macos-26`: pcap + repro log).
- A `sysdiagnose` taken immediately after the hang, from the same boot as the capture (same CI artifact).

## Real-world impact

This surfaces as a permanent, silent hang of otherwise-correct asyncio programs on macOS (a registered reader that never fires), because asyncio defers a transport's `close()` to a later event-loop iteration (`call_soon`) — which reliably places a `kevent` poll between unregistering and closing the fd, the amplifying condition. Reported to CPython at python/cpython#153117.
