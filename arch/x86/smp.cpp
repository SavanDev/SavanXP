// Adapt v0.4.0 SMP to GRUB2: startup, private TSS, kernel lock and scheduling.
#include "smp.hpp"
#include "cpu.hpp"
#include "process.hpp"
#include "paging.hpp"
#include "kernel/smp.hpp"
#include "kernel/spinlock.hpp"
#include "lapic.hpp"
#include "madt.hpp"
#include "multiboot2.hpp"
#include "kernel/acpi.hpp"
#include "kernel/console.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"

namespace i386::smp {
namespace {
constexpr uint32_t kTrampoline = 0x7000;
struct Slot {
    uint32_t lapic_id;
    uint32_t observed_id;
    uint16_t selector;
    bool schedule_ready;
    uint32_t online;
    uint32_t pings;
    memory::PageAllocation stack;
};
Slot g_slots[cpu::kMaxCpus];
uint32_t g_boot_stacks[256];
uint32_t g_count = 1, g_reported = 1, g_online = 1;
bool g_ipi_ok;
uint32_t g_scheduling_started;
sync::SpinLock g_kernel_lock;
volatile uint32_t g_probe_target, g_probe_address, g_probe_sync, g_probe_value;
static_assert(cpu::kMaxCpus == madt::kMaxCpus);
extern "C" uint8_t i386_ap_boot_start[], i386_ap_boot_end[];
extern "C" uint8_t i386_ap_boot_cr3[], i386_ap_boot_stacks[], i386_ap_boot_entry[];

void delay(uint32_t microseconds) {
    const uint64_t until = timer::monotonic_ns() + uint64_t(microseconds) * 1000;
    while (timer::monotonic_ns() < until) asm volatile("pause");
}
bool overlaps(uint64_t base, uint64_t size) {
    return size && base < kTrampoline + 4096 && (base >= kTrampoline || size > kTrampoline - base);
}
bool trampoline_available(uint32_t information) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(information);
    const uint32_t total = *reinterpret_cast<const uint32_t*>(bytes);
    if (overlaps(information, total)) return false;
    bool usable = false;
    for (uint32_t offset = 8; offset + 8 <= total;) {
        const auto* tag = reinterpret_cast<const Tag*>(bytes + offset);
        if (!tag->type) break;
        if (tag->size < 8 || tag->size > total - offset) return false;
        if (tag->type == 3) {
            const auto* module = reinterpret_cast<const uint32_t*>(tag);
            if (tag->size < 16 || module[3] < module[2] || overlaps(module[2], module[3] - module[2]))
                return false;
        }
        if (tag->type == 6) {
            const auto* map = reinterpret_cast<const MemoryMapTag*>(tag);
            if (tag->size < 16 || map->entry_size < sizeof(MemoryMapEntry)) return false;
            for (uint32_t cursor = 16; map->entry_size <= tag->size - cursor; cursor += map->entry_size) {
                const auto* region = reinterpret_cast<const MemoryMapEntry*>(bytes + offset + cursor);
                if (region->type == 1 && region->base <= kTrampoline &&
                    region->length >= kTrampoline + 4096 - region->base) usable = true;
            }
        }
        offset += (tag->size + 7u) & ~7u;
    }
    return usable;
}
void patch(const uint8_t* symbol, uint32_t value) {
    const uintptr_t offset = reinterpret_cast<uintptr_t>(symbol) - reinterpret_cast<uintptr_t>(i386_ap_boot_start);
    memcpy(reinterpret_cast<void*>(kTrampoline + offset), &value, sizeof(value));
}
void tick_gate() {}
void ping() {
    const uint32_t index = cpu::index();
    // Startup-only probe: the BSP waits for this acknowledgement before
    // modifying mappings. It bypasses the BKL like the reference ping probe.
    const uint32_t address = g_probe_address;
    if (index == g_probe_target && address) {
        if (g_probe_sync) paging::sync_kernel_tlb();
        g_probe_value = *reinterpret_cast<volatile uint8_t*>(address);
    }
    __atomic_add_fetch(&g_slots[index].pings, 1u, __ATOMIC_RELEASE);
}
bool ping_one(uint32_t index) {
    const uint32_t previous = __atomic_load_n(&g_slots[index].pings, __ATOMIC_ACQUIRE);
    if (!lapic::send(g_slots[index].lapic_id, (1u << 14) | lapic::kPingVector)) return false;
    const uint64_t deadline = timer::monotonic_ms() + 500;
    do {
        if (__atomic_load_n(&g_slots[index].pings, __ATOMIC_ACQUIRE) == previous + 1) return true;
        asm volatile("pause");
    } while (timer::monotonic_ms() < deadline);
    return false;
}
bool selftest_tlb() {
    if (g_online <= 1) return true;
    memory::PageAllocation old_page = {}, new_page = {};
    if (!memory::allocate_page(old_page)) return false;
    if (!memory::allocate_page(new_page)) { memory::free_allocation(old_page); return false; }
    *static_cast<uint8_t*>(old_page.virtual_address) = 0xa1;
    *static_cast<uint8_t*>(new_page.virtual_address) = 0xb2;
    void* view = nullptr;
    const uint64_t physical = old_page.physical_address;
    bool ok = false;
    if (paging::map_kernel_pages(&physical, 1, 2, &view)) {
        g_probe_target = 1;
        while (g_probe_target < g_count && !g_slots[g_probe_target].online) ++g_probe_target;
        g_probe_address = reinterpret_cast<uintptr_t>(view);
        g_probe_sync = 1;
        const bool first = ping_one(g_probe_target);
        const uint32_t before = g_probe_value;
        const bool retargeted = paging::retarget_kernel_page(view, new_page.physical_address);
        g_probe_sync = 0;
        const bool second = ping_one(g_probe_target);
        const uint32_t stale = g_probe_value;
        g_probe_sync = 1;
        const bool third = ping_one(g_probe_target);
        const uint32_t synced = g_probe_value;
        g_probe_address = 0;
        ok = first && second && third && retargeted && before == 0xa1 && synced == 0xb2;
        console::printf("smp: lazy kernel TLB %s (before=0x%x stale=0x%x synced=0x%x)\n",
                        ok ? "PASS" : "FAIL", before, stale, synced);
        // A timed-out IPI may still be executing the probe. As with AP boot
        // stacks, retain its mapping and both pages instead of recycling them.
        if (!first || !second || !third) {
            console::write_line("smp: TLB probe timeout; retaining probe pages, BSP scheduling only");
            return false;
        }
        paging::unmap_kernel_pages(view, 1);
    }
    memory::free_allocation(old_page);
    memory::free_allocation(new_page);
    return ok;
}
}

