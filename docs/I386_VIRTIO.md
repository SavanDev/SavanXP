# i386 virtio integration

Milestone 6 reuses the drivers from the pinned SavanXP v0.4.0 reference,
commit `8ece07f4dae90812155a541951cbf8b3701bd92d`. The optional QEMU profile
selects virtio PCI block, network, sound, GPU, tablet and keyboard devices.
It runs the same ELF32 desktop, SDK libraries and applications as the default
legacy-device profile. Physical-machine testing remains deferred to the
v0.4.2 parity target; this work is verified in emulation.

## Memory and discovery boundary

PCI discovery runs before root-volume probing, so the reference virtio block
driver can mount the existing SxFS disk. Its priority remains above ATA, with
RAM images and partition slices keeping the reference probe order. The normal
build still updates the existing disk additively; selecting a QEMU controller
does not convert, reset or recreate its contents. Smoke runs use snapshots.

The retained `virtio_pci` transport allocates queue rings and payloads through
the physical allocator, keeping DMA addresses separate from supervisor RAM
pointers. No descriptors, feature negotiation or ring algorithms are replaced
with native-specific implementations. Its BARs use the same contiguous kernel
view arena as scattered graphics-section pages, through a new MMIO boundary.
MMIO leaves are supervisor-only, writable and uncached (PWT/PCD), and retain
the original offset within an unaligned first page. These mappings are shared
by existing process directories and inherited by new ones. They do not replace
user mappings or RAM aliases. RAM section mapping still requires allocator
addresses below the mapped first GiB; device BARs may be anywhere below 4 GiB.
Out-of-range spans and BAR sizes that cannot fit `size_t` are rejected.

The TSC clock is calibrated before device probing. This gives the reference
GPU's bounded waits a working deadline even during boot with IF clear. The
local APIC delivers scheduling ticks, with the PIT available as a fallback.

## Interrupts and deferred work

The initial native GPU used its legacy PIC INTx handler and acknowledged
its ISR byte there. Completion and scanout events use the reference
object/handle model and native waits. Milestone 6.1 initially deferred the
reference MSI-X path. The subsequent
[SMP bring-up](I386_SMP.md) supplies native local-APIC vectors, so the GPU now
prefers MSI-X and retains INTx as fallback. `devtest32 virtio` requires a
nonzero GPU IRQ notification count, checking delivery rather than only arming
a table entry. Both paths use the reference background completion work.

Block, network, sound and input keep their reference polling contracts. On
i386 the PCI transport initially disables legacy INTx delivery for each device;
the GPU enables INTx only after installing its fallback handler. MSI-X uses
the local APIC independently of that legacy-delivery bit. Without this distinction,
a polling device can hold a shared PIC line active while only the GPU's ISR
is acknowledged. An observed boot repeatedly returned to the first user EIP
without executing it; QMP register samples showed the GPU interrupt path
dominating execution. Device-level interrupt masking removed that starvation
without masking the GPU or ACPI SCI and without inventing interrupt handlers
for polling drivers. The x86_64 reference's existing transport policy is retained.

## Input and audio

Virtio keyboard scancodes feed the existing i386 ES/EN decoder and raw queue.
The retained input driver translates extended evdev keys, Pause and Print
Screen. Tablet events carry their absolute position and flag through
`/dev/mouse0`; wheel counts remain separate from pointer motion. PS/2 input is
ignored while the corresponding virtio device is ready, avoiding duplicate
delivery. Session boundaries reset pointer tracking as in the reference.

The graphics harness must send absolute QMP coordinates when using a tablet.
Its original relative PS/2 movement left the cursor at the screen corner:
Notepad painted correctly, but wheel events never reached the editor under
the pointer. Selecting the harness's existing absolute-pointer mode corrects
that test setup without changing the desktop or weakening its glyph checks.

Sound retains the reference 48 kHz stereo S16 playback/capture streams and
exclusive per-direction ownership. A WAV host sink verifies real output and
master volume, but QEMU's WAV driver provides no capture input. The RX test
therefore uses a separate boot with the null audio backend, which supplies
deterministic silence. The original `audiotest --record` checks eight complete
period reads through queue recycling and user copies. This establishes capture
transport, not microphone quality, a nonzero input signal or physical hardware.

## Reproducible checks

Build the normal disk and exact ISO before running the guests:

```sh
./build.sh iso
./build.sh smoke virtio-smoke
./build.sh smoke audio-record
./build.sh smoke sxgui-smoke --virtio
./build.sh smoke partition-smoke --virtio
./build.sh smoke
./build.sh smoke memory
./build.sh smoke shared-memory
./build.sh test
./tools/verify_doom_persistence.sh --wad /path/to/doom1.wad
python3 tools/verify_posix_persistence.py
```

`virtio-smoke` checks the boot backend markers and excludes an ATA/RTL8139
fallback. It runs SxFS consistency and guest writes, device permissions and
killed-owner recovery, actual NIC frame counters, UDP and the reference TCP
loss/reordering/deadline cases. Captured playback must contain the test tone;
the 50% master-volume stream must have half its steady amplitude. Its second
guest boot checks capture separately. `sxgui-smoke --virtio` checks actual
Notepad/editor and taskbar glyphs, wheel scrolling and scrollbar interaction.
`partition-smoke --virtio` checks both MBR and GPT slices on the virtio disk.

These gates passed for milestone 6.1. Playback contained 636,704 nonzero samples
in the observed service run, with steady amplitude changing from 12,000 to
6,000 at 50% master volume. A separate null-backend boot completed all capture
reads. The graphical run passed all six original editor/taskbar glyph and
wheel/scrollbar captures. Both virtio MBR/GPT roots passed consistency and
write/read checks. The default full smoke, 32 MiB memory/shared-memory gates,
19 host tests, Doom persistence and POSIX rebuild/cold-boot persistence passed.

## Remaining milestone 6 work

Local xAPIC/MSI-X, concurrent AP scheduling and PS/2/SCI I/O APIC routing are
covered by the [SMP gates](I386_SMP.md). The legacy RTL8139's AML routes are
covered by the [device service gates](I386_SERVICES.md).
The first-GiB RAM ceiling and lack of PAT write combining remain. Full signals/
job control, ASLR and the reference boot-console/logo remain in
the final feature review. Passing the virtio gates closes only that part of
milestone 6; the active version target remains v0.4.0.
