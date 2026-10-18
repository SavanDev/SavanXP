#include <stdint.h>

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

    serial::write("I386 BOOT OK\n");
    for (;;) {
        asm volatile("hlt");
    }
}
