# SSL: the Savan Subsystem for Linux, as built

What exists of the Linux subsystem and how it works. The plan, the paths
rejected and the phases still ahead are in the
[roadmap](../roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md); the measured scope of
the first profile is in [LINUX_XEYES_PROFILE.md](LINUX_XEYES_PROFILE.md).

**Built so far: E1, the `syscall` trap.** No Linux process can be started
yet. That arrives with S1 and the `linux` launcher at L1.

## The `syscall` instruction raises #UD

`configure_efer` in `arch/x86_64/cpu_init.cpp` runs on the boot processor and
on every application processor. It sets EFER.NXE, which W^X needs, and clears
EFER.SCE. With SCE clear, the `syscall` instruction raises an invalid-opcode
exception (#UD) instead of jumping to `LSTAR`. Limine already leaves SCE
clear, but the kernel states the bit itself. With SCE set and nothing in
`LSTAR`, ring 3 could send the processor to address 0 in ring 0.

Vector 6 has its own stub, `x86_64_invalid_opcode_entry` in
`arch/x86_64/context.S`:

- **From ring 0** (CS's low bits clear), it jumps with the stack untouched to
  the ordinary exception handler, `isr_6`, through
  `savanxp_invalid_opcode_kernel`. It does not take the kernel lock: a kernel
  `ud2` can happen with the lock held.
- **From ring 3**, it is a `CONTEXT_ENTRY`, like the `int 0x80` and timer
  entries. All registers are saved as a `process::SavedContext`, the kernel
  lock is taken (counted against `Door::exception`), and the handler returns
  the context to resume, which may belong to another thread.

The other exception vectors keep their compiler-generated
`__attribute__((interrupt))` stubs. Those save only what the handler clobbers
and cannot hand back a different context, which is why #UD needed its own.

## Trap dispatchers

`process::handle_invalid_opcode` serves every #UD from ring 3:

1. It looks up the trap dispatcher of the process's subsystem
   (`subsystem::trap_dispatcher_for`).
2. If there is one, the code is 64-bit (CS is `kUserCodeSelector`) and the
   two bytes at RIP, read through `read_from_process_memory`, are `0f 05`, it
   does what the instruction does: RIP moves past it, RCX gets the new RIP,
   R11 the flags. It then calls the dispatcher. The return path is the same
   as for `int 0x80` (`finish_user_call`): termination requested meanwhile, a
   sibling's exec, and the handover to a thread the call woke.
3. Anything else is a real invalid opcode. The kernel prints
   `user: exception #6 pid=N name=... rip=0x...`, then
   `user: exception #6 killed pid=N`, and the process ends with status
   128 + 6.

A subsystem registers one with `subsystem::register_trap_dispatcher`. Only
Linux does. A native or POSIX process that executes `syscall` therefore ends
as it always did, and Linux numbers can never reach their tables: the
dispatcher is chosen by the process, not by the number.

## The kernel half

`subsystems/linux/kernel/`, compiled into the kernel. `initialize_linux_subsystem`
registers, for `subsystem::Id::linux`:

| Registration | What it does today |
| --- | --- |
| Trap dispatcher, `dispatch_linux_syscall` | Every number returns `-ENOSYS` (Linux's 38) and is logged once per process: `linux: pid=N unimplemented syscall M` |
| Ordinary dispatcher, `dispatch_linux_int80` | `int 0x80` is the 32-bit Linux ABI, which SSL does not offer: always `-ENOSYS` |
| Per-process state | A bitmap of the numbers already reported (512 bits) |

Linux numbers and error values are written in `linux_abi.hpp` from the Linux
ABI, under `ssl::abi`. Nothing there comes from the POSIX SDK.

## How it is tested

`nativetest` spawns eight children at once that execute `syscall` with RAX
set to 1, Linux's `write`. Each must end with status 128 + 6, and none may
write. It passes on 1, 2 and 4 virtual CPUs (`./build.sh smoke native-test
--smp N`). The Linux side of the trap needs a Linux process. Its first test
is L1's `hello` fixture.
