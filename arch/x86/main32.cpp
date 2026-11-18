#include <stdint.h>

#include "kernel/ata.hpp"
#include "kernel/block.hpp"
#include "kernel/boot_screen.hpp"
#include "kernel/fs.hpp"
#include "kernel/partition.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/ramdisk.hpp"
#include "kernel/pci.hpp"
#include "kernel/ioapic.hpp"
#include "kernel/virtio_blk.hpp"
#include "kernel/virtio_gpu.hpp"
#include "kernel/virtio_input.hpp"
#include "kernel/ui.hpp"
#include "kernel/timer.hpp"
#include "kernel/vfs.hpp"
#include "kernel/string.hpp"
#include "shared/version.h"

#include "boot_state.hpp"
#include "cpu.hpp"
#include "egatext.hpp"
#include "fb32.hpp"
#include "kernel/display.hpp"
#include "kernel/device.hpp"
#include "kernel/fb_gpu.hpp"
#include "kernel/gpu_device.hpp"
#include "multiboot2.hpp"
#include "memory.hpp"
#include "io.hpp"
#include "kernel/physical_memory.hpp"
#include "kernel/heap.hpp"
#include "paging.hpp"
#include "services.hpp"
#include "smp.hpp"
#include "ps2_32.hpp"
#include "serial.hpp"
#include "user32.hpp"

/*
 * Bootstrap del kernel i386 (docs/REAL_HARDWARE.md).
 *
 * Lo llaman desde entry32.S con la firma Multiboot2 y la direccion fisica de
 * la estructura de informacion. Aqui no hay kernel completo todavia: se
 * informa por serie lo que dejo el cargador y se enciende la paginacion de
 * identidad (0..1 GiB, paginas de 4 MiB). El token "I386 BOOT OK" es lo que
 * espera tools/legacy_boot_test.py.
 */

