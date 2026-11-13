#pragma once

#include <stdint.h>

#include "kernel/process.hpp"

/* x86_64's implementation of the CPU contract (docs/kernel/ARCH.md). Code
 * outside arch/ includes arch/cpu.hpp, which selects this header.
 *
 * Everything the kernel needs from the processor goes through namespace arch
 * and the sources in arch/x86_64: descriptor tables, interrupts and the APIC,
 * the FPU, page table roots and TLB invalidation, MSRs, the TSC, and the
 * handful of single instructions that drivers need by name (memory_barrier,
 * store_barrier, cpu_relax). Port I/O is in arch/x86/io.hpp. Code outside
 * arch/ has no inline assembly of its own. */
namespace arch {

using IrqHandler = void (*)();

enum class InterruptEoi : uint8_t {
    none = 0,
    pic = 1,
    local_apic = 2,
};

// Called as the very first thing in _start. The function and the entropy it
// gathers are built without the stack protector: there is no canary yet.
void initialize_stack_chk_guard();
void initialize_cpu();
// Entropy for cookies and placement decisions. Uses RDRAND when the CPU
// advertises it; the TSC fallback exists so old machines still boot, not for
// cryptography.
uint64_t random_u64();

/* --- SMP ---------------------------------------------------------------------
 * Bringing up the application processors, and per-core state.
 *
 * The BSP runs initialize_cpu(); an AP runs ap_initialize_cpu(), which is
 * deliberately narrower: it points the core at the GDT and IDT the BSP already
 * built and prepares its FPU, nothing more. Only once it knows its index does
 * it load its own TSS, with load_task_register().
 *
 * What an AP never does: program LINT0 = ExtINT. The APIC rule is one ExtINT
 * per system, and the BSP already took it in initialize_local_apic(). That is
 * why ap_initialize_local_apic() exists separately.
 */
constexpr uint32_t kMaxCpus = 32;

// The ring-3 selectors (docs/kernel/ARCH.md#segments). User data is flat with
// D/B set and a 4 GiB limit, so the same selector is a 64-bit program's SS
// and a 32-bit stack and data segment; 64-bit mode ignores those bits. The
// 32-bit code segment is how a native program runs 32-bit code (the Win32
// subsystem's guest); the kernel's ABI stays 64-bit only, so int 0x80 from it
// is refused.
constexpr uint16_t kUserDataSelector = 0x1b;
constexpr uint16_t kUserCodeSelector = 0x23;
constexpr uint16_t kUserCode32Selector = 0x2b;

// There is one GDT for every core; what each core owns is its TSS. Each TSS
// has its descriptor in the GDT, one after another starting at this selector,
// and in long mode a TSS descriptor takes two entries (16 bytes).
constexpr uint16_t kTssSelectorBase = 0x30;
constexpr uint16_t kTssDescriptorSize = 16;

// Dense index of this core: 0 is the BSP and the APs follow in start order.
//
// It comes from the selector of the loaded TSS. Since each core loads its own,
// `str` identifies the core with one register read: no MSR or APIC MMIO (which
// under KVM can exit the VM on every access, and this is asked dozens of times
// per syscall), and nothing userland can touch, because `ltr` is privileged.
//
// The asm is deliberately not volatile: TR does not change during a kernel
// entry, which is not preemptible, so the compiler may merge repeated reads
// within one function. Before the first `ltr` TR is 0 and this returns 0,
// which at that point of boot is the BSP.
inline uint32_t cpu_index() {
    uint16_t selector = 0;
    asm("str %0" : "=r"(selector));
    const uint32_t index =
        static_cast<uint32_t>(selector - kTssSelectorBase) / kTssDescriptorSize;
    return index < kMaxCpus ? index : 0;
}

// The same without merging: it really reads TR every time. It is for AP
// start-up, the only place where TR changes, and where a read hoisted above
// the `ltr` would return the wrong core.
inline uint16_t read_task_register() {
    uint16_t selector = 0;
    asm volatile("str %0" : "=r"(selector) : : "memory");
    return selector;
}

void ap_initialize_cpu();
bool ap_initialize_local_apic();

// Loads the TSS of core `index` on this core. An AP does it once, at start-up;
// the BSP already loaded its own (index 0) in initialize_cpu().
void load_task_register(uint32_t index);

// Vectors 64-71: messages between cores.
//
// The BSP sends the ping during start-up to check that the ICR delivers; its
// handler only touches an atomic and does not take the kernel lock.
//
// The reschedule vector enters through its own stub with a saved context (like
// the timer) and does take the lock: it tells a core to go through the
// scheduler now, either to pick up a process that was just woken or to finish
// one that was killed from another core.
constexpr uint8_t kIpiPingVector = 64;
constexpr uint8_t kIpiRescheduleVector = 65;

// Fixed-vector IPI (fixed mode, physical destination, edge) to a core by its
// LAPIC id. False if the local APIC is not ready, if the destination does not
// fit in 8 bits in xAPIC mode, or if the previous delivery has not finished.
bool send_ipi(uint32_t destination_lapic_id, uint8_t vector);

// A non-maskable interrupt to a core by its LAPIC id. It arrives even on a core
// spinning with IF=0, for a lock or anything else; smp::stop_other_cores() uses
// it to stop the machine on a panic. Same failure cases as send_ipi.
bool send_nmi(uint32_t destination_lapic_id);

bool register_irq_handler(uint8_t irq, IrqHandler handler);
bool register_interrupt_handler(uint8_t vector, IrqHandler handler, InterruptEoi eoi);
bool initialize_local_apic();
bool local_apic_ready();
bool local_apic_x2apic_mode();
uint32_t local_apic_id();
bool local_apic_start_oneshot_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value);
bool local_apic_start_periodic_timer(uint8_t vector, uint32_t initial_count, uint8_t divide_value);
uint32_t local_apic_current_timer_count();
void initialize_syscall_gate();
void acknowledge_local_apic_interrupt();
void acknowledge_pic_irq(uint8_t irq);
void set_kernel_stack(uint64_t stack_top);
[[noreturn]] void resume_context(process::SavedContext* context, uint64_t cr3);

