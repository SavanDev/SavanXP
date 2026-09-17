# Media Player

How `/disk/bin/mediaplayer` plays a file, what it can and cannot promise about
audio/video sync on the devices SavanXP has today, and the order in which the
missing pieces should arrive. Building it is covered by
[`sdk/ffmpeg/README.md`](../sdk/ffmpeg/README.md).

## Layers

```
mediaplayer.c   window, controls, input; owns the loop
playback.c      clock, audio to /dev/audio0, which frame goes on screen and when
media.c         FFmpeg: demux, packet queues, decoders, swresample, swscale, seek
selftest.c      --probe, --selftest, --gpu-hold: the same engine without a window
```

`media.c` knows nothing about screens, speakers or time of day; `playback.c`
knows nothing about windows. The split is what lets the headless selftest drive
exactly the engine the window uses, and what a second front end (a fullscreen
mode, a command-line player) would reuse.

Everything is **one thread**, because the kernel has no threads. The loop is:

1. drain keyboard and pointer events;
2. `playback_pump()`: top up the audio device, present the frame that is due;
3. repaint the controls if they changed (at most every 250 ms while playing);
4. sleep in `poll()` on the window's event channel for as long as the pump
   said -- never more than 10 ms while playing, which is what the audio cushion
   allows.

A decode that takes longer than a frame stalls the whole loop, input included.
That is the price of no threads, and the first thing threads would buy.

## What a binary costs

Every enabled decoder is linked in: `allcodecs.c` references them all, so static
linking cannot drop the ones a program never opens. That is why the test tools
are modes of `mediaplayer` and not separate binaries. With the current format
set the ELF is ~6.6 MiB of text.

It also carries **~18 MiB of BSS**, almost all of it `ff_tx_tab_*`: libavutil's
transform tables, one static array per size up to 2^21 entries for each of
`float`, `double` and `int32`, filled on first use. On a system with
demand-zero BSS that costs nothing until touched; the SavanXP loader maps the
whole BSS at exec, so today it is resident memory per running instance. Lazy
BSS in the kernel is the fix, not a patch to FFmpeg.

## The engine is pull-based

The caller *asks* for the next video frame or the next N audio samples, and
`media.c` reads packets from the container until it has what was asked. Packets
of the other stream found on the way are queued, not dropped, so asking for
video never loses audio. Queues are capped at 8192 packets per stream; a file
only reaches that if one stream is ignored for minutes, and the selftest fails
if anything was dropped.

