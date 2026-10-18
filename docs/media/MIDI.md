# libsxmidi: the MIDI synthesizer

`libsxmidi.so.0.4` turns a Standard MIDI File into PCM. It exists because the
userland runtime had no synthesiser at all: `savanxp/audio.h` mixes
already-decoded voices, and `audiod` sums finished stereo. Anything that wanted
music had to bring its own decoder, which is why the ccleste port decodes its
tracks through FFmpeg. Doom plays MUS lumps, and neither is a codec — so the OS
grew a synth instead of a second decoder.

The library is built by the tree, staged at `/disk/lib/libsxmidi.so.0.4` and
loaded through the normal `DT_NEEDED` path. Its public surface is
`savanxp/midi.h`.

## Two banks: GENMIDI first, families as fallback

A faithful General MIDI bank is a soundfont plus a sampler: megabytes of samples,
a provenance entry, and a loader for a format nobody else in the tree reads.
This library instead has two sources, per song:

- **The WAD's `GENMIDI` bank**, when the caller passes it. A Doom WAD ships the
  ~175 two-operator OPL2 instruments that DMX fed the chip (128 melodic plus 47
  percussion for keys 35..81), and the song plays through them. This is the timbre
  the MUS tracks were written for.
- **Sixteen built-in families plus a drum kit**, when there is no bank or it
  does not parse. One family per eight program numbers, plus seven drum shapes on
  channel 9. That is enough for each family to keep its character (piano vs
  strings vs metal vs percussion), which is what makes a track recognisable
  without its original instruments. It is an approximation by design, not a GM
  implementation; `savanxp/midi.h` says so.

The bank belongs to the song, not the process: `sx_midi_song_create_with_bank`
takes the `GENMIDI` lump as bytes, decodes it during the call, and two songs in
the same program can play with different banks. Without a bank, with a short
one, a bad header, or a record the chip could not have produced, the song falls
back to the families and renders exactly what `sx_midi_song_create` would have.
`sx_midi_song_uses_genmidi` reports which path a song took, for the caller to
count — never to branch on, since the fallback is already settled inside.

## The FM voice

Each OPL voice is two operators — a modulator and a carrier — with the chip's
own parameters: one of four waveforms, a frequency multiplier (0 means ×0.5),
a fixed level in 0.75 dB steps, key scaling (KSL), per-operator ADSR rates with
key-rate scaling (KSR), tremolo and vibrato flags, and a sustaining flag that
decides whether the voice holds its sustain level or keeps falling through it.
Feedback feeds the modulator's last two outputs (already levelled, so it dies
with the envelope) back into its phase; the connection bit chooses FM or
additive summing instead.

An instrument holds two such voices plus its header: fixed-pitch flag and note,
double-voice flag, fine tuning for the second voice only (1/32 semitone steps),
and a base note offset per voice. A double-voice instrument sounds both voices
at once, the second detuned — that width is the chorus of the brass and strings.
Percussion lives in the same table: all but three keys are fixed-pitch, so the
drum does not follow the keyboard, and keys outside 35..81 open no voice at all.

Everything is integer fixed point. The only table the library builds is a sine,
generated at first use with a fifth-order Taylor series over a quarter period and
mirrored for the rest, plus an attenuation-to-gain table (0.75 dB per step) and
a rate-to-milliseconds table built by repeated multiplication, not copied. The
envelope itself runs in Q16 attenuation so a 5.5 s tail still takes 5.5 s instead
of collapsing to 128 samples. There is no `libm` dependency and no vendored OPL
core: `libsxmidi` asks the executable only for
`malloc`/`calloc`/`realloc`/`free`/`mem*`, exactly like `libgfx2d`.

The families, for comparison, are subtractive: two oscillators (the second at an
interval and weight the family chooses), each blended toward a sine (`wave_mix`),
through a one-pole filter whose coefficient drops with the note, an ADSR
envelope, an optional pitch LFO (vibrato) and an attack pitch sweep (the kick
starts two octaves up and falls in 45 ms). The drum kit leans on the same
fields.

## The pipeline

- **Parse.** `sx_midi_song_create_with_bank` reads an SMF type 0 or 1: it walks
  every `MTrk`, honours running status, tempo meta events and the usual channel
  messages, drops aftertouch and sysex, and merges the tracks into one event
  array sorted by absolute tick (stable, so equal ticks keep their order). It
  then decodes the `GENMIDI` blob, if any, into 175 patches.
- **Time.** Tick advance is exact. Each sample adds `division * 1e6` to a
  counter and a tick is consumed when it crosses `rate * tempo_us`; a truncated
  `samples-per-tick` step drifts about 0.1% and the loop stops landing on the
  same sample. A tempo event recomputes the threshold, so the tempo map is
  honoured mid-song.
- **Synthesise.** Up to 32 voices, one per sounding note (two per note for a
  double-voice FM instrument). Note-on picks a `GENMIDI` patch by program — or
  by key on channel 9 — or a family by `program / 8` without a bank; pitch bend
  is ±2 semitones applied to live voices. The render loop is event-driven per
  sample, so notes start on their exact tick.
- **Output.** Mono unsigned-8-bit, centred at 128, soft-kneed the same way the
  mixer is, which is exactly what an `sx_audio_mixer` voice consumes.