/* --- FPU/SSE (per-process state) ---------------------------------------------
 * The kernel is built with -mno-sse, so it never touches the FPU: the x87/SSE
 * state always belongs to the running userland. enable_fpu() turns SSE on at
 * boot and captures a clean state; the scheduler calls fpu_save/fpu_restore
 * when it switches processes, and fpu_init_area() seeds a new process with the
 * clean state. 512-byte areas aligned to 16 (FXSAVE format). */
constexpr uint64_t kFpuStateSize = 512;
void enable_fpu();
void fpu_save(void* area);
void fpu_restore(const void* area);
void fpu_init_area(void* area);

/* --- user FS/GS bases (docs/roadmaps/done/THREADS_ROADMAP.md, phase 3) -------------
 * IA32_FS_BASE and IA32_GS_BASE belong to the user thread running on this
 * core, like the FPU: the kernel never uses either segment (cpu_index() reads
 * TR, and there is no swapgs), so they stay loaded across kernel entries and
 * the scheduler swaps them on a real thread switch. POSIX TLS lives at %fs,
 * Win64's TEB at %gs. */
void save_user_segment_bases(uint64_t& fs_base, uint64_t& gs_base);
void load_user_segment_bases(uint64_t fs_base, uint64_t gs_base);

/* The data selectors DS, ES, FS and GS belong to the user thread too: a
 * kernel entry leaves them as they were, and 64-bit code ignores them, but
 * 32-bit code needs its DS, ES and SS non-null and its own FS. The scheduler
 * swaps them with the bases, the selectors first, because loading a selector
 * resets its segment's base. Index 0..3 = DS, ES, FS, GS. */
constexpr uint32_t kUserSelectorCount = 4;
void save_user_selectors(uint16_t (&selectors)[kUserSelectorCount]);
void load_user_selectors(const uint16_t (&selectors)[kUserSelectorCount]);
/* --- CPU identity (CPUID) ----------------------------------------------------
 * Who this processor is, as the processor itself says. The system properties
 * window asks for it, to show the brand and speed the way the period's "About
 * this Computer" did.
 *
 * The brand string is leaves 0x80000002..4 concatenated (48 bytes) and comes
 * padded with leading spaces; query_cpu_identity hands it over trimmed. If the
 * extended leaf does not exist -- a very old CPU, or a hypervisor that hides
 * it -- `brand` stays empty and the caller falls back to the vendor.
 */
constexpr uint32_t kCpuFeatureSse2 = 1u << 0;
constexpr uint32_t kCpuFeaturePae = 1u << 1;
constexpr uint32_t kCpuFeatureNx = 1u << 2;
constexpr uint32_t kCpuFeatureLongMode = 1u << 3;
constexpr uint32_t kCpuFeatureX2Apic = 1u << 4;
constexpr uint32_t kCpuFeatureHypervisor = 1u << 5;

struct CpuIdentity {
    char vendor[16];  // "GenuineIntel", "AuthenticAMD", ...
    char brand[52];   // leaves 0x80000002..4, trimmed; empty if absent
    uint32_t features;
};

void query_cpu_identity(CpuIdentity& out);

