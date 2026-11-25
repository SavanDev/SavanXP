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
depth. At that milestone, fork, complete signals, memory objects, shared libraries
and the full desktop remained parity gaps. The next dependency at that point was section objects,
views, heap growth and ELF32 shared-library semantics; v0.4.0 parity remained open.

## 9. Restoring the memory-object and shared-library model

The POSIX milestone left a deliberate ownership mismatch: every present user
page belonged to its process directory. That was sufficient for static images,
but a shared library needs a section to outlive any one mapping. Freeing its
pages when the first process exited would corrupt the second process; avoiding
all frees would leak them. The frozen reference already solves this through
independent handle and view references. The port therefore extracts the section
implementation from `v0.4.0:kernel/object.cpp` into a shared `kernel/section.cpp`,
rather than adding an architecture-specific section allocator. The physical-page
list, global budget, zeroing and rollback stay recognizable. The reference kernel
heap supplies metadata; explicit metadata alignment accommodates 32-bit pointers.

The architecture boundary is the page-table leaf. An i386 software PTE bit marks
pages borrowed from a section. Unmapping removes those leaves and then releases
the view reference; directory destruction frees only owned private image pages.
Empty borrowed-page tables are reclaimed too, including a table allocated just
before a mapping fails. A closed handle need not kill its view, and a dead process
need not kill another process's backing. `vmtest32` checks aliases, close-before-
unmap, global/per-process exhaustion and abandoned handles/views, while the
32 MiB scenario measures exact physical-page recovery after partial allocation.

Keeping the original 16 MiB user window was considered as a small first step.
It would make the reference allocator's geometric arenas and multiple libraries
compete with images and the stack for a prototype-sized hole. Instead, the image
base and stack top stay stable while section space extends below the existing
1 GiB supervisor alias boundary. This changes address capacity without changing
the physical allocator or persistent image. A fixed view below the automatic
arena must not lower the subsequent allocation cursor; the regression places
one there before requesting an automatic view. It remains a constrained 32-bit layout,
not the reference's 64-bit virtual geometry or ASLR. Device identity mappings may
not overlap that user space; different hardware framebuffer placements still need
an explicit kernel mapping.

The reference stack reserve also replaces the eager 64 KiB prototype. Eight
initial pages serve normal startup; user not-present faults inside the 1 MiB
reserve allocate a page. The exception frame contains an extra normalized error
word. Earlier containment never resumed this frame, so resuming it unchanged
would restore the error code as EIP. Recovery shifts only the register/vector
prefix past that word and returns a normal saved frame. A 256 KiB stack probe
checks resumed values; the ordinary fault regression still requires the new guard
address and its exact hardware error bits. Non-PAE paging cannot supply NX, and
this work does not disguise the API's W^X check as hardware execute protection.

Heap growth alone would have passed simple allocations while missing reference
ownership behavior. Both the retained `heaptest` and `mmaptest` exercise fork:
shared views must see the child's writes, private arenas must not. Fork therefore
lands with this milestone, copying private leaves and FPU/CPU state and applying
the reference shared/private view rules. Copying private section contents before
publishing the child preserves rollback on exhaustion. The six-task bound stays
explicit. Three children plus the parent can still prove global section exhaustion
by holding views and bare handles, without raising capacity merely to fit a test.
The reference pipe handle queue supplies the other transfer boundary: queue,
handle and view references are each released by their respective owners.

### The first PIE failed before ordinary relocation

The first native ELF32 build loaded all six initial PIE probes, but every one
faulted before main. For `libtest`, the observed CR2 was a link-time guard address
near `0x00050ff8`, while EIP was in the correctly biased image near `0x0804d44c`.
Disassembling the startup/adoption path showed that protected C loaded its canary
through a GOT pointer whose word had not yet gained the image bias. A relative
function call was correct; the data pointer it used was not.

Disabling stack protection for the loader would only move this dependency into
its libc calls and error paths. Moving all dynamic linking into the kernel would
replace the reference design. The smaller adaptation is the interpreter's usual
self-relocation step: a call/pop establishes the i386 GOT, entry values are stored
through local offsets, and a bounded helper without stack protection applies the
main image's RELATIVE entries before any protected C. The ordinary embedded
loader then brings dependencies and resolves imported symbols. It skips those
already-applied main-image entries, because i386 REL reads the addend from the
word it overwrites and applying the bias twice corrupts the pointer.

The loader remains the retained `ldso.c`, using native layouts for ELF headers,
program headers, symbols and dynamic entries. ELF32 relocation records have two
32-bit words, not an explicit RELA addend. The new fixture forces a nonzero
`R_386_32` addend and `R_386_PC32` in data, an unresolved weak symbol, GOT and PLT
entries, and an executable callback. These are checked through actual calls and
pointer equality rather than just inspecting `readelf`. The reference PIE,
interpreter, math, chain, diamond and reduced-slot tests also execute. SysV hash
counts and mapped segment bounds replace unbounded symbol-table probing on the
native build. Temporary range handles close once a view owns its reference;
repeated rejection after a valid first segment verifies rollback, including fd 0.

### File identity after the POSIX port

The retained section documents assumed a running guest could not overwrite a
library and therefore deferred generation tracking. That premise stopped being
true with the preceding file-write milestone. An inode/range lookup could serve
old bytes after an equal-size write, or after unlink and inode reuse. Rewriting
the SxFS journal or adding a persistent generation was unnecessary: live sections
are immutable snapshots, so VFS mutation can remove the old snapshot's lookup key
while its existing references retain its bytes. New opens then read fresh backing.
The identity also includes the mounted volume. The mutation test holds old views
while overwriting and recreating the file and compares both generations' bytes.
Unlink invalidation must happen before SxFS releases the file record; doing it
afterward loses the identity needed to find the snapshot. Rename changes neither
bytes nor identity and keeps sharing.

### Evidence and remaining scope

The focused `shared` and `shared-memory` scenarios run through the reference
shell against rebuilt disk binaries. The latter uses 32 MiB and requires exact
rollback of partially allocated 64 MiB sections and a fork whose private clone
cannot fit. `dyntest32` keeps parent and spawned child alive simultaneously and
asserts that file-backed section counts do not increase, while their counters
remain private. A forked child also gets independent writable library data. The
loader must leave all 61 nonstandard descriptor slots available. The retained
build-time duplication check confirms that real `DT_NEEDED` programs do not carry
private implementations of the libraries they map. An isolated diagnostics-off
build exposed its old assumption that CMake and staging directories are adjacent;
passing the configured library directory explicitly restores independent build/output
overrides. The same build retains the shell and libmath and excludes diagnostics.
Fresh builds also expose a pre-existing SDK boundary: `posix.c` aliases
`intmax_t` functions onto `long` helpers, while `long` is only 32 bits here, and
`inttypes.h` still assumes LP64 format modifiers. This does not change section or
loader ownership, but it prevents claiming complete SDK/application parity. The
later application port must audit these helpers and their 64-bit parsers/formatters.

[I386_MEMORY.md](I386_MEMORY.md) records interfaces, supported relocations,
commands and limits. The expanded workload reached its memory, POSIX and process assertions but
exhausted the old 240-second deadline before the desktop phase. The default is
now 360 seconds; individual command deadlines still detect stalled guests.
The full regression additionally covers the existing
POSIX/shell, process/FPU/fault and minimal desktop behavior, with the persistence
checks guarding external binaries and assets. The 18 host tests, the full smoke,
the original 32 MiB memory smoke, the expanded 32 MiB shared-library smoke and
both POSIX cold-boot and Doom rebuild-persistence checks passed. Built/staged
binaries and libraries were extracted from the real image and compared byte for
byte, preventing a stale image from standing in for the rebuilt programs.
Those results establish the memory
and loader dependency. They do not establish SxGUI painting: GPU surfaces,
window protocols, the compositor and the reference desktop remain milestone 4,
with `sxgui-smoke` to return alongside that port. The SDK's ordinary external
application path remains static, and full signals, other waitable object APIs,
ASLR, TLS and hardware/capacity parity remain open.

## 10. Removing the SDK's LP64 integer assumptions before graphics

Milestone 3's clean builds exposed alias warnings that incremental builds could
hide: the public `intmax_t` was 64 bits, but its implementation used 32-bit
`long`. This was not only a compiler diagnostic. The private `imaxdiv_t` had two
32-bit fields, while the public struct had two 64-bit fields. An ELF32 library
could resolve the right symbol and still pass the wrong arguments or receive
an incompatible struct. The user moved this prerequisite ahead of milestone 4
so these failures would not be mistaken for graphics-port failures.

The same assumption appeared in three other places. `limits.h` advertised a
64-bit `long`; `strtoll` and `atoll` cast the result of an already-narrowed
`strtol`; and `<inttypes.h>` used `l` for every 64-bit format. Inspection then
found that the formatter treated one or two `l` characters identically and that
the scanner parsed through `long` even for an eight-byte destination. Changing
only the macros would leave those paths broken. A mixed format could also shift
the next vararg by four bytes, corrupting later output rather than merely
truncating one number.

### Preserve the target types, repair the operations

The adaptation keeps the compiler's i386 model: native integers/pointers remain
32 bits, and explicitly wide integers remain 64 bits. Bounds come from the
target's compiler definitions. Format strings follow the compiler's actual
fixed/least/fast/max/pointer integer types, rather than a second manually
maintained type model. The runtime now consumes the public `imaxdiv_t` definition
and uses `intmax_t` at both ends of its aliases. Casting between the old private
and public structs would hide the symptom while retaining an ABI mismatch.

Separate 32-bit and 64-bit parser implementations would duplicate sign, prefix
and overflow rules. Instead, one wide magnitude parser accepts a caller-supplied
limit. This is the distinction the old cast missed: `long` still needs its
32-bit saturation bound, while a wide result needs all 64 bits before conversion.
Cutoff arithmetic detects overflow without wrapping; the parser consumes the
remaining digits and leaves an exact suffix pointer. Signed minimum values are
formed without first creating or negating an unrepresentable positive signed
value. Unsigned negative values wrap only if the magnitude itself fitted.

This also repairs two exposed lexical defects in the existing parser: leading
octal zero remains a digit, and hexadecimal prefixes are recognized only with
a following hex digit. Invalid bases are rejected before arithmetic. The SDK's
existing no-conversion `EINVAL` policy remains explicit; this is not a general
locale or C23 parser replacement.

The formatter retains its callbacks and entry points. Its integer dispatch now
reads the requested promoted/native/wide type, including `hh`, `h`, `l`, `ll`,
`j`, `z` and `t`. Integer octal and `%n` complete the corresponding macro surface.
The scanner parses wide before storing at the requested width, and distinguishes
`intmax_t` from `size_t`. Size/difference counters remain native-width objects.
These are type-boundary corrections; the kernel syscall layout, disk format,
allocator model and graphics protocol do not change. Native `off_t` and time
interfaces do not gain a new 64-bit ABI through this change.

### Prove both sides of the shared-library call

A static test alone would not detect a public/private ABI disagreement across
an ELF32 call. `sdkint32` therefore runs the matrix against the static SDK, and
`sdkintpie` imports a fixture library that runs it against the executable's PIC
runtime. The library also returns `imaxdiv_t` and 64-bit values through real PLT
calls. The initial test layout put the full matrix in both the PIE and library;
the existing no-duplicate-library check rejected it. The correction keeps the
PIE caller as an importer, rather than weakening that check or renaming copies
to evade it.

The matrix compares extrema, overflow, errno and exact end pointers against
explicit expected results, and exercises every base from 2 through 36 above the
32-bit range. Mixed varargs place a native integer between wide arguments.
Formatting/scanning round-trips compare with the original value and place guard
bytes immediately after each typed destination, so a matching pair of truncation
bugs or a store wider than the destination cannot pass. Native sizes, long limits
and struct offsets are also compile-time assertions. The same matrix runs with
32 MiB alongside memory/loader exhaustion and cleanup regressions.

The normal build, both shared smokes (including 32 MiB), the full POSIX/process/
desktop smoke and all 18 host tests passed. An isolated diagnostics-disabled
build kept production programs and excluded the SDK test clients/library. Fresh
static and PIC builds no longer emitted integer alias-signature warnings.
Installed artifacts matched the staged binaries byte for byte. POSIX guest-write/
rebuild/cold-boot and canonical Doom executable/WAD/config/save persistence checks
also passed with the rebuilt SDK.

[I386_SDK.md](I386_SDK.md) defines the contract, commands and remaining limits.
These checks establish the integer-width prerequisite, not universal libc,
formatted-I/O, large-file or graphical-session conformance. Already-installed
static external programs retain their bytes and receive the fixes when rebuilt.
The ordinary build and canonical persistence checks continue to protect those
binaries and their assets.

## 11. Restoring the reference graphics session

Milestone 4 replaces the default bring-up desktop with the v0.4.0 session:
reference init, window manager, compositor daemon, taskbar, desktop background,
Program Manager and native applications. The reference rendering and client
protocol sources remain the implementation. Rewriting a smaller desktop would
have avoided several missing kernel mechanisms, but would also have changed the
parity target. Compiling the retained implementations exposed those mechanisms
instead. [I386_GRAPHICS.md](I386_GRAPHICS.md) records the resulting contracts.

### A contiguous surface is a virtual-memory requirement

The framebuffer backend imports a section's physical-page list and expects a
contiguous kernel view. A user view or a physical direct-map pointer cannot
substitute: sections can be physically fragmented, and each user CR3 can map
that view at a different address. The port therefore reuses the reference
framebuffer backend and GPU dispatcher through native user-copy and mapping
adapters. Supervisor-only page tables in `0x80000000..0xb0000000` provide those
contiguous aliases and are shared into every process directory. Removing an
import releases its view and section reference without freeing borrowed pages.
Empty kernel page tables remain cached; the window bounds that retained cost.

