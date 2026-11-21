# Web Viewer with Ladybird

> **Status as of 2026-11-21: N1–N4 complete — shared DNS, a loading worker and shared HTTP.**
> Web Viewer runs as `/system/webview.sxe`, resolves through SUN's `sxndns` and
> loads remote pages on a per-window network worker through SUN's `sxnhttp`.
> N5 (validated HTTPS) is next. N5 and all Ladybird stages remain pending; no
> Ladybird code has been imported or built.

## Direction

Develop Web Viewer as a native SUN browser using Ladybird's engine and a
SavanXP frontend. Retire the NetSurf plan in full: no NetSurf library adoption,
reference-browser port, or separate in-house CSS layout engine is planned.
The current `/system/webview.sxe` prototype remains available with the capabilities
recorded in [WEBVIEWER.md](../../desktop/WEBVIEWER.md).

The native application is `/system/webview.sxe`, with SUN helpers.
The existing POSIX application was migrated to SUN in a separate change with
its own changelog entry and behavior checks. New platform gaps are delivered as
SUN slices with `nativetest` assertions, following [SUN.md](../../userland/SUN.md).
Running the upstream browser through a compatibility subsystem is not the
native browser milestone.

## Target architecture

The SUN frontend retains Web Viewer's chrome, history, local-file support and
renderer. A reusable SUN loading library separates URL
resolution, DNS, transport and HTTP from rendering. Its result contract must
carry the final URL, HTTP status, content type and body, or a structured error.

Each window has one network worker and at most one active navigation. The worker
owns network operations and publishes results through synchronized queues and
SUN events; it never changes widgets or renderer data. A new navigation cancels
the previous one, and navigation IDs prevent obsolete results from replacing
newer content. Back, Forward, Reload, Stop and window close participate in that
cancellation protocol. Failed loads keep the previous page visible.

Use native shared OpenSSL as a reusable SUN dependency, aligned with the pinned
Ladybird baseline. Do not introduce curl or upstream RequestServer in this
stage. The later engine integration adapts requests to this infrastructure;
compatibility with Ladybird's internal interfaces is not assumed. Show validated
HTTPS in the UI without implying that the current renderer is sandboxed.

## Network-first delivery

Complete networking in the existing Web Viewer before replacing its renderer.
Keep its interface, navigation, history, local files and identity; Ladybird is
an integrated engine at the pin below, never a separate installed browser.
The existing renderer remains until its replacement has its own acceptance gate.

1. Add SUN IPv4 TCP/UDP handles, pending connection/error queries and cancellable
   readiness without changing `sxn_wait_many`; then migrate Web Viewer in a
   separate change to `/system/webview.sxe`, retiring only `/bin/webview`.
2. Give TCP bounded independent servicing, correct asynchronous errors, FIN
   retransmission and TIME_WAIT. Keep POSIX on the same core and retain the
   single outstanding segment initially.
3. Extract a shared SUN DNS resolver (numeric IPv4, A/CNAME, bounded TTL cache,
   source validation, TCP fallback for truncated UDP). Preserve `--dns` and
   `dns://`; changing servers invalidates the cache. One worker per window owns
   remote loading; synchronized queues/events and navigation IDs keep widgets
   on the UI thread and discard stale results. Stop, navigation and close cancel
   work; failures preserve the displayed document.
4. Extract URL and HTTP/1.1 loading from rendering. Support explicit ports,
   relative redirects, Content-Length, chunked and close-delimited bodies;
   reject malformed or contradictory framing and incomplete responses. Use
   `Host`, `Connection: close`, `Accept-Encoding: identity`, a 32 KiB decoded
   body cap, 16 KiB header cap and five redirects. Reject remote-to-file and
   HTTPS-to-HTTP redirects. Budgets: DNS two 2-second attempts, connect 5 seconds,
   TLS 10, read idle 10 and navigation total 30, all cancelable and monotonic.
5. Add a system CSPRNG seeded from real entropy (virtio-rng for QEMU), SUN random
   bytes and civil-clock validity. Pin and audit native shared OpenSSL and the
   CA store in the provenance registry before enabling HTTPS. TLS 1.2/1.3 must
   validate chain, hostname/IP and dates, send SNI and offer only HTTP/1.1.
   Fail explicitly for missing entropy or clock validity; never offer a
   certificate bypass. Distinguish trust, name, date, clock, entropy and
   transport errors. Test CAs belong only in test images.