All times cross the interface in **microseconds from the start of the file**
(the container's `start_time` already subtracted). No `time_base` leaves
`media.c`.

Output formats are fixed by the consumers, not negotiated:

- video: `AV_PIX_FMT_BGR0`, which is byte for byte the SavanXP framebuffer
  (`XRGB8888` with the top byte zero, see `gfx_rgb`). `BGRA` would put `0xff`
  in that byte. Scaling uses `SWS_FAST_BILINEAR`: without assembly the cost
  difference to `SWS_BILINEAR` is large and on moving video the quality
  difference is not.
- audio: interleaved `s16` at the device's rate and channel count, whatever the
  file has. `swresample` is rebuilt if the input format changes mid-stream.

### Accurate seek

The demuxer is asked for the keyframe *at or before* the target
(`avformat_seek_file` with `max_ts = target`). Frames and samples before the
target are still decoded -- later frames reference them -- but not delivered:
the first frame out is the one on screen at the target, and audio is trimmed to
the exact sample. Each stream clears its own skip mark on its first delivery,
so a timestamp that goes backwards later (an MPEG-TS wrap) is not discarded.

Seeking costs decoding from the previous keyframe. For MJPEG or short GOPs that
is nothing; for H.264 with a 10 s GOP it is up to 10 s of video decoded without
showing it.

## The clock

The master clock is the **wall clock** (`monotonic_ns`), not the audio device,
because `/dev/audio0` cannot say how much it has played. What the device *does*
tell us shapes everything else:

- `write()` never blocks. When the driver already has as much in flight as it
  accepts (8 periods on AC97), the period is **discarded** and `write()` still
  succeeds. A player that writes ahead "to be safe" gets silent holes.
- Opening the stream primes **4 periods of silence** (both `ac97.cpp` and
  `virtio_sound.cpp`): at 48 kHz with 1024-frame periods that is ~85 ms between
  writing a sample and hearing it.

So `playback.c` feeds audio *at the rate of the clock*: it keeps what has been
written at most 40 ms ahead of the clock (4 primed periods + 40 ms stays under
the 8 in flight), in 1024-frame chunks, every pump. And the position the player
reports -- the one video is timed against and the seek bar shows -- is the one
being *heard*:

```
position = anchor_position + max(0, elapsed_since_anchor - driver_latency)
```

The first frame is shown immediately; motion starts once the primed silence
has played out, together with the sound.

### Pause, seek and stalls all restart the stream

The latency above is only known right after the stream opens. Anything that
lets the driver's queue drain breaks it: after an underrun the AC97 driver
primes a *new* cushion, and the audio falls ~85 ms further behind the video
each time. So the player never lets that happen silently:

- **Pause** closes `/dev/audio0` (which also frees it for other programs).
  **Resume** seeks the engine back to the paused position and reopens: what
  was sitting in the driver's queue when it closed was never heard.
- **Seek** reopens the device, whatever the state.
- **A stall** -- the pump not running for longer than latency + lead, e.g. a
  slow decode -- is detected on the next feed. Everything written was heard by
  then, so the stream restarts at the last written sample, or at the video
  position if the video ran past it (the audio in between is skipped).

### Dropping frames

A frame more than 100 ms late is decoded but not shown, up to 8 in a row per
pump, so a decoder slower than the video catches up with the audio instead of
drifting. If the screen has not changed for 250 ms a late frame is shown anyway:
dropping everything would freeze the picture while the sound goes on.

## Control icons

The transport buttons, Open and the volume speaker are icon buttons with **our
own 16x16 art**, drawn by `sdk/ffmpeg/mediaplayer/gen_icons.py` and embedded as
`icons.inc` (~4 KiB, useless outside this program, so not `.sxicon` files on
disk). The app icon is separate and also our own.

Tango 0.8.90 -- the set the file-type catalog adopted -- was tried first and
rejected for this use: its `media-playback-*` and `media-seek-*` glyphs are light
grey with an outline, drawn for a white background. On the grey button face an
enabled Play reads the same as a disabled one. It is the same finding
[`THIRD_PARTY_PROVENANCE.md`](THIRD_PARTY_PROVENANCE.md) records for Tango's
`devices/` icons. The replacements use the system's app-icon palette: flat black
glyphs like the toolkit's text and scrollbar arrows, ochre for the folder, red
for the mute cross. A disabled icon is drawn with the toolkit's etched relief (the
silhouette in white, offset one pixel, under the same silhouette in grey), not
faded, so it matches a disabled label. The names follow the freedesktop Icon
Naming Specification, like `diskfs/mimeicon.ini`, so the source can change
without touching the program.

## What sync is verified, and how

`mediaplayer --selftest --sync /disk/media/avsync.avi` (part of
`build.ps1 ffmpeg-smoke`) decodes a generated AVI with a white flash frame and
a 1 kHz beep at the start of every second, audio resampled from 44.1 kHz mono,
and checks that every flash starts within 20 ms of its beep, and that a seek to
the middle lands on a flash frame and on the first sample of a beep.

That proves **the timestamps are right end to end**: demuxer, both decoders,
resampler and the seek trim agree on what time each frame and sample is. It
does not prove what reaches the speaker is in step with the screen -- that
depends on the latency model above, and no harness can listen. The manual check
is the same clip in the window: the beep has to land on the flash.

## Roadmap

In the order they unblock each other.

1. **A playback position from the audio driver** (`AUDIO_IOC_GET_POSITION`:
   frames actually played). AC97 can answer it from `CIV` and `PICB`. It would
   replace the latency constant and the stall heuristic with the real master
   clock every desktop player uses. virtio-sound in QEMU completes buffers
   immediately, so there the answer would be no better than today's estimate.
2. **Threads.** Decode ahead on one thread, feed audio from another, and a slow
   frame stops freezing input. It also turns on FFmpeg's frame and slice
   threading, which is where most H.264/HEVC throughput on more cores comes
   from. Blocked on the kernel ([`SMP_ROADMAP.md`](SMP_ROADMAP.md)).
3. **Assembly.** `--disable-asm` leaves every DSP routine in scalar C: that is
   the other large throughput factor. Needs `nasm` pinned in the toolchain.
4. **File dialog.** The Open dialog is a path field. A real one belongs in
   SXGUI, shared with Notepad.
5. **Fullscreen**, **playlists**, **subtitles** (`libavcodec`'s text and bitmap
   subtitle decoders, drawn over the frame), **album art** (the attached-picture
   stream is skipped today).
6. **Test vectors for the real codecs.** The smoke clips only exercise MJPEG,
   PCM and the resampler, because Python + Pillow is all the host has. H.264,
   VP9, AAC and Opus are verified by hand with real files until there is an
   encoder on the host, or pinned public test streams with a license that
   allows redistributing them ([`THIRD_PARTY_ADOPTION.md`](THIRD_PARTY_ADOPTION.md)).