GRUB's linear framebuffer has a device identity mapping, whereas allocated RAM
uses aliases starting at `0x40000000`. Treating these as one uniform HHDM would
point the GPU at unrelated memory. The native adapter keeps the two mechanisms
separate, rejects overlapping device mappings and supplies the existing VBE
dispi aperture path before processes start. QEMU exposes the larger aperture,
mode switching and double buffering. PAT write combining and other hardware
backends are later capacity/hardware work; this implementation does not claim
asynchronous presentation or a hardware cursor.

### Input draining was a real interface difference

The first visible session painted Program Manager and its taskbar but did not
respond to the keyboard. Raw scancodes still reached the kernel. The problem
was the prototype's blocking `/dev/input0` read: the reference WM loops over
reads until the queue is empty, then returns to frame/request processing. A
blocking empty read suspended it inside that loop indefinitely. Keeping the
prototype behavior and modifying the WM would have changed a reference client
contract. Native device reads now drain complete records and return zero when
empty; `poll` waits for readiness. Event polling also tests waitable-object
signals instead of reporting `POLLNVAL`.

The mouse now exposes the same event records and readiness path. IntelliMouse
sample-rate negotiation precedes reporting so acknowledgements cannot enter the
packet stream; ID 3 enables the fourth wheel byte, with the reference sign.
Three-byte devices remain supported. The queues use the reference 512-record
bounds and carry a dropped mouse record's wheel count forward. Graphical
ownership suppresses canonical TTY consumption and the old kernel cursor; the
reference compositor draws its own cursor. The retained `desktop32` diagnostic
was adapted to the reference GPU ABI and its own cursor, rather than preserving
the prototype's incompatible strip/pitch interpretation inside the real driver.

### Object waits and process supervision

The reference objects already separated events, timers and semaphores from the
architecture scheduler. Reusing `kernel/object.cpp` avoids replacing the client
wake/submit protocol with pipes or periodic busy waiting. The native scheduler
retains objects during waits, evaluates wait-any/all and deadlines, and drops
references on completion or process termination. The PIT services timers;
blocking returns a saved user frame rather than retaining a kernel continuation.
The wide `monotonic_ns()` result needed an explicit EDX:EAX return on i386:
casting a native 32-bit syscall result to `unsigned long long` loses its high
half. Clock resolution remains the PIT's delivered tick resolution.

The six-process bound could start a few examples but could not support the
reference session's capacity contract. It is now 64, matching the reference;
the 64-descriptor and 64-view budgets remain. The pressure regression fills the
actual table and still exercises clean refusal/reclamation. Its sparse helper
now uses 4 MiB of BSS rather than 12 MiB so the expanded kernel and CPIO leave
room for a surviving child on the 32-MiB regression machine.

The reference init also expects orphan apps to become its children when their
window manager exits. Detaching them, as the bring-up scheduler did, bypassed
that supervision. Orphans are now reparented to PID 1 and wake its child wait
when appropriate. Diagnostic init reaps unrelated adopted children while
waiting for its supervised shell. Missing socket support previously made
`audiod` fail and restart repeatedly; an unavailable socket syscall now causes
a deliberate clean exit. This is a temporary service boundary until milestone
5, not a substitute audio implementation.

### Two stack assumptions found by the WM selftest

The reference presentation selftest initially failed its independent SXE icon
read, although ordinary icons had painted. Its automatic 64-KiB buffer crossed
pages that user instructions had not touched. The native copy validator rejected
those pages before the file read could initialize them. The reference VMM
materializes reserved stack pages during validation as well as on user faults.
Restoring that mechanism fixes the caller without making its buffer static or
pre-touching it. The guard, arbitrary holes and existing read-only permissions
remain protected.

The next selftest failure was an `ENOEXEC` for `gfxdemo`. The original demo's two
1920x1080 arrays occupy about 17 MiB, extending past the prototype's stack at
`0x09000000`. Reducing those arrays would change the reference program; moving
the native stack to `0x0f000000` instead gives the image room below the section
allocation window. The reserve remains 1 MiB, its guard is `0x0eeff000`, and only
eight pages are mapped initially. This supersedes the original stack addresses
in earlier chapters. Loader checks still reject an image that reaches the new
reserved stack, and fault regressions assert the new guard address.

### Retiring the prototype's permanent screen copy

The 32-MiB memory/fault scenario passed, but the shared-memory regression then
failed the fifth simultaneous 1-MiB heap allocation. This was genuine capacity
pressure: the full desktop binaries enlarged CPIO, while the prototype still
reserved a 1280x800 pixel copy in kernel BSS. Only about 5.9 MiB remained free at
boot. The allocator correctly refused the request, but reducing the regression
would have hidden the cost introduced by retaining a bring-up mechanism.

An experiment stripping all ordinary ELF symbols from CPIO copies would save
about 1.4 MiB, insufficient headroom for the existing heap workload. Keeping the
six-live-block and private-fork checks, and removing the obsolete screen copy,
is the smaller system change. The reference backend and user compositor already
own their surfaces; `desktop32` now owns its diagnostic cursor too. Native text
can write glyphs and scroll directly in the framebuffer without another complete
screen in RAM. The prototype kernel cursor and its presentation entry points
were retired with that storage. Fonts, text colours and the running desktop's
rendering sources stay the same. `llvm-size build/kernel.elf` shows the
4,096,000-byte reduction in BSS. The unchanged six-live-block, reallocation and
private-fork cases then passed `smoke shared-memory` with 32 MiB. Physical
framebuffer-read performance remains part of the later hardware assessment.
Restoring 1280x800 also exposed the diagnostic shell's old 768-line upper
bound: its clear command fell back to 25 EGA rows instead of the actual 50.
Using the reported console height preserves that command's original scrolling
approach. Hidden native console clears must also respect graphics ownership;
writing directly to VRAM cannot bypass the existing console-enable gate.

### Resources and evidence

The retained SXE reader only understood ELF64 section headers. Supporting
ELF32 offsets and entry widths preserves the resource format and lets the same
manifest, title, accent and icon bytes reach Program Manager and the WM. The
reader retains ELF64 support for inspecting preserved files; that does not make
them executable. Resource generation and stamping now follow manifest/icon and
build-ID dependencies. The shared runtime variants remove graphics units owned
by declared libraries, and the built-artifact sharing check remains mandatory.
Reference disk assets and generated MIME icons are staged for fresh images;
CPIO carries binaries/libraries rather than duplicating persistent media assets.

The reference WM selftest now reports `DESKTOP SMOKE PASS`, including twelve
overlay windows with `windowd_fds=40` and `app_fds=12`, owned windows, fullscreen
restoration and two deliberate compositor reconnections. The visual Notepad
scenario verified actual built-font glyphs for `linea38` and `linea39`, their
spacing, wheel movement/return and scrollbar clicks. The restored gate also
checks the taskbar's label; a loaded library or a changed pixel alone is not
adequate evidence. Normal smoke runs boot the exact normal ISO for graphics;
console regressions replay that ISO into a disposable copy with only the
`console-init` command-line selection changed. The first attempt to rebuild
that copy with an unrestricted `grub-mkrescue` selected optional EFI tooling
and failed on missing `mformat`; replaying the existing boot records avoids
introducing that unrelated host dependency.

The repeatable entry points are `smoke windowd-smoke`, `smoke sxgui-smoke`,
`smoke taskbar`, `smoke calc-smoke`, the full `smoke`, `smoke memory`,
`smoke shared-memory`, host tests and the POSIX/Doom persistence checks.
These establish the running desktop dependency, not complete v0.4.0 parity.
Audio/network/power services, external application runtime, optional SMP/virtio,
physical-machine validation and performance remain later milestones. The boot
console/logo path also remains the native bootstrap rather than proof of full
boot-visual parity.

## 12. Restoring device services and the external application ABI

Milestone 4 ended with a working reference desktop, but its service boundary
still rejected socket syscalls and dispatched only GPU and raw-input handles.
The audio daemon therefore left deliberately, and retaining the AC'97, network
and power sources did not make any of them usable. Milestone 5 reconnects these
reference implementations rather than introducing a second service protocol.
[I386_SERVICES.md](I386_SERVICES.md) records the current native contract.

### Physical addresses, legacy interrupts and sleeping readers

The PCI enumerator and AC'97 driver compile unchanged. Their DMA allocations
already separate physical bus addresses from the allocator's supervisor pointer.
RTL8139 needed the protected-mode FLAGS save/restore instructions in its receive
critical section. BIOS-assigned INTx routes through the PIC, with ELCR level
triggering and shared handlers; retaining polling alone was considered, but it
would discard the reference device wakeup path. AML/IOAPIC routing and optional
virtio backends remain the later hardware/capacity milestone, rather than being
claimed from successful legacy-device tests.

ACPI brought a different address ambiguity. The original high-half comparison
can distinguish physical addresses from high virtual pointers on x86_64; on
this branch a physical table may numerically exceed the RAM alias base. Native
ACPI table references must therefore be treated as physical and translated
explicitly. Multiboot2 carries the RSDP itself in a boot-information tag, not a
Limine pointer response. The reference FADT/DSDT parser and S5/reset policy can
then remain in use.

Socket I/O cannot permanently wait inside the caller's kernel continuation:
that would stop the audio daemon from running while a client waits for it. The
native descriptor layer retains blocked receives in the process and returns to
the scheduler. On retries it receives into kernel memory, then copies through
the waiting process's page directory. Temporarily changing the running task to
make an existing user-copy helper select another CR3 was rejected: destination
ownership belongs to the caller of the copy, as in the reference TCP reader.
The added datagram receive helper extends the same kernel-buffer boundary to
UDP peer addresses. The reference stack, packet queues and retransmission
algorithms remain shared.

Observed guest evidence includes `nettest` gateway replies and increasing
hardware TX/RX counters, `udptest`, `audiod --selftest` and AC'97 streamed PCM.
The reference `tcptest` verified injected packet loss, bulk and echo payloads,
reordering, a reply delayed seven seconds, a 500 ms receive timeout and partial
writes. Its delayed reader consumed one of 672 elapsed ticks in the recorded
run, establishing that it parked instead of merely returning correct bytes.
The WAV backend left zero RIFF/data sizes even after QMP quit; the capture held
425,352 nonzero samples spanning approximately -14,000..14,000. The harness
therefore validates its actual PCM bytes rather than accepting an empty header.

### Two alignment assumptions exposed by real FFmpeg decoding

The first native FFmpeg build retained the upstream source and selected the
existing no-assembly codec profile for `x86_32`. Media Player loaded the engine,
but decoding its first WAV ended in #GP at `write_number`'s aligned SSE store to
the stack. Disassembly identified `movapd` at offset `0x68a90e` in that build.
FFmpeg's configure profile declared 16-byte stack alignment, while the native
freestanding compiler profile had not explicitly stated that ABI. Aligning
only the initial stack was insufficient: every intermediate caller must use
the same convention. The correction declares it for all native user code and
the standalone builders; it does not disable SSE or add realignment thunks to
upstream code. The next WAV selftest passed.

The MJPEG test then reached another #GP, at an aligned store into a
`SwsContext` (`ff_yuv2rgb_c_init_tables`, offset `0x640e7a` in that engine).
The SDK rounded allocation lengths to 16 bytes, but its heap header shrank
from 32 bytes on LP64 to 20 bytes on i386. Returning `header + 1` broke the
advertised payload alignment, and later splits repeated the error. Giving the
header itself 16-byte alignment preserves the original allocator and its
split/merge rules; a static assertion makes the size invariant explicit.
`heaptest` now checks consecutive small split allocations and an explicitly
aligned allocation alongside its existing arena-growth/recycling workload.
The codec and game tests are being rerun after this correction; their completion
must be established independently of the successful compile.

The build's sharing guard also rejected Media Player because its local loader
helper `read_header` had the same name as an unrelated local FFmpeg helper.
These symbols cannot interpose on one another. Comparing globally visible
symbols preserves the intended guard against private copies of shared APIs
without treating coincidental static helper names as duplicate library code.

The optional FFmpeg engine is installed only on the persistent volume, without
a second resident CPIO copy. Doom's temporary static MIDI runtime is replaced
by the reference PIE and shared synthesizer, and Ccleste's assembly startup is
compiled with the same target triple as its C units. These changes restore the
reference architecture of the applications; installation alone is still not
proof that their graphics, audio and persistence work. At this stage milestone
5 was still in progress; the following runtime gates established its closure.

### Follow-up evidence and the remaining storage boundary

After aligning the heap header, the unchanged Media Player tests decoded WAV,
MJPEG and synchronized AVI, including seeking. Ccleste decoded all 23 effects,
a music track and 120 simulation frames; the graphical session showed its title
and playable room. The power regression established guest-requested S5, a fresh
boot after reset, and shutdown from QMP's firmware power-button event. The
native Haxe sample also verified its ABI, containers, floats and direct display
calls after adapting its register/string instructions and static entry.

The first Doom close test appeared to report one second of stale audio.
Inspection contradicted that interpretation: the purported residue contained
159 distinct changing amplitudes; the final constant sample lasted under one
millisecond. The verifier had equated eight zero crossings in 20 ms with live
music, so bass-heavy or DC-biased FM audio looked stale. It now detects varying
PCM levels rather than high crossing frequency. A host regression includes
changing 40 Hz audio that never crosses zero and confirms that 50 ms of repeated
last-sample PLC passes while 500 ms still fails. The complete game scenario was then rerun with that corrected measurement:
music remained audible and the measured residue after window closure was
0.000 s. The change to the checker alone was not treated as runtime evidence.

