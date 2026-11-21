# Networking: what the TCP path guarantees

The kernel stack is small on purpose, and the small parts still have to be
correct. This document covers the TCP client path in `kernel/net.cpp`: what it
guarantees today, the design decisions behind those guarantees, and — as
explicitly — what it still does not do. ARP, ICMP and UDP are simple enough to
read directly from the source; TCP is not.

## The shape of the stack

```
app  ──►  socket syscalls  ──►  net::  ──►  nic::  ──►  rtl8139 / rtl8168 / virtio-net
                                 │
                                 ├─ ARP cache (16 entries, 60 s lifetime)
                                 ├─ ICMP echo (/dev/net0 NET_IOC_PING)
                                 ├─ UDP  (datagram queue per socket)
                                 └─ TCP  (client only — see below)
```

The BSP's timer services the stack independently of socket consumers. NIC
interrupts may also receive frames, and blocking calls can request a pass.
Every entry uses the kernel's existing global lock; the network core is shared
by SUN and POSIX rather than duplicated behind their syscall surfaces.

## What TCP guarantees

Given a peer that behaves like a normal TCP implementation, a connected socket
delivers **the bytes that were sent, in the order they were sent, or an error**.
It gets there over a network that loses and reorders segments. Concretely:

- **A lost segment is resent.** Every segment that consumes sequence space —
  data, `SYN`, `FIN` — is kept until the peer acknowledges it. On timeout it
  goes out again with exponential backoff: 300 ms initial RTO, doubling to a
  4 s ceiling, 6 retransmissions, ~17 s total before the connection is declared dead.
- **Segments that arrive out of order are reassembled.** Data that lands past
  the expected sequence number is stored at its correct position and held until
  the gap is filled, at which point everything contiguous becomes readable at
  once.
- **Duplicated and partially duplicated segments are trimmed**, not discarded:
  a retransmission that carries some already-seen bytes and some new ones
  contributes the new ones.
- **Sequence numbers wrap correctly.** All comparisons use serial arithmetic
  (RFC 1982). At 1 KiB per segment the 32-bit counter wraps after 4 GiB
  transferred — a large download, not an impossibility.
- **A connection that dies is an error, not an end of file.** A peer `RST` or an
  exhausted retransmission budget makes subsequent reads return `ECONNRESET` or
  `ETIMEDOUT`. Buffered bytes that arrived before the failure are still
  delivered first.
- **The advertised window is the real free space**, and it reopens: when the
  application falls behind and the window shrinks below one segment, the next
  read that frees a full MSS sends a window update.

## Design decisions

### One segment in flight, buffered

The send side is stop-and-wait: one unacknowledged segment per socket, its
bytes kept in a 1 KiB buffer inside the socket so it can be resent.

The alternative — a real sliding window with a send ring — is faster and was
deliberately deferred. What makes stop-and-wait acceptable is where this stack
is used: a LAN or a host bridge, where the round trip is well under a
millisecond, which puts the ceiling at a few MB/s. What makes it *correct* is
that the copy exists at all. The previous code sent each segment once and
advanced `send_next` immediately; a single lost segment left the stream
permanently desynchronised while the socket still reported itself connected.
Throughput was never the bug.

A visible consequence: `POLLOUT` is false while a segment is unacknowledged,
and a non-blocking `write()` returns `EAGAIN` there. That is the honest report
of a one-slot send window. In exchange, a non-blocking `write()` that returns
success now means the data is durable — it will be resent if lost — which was
not true before.

### Reassembly by extents, not by segment queues

Out-of-order data is written into the receive ring at the offset its sequence
number dictates, and only its *extent* — a `{start, end}` pair — is recorded.
Four slots per socket, coalescing on insert.

Queuing whole out-of-order segments would need a buffer per segment; at 32
sockets that is memory the kernel does not have to spare. Extents cost 32 bytes
per socket and handle the common case exactly: a single loss produces a single
hole, and every segment that arrives after it merges into one extent. When the
slots fill, the data is dropped and the peer retransmits it — degraded, never
wrong.

Writing out-of-order bytes into the ring before the gap is filled is safe
because the ring position is derived from the sequence number: a byte can only
ever be written at the one place its sequence maps to, so a retransmission
overwrites identical bytes.

### Independent, bounded and synchronized progress

