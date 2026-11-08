# audiod: the mixing daemon

One writer per `/dev/audio0` and one mixer per process meant two programs
could never sound together: the second got `EBUSY`. `audiod` (`/bin/audiod`,
no window, no launcher entry) is the single writer and sums what clients
send; every client keeps mixing its own voices and sends finished stereo.
Master volume and mute stay in the kernel, so the taskbar never learned a
new protocol.

## Native playback and legacy UDP

Rebuilt POSIX clients use the same native object port as SUN clients whenever
the device is busy with the daemon. Object ports, pipe I/O and nonblocking flags
are part of the shared kernel floor; POSIX process/file operations remain POSIX.
The existing `sx_audio_server_link` binary layout is unchanged: mode 3 stores
the native request-pipe handle in its old descriptor field. Closing uses the
appropriate handle API. Native EOF/error retries direct output and reconnects
to a replacement daemon; pipe pressure preserves the connection without blocking.
DECLARE precedes the first samples and is repeated after a role change. A
native connection retains its role across mixer entry expiry/re-registration.

Older binaries and explicit UDP census/controls remain supported. UDP requires
a configured network stack; native playback works on the physical host without
a supported NIC. Anonymous UDP transport cannot authenticate an executable and
therefore does not acquire persistent application preferences.

External ports embed their SDK runtime. A normal system build preserves those
installed binaries; it does not relink them. After the native audio transport
update, old Doom/Celeste ELFs can therefore play in QEMU with a supported NIC
while remaining silent and absent from Sound on the no-NIC physical host.
Rebuild the ports with the current SDK and reinstall their executables while
retaining `/games` data. Preserving a port is not the same as upgrading it.
`smoke audio-ports-offline` boots HDA without a NIC and checks the installed
Doom and Celeste independently: each must appear in Sound and produce actual
recorded PCM. This supplements the SDK-level offline test with real consumers.

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
Capture keeps an independent kernel owner; native clients share that owner
through the daemon's recording subscriptions described below.

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
- **Playback connections use their slot number**, 1 to 16
  (`SAVANXP_AUDIOD_NATIVE_FIRST_PORT`), below every port UDP hands out, so the
  census, `SET_VOLUME` and the volume popup treat it like any other stream.
  `DECLARE` names it; `LIST` is answered with REPLY records on the event pipe.
  Closing the pipe ends the connection, and the stream expires with its
  silence like a UDP one.

There are sixteen native connection slots and eight mixer streams. Capture-only
and control-only connections consume a connection slot but never register as
playback streams.

Without a daemon (a session that never started one) there is no port, and `sxnaudio_open`
falls back to writing `/dev/audio0` directly -- the same "first writer wins"
rule as below. `nativetest` asserts both paths: direct with no daemon, then
through the port of a real `/bin/audiod` it starts, finding its declared name
in the census.

## Sound panel control

The SUN [Sound panel](SOUND.md) uses a control-only native connection. It asks
for one atomic snapshot of every mixer stream and the output's PCM peaks,
and changes a stream's gain or mute using both its source port and a unique
registration id. Reused ports cannot receive an old panel's delayed command.
These additional messages leave the original UDP census/volume layouts intact.
Control-only connections do not register as playback streams.

The daemon holds per-channel absolute peaks for 100 ms, independently of reads.
Stream peaks include stream gain and mute; output peaks include the limiter
but precede kernel master gain. PLC retains raw samples and applies the current
gain/mute at render time, so muting an application also silences its concealed
frames. Native-control assertions live in `nativetest`, and `audiod --selftest`
compares the actual mixed samples for independent mute/gain and stale ids.

## Startup without networking

The native port is reserved before acquiring the PCM writer; an existing port
makes a duplicate daemon exit successfully. UDP is an optional compatibility
transport. A failed socket/loopback check logs `UDP unavailable; serving native
clients` and leaves the native mixer running. No NIC or network driver is
needed for SUN playback and Sound controls. `smoke audio-offline` runs the real
native audio assertions on HDA with QEMU's NIC and network backend removed.

## Optional jack policy

Native policy query/set messages are acknowledged independently of the mixer
snapshot. Manual mode is the default. Enabling requires detectable analog
headphones and another output on the selected device; policy is evaluated at
most every 250 ms. Only valid presence readings choose a headphone/fallback
route. Unknown readings do not cause a switch; failed transitions are latched
until presence changes, avoiding retries on every render tick. Manual output
selection disables the policy after successful acknowledgement. Support,
effective policy and the last automatic error are queryable by Sound.
The kernel still owns connector detection and transactional route setup.

The endpoint catalog includes all bound backends and independent playback/input
defaults. Policy remains limited to the currently selected analog device's
connectors. A manual selection can move playback without disturbing capture on
another controller or codec. Input changes explicitly retire old-source native
capture subscriptions, as described below.