The reference ramdisk was another retained source that the native graph did not
use. The normal GRUB ISO must remain small enough for 32 MiB console tests, and
embedding the persistent 1 GiB disk would both exceed the RAM window and carry
user-owned games/configuration into a generated distribution artifact. The
separate LiveCD target instead builds a fresh bounded SxFS module from internal
staging and derives a second ISO using the original BIOS boot records. The
kernel reserves all Multiboot modules before allocation and translates the RAM
image through the supervisor alias before attaching the reference backend.
A diskless guest selected `live0`, passed `fscheck` and wrote/read a new file;
the original ATA-backed persistent path is still checked independently.

### Retaining the 32 MiB heap regression

The expanded resident kernel and CPIO first failed the original six-live-block
heap regression at allocation index four. Reducing the workload or changing
its geometric arena-growth policy would have hidden a capacity regression.
Inspection instead found 1,390,584 bytes of unused static symbol tables in the
resident CPIO. Stripping unneeded symbols from only those boot copies preserves
their dynamic symbol tables and SXE resources while keeping the full original
executables on the persistent disk. The 32 MiB regression is rerun after that
change; its original arena count and allocation sizes remain unchanged.

The corrected boot packaging restored the complete shared-memory regression on
32 MiB, including all six live heap allocations; it did not reduce that gate.
The CPIO and LiveCD image now both use supervisor aliases, so their kernel reads
do not depend on module placement outside the per-process user window. The
reference CPUID query and CMOS clock also fill the existing discovery/realtime
ABI rather than leaving the system properties and taskbar with placeholder
values. `devtest32` checks that discovery, the realtime clock, device access
permissions and refusal of native-only syscalls from a POSIX image.

### Milestone closure and its limits

The final service run passed the reference TCP/UDP, audio-daemon and media
checks, including a decoded frame presented through the GPU. Captured steady
AC'97 levels changed from 12,000 to 6,000 at 50% master volume. Measuring the
steady distribution avoids charging a preceding stream's buffered tail to the
next stream; leaving gain unapplied would still fail the half-amplitude gate.
Both MBR and GPT guest roots passed consistency and file-I/O checks. The LiveCD
also booted the reference graphical session without an ATA disk, and a fresh
build without diagnostic applications displayed Program Manager correctly.

The complete smoke, 32 MiB memory/shared-library gates, WM capacity/reconnect/
fullscreen test and 19 host tests passed. The original native compositor and
interactive toolkit harnesses verified shared surfaces, fork/exec identity,
input delivery and repainting. Doom and POSIX persistence checks used isolated
copies across normal rebuilds, preserving executable/WAD/configuration/save
bytes and guest-written file contents. An installed-image audit compared the
actual ELF32 artifacts with their built counterparts, avoiding stale-image
results after a failed build. [I386_SERVICES.md](I386_SERVICES.md) gives the
reproducible gates and prerequisite applications.

Milestone 5 is therefore complete for the reference legacy-device path. This
does not close the version target. At closure, the roadmap grouped physical
hardware behavior, performance, suspend, SMP and optional virtio support into
the next milestone; the validation-scope correction below supersedes that grouping.
Legacy PIC routing does not claim the reference AML/IOAPIC implementation, and
AC'97 playback does not stand in for virtio capture. Signal/job-control gaps,
explicit idle-process accounting and the reference boot visuals remain visible
in the eventual v0.4.0 parity review. The target does not move to v0.4.1 yet.

### Correction: validation scope follows the reference version

After milestone 5, the project clarified that physical-machine bring-up and
testing enter the reference sequence at v0.4.2. The previous roadmap made
laptop testing a v0.4.0 completion gate, adding a requirement ahead of the
version whose behavior this branch is meant to reproduce. Keeping that gate
would mix architecture parity with later hardware enablement.

The decision is to validate v0.4.0 and v0.4.1 in emulation, then introduce
physical-machine bring-up when the active target reaches v0.4.2. The alternative
of retaining physical tests in milestone 6 was rejected because it changes the
reference validation scope. SMP, virtio and other capabilities already present
in the active reference remain parity requirements, exercised in QEMU; a
later physical test does not substitute for implementing them now.

This is a planning correction, not new runtime evidence. The existing QEMU
results remain valid and physical hardware remains untested. The roadmap,
service contract and hardware analysis now distinguish the current emulated
parity work from the deferred laptop work. No kernel or build behavior changes.

## 13. Restoring the optional virtio profile in emulation

Milestone 6 starts with the optional device profile, after correcting the
version validation scope above. Milestone 5 had restored the legacy devices,
but retaining the virtio sources did not make that alternative profile work.
The reference transport and block/network/sound/input drivers are reused
without changing their rings, feature negotiation, DMA layouts or data paths.
The GPU keeps its reference resource, cursor, imported-surface and presentation
machinery. This exercises the new mapping boundary on one CPU before adding
the concurrent-kernel requirements of SMP.

### MMIO is not a physical RAM alias

The native architecture supplied mappings for scattered RAM sections and an
identity-mapped framebuffer, but no `map_kernel_mmio` implementation for PCI
BARs. Reusing the RAM alias would address ordinary RAM rather than the BAR;
extending identity mapping would mix device addresses with the existing user,
RAM-alias and kernel-view windows and their cache attributes. Neither is a
valid replacement for the transport's MMIO contract.

The selected adaptation maps BAR pages into the existing shared supervisor
view arena. It preserves the offset within an unaligned first page, installs
writable PWT/PCD leaves and rejects spans outside the non-PAE 4 GiB ceiling.
RAM sections retain their stricter allocator-address bound; accepting a device
BAR above the first GiB does not expand allocatable RAM. The mapping shares
the native page-table propagation mechanism with graphics sections, so a
driver invoked after a process switch sees the same BAR through that CR3.
The transport also rejects a BAR size that cannot fit the native `size_t`.

Reading the initialization order identified a second dependency: virtio root
probing needs PCI discovery before mounting SxFS, whereas the legacy path had
enumerated PCI later with audio/network initialization. Discovery now happens
once before root probing. The TSC calibration also moves ahead of GPU probing,
so the reference GPU's boot-time timeout can advance with IF clear. These are
ordering and architecture-boundary adaptations, not new storage or GPU models.

### A shared PIC line can stop the first user instruction

The first block/network/sound integration booted and passed file operations,
device ownership and UDP checks. Adding the GPU exposed a different failure:
the boot reached the scheduler, then init made no visible progress. QMP samples
showed the CPU at the initial user EIP and inside the GPU ISR/interrupt return,
rather than a user exception or an ELF loader fault. Polling devices and the
GPU shared a level-triggered PIC line. Enabling that line for the GPU also
exposed another device's unacknowledged INTx, repeatedly interrupting the CPU.

One option was to add handlers to all polling drivers solely to acknowledge
their ISR bytes. Another was to disable the GPU interrupt and rely entirely
on polling. The smaller adaptation is to leave PCI INTx disabled on native
devices that use polling, and enable delivery only after the GPU installs its
existing handler. No driver's completion algorithm changes. The corrected
guest advances into init and runs the reference applications, while the GPU
keeps its interrupt notification and deferred completion path. MSI-X remains
explicitly deferred until the native local-APIC boundary is implemented;
programming a message into an unavailable vector would not be parity.

The architecture adapter exposes GPU completion/scanout event handles through
the existing descriptor objects and native object waits. Scheduler service
boundaries run the retained display background work. Virtio keyboard input
feeds the existing native decoder, and tablet events retain absolute position,
wheel and button fields through the raw mouse queue. The legacy source is
ignored while its corresponding virtio input source is active, following the
reference preference and avoiding duplicate events.

### Correcting the instruments without weakening the checks

The first capture read failed with an I/O error while playback worked. QEMU
reported that the WAV sink had no input driver: that backend implements output
only. Keeping the WAV sink and changing guest capture to manufacture data
would conceal the missing source. Instead, playback/gain use the WAV sink and
capture uses a separate null-backend guest, which supplies deterministic silent
input. The original eight-period `audiotest --record` workload is unchanged.
It proves RX completion, slot recycling and the copy to user memory, not
nonzero microphone input or recording quality.

The first graphical test drew Notepad and its taskbar label, but the expected
`linea20` glyphs after wheel-up were absent. Screen inspection showed that the
cursor had remained at the screen corner: the harness still sent relative
PS/2 motion to an absolute tablet. The retained harness already offered an
absolute-pointer mode. Selecting it for the virtio profile made the same glyph,
wheel and scrollbar assertions pass without modifying the toolkit, changing
the desktop's pixels or weakening the checks.

### Verification and effect on parity

The resulting profile passes guest SxFS read/write/consistency, MBR and GPT
slices, device permissions and killed audio-owner recovery. The reference NIC
test moves hardware TX/RX counters, UDP delivers data, and the TCP harness
passes its loss/reordering/deadline cases. Captured playback has 636,704 nonzero
samples in the observed run; steady amplitude changes from 12,000 to 6,000 at
50% volume. The separate capture guest completes all eight period reads. The
graphical guest passes the original six editor/taskbar and input captures.
[I386_VIRTIO.md](I386_VIRTIO.md) records reproducible commands and limitations.

The default full smoke, the unchanged 32 MiB memory/shared-library regressions
and all 19 host tests pass. Doom's installed executable, IWAD, configs and save
fixture survive an isolated normal rebuild; POSIX writes survive rebuild and
cold boot without changing the source image. The optional profile changes the
QEMU controllers, not the persistent SxFS format or the installed applications.

This completes milestone 6.1, not milestone 6. The native scheduler still runs
on one CPU; SMP, its local-APIC/TSS/locking/TLB boundaries, AML/IOAPIC routing
and the remaining internal/visual gaps need their own reference comparisons
and runtime evidence. Physical-machine testing remains reserved for v0.4.2.
The active version target remains v0.4.0.

## 14. Starting APs without a bootloader MP handoff

After the virtio integration was committed as `6aaedc05`, milestone 6.2 began
with the reference SMP phase 0 and the TSS part of phase 1. The intent is the
same as v0.4.0: establish CPU identity, private entry state and interrupt
delivery before allowing another CPU into mutable kernel state. This
checkpoint deliberately leaves user scheduling on the BSP; an online AP count
is not evidence of concurrent process execution.

### Replacing only the missing startup boundary

The x86_64 reference delegates AP startup to Limine, which supplies a running
processor, page tables and a stack. Reintroducing Limine or a long-mode stage
would contradict this branch's i386/GRUB boot contract. GRUB2 supplies ACPI
tables but no equivalent MP callback, so the smallest native adaptation is a
MADT-discovered INIT/SIPI path and a short real-to-protected-mode trampoline.
The existing ACPI table walker is reused rather than duplicating RSDP/root
discovery; the new bounded topology parser handles enabled Local APIC/x2APIC
records, disabled entries, duplicates, sparse IDs and address overrides.
The BSP is dense index zero independently of firmware enumeration order.

The trampoline occupies one checked low-memory page, physical `0x7000`,
already inside the kernel's excluded low range. It rejects an E820 or boot
information/module conflict instead of overwriting it. A single per-launch
stack mailbox was considered but rejected: after a timeout, a late AP could
read the next AP's stack/index and corrupt its state. Instead all stack slots
are populated before launch and indexed by the processor's own initial APIC
ID. Startup timeouts also retain the allocated stacks. This trades bounded
reserved RAM for a safe late-arrival path, matching the reference's rule that
boot-owned AP storage must not be recycled underneath a running processor.

The AP enables PSE and paging on the shared kernel CR3, then uses its allocated
supervisor stack, the shared GDT/IDT and its private protected-mode TSS. APs
do not initialize the PIC, print, allocate or enter the scheduler. Only their
own register state, startup slot and atomic ping counter are mutable from AP
code at this stage; the existing one-owner kernel therefore remains valid.

### Hardware state revealed two native assumptions

The first four-CPU boot reached the reference desktop and acknowledged two
targeted pings from each of its three APs. QMP showed distinct selectors
`0x30`, `0x38`, `0x40` and distinct supervisor stacks, but it also showed AP
CR0 values with CD/NW still set. INIT had disabled their caches, while the
BSP inherited enabled caches from GRUB. The trampoline now flushes and clears
those bits before enabling paging. The durable SMP check inspects every
actual CPU context and rejects that mismatch, rather than treating a startup
message or a working IPI as sufficient CPU initialization.

Preparing one TSS per CPU exposed an older native layout error. The structure
was 112 bytes and placed `iomap_base` at offset 108; protected-mode hardware
reads a 16-bit offset at 102. That field was zero, allowing zeroed TSS bytes
to act as permissions for some ports. A control boot with the pre-change
kernel and the current `multitest32 fault-io` diagnostic actually executed
`IN 0x80` from ring 3 and exited 99. With the corrected 104-byte layout,
explicit offset assertions and the I/O bitmap outside the descriptor limit,
the same instruction produced #GP and the shell survived. The old structure
is preserved in `git show 6aaedc05:arch/x86/cpu.cpp`; the control used the same
current diagnostic/disk on both kernels, so an older test binary could not
conceal the defect. This restores the reference's user port restriction rather
than introducing a new process privilege policy.

### Local APIC delivery restores the deferred GPU path

Native external-vector stubs preserve the existing i386 context layout and
dispatch local-APIC EOIs separately from PIC IRQs. The BSP alone owns LINT0
ExtINT; AP LINTs and timers remain masked. The parked AP selftest sends two
fixed-vector messages to each physical APIC ID and checks that CPU's counter,
not a global aggregate that a broadcast could accidentally satisfy. It also
reads each AP's actual task register. The two pings check repeated delivery
through EOI as well as the first successful ICR write.

This supplies the local vector boundary that milestone 6.1 lacked. The GPU
now takes its original MSI-X path when xAPIC initialization succeeds, with
INTx still available as fallback. The native device test additionally requires
actual GPU IRQ notifications, so arming an MSI-X table without delivering a
message cannot pass. The transport's earlier polling-device INTx masking
remains: MSI-X delivery is independent of that PCI legacy-interrupt bit.
No polling driver, GPU queue algorithm or desktop rendering path is replaced.