void enable_irq(uint8_t irq);
void disable_irq(uint8_t irq);
void enable_interrupts();
void disable_interrupts();
// Clears IF returning the previous RFLAGS, and puts it back. For a short
// section that does not know whether it runs with interrupts on (boot) or from
// a handler (IF=0 and holding the kernel lock): turning them on blindly in the
// second case lets in a nested interrupt that wants the same lock.
uint64_t save_and_disable_interrupts();
void restore_interrupts(uint64_t flags);
// Flushes this core's whole TLB, global pages included: with CR4.PGE on, turn
// it off and back on; otherwise reload CR3.
void flush_tlb();
void halt_once();
// `sti; hlt` in a single asm statement. sti only takes effect after the next
// instruction, so a pending interrupt wakes the hlt instead of arriving before
// it and leaving it asleep until the next one. As two separate calls, the `ret`
// in between opens exactly that window.
void enable_interrupts_and_halt();
[[noreturn]] void halt_forever();

/* --- Single instructions -----------------------------------------------------
 * Named so that code outside arch/ says what it needs (a barrier, a page table
 * switch) instead of how x86 spells it. */

// Spin-wait hint: tells the core it is in a busy loop (saves power, and on a
// hyperthreaded core gives the sibling the execution units).
inline void cpu_relax() {
    asm volatile("pause" : : : "memory");
}

// False under QEMU's TCG, detected at boot. See lock_spin_hint().
extern bool g_lock_spin_pause;

// The hint for a core spinning on a lock another core holds. On hardware and
// under KVM that is `pause`, which also lets KVM notice a vCPU spinning for a
// preempted holder and run something else. Under TCG, QEMU emulates `pause` by
// leaving translated code through its own global lock, so every core spinning
// on the kernel lock queued behind the others in QEMU itself: measured with
// smptest on 4 cores, a pipe ping-pong took 37 s with it and 0.45 s without.
inline void lock_spin_hint() {
    if (g_lock_spin_pause) {
        asm volatile("pause" : : : "memory");
    } else {
        asm volatile("" : : : "memory");
    }
}

// Whether CR4.SMAP is on. Set by the BSP at boot; every AP matches it or stays
// offline, so it holds for all cores. With it on, the kernel faults on any
// access to a user page outside a user-access window.
extern bool g_smap_enabled;

// Opens a user-access window (`stac`): the kernel may touch user pages until
// user_access_end(). Keep the window around the copy alone, never around the
// validation or anything that could block. A no-op without SMAP, where `stac`
// would be #UD.
inline void user_access_begin() {
    if (g_smap_enabled) {
        asm volatile("stac" : : : "memory");
    }
}

// Closes the window (`clac`). Every kernel entry also runs it, because ring 3
// can set RFLAGS.AC itself and an interrupt gate does not clear it.
inline void user_access_end() {
    if (g_smap_enabled) {
        asm volatile("clac" : : : "memory");
    }
}

// A user-access window for one scope.
class UserAccess {
public:
    UserAccess() { user_access_begin(); }
    ~UserAccess() { user_access_end(); }
    UserAccess(const UserAccess&) = delete;
    UserAccess& operator=(const UserAccess&) = delete;
};

// Full barrier: every earlier load and store, to memory or MMIO, is globally
// visible before any later one. For device rings in shared memory (virtio,
// AHCI command lists) before the doorbell write.
inline void memory_barrier() {
    asm volatile("mfence" : : : "memory");
}

// Store barrier: orders earlier stores, including non-temporal and
// write-combining ones, before later stores. For a framebuffer mapped
// write-combining before the device is told to read it.
inline void store_barrier() {
    asm volatile("sfence" : : : "memory");
}

// The physical address of the active top-level page table (CR3), flags
// included.
inline uint64_t read_page_table_root() {
    uint64_t value = 0;
    asm volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

// Switches to another top-level page table. Drops every non-global
// translation of this core.
inline void load_page_table_root(uint64_t physical) {
    asm volatile("mov %0, %%cr3" : : "r"(physical) : "memory");
}

// Drops this core's translation of one page.
inline void invalidate_page(uint64_t virtual_address) {
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

inline uint64_t read_msr(uint32_t msr) {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

inline void write_msr(uint32_t msr, uint64_t value) {
    const uint32_t low = static_cast<uint32_t>(value);
    const uint32_t high = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

// Cycle counter. Not calibrated here: whoever uses it as a clock measures it
// against a known one first (the PIT).
inline uint64_t read_timestamp_counter() {
    uint32_t low = 0;
    uint32_t high = 0;
    asm volatile("rdtsc" : "=a"(low), "=d"(high));
    return (static_cast<uint64_t>(high) << 32) | low;
}

} // namespace arch