### Network acceptance gates

| Stage | Completion gate |
|---|---|
| N1 — Native contracts and application migration | Existing local-file, argument, DNS-option, history and link workflows run as `/system/webview.sxe`; the migrated application makes no POSIX calls. |
| N2 — Transport progress and correctness | Pending transmissions progress without their consumer reentering the stack; losses, simultaneous operations, cancellation and close cause neither hangs nor leaks. |
| N3 — Shared DNS and worker | Slow DNS or stalled downloads leave repainting, movement, input and cancellation responsive; stale results are discarded. |
| N4 — Shared HTTP | Correct responses render independently of header, chunk and TCP fragmentation; contradictory framing and truncated bodies fail explicitly. |
| N5 — Validated HTTPS | Valid certificates work; expired, self-signed, wrong-name and invalid-chain certificates are rejected, without a bypass. |

Acceptance includes `nativetest` checks for sockets, readiness, pending
connections, errors, close, waits, clocks and entropy. DNS/HTTP protocol tests
must cover fragmented, truncated and malformed inputs, limits and bounds safety.
Local QEMU DNS/HTTP/TLS fixtures must cover timeout, cancellation, redirects,
chunked bodies, invalid certificates and obsolete responses after navigation.
Extend `tcp-smoke` for progress without consumer polling, lost FIN, simultaneous
connections and close during operations, while retaining `net-smoke`.

Extend Web Viewer's visual scenario and run `./build.sh smoke sxgui-smoke` when
touching a drawing program, the loader or SxGUI. Verify that a successful build
installed the rebuilt binary before trusting visual results. Run
`./tools/verify_doom_persistence.sh` for kernel/build/SDK/image-flow changes;
normal builds must preserve external `/bin` binaries and `/games` assets.

DHCP, IPv6, TCP servers, HTTP/2–3, WebSockets, persistent cookies,
authentication, compression and HTTP caching are outside this delivery.
Renderer replacement and its sandbox remain separate later milestones.
The network delivery is complete only when the SUN Web Viewer retains its
existing workflows and browses HTTP/validated HTTPS with cancellation and a
responsive UI.

## N4 implementation checkpoint — 2026-11-21

**N4 is complete.** URL handling and HTTP/1.1 left Web Viewer for a SUN
library, `include/sxn/sxnhttp.h`, that the worker calls. Its prerequisite,
reclaiming `TIME_WAIT` slots when the socket table is full, landed first.

- Requests carry `Host`, `Accept-Encoding: identity` and `Connection: close`.
  Content-Length, chunked and close-delimited bodies are read from any
  fragmentation; contradictory framing, malformed heads, truncated bodies,
  heads past 16 KiB and non-identity encodings fail, each with its own error.
  The body cap is the caller's buffer (32 KiB in Web Viewer), kept and marked
  as cut rather than failed.
- Up to five redirects, resolved by RFC 3986; never to `file://`, never from
  https down to http, `https://` refused until N5.
- Budgets are monotonic and cancellable: connect 5 s, idle 10 s, total 30 s.
  TLS's 10 s belong to N5.