### Evidence and the next synchronization boundary

The four-CPU regression passed software topology/selector/ping assertions and
QMP hardware checks for paging, write protection, caches, FPU controls, TSS
limits, distinct stacks and parked ring-0 state. The 32-CPU capacity run passed
with 31 private AP TSS/stacks and 62 directed pings. An isolated optimized
build without test applications also passed four-CPU startup and reached the
reference desktop. All 20 host tests passed, including malformed MADT bounds,
disabled/duplicate/sparse IDs and a BSP listed after the retained CPU bound.

The four-CPU virtio profile passed device ownership, UDP/TCP, playback/gain and
the separate capture boot; GPU MSI-X was armed and IRQ delivery observed.
The same profile passed all six SxGUI glyph/wheel/scrollbar captures and ACPI
reboot, shutdown and power-button SCI checks. [I386_SMP.md](I386_SMP.md) gives
the repeatable commands and the exact scope of those assertions.

The default full smoke and unchanged 32 MiB memory/shared-library gates passed,
including the added port-permission fault. Doom and POSIX persistence survived
normal rebuilds and cold boot of isolated images. An extraction after the
normal rebuild matched the built diagnostics/UI libraries and preserved the
installed external executables and IWAD. The final kernel's executable
sections also matched the tested artifact after a comment-only cleanup.

The next boundary is deliberately still closed: a parked AP accesses only
permanent kernel mappings and its own state, so it does not need a global
kernel lock or shared-view TLB invalidation. An AP running user processes
would immediately need both. The reference per-CPU current/idle state, lock
acquisition at entry, release after leaving the outgoing stack, local timers,
remote termination and lazy kernel-TLB synchronization must precede that
release. Bringing up 32 processors does not complete those mechanisms or
milestone 6, and does not advance the v0.4.0 parity target.

## 15. Releasing APs under the reference kernel lock

### Why a working IPI was not enough

The previous checkpoint (`e29757b3`) left every AP parked. Its distinct TSS
selectors and repeated pings established a usable interrupt boundary, but the
native scheduler still had one global current process, one idle frame and one
quantum counter. Releasing an AP against that state could select a process
already running elsewhere, restore another CPU's FPU context or recycle a
stack still in use. Retaining the parked stage while introducing synchronization
was therefore a dependency boundary, rather than evidence of SMP parity.

The chosen model is the pinned v0.4.0 implementation's shared run queue and
single ticket lock, reused from `include/kernel/spinlock.hpp`. Fine-grained
subsystem locks or private run queues would require broader changes to the
allocator, handles, drivers and process ownership, without helping the active
parity target. Native per-CPU state now owns the current process, private idle
frame/stack and local tick/quantum counters. Runnable selection excludes any
slot still owned by another CPU, including a process whose kernel continuation
has not returned. User contexts can migrate only after their outgoing stack,
CR3 and eager x87/SSE state are safe to hand over.

### The lock follows the stack boundary

Each syscall, PIC IRQ, ordinary external vector and exception acquires the
reference ticket lock before entering C++. The i386 entry assembly already
saved all registers and returned a selected frame pointer; the smallest
adaptation records whether this entry acquired the lock, switches to that
frame's stack and calls the release helper there. Releasing in the C++
scheduler would publish a runnable outgoing process while assembly still used
its kernel stack. The same ordering protects exit and immediate slot reuse.
The release helper runs below the saved incoming frame, preserving that frame
and the interrupt-gate IF contract.

Native network waits can retain C++ continuations. Their shared wait helper
now releases the lock around `sti; hlt; cli` and reacquires it before returning.
A nested interrupt acquires/releases its own ownership when the outer wait has
released it; if the CPU still owns the lock, the nested entry leaves ownership
with its caller. This keeps the existing non-preemptible native syscall design:
a ring-0 continuation does not migrate or get scheduled away. The reference's
ability to preempt pinned kernel continuations was considered, but adapting
that machinery is not required to run user processes concurrently and would
expand this checkpoint. This is an explicit remaining internal difference.
Ping and spurious vectors bypass the lock because startup waits for their
acknowledgement while the BSP may be inside the kernel.

Remote termination cannot use the uniprocessor's immediate destruction path.
The requester records a pending exit and sends reschedule vector 66 to the
owner. At a user-mode interrupt or syscall-return boundary, that owner releases
resources and selects another context before destroying the old address space.
A retained kernel continuation keeps its mappings until it can return safely.
The guest log distinguishes a remote request, actual reschedule-vector
delivery and termination in that handler. A correct wait status alone could
otherwise pass without proving reschedule-vector delivery.

### Time and shared translations

The BSP keeps its existing PIC/PIT path and alone advances uptime. APs use a
periodic local APIC timer, calibrated once against the already available TSC
clock at the same 10 ms interval. Per-CPU slices preserve the 20 ms quantum.
System CPU ticks now sum the scheduling CPUs, matching the reference accounting
denominator instead of comparing several CPUs' work against one PIT counter.
Startup, directed-IPI and timer readiness gate AP scheduling separately from
firmware and online CPU counts. Readiness is retained per slot, so a late AP
cannot inherit other CPUs' successful ping checks after a startup timeout.

Kernel views use shared page-table leaves, but local `INVLPG` cannot invalidate
another CPU's cached translation. Map/unmap/retarget operations now advance a
kernel generation; lock acquisition compares each CPU's observed generation
and reloads CR3 before accessing mutable views. Identity device mappings also
propagate their PDEs to live address spaces and synchronize the current CR3,
rather than replacing the caller's active directory with the boot directory.
If an IPI probe times out, its mapping and pages stay reserved so a late AP
cannot access recycled memory. Outside the lock, APs touch only permanent code/state, reserved stacks and
APIC registers. This is the same lazy synchronization boundary as v0.4.0,
avoiding a synchronous broadcast shootdown that would deadlock against CPUs
spinning for the global lock with interrupts disabled.

The boot regression maps a page marked `0xa1`, makes an AP read it, retargets
the view to a page marked `0xb2`, and reads again before and after synchronization.
Observed runs reported `before=0xa1 stale=0xa1 synced=0xb2`: the unsynchronized
read actually retained the old translation, while the synchronized read saw
the new one. Only the first and last values are required, since hardware may
evict a stale entry on its own. This tests the remote TLB mechanism rather
than inferring it from shared page-table contents.

### Evidence, a capacity failure and remaining scope

The port reuses the reference `smptest` with native process-enumeration end
semantics and additional CPUID samples. It observes a child in `running` while
the parent is inside a syscall, records user execution on APs, performs sixteen
termination/reap rounds and exchanges 2,000 pipe records. The harness requires
a remote request and actual vector-66 delivery, alongside correct wait status. The existing
`multitest32` then exercises integer/x87/SSE preservation, private address
spaces, user faults, slot reuse and exact page reclamation under SMP.

A control ISO containing the previous parked-AP kernel and the same current
disk/diagnostic failed with `no child ran concurrently with the parent`.
The new kernel passed. Online CPU counts cannot satisfy this regression by
themselves. Repeatable commands and the QMP hardware checks are documented in
[I386_SMP.md](I386_SMP.md); `smoke smp --smp 4` is the functional entry point.

The first debug build reached the desktop with four CPUs, but the 32-CPU
virtio boot timed out waiting for Program Manager. QMP showed the CPUs mainly
contending in the ticket lock, with a CPU still scanning the scheduler, rather
than a fatal exception. The native scan checked all 32 ownership slots for
every process slot, including empty and blocked ones. Rejecting non-runnable
slots first removed that unnecessary work without changing ownership rules.
The next 32-CPU boot reached the reference desktop within the same harness
limit and passed all private-TSS, repeated-IPI, cache/FPU and TLB checks. This
is evidence for that capacity regression, not a general performance benchmark.

The initial two-CPU harness rejected a successful guest run because it required
an AP sample from a child: the child had run on the BSP while its parent ran on
the AP. The diagnostic now samples both sides of the observed parallel pair.
It still requires AP user execution and a concurrent child, without assuming
which process must own the BSP. The parked-kernel control remains a failure.
Another run showed that every victim's timer could already be queued when its remote kill arrived: the timer
correctly performed the pending exit before vector 66 ran. Requiring the IPI
itself to perform a kill rejected that valid ordering. The gate now separately
requires actual vector-66 handler delivery and successful remote termination;
a scheduler relying exclusively on timers without sending/delivering reschedule
IPIs still fails.

The full functional gate also passed with two and 32 CPUs, including `multitest32`
fault/pressure/reclamation cases. An isolated optimized build without diagnostic
applications reached the desktop with four scheduling CPUs. The four-CPU virtio
profile also passed ACPI reboot, shutdown and firmware power-button delivery.

Four-CPU functional scheduling, virtio device/UDP/TCP/audio services and all six
SxGUI glyph/wheel/scrollbar captures passed. The host suite passed all 20 tests.
The unchanged 32 MiB memory-pressure/shared-library gates and the default full
smoke passed, and isolated Doom and POSIX normal-rebuild/cold-boot persistence
checks preserved external data. These
results extend emulated v0.4.0 coverage; they do not authorize advancing to
v0.4.1 or claim physical-machine validation.

The per-CPU idle remains the native ring-0 frame, not the reference's explicit
ring-3 idle process. Explicit idle accounting, idle wake IPIs, kernel-continuation
preemption, wider interrupt routing and the remaining visual/internal parity
review stay visible follow-up work. Milestone 6 remains in progress and the
active target remains v0.4.0.

## 16. Replacing synthetic idle frames with reference processes

### An invisible idle cannot account for the machine

The scheduling checkpoint above still used a synthetic ring-0 frame whenever
no ordinary task was runnable. That made the CPU halt correctly, but left
its time outside process enumeration: there was no idle PID, address space,
handle count or `SAVANXP_PROC_FLAG_IDLE` entry. The reference Task Manager
calculates system usage from total ticks minus idle ticks. Retaining source
for that application therefore did not establish accounting parity.

The pinned v0.4.0 `kernel/process.cpp` creates an ordinary process for each
scheduling CPU, maps one code page and one user stack page, and gives it a
nine-byte yield loop. This checkpoint reuses that loop and process model.
The native user window begins at `0x08000000`, so code moves there from the
reference's long-mode address; the instruction bytes and syscall ABI do not
change. Code is read-only, the top stack page is writable, and the normal
user/supervisor permissions and guard still apply. Each idle has a regular
16 KiB kernel stack, private CR3, saved frame and eager x87/SSE context, root
CWD, no parent and no handles. Its exported resident user size is 8 KiB.

Keeping the synthetic frame and merely inventing an idle row/counter was
considered and rejected: it would leave an internal bring-up shortcut as a
permanent replacement for the reference process. Instead, the ordinary search
excludes all idle processes and the fallback explicitly selects the current
CPU's own idle. No idle can migrate or be selected by another CPU. The old
32-entry synthetic stack/frame array is removed. These processes occupy the
same 64-slot table as other tasks, rather than increasing its reference bound.
At 32 CPUs, init, shell and test also need slots; `smptest` consequently limits
its child spinners to the slots actually left. It still requires concurrent
execution and AP user code, rather than treating a capacity failure as success.

### Sleep on behalf of the idle, with deadlines still serviced

Simply returning through the ring-3 yield loop would spin through syscalls
while idle. The reference handles this in `SAVANXP_SYS_YIELD`: when the idle
finds no ordinary runnable work, the kernel releases its global lock and
executes the common interrupt wait on its behalf. The native port now follows
that same path. The idle's live C++ continuation stays owned by its CPU,
and reacquires the lock before finishing yield and selecting a user context.
The first completed idle yield validates the saved ring-3 CS/EIP, dense CPU
owner and that CPU's TSS.esp0 before emitting `idle: ring3 cpu=N pid=P`.
Those checks are evidence of actual execution, not just successful allocation.

This changes the old null-current assumption in the timer path. A timer that
interrupts an idle wait now sees a real current process and a ring-0 frame.
Returning before deadline service would allow all CPUs to sleep while user
sleepers never become ready. Following the reference order, the BSP services
deadlines before the ring-0 continuation check; AP timer paths do not advance
global uptime or run that periodic wake pass. Blocked completions are still
checked at scheduler boundaries. Every delivered accounting tick
charges exactly the current process and total, including time halted inside
idle's syscall. A CPU is enabled for scheduling only when both hardware/IPI
readiness and its idle allocation succeed. Partial allocation failure releases
mapped user pages; a failing AP stays parked, while the BSP idle is required.

### Restore process identity instead of assuming PID 1

The reference allocates idles before init. Their PIDs now precede init on i386
as well: observed console boots report init PID 2 with one CPU, PID 5 with four
and PID 33 with 32. Orphan adoption records the actual initial process instead
of searching for PID 1. Idle and init kill requests return `EACCES`, matching
the reference protection policy. Process counts now include zombies until they
are reaped, consistent with enumeration; exit still releases their user pages.

This exposed an earlier diagnostic assumption. The POSIX shell audit required
exactly three processes and three handles on every row. An idle correctly has
zero handles, so the first full smoke reported `shell leaked descriptors`.
The revised audit still requires exactly three ordinary processes, no zombies
and exactly three handles for each ordinary process, while separately checking
one zero-handle idle per scheduling CPU. It does not hide extra rows or loosen
the descriptor-leak check. The rerun passed the exact free-page baseline after
failed pipelines. Orphan/reap regressions continue to use the real init.

### Non-atomic samples and the Task Manager correction

The new guest diagnostic sandwiches its process enumeration between two system
samples. With a stable process set, the change in summed process ticks must
lie between the global interval's lower and upper bounds. The first version
allowed only two ticks of enumeration skew per CPU; that bound was not valid
under 32-CPU TCG contention. A later observed sleep sample had 2,186 idle ticks,
a start-to-start total of 2,195 and measured interval bounds 1,969–2,404. The
check now uses those actual bounds, preserving tick conservation instead of
assuming a maximum duration for a sequence of syscalls. Separate sleep/busy
checks require idle attribution at rest and roughly one CPU's share under load.

