#pragma once

#include "libc.h"
#include "desktop_icons.h"

/*
 * Window presentation: title, small icon, accent and window style.
 *
 * The window declares them about itself (docs/desktop/DESKTOP_ARCHITECTURE.md
 * #presentation-comes-from-the-client): its runtime reads the program's own
 * .sxe and publishes them in the surface header (savanxp_wm_presentation).
 * windowd never opens a binary and keeps no table of known programs. Until a
 * window publishes -- or if it never does -- it shows its file name and the
 * generic icon.
 */

#define WINDOWD_PRESENTATION_LABEL_CAPACITY 32
/* The window frame and the Alt+Tab switcher draw the small icon. */
#define WINDOWD_PRESENTATION_ICON_EXTENT 16u
#define WINDOWD_PRESENTATION_ICON_PIXELS \
    (WINDOWD_PRESENTATION_ICON_EXTENT * WINDOWD_PRESENTATION_ICON_EXTENT)

/*
 * Resolved presentation of a window. Pixels are kept by value, so the struct
 * stays trivially copyable (owned dialogs copy their owner's). About 1 KiB per
 * client.
 */
struct windowd_presentation
{
    /* Empty = nobody gave a name; the file name is used. */
    char label[WINDOWD_PRESENTATION_LABEL_CAPACITY];
    uint32_t accent;
    /* 1 = the window declared its accent; 0 = generic (WINDOWD_DEFAULT_ACCENT).
     * The active caption uses a ramp of its own only for a declared accent;
     * otherwise it keeps the Windows Standard gradient. */
    uint32_t accent_declared;
    /* SAVANXP_WM_WINDOW_STYLE_*. 0 = an ordinary window. */
    uint32_t window_flags;
    /* 0 = no icon of its own; fallback_icon_id is used. */
    uint32_t icon_extent;
    uint32_t fallback_icon_id; /* enum desktop_icon_id */
    uint32_t icon_pixels[WINDOWD_PRESENTATION_ICON_PIXELS];
};

/* What a window shows before it publishes anything: the last component of
 * `path` as its title, the generic icon and accent, an ordinary window. */
void windowd_presentation_default(struct windowd_presentation *presentation, const char *path);

/* Adopts what the client published. Every field is validated, since the
 * header is memory another process writes: the title is cut and terminated,
 * an icon is taken only at the exact slot size. Returns 1 if anything shown
 * changed. */
int windowd_presentation_adopt(
    struct windowd_presentation *presentation,
    const struct savanxp_wm_presentation *published);

/* Resolved title. `path` is the last resort when nobody gave a name. */
const char *windowd_presentation_label(const struct windowd_presentation *presentation, const char *path);

/* Resolved accent (declared or default). */
uint32_t windowd_presentation_accent(const struct windowd_presentation *presentation);

/* 1 if the window declared its accent, 0 if it is the generic one. */
int windowd_presentation_has_accent(const struct windowd_presentation *presentation);

/*
 * Small icon. `storage` comes from the caller to describe the window's own
 * pixels without the struct pointing at itself. Never returns 0: without an
 * icon of its own it falls back to the baked set.
 */
const struct desktop_embedded_bitmap *windowd_presentation_icon(
    const struct windowd_presentation *presentation,
    struct desktop_embedded_bitmap *storage);

/* Checks the defaults and the adoption rules. 0 if everything passes. */
int windowd_presentation_selftest(void);
