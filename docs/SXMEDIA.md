# SxMedia: what was built, what it taught us, and why it is not in the system

**Status: withdrawn.** The code is gone. This document is the record, kept so the
work is not lost and so the next attempt starts from what was learned rather than
from what was assumed.

The layer was built across eleven commits between `092dfd5` and `42c12ca` and then
removed. It worked, at the level it reached. What it could not do was the one thing
it was for, and the reason is structural rather than a matter of effort.

---

## What it was

A multimedia layer between the kernel's media devices and the programs above them,
so that a program never depends on FFmpeg:

```
program  ->  SxMedia engine + backend registry  ->  backends
```

Three separable roles per backend, chosen so the pieces could come from different
libraries:

| Role | Question it answers | Who filled it |
|---|---|---|
| source | can you parse this file? | FFmpeg (libavformat) |
| decoder | can you decode this stream? | FFmpeg (libavcodec), SxCodecs (stb_vorbis) |
| converter | can you resample or scale it? | FFmpeg (swresample, swscale) |

The engine opens no file and reads no samples. It is driven entirely by providers,
which is why the whole registry could be exercised on the host in 0.01 s with zero
`libc` stubs while the real thing took a QEMU boot.

What was built and verified at the end:

- the engine, the registry, three roles, a per-stream decoder claim
- a capability query (`sx_media_has_codec`) and a codec enumeration
- times crossing the interface as microseconds, so no `time_base` leaves a backend
- a notice that names the codec, never the library
- the FFmpeg port as a backend rather than an engine of its own
- SxCodecs' first codec: an Ogg Vorbis decoder, decoding a real file to within
  1 LSB of libvorbis over 43844 samples

## Why it was withdrawn

**Because a static link means a program can only use the backends it was built
with.**

The whole premise was that a program would ask SxMedia what it can play, and
SxMedia would answer from whatever backends were registered. That works inside one
process. It does not cross processes, and it never did:

- there is no `ld.so`, no shared libraries, nothing installable that is not a
  program (`--disable-shared` in the port, `grep dlopen` finds nothing)
- the registry is per-process, so `sx_media_has_codec` sees only the backends in
  the binary asking
- therefore a program is a different binary per set of codecs

So `mediaplayer` would have to be **a different program for every combination of
codec libraries**, and the "one player" that the design assumed could not exist. The
layer could tell a program what it could play, but only after the build had already
decided what it could play — which is the part that was never in doubt.

The alternative shapes, all rejected and all recorded here:

- **dynamic linking.** A real piece of work: segment mapping, relocations, TLS,
  ABI per library. It would let one program hold many backends. It would not make
  the enumeration easier, and nothing needs it yet.
- **a codec service over pipes.** The system has `spawn` and `pipe` today, so this
  is the narrow version of the same idea and needs no linker. It costs a session
  model, threads, and a copy for every block of audio. The end state, not the next
  step.
- **a backend per codec library, chosen at build time.** What actually got built,
  and what makes the combinatorial problem visible: the base player plays Vorbis,
  the port's player plays everything, and there is no mechanism that lets the first
  borrow from the second.

---

## What the attempts taught, which is the point of keeping this

### A document describing the right thing and an implementation doing the other is a bug nobody has written down

The header said the engine hands a frame back to "this same provider's scaler or
resampler". The engine took the converter from the *source* provider and nothing
else. Invisible while one library filled every role; the first time a second codec
existed, swresample was handed a block of interleaved s16, read the first bytes of
it as a channel layout, failed to initialise, and the stream produced silence with
nothing reporting why.

**It was found by a real Ogg file on the real target, not by any test.** A hundred
and sixty host checks had passed, and every one of them was a different library
filling every role.

### The claim list and the capability list must be the same table

A backend that advertises codecs from one list and claims them from another will
eventually advertise a file it cannot open. When that list is generated from the
build, the two cannot disagree at all — which is the argument for generating it and
the only real reason to.

### stb_vorbis is a whole-file decoder and the decoder role was packet-oriented

Its entry points want the file: `open_memory` takes all of it, and the pushdata
workflow resynchronises by finding Ogg page boundaries inside the bytes it is
handed. A demuxer hands out packets with that framing already stripped. Three
adapter attempts failed before it was accepted that the two are not the same bytes.

