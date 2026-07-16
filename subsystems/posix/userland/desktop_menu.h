#pragma once

#include "libc.h"
#include "desktop_icons.h"

#define DESKTOP_MENU_ITEM_FLAG_SHORTCUT 0x00000001u
/* App can take over the whole screen via composited fullscreen (F11): its
 * client surface is allocated at a low render size and scaled by the shell. */
#define DESKTOP_MENU_ITEM_FLAG_FULLSCREEN 0x00000002u

struct desktop_menu_item
{
    const char *label;
    const char *path;
    const char *subtitle;
    enum desktop_icon_id icon_id;
    uint32_t accent;
    uint32_t flags;
};

/* Accion de energia pendiente de confirmacion. */
#define DESKTOP_CONFIRM_NONE 0
#define DESKTOP_CONFIRM_SHUTDOWN 1
#define DESKTOP_CONFIRM_REBOOT 2

/* Menu contextual del escritorio (click derecho sobre el fondo). */
enum desktop_context_action
{
    DESKTOP_CONTEXT_ACTION_REFRESH = 0,
    DESKTOP_CONTEXT_ACTION_NEXT_WALLPAPER,
    DESKTOP_CONTEXT_ACTION_ABOUT,
};

struct desktop_context_item
{
    const char *label;
    int action; /* DESKTOP_CONTEXT_ACTION_* */
    int separator_before;
};

/* Estado del menu contextual que main() arrastra entre frames. x/y ya vienen
 * clampeados por desktop_context_menu_place() al abrirlo, asi layout, render
 * e hit-test parten de la misma esquina. */
struct desktop_context_menu_state
{
    int open;
    int x;
    int y;
    int selected;
};

struct desktop_power_item
{
    const char *label;
    int confirm; /* DESKTOP_CONFIRM_* */
};

int desktop_menu_item_count(void);
const struct desktop_menu_item *desktop_menu_item_at(int index);
const struct desktop_menu_item *desktop_find_menu_item_by_path(const char *path);
int desktop_shortcut_count(void);
const struct desktop_menu_item *desktop_shortcut_at(int index);

int desktop_power_item_count(void);
const struct desktop_power_item *desktop_power_item_at(int index);

int desktop_context_item_count(void);
const struct desktop_context_item *desktop_context_item_at(int index);