## Who holds the device

Whoever writes first keeps it; there is no preemption. init spawns audiod
before windowd, so in the steady state the daemon always wins and every
client learns remote with its first write (`EBUSY` means someone mixes).
Without a daemon -- smoke mode or an old image -- the same first write
succeeds and everything behaves exactly as before, with zero new code paths
for the no-daemon case. If the daemon dies serving, init relaunches it and
clients notice on the next send, probing direct until it is back, so the
system degrades to turn-taking instead of going silent; a deliberate startup
exit (no device or another daemon already listening) is left alone
rather than respawned. If a direct holder outlives a daemon restart, the
daemon waits its turn.

Pause and close stop sending; the live mixer stream expires on its own.
Its persisted identity is independent of that transient entry.

## Preferences and durability

The service owns the single-user profile's saved audio policy. The companion
`Settings/audio-mixer.cfg` begins with `SAVANXP_AUDIO_PREFS 1`, followed by one
`D` record (output, input, fallback, headphone policy, input volume/mute) and up
to 32 `A` records (uid, volume/mute, executable and role). Endpoint ids use full
16-digit hexadecimal fields; names use canonical zero-padded 32-byte hex fields.
The parser rejects unknown versions, extra tokens, malformed padding, duplicate
keys, overflow, truncation and files beyond 6 KiB. `audio.cfg` preserves the
legacy two-number master format. Existing loose-profile/root settings remain
read fallbacks. A missing canonical file first tries its `.previous` copy;
a malformed canonical is reported rather than silently reverting to older state.

Trusted identities come from native port credentials and the kernel process
inventory, never a client-supplied executable string or UDP source port.
Explicit controls update persistent intent; automatic headphone transitions do
not replace the saved manual output. A saved missing device remains remembered.
The bounded application table stores nondefault controls only. `--no-preferences`
disables loading/saving for diagnostics and isolated protocol tests.

Changes are observed every 100 ms and debounced for 250 ms. One persistent
`audiod --save-preferences` worker receives bounded snapshots on a private pipe,
performs POSIX file I/O and replies only after final `sync` completion. The mixer
never loads a new helper image for every change. A crashed writer is restarted
only while playback entries and capture subscribers are idle; dirty state and
an explicit error remain until a durable retry succeeds. Ordinary save failures
retry after five seconds. The service does not acknowledge an unsaved flush.

Each complete temporary file is private (0600) and owned by the profile user.
The worker syncs it, rotates the previous canonical to `.previous`, installs the
complete new file and finally syncs both files. SxFS cannot replace an existing
rename destination, so installation failure restores the old canonical. The
previous complete copy survives a missing canonical after interruption. The two
files are **not a cross-file atomic transaction**: interruption may retain one
new file and one previous file; no successful flush is sent until both complete.
Power-loss behavior still depends on the storage device honoring flushes.

Native flush requests identify a generation; replies include durable generation
and load/save errors. A query is not a durability guarantee. The mixer bounds
native draining to one pipe capacity per client per turn and measures render
time after control handling, preserving PCM deadlines during control traffic.
The mixer is compiled with optimization even with a Debug kernel: rendering
eight PCM streams remains a deadline-driven workload in that configuration.

## Native recording

`sxn/sxncapture.h` connects with acknowledged CAPTURE_START/STOP records.
The first subscriber starts kernel capture and the last one stops it,
independently of playback. Metadata identifies the actual rate, sample format
and period size. The daemon polls nonblocking complete device periods and
fans the same post-gain/mute input out to subscribers; capture never enters
the playback census or packet-loss concealment.

Each subscriber has a bounded 4096-frame FIFO plus eight event-pipe records,
about 127 ms at 48 kHz. A slow reader drops only its own oldest unsent frames.
PCM records carry an absolute frame index, so each client can count loss and
decide whether to discard its recording. EAGAIN on the event pipe preserves
queued samples. Device errors are retained until the pipe can deliver the
error or closes; they are never replaced by successful silence.

Selecting a different input stops capture, discards queued old-source samples
and sends subscribers a retry error. Selecting the effective input is
idempotent. Clients reopen explicitly; Sound does so automatically for its
meter, while invalidating an interrupted test. The SUN helper retains terminal
errors even if a read first returns already copied frames. A new subscription
resets the error and frame index.

Without the daemon, SUN reads periods directly with the same API. Readable
and writable audio device handles release their respective directions on close;
ownership is per process, so closing another readable handle in that process
also ends its direct capture session. Use one dedicated handle per direction.

## Deliberately out

HDMI, per-application device routing, resampling in the daemon and real-time promises: slices are
wall-clock sized and loss is counted, not hidden. See
`docs/media/SXMEDIA.md` for the withdrawn layer this replaces the need for, and
`docs/media/MEDIA_PLAYER.md` for the clock both sides still share.