This also revealed a real user-visible failure in the unchanged reference
Task Manager. At 32 CPUs its selftest observed a busy process but reported
0% system CPU: separately enumerated idle deltas could exceed the sampled
global delta. At the native 100 Hz granularity, enumeration skew can exceed
the roughly 1/32 share of a single busy CPU. The guest's independent accounting
check passed in that same run, distinguishing the estimator failure from lost
kernel ticks.

Adding a batch snapshot syscall would change the version's ABI and create a
larger adaptation. Replacing the estimator entirely with active-process sums
would lose work from processes that exited between samples. The selected fix
retains the idle-complement estimate, including exited work, but gives it a
floor from observed non-idle deltas and bounds percentages to 100%. This is
limited to the sampled estimate: raw counters, API layouts, windows, controls,
fonts and drawing paths remain unchanged. It is not an atomic-snapshot promise.
The subsequent 32-CPU reference selftest passed, reporting 4% during its one-CPU
busy interval; four-CPU and one-CPU runs also passed their accounting checks.

### Evidence and the remaining boundary

Reproducible gates are:

```sh
./build.sh smoke idle
./build.sh smoke idle --smp 4
./build.sh smoke smp --smp 32
./build.sh smoke shared-memory
./build.sh smoke memory
./build.sh smoke
./build.sh smoke sxgui-smoke --smp 4 --virtio
./build.sh test
./tools/verify_doom_persistence.sh --wad /path/to/doom1.wad
python3 tools/verify_posix_persistence.py
```

`idletest` checks idle count/flags, state, 8 KiB resident size, zero handles,
protection of idle/init, tick conservation and sleep/busy attribution. It also
creates a zombie, checks that it appears in the count with no resident user
pages, then reaps it and requires the exact free-page baseline. The harness
runs the reference Task Manager selftest and requires a verified ring-3 idle
entry for every scheduling CPU. A control ISO with the previous ring-0-idle
kernel and the same current disk/diagnostic failed the idle-count check; the
new kernel passed. CPU online counts alone cannot satisfy these assertions.

An isolated optimized build without diagnostics also reached the reference
desktop with four scheduling CPUs and their verified ring-3 idle entries.

The unchanged six-live-allocation ELF32 heap/library gate passed with 32 MiB.
The 20 host tests passed, as did isolated Doom rebuild persistence. The virtio
SxGUI gate passed all six glyph, wheel and scrollbar captures. The default full
smoke and POSIX rebuild/cold-boot persistence passed as well. Six additional
system/Task Manager captures showed the original layout, four 8 KiB idle rows
with no handles, the disabled End Process action for idle and the performance/
network tabs; the rebuilt application matched the installed image. These gates
supplement the recorded SMP scheduling and process-lifecycle checks, rather
than replacing them with a Task Manager percentage alone.

Explicit idle processes and their accounting are no longer parity gaps.
Milestone 6 remains open: native kernel continuations are still non-preemptible,
idle CPUs find new work at timer boundaries, and wider interrupt routing,
ASLR/signals and the remaining visual/internal review still need their own
evidence. The target remains v0.4.0, with physical validation deferred to v0.4.2.

## 17. Preempting retained kernel waits without migrating them

### A released lock is not yet a released CPU

The idle checkpoint restored real processes and released the global lock while
waiting for interrupts, but the timer still returned immediately from every
ring-0 frame. Other CPUs could enter the kernel, yet a blocking CONNECT could
keep its own CPU until the syscall finished. The pinned v0.4.0 scheduler can
suspend such a continuation and later resume it on the same CPU. Its ordinary
critical work remains protected by interrupt gates and the global kernel lock;
this is not arbitrary preemption inside a half-mutated subsystem.

The reference rule is visible in `git show v0.4.0:kernel/process.cpp`:
`runnable_here` pins saved ring-0 contexts to `proc.cpu`, while the timer may
choose another context from an interruptible wait. The native adaptation keeps
that shared queue, quantum and eager FPU switch. Ordinary asynchronous waits
continue using stored requests/user frames; connect/send paths and idle yield
can instead retain a C++ continuation. No new user syscall or alternate network
implementation is introduced.

### Name the interruptible boundary and retain the actual frame

Unconditionally allowing every ring-0 timer frame to schedule was rejected:
the native entries also support nested interrupts, and kernel critical code
must retain its existing atomicity. A small assembly helper now contains
`sti; hlt; cli; ret` and exports the instruction after HLT. The timer permits
ring-0 scheduling only when saved EIP is that named wait boundary, IF was set,
and the running process has an active syscall/wait depth. The helper is called
after releasing the lock; its C++ wrapper reacquires the lock and synchronizes the
kernel TLB before accessing mutable state after resumption.

Protected-mode hardware makes this adaptation narrower than copying the
long-mode frame implementation. A same-privilege interrupt has EIP/CS/EFLAGS
but no hardware ESP/SS pair: the native saved frame is 64 bytes in ring 0 and
72 in ring 3. The scheduler retains its actual address on the private kernel
stack and assembly restores it in place. IRET then consumes the short frame
and returns to CLI/RET, leaving the original C++ return address and locals
above it. Fabricating a full user-sized frame or copying its missing words
would corrupt that continuation's stack. Existing entry/release ordering is
preserved: the global lock is released only after moving to the chosen stack.

A private `in_syscall` flag records the lifetime from user entry through outer
syscall return. Runnable continuations with that flag can be selected only on
their original CPU. It is cleared after the syscall's C++ work and return-time
cleanup, before an asynchronous user frame is published, so ordinary processes
remain migratable. Using a remembered frame's CS alone outside the scheduler
was considered but is insufficient for lifetime decisions: an IRQ frame can
already have been consumed while the outer syscall still has live C++ locals.
The flag describes that live continuation even after an individual wait ends.

The wait wrapper retains a volatile local marker, caller pointer, CPU and wait
depth, then checks them after resumption. The hardware task register is read
with volatile STR. The normal CPU-index helper intentionally permits commoning
within a non-migrating entry; reusing that cached read in the migration guard
would not independently verify the resumed CPU. Suspend/resume diagnostics
require actual selection of another process and distinguish non-idle waits
from startup or idle-only activity.

### A runnable kernel context still owns resources

Before this change, kill deferred cleanup only for a process found as another
CPU's current task. A preempted kernel continuation is runnable and has no
current-task owner; applying the old immediate cleanup there would destroy its
CR3, handles and user mappings while a saved C++ stack still refers to them.
The native kill path now recognizes the active syscall regardless of run state.
It records the exit status and requests rescheduling on the pinned CPU, retaining
all resources until the syscall returns. Its wait resumes under the same lock,
then the ordinary syscall-return boundary performs exit and reclamation.

This deliberately does not add an unwind/cancellation ABI to network code.
A pending kill can still take the retained operation's existing timeout before
it reaches that boundary, as in the reference's deferred ring-0 termination.
A user-mode runner keeps the earlier direct timer/IPI termination behavior.
The process table's 64-slot bound, idle/init protection and normal page/handle
reclamation rules remain in force.

### Evidence from actual CONNECT continuations

`kpreempt32` uses the existing TCP fault injector with every sequence-consuming
packet dropped. A SYN never reaches an external TCP server; the existing
CONNECT path remains pending until its configured timeout. More waiters than
CPUs force timesharing of real ring-0 continuations. A sibling executes the
non-cooperating integer/segment/flags/x87/SSE probe while they remain pending.
Its progress is measured in charged CPU ticks during short probes, excluding
probes frozen for a whole connection timeout; elapsed wall time alone would
mistake descheduling for progress.

The same assembly checker now has a CONNECT entry. It seeds GPRs, FS/GS,
flags, the stack marker, all eight XMM registers, x87 data/control and MXCSR,
then checks them after the real syscall returns `ETIMEDOUT`. The original
UPTIME stress entry shares those checks and remains exercised by `multitest32`.
Kernel-side checks independently validate continuation CPU, stack and balanced
suspend/resume counts. No test-only kernel entry substitutes for the operation.

The one-CPU run recorded two network waiters suspended/resumed 20 and 21 times,
respectively, while the sibling accumulated 38 ticks in the pending interval.
Three subsequent kills each targeted a genuinely suspended kernel context,
resumed it, prevented further user instructions after CONNECT and recovered the
exact free-page/process baseline. The test also discovers socket capacity at
runtime, then requires that full capacity after normal exits and each kill;
page accounting alone would not expose leaked kernel socket slots. A four-CPU run also passed with five waiters,
including same-CPU validated resumptions and deferred kills of live kernel
owners. Kills of running owners and kills of unowned suspended continuations
are both necessary cases; one cannot stand in for the other.

Control ISOs with the preceding idle-checkpoint kernel and the durable
`e29757b3` kernel, each with the same current diagnostic/disk, failed `sibling did not progress during a kernel wait` on one CPU. The new
kernel passed. This distinguishes actual kernel preemption from merely having
working APs, a released lock or progress on another CPU.

Repeatable gates are:

```sh
./build.sh smoke kernel-preempt
./build.sh smoke kernel-preempt --smp 4
./build.sh smoke smp --smp 4
./build.sh smoke smp-boot --smp 32 --virtio
./build.sh smoke
./build.sh smoke memory
./build.sh smoke shared-memory
./build.sh smoke virtio-smoke --smp 4
./build.sh smoke sxgui-smoke --smp 4 --virtio
./build.sh test
```

`smoke kernel-preempt` accepts 1–8 CPUs so the workload can oversubscribe them
within the unchanged process table. This diagnostic limit does not reduce the
system's 32-CPU scheduling limit. The final resource-capacity gate passed with one and eight CPUs, restoring
all 32 discovered socket slots after each phase. The optimized four-CPU build
passed the same CPU/FPU, deferred-kill and socket-capacity checks. The standard
full smoke, 32 MiB memory and unchanged six-live-allocation library gate passed. The four-CPU virtio suite
passed TCP loss/timeout behavior, UDP, playback/gain and capture; the SxGUI gate
passed six glyph/input captures. All 20 host tests, normal Doom rebuild
persistence and POSIX rebuild/cold-boot persistence passed. The normal 32-CPU
virtio desktop still booted with private TSS/cache/FPU and directed-IPI checks.

### Correct the wakeup target against the pinned reference

