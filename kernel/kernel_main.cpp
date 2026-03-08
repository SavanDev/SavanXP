#include "kernel/kernel.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/heap.hpp"
#include "kernel/panic.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/ps2.hpp"
#include "kernel/svfs.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/vfs.hpp"
#include "kernel/vmm.hpp"
#include "shared/version.h"

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
};

MemorySummary summarize_memory(const boot::BootInfo& boot_info) {
    MemorySummary summary = {};
    for (size_t index = 0; index < boot_info.memory_map_entries; ++index) {
        const boot::MemoryRegion& entry = boot_info.memory_map[index];
        if (entry.type == boot::MemoryRegionType::usable) {
            summary.usable_bytes += entry.length;
        } else if (entry.type == boot::MemoryRegionType::acpi_reclaimable ||
                   entry.type == boot::MemoryRegionType::bootloader_reclaimable) {
            summary.reclaimable_bytes += entry.length;
        }
    }
    return summary;
}

} // namespace

[[noreturn]] void kernel_main(const boot::BootInfo& boot_info) {
    arch::x86_64::initialize_cpu();

    console::printf("=== %s bootstrap ===\n", SAVANXP_DISPLAY_NAME);
    console::printf(
        "boot: %s %s on %s\n",
        boot_info.bootloader_name != nullptr ? boot_info.bootloader_name : "unknown",
        boot_info.bootloader_version != nullptr ? boot_info.bootloader_version : "unknown",
        firmware_type_name(boot_info.firmware_type)
    );

    memory::initialize(boot_info);
    if (!memory::ready()) {
        panic("pmm: no usable memory");
    }

    heap::initialize();
    if (!heap::ready()) {
        panic("heap: bootstrap failed");
    }

    vm::initialize(boot_info);
    if (!vm::ready()) {
        panic("vmm: bootstrap failed");
    }

    tty::initialize();
    timer::initialize(100);
    ps2::initialize();
    process::initialize();
    vfs::initialize(boot_info.initramfs_address, static_cast<size_t>(boot_info.initramfs_size));
    block::initialize();
    svfs::initialize();

    if (!vfs::ready()) {
        panic("vfs: initramfs unavailable");
    }
    const bool disk_mounted = svfs::mount_at_root();

    const MemorySummary memory = summarize_memory(boot_info);
    console::printf(
        "memory: usable=%llu MiB reclaimable=%llu MiB pages=%llu\n",
        mib_from_bytes(memory.usable_bytes),
        mib_from_bytes(memory.reclaimable_bytes),
        memory::total_page_count()
    );
    console::printf(
        "video: %llux%llu tty grid online\n",
        boot_info.framebuffer.width,
        boot_info.framebuffer.height
    );
    console::printf(
        "timer: %s at %u Hz\n",
        timer::backend() == timer::Backend::local_apic ? "local apic" : "none",
        timer::frequency_hz()
    );
    console::printf(
        "input: ps2=%u tty=1 initramfs=%llu bytes\n",
        ps2::ready() ? 1u : 0u,
        boot_info.initramfs_size
    );
    console::printf(
        "disk: block=%u svfs=%u files=%llu mount=%s\n",
        block::ready() ? 1u : 0u,
        svfs::mounted() ? 1u : 0u,
        static_cast<unsigned long long>(svfs::file_count()),
        disk_mounted ? "/disk" : "unavailable"
    );
    console::printf(
        "system: %s terminal + shell + sdk v1 + persistent /disk\n",
        SAVANXP_VERSION_STRING
    );
    console::write_line("userland: init + shell + tests (ps/fdtest/waittest/pipestress/spawnloop/badptr)");
    console::write_line("");

    process::start_init("/bin/init");
}
