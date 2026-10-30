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

A stream that has already played a frame is concealed, not zeroed: the daemon
repeats its last frame, scaled by its own volume, so a brief packet gap does not
click. Before the first frame the contribution is silence. Concealment is not
the stream's lifetime: it covers `AUDIOD_PLC_MS` (50 ms) from the last datagram
with samples and then stops, while the entry itself lives on until 500 ms of
silence expire it. Holding the last frame for that whole window kept the device
busy for half a second after a client was killed -- audible residue for audio
nobody asked for -- and the entry's own expiry does not need it. Concealed
frames are counted as underruns in the stats.

Control rides the same socket under another magic (`SAUC`): `DECLARE` names
the sender's own stream, `SET_VOLUME` levels any stream by port, and `LIST`
takes a census (the daemon answers one datagram per stream with a count, so
an empty census still answers). No handshake here either: names attach on
arrival, volumes persist while the stream lives, and anything malformed is
dropped and counted. There is deliberately no access control, matching a
single-user machine where every program already shares the screen.

Per-application volume rides the same control socket: a client `DECLARE`s a
name, `LIST` returns the census the popup and `volume list` read, and
`SET_VOLUME` levels any stream by port. Levels are per session and are not
persisted; the stream's name is announced again every 10 s so a restarted
daemon relearns it.

## SUN clients

A SUN program has no sockets, so it cannot send a datagram. audiod also
creates the native port `audiod` (`SAVANXP_AUDIOD_PORT_NAME`), and a SUN
program reaches it through `sxn/sxnaudio.h`: `sxnaudio_open` connects, and
`sxnaudio_write` sends **the same datagrams**, each wrapped in a fixed-size
record (`struct savanxp_audiod_record`, 1024 bytes). The wire protocol both
transports share lives in `include/audio/audiod_protocol.h`, outside either
subsystem, like the WM's; `savanxp/audio_server.h` keeps only the UDP side.

The record size is the whole design:

- **Eight records fill a pipe exactly** (8 KiB), and a non-blocking write of a
  whole record is all or nothing, so neither end ever sees half of one.
- **A full pipe ends a write early, it does not block or drop.** The pipe holds
  about 40 ms of sound; `sxnaudio_write` returns how many frames it took, and
  the rest is simply due again on the next `sxnaudio_frames_due`, which paces
  the program by the clock at a lead it chooses.
- **Each connection is a stream** numbered by its slot, 1 to 8
  (`SAVANXP_AUDIOD_NATIVE_FIRST_PORT`), below every port UDP hands out, so the
  census, `SET_VOLUME` and the volume popup treat it like any other stream.
  `DECLARE` names it; `LIST` is answered with REPLY records on the event pipe.
  Closing the pipe ends the connection, and the stream expires with its
  silence like a UDP one.

Without a daemon (smoke mode, no NIC) there is no port, and `sxnaudio_open`
falls back to writing `/dev/audio0` directly -- the same "first writer wins"
rule as below. `nativetest` asserts both paths: direct with no daemon, then
through the port of a real `/bin/audiod` it starts, finding its declared name
in the census.

## Who holds the device

Whoever writes first keeps it; there is no preemption. init spawns audiod
before windowd, so in the steady state the daemon always wins and every
client learns remote with its first write (`EBUSY` means someone mixes).
Without a daemon -- smoke mode, no NIC, old image -- the same first write
succeeds and everything behaves exactly as before, with zero new code paths
for the no-daemon case. If the daemon dies serving, init relaunches it and
clients notice on the next send, probing direct until it is back, so the
system degrades to turn-taking instead of going silent; a deliberate startup
exit (no network, no device, another daemon already listening) is left alone
rather than respawned. If a direct holder outlives a daemon restart, the
daemon waits its turn.

This is also why there is no per-client state worth persisting: pause and
close just stop sending, and the stream expires on its own.

## Deliberately out

Capture routing, resampling in the daemon, and real-time promises: slices are
wall-clock sized and loss is counted, not hidden. See
`docs/media/SXMEDIA.md` for the withdrawn layer this replaces the need for, and
`docs/media/MEDIA_PLAYER.md` for the clock both sides still share.
