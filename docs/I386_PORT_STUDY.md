# SavanXP on 32-bit x86: a porting study

## Purpose and reference

This is the evolving technical account of bringing SavanXP to 32-bit x86.
It explains the problems, the mechanisms behind them and the decisions that
shape the port, at the level of a concise operating-system internals study.
The objective is parity with **v0.4.0**, preserving its appearance, behavior
and internal design with the smallest architecture-dependent changes. The
reference commit is `8ece07f4dae90812155a541951cbf8b3701bd92d`. Only after that
target is reached does the comparison move to v0.4.1.

[I386_PORT.md](I386_PORT.md) defines the parity policy and roadmap. This study
explains how the branch got to its current state and why unfinished choices
remain provisional. Subsystem documents carry the detailed current contracts.

## Reading the evidence

The initial chapters below are retrospective, reconstructed from repository
history, implementation and recorded verification. Commit identifiers can be
inspected with `git show <commit>`. Verification reported for an earlier
milestone describes that revision; it is not a fresh test of every later tree.
The documented commands identify repeatable checks without depending on a
particular developer's temporary logs.

Future chapters should follow the same thread: problem and context, relevant
mechanism, alternatives considered, decision and rationale, observed evidence,
then limitations and consequences for parity. Record unsuccessful approaches
when they explain a decision, and identify hypotheses as such. Extend the
account as work lands; explain superseding decisions instead of erasing their
history. Keep routine implementation detail in the linked code and commits.

## 1. Establishing a boot path without long mode

The original kernel enters a 64-bit address space through Limine. The reference
Compaq Evo N1020v has a 32-bit Pentium 4 without long mode. Keeping that boot
contract would prevent execution before any filesystem or desktop code could
be tested. The [hardware analysis](REAL_HARDWARE.md) describes this boundary.

The first bootable port therefore used GRUB2 and Multiboot2 to enter a protected
mode kernel. A flat GDT, explicit stack and COM1 output made the handoff
observable. Initial paging used 4 MiB PSE pages over the first GiB, giving the
new kernel a simple address map before a full memory-manager port existed.
This is a concrete CPU/boot adaptation; the simple map is a bring-up facility,
not the final replacement for the reference memory model.

Two failures showed how early ABI mistakes can resemble a broken bootloader.
Reloading a segment selector through AX overwrote part of the incoming EAX
value. Separately, placing alignment padding between pushed arguments and the
call made the C callee read the wrong stack slots. Preserving the handoff
register and putting padding before the argument pushes corrected the actual
entry contract rather than compensating inside the callee.

Commit `be4de577` records the fixes and a QEMU boot reaching `I386 BOOT OK`.
The harness also required the paging message, so printing a final token alone
could not establish that paging had been enabled. This established protected
mode execution, not userland or hardware parity.

## 2. Reusing storage while preserving the disk

A new CPU target did not require a new filesystem. The ATA PIO, block,
partition and SxFS modules already expressed the needed storage behavior, so
the port reused them. The useful boundary was between machine-specific memory
operations and architecture-independent filesystem semantics.

Commit `b8c781a8` records two required adaptations. The runtime could not use
`rep movsq` in 32-bit mode; its widest copy unit became architecture-dependent.
The VFS initialization path also assumed an initramfs trailer would mark the
tree ready. The i386 kernel needed an empty ready VFS before attaching its disk,
so initialization without an archive was supported while truncated archives
remained errors. Mounting exposed a separate resource requirement: filesystem
cache reconstruction needed more bootstrap stack space.

The guest mounted the existing SxFS image through ATA and reported
`SXFS ROOT OK`. Snapshot-backed boot checks protected the original image during
experiments. The principle carried into later build work: architecture changes
must preserve unrelated applications, WADs, configuration and saved games.
Mounting existing data did not establish that its ELF64 programs could execute
on a 32-bit CPU.

## 3. Making execution and graphics observable

ELF32 ring 3 execution, keyboard delivery, a PIT clock and `sh32` provided an
interactive way to inspect the port before the full shell could run. The
history is recorded by `80e0885f`, `fec419ad`, `ed6dfd64`, `37f81147` and
`92666562`. These programs reduced the dependencies needed to test syscalls;
they did not establish equivalence with the reference POSIX environment.

