#include "kernel/kernel.hpp"

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"
#include "kernel/console.hpp"
#include "kernel/cpu.hpp"
#include "kernel/device.hpp"
#include "kernel/heap.hpp"
#include "kernel/net.hpp"
#include "kernel/panic.hpp"
#include "kernel/pci.hpp"
#include "kernel/pcspeaker.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/process.hpp"
#include "kernel/ps2.hpp"
#include "kernel/svfs.hpp"
#include "kernel/timer.hpp"
#include "kernel/tty.hpp"
#include "kernel/ui.hpp"
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

void copy_string_field(char* destination, size_t capacity, const char* source) {
    if (destination == nullptr || capacity == 0) {
        return;
    }

    size_t index = 0;
    while (source != nullptr && source[index] != '\0' && index + 1 < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
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

    console::printf("%s booting...\n", SAVANXP_DISPLAY_NAME);

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
    device::initialize();
    pci::initialize();
    ui::initialize(boot_info.framebuffer);
    pcspeaker::initialize();
    net::initialize();
    block::initialize();
    svfs::initialize();

    if (!vfs::ready()) {
        panic("vfs: initramfs unavailable");
    }
    const bool disk_mounted = svfs::mount_at_root();

    const MemorySummary memory = summarize_memory(boot_info);
    savanxp_system_info system_info = {};
    copy_string_field(system_info.bootloader_name, sizeof(system_info.bootloader_name), boot_info.bootloader_name);
    copy_string_field(system_info.bootloader_version, sizeof(system_info.bootloader_version), boot_info.bootloader_version);
    copy_string_field(system_info.firmware, sizeof(system_info.firmware), firmware_type_name(boot_info.firmware_type));
    system_info.input_ready = ps2::ready() ? 1u : 0u;
    system_info.framebuffer_ready = ui::framebuffer_available() ? 1u : 0u;
    system_info.net_present = net::present() ? 1u : 0u;
    system_info.speaker_ready = pcspeaker::ready() ? 1u : 0u;
    system_info.block_ready = block::ready() ? 1u : 0u;
    system_info.svfs_mounted = disk_mounted ? 1u : 0u;
    system_info.timer_backend =
        timer::backend() == timer::Backend::local_apic ? SAVANXP_TIMER_LOCAL_APIC : SAVANXP_TIMER_NONE;
    system_info.timer_frequency_hz = timer::frequency_hz();
    system_info.framebuffer_width = static_cast<uint32_t>(boot_info.framebuffer.width);
    system_info.framebuffer_height = static_cast<uint32_t>(boot_info.framebuffer.height);
    system_info.framebuffer_bpp = boot_info.framebuffer.bpp;
    system_info.pci_device_count = static_cast<uint32_t>(pci::device_count());
    system_info.svfs_file_count = static_cast<uint32_t>(svfs::file_count());
    system_info.memory_usable_bytes = memory.usable_bytes;
    system_info.memory_reclaimable_bytes = memory.reclaimable_bytes;
    system_info.memory_total_pages = memory::total_page_count();
    system_info.initramfs_size = boot_info.initramfs_size;
    process::set_boot_system_info(system_info);

    console::printf(
        "boot ready: %u MiB usable, %u MiB reclaimable, video %ux%u\n",
        static_cast<unsigned>(mib_from_bytes(memory.usable_bytes)),
        static_cast<unsigned>(mib_from_bytes(memory.reclaimable_bytes)),
        system_info.framebuffer_width,
        system_info.framebuffer_height
    );
    console::printf(
        "handoff: starting /bin/init (%s, /disk %s)\n",
        system_info.net_present != 0 ? "net online" : "net absent",
        disk_mounted ? "mounted" : "offline"
    );
    console::write_line("");

    process::start_init("/bin/init");
}
