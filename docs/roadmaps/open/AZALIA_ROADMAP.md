# Azalia: OpenBSD HDA port for the x86_64 host

Status: all six follow-up software stages implemented: analog playback/capture,
SUN inventory, selection, jack policy, independent devices, persistent controls
and controller recovery. Software evidence is recorded below; follow-up physical
checks remain for the user. **Initial physical playback on the
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
the codec port. The six follow-up stages below cover analog routing, capture and multiple
devices. HDMI was explicitly deferred by the user on 2026-11-07; the pinned
OpenBSD source skips HDMI codecs. Broad codec compatibility remains outside
this target.

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
| 5. Multiple devices | Default playback and capture choices, availability and failures | Independent supported controllers/codecs; keep digital codecs separate | Multiple-device isolation, unavailable devices and playback/capture regression |
| 6. Preferences / recovery | Stable application identities; saved devices, routes, policy and levels; restore after service restart | Recover driver failures and document target-specific quirks | Restart/restore, malformed preferences, failed recovery and persistence checks |

### Boundaries and evidence

The physical analog target is the ASRock B450/Ryzen/ALC892 device documented in
REAL_HARDWARE.md. The user explicitly deferred Vega HDMI to another objective on
2026-11-07 after reviewing the pinned source's HDMI exclusion (`azalia.c`,
`azalia_init_codecs`). HDMI needs separate source/protocol and GPU dependency
analysis; an HDA PCM path alone will not be represented as working HDMI output.
USB audio, arbitrary codecs, suspend/resume and routing each application to a
different device are also future work. Manual routing and the global
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
restart/recovery on the target. Vega HDMI validation belongs to its separate
future objective.
The earlier user-confirmed analog playback result remains valid only for that
previous implementation and does not certify these new features.

### Follow-up software evidence

- Stage 1: `audio-offline` boots HDA with no NIC/network backend and runs the
  real native direct/daemon PCM, census, gain/mute, meter and singleton checks.
  `7cd7c34` removes the daemon's network dependency. Native assertions also
  verify stable endpoint ids, names and read-only enumeration.
- Stage 2: `azalia-test` verifies ALC892 rear/front paths, the front pin's fourth
  selector input and DAC 0x05, exact headphone/EAPD/amp commands, no playback
  writes to the pin's microphone boost, rollback and failed rollback. Native
  tests check owner-only selection and acknowledged success/unknown-id failure.
  `sound-smoke` checks the output dropdown, effective selected connector and
  all previous gain/mute/meters with two SUN sources (14 screenshots).
- Playback regressions: HDA full/half volume retain about 2.4 s at 440 Hz,
  RMS 12000/6000 and no internal silence; AC'97 and virtio streams pass.
  Native tests pass on AC'97 and HDA without NIC; the daemon sample assertions
  pass. The host suite is 24/24 and the isolated Doom persistence check passes.
  Extracted Sound/audiod binaries match the new image payload; Doom executable
  and WAD remain present. `sxgui-smoke` passes its eight glyph/scroll/taskbar
  screenshots. These results do not cover physical jack changes.
- Stage 3: the actual codec transport tests distinguish Present/Absent/Unknown/
  Unsupported, check trigger/read ordering and suppress unsupported/overridden
  pin sensing. `jack-policy-test` covers plug/unplug, unknown readings,
  controller isolation, digital/absent fallbacks, failed-transition latching
  and explicit manual mode. The native policy query/set assertions pass on
  HDA without NIC and on AC'97; the host suite is 25/25. HDA recorded PCM and
  the isolated Doom persistence regression still pass. Physical jack sensing
  and automatic analog routing remain unvalidated until the user's checks.
- Stage 4: `azalia-capture-test` checks the ALC892 ADC/mixer/microphone/line-in
  graph, numeric unity/bias verbs, transactional input rollback, real DMA
  copies over wraps and explicit deadline/overrun errors. `hda-capture` and
  `virtio-capture` check deterministic independent stereo input, full/half/muted
  samples, direct and shared native capture, duplex and independent owners.
  `sound-capture-smoke` passes its 17 screenshots and independently verifies
  2.977 s of replay at left RMS 12000 / 401.4 Hz and right RMS 6000 / 600.6 Hz.
  These checks use a generated ALSA file/null microphone, never the host mic.
  The host suite passes 26/26, full/half-volume HDA PCM retains RMS 12000/6000
  with no internal gap, AC'97/virtio playback and legacy capture pass, and the
  isolated Doom persistence regression passes. Extracted Sound/audiod/native
  test binaries match the new payload; Doom's executable and WAD remain present.
  The existing Sound controls pass 14 visual captures and `sxgui-smoke` passes
  its eight glyph/scroll/taskbar captures after the successful rebuild.
  Physical microphone sensitivity and duplex remain for final hardware tests.