Earlier study checkpoints listed wake IPIs alongside future native work.
The pinned v0.4.0 `note_woken` and
[SMP roadmap's measured handoff discussion](SMP_ROADMAP.md#known-traps)
explicitly retain same-CPU syscall-return handoff instead of assigning each
woken process to an idle CPU by IPI. The reference measured that alternative
and reversed it for synchronous desktop RPC latency. Timer-bound idle wakeups
alone are therefore not a parity gap, and adding those IPIs is not the next
required adaptation. Current subsystem docs supersede that earlier assumption.

The remaining native difference is the reference's immediate wakeup handoff
at the waker's syscall return, followed by wider interrupt routing and the
visual/internal feature review. This checkpoint closes retained kernel-wait
preemption, not milestone 6 or the v0.4.0 target. Critical kernel work still
runs with IF clear and the lock held; physical validation remains deferred
to the v0.4.2 target.

## 18. Returning the CPU to a process awakened by a syscall

Chapter 17 left a specific scheduling difference open. The native scheduler
could resume a blocked process, but an event setter could return to user code
and continue consuming its quantum before the waiter ran. A pipe reader or
writer was often not even recognized as ready until a scheduling/timer service
pass. Correct eventual data delivery therefore did not establish v0.4.0's
wakeup behavior, especially for the synchronous RPC used by the desktop.

The pinned reference's `note_woken`, `complete_blocked_read/write/wait/poll`
and `handle_syscall` in `kernel/process.cpp` provide the relevant mechanism.
Completing a blocked operation requests rescheduling on the waking CPU. If
its caller remains running, syscall return hands that CPU back to the ordinary
runnable search. This is the policy retained after the reference's measured
idle-CPU IPI experiment, as explained in [SMP_ROADMAP.md](SMP_ROADMAP.md#known-traps).
Adding an IPI or waiting for the next timer would change that decision rather
than restore it. Unconditionally yielding after every syscall would hide the
missing notification and change unrelated calls as well.

Native completion paths now record the same per-CPU request. Object waits,
blocked reads/writes and polls use it; the outer syscall frame is published
and retained C++ work finishes before scheduling. Existing
round-robin selection and kernel-continuation pinning stay authoritative. A
notification grants a scheduling opportunity, not a new waiter-priority rule.
Child-status delivery retains the reference path: exiting already schedules
the next process, without introducing a separate wakeup notification there.
The timer and the scheduler consume notifications generated by their own
service passes, preventing a stale flag from causing a later unrelated yield.

### Propagating pipe readiness without recursive kernel stacks

The long-mode reference wakes pipe readers from a write and writers from a
read; those operations can wake each other while completing partial transfers.
Copying that call graph literally into the native implementation would make
recursive wakeups consume the bounded 16 KiB process kernel stack. Polling all
drivers after every syscall was also considered, but would add unrelated work
to calls such as `getpid` and uptime reads.

The selected adaptation marks pipe data/endpoint changes and drains the
existing bounded waiter table at syscall return. A pass can progress a partial
write, which marks another pass necessary; completed operations become runnable
and are not retried. Poll readiness is checked after progress. Iteration ends
when there are no more pipe mutations, so no recursive wake chain is needed.
Regular device polling keeps its timer/scheduling entry points. Publishing the
outer user frame first is essential: the pass can complete the caller's own
newly blocked operation, whose return value must survive the return path.
Final endpoint destruction also marks readiness, including a last endpoint
replaced by `dup2`; otherwise EOF and broken-pipe waits would retain the old
latency. Object wait servicing rechecks polls as well, preserving the display
submit-event path described by the reference.

This defers internal pipe wake propagation to the end of the same syscall,
under the existing global kernel lock. It preserves the externally observable
handoff boundary while adapting the reference to the native saved-frame and
stack lifetime rules. It does not introduce a new scheduling policy or a
replacement IPC mechanism. [I386_SMP.md](I386_SMP.md#syscall-return-wakeup-handoff)
records the current contract.

### Execution-order evidence

`wakeup32` maps a shared section before forking, confirms through `proc_info`
that the child is blocked, then performs the operation that wakes it. The
child records its result and execution in that section. The parent reads the
marker immediately after its triggering syscall, before any further syscall,
sleep or yield. With one CPU and only that waiter runnable, the marker proves
handoff rather than eventual timer progress. With multiple CPUs, another CPU
can run the child before or after the observation; those runs consequently
check delivery/results rather than claiming the same ordering proof.

Fourteen cases each repeat 16 times: auto/manual events, atomic wait-all,
semaphore permits, event polling, pipe read/write and poll readiness, EOF,
hangup, `EPIPE`, last-writer replacement and two-stage partial writes. They also
check event/permit consumption and exact process/free-page recovery. A runnable
observer checks that unrelated `getpid` calls do not hand off; samples crossed
by a real PIT tick are discarded. The one-CPU run passed all 224 operations
and 64 eligible unrelated-call samples. Four-CPU delivery, the retained kernel
preemption regression and the existing SMP CPU/FPU/fault/reclamation suite
also passed.

A control ISO retained the same current CPIO/disk and substituted the preceding
`aed04865` kernel. It failed the first auto-event ordering observation:
`result=0 expected=0 handoff=0`. The operation still delivered its result, but
the setter returned before the waiter executed. This distinguishes the restored
boundary from the old eventually-correct implementation. A reproducible
control can be built outside the working tree:

```sh
control_root=$(mktemp -d)
git archive aed04865 | tar -x -C "$control_root"
(cd "$control_root" && ./build.sh kernel)
xorriso -indev build/SavanXP.iso -outdev "$control_root/control.iso" \
  -boot_image any replay \
  -map "$control_root/build/kernel.elf" /boot/savanxp-kernel.elf -commit -end
python3 tools/run_smoke.py --scenario wakeup --iso "$control_root/control.iso" \
  --disk-image build/disk.img --artifacts "$control_root/evidence"
```

The current kernel's repeatable gates are:

```sh
./build.sh smoke wakeup
./build.sh smoke wakeup --smp 4
./build.sh smoke kernel-preempt
./build.sh smoke smp --smp 4
./build.sh smoke shared-memory
./build.sh smoke sxgui-smoke --smp 4 --virtio
./build.sh test
./tools/verify_doom_persistence.sh --wad path/to/doom1.wad
python3 tools/verify_posix_persistence.py
```

The full memory/library, POSIX, CPU/FPU/fault and graphical smoke passed.
The 32 MiB shared-library gate and all six SxGUI glyph/input captures on four
virtio CPUs also passed. All 20 host checks passed (the TCP helper requires local
socket access). Doom executable/IWAD/config/save data survived the normal
rebuild regression, and POSIX files survived write/rebuild/cold-boot validation.
Extracting the installed image confirmed the new diagnostic matches its rebuilt
binary, while external Doom/Celeste executables and the Doom IWAD retain their
previous bytes.

This closes syscall-return wakeup handoff within milestone 6.2. Wider interrupt
routing, AML/power mechanisms, signals/ASLR and the final internal/visual audit
remain separate parity work. Idle CPUs still find work on their timer boundary,
as deliberately retained by v0.4.0. No physical-machine test is needed before
the active target reaches v0.4.2; milestone 6 and v0.4.0 parity remain open.

## 19. Restoring I/O APIC routes for PS/2 and ACPI

The native PIC path was sufficient to type into the shell and receive the
firmware power button, even after the local APIC enabled SMP and MSI-X. That
success hid a difference from v0.4.0. The native build omitted
`kernel/ioapic.cpp`, PS/2 unmasked the PIC before ACPI discovery, and the SCI
skipped the reference GSI router. The PIC-only boot at `8eb4223f` is the
baseline for this comparison.

The reference router discovers the I/O APIC's GSI ranges and applies MADT
source overrides to ISA inputs. PS/2 defaults to active-high edges. The
FADT's system control interrupt uses an active-low level, which the ACPI
handler clears by acknowledging PM1 status. A programmed GSI targets the BSP
and uses a vector from 50 through 63. An additional handler on the same GSI
shares the original vector and electrical configuration. The native external
dispatcher runs the chain before sending EOI to the local APIC. These gates
already existed between the PCI/MSI-X vectors and the SMP IPI vectors.

Keeping the devices on the PIC would preserve the observable input and power
behavior in QEMU while leaving the routing difference intact. Copying the
reference router unchanged would add a second ACPI table walk with
long-mode assumptions about physical addresses and the high-half direct map.
The port keeps the routing logic and obtains the validated MADT from the
existing ACPI parser. It checks entry bounds, maps the controller with native
uncached supervisor pages, and reuses the existing xAPIC dispatcher.
The BSP installs the routes with interrupts disabled, after ACPI discovery
and local APIC setup. PS/2 and SCI use the PIC if their APIC route fails.
Successful routes leave the corresponding PIC inputs masked.

In QEMU's PC profile the driver found one 24-input controller and five ISA
overrides. GSI 1 and 12 used edge vectors 50 and 51. The SCI used level vector
52 on GSI 9. The power smoke asserted those route modes and observed shutdown
from a QMP power-button event. It also verified guest reset and shutdown, then
hid the local APIC with `qemu32,-apic`. That boot typed a command using the PIC
keyboard path and shut down in response to a PIC SCI. On four CPUs the PS/2
desktop smoke checked the editor and taskbar glyphs, pointer, wheel and
scrollbar. The RTL8139 smoke exchanged IPv4 and UDP traffic with the legacy
network adapter. The virtio SMP boot reached the desktop with MSI-X armed
alongside all three I/O APIC routes, and the 32 MiB boot passed the ELF32
shared-library checks. The real Doom IWAD, executable, configuration and save
survived the isolated normal rebuild.

```sh
./build.sh build
./build.sh iso
./build.sh smoke power-smoke --smp 4
./build.sh smoke sxgui-smoke --smp 4
./build.sh smoke net-smoke --smp 4
./build.sh smoke smp-boot --smp 4 --virtio
./build.sh smoke shared-memory
./tools/verify_doom_persistence.sh --wad path/to/doom1.wad
```

The tested device wiring uses identity mappings for PS/2. The observed MADT
overrides establish discovery, while alternate firmware input wiring and
shared PCI GSIs remain unverified. The BSP still uses the 100 Hz PIT through
ExtINT, and BIOS-assigned PCI INTx uses the PIC until the reference AML
`_PRT` and link resolution are available. This closes the PS/2 and SCI routing
portion of milestone 6.2. The reference BSP timer, AML, and the other
internal and visual gaps still need review before v0.4.0 parity.

## 20. Resolving PCI interrupts through firmware AML

The BIOS interrupt-line register provided a useful PIC fallback for RTL8139.
It does not describe how firmware connects an INTx pin to an I/O APIC input.
Packets could also arrive through polling, so the previous network smoke
could pass without exercising even one hardware interrupt. The port now runs
the same vendored uACPI interpreter as v0.4.0 and asks firmware for the routing
table appropriate to the I/O APIC with `_PIC(1)` when that method exists.
Root bridge selection uses segment zero and the device's bus. `_PRT` matches
the slot, function or wildcard, and INTx pin. A direct entry supplies the
GSI, while a link entry supplies it and the electrical mode through `_CRS`.
The previous BIOS/PIC route is still available when firmware resolution fails.

The native bridge accepts the physical Multiboot RSDP, uses uncached
supervisor mappings and returns the complete page span when unmapping. The
old unmap calculation omitted the first-page offset, which could leave a
second page mapped when a firmware table straddled a boundary. The bridge
also reuses the native clock, saves 32-bit EFLAGS and rejects PCI configuration
access outside the aligned legacy register window. It runs on the BSP while
APs are parked. Namespace loading attaches uACPI to the existing SCI vector
for its own event machinery and clears fixed-event enables. The native ACPI
driver rearms the power button after that step, including on failure.
Full namespace initialization and GPE activation remain deferred as in
the reference boot path.

The initial routing experiment could log an IRQ while the RTL8139's device
status was already empty. The periodic RX service could acknowledge the
interrupt before the external vector acquired the kernel lock. Checking the
device status exposed the problem on both tested chipsets even though
packet tests passed. For routed devices, the interrupt handler now checks
and clears the adapter's pending packet status before EOI. RX polling still
drains the ring. With no route, polling also clears the status.

On QEMU i440fx, the RTL8139 INTA link resolved to GSI 11 and delivered a
device-owned interrupt on vector 53. Q35 resolved the link to GSI 23 and
also delivered the interrupt. The normal network gate checks both the
firmware result and actual ISR service. The same gate rejected the
`86d8c3a7` ISO with its missing AML-derived route. The Q35 check ran the same
gate with a `SAVANXP_QEMU` wrapper that selected `-machine q35`, replaced the
IDE drive connection with `if=none,id=svdisk`, and appended
`-device virtio-blk-pci,drive=svdisk,disable-legacy=on`. It kept the RTL8139
and the same SxFS snapshot.

The power smoke passed after AML setup, including a QMP power-button
shutdown and a separate boot with `qemu32,-apic`. The TCP suite passed
both with the AML route and with `qemu32,-apic`, where the RTL8139 still
reported a device-owned interrupt through the PIC. The 32 MiB ELF32
shared-library check and the isolated Doom rebuild also passed.

```sh
./build.sh build
./build.sh iso
./build.sh smoke net-smoke --smp 4
./build.sh smoke tcp-smoke --smp 4
./build.sh smoke power-smoke --smp 4
./build.sh smoke shared-memory
./tools/verify_doom_persistence.sh --wad path/to/doom1.wad
```

These runs cover link-based routes on two emulated chipsets. Direct
`_PRT` entries are supported by the reused resolver, but these
firmware tables exercised link resources. PCI bridge swizzling, ECAM and
enabling device AML event methods remain outside the verified scope. This
closes the reference root-bus PCI interrupt path within milestone 6.2.

## 21. Separating elapsed time from interrupt delivery

The previous i386 BSP ran on PIT IRQ0 at 100 Hz. Uptime, sleeps, timer
objects and parked I/O deadlines used that delivered tick count.
`monotonic_ns` merely multiplied tick-based uptime by a million, so it
reported time in 10 ms steps. Raising the PIT rate alone would reduce those
steps but keep elapsed time tied to IRQ delivery. An interrupt delayed by
the global kernel lock, an IF-clear region or emulation would still slow
the clock.

The v0.4.0 backend programs the local APIC at 1000 Hz on vector 48. The BSP
measures the divide-by-16 countdown between two CMOS RTC seconds and shares
the calibrated period with the APs. The port uses the same procedure and
places a monotonic bound on each RTC wait. The reference fallback count is
available when RTC calibration fails, and an unavailable local APIC selects
PIT channel 0 in rate-generator mode. Twenty delivered ticks preserve the
nominal 20 ms quantum. The BSP alone increments the system tick counter and
services shared deadlines. Each scheduling CPU charges its own process ticks,
including time spent in idle.

The early clock still calibrates the TSC against PIT channel 2. When firmware
advertises a 32-bit ACPI PM counter, the clock preserves its TSC epoch and
extends that counter with unsigned deltas. The BSP samples it during boot
and the global kernel lock serializes sampling once APs schedule. As in the
reference, firmware with a 24-bit counter keeps the TSC. Both elapsed-time
syscalls now read the monotonic clock. The nanosecond result uses EDX:EAX,
and the SDK's 32-bit uptime remains milliseconds modulo 2^32. Process
deadlines use this same source. Timer interrupts still determine when
the kernel checks those deadlines and how it accounts for CPU time.

The QEMU PC profile used the TSC and calibrated the local APIC to about
62,498 counts per millisecond. The 1000 Hz fallback also booted and serviced
the power button with APIC support hidden. The reference `clocktest` was
added to native staging with an ABI check that brackets each uptime reading
with two monotonic reads and requires submillisecond resolution. Its
four-CPU run measured uptime at 99%, 100% and 98% of the RTC while spinning,
sleeping and running competing workers. Over the same run, `timer-stats`
showed windows with substantially fewer delivered ticks than elapsed
milliseconds. This is why the two clocks must remain independent.
A second run passed the one-shot and periodic timer-object checks and
measured 91%, 100% and 100% against the second-resolution RTC. The same
gate rejected the preceding AML kernel's PIT backend.

```sh
./build.sh build
./build.sh iso
./build.sh smoke clock-smoke --smp 4
./build.sh smoke power-smoke --smp 4
./build.sh smoke smp --smp 4
./build.sh smoke kernel-preempt
./build.sh smoke wakeup
./build.sh smoke shared-memory
./build.sh smoke sxgui-smoke --smp 4 --virtio
./build.sh smoke audio-smoke
./tools/verify_doom_persistence.sh --wad path/to/doom1.wad
```

The SMP gates checked per-CPU accounting, private TSS, IPIs and retained
kernel waits. The 32 MiB library gate, all six SxGUI captures, AC'97
playback and gain, and the isolated Doom rebuild passed. These runs exercise TSC time with both
scheduling backends. The PM32 path and failed RTC calibration were outside
the observed QEMU configuration. The timer work completes the clock and
interrupt infrastructure checkpoint in emulation. Delivery under host
contention remains a performance measurement, and the v0.4.0 review
continues with the remaining internal and visual differences.

## Continuing the study

Review signals, ASLR and the remaining boot and desktop differences against
the pinned reference. Record performance and power evidence before closing
the v0.4.0 target.

## 22. Randomized section views inside the 32-bit user window

The first i386 section allocator always began at `0x10000000`. Moving the ELF
load bias would address a different part of the virtual map. v0.4.0 keeps the
executable and stack at fixed addresses and seeds only the section cursor in
`VmSpace`. The 16 GiB reference offset cannot fit below the i386 kernel's
supervisor alias at `0x40000000`. A 256 MiB initial window supplies 65,536
page-aligned starting positions and leaves at least 512 MiB above every seed.
The full 768 MiB section region remains available to the allocator.

The native CPU boundary tries a 32-bit RDRAND sample after checking CPUID and
uses the reference TSC/counter mix on CPUs without it or after repeated
failures. The upper half of the mixed value supplies the 32-bit fallback
sample, and the PID participates as in the reference. The global kernel lock
serializes the counter after scheduling starts. The fallback has the same
non-cryptographic limitation as v0.4.0.

A new process receives its seed before it becomes runnable. Exec replaces the
cursor with the rest of the user image, after the ELF load can no longer fail.
Fork copies both the mapped addresses and the cursor. The allocator searches
forward, then below the cursor. A free range that begins below the cursor may
span it, as in the reference. A successful automatic placement advances the
cursor even after a wrap. Explicit mappings keep their requested addresses and
can raise the cursor when they extend above it.

In QEMU, `./build.sh smoke shared` samples the first view across eight spawn
and exec pairs. It checks page alignment, variation within the seed window,
fork inheritance, failed-exec preservation and explicit placement at the
upper boundary followed by automatic wrap. The 32 MiB `shared-memory` run
also retained the existing section and loader rollback gates. Running
`aslrtest32` with `-cpu qemu32,+rdrand` passed with RDRAND enabled and the
fallback counter still zero. The desktop
passed `sxgui-smoke --virtio --smp 4` with the randomized views and the
isolated Doom persistence check passed after a normal rebuild. This restores
the reference's section-placement policy within the smaller address space.

## 23. Signals as process bookkeeping and termination

The initial native kill path recognized the five v0.4.0 signal numbers,
but looked up PID zero as an ordinary PID, reported a negative PID as missing
and returned from SIGCHLD without updating the process. In the pinned
`kernel/process.cpp`, `kill_process` resolves zero to the sender, validates the
signed PID and records a pending bit and the last signal. Child termination
records the same SIGCHLD state on the parent before waking a waiter. An
unreaped zombie remains addressable. A signal to it leaves its existing exit
status intact.

The i386 process slot now carries the same two fields, with fresh slots
starting clear. Fork starts the child clear and exec preserves the signal
state of the existing process. SIGCHLD updates bookkeeping, while the other
four signals end the target with status `128 + signal`. A terminating self-signal can use the
syscall return path already responsible for deferred exits. A remote or
retained ring-0 target still unwinds on its owning CPU. A queued termination
keeps its first requested exit status even when another signal updates
`last_signal`. The pinned SDK's user-installed actions are local libc
state. Handler dispatch, process groups and job control require a different
contract. Broken pipe writes continue to return `EPIPE`.

`./build.sh smoke posix` runs `sigtest` through the normal syscall entry in
the diagnostic console. It verifies signed PID validation, protected init,
nonterminating SIGCHLD, self-termination, SIGTERM sent to another process,
zombie status and the broken-pipe result.
`./build.sh smoke smp --smp 4` exercises the IPI and memory reclamation.
For direct bookkeeping evidence, the process-table address and field offsets
were taken from the ISO's debug ELF with GDB. QEMU `pmemsave` snapshots of a
console shell before and after running `true` showed
`(pending_signals, last_signal)` change from `(0, 0)` to `(16, 17)`. This
observes the child-exit event independently of an explicit signal syscall.

## 24. The reference boot screen and the display handoff

Before this step, native i386 mirrored early serial messages onto the
framebuffer and left that console visible until the graphical client took
over. The pinned kernel sets aside the linear boot scanout for its
logo, fade, marquee and status. The existing splash module already accepts
a `boot::FramebufferInfo` and draws without a heap or an active display
driver. The port compiles that renderer and the existing generated logo and
wordmark into ELF32. GRUB supplies the BGRX aperture. The native page tables
must map it before painting, so its first status follows the switch to the
bootstrap page directory. The phase labels follow native initialization
order, including PCI and storage discovery.

The selected clock preserves the reference animation during quiet boot
stages. Serial line completion, the RTC edge wait and the AML allocator
advance it before timer interrupts can run. The BSP tick supplies the pulse
after scheduling starts. The boot thread is the sole painter before APs
enter the scheduler, and the kernel lock serializes drawing and ownership
once they do. Acquiring the display stops the splash before the compositor
presents. Under virtio, the initial image belongs to the VGA compatibility
scanout and the compositor presents through virtio's own primary surface.
The diagnostic init preserves text output, and an unavailable SxFS root or
a failed init load gives the screen back to boot diagnostics.

The baseline ISO was built from `git archive v0.4.0` with the tag's
`./build.sh iso`. Both builds booted in QEMU at 1280x800 with standard VGA.
The generated `boot_logo.h` matched byte for byte. At full brightness, the
captured logo, wordmark and version regions matched pixel for pixel. The branch's
`Legacy Edition` label, introduced by `8f8b3c1a`, remains visible, and the
status text reflects the initialization stage at capture time. The
`boot-screen` smoke checks a dimmed frame, full artwork, two marquee
positions and the Program Manager taskbar glyphs after handoff. It passes
on VGA and on the four-CPU virtio profile. The previous ISO reached the
desktop and failed the artwork and animation assertions. A diskless LiveCD
boot reached Program Manager from the SxFS ramdisk. QEMU checks without an
SxFS disk or without VGA confirmed the fallback diagnostics.

## 25. Matched QEMU boots for the v0.4.0 review

The parity review starts with two clean Debug builds, the pinned v0.4.0
reference (`8ece07f4`) and the i386 checkpoint (`de642584`). Both use the
same Clang 23.1.5 and QEMU 11.1.3. Each build creates its own 1 GiB SxFS
volume with the userland for its ABI. Snapshots place each guest's writes
in a disposable overlay so every run starts from its build's initial disk.

The tag normally launches its development tree through Q35/UEFI. Its
release ISO can boot through BIOS, but loads a 256 MiB SxFS module into
guest RAM. For this comparison, package the tag's development tree as a
BIOS Limine CD. Its `limine-dev.conf` loads the kernel and CPIO, and the
guest mounts an attached SxFS disk just as i386 does. This keeps the BIOS,
machine and storage topology common. Limine versus GRUB and the different
kernel and initramfs sizes still affect end-to-end startup time. Keep
loader time distinct from kernel and desktop time when measuring startup.

### Reproducing the pair

From the i386 repository with a local Limine clone, build the two pinned
trees outside the working build directory. The tag's `build.sh build`
finishes the CMake target and then calls `tools/check_shared_libs.py`,
which the tag lacks. Invoke the CMake target directly and select the
Python interpreter with Pillow in its initial configuration.

```sh
audit=$(mktemp -d /tmp/savanxp-parity.XXXXXXXX)
git clone --shared --no-checkout . "$audit/reference"
git -C "$audit/reference" checkout --detach 'v0.4.0^{commit}'
git clone --shared --no-checkout . "$audit/i386"
git -C "$audit/i386" checkout --detach de642584dc3f85502547cdee1652a7c9111c4e41
git clone --shared tools/limine "$audit/reference/tools/limine"
git -C "$audit/reference/tools/limine" checkout --detach 7a9013f305de0bea1f6310e76e7baba30499fef0
(
    cd "$audit/reference"
    cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-linux.cmake \
        "-DPython3_EXECUTABLE=$(command -v python3)"
    cmake --build build/linux --target savanxp
    make -C tools/limine
    xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
        -no-emul-boot -boot-load-size 4 -boot-info-table -iso-level 3 \
        -V SAVANXP -o build/SavanXP-parity.iso build/image
    tools/limine/limine bios-install build/SavanXP-parity.iso
)
(cd "$audit/i386" && ./build.sh iso --debug)
```

Launch both with the i386 tree's QEMU argument builder and the same
`qemu-system-x86_64` binary so that the reference can enter long mode.
The builder gives both guests 1024 MiB, TCG, the `pc` machine, an IDE CD,
user networking and headless audio. Set `version` to `reference` or
`i386` and `profile` to `pc` or `virtio`. Run the guests in sequence.

```sh
version=reference profile=pc
PYTHONPATH="$audit/i386/tools" python3 - "$audit" "$version" "$profile" <<'PY'
from pathlib import Path
import subprocess
import sys
from run_qemu import build_qemu_args

root, version, profile = sys.argv[1:]
build = Path(root) / version / "build"
run = Path(root) / "runs" / version / profile
run.mkdir(parents=True, exist_ok=True)
virtio = profile == "virtio"
iso = build / ("SavanXP-parity.iso" if version == "reference" else "SavanXP.iso")
args = build_qemu_args(
    iso=iso, disk_image=build / "disk.img", debug_log=run / "debugcon.log",
    accel="tcg", smp=4 if virtio else 1, headless=True, virtio=virtio,
    snapshot=True, serial_path=run / "serial.log", qmp_path=run / "qmp",
)
args[0] = "qemu-system-x86_64"
args[args.index("-cpu") + 1] = "max" if virtio else "qemu64"
subprocess.run(args, check=True)
PY
```

### Boot evidence and comparison limits

| Profile | CPU | vCPU | Emulated devices | v0.4.0 | i386 |
| --- | --- | ---: | --- | --- | --- |
| `pc` | `qemu64` | 1 | ATA, VGA, PS/2, RTL8139, AC'97 | Desktop reached | Desktop reached |
| `virtio` | `max` | 4 | virtio block, GPU, input, network and sound | Desktop reached, four scheduling CPUs | Desktop reached, four scheduling CPUs |

The first four-vCPU reference run with `qemu64` reached IOAPIC
initialization and stopped producing boot output before the kernel SMP
report. The 65-second timeout expired at that point. With one vCPU,
`qemu64` reached the virtio desktop on both builds. With four vCPUs,
the same machine and virtio devices reached the desktop with `max` on
both builds. With that model the reference uses x2APIC while i386
retains xAPIC. Later measurements need to account for this difference
in interrupt delivery.

Extracting the kernel from each ISO matched its staged ELF byte for byte.
The disk extraction confirmed each ABI's Program Manager and that the
fonts used for the check match the guest's `libsxgfx.so.0.4`. In each boot,
QMP confirmed the vCPU count and the SxFS backing image under the overlay.
The PCI device lists matched within each profile. Serial confirmed the
storage device and SxFS mount. A QMP screenshot
at 1280x800 and the desktop smoke's `expect_text` check confirmed the
`Program Manager` taskbar glyphs. SHA-256 of each base disk matched
before and after its runs. This establishes the comparison environment
and a desktop checkpoint. The feature, visual and performance comparisons
are still part of the parity review.

## 26. Stack guards and SMEP

The [parity review](I386_PARITY_REVIEW.md) found three security rows
where i386 still ran bring-up values. The kernel's `__stack_chk_guard`
stayed zero for the whole boot. Every process received the constant
`0x00c0ffee` in EDX, so all images shared one guard. SMEP was never set,
although 32-bit paging supports it. v0.4.0 seeds the kernel guard first
thing in `_start`, gives every new image a fresh guard and sets
`CR4.SMEP` when CPUID advertises it.

The port reuses the native sample from section 22: RDRAND when CPUID
advertises it, otherwise the reference TSC/counter mix. The boot entry
is now `no_stack_protector` and seeds the guard before serial setup.
Frames entered with the old zero never return, and later frames compare
against the stable boot value. A guard with a zero low byte is XORed with
`0xa5`, as in the reference. `initialize_user_frame` draws the user
guard for spawn and exec. Fork copies the parent's memory, so the child
keeps the parent's `__stack_chk_guard` as in v0.4.0.

SMEP uses CPUID leaf 7 (EBX bit 7). `CR0.WP` was already set together
with paging on the BSP and in the AP trampoline. The kernel image, its
direct map and the AP trampoline are supervisor pages. The only
user-accessible mappings are the per-process windows, which the kernel
reads and writes but never executes. The BSP records whether it enabled
SMEP and prints `cpu: SMEP enabled` or `cpu: SMEP unavailable on this
processor`. An AP enables SMEP only when the BSP has it, and halts before
reporting online if it cannot.

That AP rule follows the baseline in v0.4.0's `KERNEL_SECURITY.md`: the
BSP continues on older CPUs, and an AP that cannot match it stays
offline. The tag's code is stricter than that document. Its
`ap_initialize_cpu` halts any AP without SMEP, even when the BSP also
lacks it. The BSP then spins up to 200 million times per missing AP.
`qemu64` has no SMEP, which probably explains why the four-vCPU `qemu64`
reference run in section 25 stopped after IOAPIC setup. That cause has
not been confirmed. i386 keeps its `qemu32` SMP profiles working because
they do not expose SMEP.

`qemu32` exposes neither SMEP nor RDRAND, so the default smokes exercise
the fallbacks. `./build.sh smoke hardening --smp 4` boots `-cpu max`,
which exposes both. It requires the SMEP and RDRAND boot lines and reads
`CR4` from every CPU through QMP; every value had bit 20 set
(`0x00100610`). It also runs `guardtest32`, which `smoke shared` runs
under `qemu32` too. The test checks that the guard is non-zero with a
non-zero low byte and that fork inherits it. Across eight spawned images,
some guard must differ from the parent's, and the spawned guards must
vary. Each sample also execs itself, and the exec must install a new
guard. With this change the three security rows match the reference. NX
remains an architectural difference without PAE.

## 27. Reference init automation specs on i386

The parity review listed 28 v0.4.0 smoke scenarios with no i386
counterpart. Most are init automation specs. The reference runner plants
`/SMOKE` in the boot root; `init` reads that one line, runs the matching
selftest instead of starting the desktop and prints `<LABEL> PASS` or
`<LABEL> FAIL`. The i386 `init` kept this dispatch unchanged, but no i386
harness used it. Earlier i386 checks typed commands into the console
shell instead, which proves the program but not the reference path.

`tools/i386_spec_test.py` reproduces the reference path. It extracts the
CPIO from the GRUB ISO and inserts a newc `SMOKE` entry before the
trailer, using the same writer as `mkinitramfs.py`. It then replays the
ISO with only that module replaced. The persistent disk boots as a
snapshot. Commands, tokens, timeouts and preparation steps come from the
v0.4.0 catalog. As in the reference runner, driver-specific audio
scenarios force AC'97 or virtio-sound, a WAV capture must be non-empty,
and the keyboard and pointer scenarios send their QMP sequences after
the guest's READY token. The four reference host scenarios alias the
existing CTest targets. `kbdtest` was in the reference registry but not
in the i386 build; it now builds as the static test it was.

Two scenarios failed on their first run, and both exposed real
differences. `filesapp-smoke` found only Notepad's four associations.
`/bin/mediaplayer` carried a 96-byte `.sxmeta` and no `.sxicon` because
`gen_sxe_resources.py` searches manifest directories without recursing.
The Media Player is the only program whose manifest lives in a
subdirectory. v0.4.0 passes that directory explicitly, with a comment
warning that the player otherwise loses its category and associations;
the i386 resource step had dropped the line. Restoring it gives the
binary its icon and its `ext_open` list. `.mp3`, `.avi` and `.flac` then
resolve to the launcher again. The desktop and SxGUI captures still pass.

`float-smoke` failed while it was being built. `sdk/floatsmoke` checks
that XMM state survives preemption with inline assembly that uses
`xmm8` and moves XMM values into 64-bit general registers. Neither
exists in protected mode. An `__i386__` branch keeps the same check in
`xmm4`–`xmm7` and moves the 64-bit patterns through memory operands; the
x86_64 code is unchanged. The external SDK builder stays static, as in
v0.4.0.

All 23 newly ported scenarios pass in QEMU: the application selftests,
`cursor-repro`, `gpu-soak`, the AC'97 and virtio-sound stream, count and
record scenarios, `kbd-smoke`, `pointer-smoke --virtio` and the host
aliases. Five reference scenarios were still pending at this checkpoint.
Section 28 records how they were restored.

## 28. Port-specific smoke scenarios and the reference failure paths

The two existing aggregate smokes could exercise the Media Player and Haxe
clients from a diagnostic shell. The remaining reference scenarios also
cover the regular init automation path, the graphical error window and
preparation of the native Haxe binaries. The reference Media Player runner
checks a held frame through its port-owned QMP callback. i386 now invokes
that callback after `MEDIAPLAYER DISPLAY READY` and requires the init
success token. The `mediaplayer-selftest` name also works from the
existing two-phase FFmpeg port smoke. With the i386 build cache as its
default, `./ports/ffmpeg/smoke.sh --skip-port` passed in an independent
tree using the built ELF32 library.

The two missing-library scenarios use the same build that can play media.
They install the fixture library and test media on a sparse copy of the
SxFS disk, then remove `lib/libffmpeg.so.0.4` only from that copy.
`mediaplayer-nolib` requires both the library name on serial and the
reference exit status 2. `mediaplayer-noport` opens the Media Player from
Program Manager and verifies the error glyphs in the video area with
the reference capture script. The Haxe scenarios use the same staging
path to build and install each Haxe application and its POSIX host test
before starting the appropriate init spec.

All six scenarios passed through `tools/run_smoke.py` with disks built
by `./build.sh iso`. The missing-library scenario drew the expected
warning, and the display callback saw 1045 colors in the 1280x800 frame.
The SHA-256 of the persistent disk agreed before and after the six runs.
The same four Media Player paths then passed on both BIOS ISOs in the
matched VGA profile. The held 1280x800 decoded frame was pixel-identical.
For the missing-engine window, the captures differed only in the known
edition label on the desktop wallpaper. The pinned Haxe build could not
link either client. Its build script uses the dynamic POSIX `crt0.S`
without the objects defining `sx_interpreter_path`, `sx_image_base` and
`sx_start_dynamic`. The i386 tests pass with the native startup stub
described in the device services chapter.

## 29. Argument budgets and adoption of orphaned children

The audit found the original fixed array of 15 strings with at most 63
characters in the i386 syscall handler. v0.4.0 copies up to 128 arguments
into one heap buffer. It counts string terminators against the 8 KiB
buffer and reserves its last byte for termination on failure. The
effective successful limit is 8191 bytes. An exact 8192-byte payload
returned `E2BIG` on the pinned reference. The native handler now uses the
same heap-backed budget for spawn, spawn with redirected descriptors
and exec, retaining 4-byte user pointers and the i386 startup ABI.
It reports `E2BIG` for invalid copier input, preserving the reference
syscall contract. Failed exec leaves the caller's image intact.

The other discrepancy was in orphan cleanup. The i386 scheduler handed
even existing zombies to init and could retain their slots while init
waited for a different child. The reference reaps already-dead children
when their parent exits. It marks live children as adopted and frees
their slots on exit unless a waiting parent consumes the status.
The port applies the same rule to its existing process slots and
preserves notification of a waiter for a live adopted child.

`./build.sh smoke processes` exercises the argument limit, an argument
longer than 63 characters, the terminating null pointer, byte and count
overflow, successful exec and the existing lifecycle checks. For a direct
comparison, the argument workload was also built with each SDK as
`/disk/bin/smoke` on a disposable disk and booted with `/SMOKE=smoke`.
In that run, init waits for the test process itself. The test abandons
both a live child and an already-exited child and checks the process
count after their slots should be free. Both guests passed the same C
workload. The previous i386 ISO left the adopted live child in the
process table after it exited.

## 30. Desktop shortcuts and power in QEMU

`./build.sh smoke window-keys` drives the graphical session on i386.
The script opens Notepad, asserts the Task List's
`Switch To` button after Ctrl+Esc and while Alt is held during Tab, checks
the taskbar bevels as Alt+Tab changes focus, and verifies that Alt+F4
removes Notepad. On the reference BIOS ISO, invoke the same
`shoot_session.run_scenario("window-keys", ...)` after Program Manager is
visible, with `SAVANXP_OUTPUT_ROOT` pointing at the reference build for
the expected font glyphs. Both guests passed with standard VGA/PS/2.

The same profile passed the shared `kbdlayout` and `clipboard` capture
sequences. Each guest started with the `ES` taskbar glyphs and showed
`EN` after English was selected in the popup. The same physical key
then drew `[` in Notepad. Selecting and copying `hola mundo`, moving to
the end of the line and pasting produced `hola mundohola mundo` in the
editor on both guests.

The same QEMU profile also matches the normal desktop power paths.
Start menu Restart showed the confirmation text and produced another
complete boot to Program Manager. Start menu Shut Down reached QEMU's
`shutdown` state with `-no-shutdown`. On a fresh boot,
`system_powerdown` reached the same state through the firmware button.
The i386-specific `power-smoke` retains its extra SCI routing checks
including PIC fallback.

## 31. Timing with the APIC mode controlled

The four-vCPU reference boot that timed out in section 25 completed
with `qemu64,+smep` on the same machine. It reported xAPIC and scheduled
four CPUs. This isolates SMEP as the change needed for that QEMU boot
and agrees with the pinned AP initialization check in section 26.
The branch's LAPIC driver uses the MMIO register interface. For a
comparison with both guests using xAPIC and SMEP, use
`-cpu max,-x2apic` with either one or four vCPUs.

For the pipe workload, build the branch's `smptest.c` with both SDKs,
using `build-user.sh --name smoke` on separate writable SxFS copies.
Boot with `/SMOKE=smoke` as above. The test verifies the online CPUs,
remote termination and data integrity, and measures 2000 pipe round
trips using guest uptime. These are milliseconds from two sequential
runs per row under QEMU 11.1.3 TCG, Debug builds, 1024 MiB and the
default `pc` devices.

| CPU configuration | v0.4.0 | i386 |
| --- | ---: | ---: |
| 1 vCPU, `max,-x2apic` | 307–323 ms | 514–536 ms |
| 4 vCPUs, `max,-x2apic` | 810–893 ms | 1881–1962 ms |
| 4 vCPUs, `max` | 742–1039 ms | — |

The i386 pipe path is slower in these samples with either guest CPU
count. The reference ranges with xAPIC and x2APIC overlap. The pipe test
includes scheduling, system calls and data transfer, so the isolated
cost of interrupt delivery remains open.

For composition, run the shared `shoot_session` scenario `bench` on
each BIOS ISO with one `qemu64` CPU and VGA. It moves a small box in
Gfx Demo for 20 seconds, delivering 172 and 175 key taps per second.
Among the nine `windowd-stats` intervals per guest with at least ten
frames and a damage-bounds area below 100,000 pixels, the medians of
the interval means were:

| Guest | Compose (µs) | Sync (µs) | Present (µs) |
| --- | ---: | ---: | ---: |
| v0.4.0 | 538 | 470 | 1158 |
| i386 | 794 | 1374 | 1495 |

These are TCG workload samples. Frame counts depend on how each guest
batches input, and the host was also running other verification work.
Interactive desktop samples put the boot banner to visible Program
Manager milestone at roughly 7–10 seconds on the reference and 7–14
seconds on i386. The kernel-entry and first-composed-frame boundaries
still need instrumentation for the formal boot measurement.

For idle, boot each matched VGA guest with one `qemu64` CPU and let the
init spec finish so that init sleeps and the desktop and audiod are
absent. Sample QEMU user plus system CPU time from `/proc/PID/stat`
over ten seconds and query HMP `info registers` through QMP. Both
guests reached `HLT=1` in their interrupt-wait routine. The reference
used 21.7% of one host CPU and i386 used 34.8% in that sample. The
desktop sessions sampled separately used 45.6% and 46.4%. The steady
timer windows in the headless run reported average handler times of
62–73 µs and 156–232 µs respectively. Some native running samples
resolved to the timer's process and I/O scans or 64-bit division.
The sample identifies paths to profile and leaves the cost of each
path to be measured.

## 32. The reference core smoke and the remaining scenario names

The parity review listed `libctest` as the evidence for the libc row,
but an audit of a clean i386 disk showed that it had never been built.
Twelve v0.4.0 programs were missing: `smoke`, `libctest`, `forktest`,
`polltest`, `sectiontest`, `stacktest`, `imagetest`, `badptr`, `spawnloop`,
`ticker`, `demo` and `ffmpegload`. The branch's working disk still held
ELF64 copies from earlier master builds, so a listing of that disk hid
the gap. Only a clean image or a check of each file's ELF class shows
it. The reference `./build.sh smoke` boots `/SMOKE=smoke`, and init then
runs `/disk/bin/smoke`. That runner spawns 40 programs, including these
twelve, BusyBox and `fscheck`. The i386 smoke typed its own checks into
the console shell instead and never ran this path.

The sources were unchanged since the tag. Eleven programs join the
static ELF32 test list, and `ffmpegload` uses the reference PIE profile
with `libmath`. Two tests contain constants for the x86_64 address
space. `sectiontest` asked `map_view_at` for `0x2000000000` and expected
a refusal over the image at `0x400000`. On i386 these become
`0x38000000`, inside the section range that ends at `0x40000000`, and
the `0x08000000` image base. `imagetest` stored 64-bit patterns in an
`unsigned long`, which i386 truncated, so its `.data` check could not
pass. It now uses 32-bit patterns under `__i386__`. The x86_64 code is
unchanged in both tests.

The first run of the spec passed every step. The harness still failed
it because `stacktest` overflows into the stack guard on purpose. The
guest printed `user: exception #14` with `cr2 0x0eeffc34`, inside the
guard page at `0x0eeff000`. The parent requires that exit status. The
spec harness rejected any user exception, so specs now declare how many
deliberate faults they expect. The core spec allows one.
`./build.sh smoke` runs this reference spec between the i386 console
checks and the SxGUI capture.

The remaining reference catalog names were loader diagnostics
(`brokentest`, `missingtest`, `slottest`, `diamondtest`,
`needstest-missing`), `ffmpegload`, `ccleste-selftest`, `native-hello`
and three names for checks that i386 already ran under other names.
The diagnostics use their reference tokens, and `needstest-missing`
removes `libneeded.so.0.4` from a disposable disk copy. The port-backed
specs install their port on an isolated copy first. `taskbar-smoke`
and `doom-music-smoke` alias `taskbar` and `doom`, and `ffmpeg-smoke`
runs the port's two-phase script. All passed in QEMU. `ffmpegload`
reported 1870 ms to load the FFmpeg library in that run.

The audit also found two tooling differences. The kernel never printed
the reference `boot ready:` and `handoff: starting /bin/init` lines, so
the reference `shoot_session` driver waited for a line that never came.
`tools/shoot.sh` accepted only `desktop`. The i386 entry now prints both
lines before init, from the boot memory totals, framebuffer size, NIC
presence and SxFS mount state. `shoot.sh` follows the reference script
with the GRUB ISO in place of the Limine image root. A disposable disk
copy replaces its UEFI variable copy, which the BIOS boot does not use.
`tools/parity_capture.py` runs one `shoot_session` scenario on both
matched ISOs from section 25. It compares captures with the same name
and can mask the documented edition label. The `desktop` captures differ
only in that label.