- Contracts are in [HTTP.md](../../net/HTTP.md) and
  [WEBVIEWER.md](../../desktop/WEBVIEWER.md#loading-in-the-background-n3).

### N4 acceptance evidence

| Gate | Evidence |
|---|---|
| Correct responses render independently of header, chunk and TCP fragmentation | `sxnhttp-message-test` feeds every response whole, split at every byte and byte by byte; `native-network` loads each framing from `tools/http_fixture.py`, which sends every response in small pieces; the webview scenario renders a chunked page sent 11 bytes at a time. |
| Contradictory framing and truncated bodies fail explicitly | The host test covers each framing error and truncation at every cut of a good response (two parser mutations each make it fail); `native-network` gets `BAD_FRAMING`, `TRUNCATED` (close and reset), `BAD_RESPONSE`, `HEADERS_TOO_LARGE` and `UNSUPPORTED_ENCODING` from the fixture; the webview scenario shows the message and keeps the page. |
| Redirects, timeout, cancellation | `native-network`: a relative redirect, five allowed and the sixth refused, file and https targets refused, the total and idle budgets, cancellation from a thread mid-body. |

### Next implementation step — N5

A system CSPRNG with real entropy, civil-clock validity, and an audited
shared OpenSSL with its CA store, before `sxnhttp` accepts `https://`: see
step 5 above. In order: N5a, the CSPRNG; N5b, civil-clock validity; N5c, the
OpenSSL 3.5.3 port (the version the pinned Ladybird's `vcpkg.json` overrides
to) and its CA store; N5d, TLS in `sxnhttp`, its fixtures and Web Viewer's
indicator.

**N5a landed 2026-11-21:** the kernel CSPRNG ([RANDOM.md](../../kernel/RANDOM.md)),
seeded only from virtio-rng or RDSEED and refusing while unseeded, with SUN,
POSIX and Linux interfaces.

**N5b landed 2026-11-21:** clock validity (`sxn_clock_status`,
`clock_status`; [TIME.md](../../kernel/TIME.md#can-the-date-be-believed)):
whether the date can be believed, its source, and why not.

## N3 implementation checkpoint — 2026-11-21

**N3 is complete.** Slow DNS and stalled downloads no longer hold the window:
remote loading runs on one network worker per window, with Stop, cancellation
and stale results discarded, over the shared SUN resolver. The next stage is
N4; the TIME_WAIT reclamation planned before it has landed (2026-11-21,
[NETWORKING.md](../../net/NETWORKING.md#closing-keeps-protocol-state-alive)).

### The worker

- `sxngui_app_wake` and the `on_wake` hook (`include/sxn/sxngui.h`) let any
  thread wake the SUN frame; the hook runs on the UI thread. POSIX's
  `sxgui_app.c` has no counterpart yet, recorded as pending in
  [PLATFORM_GRAPHICS.md](../../desktop/PLATFORM_GRAPHICS.md).
- Web Viewer's worker owns the resolver, the sockets and its own body buffer.
  Request, progress and result cross in single-slot mailboxes under a futex
  lock; navigation IDs drop anything obsolete. A new navigation, Back/Forward,
  Reload, Stop (the Go button, Esc, Go > Stop) and window close set the
  worker's cancel event, checked first in every wait. Failed and stopped loads
  keep the displayed document; history moves apply only when a page arrives.
- Contracts are in [WEBVIEWER.md](../../desktop/WEBVIEWER.md#loading-in-the-background-n3).

### N3 acceptance evidence

| Gate | Evidence |
|---|---|
| Slow DNS and stalled downloads leave the window responsive | `./tools/shoot.sh --scenario webview`: during `/stall` (a download that never ends) the status follows the worker, Go reads `Stop`, and Ctrl+U toggles the source of the page on screen; Esc stops and keeps the page. |
| Cancellation | Stop during resolution of a name the fixture never answers returns `Stopped.` at once; closing the window mid-load ends the process. |
| Stale results discarded | A typed navigation replaces a stalled one and only the newer page appears. |
| Resolver | `sxndns-message-test` on the host; `native-network` against `tools/dns_fixture.py`. |

### The shared resolver

- `include/sxn/sxndns.h` (`system/runtime/sxndns.c`, in every native
  program's runtime): numeric IPv4, A/CNAME with chains in one reply and
  across up to four further questions, a 16-entry cache bounded by TTL and
  five minutes, replies believed only from the server's address and port for
  the TXID and question asked, a TCP retry for truncated replies assembled
  from any number of reads, and a manual-reset cancel event checked first in
  every wait. Budgets: six seconds for the route, two attempts of two seconds.
- A resolver is caller-owned and single-threaded; changing its server empties
  its cache. Web Viewer keeps one per window and keeps `--dns` and `dns://`.
- The wire format is a pure module with its own host test
  (`sxndns-message-test`); `native-network` checks the resolver against
  `tools/dns_fixture.py`, which the echo server starts on one port number for
  UDP and TCP.

Contracts and limits are in
[NETWORKING.md](../../net/NETWORKING.md#name-resolution-sxndns). POSIX has no
resolver yet; that gap is recorded there as pending.

### Next implementation step — N4

Extract URL and HTTP/1.1 loading from Web Viewer into a reusable SUN library
that the worker calls, with the framing, redirect, limit and budget rules of
step 4 above, including the 30-second total navigation budget. Its
prerequisite, reclaiming a `TIME_WAIT` slot when the socket table is full, has
landed.

## N2 implementation checkpoint — 2026-11-16

**N2 is complete.** The shared TCP core has independent progress, bounded NIC
servicing, retained asynchronous errors and detached close state. N1 remains
complete; N3–N5 and Ladybird remain pending. The next implementation stage is N3.

- The BSP heartbeat services routes, RX, queued TX, retransmissions and close
  before waking readers/waiters, even when no consumer reenters the stack.
  Monotonic elapsed time supplies deadlines. The existing big kernel lock
  serializes SUN/POSIX calls, timer entries and NIC IRQs; callbacks do not sleep.
- RX handles at most 16 frames per pass; TX drains at most 16 before and after
  RX through a bounded 64-frame queue. Drivers submit DMA without waiting for
  completion. Queued TCP frames carry an owner token for cancellation before
  socket reuse. The single outstanding 1 KiB TCP segment is retained.
- Final release drains accepted stream data before queuing FIN. FIN retries,
  active/passive/crossed close, 60-second TIME_WAIT with duplicate-FIN replies,
  and bounded orphan expiry retain protocol state independently of handles.
  Attached and detached sockets share the 32-slot budget. The read-only
  `NET_IOC_GET_TCP_LIFETIMES` query observes reclamation without driving it.
- Retry/ACK failures remain available through status, readiness and I/O. ACKs
  beyond sent sequence space cannot clear pending TX; blocking writes check
  transport failure after the pending slot is disarmed. Failed I/O remains
  ready; POSIX `poll` reports `POLLERR` even without requested interests.

The maintained contracts and limits are in
[NETWORKING.md](../../net/NETWORKING.md#independent-bounded-and-synchronized-progress)
and [SUN.md](../../userland/SUN.md#ipv4-socket-foundation).

### N2 acceptance evidence

| Gate | Evidence |
|---|---|
| Build and host tests | Build succeeded with `check_shared_libs: OK`; `100% tests passed out of 54`, with three Win32 fixture tests skipped because `SAVANXP_WIN32_FIXTURES` is unset. |
| Independent progress and close | `tcp-smoke`: simultaneous unpolled writes retry; closing preserves the exact bytes saved by the peer; dropped FIN retries; exhausted FIN and a half-open peer reclaim state; TIME_WAIT reserves the port and expires after its real interval. |
| Native concurrency and errors | `native-network`: distinct threaded streams under loss, independent SYN retries and accepted-data timeout, cancellation/close during setup and a retained stream wait; existing refusal/reset/route/EOF assertions pass. |
| General regression | `native-network-offline`, `native-test`, `net-smoke`, `smoke` and `sxgui-smoke` passed. |
| Additional driver | `native-network`, `tcp-smoke` and `net-smoke` also passed with `--virtio`. |
| Visual regression | `webview: OK`, with 25 captures; local files, history, links, CSS, DNS/HTTP, redirects, source, arguments and Reload. |
| Persistence | `./tools/verify_doom_persistence.sh --jobs 8`: `Doom persistence: PASS`, using the deterministic WAD fixture. |

The tests run under QEMU/TCG with two CPUs. The visual check's extracted SUN
executable matches the staged binary by SHA-256 and has `EI_OSABI=0x53`; both
retired POSIX Web Viewer copies are absent. Physical RTL8168 hardware was not
available for verification.

### Next implementation step — N3

Extract the shared SUN resolver and move remote loading to one worker per
window. Preserve DNS overrides, use synchronized queues/events and navigation
IDs, and make Stop, navigation and close cancel work while the UI remains
responsive. Loading still runs synchronously in Web Viewer; N2 does not claim
background loading, shared DNS/HTTP, HTTPS or a new renderer.

**Carried limit: TIME_WAIT shares the 32 socket slots.** Rapid active closes
can fill the table for up to 60 seconds, and a new socket then fails with
`ENOMEM`. Before N4 opens many short HTTP connections, a full table should
reclaim a `TIME_WAIT` slot, the one closest to expiry, instead of refusing,
with a `tcp-smoke` assertion that opens more sockets than the table holds.
See [NETWORKING.md](../../net/NETWORKING.md#closing-keeps-protocol-state-alive).

## N1 implementation checkpoint — 2026-11-15

**N1 is complete.** This records N1's acceptance; the N2 checkpoint above
supersedes its next-step list. The remaining setup/route contract and the application
migration were delivered as separate changes. The historical checkpoints below
record earlier partial states; this checkpoint supersedes their next-step lists.

| Stage | Current state | Remaining work |
|---|---|---|
| N1 | Complete: cancelable native setup/ARP waits, retained transport errors, and `/system/webview.sxe` with its existing workflows. | None for this gate. |
| N2 | Pending; progress still depends on consumers or parked waiters. | Independent bounded and synchronized servicing, complete asynchronous error handling, FIN retransmission and TIME_WAIT. |
| N3 | Pending; native DNS and loading still run synchronously in Web Viewer. | Shared resolver/cache/validation/TCP fallback, worker, cancellation, Stop and stale-result handling. |
| N4 | Pending; URL/HTTP handling remains in the application. | Reusable result contract, strict framing, redirects, limits, deadlines and protocol tests. |
| N5 | Pending; HTTPS is not enabled. | Entropy and valid civil clock, audited shared OpenSSL, CA store, certificate validation and TLS fixtures. |

### Delivered for N1

- Native sockets advance NIC reset/link negotiation and ARP without sleeping
  inside socket operations. Write/error readiness carries route completion;
  events cancel waits and last close releases pending route state. The shared
  TCP pump also avoids blocking on a POSIX socket's ARP. The six-second route
  budget and UDP retry contract are maintained in
  [SUN.md](../../userland/SUN.md#ipv4-socket-foundation).
- `native-network` checks pending setup/ARP, cancellation, route timeout,
  pending read/write, invalid user buffers, TCP success/refusal/reset/retry
  timeout, partial reads, EOF and handle lifetime. `native-network-offline`
  checks retained `ENODEV` through native status, readiness and I/O.
- Web Viewer uses SUN files, sockets, waits, clock and the `sxngui_app` frame,
  with compiled resources and no POSIX calls in its application paths. Its
  interface, arguments, local files/URLs, DNS overrides, HTTP, history, links,
  redirects, Reload, CSS and View Source remain covered by the visual scenario.
  Only the old Web Viewer executables are retired by this migration.

### Acceptance evidence

| Command | Observed result |
|---|---|
| `./build.sh build` | Passed; `check_shared_libs: OK`, valid persistent image synced. |
| `./build.sh test` | `100% tests passed out of 53`; three Win32 fixture tests skipped because `SAVANXP_WIN32_FIXTURES` is unset. |
| `./build.sh smoke native-network` (RTL8139 and `--virtio`) | `NATIVE NETWORK PASS`. |
| `./build.sh smoke native-network-offline` | `NATIVE NETWORK OFFLINE PASS`. |
| `./build.sh smoke native-test`, `net-smoke`, `tcp-smoke`, `smoke` | Their respective `PASS` tokens. |
| `./build.sh smoke windowd-smoke`, `taskbar-smoke`, `sxgui-smoke` | Their respective `PASS` tokens, including editor glyph/scroll and taskbar checks. |
| `./tools/shoot.sh --scenario webview` | `webview: OK`; existing flows plus native argument/DNS, Reload and file-URL checks. |
| `./tools/verify_doom_persistence.sh --jobs 8` | `Doom persistence: PASS`, with the deterministic WAD fixture. |

The visual check's extracted executable matches the built SUN image by SHA-256
and carries `EI_OSABI=0x53`, with both old `/bin` copies absent. Network tests
also run with `native-network --virtio`; no physical RTL8168 hardware was
available for verification.

### Next implementation step — N2

Give the shared TCP core bounded independent and synchronized servicing, so
pending traffic progresses even without a consumer calling or waiting in the
stack. Extend `tcp-smoke` for progress without consumer polling, simultaneous
connections, close during operations and lost FIN; implement FIN retransmission
and TIME_WAIT. Keep POSIX on that same core and initially retain one outstanding
segment. Do not mark N2 complete from N1's waiter-driven network checks.

## Historical checkpoint — 2026-11-09

**Only the socket foundation portion of N1 is implemented. No network stage is
complete, and Web Viewer itself has not been migrated or changed by this slice.**
This checkpoint records development work; it does not identify a release or
claim that the complete network delivery is available.

| Stage | Current state | Remaining work |
|---|---|---|
| N1 | Partial: SUN socket handles and interest-based waits exist; success, refusal and retry timeout are tested. | Finish asynchronous initialization/ARP and the remaining transport contract, then migrate Web Viewer separately and verify all existing workflows. |
| N2 | Pending; POSIX continues using the existing shared network core. | Independent bounded and synchronized servicing, complete asynchronous error handling, FIN retransmission and TIME_WAIT. |
| N3 | Pending; Web Viewer still uses its own synchronous DNS/loading code. | Shared resolver, TTL cache, sender validation, TCP fallback, worker, cancellation, Stop and stale-result handling. |
| N4 | Pending; the existing application still owns URL/HTTP handling. | Reusable loading result contract, URL/HTTP extraction, strict framing, redirects, limits, deadlines and protocol tests. |
| N5 | Pending; HTTPS is not enabled. | CSPRNG/virtio-rng, SUN entropy and valid civil clock, audited shared OpenSSL, versioned CA store, certificate validation and TLS fixtures. |

### Implemented foundation

- `sxn_socket_create`, `sxn_socket_bind`, `sxn_socket_connect`,
  `sxn_socket_status`, `sxn_socket_sendto` and `sxn_socket_recvfrom` call the
  network core directly, without POSIX process state. TCP streams use SUN
  read/write, including partial operations; duplicate/close use native handles.
- Connection initiation can return `-EAGAIN` while the SYN exchange is pending.
  Status distinguishes pending, established and retained transport failure;
  callers must query it after readiness rather than assuming success.
- `sxn_wait_io` combines read/write/error interests with waitable objects such
  as a cancellation event. Existing `sxn_wait_many` semantics are unchanged.
  The new wait retains object references through completion, including when
  another thread closes a handle. Socket calls also hold a reference across
  network-core operations that may sleep.
- `nativetest network` and `smoke native-network` exercise UDP loopback/source
  addresses, duplicate/close, timeouts, event cancellation, closing a handle
  during a wait, TCP establishment, fragmented reads and orderly EOF.

The maintained API contract and limits are in
[SUN.md](../../userland/SUN.md#ipv4-socket-foundation). Implementation entry
points are the [native ABI](../../../include/abi/savanxp_native_abi.h),
[SUN wrappers](../../../include/sxn/sxn.h),
[native dispatcher](../../../kernel/native_dispatch.cpp),
[wait core](../../../kernel/sx.cpp),
[network core](../../../kernel/net.cpp) and
[nativetest](../../../sdk/nativetest/main.c).

### Known limits and next implementation step

NIC initialization and ARP resolution may still wait inside the network core:
these APIs do not yet provide fully asynchronous transport. Retransmission
progress still depends on network servicing, including servicing for parked
waiters. Independent periodic progress, bounded concurrent servicing, FIN
retransmission and TIME_WAIT have not been delivered. Pending-connect success,
refusal and retry timeout are covered by the intermediate step below; this is
not complete coverage of every transport error or concurrent operation.

Resume at N1's remaining transport contract: make initialization/address
resolution cancelable without blocking the window's worker protocol and cover
the remaining transport/I/O failure paths with `nativetest` assertions. Then
migrate Web Viewer in a separate change, preserving its visible identity,
arguments, local files, history, links and DNS overrides. Do not mark N1 done
based solely on the socket smoke or retire the current executable early.

### Verification recorded for this foundation

The implementation session completed these checks successfully:

| Command | Result and coverage |
|---|---|
| `./build.sh build` | Passed; the existing valid persistent image was synced. |
| `./build.sh smoke native-network` | Passed with the local TCP fixture, including cancellation and concurrent handle close. |
| `./build.sh smoke native-test` | Passed the existing native ABI regression suite. |
| `./build.sh smoke net-smoke` | Passed the existing network regression scenario. |
| `./build.sh smoke tcp-smoke` | Passed the existing POSIX TCP loss/reordering scenario. |
| `./tools/verify_doom_persistence.sh --jobs 8` | Passed the isolated external executable and persistent-asset check. |
| `git diff --check` | Passed. |

These checks do not establish independent TCP progress, FIN/TIME_WAIT behavior,
a responsive browser, shared DNS/HTTP correctness or TLS validation. Browser
visual checks, `sxgui-smoke`, the expanded transport scenarios and DNS/HTTP/TLS
fixtures remain acceptance work for their corresponding changes. No browser or
renderer binary was changed in this slice.

### Intermediate step — pending TCP connection contract

**Completed and verified on 2026-11-09. Work stops at this intermediate gate;
N1 remains partial.**

This step is limited to completion of a pending connection: success, refusal
and timeout. It does not migrate Web Viewer or add independent TCP progress.

- A completed handshake reports zero through `sxn_socket_status`; a pending
  handshake reports `-EAGAIN`. Starting another connect while pending or
  established returns `-EBUSY`.
- A rejected handshake reports `-ECONNREFUSED`, distinct from the
  `-ECONNRESET` of an established connection reset. Exhausting the SYN retry
  budget reports `-ETIMEDOUT`. Failures wake write/error interests and remain
  available through repeated status queries and I/O; readiness alone is never
  evidence of a successful connection.
- Expiring `sxn_wait_io` leaves the socket pending. A deliberate new connect
  attempt replaces a retained failure. The existing blocking `connect` also
  stops retransmitting after its own monotonic deadline expires.
- Pending handshakes only accept a SYN-ACK or reset acknowledging the SYN;
  unrelated ACKs no longer disarm its retransmission slot.

`smoke native-network` tests the SUN success path, error-only polling on an
established connection, a host port reserved without a listener for refusal,
and the normal retry budget with every outgoing SYN dropped by the existing
fault injector. It checks wait expiry versus socket timeout, repeated status,
read/write errors, write/error readiness and a new attempt after timeout. The
fault injector is restored on exit. `tcp-smoke` additionally checks that a
short blocking-connect timeout leaves no SYN retransmission after servicing
the stack beyond the initial RTO.

The host fixture's `REFUSE` command returns a reserved, non-listening TCP port
and keeps it reserved for the lifetime of the control connection. Its host
protocol test checks that this port refuses connections, along with the
existing echo/bulk/delay commands.

Verification for this intermediate step:

| Command | Result |
|---|---|
| `./build.sh build` | Passed. |
| `python3 tests/host/tcp_echo_server_test.py --server tools/tcp_echo_server.py` | Passed, including the refusal-port fixture. |
| `./build.sh smoke native-network` | Passed, including success, refusal, wait expiry and SYN retry timeout. |
| `./build.sh smoke native-test` | Passed. |
| `./build.sh smoke net-smoke` | Passed. |
| `./build.sh smoke tcp-smoke` | Passed, including blocking-connect timeout cleanup. |
| `./build.sh smoke sxgui-smoke` | Passed the editor glyph/scroll and taskbar visual checks. |
| `./tools/verify_doom_persistence.sh --jobs 8` | Passed. |
| `git diff --check` | Passed. |

The remaining transport work listed above is not part of this completed step.
No asynchronous ARP, independent TCP servicing, Web Viewer migration or browser
loading changes have been started by this step.

## Upstream baseline and constraints

The fixed upstream baseline is
[`37ec6a08a8aaff5808e5b570430001594692fde3`](https://github.com/LadybirdBrowser/ladybird/commit/37ec6a08a8aaff5808e5b570430001594692fde3),
selected to stay on the C++ codebase before the Rust transition. Do not track
`master` or advance this baseline automatically. No source has been imported;
W0 must reproduce this exact revision and audit its complete dependency closure.

- [README](https://github.com/LadybirdBrowser/ladybird/blob/37ec6a08a8aaff5808e5b570430001594692fde3/README.md):
  pre-alpha browser with LibWeb and LibJS, under a top-level BSD-2-Clause
  license. Dependencies and assets still require individual review.
- [Build instructions](https://github.com/LadybirdBrowser/ladybird/blob/37ec6a08a8aaff5808e5b570430001594692fde3/Documentation/BuildInstructionsLadybird.md):
  C++23, CMake 3.30+, Qt6, nasm and additional host tools; CI uses GCC 14 and
  Clang 21. These instructions do not list Rust as a prerequisite. A native
  SUN frontend is SavanXP work; no stable embedding API is assumed. Host Qt
  requirements do not imply that Qt must run on SavanXP.
- The pinned README describes UI, WebContent, ImageDecoder and RequestServer
  processes. Audit this revision's actual process/IPC graph; do not import
  assumptions about Hub or a separate Compositor from later upstream designs.
  Process launch, shared buffers and isolation remain platform audit areas.
- [Dependency manifest](https://github.com/LadybirdBrowser/ladybird/blob/37ec6a08a8aaff5808e5b570430001594692fde3/vcpkg.json):
  includes Skia, font/shaping libraries, ICU, curl, OpenSSL and media codecs.
  Reproduce its vcpkg baseline and overrides as well as Ladybird's commit.
  Reusing only LibWeb does not establish that these dependencies disappear.

Rust toolchain/runtime support is not a planned prerequisite for this baseline.
W0 still audits transitive dependencies and build generators before claiming
that the complete build is Rust-free.

This is a platform port with substantial runtime work, not a replacement HTML
parser. No completion date or memory budget is claimed before measurement.

## Later Ladybird engine stages and acceptance gates

### W0 — Reproduce and measure the pinned baseline

Reproduce the selected commit's host build and run local HTML/CSS/JavaScript
fixtures. Record tool versions, dependency closure, resource licenses, process
count, peak memory and rendering behavior. Separate build-time generators from
target libraries. Audit the selected source for OS and runtime calls.

**Done when:** a reproducible host baseline and a gap matrix identify each
required facility, its current SavanXP support, owner and verification gate.
Explicitly decide which optional features can be disabled at that commit;
unsupported dependencies remain blockers rather than silent stubs.

### W1 — Runtime and SUN platform foundation

Use the gap matrix to sequence C++ runtime and ABI support, allocation,
threading and synchronization, virtual memory, timers/event waiting, files,
process lifecycle, IPC, shared memory and handle transfer. These are audit
areas, not claims that every facility is absent. Keep platform adaptation
separate from pristine upstream code, with a reproducible patch series.

**Done when:** minimal target probes exercise every facility required by the
first renderer, and each added SUN contract has a `nativetest` assertion.
Host generators run on the host; target binaries contain no host paths.

### W2 — Local rendering without a desktop frontend

Cross-build the smallest viable rendering configuration and render controlled
local pages to images on SavanXP. Establish fonts, resource discovery, shaping,
image decoding and a viable raster backend. Investigate software rasterization
first; do not assume SxGL satisfies Skia's GPU requirements. Keep LibJS in the
plan and verify DOM changes driven by a local script.

**Done when:** target-produced images pass text, CSS, image and scripted-DOM
fixtures against the pinned host baseline, with time and memory measurements.
Any temporary reduction of process isolation is local-test-only and documented.

### W3 — Native Web Viewer frontend

Adapt the existing SUN/SxGUI chrome for address entry, navigation, status and errors; connect
rendered surfaces, resize, scrolling, mouse, keyboard and focus through the
selected engine interfaces. Keep page execution out of the UI process.

**Done when:** the native application renders and navigates the local corpus,
handles renderer failure visibly, and passes input and screenshot checks.
Run `./build.sh smoke sxgui-smoke` as well as browser-specific visual checks;
verify that the rebuilt application is the binary installed and exercised.

### W4 — Network services and security

Adapt engine requests to the shared SUN loading infrastructure delivered above.
Do not assume compatibility with upstream RequestServer interfaces.
Audit entropy, clock accuracy, trust-store provenance, certificate validation,
redirects, cancellation, cookies and origin boundaries. Implement and test
renderer/helper confinement before enabling arbitrary remote content in the
Ladybird engine. This is separate from validated HTTPS in the current renderer.

**Done when:** controlled HTTP/TLS fixtures cover success, invalid certificates,
redirects and failures; UI remains responsive; a compromised renderer cannot
read arbitrary files or open arbitrary network connections. Missing security
primitives block general browsing rather than being bypassed.

### W5 — Replacement and ongoing compatibility

Switch Web Viewer to the integrated engine only after it covers the existing
SUN application's local-file, HTTP/HTTPS, history and link workflows. Preserve
the application identity and retire the old renderer in an explicit change. Add a selected upstream conformance corpus,
crash/restart and resource-pressure checks; measure budgets in documented QEMU
configurations. Tabs, downloads, media, WebGL and broader web compatibility
receive separate gates after the first usable browser.

**Done when:** native installation and updates preserve external `/bin` programs
and `/games` assets, the normal build succeeds, and
`./tools/verify_doom_persistence.sh` passes for build/SDK/image-flow changes.
No browser work may recreate a valid disk image as a normal build step.

## Adoption and maintenance

The intended category is `Adopt`, subject to W0's component/license audit.
Before importing code, record exact revisions, notices, patches, dependency and
asset licenses, and installed outputs in the
[provenance registry](../../third-party/THIRD_PARTY_PROVENANCE.md).
Use `vendor/ladybird/` for pristine pinned sources and separate platform glue;
maintain this C++ baseline with explicitly reviewed fixes. Do not follow the
Rust transition. Evaluate any backport against the same corpus and resource
measurements, and record its origin and dependency changes separately from the
unchanged upstream pin. Security fixes remain necessary for a frozen baseline.
Discover additional host tools through CMake and document their requirements.