The vtable grew a second decoder shape — `open_whole` instead of `send_packet` —
and it worked, and it is still the right shape for any whole-file decoder. It just
does not help with the process boundary, which is the actual obstacle.

That shape also forced a change worth keeping regardless: choosing a decoder became
a loop that asks every claimant for a turn, because a whole-file decoder cannot
know whether it can read a file until it has tried, so a NULL from it is a
statement and not a failure. A claim is an intention, not a guarantee.

### Two decoders disagree about where a track ends, and neither is broken

stb_vorbis decoded 22050 frames of the fixture where libvorbis trimmed to 21922.
The file declares 22050, which is what `ffprobe` reports. The extra 128 frames are
signal at about a tenth of full scale, not padding — a 2.9 ms tail, which is a seam
in a looped track. Worth knowing, not worth patching a pinned file for.

### The lazy resampler is not a resampler

The Vorbis backend's converter copies samples and **refuses** a rate mismatch. A
linear resampler is about sixty lines and would have made a 44.1 kHz track "play" on
a 48 kHz device, and a music track resampled by something that is not a resampler
has an artefact in it. The first two consumers of that codec are two platformers'
worth of music. The gap was left open rather than filled with something that would
have hidden itself.

### The host suite cannot catch a port breaking

A vtable change left the FFmpeg backend's positional initialiser misaligned, and
nothing noticed for two commits: the host tests do not compile the port. The first
relink failed. A field-count check now exists in the port's layout test and is
verified to fail when the table is short — but that is a text check standing in for
a compile, and the compile is the real fix.

### Test the important path first

The last piece of work built a whole capability tag, a new launcher, and four smoke
scenarios before running anything, and the tag's discovery path did not work. The
one question that mattered — "does `sxe_load_meta` read a 7 MB binary out of
`/disk/bin` from a userland program?" — is ten lines and one smoke. Build that
first next time.

---

## What has to exist before trying again

In order, because each one removes a reason the last attempt failed:

1. **A way for one program to use a codec library it was not built with.** Either a
   dynamic linker, or a codec service over the pipes the system already has. Without
   this there is no "one player", only N players.
2. **A generated capability table**, which the attempt already proved is a good
   idea: derived from the same configuration that linked the library, never typed.
   Cheap, and it stays correct on its own.
3. **A notice that names the codec**, which is the part that does not depend on any
   of the above. A user who cannot play a file should be told `vorbis`, not
   `stb_vorbis`, and never be told which library failed.
4. **A streaming voice in the mixer.** Independent of SxMedia and still blocking:
   `sx_audio_mixer_start_voice` takes a whole sample buffer, so a three-minute track
   has to be decoded into memory first. Both music consumers need this.

The first of these is the whole ballgame. The other three are worth doing on their
own merits and none of them needs the layer that was withdrawn.

## What was thrown away, and where to find it

`git log 11b4221..42c12ca` on the branch that carried it. Eleven commits, in order:

| Commit | What |
|---|---|
| `092dfd5` | the original design document |
| `7853817` | the engine and the registry, 118 host checks |
| `1ace43d` | the FFmpeg port became a backend instead of an engine |
| `4b1f096` | the codec setup and the time base across the seam |
| `976faac` | the audio buffer sized from the block, not a fixed guess |
| `86a2356` | recorded that stb_vorbis cannot be a packet decoder |
| `96c3d7d` | the whole-source decoder shape |
| `4f47b69` | its test, and the fallback loop it found missing |
| `b7ad5a6` | the Vorbis codec, and the converter bug it found in the image |
| `42c12ca` | a build publishing what it can decode |

The pinned `stb_vorbis` (`1ee679ca2ef753a528db5ba6801e1067b40481b8`, Unlicense/MIT)
was vendored and never compiled into anything. `docs/THIRD_PARTY_PROVENANCE.md` no
longer lists it, so the pin is recorded here instead.

## The tool that was built and thrown away with it

`gen_sxmedia_caps.py` is worth keeping in this document's memory rather than in the
tree: it reads FFmpeg's own `config_components.h` for what was enabled and
`libavcodec/codec_desc.c` for each name and its media type, and emits a table of 24
decoders for one configuration. Asking the library with `av_codec_iterate` does not
work on the host, because the port's libav is a cross-compiled archive; reading the
two files configure generated is the same answer as text. That works, and it is
about forty lines, and it will work again whenever something needs to know what a
build can decode.