- Stage 5: `audio-devices-test` checks all-driver discovery, multiple instances,
  independent defaults and failed/unavailable target preparation. `audio-devices`
  boots two HDA controllers and three codecs (addresses 0 and 2 share one),
  AC'97 and virtio-sound. Exact capture samples distinguish 400/600 Hz at
  12000/6000, 500/1000 Hz at 8000/4000 and 300/750 Hz at 10000/2000. Independent
  playback WAVs contain 2.164 s, 1.888 s and 8.414 s at RMS 3000 / about 440 Hz.
  Direct and daemon routing preserve the opposite direction; native capture
  receives an explicit old-source error and reopens on the selected input.
  The host suite passes 28/28, including input-meter freshness across measurement
  windows. The unchanged visual recording scenario exposed and then verified
  that meter fix: 17 captures and a replay WAV of 2.940 s at RMS 12000/6000,
  399.6/599.9 Hz. Sound's prior 14 control captures and SxGUI's eight glyph/scroll
  captures pass; isolated Doom persistence and installed binary comparisons pass.
  General driver recovery is recorded below under stage 6.
- Stage 6: `audio-preferences` restarts the real daemon and reconnects independent
  SUN and POSIX producers with different process/stream ids and native ports,
  without a NIC. It restores output/input ids, master 62/muted, capture 50/muted
  and application levels 43/muted and 37/muted. Flush acknowledgements require
  equal requested/durable generations. Exact capture distinguishes the third
  controller at 300/750 Hz and amplitudes 5000/1000, with no lost frames.
  The independent restored-output oracle records 2.193 s, RMS 1290, 439.1 Hz
  and no internal silence. Its 20 ms continuity and frequency bounds remain
  unchanged; malformed/gain/gap fixtures must still fail.
- Recovery host cases exercise actual port code: stuck RUN/CRST cannot modify
  DMA buffers, late same-codec replies and RIRB overflow cannot satisfy a new
  command, missing codecs remain unavailable and a verified reset restores the
  expected codec and desired pins. Timeout fixtures now require a cleared
  effective route and no more commands until reset, rather than claiming rollback
  over an unattributable response channel. No PCM assertion was relaxed.
- The final host suite passes 31/31, including strict preference parsing,
  failed write/sync/rename rollback, writer durability acknowledgements,
  writeback frontiers and memory-comparison guard pages. Isolated Doom
  installation/build persistence passes; extracted Sound/audiod/audiotest/native
  test binaries match their payloads and Doom's executable/WAD remain present.
  Pristine OpenBSD source hashes still match the pin.
- Final HDA full/half captures contain 2.359/2.363 s at RMS 12000/6000,
  439.9/440.2 Hz and no internal gap. HDA/virtio capture and independent
  multi-device, native/offline and daemon sample regressions pass. The final
  multi-device WAVs contain continuous intervals of 2.131, 1.843 and 8.371 s,
  all at RMS 3000 and about 440 Hz.
- Sound's existing control scenario passes 14 captures. Its recording scenario
  exposed GUI-paced replay starvation (375.3 Hz); the production fix uses a
  dedicated SUN playback thread and joins before reusing its PCM/stream/stack.
  The unchanged 17-capture scenario then passes with replay of 2.922 s at
  RMS 12000/6000 and 400.4/600.6 Hz. The final required SxGUI check passes all
  eight glyph/scroll/taskbar captures after the replay-thread change. All 14
  distinct QEMU scenarios pass, using the focused rerun for the repaired replay.

The software implementation is complete; physical follow-up checks in
[REAL_HARDWARE.md](../../kernel/REAL_HARDWARE.md#follow-up-audio-checks-after-the-software-objective)
are still pending. This roadmap remains open for those results. HDMI remains a
separate future objective.