The next problem was to verify an actual graphics path. A Multiboot2 framebuffer
had to be validated and mapped, console painting had to respect video
ownership, and keyboard and mouse bytes had to be distinguished on the shared
8042 port. A cursor also had to restore the pixels underneath it. A successful
program exit would reveal none of those failures.

Commit `daaf68b5` records the minimal desktop milestone. IRQ entry was corrected
to preserve EAX before extracting the IRQ number; mouse delivery needed the
slave PIC cascade unmasked and dispatch based on the controller's AUX status.
Diagnostic GPU opens were removed, and video acquisition suppressed console
painting over the client's image.

The visual harness checked console clearing, text glyphs derived from the
built ELF32, mouse buttons, cursor restoration and edge clamping. Installed
programs were compared with rebuilt artifacts, avoiding a false success from
an old executable left in the disk after a failed build. The resulting desktop
is still a test client: its panels do not provide the reference window manager,
taskbar or application interactions. Restoring those remains a parity task.

## 4. Giving each process its own interrupted state

Running one program at a time could not establish that processes were
independent. Preemption requires preserving everything another task can alter:
general and segment registers, flags, stacks, address-space selection and
x87/SSE state. The chosen execution model uses PIT-driven preemption so a
CPU-bound application can be interrupted without yielding voluntarily.

Commit `4fca1847` introduced private stacks and maps, an explicit shared
IRQ/syscall frame and eager FXSAVE/FXRSTOR. Assembly returns through the frame
selected by the scheduler. Passing the frame directly removed dependence on a
guessed offset from the TSS stack. Blocking requests retain their saved return
state, allowing another user task to run without retaining a sleeping kernel
C++ continuation. `init32` owns launch/restart policy, replacing the earlier
kernel-controlled chain of programs.

A Debug aggregate temporary exposed a kernel stack overflow during this work.
That failure led to bounded compiler-frame checks, stack canaries and a linker
guard against kernel storage reaching the user window. It demonstrated that
source-level initialization can have a stack cost larger than its apparent
final destination.

The regression ran two CPU-bound workers at the same virtual addresses with
different memory and register patterns. It required overlapping execution and
multiple recorded PIT preemptions, as well as correct registers, x87/SSE state
and process lifecycle results. [I386_PROCESSES.md](I386_PROCESSES.md) describes
the mechanism and the checks in detail.

The six static image slots reserve 96 MiB, kernel execution remains atomic
with respect to scheduling, and a user exception still halts the guest. Those
are explicit prototype limits. The process milestone supplies evidence of
preemption and ownership, not parity with the reference scheduler, allocator,
fault isolation or complete process API.

## 5. Making the branch's normal workflow exercise the port

Initially the build graph supported both architectures while normal commands
still selected x86_64. That allowed development alongside the source system,
but later made a successful ordinary build an ambiguous signal about the port.
The branch was designated i386-only, with the original system retained on
`master`; commit `ce6feac2` records that change and retires the dedicated
64-bit boot implementation from this branch.

The change affected more than a default compiler flag. `run` needed a GRUB ISO
and IDE disk rather than EFI staging. A separate CMake cache prevented old
architecture settings from selecting the wrong graph. Normal image staging
moved to the persistent `build/disk.img`, using additive synchronization to
preserve existing content. Retained ELF64 binaries remain data until rebuilt.

The external builder also needed the i386 user address and runtime helpers.
The port gained i386 `setjmp`/`longjmp` and signed 64-bit division helpers.
Doom was built as static ELF32 so its installation/persistence regression could
continue without the unported dynamic loader. That static linkage is temporary:
v0.4.0 parity requires restoring the reference shared-library integration.

Recorded verification includes 17 host tests, the full i386 smoke, an isolated
normal launch executing an external ABI probe, and Doom binary/WAD/config/save
preservation across a rebuild. A before/after comparison preserved 123 existing
files byte for byte, excluding the intentionally updated internal `sysinfo`.
These are build, ABI and persistence results; they do not demonstrate Doom
gameplay or audio on the i386 guest.

