# i386 device services and applications

This records completed milestone 5 against the pinned v0.4.0 reference, with
its verified legacy-device scope and the remaining milestone 6 boundaries.

## Hardware boundary

The native kernel links the reference PCI/CPUID queries, CMOS realtime clock,
AC'97 playback driver,
audio dispatcher, RTL8139 driver, NIC registry, IPv4/ARP/ICMP/UDP/TCP stack,
ACPI parser and power device. It also restores the reference PC speaker,
clipboard and serial device nodes. These use the same user ABI and descriptor
objects as the reference, with native user-memory copies and scheduler waits.
PCI DMA allocations use the physical allocator's contiguous-page API and its
supervisor RAM aliases; they are never passed a virtual address as a bus address.

BIOS assigns PCI INTx lines. The native adapter enables level triggering in the
PIC ELCR and permits shared handlers, including an ACPI SCI on the same line.
This is the legacy PIC path; AML/IOAPIC routing remains a milestone 6 parity
gap. The optional virtio integration is described in [I386_VIRTIO.md](I386_VIRTIO.md).
The ACPI RSDP comes from Multiboot2 tags 14/15;
physical ACPI addresses use the supervisor RAM alias, independently of the
framebuffer's device identity mapping. S5/reset keep the reference fallbacks.
The device clock copies the reference PIT-channel-2 TSC calibration and works
with interrupts disabled. User scheduling continues to use PIT channel 0.

## Descriptors and parked receives

Generic device opens retain their registered dispatcher, so `read`, `write`,
`ioctl`, close and polling reach the reference service. The process termination
path releases the audio session before closing handles, including a killed
owner. Permissions remain the access mask granted to the descriptor.

Sockets are reference `IoObject` instances. A pending native `read`/`recvfrom`
retains its descriptor, destination, peer pointer and deadline in the process,
then returns to the scheduler. Service retries receive into a bounded kernel
buffer and copy through the waiting process's page directory. A running task's
CR3 must never select the destination of another task's socket data. TCP's
`SO_RCVTIMEO`, UDP's explicit timeout, nonblocking errors and poll readiness
remain observable through the original SDK. Connect/send waits retain the
reference atomic kernel behavior while permitting device IRQs.

## Native application ABI

All native user code declares a 16-byte call-stack alignment. The SDK heap
header has 16-byte alignment as well, so its size remains a multiple of the
payload alignment after every split, merge and arena expansion. These are
contracts needed by compiler-generated SSE, including FFmpeg with assembly
optimizations disabled. `crt0` establishes the initial stack alignment.

The pinned FFmpeg port configures `x86_32`, uses `linker32.ld` for static
configure probes and includes the native bootstrap and 64-bit division helpers.
Once `ports/ffmpeg/build.sh` has produced an ELF32 engine, normal CMake builds
include the original Media Player sources. An existing foreign engine remains
data until rebuilt. The optional codec engine stays on the persistent disk,
without a duplicate in the resident CPIO, so installing it does not consume
another 7 MiB of the 32 MiB console test's RAM.
The optional Haxe experiment keeps ELF OSABI `0x53` and its native syscall range,
while sharing the ordinary process/descriptors/sections baseline. Its protected-
mode adapter preserves the info/log/display calls and identity across fork/exec.
Its static runtime needs its own short entry stub, not the POSIX embedded loader;
string instructions use four-byte protected-mode operations. `native-smoke`
checks `nativehello` and the original compositor/toolkit guest harnesses after
`subsystems/native/build.sh --all --install`.

Ccleste keeps the unchanged upstream engine and overlay, with native assembly
compilation and embedded-loader startup. Doom restores the reference PIE build
and shared `libsxmidi.so.0.4` instead of the temporary static synthesizer.

## Diskless storage

