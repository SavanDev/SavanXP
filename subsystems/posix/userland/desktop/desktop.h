#pragma once

#include "libc.h"
#include "savanxp/sxgui.h"
#include "savanxp/wm_shell_protocol.h"

/*
 * The desktop: one process that owns everything the shell shows -- the
 * background, the taskbar, the Start menu and the taskbar's popups -- as
 * windows with roles (docs/desktop/DESKTOP_ARCHITECTURE.md). windowd starts it
 * as the shell and knows nothing about what any of those windows shows: their
 * sizes, places and behaviour are decided here.
 *
 * desktop.c runs the windows and the loop; every other desktop_*.c is a module
 * that paints one window and reacts to its pointer.
 */

/* Window ids of the desktop process (savanxp_wm_window_request.window_id).
 * 0 is the main window, which windowd gives the background role. The Start
 * menu and the tray popups have an id each because they can be open at the
 * same time; the three tray popups share a corner and an id, so at most one of
 * them is open. */
enum desktop_window_id
{
    DESKTOP_WINDOW_BACKGROUND = 0,
    DESKTOP_WINDOW_TASKBAR = 1,
    DESKTOP_WINDOW_START_MENU = 2,
    DESKTOP_WINDOW_TRAY_POPUP = 3,
    /* "Not responding": centred, a question rather than a menu. */
    DESKTOP_WINDOW_HUNG_DIALOG = 4,
};

enum desktop_popup
{
    DESKTOP_POPUP_START_MENU = 0,
    DESKTOP_POPUP_VOLUME = 1,
    DESKTOP_POPUP_CLOCK = 2,
    DESKTOP_POPUP_KEYBOARD = 3,
    DESKTOP_POPUP_HUNG = 4,
    DESKTOP_POPUP_COUNT = 5,
};

/* Bits a module's pointer or tick handler returns. */
#define DESKTOP_ACTION_NONE 0u
#define DESKTOP_ACTION_REPAINT 0x1u
/* Close the popup that returned it (a layout was chosen, a program launched). */
#define DESKTOP_ACTION_CLOSE 0x2u
/* Something the taskbar shows changed (volume, mute, keyboard layout): it
 * refreshes now instead of at its next poll. */
#define DESKTOP_ACTION_TASKBAR_CHANGED 0x4u

/* The taskbar's strip at the bottom of the screen. Its thickness is the
 * desktop's to choose; windowd reserves whatever the panel asks for. */
#define DESKTOP_TASKBAR_HEIGHT 28

/* One popup kind. `width` and `height` are its surface; `anchor_right` puts it
 * at the right end of the taskbar (the tray corner) instead of the left. */
struct desktop_popup_ops
{
    int width;
    int height;
    int anchor_right;
    /* Prepares the popup's state each time it opens. Non-zero refuses to open
     * (for instance the volume popup without an audio device). */
    int (*open)(void);
    void (*close)(void);
    uint32_t (*pointer)(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer);
    /* Called every loop; may be 0. */
    uint32_t (*tick)(void);
    void (*paint)(struct savanxp_gfx_context *gfx);
    /* 1: centred on the screen and not dismissed by a press elsewhere (a
     * dialog); 0: anchored to the taskbar, closed by a press elsewhere. */
    int centered;
};

extern const struct desktop_popup_ops desktop_startmenu_ops;
extern const struct desktop_popup_ops desktop_volume_ops;
extern const struct desktop_popup_ops desktop_clock_ops;
extern const struct desktop_popup_ops desktop_keyboard_ops;
extern const struct desktop_popup_ops desktop_hung_ops;

/* desktop_hung.c: the window the dialog is about (its id is its pid). */
void desktop_hung_set_target(uint32_t window_id, const char *title);
uint32_t desktop_hung_target(void);
/* desktop.c: the user answered for this window; do not ask again until windowd
 * clears and sets its flag anew. */
void desktop_hung_dismissed(uint32_t window_id);

/* What desktop.c offers the modules. */
void desktop_toggle_popup(enum desktop_popup popup);
void desktop_close_popups(void);
int desktop_popup_is_open(enum desktop_popup popup);
/* Starts a program; it connects to windowd for its own window. */
long desktop_launch(const char *path, uint32_t launch_flags);
/* Sends a request on the shell's request pipe (savanxp/wm_shell_protocol.h). */
void desktop_shell_request(const struct savanxp_wm_shell_request *request);

/* The Win95 bevel every desktop surface draws with. */
void desktop_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken);

/* desktop_taskbar.c */
int desktop_taskbar_init(void);
void desktop_taskbar_shutdown(void);
uint32_t desktop_taskbar_pointer(const struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer);
uint32_t desktop_taskbar_tick(void);
/* Re-reads volume, mute and keyboard layout now. Non-zero if any changed. */
int desktop_taskbar_refresh_indicators(void);
void desktop_taskbar_paint(struct savanxp_gfx_context *gfx);
/* The first window windowd marks as not responding: 1 with its id and title,
 * 0 if none. From the last snapshot of the window list. */
int desktop_taskbar_not_responding(uint32_t *window_id, char *title, size_t capacity);
