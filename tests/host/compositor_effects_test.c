/* Exercise the actual scene renderer with memory-backed surfaces. Only
 * catalog lookup and the raw rectangle primitive are supplied by the host. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../system/compositord/compositord_scene.c"
#include "../../system/compositord/effects_settings.h"

static struct cursor_image test_cursor;
static struct cursor_shadow test_shadow;
const struct cursor_image *cursor_catalog_get(unsigned shape) { (void)shape; return &test_cursor; }
const struct cursor_shadow *cursor_catalog_shadow(unsigned shape) { (void)shape; return &test_shadow; }
unsigned int gfx_stride_pixels(const struct savanxp_fb_info *info) { return info->pitch / 4; }
void gfx_rect(uint32_t *pixels, const struct savanxp_fb_info *info, int x, int y, int w, int h, uint32_t color) {
    for (int row = y; row < y + h; ++row) for (int col = x; col < x + w; ++col)
        pixels[row * (info->pitch / 4) + col] = color;
}

static uint32_t screen[128 * 96], background[128 * 96], window[128 * 96];
static struct savanxp_compositor_scene shared;
static uint32_t at(int x, int y) { return screen[y * 128 + x] & 0xffffff; }
static void compose(void) { struct sx_region presented; scene_compose(&presented); }
static void layer(int i, int surface, int x, int y, int w, int h, unsigned flags) {
    shared.layers[i] = (struct savanxp_compositor_layer){surface, flags, {0,0,w,h}, {x,y,w,h}};
}

int main(void) {
    struct savanxp_fb_info info = {0};
    assert(effects_settings_parse("") == SXN_EFFECTS_ALL);
    assert(effects_settings_parse("window_shadows=0\ncursor_shadow=0\n") == 0);
    assert(effects_settings_parse("cursor_shadow=0\nwindow_shadows=bad\nunknown=0\n") == SXN_EFFECTS_WINDOWS);
    assert(effects_settings_parse("window_shadows=0") == SXN_EFFECTS_CURSOR);
    assert(effects_settings_parse("window_shadows=00\ncursor_shadow=2") == SXN_EFFECTS_ALL);

    {
        struct sx_rect frame = sx_rect_make(20, 20, 40, 30);
        assert(window_shadow_alpha(frame, 60, 40) == 36);
        assert(window_shadow_alpha(frame, 61, 40) >= window_shadow_alpha(frame, 65, 40));
        assert(window_shadow_alpha(frame, 65, 40) > window_shadow_alpha(frame, 70, 40));
        assert(window_shadow_alpha(frame, 74, 40) == 0);
    }
    test_cursor.width = test_cursor.height = 16;
    test_cursor.hotspot_x = 2; test_cursor.hotspot_y = 3;
    for (int i = 0; i < 256; ++i) test_cursor.pixels[i] = 0xffffffff;
    cursor_shadow_build(&test_cursor, &test_shadow);
    assert(test_shadow.width == 24 && test_shadow.height == 24);
    assert(test_shadow.pixels[12 * 24 + 12] == 80u << 24);
    assert(!test_shadow.pixels[0]);
    test_cursor.classic = 1;
    for (int i = 0; i < 256; ++i) test_cursor.and_mask[i] = 0xffffff;
    cursor_shadow_build(&test_cursor, &test_shadow);
    for (int i = 0; i < 24 * 24; ++i) assert(!test_shadow.pixels[i]);
    test_cursor.xor_rgb[128] = 0xffffff;
    cursor_shadow_build(&test_cursor, &test_shadow);
    assert(test_shadow.pixels[13 * 24 + 4] >> 24);
    test_cursor.classic = 0;
    cursor_shadow_build(&test_cursor, &test_shadow);

    {
        static uint32_t hardware[64 * 64];
        assert(!cursor_shadow_pack(&test_cursor, &test_shadow, hardware));
        assert(hardware[3 * 64 + 3] == 0xffffffff); /* original (0,0) plus margin */
        assert(hardware[21 * 64 + 19] >> 24); /* shadow beyond the original */
        test_cursor.width = test_cursor.height = 64;
        cursor_shadow_build(&test_cursor, &test_shadow);
        assert(cursor_shadow_pack(&test_cursor, &test_shadow, hardware) < 0);
        assert(!cursor_shadow_pack(&test_cursor, 0, hardware));
        test_cursor.width = test_cursor.height = 16;
        cursor_shadow_build(&test_cursor, &test_shadow);
        test_cursor.needs_background = 1;
        assert(cursor_shadow_pack(&test_cursor, &test_shadow, hardware) < 0);
        test_cursor.needs_background = 0;
        test_cursor.pixels[0] = 0x80402010;
        cursor_shadow_build(&test_cursor, &test_shadow);
        assert(!cursor_shadow_pack(&test_cursor, &test_shadow, hardware));
        {
            unsigned sa = test_shadow.pixels[3 * test_shadow.width + 3] >> 24;
            uint32_t packed = hardware[3 * 64 + 3];
            unsigned a = packed >> 24;
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned original = (test_cursor.pixels[0] >> shift) & 255;
                unsigned software = (original * 128 + (255 - sa) * 127 + 127) / 255;
                unsigned gpu = (((packed >> shift) & 255) * a + 255 * (255 - a) + 127) / 255;
                assert(gpu + 1 >= software && software + 1 >= gpu);
            }
        }
        test_cursor.pixels[0] = 0xffffffff;
        cursor_shadow_build(&test_cursor, &test_shadow);
    }

    info.width = 128; info.height = 96; info.pitch = 128 * 4; info.bpp = 32; info.buffer_size = sizeof(screen);
    scene_set_display(screen, &info);
    for (int i = 0; i < 128 * 96; ++i) { background[i] = 0xffffff; window[i] = 0xabcdef; }
    for (int id = 2; id <= 3; ++id) {
        g_surfaces[id].in_use = 1;
        g_surfaces[id].pixels = id == 2 ? background : window;
        g_surfaces[id].pitch = info.pitch;
        g_surfaces[id].capacity_width = 128; g_surfaces[id].capacity_height = 96;
        g_surfaces[id].wake_event = -1;
    }
    shared.magic = SAVANXP_COMPOSITOR_SCENE_MAGIC;
    g_scene_shared = &shared;
    shared.layer_count = 2;
    layer(0, 2, 0, 0, 128, 96, SAVANXP_COMPOSITOR_LAYER_OPAQUE);
    layer(1, 3, 20, 20, 40, 30, SAVANXP_COMPOSITOR_LAYER_OPAQUE | SAVANXP_COMPOSITOR_LAYER_SHADOW);
    assert(!scene_commit()); compose();
    assert(at(30,30) == 0xabcdef);
    assert(at(62,40) < 0xffffff && at(62,40) > 0);
    assert(at(75,40) == 0xffffff);
    uint32_t shadow_pixel = at(62,40);
    for (int i = 0; i < 5; ++i) { damage_rect(display_rect()); compose(); assert(at(62,40) == shadow_pixel); }
    /* A client updates underneath the translucent shadow. */
    for (int i = 0; i < 128 * 96; ++i) background[i] = 0x808080;
    damage_surface_rect(2, sx_rect_make(60, 35, 8, 10), 0); compose();
    assert(at(62,40) < 0x808080);
    /* A front opaque window hides the shadow; removing it restores it. */
    shared.layer_count = 3;
    layer(2, 3, 61, 35, 10, 10, SAVANXP_COMPOSITOR_LAYER_OPAQUE);
    assert(!scene_commit()); compose(); assert(at(62,40) == 0xabcdef);
    shared.layer_count = 2;
    assert(!scene_commit()); compose(); assert(at(62,40) < 0x808080);
    /* Move without WM-supplied damage: both old and new visual bounds matter. */
    shared.layers[1].dst.x = 80;
    assert(!scene_commit());
    assert(sx_region_rect_count(&g_damage) > 0);
    assert(sx_region_rect_count(&g_damage) < 10);
    {
        struct sx_rect dirty;
        size_t area = 0;
        for (size_t i = 0; sx_region_rect_at(&g_damage, i, &dirty); ++i) area += dirty.width * dirty.height;
        assert(area < 128 * 96); /* dragging does not invalidate the full display */
    }
    compose(); assert(at(62,40) == 0x808080); assert(at(122,40) < 0x808080);
    shared.layers[1].dst.width = 20;
    assert(!scene_commit()); compose(); assert(at(122,40) == 0x808080); assert(at(102,40) < 0x808080);
    shared.layers[1].flags &= ~SAVANXP_COMPOSITOR_LAYER_SHADOW;
    assert(!scene_commit()); compose(); assert(at(102,40) == 0x808080);
    shared.layers[1].flags |= SAVANXP_COMPOSITOR_LAYER_SHADOW;
    shared.layers[1].dst.width = 40;
    assert(!scene_commit()); compose();
    scene_set_effects(0); compose(); assert(at(122,40) == 0x808080);
    scene_set_effects(SXN_EFFECTS_ALL); compose(); assert(at(122,40) < 0x808080);
    shared.layer_count = 1;
    assert(!scene_commit()); compose(); assert(at(122,40) == 0x808080);
    /* Software pointer bounds, clipping at screen edges and old-image cleanup. */
    scene_set_software_cursor(1);
    shared.cursor_visible = 1; shared.cursor_x = 12; shared.cursor_y = 12;
    assert(!scene_commit()); compose(); assert(at(12,12) == 0xffffff);
    assert(at(26,26) < 0x808080);
    shared.cursor_x = 127; shared.cursor_y = 95;
    assert(!scene_commit()); compose(); assert(at(12,12) == 0x808080); assert(at(26,26) == 0x808080);
    shared.cursor_visible = 0;
    assert(!scene_commit()); compose(); assert(at(127,95) == 0x808080);
    puts("compositor-effects-test: PASS");
    return 0;
}
