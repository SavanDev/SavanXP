#pragma once

#include <stdint.h>

namespace i386 {

namespace egatext {

/* Consola de texto del kernel i386 sobre el modo que GRUB2 deja activo: con
 * `-display none` el tag Multiboot2 framebuffer es texto 80x25 tipo 2 en
 * 0xB8000, no un framebuffer grafico. Es la mitad "consola de texto sobre el
 * framebuffer EGA" del paquete "PS/2 and the desktop"
 * (docs/REAL_HARDWARE.md): serial:: escribe aqui tambien, asi que el log del
 * kernel y la salida del proceso se ven en pantalla. */
constexpr uint16_t kColumns = 80;
constexpr uint16_t kRows = 25;
constexpr uint32_t kTextBase = 0x000B8000u;
constexpr uint32_t kTypeEgaText = 2;

/* Atributo VGA: fondo negro, gris claro. */
constexpr uint8_t kAttribute = 0x07;

/* Se activa solo con un tag de texto 80x25 apuntando a 0xB8000; cualquier
 * otro modo (VBE grafico) se ignora antes que escribir en memoria que no es
 * texto. Limpia la pantalla. */
void initialize(uint32_t width, uint32_t height, uint8_t color_type, uint64_t address);

bool ready();

void clear();

void write_char(char character);

void write(const char* text);

}  // namespace egatext

}  // namespace i386
