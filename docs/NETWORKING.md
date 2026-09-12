# Networking: what the TCP path guarantees

The kernel stack is small on purpose, and the small parts still have to be
correct. This document covers the TCP client path in `kernel/net.cpp`: what it
guarantees today, the design decisions behind those guarantees, and — as
explicitly — what it still does not do. ARP, ICMP and UDP are simple enough to
read directly from the source; TCP is not.

## The shape of the stack

```
app  ──►  socket syscalls  ──►  net::  ──►  nic::  ──►  rtl8139 / virtio-net
                                 │
                                 ├─ ARP cache (16 entries, 60 s lifetime)
                                 ├─ ICMP echo (/dev/net0 NET_IOC_PING)
                                 ├─ UDP  (datagram queue per socket)
                                 └─ TCP  (client only — see below)
```

Everything is polled. The stack has no heartbeat of its own: frames are pulled
from the NIC by whoever is inside the stack at that moment. This has a direct
consequence for TCP, spelled out under
[the retransmission clock](#the-retransmission-clock-is-the-poll-path).

## What TCP guarantees

Given a peer that behaves like a normal TCP implementation, a connected socket
delivers **the bytes that were sent, in the order they were sent, or an error**.
It gets there over a network that loses and reorders segments. Concretely:

- **A lost segment is resent.** Every segment that consumes sequence space —
  data, `SYN`, `FIN` — is kept until the peer acknowledges it. On timeout it
  goes out again with exponential backoff: 300 ms initial RTO, doubling to a
  4 s ceiling, 6 attempts, ~12 s total before the connection is declared dead.
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

### The retransmission clock is the poll path

`net_pump()` — poll the NIC, then service every socket's retransmission timer —
is called from the blocking `connect`/`read`/`write` loops and from `net::poll()`.

`net::poll()` used to be called by `poll_fds()`, which spun in the caller's
context. Now that `poll()` parks its callers instead
([`SYSTEM_MONITORING.md`](SYSTEM_MONITORING.md#waiting-is-not-running-what-the-first-measurement-found)),
`wake_poll_waiters()` calls it from the timer tick — but **only while at least
one process is waiting in `poll`**, which is the same cadence as before: with
nobody polling, nobody called it either. `wake_blocked_socket_readers()` does
the same for processes parked in a blocking socket `read()`, for the same
reason: whoever is waiting for the data cannot be the one who goes and gets it
while asleep. Neither is a heartbeat — together they cover every case where
somebody is actually waiting on the network.

The consequence to keep in mind is unchanged: **a socket with unacknowledged
data does not retransmit while no thread is inside the stack.** Blocking calls
are inside it by construction, and non-blocking callers get there through
`poll()`. A program that writes non-blocking and then never polls will stall
until it comes back. If that ever becomes a real pattern, the fix is an
unconditional periodic callback in the kernel, not more pump call sites.

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
[its own fix](SYSTEM_MONITORING.md#waiting-is-not-running-what-the-first-measurement-found).
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
};
```

`drop_tx_every` counts only segments that consume sequence space, and
`reorder_rx_every` only segments carrying data. Pure ACKs are the majority of
the traffic, and including them spends the period on packets whose loss
exercises the *peer's* retransmission rather than ours.

`NET_IOC_GET_TCP_STATS` reports what actually happened — retransmits,
out-of-order, duplicates, out-of-window, window updates, aborts. A stack that
only works against a perfect network finishes a test run with those at zero, so
the test asserts on them as well as on the data.

`build.ps1 tcp-smoke` puts it together: `tools/tcp_echo_server.ps1` listens on
the host's loopback, the guest reaches it at `10.0.2.2` (how QEMU's user-mode
networking presents the host — no real network, no firewall rule), and
`tcptest` connects through 50% loss on the handshake, verifies 32 KiB streamed
in and 8 KiB echoed out byte for byte, and requires the counters to prove the
failures happened. It is deterministic: the same counts on every run.

`build.ps1 net-smoke` is the other half and covers the layer below — the NIC
driver, ARP and ICMP against the slirp gateway.

## What this does not do

Not defects — scope. In rough order of when each would start to hurt:

- **No `listen`/`accept`.** Client sockets only. Nothing in the guest can be a
  server.
- **No DNS.** Addresses are numeric.
- **`setsockopt` understands only the two timeouts.** `SO_REUSEADDR` and
  `SO_BROADCAST` are accepted and ignored in userland; everything else is
  `ENOSYS`. `getsockopt` reads back a userland copy, not the kernel's.
- **No congestion control.** With one segment in flight there is nothing to
  control; a real send window needs slow start and congestion avoidance with it.
- **No `FIN` retransmission and no `TIME_WAIT`.** `close()` sends `FIN` once and
  releases the socket. If it is lost, the peer waits out its own timeout.
- **No MSS option, no path MTU discovery.** We send at most 1 KiB per segment
  and accept whatever the peer sends.
- **No SACK, no timestamps, no window scaling.** Recovery is one hole at a time,
  the RTO is fixed rather than measured from the round trip, and the window is
  capped at 64 KiB — well above the 8 KiB receive buffer, so it does not bind.
- **A single fault-injection stash** shared across sockets, and the injector is
  reachable by any process that can open `/dev/net0`. It is a test hook, sized
  for tests.
- **No IPv6.** The address configuration is static IPv4
  (`10.0.2.15/24`, gateway `10.0.2.2`) matching QEMU's user-mode network.
