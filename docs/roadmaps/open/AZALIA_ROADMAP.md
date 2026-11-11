# Azalia: OpenBSD HDA port for the x86_64 host

Status: all six follow-up software stages implemented: analog playback/capture,
SUN inventory, selection, jack policy, independent devices, persistent controls
and controller recovery. Software evidence is recorded below. **Physical
playback on the Ryzen/ALC892 host was confirmed by the user on 2026-11-07, and
microphone record/replay plus application playback on 2026-11-08.** The roadmap
stays open for two things, both listed in
[Audit, 2026-11-11](#audit-2026-11-11): the physical checks nobody has run yet
(connector routing, jack switching, line-in, duplex, restart and recovery) and
the software backlog the audit found. The legacy AC'97 target remains separate.

The target is the ASRock B450/Ryzen host in
[RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md): AMD HDA `1022:1457` with
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
this target; the other controllers and codecs are package HW-6 in
[REAL_HARDWARE.md](../../hardware/REAL_HARDWARE.md#hw-6--hd-audio-beyond-the-development-host).

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
RYZEN5_1600_VEGA56.md. The user explicitly deferred Vega HDMI to another objective on
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

Physical verification was deferred until after the six stages. What has been
confirmed since, and what is still owed, is tracked in
[Audit, 2026-11-11](#audit-2026-11-11). Vega HDMI validation belongs to its
separate future objective.
The user-confirmed playback result of 2026-11-07 is valid only for the initial
port and does not certify the follow-up features.

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
[RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md#follow-up-audio-checks-after-the-software-objective)
are still pending. This roadmap remains open for those results. HDMI remains a
separate future objective.

Physical follow-up, 2026-11-08: the user confirms microphone recognition,
recording and audible replay in Sound. The input pin, gain and firmware settings
remain unrecorded. Doom and Celeste did not appear or play on that host; the
shipped external ELFs still contained the pre-native, UDP-only SDK, explaining
why network-enabled QEMU masked that packaging gap. Rebuilt external consumers
pass `audio-ports-offline`: 11 screenshots verify each game's mixer row, and
separate playback intervals record normalized RMS 0.1281 (Doom) and 0.1174
(Celeste), with the NIC absent. All 34 original game-data files remain unchanged
after executable installation and the normal build. This result does not certify every analog
connector, automatic jack policy or physical controller-fault recovery.
The user subsequently confirmed correct operation with the updated games in
the ISO from `22ae69e`; basic physical application playback and microphone
record/replay are therefore user-verified.

## Audit, 2026-11-11

The driver (`kernel/azalia.cpp`, `kernel/azalia/`), its tests and this roadmap
were read against the pinned OpenBSD source at `8120901`.

**Verdict.** The six stages are implemented as described and nothing found
blocks the Ryzen/ALC892 target. No stage claims more than the code does. The
findings below are robustness, portability and debt items, plus the physical
checks that were never run. The "Results" and "Follow-up software evidence"
sections above are a dated log; their host-suite counts (23 to 31) are the
counts of that day.

Re-run for the audit:

- Vendored `azalia.c`, `azalia_codec.c` and `azalia.h` still match `SHA256SUMS`.
- `./build.sh test`: 37/37, including `azalia-test`, `azalia-capture-test`,
  `jack-policy-test`, `audio-devices-test` and the preference tests.
- `./build.sh smoke hda-stream` and `hda-capture` pass (`drops=0 underruns=0`
  on playback; exact capture PCM, gain/mute, duplex and ownership).
- Not re-run: `hda-stream-quiet`, `audio-devices`, `audio-preferences`,
  `audio-offline`, `audio-ports-offline`, the Sound visual scenarios and the
  Doom persistence check. No code changed, so their recorded results stand.

Confirmed while reading, because the documents depend on it: the BSP timer
ticks at 1000 Hz and takes the kernel lock on interrupt entry, so `azalia::poll`
and the syscall paths never run concurrently; every verb failure path poisons
the transport instead of guessing attribution; no DMA buffer is rewritten
before its stream is proved stopped.

### Physical checks still owed

Confirmed so far: playback (2026-11-07), microphone recognition, record and
replay, and Doom/Celeste playback (2026-11-08). None of those reports recorded
the serial log, connector, pin, gain or firmware settings. The procedure is in
[RYZEN5_1600_VEGA56.md](../../hardware/RYZEN5_1600_VEGA56.md#follow-up-audio-checks-after-the-software-objective).

| # | Check | Covers |
| --- | --- | --- |
| P-1 | Rear line-out and front headphones selected by hand, one at a time | Stage 2 on real amplifiers and EAPD |
| P-2 | Plug and unplug with the automatic policy on, then off | Stage 3: real pin sense, Unknown preserving the route |
| P-3 | Line-in and the second microphone jack; input gain and mute; recording while playing | Stage 4 beyond the one microphone heard |
| P-4 | Restart the audio service and reopen clients; preferences restored | Stage 6 on the target |
| P-5 | Whether a controller fault ever occurs, and what the log says if it does | Stage 6 recovery, which only has simulated coverage |
| P-6 | The boot log's `azalia:` lines, the connector and pin used, firmware audio settings | The details missing from both confirmations |

### Software backlog

None of these is scheduled. Each is small unless noted, and each needs its own
test before it is called done.

| # | Finding | Where | Consequence |
| --- | --- | --- | --- |
| AZ-1 | `azalia_configure_pci` is ported as one row (`1022:1457` snooping). Upstream also clears the traffic-class select (`HDTCSEL`) on **every** controller, including this one, and has the ATI SBx00, NVIDIA MCP and Intel rows | `probe()` in `kernel/azalia.cpp` | Harmless on the target as heard. The other rows are HW-6; a controller without its row may attach and play silence or noise |
| AZ-2 | A converter with a format override and no PCM makes `probe_codec` fail the whole codec. Upstream `azalia_widget_init` ignores that result and keeps the codec | `kernel/azalia/codec.cpp`, the `azalia_widget_init_audio` call | A codec with one non-PCM converter gets no analog audio at all. The widget should be disabled, not the codec. Transport errors must still fail the probe |
| AZ-3 | Busy waits under the kernel lock: the legacy blocking `capture_read` spins for up to 250 ms, and a stream stop on a `poll()` error path can spin for up to 150 ms inside the BSP timer | `Device::capture_read`, `Device::poll` → `stop_dma` | The whole system stalls for that time. The native nonblocking capture path is not affected. Belongs with the waiting rule in [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) |
| AZ-4 | Selecting the connector that is already selected stops and restarts DMA before noticing nothing changed | `Device::select_endpoint`, `Device::select_input` | An audible gap in playback; a running capture restarts with a fresh cursor and no error to its reader |
| AZ-5 | Probe accounting: a controller that fails reset or has no analog path keeps one of the four slots; PCM pages leak when a codec is rejected after allocation; a codec whose output setup fails is dropped even when its capture path is valid | `probe()` in `kernel/azalia.cpp` | Bounded and only on unusual machines. A host with several display-audio functions can run out of slots before the analog controller |
| AZ-6 | No hardware gain: output amplifiers sit at their 0 dB offset, the microphone boost at unity, and neither is exposed | `amp()` in `codec.cpp`, `configure_input_path` in `input.cpp` | A quiet microphone can only be raised in software, with its noise. Needs P-3's measurement first |
| AZ-7 | The adapter has no host test. `azalia-test` and `azalia-capture-test` link the port files, not `kernel/azalia.cpp` | `CMakeLists.txt` | Queue arithmetic in `submit`/`poll`, the rollback glue in `select_endpoint` and `ensure_controller` are covered only through QEMU |
| AZ-8 | Polling only: no stream or RIRB interrupts, unsolicited responses off, jack sense refreshed only when someone asks for the inventory | `kernel/azalia.cpp`, `openbsd.cpp` | Works at a 1 ms tick. Interrupts wait for the IRQ work in [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) |
| AZ-9 | Power: codecs without an analog path (the Vega's HDMI codec) are left in D0 after the probe, where upstream sets unused codecs to D3. No suspend or resume | `probe_codec`, `probe()` | Wasted power, no functional effect. Suspend/resume is already out of scope above |
| AZ-10 | A controller without 64-bit DMA is refused when the allocator returns memory above 4 GiB; there is no way to ask for low memory | `probe()`, `capture_configure` | Correct refusal, but on such a controller audio depends on allocation luck. Needs the DMA contract in [DRIVERS_ROADMAP.md](DRIVERS_ROADMAP.md) |

Unchanged and still outside this roadmap: HDMI and DisplayPort audio, the
codec quirk table and GPIO quirks, formats other than 48 kHz stereo S16, USB
audio and per-application device routing.

### Closing this roadmap

It moves to `done/` when P-1 to P-4 have a recorded result (pass or a filed
defect) and AZ-2, AZ-3 and AZ-4 are fixed with tests. AZ-1 and the codec work
close under HW-6; AZ-8 and AZ-10 close under the drivers roadmap; AZ-5, AZ-6,
AZ-7 and AZ-9 may be carried to whichever document owns audio next.
