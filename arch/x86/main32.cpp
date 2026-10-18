#include <stdint.h>

#include "kernel/ata.hpp"
#include "kernel/block.hpp"
#include "kernel/fs.hpp"
#include "kernel/partition.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/vfs.hpp"

#include "boot_state.hpp"
#include "cpu.hpp"
#include "multiboot2.hpp"
#include "paging.hpp"
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

[[noreturn]] void halt_with(const char* message) {
    serial::write(message);
    serial::write("\nI386 BOOT FAIL\n");
    for (;;) {
        asm volatile("hlt");
    }
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

/* Monta el SxFS persistente del primer disco que lo reconozca y lo publica
 * bajo /disk. La pila de almacenamiento es la misma que la de x86_64: ATA PIO,
 * registro de dispositivos de bloque, driver MBR y SxFS sobre el VFS. */
void mount_root_filesystem() {
    vfs::initialize(nullptr, 0);
    block::register_driver(ata::driver());
    block::register_driver(partition::driver());
    block::probe_all();
    report_block_devices();
    boot_state::state().block_ready = block::device_count() != 0 ? 1 : 0;

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

namespace {

/* Programa del MVP: es el primero que el kernel i386 ejecuta en ring 3, asi
 * que tiene que ser uno que solo use syscalls de escritura y de consulta. */
constexpr const char* kUserProgramPath = "/disk/bin/sysinfo";

}

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

    uint32_t offset = 8;
    while (offset + 8 <= total_size) {
        const uint8_t* tag = info + offset;
        const uint32_t type = *reinterpret_cast<const uint32_t*>(tag);
        const uint32_t size = *reinterpret_cast<const uint32_t*>(tag + 4);

        if (type == static_cast<uint32_t>(TagType::end)) {
            break;
        }
        if (size < 8 || offset + size > total_size) {
            halt_with("savanxp/i386: etiqueta multiboot2 invalida");
        }

        switch (static_cast<TagType>(type)) {
            case TagType::cmdline:
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

    mount_root_filesystem();

    serial::write("I386 BOOT OK\n");

    /* Primer salto a ring 3 del objetivo: el proceso vive hasta que llama a
     * exit, y el kernel se para entonces. El token de arranque ya esta
     * escrito, asi que un fallo al cargar el programa no lo oculta. */
    user32::run(kUserProgramPath);

    for (;;) {
        asm volatile("hlt");
    }
}
