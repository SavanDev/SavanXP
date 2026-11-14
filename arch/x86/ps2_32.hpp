#pragma once

#include <stddef.h>
#include <stdint.h>

#include "savanxp/syscall.h"

namespace i386 {

namespace ps2_32 {

/* Legacy 8042/PS2 input with the reference ES/EN layouts. Canonical TTY
 * consumers receive lines; raw clients read savanxp_input_event records from
 * /dev/input0 (diagnostics can explicitly duplicate that descriptor to stdin). */

void initialize();

/* Hay evento pendiente para la syscall READ. */
bool has_pending_event();

/* Saca el evento mas viejo a `out`. false si no habia ninguno. */
bool pop_event(struct savanxp_input_event& out);

/* Clear boot-time events when starting the scheduler. Spawning a program
 * does not clear the shared queue. */
void clear_queue();

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
