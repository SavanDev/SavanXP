# audiod: the mixing daemon

One writer per `/dev/audio0` and one mixer per process meant two programs
could never sound together: the second got `EBUSY`. `audiod` (`/bin/audiod`,
no window, no launcher entry) is the single writer and sums what clients
send; every client keeps mixing its own voices and sends finished stereo.
Master volume and mute stay in the kernel, so the taskbar never learned a
new protocol.

## Why UDP loopback and not a new primitive

The kernel already delivers UDP to its own address without ARP or a NIC on
the path (`sendto` short-circuits to the local socket table), with a 1024
byte cap and 16 datagrams queued per socket. That fits control and 48 kHz
slices at a steady pace; a catch-up burst after a long stall can still
overflow the 16 slots, and those slices count as lost instead of delaying
everyone. It needs no new syscall, no filesystem rendezvous and no windowd
involvement -- any process, parented or not, can reach the daemon. The price
is honest: on a full queue the kernel drops instead of blocking, so slices
carry sequence numbers and the daemon counts gaps instead of pretending
they cannot happen.
Capture keeps its own independent owner and never goes through the daemon.

## The protocol (savanxp/audio_server.h)

One datagram type: a 16 byte header (magic, version, channels, rate,
sequence) plus stereo s16le frames, 1024 bytes total max. No handshake and
no connection: the first datagram from a source port registers the stream,
500 ms of silence expires it, and anything malformed, rate-mismatched or
over capacity is dropped and counted. Version bumps are explicit because
both sides rebuild from this tree.

Each client sends at the device rate, which it reads with `AUDIO_IOC_GET_INFO`
(opening never contends). The daemon only mixes streams that match its own
rate.

## Who holds the device

Whoever writes first keeps it; there is no preemption. init spawns audiod
before windowd, so in the steady state the daemon always wins and every
client learns remote with its first write (`EBUSY` means someone mixes).
Without a daemon -- smoke mode, no NIC, old image -- the same first write
succeeds and everything behaves exactly as before, with zero new code paths
for the no-daemon case. If the daemon dies, clients notice on the next send
and probe direct again, so the system degrades to turn-taking instead of
going silent; if a direct holder outlives a daemon restart, the daemon
waits its turn. A SIGKILLed daemon leaves a zombie (init does not supervise
it) and the same fallback covers it.

This is also why there is no per-client state worth persisting: pause and
close just stop sending, and the stream expires on its own.

## Deliberately out

Per-application volume (the header has room, the popup does not use it
yet), capture routing, resampling in the daemon, and real-time promises:
slices are wall-clock sized and loss is counted, not hidden. See
`docs/SXMEDIA.md` for the withdrawn layer this replaces the need for, and
`docs/MEDIA_PLAYER.md` for the clock both sides still share.
