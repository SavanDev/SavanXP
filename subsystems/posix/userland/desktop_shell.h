#pragma once

#include "libc.h"
#include "desktop_menu.h"

/*
 * Estado de chrome del shell que main() arrastra entre frames.
 *
 * Fase A del boundary WM<->shell (ver docs/WM_SUBSYSTEM.md): en este paso el
 * estado vive como un bundle dentro del proceso desktop; en A2 se muda al
 * shell-client separado. Agruparlo aca es el prerrequisito para extraer los
 * handlers de input del chrome fuera del for(;;) de main().
 */
struct shell_state
{
    int menu_open;
    int selected_index;
    int selected_shortcut;
    struct desktop_context_menu_state context_menu;
    int confirm_action;
    int welcome_visible;
    unsigned long welcome_until_ms;
    int last_shortcut_click;
    unsigned long last_shortcut_click_ms;
    unsigned long last_clock_stamp;
};

/*
 * Inicializa el estado de chrome a sus valores de arranque: menu cerrado, sin
 * shortcut seleccionado, sin accion de energia pendiente, welcome visible.
 * welcome_until_ms y last_clock_stamp quedan en 0; main() los fija tras abrir
 * la sesion del compositor.
 */
void shell_state_init(struct shell_state *shell);