The BSP timer calls `net::tick()` before waking readers and readiness waiters,
regardless of whether any thread is waiting on a socket. A nonblocking caller
can sleep or compute after accepting a write; SYN, data, routes and close
continue to progress. Deadlines use the monotonic clock. POSIX blocking calls
can still ask for a pass to reduce latency.

Each pass receives at most 16 frames, drains at most 16 TX frames before and
after RX, releases at most one fault-injection stash and scans 32 socket slots.
No receive/transmit callback waits for hardware completion. The NIC dispatcher
copies accepted frames into a 64-entry queue (1600 bytes each); a busy driver
keeps the head queued, with a 100 ms deadline rather than a wait under the
kernel lock. A full queue rejects a new transmission. Lost frames or hardware
completion failures are recovered by TCP's retained segment and retry budget;
UDP acceptance does not promise delivery. TX frame counters count submissions.
RTL8139 uses its four DMA slots, RTL8168 its descriptor ring and virtio-net one
outstanding DMA buffer, always retaining ownership until hardware returns it.

Syscalls, the timer and external IRQs already hold the big kernel lock (see
[SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md)). Network callbacks never sleep
or drop it. This serializes socket slots, ARP, fault injection, RX and TX across
cores and prevents IRQ reentry. Blocking wrappers release that lock only while
waiting for an interrupt, retain their I/O objects across the wait and recheck
transport state afterwards. A per-pump recursion guard only prevents duplicate
work; it is not the lock. Splitting the global lock in the future must preserve
these ownership rules. Boot initialization runs before competing entries exist.
Queued TCP frames carry a socket owner token: failure or cancellation removes
frames not yet submitted to hardware before the slot can be reused. An already
submitted frame cannot be recalled.

### Receive interrupts and the tick

The tick's pass serves the protocol timers every time, but not always the
NIC. A driver whose interrupt is routed says so through `Nic::interrupts`,
with the number it has delivered (RTL8139 and RTL8168 today; virtio-net has
none and is polled every tick). For such a NIC, `nic::poll_from_tick()`
retries a transmit queue the ring refused, and otherwise reads the device
only every 64 ticks, as a safety net: the interrupt handler drains receive.
Syscalls (`net::poll()`, the blocking wrappers) still poll the device every
time.

The safety net also checks the interrupt. When two checks in a row find
frames with no interrupt delivered since the previous one, the interrupt is
not arriving: the boot log says `nic: <driver> receives frames its interrupt
does not announce; polling it every tick`, and the tick polls every time from
then on. One such check alone proves nothing, because the tick runs with
interrupts off and a pending one has not been taken yet.

Why: reading the RTL8139's status registers costs two or three port reads,
and under KVM each leaves the VM. That was 37 µs of every tick, a third of
the tick's hold of the kernel lock on an idle desktop
([SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md), slice 5.1).

### Closing keeps protocol state alive

The last handle/reference release cancels an unestablished connection or UDP
route immediately. For an established stream, it detaches the TCP state from
the application. Accepted data retains the single retransmission slot until
acknowledged; only then does FIN use that same slot and retry budget. Incoming
contiguous bytes are discarded after detachment to keep the window open for
the peer's FIN. Closing never waits in the caller.

Active close advances through `FIN_WAIT_1`, `FIN_WAIT_2`, and (for crossed FINs)
`CLOSING` into `TIME_WAIT`. Passive close uses `LAST_ACK` and releases state
after its FIN is acknowledged. `TIME_WAIT` retains the local port and tuple
for 60 seconds (twice the chosen 30-second MSL); duplicate FINs are acknowledged
and restart the interval. Old data and resets cannot tear it down. Unfinished
orphan closes, including `FIN_WAIT_2`, expire after 60 seconds; retransmission
exhaustion can end them earlier. The heartbeat reclaims every expired slot.

Attached and detached sockets share the existing 32-slot budget. When it is
full, a new socket takes the `TIME_WAIT` slot closest to expiry instead of
failing: that state only waits out stray segments of a finished connection,
and losing it early risks no more than a late duplicate meeting a new
connection on the same tuple, which a 30-second MSL on a LAN makes remote.
Closing states that still owe data or a FIN are never reclaimed, so a table
full of those still returns `ENOMEM`. An explicitly bound port stays busy until
its protocol state expires or is reclaimed. `tcp-smoke` opens and actively
closes 40 connections within one `TIME_WAIT` interval to prove it. The read-only `NET_IOC_GET_TCP_LIFETIMES` diagnostic reports
attached streams, closing streams, `TIME_WAIT` streams, FIN retransmissions and
orphan close timeouts, without servicing the stack. Existing TCP statistics
and fault-injection structures keep their ABI sizes.

