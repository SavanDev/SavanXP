// Adapt v0.4.0 SMP phases 0/1 to GRUB2: MADT, INIT/SIPI, private TSS and IPIs.
#include "smp.hpp"
#include "cpu.hpp"
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
    uint32_t online;
    uint32_t pings;
    memory::PageAllocation stack;
};
Slot g_slots[cpu::kMaxCpus];
uint32_t g_boot_stacks[256];
uint32_t g_count = 1, g_reported = 1, g_online = 1;
bool g_ipi_ok;
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
void ping() {
    const uint32_t index = cpu::index();
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
}

extern "C" [[noreturn]] void i386_ap_entry() {
    // APs do not touch the shared console, allocator or scheduler. The BSP
    // finishes both tables before sending the first INIT/SIPI sequence.
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
    // Scheduling on APs is deliberately gated on the later kernel-lock and
    // TLB work. Parked cores accept only local IPIs and spurious interrupts.
    for (;;) asm volatile("sti; hlt" ::: "memory");
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
        if (ping_one(index)) ++received;
        if (ping_one(index)) ++received;
        console::printf("smp: AP index=%u lapic=%u tr=0x%x pings=%u\n", index,
                        slot.observed_id, unsigned(slot.selector), slot.pings);
    }
    g_ipi_ok = received == expected && selectors == g_online - 1;
    console::printf("smp: reported=%u online=%u scheduling=1 (xAPIC)\n", g_reported, g_online);
    console::printf("smp: private TSS %u/%u, targeted IPI %u/%u %s\n", selectors,
                    g_online - 1, received, expected, g_ipi_ok ? "PASS" : "FAIL");
}
uint32_t cpu_count() { return g_reported; }
uint32_t online_count() { return g_online; }
bool ipi_test_passed() { return g_ipi_ok; }
}