## 6. Fixing the comparison target

After the i386 workflow became the default, the branch objective was clarified:
run v0.4.0 with minimal visual and internal differences, then advance to
v0.4.1. The moving tip of `master` is therefore not the current specification.
The pinned reference and completion criteria are maintained in
[I386_PORT.md](I386_PORT.md).

This changes how intermediate success is evaluated. A smaller shell or a
decorative desktop can make another mechanism testable, but cannot permanently
replace the original behavior. Future work must explain which v0.4.0 subsystem
is being adapted, what architectural constraint requires a difference and what
evidence shows that the difference has been contained. The present branch still
has substantial parity gaps; no chapter here claims that v0.4.0 is complete.

## 7. Fault containment and allocator-backed user pages

The first ordered parity milestone exposed two related limits of the earlier
bring-up: one #UD or #PF stopped the entire guest, and the process table occupied
96 MiB before any user program ran. The v0.4.0 reference already supplied the
physical free-region allocator in
[`kernel/physical_memory.cpp`](../kernel/physical_memory.cpp), page ownership and
teardown in [`kernel/vmm.cpp`](../kernel/vmm.cpp), and the
`terminate_current_from_exception` convention in
[`kernel/process.cpp`](../kernel/process.cpp). The adaptation reuses that allocator
and those lifecycle rules. It does not import the reference's four-level paging
instructions or 64-bit virtual addresses, which cannot run in protected mode.

### Giving GRUB memory the reference allocator's contract

Keeping the static images and merely catching exceptions would leave the memory
half of the milestone unresolved. Allocating another contiguous 16 MiB block for
each process would remove BSS reservations but retain unnecessary consumption
and a fragmentation dependency. The selected path follows the reference VM:
allocate individual 4 KiB physical pages for ELF load segments, the stack and
page tables, then return them through the same allocator on teardown.

The reference boot adapter received a map that distinguished executable/modules
from usable memory. GRUB's E820 usable ranges still contain the loaded kernel,
Multiboot information and modules. Passing them unchanged would eventually
allocate over live kernel state. The new architecture adapter first clips usable
RAM to the mapped first GiB and splits out those objects and the framebuffer.
The shared allocator retains its free-region/first-fit/coalescing algorithm;
additional checks reject wrapped address/count arithmetic, unrepresentable
aliases, overlapping usable ranges and duplicate frees. Allocation failure clears
the output rather than exposing an earlier allocation.

A zero direct-map offset was considered, but a process replaces the identity
mapping at `0x08000000..0x09000000`. A kernel pointer into physical RAM in that
range would then address the caller's user memory. Reserving those physical
16 MiB forever would avoid the collision at the cost of usable RAM. Instead,
the kernel installs supervisor-only aliases of physical 0..1 GiB at
`0x40000000..0x80000000`. The allocator can keep its nonzero direct-map contract,
and loading a child never replaces the parent's CR3. Device identity mappings
are rejected if they would overwrite this alias window; supporting such a
framebuffer requires a later device-mapping adaptation. RAM above 1 GiB remains
unallocated rather than producing inaccessible pointers.

### Permissions and the meaning of a user pointer

PSE remains appropriate for shared kernel identity/direct mappings. User pages
now use ordinary two-level, 4 KiB non-PAE tables: PT_LOAD determines write access,
page-sharing segments union their permissions, fresh pages start zeroed, and
unused addresses remain absent. The eager 64 KiB stack has an unmapped guard
below it. The loader validates all segment bounds before allocating, and failed
loads destroy even partially populated page tables. The table still has six
process slots; that metadata bound is independent of physical exhaustion.

An address inside the old virtual window no longer proves that it is safe to
access. Syscall validation walks the mapped pages and checks write permissions
for destination buffers. Bad or read-only buffers return `EFAULT` before the
kernel touches them. This is essential because CR0.WP applies to supervisor
writes too: failing to validate a destination would turn a user error into a
fatal kernel page fault. The low kernel map, RAM aliases and framebuffer aliases
remain supervisor-only. NX is unavailable, so this milestone cannot make data
non-executable or claim the reference's complete W^X policy.

