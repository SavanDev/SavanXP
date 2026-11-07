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
the codec port. The six follow-up stages below cover route selection, capture and the host's
HDMI controller. Broad codec compatibility remains outside this target.

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
6. **Follow-up consumers:** the SUN Sound panel and native recording API drive
   the six implementation stages below, with independent protocol/PCM assertions.

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

## Follow-up completion contract

The user requested all six stages, with real-hardware checks deferred until
implementation is complete. A software gate is never recorded as physical
validation. Every stage must preserve AC'97, virtio-sound, SUN playback and the
persistent system image. Progress is recorded against each stage; completing an
early stage does not close the whole roadmap.

| Stage | Audio service / SUN / Sound | OpenBSD Azalia port | Software closure gate |
| --- | --- | --- | --- |
| 1. Inventory | Stable device/connector ids, names, capabilities, effective state; native service without NIC | Enumerate valid analog routes; distinguish HDMI and disconnected firmware pins | Native inventory assertions; service and mixer on a machine without NIC |
| 2. Manual selection | Output selector; coordinated transitions and useful failure state | Rear line-out / front headphones, selectors, DACs, amps, EAPD; rollback on failure | Actual verb assertions and failed-transition rollback; SUN and visual checks |
| 3. Jack detection | Visible plug state and explicit automatic-switch policy | Detect presence and report changes before enabling policy | Fake hardware plug/unplug and unsupported/error cases; policy assertions |
| 4. Capture | SUN recording, input selector, gain, mute, meter and recording test | ADC routing, capture DMA, concurrent playback/capture | Known input PCM, independent gain/mute, duplex and ownership assertions |
| 5. Multiple devices / HDMI | Default playback and capture choices, availability and failures | Independent controllers/codecs; host Vega HDMI with compatible receiver | Multiple-device isolation; HDMI verbs/receiver state; playback regression |
| 6. Preferences / recovery | Stable application identities; saved devices, routes, policy and levels; restore after service restart | Recover driver failures and document target-specific quirks | Restart/restore, malformed preferences, failed recovery and persistence checks |

### Boundaries and evidence

The target is the ASRock B450/Ryzen/ALC892 analog device and Vega HDMI documented
in REAL_HARDWARE.md. USB audio, arbitrary codecs, suspend/resume and routing each
application to a different device are future work. Manual routing and the global
default are in scope. Application settings must use a stable identity rather
than transient stream ids or source ports.

Each new SUN capability has a `nativetest` assertion. Codec/controller tests link
the actual port code and check externally specified verb payloads and DMA rules;
expected values must not be changed merely because an implementation fails.
Recorded PCM is checked for actual sample content, duration and continuity.
Sound tests verify effective state and meaningful pixels after a successful
build; extracted image binaries must match the new build. The required
`sxgui-smoke` and Doom image-persistence checks remain gates.

Physical verification pending for the user after the six stages: rear/front
analog routing and jack changes; microphone/line-in and simultaneous playback;
Vega HDMI with a connected compatible receiver; restart/recovery on the target.
The earlier user-confirmed analog playback result remains valid only for that
previous implementation and does not certify these new features.
