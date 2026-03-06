#include "kernel/kernel.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/heap.hpp"
#include "kernel/panic.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/string.hpp"
#include "kernel/timer.hpp"

namespace {

const char* firmware_type_name(boot::FirmwareType firmware_type) {
    switch (firmware_type) {
        case boot::FirmwareType::x86_bios:
            return "x86 BIOS";
        case boot::FirmwareType::efi32:
            return "UEFI 32";
        case boot::FirmwareType::efi64:
            return "UEFI 64";
        case boot::FirmwareType::sbi:
            return "SBI";
        default:
            return "unknown";
    }
}

uint64_t mib_from_bytes(uint64_t value) {
    return value / (1024ULL * 1024ULL);
}

struct MemorySummary {
    uint64_t usable_bytes;
    uint64_t reclaimable_bytes;
    uint64_t reserved_bytes;
};

struct PmmSelfTestResult {
    uint64_t first_page;
    uint64_t second_page;
};

struct HeapSelfTestResult {
    uint64_t first_block;
    uint64_t second_block;
};

struct TimerSelfTestResult {
    bool software_irq_ok;
    bool hardware_irq_ok;
    uint64_t hardware_ticks;
};

const char* timer_backend_name(timer::Backend backend) {
    switch (backend) {
        case timer::Backend::local_apic:
            return "local apic";
        case timer::Backend::none:
        default:
            return "none";
    }
}

MemorySummary summarize_memory(const boot::BootInfo& boot_info) {
    MemorySummary summary = {};

    for (size_t index = 0; index < boot_info.memory_map_entries; ++index) {
        const boot::MemoryRegion& entry = boot_info.memory_map[index];
        switch (entry.type) {
            case boot::MemoryRegionType::usable:
                summary.usable_bytes += entry.length;
                break;
            case boot::MemoryRegionType::bootloader_reclaimable:
            case boot::MemoryRegionType::acpi_reclaimable:
                summary.reclaimable_bytes += entry.length;
                break;
            case boot::MemoryRegionType::reserved:
            case boot::MemoryRegionType::reserved_mapped:
            case boot::MemoryRegionType::bad_memory:
                summary.reserved_bytes += entry.length;
                break;
            default:
                break;
        }
    }

    return summary;
}

PmmSelfTestResult run_pmm_self_test() {
    memory::PageAllocation first = {};
    memory::PageAllocation second = {};

    if (!memory::allocate_page(first) || !memory::allocate_page(second)) {
        panic("pmm: unable to reserve bootstrap pages");
    }

    memset(first.virtual_address, 0, memory::kPageSize);
    memset(second.virtual_address, 0, memory::kPageSize);

    auto* first_words = static_cast<uint64_t*>(first.virtual_address);
    auto* second_words = static_cast<uint64_t*>(second.virtual_address);
    first_words[0] = 0x534156414e585031ULL;
    second_words[0] = 0x534156414e585032ULL;

    if (first_words[0] != 0x534156414e585031ULL || second_words[0] != 0x534156414e585032ULL) {
        panic("pmm: hhdm mapping self-test failed");
    }

    return {
        .first_page = first.physical_address,
        .second_page = second.physical_address,
    };
}

HeapSelfTestResult run_heap_self_test() {
    auto* first = static_cast<uint8_t*>(heap::allocate(128, 16));
    auto* second = static_cast<uint8_t*>(heap::allocate(8192, 64));

    if (first == nullptr || second == nullptr) {
        panic("heap: bootstrap allocation failed");
    }

    for (size_t index = 0; index < 128; ++index) {
        first[index] = static_cast<uint8_t>(index);
    }

    for (size_t index = 0; index < 8192; ++index) {
        second[index] = static_cast<uint8_t>(0xa5);
    }

    if (first[17] != 17 || second[4095] != 0xa5) {
        panic("heap: memory self-test failed");
    }

    return {
        .first_block = reinterpret_cast<uint64_t>(first),
        .second_block = reinterpret_cast<uint64_t>(second),
    };
}

TimerSelfTestResult probe_timer() {
    const uint64_t before_soft = timer::ticks();
    asm volatile("int $48");
    const uint64_t after_soft = timer::ticks();

    const uint64_t before_hardware = timer::ticks();
    for (uint64_t spin = 0; spin < 50000000ULL && timer::ticks() == before_hardware; ++spin) {
        asm volatile("pause");
    }
    const uint64_t after_hardware = timer::ticks();

    return {
        .software_irq_ok = (after_soft == before_soft + 1),
        .hardware_irq_ok = (after_hardware > before_hardware),
        .hardware_ticks = after_hardware - before_hardware,
    };
}

void append_text(char*& cursor, const char* text) {
    while (*text != '\0') {
        *cursor++ = *text++;
    }
}

void append_decimal(char*& cursor, uint64_t value) {
    char buffer[32] = {};
    size_t index = 0;

    if (value == 0) {
        *cursor++ = '0';
        return;
    }

    while (value != 0) {
        buffer[index++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        *cursor++ = buffer[--index];
    }
}

void terminate_line(char*& cursor) {
    *cursor = '\0';
}

} // namespace

[[noreturn]] void kernel_main(const boot::BootInfo& boot_info) {
    console::write_line("=== SavanXP kernel bootstrap ===");
    arch::x86_64::initialize_cpu();

    if (boot_info.bootloader_name != nullptr) {
        console::printf(
            "boot: %s %s on %s\n",
            boot_info.bootloader_name,
            boot_info.bootloader_version != nullptr ? boot_info.bootloader_version : "unknown",
            firmware_type_name(boot_info.firmware_type)
        );
    } else {
        console::printf("boot: unavailable on %s\n", firmware_type_name(boot_info.firmware_type));
    }

    console::write_line("cpu: gdt loaded, idt loaded, breakpoint probe ok");

    if (boot_info.framebuffer.available) {
        console::printf(
            "video: %llux%llu pitch=%llu bpp=%u\n",
            boot_info.framebuffer.width,
            boot_info.framebuffer.height,
            boot_info.framebuffer.pitch,
            static_cast<unsigned>(boot_info.framebuffer.bpp)
        );
    } else {
        console::write_line("video: unavailable");
    }

    const MemorySummary memory = summarize_memory(boot_info);
    console::printf(
        "memory: %u regions, usable=%llu MiB reclaimable=%llu MiB reserved=%llu MiB\n",
        static_cast<unsigned>(boot_info.memory_map_entries),
        mib_from_bytes(memory.usable_bytes),
        mib_from_bytes(memory.reclaimable_bytes),
        mib_from_bytes(memory.reserved_bytes)
    );

    memory::initialize(boot_info);
    if (!memory::ready()) {
        panic("pmm: no usable regions or missing hhdm");
    }

    const PmmSelfTestResult pmm_test = run_pmm_self_test();
    console::printf(
        "pmm: %llu pages ready, sample=0x%llx..0x%llx free=%llu\n",
        memory::total_page_count(),
        pmm_test.first_page,
        pmm_test.second_page,
        memory::free_page_count()
    );

    heap::initialize();
    if (!heap::ready()) {
        panic("heap: unable to reserve initial arena");
    }

    const HeapSelfTestResult heap_test = run_heap_self_test();
    console::printf(
        "heap: %llu bytes total, used=%llu free=%llu sample=0x%llx..0x%llx\n",
        heap::total_bytes(),
        heap::used_bytes(),
        heap::free_bytes(),
        heap_test.first_block,
        heap_test.second_block
    );

    timer::initialize(100);
    const TimerSelfTestResult timer_test = probe_timer();
    console::printf(
        "timer: backend=%s vector48=%u hw=%u target=%u sampled=%llu\n",
        timer_backend_name(timer::backend()),
        timer_test.software_irq_ok ? 1u : 0u,
        timer_test.hardware_irq_ok ? 1u : 0u,
        timer::frequency_hz(),
        timer_test.hardware_ticks
    );

    char line1[48] = {};
    char line2[48] = {};
    char line3[48] = {};
    char line4[48] = {};
    char line5[48] = {};
    char line6[48] = {};
    char line7[48] = {};

    {
        char* cursor = line1;
        append_text(cursor, "UEFI LIMINE BOOT OK");
        terminate_line(cursor);
    }
    {
        char* cursor = line2;
        append_text(cursor, "GDT IDT EXCEPTIONS OK");
        terminate_line(cursor);
    }
    {
        char* cursor = line3;
        append_text(cursor, "USABLE MEMORY ");
        append_decimal(cursor, mib_from_bytes(memory.usable_bytes));
        append_text(cursor, " MIB");
        terminate_line(cursor);
    }
    {
        char* cursor = line4;
        append_text(cursor, "PMM ");
        append_decimal(cursor, memory::total_page_count());
        append_text(cursor, " PAGES READY");
        terminate_line(cursor);
    }
    {
        char* cursor = line5;
        append_text(cursor, "HEAP ");
        append_decimal(cursor, heap::total_bytes());
        append_text(cursor, " BYTES READY");
        terminate_line(cursor);
    }
    {
        char* cursor = line6;
        append_text(cursor, "IRQ VECTOR 48 ");
        append_text(cursor, timer_test.software_irq_ok ? "OK" : "PENDING");
        terminate_line(cursor);
    }
    {
        char* cursor = line7;
        if (timer_test.hardware_irq_ok) {
            append_text(cursor, "LOCAL APIC TIMER ");
            append_decimal(cursor, timer::frequency_hz());
            append_text(cursor, " TARGET LIVE");
        } else {
            append_text(cursor, "LOCAL APIC TIMER PROBE PENDING");
        }
        terminate_line(cursor);
    }

    const char* welcome_lines[] = {
        line1,
        line2,
        line3,
        line4,
        line5,
        line6,
        line7,
    };
    console::show_welcome_screen(welcome_lines, sizeof(welcome_lines) / sizeof(welcome_lines[0]));
    console::write_line("screen: welcome panel rendered");
    console::write_line("idle: hlt loop");

    arch::x86_64::halt_forever();
}