The old physical-alias `memcpy` also assumed adjacent user addresses implied
adjacent physical addresses. That assumption ends with page allocation. Loader,
blocked input and wait-result writes split at page boundaries. The regression
uses a large BSS helper and a wait status straddling a 4 MiB page-table boundary:
a page-table allocation can separate the backing frames even when loading an
otherwise contiguous image. Both blocked and already-zombie waits must preserve
the mappings and return the expected result.

### An exception is a process exit only when it came from user mode

The exception entry previously overwrote EAX to obtain the vector, retained user
data selectors and halted unconditionally. It now saves all registers, installs
kernel data selectors, clears DF and supplies an error word for exceptions whose
hardware frame has none. This dedicated frame includes an extra error word;
it is deliberately not treated as a resumable IRQ/syscall frame. The saved CS
establishes whether the failure occurred at ring 3.

A user exception reports its vector, error, EIP and CS, plus CR2 for #PF, and
uses exit status `128 + vector`, as in v0.4.0. Normal exit and exceptional exit
share waiter notification, orphan handling and GPU ownership release. The
scheduler activates another CR3 before freeing the dead task's image/tables;
the fixed kernel stack remains valid until assembly restores the selected task's
frame. Zombies retain their status and slot, not their user pages. A kernel
exception remains fatal; NMI, double fault and machine check also stay fatal
even if they interrupted user code.

### Evidence and remaining parity limits

`./build.sh smoke` passed the existing CPU/FPU context checks, shell lifecycle,
and desktop glyph/mouse/cursor assertions with the new memory implementation.
Its process regression also verifies #UD, divide error, #GP from `cli`, and #PF
for a null pointer, an absent user hole, low kernel memory, RAM aliases, text
writes and the stack guard. A child faults with null data selectors and DF set;
another faults while owning the GPU. Wait statuses, surviving siblings, GPU
reacquisition and exact free-page restoration are required, and the host types
a faulting foreground command and verifies that the shell returns to its prompt.

The initial host assertion expected a null read to have #PF error 4 (absent user
page). The actual error was 5: address zero is present in the supervisor-only
bootstrap identity map. The test was corrected to verify the permission fault,
not to change the mapping. Error 5 is also required for kernel/RAM aliases,
7 for a text write, and 6 for a write into the absent guard.

`./build.sh smoke memory` passed on 32 MiB RAM. A 12 MiB BSS helper forces an
`ENOMEM` partway through loading another image while a sibling remains alive.
The regression retries failed loads without changing the free-page count,
collects successful children, and repeats the cycle with exact baseline page
restoration. On the normal 1 GiB machine the same workload instead reaches the
six-slot `EAGAIN` limit. `./build.sh test` passed all 18 host tests, including
allocator exhaustion, fragmentation/coalescing, reuse and arithmetic boundaries.
`./tools/verify_doom_persistence.sh --jobs 4` passed the external executable,
WAD/config/save preservation check after a normal isolated rebuild.

A separate debugger experiment checked the opposite boundary without modifying
the disk: boot the same ISO without a disk using QEMU `-S -s`, break at
`i386::process::start()`, replace its first instruction in guest memory with
`ud2`, then continue to a breakpoint at `i386::cpu::halt_forever()`. Serial reports
vector 6, CS `0x08` and `kernel: fatal exception`. Repeating with a load from
unmapped `0x80000000` reports vector 14 and the same fatal path. These are
observations of injected kernel failures, not automatic coverage of every
possible kernel exception.

This completes the roadmap's fault-containment/memory-foundations milestone.
It does not close v0.4.0 parity: the six-slot table, fixed 16 MiB virtual window,
eager small stack and 1 GiB physical ceiling remain explicit temporary bounds.
Section objects, heap growth and reference stack-demand behavior still need
porting. The next ordered dependency is the reference descriptor/POSIX base;
SxGUI, the original shell and the full desktop remain later consumers.

## 8. Restoring the reference descriptor and console environment

