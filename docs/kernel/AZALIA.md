# OpenBSD Azalia analog audio port

The x86_64 hardware target is the AMD `1022:1457` HDA controller and its
Realtek ALC892 codec on the [development host](REAL_HARDWARE.md). The backend
is a selective port, not the full OpenBSD audio subsystem. It supports one
active analog output and input at a time, selected from valid codec paths,
48 kHz stereo S16 PCM, independent software playback/capture gain and duplex
DMA. AC'97 and virtio-sound remain separate
backends. The audio service optionally switches analog jacks using the
presence inventory below. HDMI, power management and most
codec-specific quirks are not implemented.

## Source and license

The pristine inputs are `vendor/openbsd/azalia/{azalia.c,azalia_codec.c,azalia.h}`,
from OpenBSD commit `3f02b625e68a509b55a83343a6be40565e836cea`. The adjacent
`OPENBSD_COMMIT`, `SHA256SUMS` and `LICENSE` record that origin. The normal
build uses only local sources and never downloads from OpenBSD. The adapted
sources retain the NetBSD Foundation BSD-2-Clause copyright and disclaimer.
The [provenance registry](../third-party/THIRD_PARTY_PROVENANCE.md) records
where the derived code is compiled.

## Upstream-to-port map

| OpenBSD routine or data | SavanXP adaptation |
| --- | --- |
| `azalia.h` register/verb definitions, BDL and RIRB wire structures | `kernel/azalia/azaliareg.hpp`, extracted with unchanged values; no BSD audioio dependencies |
| `azalia_reset`, `azalia_set_command`, `azalia_get_response` | `kernel/azalia/openbsd.cpp`; same controller sequence and command encoding, bus callbacks in place of bus_space |
| `azalia_widget_init_audio`, `azalia_widget_init_pin` | `openbsd.cpp`; PCM inheritance and pin configuration/EAPD retained |
| `azalia_stream_reset`, `azalia_stream_start` | `openbsd.cpp`; complete cyclic BDL before RUN; interrupt enables omitted; one selected DAC replaces converter groups |
| `azalia_widget_enabled` | `openbsd.cpp`, from `azalia_codec.c`; retained for validating the codec graph |
| `azalia_get_ctrlr_caps`, `azalia_init_corb`, `azalia_init_rirb` | `kernel/azalia/codec.cpp`; bounded ring setup without autoconf, allocation or timeout(9) |
| `azalia_codec_init`, `azalia_widget_init_connection` | `codec.cpp`; function/widget enumeration and amplifier/PCM inheritance; bounded connection storage and independent encoded/expanded indices for ranges |
| `azalia_mixer_set` amplifier encoding | `codec.cpp`; both channels, input index, direction and mute bits retained; unity gain from amplifier offset, including mute-only amps |
| ADC discovery, `azalia_mixer_set` and `azalia_widget_init_pin` | `kernel/azalia/input.cpp`; bounded ADC-to-input graph, shared amp encoding and pin initialization; explicit transactional input selection |
| Input stream transport | `kernel/azalia/capture.cpp`; SavanXP-specific polling cursor and overrun checks around the ported stream/BDL routines |
| `azalia_configure_pci` AMD Family 17h snooping | `kernel/azalia.cpp`; PCI byte `0x42`, mask `0xf8`, enable `0x02` for `1022:1457` |

The routines in `openbsd.cpp` retain their upstream names and control flow.
C's `this` becomes `self`; endian conversions are identity on x86_64;
`bus_addr_t` becomes `uint64_t`. DMA reads are volatile. Pin-control verb
errors are propagated instead of ignored. OpenBSD acknowledges
RIRB status in its interrupt handler; this polled port acknowledges each
response and discards unsolicited entries without scheduling a timeout.

`azalia_port.hpp` replaces the BSD types with bounded structures and injectable
MMIO/delay operations. `kernel/azalia.cpp` owns PCI discovery, mapping,
physical allocations and the `audio::Backend` adapter. Host tests link the
same ported translation units with a simulated controller.

