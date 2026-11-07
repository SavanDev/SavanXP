# OpenBSD Azalia playback port

The x86_64 hardware target is the AMD `1022:1457` HDA controller and its
Realtek ALC892 codec on the [development host](REAL_HARDWARE.md). The backend
is a selective port, not the full OpenBSD audio subsystem. It supports one
analog output, 48 kHz stereo S16 PCM, and software master gain. AC'97 and
virtio-sound remain separate backends. HDMI, capture, jack switching, power
management and most codec-specific quirks are not implemented.

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
the selectors and input amps along that route. It rejects cycles, paths
beyond ten nodes and connection lists beyond the bounded storage.

The register space stays mapped for an abandoned controller because the
current VMM has no MMIO unmap. Command DMA is stopped before another candidate
reuses its ring storage. A controller that refuses to stop ends the probe;
its buffers stay allocated. DMA above 4 GiB is refused when GCAP lacks 64-bit
address support. Probe failure leaves lower-priority AC'97 available.

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

## Verification and limits

- `azalia-test`: the actual ported routines against simulated MMIO, 2/16/256
  entry ring wrapping, an ALC892-shaped graph from the Linux host dump,
  disconnected/digital pins, non-default selector inputs, mute-only amps,
  short/long range lists, complete BDL before RUN and unresponsive codecs.
- `audio-wav-test`: independently generated valid/invalid captures. Silence,
  internal holes, wrong gain, wrong frequency, wrong rate and unequal channels
  must fail the audio oracle.
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
ALC892 amplifiers. The hardware gate remains an actual SavanXP boot, a log
naming controller/codec/pin/DAC, and audible output from the rear green jack.
Linux inventory and simulated codec tests do not satisfy that gate.
