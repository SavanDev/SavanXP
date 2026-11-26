/* The interface faces from /system/fonts (sxfont/ui.h). */

#include "sxfont/ui.h"

#include <stdlib.h>

#include "savanxp/font.h"
#include "sxgfx/pixels.h"

/* Weak: a static program links this unit without libsxfont, and then these
 * are null and the baked tables stay. */
#pragma weak sx_font_engine_create
#pragma weak sx_font_face_open
#pragma weak sx_font_strike_open
#pragma weak sx_font_gfx_face

/* Each slot's file and size. These are the faces the tables were baked from,
 * at the sizes they were baked at, so the files draw the same pixels
 * (ui-face-files-test). The monospace slot keeps UniFont's table. */
static const struct {
    int slot;
    const char *path;
    unsigned ppem;
} kInterfaceFaces[] = {
    {GFX_FACE_UI, "/system/fonts/MSSansSerif.ttf", 13},
    {GFX_FACE_TITLE, "/system/fonts/RetroSans.ttf", 17},
};

static int g_loaded = -1;

int sxfont_ui_load(sxfont_ui_read_file read_file) {
    if (g_loaded >= 0) return g_loaded;
    g_loaded = 0;
    if (read_file == NULL || sx_font_engine_create == NULL || sx_font_face_open == NULL ||
        sx_font_strike_open == NULL || sx_font_gfx_face == NULL)
        return 0;
    /* What the faces point at lives as long as the process: the slots keep
     * using it, and nothing ever unloads them. */
    struct sx_font_engine *engine = NULL;
    if (sx_font_engine_create(&engine) != SX_FONT_OK) return 0;
    for (size_t i = 0; i < sizeof kInterfaceFaces / sizeof kInterfaceFaces[0]; i++) {
        size_t size = 0;
        void *data = read_file(kInterfaceFaces[i].path, &size);
        if (data == NULL) continue;
        struct sx_font_face *face = NULL;
        struct sx_font_strike *strike = NULL;
        struct sx_font_strike_options options = {kInterfaceFaces[i].ppem, SX_FONT_FORCE_MONO};
        struct gfx_face gfx;
        if (sx_font_face_open(engine, data, size, 0, &face) != SX_FONT_OK) {
            free(data);
            continue;
        }
        if (sx_font_strike_open(face, &options, &strike) != SX_FONT_OK || sx_font_gfx_face(strike, &gfx) != SX_FONT_OK ||
            gfx_face_install(kInterfaceFaces[i].slot, &gfx) != 0)
            continue;
        g_loaded++;
    }
    return g_loaded;
}