The general quirk table and OpenBSD mixer API are not compiled. The ALC892
CD-in/beep quirks do not participate in the selected rear line-out path.
Hardware mixer *configuration* is still required: input selection, muting
unused mixer inputs, unmuting the selected route, and powering the external
amplifier. Software gain does not replace those operations.

## Output selection

Every PCI class `04:03` controller is considered. A codec must expose a
connected, non-digital pin and a path to a stereo converter supporting PCM,
48 kHz and 16-bit samples. Disconnected firmware pins and HDMI pins are
excluded. Speakers precede line-out, which precedes headphones; firmware
sequence breaks ties. The walk records each connection index and programs
the selectors and input amps along that route. A valid firmware-selected
connection is tried first (the ALC892 front pin commonly selects mixer 0x0f /
DAC 0x05). It rejects cycles, paths
beyond ten nodes and connection lists beyond the bounded storage.

The endpoint catalog exposes every valid analog output with a stable id formed
from backend, PCI function, codec address and pin node. A manual selection stops
DMA, disables the old pin, detaches its converter, configures the new path and
restarts with a silence-filled buffer when playback was active. Initial setup
also disables other valid output pins. A failed setup restores every old
selector, amp and pin; a failed restart attempts the same rollback. If rollback
fails, the catalog has no effective selection and PCM is refused until a
successful selection. Kernel ownership prevents a control-only process from
rerouting another PCM writer; Sound sends the request to that writer (`audiod`).

The register space stays mapped for an abandoned controller because the
current VMM has no MMIO unmap. Command DMA is stopped before another candidate
reuses its ring storage. A controller that refuses to stop ends the probe;
its buffers stay allocated. DMA above 4 GiB is refused when GCAP lacks 64-bit
address support. Probe failure leaves lower-priority AC'97 available.

## Jack presence

Control inventory reads refresh analog pin sense at most every 250 ms. The
port uses the OpenBSD capability/firmware-override rules from `sense_pins` and
its `MI_TARGET_PINSENSE` read. Pins without presence capability or marked
`NO_PRESENCE` never receive sensing commands. Trigger-capable pins receive
`EXECUTE_PIN_SENSE` before `GET_PIN_SENSE`; this protocol detail was cross-checked
against Linux v6.12's [read_pin_sense](https://github.com/torvalds/linux/blob/v6.12/sound/pci/hda/hda_jack.c#L41-L58)
without importing Linux source. Register values still come from the pinned BSD
header. Trigger/read failures produce Unknown, not Absent. Unsupported,
Absent, Present and Unknown are distinct in the port and exposed as capability,
validity and connected flags in the endpoint ABI.

Sensing runs during inventory requests under the kernel lock, never as a
potentially timing-out verb in the BSP timer. The service polls inventory for
its optional policy; Sound polls it for connector status. Unsolicited events
remain disabled. Automatic preference and failure latching live in the service,
not in the hardware driver. They only target connectors on the chosen device.

## DMA and timing

All 32 descriptors have fixed 4096-byte buffers. CBL is their sum and LVI is
31 before RUN. Neither descriptors nor LVI change while running. User writes
populate the PCM bytes, with four periods of initial lead and at most eight
periods queued. Short writes append bytes rather than changing descriptor
lengths. A late writer resumes at least two periods ahead of the hardware;
a full queue drops input without blocking in an interrupt-disabled syscall.

The BSP timer polls LPIB under the kernel lock and clears completed periods
to silence. This prevents an idle producer from replaying old PCM on the next
cyclic lap. A missed polling interval longer than a full buffer stops the
stream, because LPIB cannot distinguish unobserved complete laps. Stream DMA
errors also stop it. This is a polling adapter to the kernel's current IRQ
infrastructure, not a claim that OpenBSD uses this scheduling model.

All reset and verb delays use calibrated `monotonic_ns()`, including while
interrupts are disabled. No CPU-iteration count is treated as milliseconds.

## Analog recording

