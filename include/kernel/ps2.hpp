#pragma once

#include <stdint.h>

namespace ps2 {

void initialize();
bool ready();
bool mouse_ready();
bool set_layout(int layout);
int get_layout();
void poll();

// Punto de entrada para fuentes de teclado que no son el controlador 8042
// (virtio_input:: con un device virtio-keyboard). Reutiliza toda la logica de
// layout/modificadores/locks de ps2:: en vez de duplicarla: raw_code trae el
// bit de release igual que un byte real de scancode set 1 (0x80 puesto),
// extended indica si iria con el prefijo 0xE0.
void inject_scancode(uint8_t raw_code, bool extended);
// Para teclas que no pasan por la traduccion de scancode (Pause/PrintScreen):
// virtio-input las entrega como un evento limpio de press/release, a
// diferencia de las secuencias multi-byte sin release que manda un teclado
// PS/2 real para esas mismas teclas.
void inject_key_event(uint32_t key, bool pressed);

} // namespace ps2