namespace {

using namespace i386;

bool g_console_boot;

[[noreturn]] void halt_with(const char* message) {
    cpu::disable_interrupts();
    boot_screen::finish();
    fb32::console_set_enabled(true);
    serial::write(message);
    serial::write("\nI386 BOOT FAIL\n");
    cpu::halt_forever();
}

/* Las cadenas (cmdline, nombre del cargador) empiezan en el byte 8 de la
 * etiqueta y no salen de ella. */
void write_tag_string(const uint8_t* tag) {
    const uint32_t size = *reinterpret_cast<const uint32_t*>(tag + 4);
    const char* text = reinterpret_cast<const char*>(tag + 8);
    for (uint32_t i = 8; i < size && text[i - 8] != '\0'; ++i) {
        serial::write_char(text[i - 8]);
    }
}

void report_memory_info(const uint8_t* tag) {
    const auto* info = reinterpret_cast<const MemoryInfoTag*>(tag);
    serial::write("memoria: ");
    serial::write_dec(info->lower_kib);
    serial::write(" KiB bajos, ");
    serial::write_dec(info->upper_kib);
    serial::write(" KiB altos\n");
    boot_state::state().memory_usable_bytes =
        static_cast<uint64_t>(info->lower_kib + info->upper_kib) * 1024ull;
}

/* Las etiquetas de cadena de Multiboot2 son un bloque de NUL terminated: la
 * longitud esta en el offset 4 de la etiqueta y el texto despues. */
size_t copy_tag_text(const uint8_t* tag, uint32_t start, char* destination, size_t capacity) {
    if (capacity == 0) {
        return 0;
    }
    const uint32_t available = *(reinterpret_cast<const uint32_t*>(tag + 4));
    const uint8_t* source = tag + start;
    size_t written = 0;
    while (written + 1 < capacity && written < available && source[written] != '\0') {
        destination[written] = static_cast<char>(source[written]);
        ++written;
    }
    destination[written] = '\0';
    return written;
}

void store_bootloader(const uint8_t* tag) {
    boot_state::State& state = boot_state::state();
    const size_t name_length = copy_tag_text(tag, 8, state.bootloader_name, sizeof(state.bootloader_name));
    copy_tag_text(tag, 8 + static_cast<uint32_t>(name_length) + 1u, state.bootloader_version,
                  sizeof(state.bootloader_version));
    /* GRUB2 escribe el nombre y la version como dos cadenas separadas, pero si
     * la segunda no esta, al menos el nombre es honesto: se parte por el ultimo
     * espacio para no dejar la version en "unknown" cuando si esta ahi. */
    if (state.bootloader_version[0] == '\0') {
        size_t split = 0;
        for (size_t index = 0; state.bootloader_name[index] != '\0'; ++index) {
            if (state.bootloader_name[index] == ' ') {
                split = index;
            }
        }
        if (split != 0) {
            for (size_t index = split + 1; index < sizeof(state.bootloader_name); ++index) {
                const size_t target = index - split - 1;
                if (target >= sizeof(state.bootloader_version)) {
                    break;
                }
                state.bootloader_version[target] = state.bootloader_name[index];
            }
            state.bootloader_name[split] = '\0';
        }
    }
    /* "BIOS" porque esta ISO solo arranca por BIOS: GRUB2 no expone una
     * etiqueta de firmware y el camino UEFI no existe todavia en i386. */
    const char firmware[] = "BIOS";
    for (size_t index = 0; index < sizeof(state.firmware) - 1 && firmware[index] != '\0'; ++index) {
        state.firmware[index] = firmware[index];
    }
}

void report_memory_map(const uint8_t* tag) {
    const auto* map = reinterpret_cast<const MemoryMapTag*>(tag);
    if (map->size < 16 || map->entry_size < sizeof(MemoryMapEntry)) {
        halt_with("memory: invalid Multiboot2 memory map");
    }
    const uint8_t* entries = tag + 16;
    const uint8_t* end = tag + map->size;
    uint32_t count = 0;

    serial::write("mapa de memoria: entradas de ");
    serial::write_dec(map->entry_size);
    serial::write(" bytes\n");

    while (entries + map->entry_size <= end && count < 32) {
        const auto* entry = reinterpret_cast<const MemoryMapEntry*>(entries);
        serial::write("  [");
        serial::write_dec(count);
        serial::write("] base ");
        serial::write_hex64(entry->base);
        serial::write(" longitud ");
        serial::write_hex64(entry->length);
        serial::write(" tipo ");
        serial::write_dec(entry->type);
        serial::write("\n");
        /* Tipos 3 (ACPI reclaimable) y 4 (ACPI NVS): memoria que el firmware todavia
     * usa y que el kernel podria reclaimar. Se acumula en el mismo recorrido
     * que imprime, asi que cuenta hasta las 32 primeras entradas. */
        if (entry->type == 3 || entry->type == 4) {
            boot_state::state().memory_reclaimable_bytes += entry->length;
        }
        entries += map->entry_size;
        ++count;
    }
}

void report_framebuffer(const uint8_t* tag) {
    const auto* framebuffer = reinterpret_cast<const FramebufferTag*>(tag);
    serial::write("framebuffer: ");
    serial::write_dec(framebuffer->width);
    serial::write("x");
    serial::write_dec(framebuffer->height);
    serial::write(" ");
    serial::write_dec(framebuffer->bpp);
    serial::write(" bpp tipo ");
    serial::write_dec(framebuffer->color_type);
    serial::write(" en ");
    serial::write_hex64(framebuffer->address);
    serial::write("\n");
    boot_state::state().framebuffer_ready = 1;
    boot_state::state().framebuffer_width = framebuffer->width;
    boot_state::state().framebuffer_height = framebuffer->height;
    boot_state::state().framebuffer_bpp = framebuffer->bpp;
    boot_state::state().framebuffer_pitch = framebuffer->pitch;
    boot_state::state().framebuffer_address = framebuffer->address;
    boot_state::state().framebuffer_color_type = framebuffer->color_type;
    // Multiboot2 RGB fields: position and mask size for red, green and blue.
    boot_state::state().framebuffer_bgrx = framebuffer->size >= 38 &&
        framebuffer->color_type == 1 && tag[32] == 16 && tag[33] == 8 &&
        tag[34] == 8 && tag[35] == 8 && tag[36] == 0 && tag[37] == 8;

    /* Consola de texto: solo si el cargador dejo el modo texto EGA que este
     * kernel sabe pintar (80x25 tipo 2 sobre 0xB8000). El token se emite
     * despues del clear, porque el clear borra la pantalla pero no la serie. */
    egatext::initialize(framebuffer->width, framebuffer->height, framebuffer->color_type,
                        framebuffer->address);
    if (egatext::ready()) {
        serial::write("consola: texto EGA 80x25 en 0x000b8000\n");
    }
}

// Map the GRUB BGRX framebuffer before either the splash or GPU probes use it.
// Unsupported modes keep the serial and, if provided, EGA console.
boot::FramebufferInfo initialize_video() {
    const boot_state::State& boot = boot_state::state();
    if (boot.framebuffer_ready == 0 || boot.framebuffer_bgrx == 0 ||
        !fb32::initialize(boot.framebuffer_address, boot.framebuffer_pitch, boot.framebuffer_width,
                          boot.framebuffer_height, static_cast<uint8_t>(boot.framebuffer_bpp),
                          boot.framebuffer_color_type)) {
        return {};
    }
    serial::write("console: framebuffer 8x16 at ");
    serial::write_hex64(fb32::address());
    serial::write(", ");
    serial::write_dec(fb32::info().width);
    serial::write("x");
    serial::write_dec(fb32::info().height);
    serial::write(", ");
    serial::write_dec(fb32::info().pitch);
    serial::write(" bytes per row\n");

    return {reinterpret_cast<void*>(static_cast<uintptr_t>(fb32::address())),
        fb32::info().width, fb32::info().height, fb32::info().pitch,
        static_cast<uint16_t>(fb32::info().bpp), true, fb32::info().buffer_size};
}

void report_block_devices() {
    const size_t count = block::device_count();
    serial::write("block: ");
    serial::write_dec(static_cast<uint32_t>(count));
    serial::write(" dispositivo(s)\n");
    for (size_t index = 0; index < count; ++index) {
        block::DeviceInfo info = {};
        if (!block::device_info(index, info) || !info.present) {
            continue;
        }
        serial::write("  ");
        serial::write(info.name);
        serial::write(": ");
        serial::write_dec(info.sector_count);
        serial::write(" sectores, ");
        serial::write(info.writable ? "rw" : "ro");
        serial::write("\n");
    }
}

// Mount the first recognized SxFS volume at /disk. Reference driver priority
// selects virtio, direct ATA, then the optional LiveCD RAM image.
void mount_root_filesystem() {
    const auto& boot = boot_state::state();
    vfs::initialize(reinterpret_cast<const void*>(boot.initramfs_address + paging::kDirectMapBase), boot.initramfs_size);
    if (!vfs::ready()) halt_with("vfs: invalid initramfs");
    block::register_driver(ata::driver());
    block::register_driver(virtio_blk::driver());
    block::register_driver(ramdisk::driver());
    if (boot.disk_image_address && boot.disk_image_size)
        ramdisk::attach_image(reinterpret_cast<void*>(boot.disk_image_address + paging::kDirectMapBase),
            boot.disk_image_size, true, "live0");
    block::register_driver(partition::driver());
    block::probe_all();
    report_block_devices();
    boot_state::state().block_ready = block::device_count() != 0 ? 1 : 0;

    boot_screen::show("Mounting storage");
    fs::initialize();
    sxfs::initialize();
    fs::register_driver(sxfs::driver());

    const size_t mount = fs::mount_any(sxfs::kRootMountPoint);
    if (mount == fs::kInvalidMount || !fs::attach(mount) || !vfs::ready()) {
        serial::write("sxfs: sin volumen montado en ");
        serial::write(sxfs::kRootMountPoint);
        serial::write("\n");
        return;
    }

    fs::MountInfo info = {};
    if (!fs::mount_info(mount, info)) {
        serial::write("sxfs: el montaje no expone su volumen\n");
        return;
    }
    boot_state::state().sxfs_mounted = 1;

    serial::write("sxfs: ");
    serial::write_dec(static_cast<uint32_t>(sxfs::file_count(info.volume)));
    serial::write(" ficheros, ");
    serial::write_dec(static_cast<uint32_t>(sxfs::used_bytes(info.volume) / 1024u));
    serial::write(" KiB usados de ");
    serial::write_dec(static_cast<uint32_t>(sxfs::total_bytes(info.volume) / 1024u));
    serial::write(" KiB\n");

    const vfs::Vnode* root = vfs::resolve(sxfs::kRootMountPoint);
    if (root == nullptr) {
        serial::write("sxfs: el montaje no aparece en el vfs\n");
        return;
    }
    const size_t children = vfs::child_count(*root);
    serial::write("sxfs: ");
    serial::write_dec(static_cast<uint32_t>(children));
    serial::write(" entrada(s) en ");
    serial::write(sxfs::kRootMountPoint);
    serial::write("\n");
    for (size_t index = 0; index < children; ++index) {
        const vfs::Vnode* child = vfs::child_at(*root, index);
        if (child == nullptr) {
            continue;
        }
        serial::write("  ");
        serial::write(child->name);
        if (child->type == vfs::NodeType::directory) {
            serial::write("/\n");
        } else {
            serial::write(" (");
            serial::write_dec(static_cast<uint32_t>(child->size));
            serial::write(" B)\n");
        }
    }
    serial::write("SXFS ROOT OK\n");
}

}