The input graph walks from a stereo PCM ADC through mixers/selectors to a
firmware-connected microphone, line-in or auxiliary pin. Digital pins, cycles,
unsupported formats and oversized graphs are rejected. Each selected mixer
input is unmuted and the others muted. ADC gain is set to its advertised unity
offset; a microphone pin's boost is a separate input amp at unity, never an
output connection index. Mic pins receive supported VREF80/VREF50 bias through
the ported pin initializer; line-in receives no microphone bias. Input gain and
mute then operate on PCM in kernel memory before any samples reach userland.

On the ALC892 target the intended path is ADC 0x08 through mixer 0x23 to rear
mic 0x18, front mic 0x19 or line-in 0x1a. ADC 0x08's unity index is 0x10;
the mic boost's unity index is zero. Host-shaped tests assert these numeric
payloads, but physical sensitivity, board wiring and capture remain unverified.
No hardware boost control is exposed yet.

Capture uses input descriptor zero and stream tag 2, separately from playback's
output descriptor and tag 1. Its independent 32-period cyclic ring is fully
populated before RUN. A consumer waits one whole completed period behind DMA
and copies only complete stereo frames. An unobserved full lap, a consumer
overrun within one period of overwrite, invalid LPIB or DMA error fails capture
explicitly. DMA must stop before capture buffers can be reset or reused.
Selecting another input stops only capture, changes the path transactionally
and restores the old route on failure. Playback continues independently.

The native nonblocking START/READ/STOP ioctls use independent capture ownership;
READ returns EAGAIN when a complete period is unavailable. Gain/mute and held
peak state do not acquire PCM. The existing blocking read remains available
for the legacy recording test. The [audio service](../media/AUDIO_DAEMON.md)
shares a single kernel capture session among native subscribers.

## Verification and limits

- `azalia-test`: the actual ported routines against simulated MMIO, 2/16/256
  entry ring wrapping, an ALC892-shaped graph from the Linux host dump,
  disconnected/digital pins, non-default selector inputs, mute-only amps,
  short/long range lists, complete BDL before RUN and unresponsive codecs;
  front/rear routing, selector preference, exact headphone/EAPD verbs, failure
  rollback and loss of effective selection when rollback fails.
- `audio-wav-test`: independently generated valid/invalid captures. Silence,
  internal holes, wrong gain, wrong frequency, wrong rate and unequal channels
  must fail the audio oracle.
- `azalia-capture-test`: ADC/mic/line-in graph, unity and bias verbs, input
  rollback, independent stream tag/BDL, actual DMA sample copies over repeated
  wraps, partial-frame alignment and rejected overruns/deadlines/DMA errors.
- `hda-capture` and `virtio-capture`: known 400/600 Hz input at amplitudes
  12000/6000, exact full/half/muted samples, independent owners, direct and
  daemon duplex, two subscribers and counted slow-reader loss. The playback
  oracle requires at least 0.9 s of continuous 440 Hz audio at RMS 3000.
- `sound-capture-smoke`: three-second record/replay through the actual SUN
  panel, with a separate WAV oracle for stereo content, duration and continuity.
- `./build.sh smoke hda-stream` and `hda-stream-quiet`: recorded stereo PCM
  must contain 2.0–3.5 seconds of approximately 440 Hz audio, RMS near 12000 or
  6000 respectively, and no internal silent gap longer than 20 ms. Startup and
  shutdown silence are excluded. Serial success alone does not pass the smoke.
- `./build.sh smoke ac97-stream`, normal boot, host tests and the persistence
  regression protect existing paths.

These acceptance bounds are defined from `audiotest`'s 2.5-second square wave,
not from whichever output the driver produces. A failing bound requires a
root-cause investigation; changing it requires independent justification.

QEMU exercises the generic analog codec and DMA transport, not the physical
ALC892 amplifiers. On 2026-11-07 the user confirmed working playback on the
physical ASRock B450/Ryzen host after the port in `2ac6c50`. This is a manual
hardware result, separate from the QEMU and simulated-codec tests. No serial
log, application, jack selection or firmware settings accompanied the report.
It does not establish capture, HDMI playback or automatic jack switching.