Asynchronous route or transmit failures are retained on both initial sends and
retransmissions. An ACK outside sent sequence space cannot clear a pending
segment. Blocking writes check failure even if the pump has already disarmed
the transmit slot, and buffered receive bytes precede a retained error. Failed
sockets are immediately readable/writable because I/O returns the retained
error; POSIX `poll` also reports `POLLERR` with no requested interests.

## The socket contract

The guarantees above are about the protocol. What decides whether ported C code
compiles and runs is duller, and was wrong in three places until the contract
was fixed:

- **`SO_RCVTIMEO` / `SO_SNDTIMEO` govern the wait**, set through
  `setsockopt()`, and `0` means wait indefinitely as POSIX says. The receive
  timeout used to be a hardcoded 5 seconds inside the kernel, which is not a
  timeout but a trap: a reply that took longer failed on a perfectly healthy
  connection. A client that long-polls — asks, and the server holds the request
  while it works — is exactly the case that cannot live with that.
- **`recv()` and `send()` work on stream sockets.** They did not exist at all:
  the only way out of a stream was `write()`, and `recvfrom()` rejected
  anything that was not a datagram. Every piece of ported network code uses
  them.
- **`write()` of more than one segment does a short write** and returns what it
  took, instead of failing the whole call with `EINVAL`. That is the `write(2)`
  contract, and the previous behaviour broke any caller that handed over a
  buffer larger than 1 KiB.

### A blocking read parks the process

`read()` on a socket tries once without blocking and, if there is nothing,
parks the process in `State::blocked_read` — the same machinery pipes and the
tty already used — with `wake_tick` set from `SO_RCVTIMEO`. The timer tick
retries it and completes it with the data, with the end of the stream, or with
`ETIMEDOUT`.