The next dependency after fault containment was an environment in which the
original shell could execute its ordinary commands. The prototype treated
stdin as raw key records and stdout/stderr as permanent kernel channels; its
only openable object was a fixed GPU descriptor. Adding file read/write alone
would leave redirection, pipe lifetimes, CWD and the console ABI unresolved.

The comparison source was v0.4.0's `kernel/process.cpp`,
`include/kernel/object.hpp`, `subsystems/posix/userland/sh.c`/`shell_core.c` and
`vendor/busybox-port`. Its BusyBox is an eleven-applet subset, not a complete
ash installation. Reusing those applications avoids making `sh32` a permanent
replacement. [I386_POSIX.md](I386_POSIX.md) defines the current detailed contract.

### Shared objects, private descriptor tables

A descriptor must identify an object, not a file offset copied into each task.
The reference already separates per-process `HandleEntry` slots from shared
`IoObject` state and retains objects across duplication and inherited standard
channels. The port preserves those structures, flags, first-free allocation,
256-object budget, 64 descriptors per task and 64 byte-pipe budget. The portable
I/O types and object lifetime functions were extracted from the reference
process/object modules; their interfaces and algorithms remain shared.

Linking the entire reference process translation unit would also require its
64-bit VM, SMP, device and object-wait dependencies before a file could be
opened. Replacing the object model with independent per-task file records would
change offset and pipe ownership semantics. The chosen adaptation keeps the
reference objects and implements the architecture boundaries in `arch/x86/io`:
32-bit user addresses, page-split physical-alias copies, saved i386 frames and
retryable operations on the existing scheduler. VFS/SxFS remain the same modules.

An open has its own offset; duplicates and inherited standard descriptors share
one. Normal spawn inherits only 0/1/2, as in the reference, so unrelated file,
pipe and GPU descriptors do not leak into children. A pipe endpoint survives
until its last object reference closes. Exit, fault and explicit termination
all release the handle table; zombies retain no I/O ownership. Failed pipe
construction rolls back even when only its first descriptor fitted.

The full-ring producer/consumer case needs suspended I/O state rather than a
kernel loop waiting for another task. A blocked write records its progress; the
PIT and scheduler boundaries retry it, complete the saved return value and make
the caller runnable. Read, EOF, missing readers, polling and deadlines use the
same mechanism. The existing single-CPU atomic-kernel rule keeps retained user
buffers stable. It remains a dependency to preserve when mappings/threads grow.

### The console ABI and the reference read-only root

The reference `sh` reads canonical character lines. Giving it the prototype's
raw event records would pass structure bytes to its parser. The port adapts the
reference pending-line/edit/echo discipline to PS/2 events and the framebuffer
console. Raw consumers explicitly open `/dev/input0`; the diagnostic shell and
keyecho replace stdin with that descriptor. Active raw consumers suppress TTY
draining, preserving their event stream while the canonical shell waits for
its child. This bridge does not establish terminal job control or process groups.

Keyboard traces also exposed a concrete responsiveness failure. An injected
long command appeared only up to roughly the raw queue's capacity, despite the
host having sent its remaining keys and Enter. Each IRQ trace painted/scrolled
the framebuffer while interrupts were masked; canonical input was drained at
scheduler boundaries. The evidence pointed to trace rendering delaying delivery
and consumption enough to fill the event queue. Keeping those traces on COM1,
without mirroring them to the visible console, made the long pipeline checks
complete and restored a readable session. The port also adds the ISO keyboard's
angle-bracket key so redirection can be typed with the default ES map.

The reference shell and applets expect `/bin` and `/README` alongside persistent
`/disk`. Creating special path aliases would hide another bootstrap shortcut.
Instead, GRUB now supplies a Multiboot2 CPIO module and the existing VFS archive
reader supplies the reference read-only tree. The allocator already reserves
module memory from the preceding milestone. The initial Debug archive occupied
9.5 MiB, leaving too little for the 32 MiB memory regression's successful large
child. Stripping debug sections only from CPIO staging copies reduced it to
about 1.2 MiB without changing loaded segments; disk binaries retain their exact
build bytes. This preserves the format and application code while respecting
the test machine's RAM budget.

### Exec and failure cleanup belong to process ownership