`duration_ms` comes from the merged tempo map, and end-of-track counts even when
it falls after the last event, so a trailing rest is part of the loop. `render`
fills as many frames as it is given (`loop` wraps and rewinds deterministically);
`render_all` is the convenience that mallocs one PCM buffer for a whole song.

## What it does not do

No vendored register-accurate OPL2 core and no MUS-to-OPL sequencer: the FM
voice is a two-operator approximation (phase modulation with the chip's rates,
levels and scalings), not a cycle-counted YM3812. No streaming callback into the
mixer, no stereo pan, no reverb or chorus, and no SMPTE division. A program
that needs block streaming can still call `render` in a loop; Doom does not.

## How Doom uses it

`ports/doomgeneric` is the reference consumer. Its music module
(`overlay/doomgeneric_savanxp_audio.c`) converts a MUS lump to MIDI with the
upstream `mus2mid.c` that the port now compiles, looks up the WAD's `GENMIDI`
lump and creates the song with `sx_midi_song_create_with_bank` — without it the
same call falls back to the families — and on `PlaySong` allocates a mono buffer
the length of the song, clears it to silence, synthesises a one-second lead into
it with `sx_midi_song_render`, and starts the looped voice over the whole
buffer. From then on `I_UpdateSound`'s `Poll` — once per frame — synthesises one
more quarter-second chunk in place: the voice walks the buffer while the tail is
still silence, and the song is never rendered in one shot. That is not an
optimisation for its own sake: 96 s of music cost ~200 ms natively for the
families (~140 ms for FM) and much more under TCG, which is a visible stall on
every music change, while a per-frame chunk costs well under a millisecond
(FM runs ~660x real time on the host, about 0.7x the cost of the families).
The PCM goes into a dedicated mixer voice — one past the SFX channels, so the
round-robin never steals it — with loop enabled. Music volume moves the voice's
pan, and pause sets it to zero so the position survives. The start cost stays
visible in the log line
`doomgeneric: musica sonando: <frames> frames a <rate> Hz (loop) (FM|familias) (arranque <ms>)`.

That made the port PIE: a library is only mappable from an `ET_DYN` with a
`DT_NEEDED`, which is the same profile ccleste and mediaplayer already use. The
port builds `ldso.c` in and links `-l:libsxmidi.so.0.4` from
`build/diskfs/lib`. Without the library at runtime the loader reports it and the
game still runs, silent, like ccleste without FFmpeg.

## Verification

`tests/host/sxmidi_test.cpp` builds SMF in memory and asserts what comes out:
duration from the tempo map, frame count, that most samples are not silence, a
zero-crossing frequency estimate for C4, that a program change changes the
timbre, that a note settles to silence after release, that a drum on channel 9
sounds, that looping rewinds to the same samples, and that malformed input is
rejected instead of guessed. `./build.sh smoke sxmidi-smoke` runs it.

The timbre itself is checked by what each feature does to the signal, not by a
reference recording: the kick has to sound at least 1.5x sharper in its 45 ms
attack than in its tail, the pad's LFO has to move the crossing count by ~20
between peak and valley windows while a timbre without vibrato counts the same
in both, and a bell has to stop anti-correlating at half a period.

The FM path has its own checks on a synthetic bank, each with a falsification —
a bug it would catch: a valid bank reports `uses_genmidi == 1`, renders
non-silent, tunes C4 and differs from the families; a missing, short,
bad-header or impossible-waveform bank reports 0 and renders bit-identical PCM
to no bank (a permissive parser would keep it at 1); two banks whose second OPL
voices differ must render differently (a duplicated first voice would not); full
pitch bend must land on D4, not eight semitones up; key 36 on channel 9 must
sound while key 20 opens no voice; and a fixed-note 96 must stay at C7 instead
of wrapping an octave down. The slow-envelope tail is covered by loudness: with
rate-0 decay the note must still be sounding past the first milliseconds, which
an integer-step envelope would have silenced.

The port side is covered by `tests/host/doom_port_layout_test.py` (the source
list, the overlay set, the pinned tree), by `tests/host/doom_mus_test.cpp`
(synthetic MUS plus, when a WAD is present, the real `D_E1M1` lump rendered both
ways: through the families and through the WAD's own `GENMIDI`, asserting the
bank path is taken and stays loud), and by `tools/verify_doom_persistence.sh`,
none of which need audio. The check that listens is
`./build.sh smoke doom-music-smoke`: it opens the port from the desktop, starts
the level, asserts that the WAV QEMU captured is not silent, then closes the
window with Alt+F4 and asserts that the capture falls silent with the last music.
The close matters because the window manager answers a close request with
`SIGKILL`, so nothing on the client side runs: whatever still reaches the device
afterwards is the audio daemon keeping a dead stream alive, and the assertion
(`_wav_residue_s` in `tools/shoot_session.py`) measures exactly that — its
distance from the last window with movement to the last non-zero sample. It needs
the port and a WAD installed in the volume; without them the capture is silent and
the smoke fails rather than passing quietly. The port prints
`doomgeneric: musica sonando: <frames> frames... (FM)` when a track starts and a
`doomgeneric: musica: ...` line naming the failed step when it does not.
