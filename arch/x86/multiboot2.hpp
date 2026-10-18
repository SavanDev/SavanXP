#pragma once

#include <stdint.h>

/*
 * Constantes y etiquetas del protocolo Multiboot2.
 *
 * GRUB2 lee la cabecera que entry32.S deja en .multiboot2, entra en modo
 * protegido de 32 bits sin paginacion y salta a _start con EAX = kMagic y
 * EBX = direccion fisica de la estructura de informacion. De esa estructura
 * el bootstrap solo usa lo que necesita para informar por serie: cmdline,
 * nombre del cargador, memoria basica, mapa de memoria y framebuffer.
 */

namespace i386 {

/* EAX al entrar: firma del cargador Multiboot2. */
constexpr uint32_t kMagic = 0x36D76289;

enum class TagType : uint32_t {
    end = 0,
    cmdline = 1,
    bootloader_name = 2,
    memory_info = 4,
    memory_map = 6,
    framebuffer = 8,
};

/* Cabecera comun de toda etiqueta. */
struct Tag {
    uint32_t type;
    uint32_t size; /* incluye la cabecera; las etiquetas van alineadas a 8 */
};

/*
 * Las siguientes estructuras describen solo los campos que se leen, no el
 * tamano exacto de la etiqueta: los campos posteriores (y el relleno) se
 * ignoran. Las cadenas de cmdline y bootloader_name empiezan en el byte 8 y
 * terminan en el primer '\0' dentro de la etiqueta, asi que no hay struct.
 */

struct MemoryInfoTag {
    uint32_t type;
    uint32_t size;
    uint32_t lower_kib;
    uint32_t upper_kib;
};

struct MemoryMapTag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct MemoryMapEntry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

struct FramebufferTag {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t color_type;
};

}