extern "C" [[noreturn]] void i386_ap_entry() {
    // During startup APs avoid mutable subsystems. The BSP finishes the
    // topology and stack tables before the first INIT/SIPI sequence.
    const uint32_t id = lapic::initial_id();
    uint32_t index = 1;
    while (index < g_count && g_slots[index].lapic_id != id) ++index;
    if (index == g_count) cpu::halt_forever();
    cpu::initialize_secondary(index);
    if (!lapic::initialize_secondary()) cpu::halt_forever();
    auto& slot = g_slots[index];
    slot.observed_id = lapic::id();
    asm volatile("str %0" : "=r"(slot.selector) :: "memory");
    if (slot.observed_id != id || slot.selector != cpu::kTssSelector + index * 8) cpu::halt_forever();
    cpu::enable_interrupts();
    __atomic_store_n(&slot.online, 1u, __ATOMIC_RELEASE);
    for (;;) {
        asm volatile("cli" ::: "memory");
        if (__atomic_load_n(&g_scheduling_started, __ATOMIC_ACQUIRE) && can_schedule(index))
            process::start_secondary();
        asm volatile("sti; hlt" ::: "memory");
    }
}

void initialize(uint32_t information) {
    size_t bytes = 0;
    const void* table = acpi::table("APIC", bytes);
    madt::Topology topology;
    if (!madt::parse(table, bytes, lapic::initial_id(), topology) ||
        !lapic::initialize(topology.lapic_address)) {
        console::write_line("smp: MADT/xAPIC unavailable, BSP only");
        return;
    }
    g_count = topology.count;
    g_reported = topology.reported;
    g_slots[0].lapic_id = topology.ids[0];
    g_slots[0].online = 1;
    const size_t trampoline_bytes = reinterpret_cast<uintptr_t>(i386_ap_boot_end) -
                                    reinterpret_cast<uintptr_t>(i386_ap_boot_start);
    const bool launch = g_count > 1 && trampoline_bytes <= 4096 && trampoline_available(information);
    if (g_count > 1 && !launch) console::write_line("smp: low trampoline page unavailable, APs not started");
    cpu::register_external_handler(lapic::kPingVector, ping, true);
    cpu::register_external_handler(lapic::kTimerVector, tick_gate, true);
    cpu::register_external_handler(lapic::kRescheduleVector, tick_gate, true);
    if (launch) {
        // Retain every allocated stack even on timeout: a delayed AP must
        // never enter on a stack that has been recycled into another owner.
        for (uint32_t index = 1; index < g_count; ++index) {
            auto& slot = g_slots[index];
            slot.lapic_id = topology.ids[index];
            if (memory::allocate_contiguous_pages(4, slot.stack))
                g_boot_stacks[slot.lapic_id] = reinterpret_cast<uintptr_t>(slot.stack.virtual_address) + 16384;
        }
        memcpy(reinterpret_cast<void*>(kTrampoline), i386_ap_boot_start, trampoline_bytes);
        uint32_t cr3;
        asm volatile("mov %%cr3, %0" : "=r"(cr3));
        patch(i386_ap_boot_cr3, cr3);
        patch(i386_ap_boot_stacks, reinterpret_cast<uintptr_t>(g_boot_stacks));
        patch(i386_ap_boot_entry, reinterpret_cast<uintptr_t>(&i386_ap_entry));
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        for (uint32_t index = 1; index < g_count; ++index) {
            auto& slot = g_slots[index];
            if (!g_boot_stacks[slot.lapic_id]) continue;
            if (!lapic::send(slot.lapic_id, 0xc500u)) continue;
            delay(10000);
            if (!lapic::send(slot.lapic_id, 0x8500u)) continue;
            delay(200);
            if (!lapic::send(slot.lapic_id, 0x4600u | (kTrampoline >> 12))) continue;
            delay(200);
            if (!__atomic_load_n(&slot.online, __ATOMIC_ACQUIRE))
                (void)lapic::send(slot.lapic_id, 0x4600u | (kTrampoline >> 12));
            const uint64_t deadline = timer::monotonic_ms() + 1000;
            while (!__atomic_load_n(&slot.online, __ATOMIC_ACQUIRE) && timer::monotonic_ms() < deadline)
                asm volatile("pause");
        }
    }
    uint32_t selectors = 0, expected = 0, received = 0;
    for (uint32_t index = 1; index < g_count; ++index) {
        auto& slot = g_slots[index];
        if (!__atomic_load_n(&slot.online, __ATOMIC_ACQUIRE)) continue;
        ++g_online;
        if (slot.selector == cpu::kTssSelector + index * 8) ++selectors;
        expected += 2;
        const bool first = ping_one(index), second = ping_one(index);
        received += unsigned(first) + unsigned(second);
        slot.schedule_ready = first && second && slot.selector == cpu::kTssSelector + index * 8;
        console::printf("smp: AP index=%u lapic=%u tr=0x%x pings=%u\n", index,
                        slot.observed_id, unsigned(slot.selector), slot.pings);
    }
    g_ipi_ok = received == expected && selectors == g_online - 1;
    if (g_ipi_ok && !selftest_tlb()) g_ipi_ok = false;
    console::printf("smp: reported=%u online=%u scheduling=1 (%s)\n", g_reported, g_online,
                    lapic::x2apic_mode() ? "x2APIC" : "xAPIC");
    console::printf("smp: private TSS %u/%u, targeted IPI %u/%u %s\n", selectors,
                    g_online - 1, received, expected, g_ipi_ok ? "PASS" : "FAIL");
}
uint32_t cpu_count() { return g_reported; }
uint32_t online_count() { return g_online; }
bool ipi_test_passed() { return g_ipi_ok; }
bool may_schedule(uint32_t index) {
    return index == 0 || (index < g_count && g_ipi_ok && timer::backend() == timer::Backend::local_apic && g_slots[index].schedule_ready &&
        __atomic_load_n(&g_slots[index].online, __ATOMIC_ACQUIRE));
}
bool can_schedule(uint32_t index) {
    return may_schedule(index) && process::idle_for_cpu(index);
}
uint32_t scheduling_count() {
    uint32_t count = 0;
    for (uint32_t n = 0; n < g_count; ++n) if (can_schedule(n)) ++count;
    return count;
}
void start_scheduling() {
    __atomic_store_n(&g_scheduling_started, 1u, __ATOMIC_RELEASE);
    for (uint32_t n = 1; n < g_count; ++n)
        if (can_schedule(n)) (void)lapic::send(g_slots[n].lapic_id, (1u << 14) | lapic::kPingVector);
    console::printf("smp: scheduling=%u online=%u (%s)\n", scheduling_count(), g_online,
                    timer::backend() == timer::Backend::local_apic ? "local APIC timers" : "PIT timer");
}
void kick(uint32_t index) {
    if (index && can_schedule(index) && index != cpu::index())
        (void)lapic::send(g_slots[index].lapic_id, (1u << 14) | lapic::kRescheduleVector);
    else if (index == 0 && cpu::index() != 0 && lapic::ready())
        (void)lapic::send(g_slots[0].lapic_id, (1u << 14) | lapic::kRescheduleVector);
}
void lock_kernel() { sync::acquire(g_kernel_lock); paging::sync_kernel_tlb(); }
void unlock_kernel() { sync::release(g_kernel_lock); }
bool kernel_locked_here() { return sync::held_by_this_cpu(g_kernel_lock); }
}
extern "C" uint32_t i386_kernel_enter() {
    if (i386::smp::kernel_locked_here()) return 0;
    i386::smp::lock_kernel();
    return 1;
}
extern "C" void i386_kernel_leave(uint32_t acquired) {
    if (acquired) i386::smp::unlock_kernel();
}
extern "C" void i386_wait_interrupt();
namespace smp {
void wait_for_interrupt() {
    asm volatile("cli" ::: "memory");
    const bool held = i386::smp::kernel_locked_here();
    auto* caller = held ? i386::process::current() : nullptr;
    const uint32_t here = i386::cpu::index();
    const uint32_t before = caller ? caller->kernel_suspends : 0;
    const uint32_t depth = caller ? ++caller->wait_depth : 0;
    volatile uint32_t marker = 0x6374726eu ^ here ^ depth;
    if (held) i386::smp::unlock_kernel();
    i386_wait_interrupt();
    if (held) i386::smp::lock_kernel();
    if (caller) {
        // Read hardware again: the normal non-volatile STR helper may be
        // commoned across this call, which would hide an erroneous migration.
        const uint16_t resumed_tr = arch::x86::read_task_register();
        if (i386::process::current() != caller || resumed_tr != i386::cpu::kTssSelector + here * 8 ||
            caller->cpu != here || caller->wait_depth != depth || marker != (0x6374726eu ^ here ^ depth)) {
            console::write_line("process: invalid pinned kernel continuation");
            i386::cpu::halt_forever();
        }
        --caller->wait_depth;
        if (caller->kernel_suspends != before) {
            if (!caller->idle && !caller->kernel_resumes)
                console::printf("process: kernel resumed pid=%u cpu=%u stack=PASS\n", caller->pid, here);
            caller->kernel_resumes = caller->kernel_suspends;
        }
        if (caller->pending_exit && !caller->kernel_kill_resumed) {
            caller->kernel_kill_resumed = true;
            console::printf("process: kernel kill resumed pid=%u cpu=%u\n", caller->pid, here);
        }
    }
}
}
