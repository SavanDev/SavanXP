#include <stdint.h>

#include "kernel/ata.hpp"
#include "kernel/block.hpp"
#include "kernel/fs.hpp"
#include "kernel/partition.hpp"
#include "kernel/sxfs.hpp"
#include "kernel/vfs.hpp"

#include "multiboot2.hpp"
#include "paging.hpp"
#include "serial.hpp"

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
extern "C" uintptr_t __stack_chk_guard = 0;

extern "C" [[noreturn]] void __stack_chk_fail() {
    halt_with("savanxp/i386: fallo del canary de pila");
}

extern "C" [[noreturn]] void savanxp_i386_bootstrap(uint32_t magic, uint32_t info_address) {
    serial::init();
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
    for (;;) {
        asm volatile("hlt");
    }
}