`./build.sh live-iso` creates `build/SavanXP-live.iso` with the internal staged
userland and a separate generated 128 MiB SxFS module. It never copies or resets
`build/disk.img`. The normal ISO keeps its persistent-disk flow and small memory
footprint. The LiveCD's `sxfs` Multiboot module uses the unchanged reference
ramdisk driver, with an unpartitioned ATA SxFS taking priority if attached.
Writes last only for the current boot. `storage-smoke` uses 256 MiB, attaches no ATA disk, checks `live0`,
reports a consistent filesystem and writes/reads a file in the guest. The
optional virtio disk/input profile is covered separately in milestone 6.
`partition-smoke` boots isolated MBR and GPT wrappers around the valid persistent
volume, checks `ata0p1` and exercises file I/O on each slice. `live-desktop` also
boots the full reference graphical session without an ATA disk and checks the
Program Manager taskbar glyphs. Device probing
retains reference order: virtio block, direct ATA, RAM image, then partition children. A
LiveCD module can therefore precede an ATA partition when both are present;
this existing reference limitation is not a claim of universal disk priority.

## Verification

The following gates passed when closing milestone 5. The service harness is
`tools/i386_services_test.py`; normal smoke runs retain logs and captures under
`build/smoke-logs`, and use snapshots or isolated copies of the disk.

| Gate | Evidence |
| --- | --- |
| `smoke services` | Device permissions and killed audio-owner recovery, clipboard, PC speaker, filesystem consistency, PCI/CPU discovery and CMOS clock; real RTL8139 packet counters, UDP delivery and reference TCP loss/reordering/deadline tests. |
| `smoke audio-smoke` | Original `audiod --selftest`, streamed AC'97 PCM and master gain: a steady captured level of 12,000 became 6,000 at 50% volume. |
| `smoke media-smoke` | Original WAV/MJPEG/AVI decode, seek and synchronization checks, Ccleste assets/music/120 simulation frames, and a decoded frame actually presented through the GPU. |
| `smoke doom` / `ccleste` | Changing game surfaces in the reference desktop; Doom FM music and silence after closing its window. The recorded final Doom residue was 0.000 s. |
| `smoke power-smoke` | Guest-requested S5, a fresh boot after reset, and shutdown from the firmware power-button SCI. |
| `smoke storage-smoke` / `partition-smoke` / `live-desktop` | Diskless RAM-volume boot and file operations, MBR/GPT slices, and a standalone graphical session. |
| `smoke native-smoke` | Native OSABI/info/log/display calls, Haxe containers/floats, and the original compositor and interactive toolkit guest harnesses. |
| `smoke`, `memory`, `shared-memory`, `windowd-smoke` | Process/fault isolation, exact reclamation, ELF32 libraries and the complete six-live-allocation heap workload on 32 MiB; WM capacity/reconnect/fullscreen and SxGUI editor/taskbar glyph/input checks. |

The 19 host tests passed. Doom's executable, real IWAD, configurations and save
fixture survived an isolated normal rebuild byte for byte. POSIX guest writes
survived a normal rebuild and cold boot, with the source image hash unchanged.
Extraction of the installed image matched the built bytes of 23 internal
programs, six libraries and five external applications, along with game assets.
A fresh build with `--no-test-apps` also booted the reference graphical session.

The audio checker reads captured PCM even when QEMU leaves zero WAV chunk
sizes. It measures steady tone levels separately from buffered stream-boundary
tails. Doom's stale-audio check distinguishes changing bass/DC-biased music from
repeated-sample PLC; its host regression still rejects a 500 ms stale plateau.
TCP's seven-second delayed read consumed one process tick out of 672 elapsed
ticks in an observed run, proving that the reader parked instead of spinning.

The resident CPIO strips unused static symbol tables as well as debug records;
its dynamic symbols and SXE resources remain. Disk executables/libraries retain
the exact built bytes and full debugging information. This recovers about
1.3 MiB of boot RAM after adding services, without shrinking the reference
heaptest's six live allocations or changing the heap growth algorithm.

## Remaining parity work

This closes milestone 5, not complete v0.4.0 parity. Milestone 6 covers SMP,
virtio integration and the remaining reference performance, power and
AML/IOAPIC mechanisms in emulation. AC'97 keeps the reference playback-only
contract; the separate virtio profile adds capture-ring reads. Full signals/job control, explicit
idle-process accounting and the reference boot-console/logo still need the
final version-parity review. Physical-machine bring-up, including laptop
suspend behavior, begins with the v0.4.2 target and does not block v0.4.0 or
v0.4.1 parity. The active target remains v0.4.0; see [I386_PORT.md](I386_PORT.md).