The shell's `exec` builtin requires replacing an image without changing PID,
CWD or descriptors. The port follows the reference's prepare-then-commit model:
load a complete ELF32 into a temporary directory, preserve the old image on
error, and only then switch through the supervisor directory to release the
old pages and install the new frame/FPU state. The regression checks failed
missing/bad images, identity preservation, retained descriptors and fresh BSS.

Failure partway through a shell pipeline exposed a reference cleanup defect:
returning immediately could leave opened endpoints and already-launched children
behind. The six-slot table makes that failure especially easy to reach. The
shell now closes each owned descriptor once, terminates children that could
remain asleep or blocked, and collects them before returning to the prompt.
Minimal reference kill dispositions were adapted to the i386 lifecycle for this
purpose. A host audit follows both a missing-command pipeline and a capacity
failure, requiring exactly the expected processes, three descriptors each and restoration
of the free-page baseline.
The parser and ordinary successful pipeline behavior remain the reference code.
The minimal desktop's SxFS status label now reports read/write access; its layout
and artwork are unchanged by this porting milestone.

Running the original `renametest` exposed a separate diagnostic defect: it
moved a directory containing `file.txt` and then checked `data.txt` without
renaming the child. The filesystem correctly retained the child's name. The
diagnostic now performs that missing file rename before checking content and
cleanup, so it tests both operations rather than demanding an unintended side
effect from directory rename.

### Evidence without confusing I/O throughput with pipe progress

A first large-pipe test used a regular file as its output and exceeded the wait.
A debugger backtrace found the guest in `poll_status` in `kernel/ata.cpp`, reached through
`write_journal_payload`/`commit_metadata` from a 256-byte file write, already
several chunks into the transfer. It was progressing through the reference's
expensive metadata commits, rather than establishing a pipe deadlock. The
replacement throughput regression sends 24 KiB through producer, copy and
reader processes over two pipes and verifies every byte plus blocking/EOF;
separate file and applet cases verify persistent-file operations. The port
keeps the existing SxFS journal algorithm and its performance limitation.

`posixtest32` exercises file/path operations, shared offsets and access checks,
standard-channel inheritance, exec, nonblocking/wrapped pipes, polling, 96
faulting children, descriptor exhaustion and repeated partial pipe rollback.
It runs every original BusyBox applet and checks copied bytes. Host input then
checks actual shell parsing, redirection, CWD, failed-pipeline recovery and
restart, alongside the existing ELF32 process and desktop regressions.

`python3 tools/verify_posix_persistence.py` passed a separate ownership boundary:
a guest wrote, renamed and synced a file on a disposable disk copy; the normal
build updated that copy; a cold boot read the exact bytes. The original image's
hash remained unchanged. The canonical Doom executable/WAD/config/save rebuild
check and all 18 host tests also passed. `./build.sh smoke` passed the combined
reference-shell/POSIX, process/FPU/fault and desktop glyph/mouse/cursor checks; `./build.sh smoke memory` also passed on
32 MiB with the CPIO root present. A clean build with diagnostics disabled
retained the shell/BusyBox and omitted the test programs from staging.

The milestone restores the reference console and descriptor base within the
branch's stated bring-up bounds. The six-process capacity still limits pipeline
depth, and fork, complete signals, memory objects, shared libraries and the full
desktop remain parity gaps. The next ordered dependency is section objects,
views, heap growth and ELF32 shared-library semantics; v0.4.0 parity remains open.

## Continuing the study

The next planned area is memory objects and ELF32 shared libraries. Its account
should compare section/view ownership, heap growth, relocation rules and cleanup
with v0.4.0, then record how dynamic application dependencies become usable.
This is planned work, not an implemented result.

Current repeatable entry points include `./build.sh smoke`, `./build.sh smoke memory`,
`./build.sh smoke posix`, `./build.sh test`, `python3 tools/verify_posix_persistence.py`
and `./tools/verify_doom_persistence.sh`; [BUILD_CMAKE.md](BUILD_CMAKE.md) describes
their scope. Keep unresolved questions and temporary differences visible until
the corresponding parity gap is closed.
