# Azalia: OpenBSD HDA port for the x86_64 host

Status: selective playback port implemented; host tests, QEMU recorded-PCM
acceptance and persistence regression pass. **Physical playback on the
Ryzen/ALC892 host was confirmed by the user on 2026-11-07.** The legacy AC'97
target remains separate.

The target is the ASRock B450/Ryzen host in
[REAL_HARDWARE.md](../../kernel/REAL_HARDWARE.md): AMD HDA `1022:1457` with
ALC892 `10ec0892`, initially through the rear green line-out. The Vega HDMI
controller `1002:aaf8` must not claim that analog path.

## Rules and scope

Start with pinned OpenBSD sources, preserve their notices, and keep a map from
upstream routines to local adaptations. The source pin, actual port scope,
DMA model and known limitations are in [AZALIA.md](../../kernel/AZALIA.md).
There is no claim of a wholesale OpenBSD driver import: autoconf, bus_dma,
interrupt routing and audio(9) are adapted or replaced explicitly.

Initial scope is analog stereo S16 at 48 kHz. Software master volume reuses
`audio::`; hardware amplifier setup and connection selection remain part of
the codec port. Capture, HDMI output, jack switching and broad quirk coverage
are later phases, not prerequisites silently assumed to work.

## Acceptance gates

1. **Source provenance:** pristine `azalia.c`, `azalia_codec.c`, `azalia.h`,
   pinned commit, hashes, license, compiled-source map and registry entry.
2. **Controller/codec tests:** supported ring sizes and wrap, command timeouts,
   real-host-shaped analog graph, disconnected/digital pin exclusion,
   amplifier payloads and non-default selector inputs. Negative cases must
   refuse a binding or fail a command.
3. **QEMU playback:** `hda-stream` and `hda-stream-quiet` pass the independent
   recorded-PCM assertions. A WAV file's existence or a producer's success
   token is insufficient. The oracle itself must reject defective captures.
4. **Regressions:** normal build, host tests, `ac97-stream`, `sxgui-smoke` and
   external Doom executable/asset persistence. Confirm the rebuilt executables
   are the ones installed in the image.
5. **Physical host:** boot SavanXP, identify AMD HDA/ALC892 in the log, and hear
   `audiotest --stream` on the rear green line-out. Record firmware settings,
   selected path and result. Do not mark this gate complete from QEMU or Linux.
6. **Extend only for an observed consumer:** headphone routing/jack switching,
   capture or another codec, with appropriate assertions and physical checks.

## Results

- Source pin: `3f02b625e68a509b55a83343a6be40565e836cea`.
- Host transport/codec tests and audio-oracle negative tests pass.
- QEMU `hda-stream`: 2.397 s, RMS 12000, 440.1 Hz, maximum internal gap 0 ms.
- QEMU `hda-stream-quiet`: 2.409 s, RMS 6000, 440.0 Hz, maximum internal gap 0 ms.
- Host suite: 23/23 pass; `ac97-stream` and `sxgui-smoke` pass; isolated Doom
  persistence regression passes. The Start menu visual scenario also passes.
- Physical playback on the ASRock B450/Ryzen host: **user-confirmed working on
  2026-11-07** after the port in `2ac6c50`. No serial log, tested application,
  jack selection or firmware settings were supplied with the confirmation;
  those details remain unrecorded. Capture, HDMI and automatic jack switching
  are not covered by this result.