This is what makes the indefinite default honest. The waiting loop inside
`net::` never leaves `State::running`, so every tick of the wait is charged to
the waiter: a program parked forever in a read would have shown 100% CPU
forever, which is the defect `poll()` had before
[its own fix](../userland/SYSTEM_MONITORING.md#waiting-is-not-running-what-the-first-measurement-found).
`tcp-smoke` measures it rather than assuming it — the process samples its own
`cpu_ticks` around a 7-second wait and requires its share to stay negligible
(one tick out of some 4700).

The one trap worth remembering when touching this path: the tick's retry runs
while **another** process is current, so it cannot copy into the reader with
`copy_to_user`. That is why the core of the read (`read_socket_bytes`) leaves
its bytes in kernel memory and the caller places them —
`write_to_process_memory` for the parked reader. Getting this wrong does not
fail loudly; it silently writes one process's data into another's buffer, and
the symptom is a stream that arrives with foreign bytes in the middle of it.

## Native setup and route waits

SUN sockets use the same core with asynchronous initialization and per-socket
route state. RTL8139 reset and RTL8168 reset, PHY reset and link negotiation
advance across calls; virtio-net queue setup completes in one bounded step.
Small bounded register transactions remain synchronous. POSIX's explicit
`NET_IOC_UP` retains the blocking driver entry over the same initialization
steps. TCP route preparation is shared: the pump never waits for ARP on behalf
of a POSIX socket while servicing a SUN waiter. SUN never waits for ARP inside a connect, send, status or close call.
Its six-second setup/ARP deadline, retry and cancellation contract is documented
in [SUN.md](../userland/SUN.md#ipv4-socket-foundation).
Route progress is serviced by the independent `net_pump` heartbeat as well as
explicit consumer calls.

## Testing what only a bad network exercises

None of the above is exercised by a network that works. QEMU's slirp does not
lose packets, and neither does a lab LAN when you want it to. So the kernel
injects the failures itself, below the checksum, where the rest of the path
cannot tell them apart from a bad cable:

```c
struct savanxp_net_tcp_fault {   /* NET_IOC_SET_TCP_FAULT on /dev/net0 */
    uint32_t drop_tx_every;      /* 1 of every N segments we send is dropped */
    uint32_t drop_rx_every;      /* 1 of every N segments we receive is dropped */
    uint32_t reorder_rx_every;   /* 1 of every N is held back and delivered late */
    uint32_t corrupt_rx_every;   /* 1 of every N arrives with a flipped byte */
};
```

Setting it requires the system (`EPERM` otherwise): the injector acts on every
connection, not only the caller's.

Everything received is checked before it is believed: the IPv4 header
checksum, the ICMP checksum, the TCP checksum, and the UDP checksum when the
sender included one. A datagram that fails is dropped without an answer, and
so is an IPv4 fragment, because there is no reassembly. A TCP segment is
matched to a socket by the whole tuple, also during the handshake.
`corrupt_rx_every` is what proves the TCP check: `tcptest` compares every
byte it reads, and with the check removed it fails on the first damaged
segment.

`drop_tx_every` counts only segments that consume sequence space, and
`reorder_rx_every` and `corrupt_rx_every` only segments carrying data. Pure ACKs are the majority of
the traffic, and including them spends the period on packets whose loss
exercises the *peer's* retransmission rather than ours.

`NET_IOC_GET_TCP_STATS` reports what actually happened — retransmits,
out-of-order, duplicates, out-of-window, window updates, aborts. A stack that
only works against a perfect network finishes a test run with those at zero, so
the test asserts on them as well as on the data.

`./build.sh smoke tcp-smoke` puts it together: `tools/tcp_echo_server.py` listens on
the host's loopback, the guest reaches it at `10.0.2.2` (how QEMU's user-mode
networking presents the host — no real network, no firewall rule), and
`tcptest` connects through 50% loss on the handshake, verifies 32 KiB streamed
in and 8 KiB echoed out byte for byte, and requires the counters to prove the
failures happened. The exact counters depend on packet timing.

The expanded test accepts two writes while dropping their transmissions, then
sleeps without socket polling and checks that both retried. It closes with
unacknowledged data and asks a second connection to retrieve the exact bytes
saved by the peer, then drops FIN and proves its retransmission through the
lifetime diagnostic. It checks that active close enters `TIME_WAIT`, prevents
rebinding and releases all closing state and the bound port after the real
60-second interval. A peer that keeps its write half open and a FIN whose
entire retry budget is lost also exercise orphan expiry without leaks.
`native-network` additionally runs concurrent SUN threads
with distinct byte patterns under loss, cancels a pending connect and closes a
live stream during a retained readiness wait.

`./build.sh smoke net-smoke` is the other half and covers the layer below — the NIC
driver, ARP and ICMP against the slirp gateway.

## Name resolution: `sxndns`

Name resolution lives in userland, in one shared SUN library:
`include/sxn/sxndns.h`, built from `system/runtime/sxndns.c` into every native
program's runtime. The stack itself takes no names. A **resolver** is a plain
struct its caller owns, cache included, and one thread uses it at a time: a
browser window's network worker, a tool's main thread. There is no system-wide
resolver service and no shared cache across processes; that waits for a second
consumer that needs one.

What `sxndns_resolve` does with a name:

- **Numeric IPv4 never touches the network**, and neither does a name that is
  not a host name (`SXNDNS_ERROR_BAD_NAME`): over 253 characters, an empty or
  64-character label, a character other than a letter, digit, `-` or `_`.
- **A cached answer is returned at once.** Names are cached lowercased without
  a trailing dot, 16 entries, for the smallest TTL along the records used and at
  most five minutes; a TTL of zero is not cached. Failures are not cached. A
  resolver given a different server (`sxndns_set_server`) empties its cache:
  those answers were the old server's.
- **Otherwise one A/IN question over UDP** to the resolver's server
  (`10.0.2.3:53` by default, QEMU slirp's DNS), from a fresh ephemeral port.
  Budgets are monotonic: up to six seconds for the route to the server (NIC
  setup, ARP), then two attempts of two seconds each, each with a new TXID.
- **A reply is believed only from the server's own address and port, for this
  TXID and this exact question.** Anything else is someone else's datagram and
  is ignored while the wait goes on: a forged reply from another port loses to
  the real one. A reply that passes those checks but whose bytes overrun or
  contradict themselves is remembered and waited past; if nothing better
  arrives the result is `SXNDNS_ERROR_BAD_REPLY` rather than a timeout.
- **A truncated reply (TC) asks again over TCP** to the same server and port,
  within what is left of that attempt's two seconds. The reply is assembled
  from however many reads it arrives in, the two-byte length prefix included,
  and refused past 4 KiB.
- **CNAME chains are followed** inside one reply (up to 8 aliases), and records
  about names that are not on the chain are not believed. An alias the server
  did not resolve is asked about again, up to four more questions; a chain that
  loops is `SXNDNS_ERROR_BAD_REPLY`.
- **Every failure says what it was:** `NO_SUCH_NAME` (NXDOMAIN), `NO_ADDRESS`
  (the name has no A record), `SERVER_FAILURE` (SERVFAIL, REFUSED and every
  other error code), `TIMEOUT`, `NETWORK`, `BAD_REPLY`, `CANCELLED`.
  `sxndns_error_text` turns each into a sentence for a status line.

**Cancellation** is a manual-reset event passed with the call. The event comes
first in every wait, so it wins over a ready socket; the call closes what it
opened and returns `SXNDNS_ERROR_CANCELLED`. A set event still lets a numeric
address or a cached answer through, since neither waits.

Two honesty notes. Transaction IDs mix the monotonic clock, a counter and the
resolver's address, because the tree has no kernel RNG yet; the source check
and the echoed question carry the rest. That is enough against accidents and
stale replies on a host bridge, not against an adversary who can see the
query. And there is no DNSSEC, no AAAA, no search domains and no negative
cache.

The wire format is a separate pure module, `system/runtime/sxndns_message.c`,
so `sxndns-message-test` checks it on the host: replies that belong to someone
else, replies whose bytes lie, every cut of a good reply, every single flipped
bit, and TCP framing fed one byte at a time. In the guest, `nativetest network`
runs the resolver against `tools/dns_fixture.py`, which the echo server of
`native-network` starts on one port number for both UDP and TCP. One name per
path: aliases in one reply and across questions, a loop, a truncated reply
sent back over TCP in pieces, a forged reply from another port, NXDOMAIN,
SERVFAIL, an empty answer, a malformed reply, silence, a one-second TTL, and
cancellation from another thread. The fixture counts the questions it
receives, which is how the test tells a cached answer apart.

The consumer today is the Web Viewer (`system/webview/webview.c`, see
[`WEBVIEWER.md`](../desktop/WEBVIEWER.md#names)), with two overrides for tests:
`webview --dns 10.0.2.2:5353` at launch and `dns://10.0.2.2:5353` typed in the
address bar (config, never history). `ntpsync` and the other tools still want
numeric IPv4.

**Pending, POSIX parity:** the POSIX subsystem has no resolver at all, no
`getaddrinfo` and no `gethostbyname`. It is to gain one over the same wire
module when a port or tool needs it ([AGENTS.md](../../AGENTS.md#feature-parity-between-subsystems)).

## What this does not do

Not defects — scope. In rough order of when each would start to hurt:

- **No `listen`/`accept`.** Client sockets only. Nothing in the guest can be a
  server.
- **No DNS in the syscall surface.** Addresses are numeric; `ntpsync` and every
  other tool still want numeric IPv4. Resolution lives in userland, in SUN's
  `sxndns` (see [above](#name-resolution-sxndns)); POSIX has none yet.
- **`setsockopt` understands only the two timeouts.** `SO_REUSEADDR` and
  `SO_BROADCAST` are accepted and ignored in userland; everything else is
  `ENOSYS`. `getsockopt` reads back a userland copy, not the kernel's.
- **No congestion control.** With one segment in flight there is nothing to
  control; a real send window needs slow start and congestion avoidance with it.
- **No MSS option, no path MTU discovery.** We send at most 1 KiB per segment
  and accept whatever the peer sends.
- **No SACK, no timestamps, no window scaling.** Recovery is one hole at a time,
  the RTO is fixed rather than measured from the round trip, and the window is
  capped at 64 KiB — well above the 8 KiB receive buffer, so it does not bind.
- **A single fault-injection stash** shared across sockets. It is a test hook,
  sized for tests, and only the system may set it.
- **No IPv4 reassembly.** Fragments are dropped.
- **No IPv6.** The address configuration is static IPv4
  (`10.0.2.15/24`, gateway `10.0.2.2`) matching QEMU's user-mode network.
