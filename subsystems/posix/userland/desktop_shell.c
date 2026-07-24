#include "desktop_shell.h"

void shell_state_init(struct shell_state *shell)
{
    if (shell == 0)
    {
        return;
    }

    shell->menu_open = 0;
    shell->selected_index = 0;
    shell->selected_shortcut = -1;
    shell->context_menu.open = 0;
    shell->context_menu.x = 0;
    shell->context_menu.y = 0;
    shell->context_menu.selected = -1;
    shell->confirm_action = DESKTOP_CONFIRM_NONE;
    shell->welcome_visible = 1;
    shell->welcome_until_ms = 0;
    shell->last_shortcut_click = -1;
    shell->last_shortcut_click_ms = 0;
    shell->last_clock_stamp = 0;
}
