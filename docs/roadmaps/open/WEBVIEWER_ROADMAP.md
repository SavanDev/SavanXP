# Web Viewer with Ladybird

> **Status as of 2026-11-09: network foundation partially implemented; pending-connect contract tested.**
> Web Viewer is still POSIX and its remote loads still block the UI.
> Ladybird feasibility remains unproven on SavanXP; no Ladybird code has been
> imported or built. The network delivery below precedes engine replacement.

## Direction

Develop Web Viewer as a native SUN browser using Ladybird's engine and a
SavanXP frontend. Retire the NetSurf plan in full: no NetSurf library adoption,
reference-browser port, or separate in-house CSS layout engine is planned.
The current `/bin/webview` prototype remains available with the capabilities
recorded in [WEBVIEWER.md](../../desktop/WEBVIEWER.md).

The intended native application is `/system/webview.sxe`, with SUN helpers.
Migrating the existing POSIX application to SUN is a separate change with its
own changelog entry and behavior checks. New platform gaps are delivered as
SUN slices with `nativetest` assertions, following [SUN.md](../../userland/SUN.md).
Running the upstream browser through a compatibility subsystem is not the
native browser milestone.

## Target architecture

Keep Web Viewer's current chrome, history, local-file support and renderer while
migrating the application to SUN. A reusable SUN loading library separates URL
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

## Implementation checkpoint — 2026-11-09

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
