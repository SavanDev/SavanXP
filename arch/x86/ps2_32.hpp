#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

namespace i386 {

namespace ps2_32 {

/* Rebanada completa del paquete "PS/2 and the desktop" en i386: el
 * controlador 8042 por IRQ 1, la decodificacion set 1 con los layouts
 * ES/EN de kernel/ps2.cpp y la cola de eventos savanxp_input_event que el
 * userland lee por la syscall READ sobre stdin. */

void initialize();

/* Hay evento pendiente para la syscall READ. */
bool has_pending_event();

/* Saca el evento mas viejo a `out`. false si no habia ninguno. */
bool pop_event(struct savanxp_input_event& out);

/* Ultimo ASCII imprimible (para el eco de la consola EGA). 0 si ninguno. */
char last_ascii();

/* Ultimo codigo de tecla decodificado (SAVANXP_KEY_*). */
uint32_t last_key();

/* Estado de modificadores actual (SAVANXP_KEY_MOD_*). */
uint32_t modifiers();

/* Layout del teclado (SAVANXP_KEYBOARD_LAYOUT_*), igual que ps2:: en
 * x86_64: ES por defecto, con EN como alternativa. */
bool set_layout(int layout);
int get_layout();

}  // namespace ps2_32

}  // namespace i386