/* -fstack-protector-strong viene en el perfil del kernel; en x86_64 estas dos
 * piezas las ponen cpu_init.cpp y panic.cpp. Aqui todavia no hay panic: el
 * fallo se informa por serie y se detiene la maquina. */
namespace i386 {

namespace boot_state {

State g_state = {};

}

} // namespace i386

[[noreturn]] void panic(const char* message) { halt_with(message); }

extern "C" uintptr_t __stack_chk_guard = 0;

extern "C" [[noreturn]] void __stack_chk_fail() {
    halt_with("savanxp/i386: fallo del canary de pila");
}

extern "C" [[noreturn]] void savanxp_i386_bootstrap(uint32_t magic, uint32_t info_address) {
    serial::init();
    cpu::initialize();
    serial::write("cpu: gdt de 32 bits con segmentos de usuario y tss, idt con int $0x80\n");
    serial::write("savanxp/i386: arranque multiboot2\n");
    serial::write("firma ");
    serial::write_hex(magic);
    serial::write(", info ");
    serial::write_hex(info_address);
    serial::write("\n");

    if (magic != kMagic) {
        halt_with("savanxp/i386: firma multiboot2 incorrecta");
    }

    /* Paginacion apagada: la direccion fisica que dejo GRUB es directamente
     * usable como puntero. */
    const auto* info = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(info_address));
    const uint32_t total_size = *reinterpret_cast<const uint32_t*>(info);
    if (total_size < 16 || total_size > 0x40000000u ||
        info_address >= 0x40000000u - total_size) {
        halt_with("memory: Multiboot2 information is outside the bootstrap map");
    }

    uint32_t offset = 8;
    while (offset + 8 <= total_size) {
        const uint8_t* tag = info + offset;
        const uint32_t type = *reinterpret_cast<const uint32_t*>(tag);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(tag + 4);

        if (type == static_cast<uint32_t>(TagType::end)) {
            break;
        }
        if (size < 8 || size > total_size - offset) {
            halt_with("savanxp/i386: invalid Multiboot2 tag");
        }

        if (type == 3) {
            const auto* words = reinterpret_cast<const uint32_t*>(tag);
            if (size < 16 || words[3] < words[2] || words[3] > paging::kDirectMapBytes)
                halt_with("memory: invalid Multiboot2 module");
            if (size >= 21 && memcmp(tag + 16, "sxfs", 5) == 0) {
                boot_state::state().disk_image_address = words[2];
                boot_state::state().disk_image_size = words[3] - words[2];
            } else if (boot_state::state().initramfs_address == 0) {
                boot_state::state().initramfs_address = words[2];
                boot_state::state().initramfs_size = words[3] - words[2];
            }
        }
        switch (static_cast<TagType>(type)) {
            case TagType::cmdline:
                // An explicit diagnostic boot keeps the console regression
                // suite available without changing normal desktop startup.
                g_console_boot = strcmp(reinterpret_cast<const char*>(tag + 8), "console-init") == 0;
                serial::write("cmdline: '");
                write_tag_string(tag);
                serial::write("'\n");
                break;
            case TagType::bootloader_name:
                serial::write("cargador: ");
                write_tag_string(tag);
                store_bootloader(tag);
                serial::write("\n");
                break;
            case TagType::memory_info:
                report_memory_info(tag);
                break;
            case TagType::memory_map:
                report_memory_map(tag);
                break;
            case TagType::framebuffer:
                report_framebuffer(tag);
                break;
            default:
                break;
        }

        offset = (offset + size + 7u) & ~7u;
    }

    if (!paging::cpu_has_pse()) {
        halt_with("savanxp/i386: la CPU no tiene PSE (paginas de 4 MiB)");
    }
    paging::enable_identity_1gib();
    serial::write("paginacion: identidad 0..1 GiB, 4 MiB por pagina, sin NX\n");
    timer::calibrate_monotonic();
    auto framebuffer = initialize_video();
    if (!g_console_boot) {
        boot_screen::initialize(framebuffer);
        if (boot_screen::ready()) {
            fb32::console_set_enabled(false);
            boot_screen::show("Preparing CPU");
        }
    }
    serial::write(SAVANXP_DISPLAY_NAME " booting...\n");
    boot_screen::show("Initializing physical memory");
    if (!initialize_memory(info_address)) {
        halt_with("memory: cannot initialize physical allocator");
    }
    serial::write("memory: reference allocator ready, free pages ");
    serial::write_dec(static_cast<uint32_t>(memory::free_page_count()));
    serial::write("\n");

    boot_screen::show("Initializing heap");
    heap::initialize();
    if (!heap::ready()) halt_with("memory: cannot initialize kernel heap");

    // PCI storage needs discovery before probing and mounting the root.
    boot_screen::show("Scanning PCI bus");
    pci::initialize();
    boot_screen::show("Loading userland");
    mount_root_filesystem();
    device::initialize();
    boot_screen::show("Detecting firmware");
    const uint32_t rsdp_physical = services::initialize_firmware(info_address);
    timer::adopt_pm_timer();
    i386::smp::initialize(info_address);
    timer::initialize(1000);
    ioapic::initialize();
    boot_screen::show("Initializing input");
    ps2_32::initialize();
    services::initialize(rsdp_physical);
    boot_screen::show("Preparing display");
    display::register_driver(fb_gpu::driver());
    display::register_driver(virtio_gpu::driver());
    display::bind_best(framebuffer);
    ui::sync_framebuffer_geometry();
    framebuffer.width = display::framebuffer_info().width;
    framebuffer.height = display::framebuffer_info().height;
    virtio_input::initialize(framebuffer);
    gpu_device::initialize();
    io::initialize();

    serial::write("I386 BOOT OK\n");

    // Userland init owns boot policy; the kernel only starts its first process.
    if (boot_state::state().sxfs_mounted) {
        boot_screen::show("Starting session");
    } else if (boot_screen::ready()) {
        boot_screen::finish();
        fb32::console_set_enabled(true);
        fb32::console_clear();
        serial::write("handoff: no SxFS volume available for the session\n");
    }
    user32::run(g_console_boot ? "/disk/bin/init32" : "/bin/init");
}
