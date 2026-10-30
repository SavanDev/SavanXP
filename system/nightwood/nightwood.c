/* Nightwood, the SUN side: the window, the input, the loop and the HUD.
 *
 * Every frame SxGL draws the forest into a bitmap of half the window's size,
 * then this file scales it up by two into the window surface, darkening what
 * falls outside the flashlight's beam and laying the static on top, draws the
 * HUD with the SxGFX painter, and presents the whole window through sxnwm. The
 * half-size render is what keeps a software rasterizer at a playable rate, and
 * the big pixels suit the night. See docs/userland/NIGHTWOOD.md.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sxgfx/gfx2d.h"
#include "sxgfx/sxgl.h"
#include "wm/wm_protocol.h"
#include "sxn/sxn.h"
#include "sxn/sxnaudio.h"
#include "sxn/sxnwm.h"

#include "nightwood.h"
#include "nightwood.sunres.h"

#define NW_WINDOW_WIDTH 640u
#define NW_WINDOW_HEIGHT 480u
#define NW_KEY_COUNT 512
#define NW_WALK_SPEED 2.6f
#define NW_RUN_SPEED 5.0f
#define NW_TURN_SPEED 110.0f
#define NW_REACH 2.4f

enum nw_screen {
    NW_SCREEN_TITLE,
    NW_SCREEN_PLAY,
    NW_SCREEN_PAUSE,
    NW_SCREEN_CAUGHT,
    NW_SCREEN_ESCAPED,
};

struct nw_game {
    struct sxnwm_client wm;
    struct sxgl_context* gl;
    struct sx_bitmap render;
    int render_width;
    int render_height;
    uint16_t* beam; /* per render pixel: 0..256, the flashlight's mask */
    int beam_flashlight; /* which mask `beam` holds */

    struct nw_world world;
    struct nw_camera camera;
    struct nw_warden warden;
    struct nw_rng rng;
    uint32_t noise;

    int screen;
    int photos;
    int flashlight;
    float fear; /* 0..1: reaching 1 is being caught */
    float stamina; /* 0..1 */
    float warden_timer;
    float time;
    float screen_time; /* seconds on the current screen */
    float step_phase;
    float message_time;
    char message[80];
    int prompt_landmark; /* the photograph in reach and in view, or -1 */
    int seen; /* the Warden is in view this frame */
    float warden_distance;

    uint8_t keys[NW_KEY_COUNT];
    int quit;
    int captured; /* the pointer is ours: mouse look */
    uint32_t buttons;
    float steps; /* footsteps per second, for the sound */
    struct nw_audio_mix audio;
    int audio_running;
    /* --still: a fixed pose with the Warden in view, for screenshots. Nothing
     * moves and nothing can catch the player. */
    int still;
};

static struct nw_game g_game;

/* ------------------------------------------------------------------------
 * Small helpers
 * ------------------------------------------------------------------------ */

static float nw_clamp(float value, float low, float high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static void nw_copy(char* out, size_t capacity, const char* text)
{
    size_t index = 0;
    while (text[index] != '\0' && index + 1 < capacity) {
        out[index] = text[index];
        index += 1;
    }
    out[index] = '\0';
}

static void nw_message(const char* text)
{
    nw_copy(g_game.message, sizeof(g_game.message), text);
    g_game.message_time = 4.0f;
}

static void nw_log_count(const char* prefix, int value)
{
    char line[64];
    size_t used = 0;
    char digits[12];
    int count = 0;

    while (prefix[used] != '\0' && used + 1 < sizeof(line)) {
        line[used] = prefix[used];
        used += 1;
    }
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0 && count < 11);
    while (count > 0 && used + 1 < sizeof(line)) {
        line[used++] = digits[--count];
    }
    line[used] = '\0';
    (void)sxn_log(line);
}

static uint32_t nw_noise_next(void)
{
    uint32_t x = g_game.noise;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_game.noise = x;
    return x;
}

static void nw_debug_sink(const char* message, void* user)
{
    (void)user;
    (void)sxn_log(message);
}

/* ------------------------------------------------------------------------
 * The render target and the flashlight mask
 * ------------------------------------------------------------------------ */

static void nw_build_beam(void)
{
    int width = g_game.render_width;
    int height = g_game.render_height;
    float centre_x = (float)width * 0.5f;
    float centre_y = (float)height * 0.55f;
    float radius = (float)(width < height ? width : height) * 0.62f;
    int x;
    int y;

    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            float dx = ((float)x - centre_x) / radius;
            float dy = ((float)y - centre_y) / radius;
            float distance = sqrtf(dx * dx + dy * dy);
            float value;
            if (g_game.flashlight) {
                /* Full inside the beam, a soft edge, and the dark around. */
                float t = nw_clamp((distance - 0.55f) / 0.5f, 0.0f, 1.0f);
                t = t * t * (3.0f - 2.0f * t);
                value = 256.0f - t * 170.0f;
            } else {
                value = 120.0f - 40.0f * nw_clamp(distance, 0.0f, 1.0f);
            }
            g_game.beam[(size_t)y * (size_t)width + (size_t)x] = (uint16_t)value;
        }
    }
    g_game.beam_flashlight = g_game.flashlight;
}

static int nw_make_target(void)
{
    int width = (int)(g_game.wm.width + 1u) / 2;
    int height = (int)(g_game.wm.height + 1u) / 2;
    uint16_t* beam;

    if (width < 16) {
        width = 16;
    }
    if (height < 16) {
        height = 16;
    }
    beam = (uint16_t*)malloc((size_t)width * (size_t)height * sizeof(uint16_t));
    if (beam == 0) {
        return -1;
    }
    sx_bitmap_destroy(&g_game.render);
    if (!sx_bitmap_create(&g_game.render, width, height, SX_PIXEL_FORMAT_BGRX8888)) {
        free(beam);
        return -1;
    }
    free(g_game.beam);
    g_game.beam = beam;
    g_game.render_width = width;
    g_game.render_height = height;
    nw_build_beam();

    if (g_game.gl == 0) {
        g_game.gl = sxgl_context_create(&g_game.render);
        if (g_game.gl == 0) {
            return -1;
        }
        sxgl_make_current(g_game.gl);
        sxgl_set_debug_sink(g_game.gl, nw_debug_sink, 0);
        return 0;
    }
    return sxgl_context_resize(g_game.gl, &g_game.render);
}

/* ------------------------------------------------------------------------
 * A night
 * ------------------------------------------------------------------------ */

static void nw_new_night(void)
{
    uint64_t seed = sxn_clock_ns() | 1u;

    nw_world_generate(&g_game.world, seed);
    g_game.rng.state = seed * 0x9E3779B97F4A7C15ULL + 1u;
    memset(&g_game.camera, 0, sizeof(g_game.camera));
    memset(&g_game.warden, 0, sizeof(g_game.warden));
    g_game.photos = 0;
    g_game.fear = 0.0f;
    g_game.stamina = 1.0f;
    g_game.warden_timer = 0.0f;
    g_game.prompt_landmark = -1;
    g_game.message[0] = '\0';
    g_game.message_time = 0.0f;
    g_game.seen = 0;
}

/* Mouse look is on only while playing: the title, the pause and the endings
 * give the cursor back. The WM also suspends it by itself whenever the window
 * loses the focus. */
static void nw_set_screen(int screen)
{
    int capture = screen == NW_SCREEN_PLAY && !g_game.still;

    g_game.screen = screen;
    g_game.screen_time = 0.0f;
    if (capture != g_game.captured && g_game.wm.header != 0) {
        (void)sxnwm_set_pointer_capture(&g_game.wm, capture);
        g_game.captured = capture;
    }
}

/* Puts the Warden somewhere the player is not looking, nearer with every
 * photograph taken. */
static void nw_relocate_warden(void)
{
    float progress = (float)g_game.photos / (float)NW_PHOTO_COUNT;
    int attempt;

    for (attempt = 0; attempt < 24; ++attempt) {
        float behind = 90.0f + 180.0f * nw_random_unit(&g_game.rng);
        float angle = (g_game.camera.yaw + behind) * (NW_PI / 180.0f);
        float distance = (22.0f - 15.0f * progress) * (0.8f + 0.4f * nw_random_unit(&g_game.rng));
        float x = g_game.camera.x - sinf(angle) * distance;
        float z = g_game.camera.z - cosf(angle) * distance;
        if (nw_world_blocked(&g_game.world, x, z, 0.5f)) {
            continue;
        }
        g_game.warden.x = x;
        g_game.warden.z = z;
        g_game.warden.present = 1;
        break;
    }
    g_game.warden_timer = (13.0f - 8.0f * progress) + 3.0f * nw_random_unit(&g_game.rng);
}

static int nw_key(int code)
{
    return code >= 0 && code < NW_KEY_COUNT && g_game.keys[code] != 0;
}

static void nw_take_photo(void)
{
    struct nw_landmark* landmark;
    char text[48];

    if (g_game.prompt_landmark < 0) {
        return;
    }
    landmark = &g_game.world.landmarks[g_game.prompt_landmark];
    landmark->photo_taken = 1;
    g_game.photos += 1;
    g_game.audio.pickup += 1;
    nw_log_count("nightwood: photograph ", g_game.photos);
    if (g_game.photos >= NW_PHOTO_COUNT) {
        (void)sxn_log("nightwood: escaped");
        nw_set_screen(NW_SCREEN_ESCAPED);
        return;
    }
    nw_copy(text, sizeof(text), "Photographs: 0/8");
    text[13] = (char)('0' + g_game.photos);
    nw_message(text);
    /* Every photograph wakes it, and it moves at once. */
    g_game.warden_timer = 0.0f;
}

static void nw_update_play(float dt)
{
    struct nw_camera* camera = &g_game.camera;
    float forward = 0.0f;
    float strafe = 0.0f;
    float turn = 0.0f;
    float look = 0.0f;
    float speed;
    float yaw;
    float step_x;
    float step_z;
    float old_x = camera->x;
    float old_z = camera->z;
    float moved;
    int running;
    int index;

    if (nw_key('w') || nw_key(SAVANXP_KEY_UP)) {
        forward += 1.0f;
    }
    if (nw_key('s') || nw_key(SAVANXP_KEY_DOWN)) {
        forward -= 1.0f;
    }
    if (nw_key('d')) {
        strafe += 1.0f;
    }
    if (nw_key('a')) {
        strafe -= 1.0f;
    }
    if (nw_key(SAVANXP_KEY_LEFT)) {
        turn += 1.0f;
    }
    if (nw_key(SAVANXP_KEY_RIGHT)) {
        turn -= 1.0f;
    }
    if (nw_key(SAVANXP_KEY_PAGE_UP)) {
        look += 1.0f;
    }
    if (nw_key(SAVANXP_KEY_PAGE_DOWN)) {
        look -= 1.0f;
    }

    camera->yaw += turn * NW_TURN_SPEED * dt;
    camera->pitch = nw_clamp(camera->pitch + look * 70.0f * dt, -60.0f, 60.0f);

    running = nw_key(SAVANXP_KEY_SHIFT) && g_game.stamina > 0.05f && forward > 0.0f;
    speed = running ? NW_RUN_SPEED : NW_WALK_SPEED;
    if (running) {
        g_game.stamina = nw_clamp(g_game.stamina - dt * 0.22f, 0.0f, 1.0f);
    } else {
        g_game.stamina = nw_clamp(g_game.stamina + dt * 0.12f, 0.0f, 1.0f);
    }
    yaw = camera->yaw * (NW_PI / 180.0f);
    step_x = (-sinf(yaw) * forward + cosf(yaw) * strafe);
    step_z = (-cosf(yaw) * forward - sinf(yaw) * strafe);
    {
        float length = sqrtf(step_x * step_x + step_z * step_z);
        if (length > 1.0f) {
            step_x /= length;
            step_z /= length;
        }
    }
    camera->x += step_x * speed * dt;
    camera->z += step_z * speed * dt;
    nw_world_collide(&g_game.world, &camera->x, &camera->z, NW_PLAYER_RADIUS);
    moved = sqrtf((camera->x - old_x) * (camera->x - old_x) + (camera->z - old_z) * (camera->z - old_z));
    g_game.step_phase += moved * 2.2f;
    g_game.steps = dt > 0.0f ? moved / dt * 0.85f : 0.0f;
    camera->bob = 0.05f * sinf(g_game.step_phase * 2.0f) * nw_clamp(moved / (dt * NW_WALK_SPEED + 1e-4f), 0.0f, 1.5f);

    /* The photograph in reach: near, and roughly in the middle of the view. */
    g_game.prompt_landmark = -1;
    for (index = 0; index < NW_PHOTO_COUNT; ++index) {
        const struct nw_landmark* landmark = &g_game.world.landmarks[index];
        float dx = landmark->photo_x - camera->x;
        float dz = landmark->photo_z - camera->z;
        float distance = sqrtf(dx * dx + dz * dz);
        float facing;
        if (landmark->photo_taken || distance > NW_REACH || distance < 1e-3f) {
            continue;
        }
        facing = (dx * -sinf(yaw) + dz * -cosf(yaw)) / distance;
        if (facing > 0.75f) {
            g_game.prompt_landmark = index;
        }
    }

    /* The Warden. */
    g_game.seen = 0;
    if (g_game.photos > 0) {
        struct nw_warden* warden = &g_game.warden;
        float dx;
        float dz;
        float distance;

        g_game.warden_timer -= dt;
        if (warden->present) {
            dx = warden->x - camera->x;
            dz = warden->z - camera->z;
            distance = sqrtf(dx * dx + dz * dz);
            g_game.warden_distance = distance;
            if (distance > 0.01f && distance < 32.0f) {
                float facing = (dx * -sinf(yaw) + dz * -cosf(yaw)) / distance;
                g_game.seen = facing > 0.78f &&
                    !nw_world_occluded(&g_game.world, camera->x, camera->z, warden->x, warden->z);
            }
            /* It creeps closer while it is not looked at, from the fourth
             * photograph on. */
            if (!g_game.seen && g_game.photos >= 4 && distance > 3.0f) {
                float pace = (0.4f + 0.12f * (float)g_game.photos) * dt;
                float x = warden->x - dx / distance * pace;
                float z = warden->z - dz / distance * pace;
                if (!nw_world_blocked(&g_game.world, x, z, 0.4f)) {
                    warden->x = x;
                    warden->z = z;
                }
            }
            warden->yaw = atan2f(-dx, -dz) * (180.0f / NW_PI);
            if (distance < 1.4f) {
                g_game.fear = 1.0f;
            }
        } else {
            g_game.warden_distance = 99.0f;
        }
        if (g_game.seen) {
            g_game.fear += dt * (0.10f + 1.1f / nw_clamp(g_game.warden_distance, 1.5f, 40.0f)) *
                (1.0f + 0.1f * (float)g_game.photos);
        } else {
            g_game.fear -= dt * 0.08f;
        }
        g_game.fear = nw_clamp(g_game.fear, 0.0f, 1.0f);
        if (g_game.warden_timer <= 0.0f && !g_game.seen) {
            nw_relocate_warden();
        }
        if (g_game.fear >= 1.0f) {
            (void)sxn_log("nightwood: caught");
            g_game.audio.caught += 1;
            nw_set_screen(NW_SCREEN_CAUGHT);
        }
    }
}

static void nw_update(float dt)
{
    g_game.time += dt;
    g_game.screen_time += dt;
    if (g_game.message_time > 0.0f) {
        g_game.message_time -= dt;
    }
    switch (g_game.screen) {
    case NW_SCREEN_TITLE:
        /* A slow look around the clearing behind the title. */
        g_game.camera.yaw = g_game.time * 6.0f;
        g_game.camera.pitch = 4.0f;
        break;
    case NW_SCREEN_PLAY:
        nw_update_play(dt);
        break;
    case NW_SCREEN_CAUGHT:
        g_game.fear = nw_clamp(1.0f - (g_game.screen_time - 1.5f) * 0.6f, 0.25f, 1.0f);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * Input
 * ------------------------------------------------------------------------ */

static int nw_normalize_key(uint32_t key)
{
    if (key >= 'A' && key <= 'Z') {
        return (int)(key - 'A' + 'a');
    }
    return (int)key;
}

static void nw_key_down(int key, int repeat)
{
    if (repeat) {
        return;
    }
    switch (g_game.screen) {
    case NW_SCREEN_TITLE:
        if (key == SAVANXP_KEY_ENTER || key == ' ') {
            nw_new_night();
            nw_set_screen(NW_SCREEN_PLAY);
            nw_message("Find the eight photographs.");
        } else if (key == SAVANXP_KEY_ESC) {
            g_game.quit = 1;
        }
        break;
    case NW_SCREEN_PLAY:
        if (key == SAVANXP_KEY_ESC) {
            nw_set_screen(NW_SCREEN_PAUSE);
        } else if (key == 'e' || key == SAVANXP_KEY_ENTER || key == ' ') {
            nw_take_photo();
        } else if (key == 'f') {
            g_game.flashlight = !g_game.flashlight;
        }
        break;
    case NW_SCREEN_PAUSE:
        if (key == SAVANXP_KEY_ENTER || key == ' ') {
            nw_set_screen(NW_SCREEN_PLAY);
        } else if (key == SAVANXP_KEY_ESC) {
            nw_set_screen(NW_SCREEN_TITLE);
        }
        break;
    case NW_SCREEN_CAUGHT:
    case NW_SCREEN_ESCAPED:
        if (g_game.screen_time < 1.5f) {
            break;
        }
        if (key == SAVANXP_KEY_ENTER || key == ' ') {
            nw_new_night();
            nw_set_screen(NW_SCREEN_PLAY);
            nw_message("Find the eight photographs.");
        } else if (key == SAVANXP_KEY_ESC) {
            nw_set_screen(NW_SCREEN_TITLE);
        }
        break;
    default:
        break;
    }
}

/* Captured motion turns the head: a tenth of a degree per count, the usual
 * feel of a 400 dpi mouse. The left button takes the photograph. */
static void nw_pointer_motion(const struct savanxp_gui_pointer_motion* motion)
{
    int pressed = (motion->buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0u &&
        (g_game.buttons & SAVANXP_MOUSE_BUTTON_LEFT) == 0u;

    g_game.buttons = motion->buttons;
    if (g_game.screen != NW_SCREEN_PLAY) {
        return;
    }
    g_game.camera.yaw -= (float)motion->dx * 0.12f;
    g_game.camera.pitch = nw_clamp(g_game.camera.pitch - (float)motion->dy * 0.12f, -60.0f, 60.0f);
    if (pressed) {
        nw_take_photo();
    }
}

static void nw_drain_input(void)
{
    struct savanxp_wm_event record;

    while (sxnwm_poll_input(&g_game.wm, &record) == 0) {
        const struct savanxp_input_event* key;
        int code;
        if (record.window_id != 0) {
            continue;
        }
        if (record.kind == SAVANXP_WM_EVENT_POINTER_MOTION) {
            nw_pointer_motion(&record.payload.motion);
            continue;
        }
        if (record.kind != SAVANXP_WM_EVENT_KEY) {
            continue;
        }
        key = &record.payload.key;
        code = nw_normalize_key(key->key);
        if (key->type == SAVANXP_INPUT_EVENT_KEY_DOWN) {
            if (code >= 0 && code < NW_KEY_COUNT) {
                g_game.keys[code] = 1;
            }
            nw_key_down(code, (key->modifiers & SAVANXP_KEY_MOD_REPEAT) != 0);
        } else if (key->type == SAVANXP_INPUT_EVENT_KEY_UP) {
            if (code >= 0 && code < NW_KEY_COUNT) {
                g_game.keys[code] = 0;
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * Composition: the scale-up, the beam, the static
 * ------------------------------------------------------------------------ */

static void nw_compose(float static_level, float dim)
{
    const uint32_t* source = g_game.render.pixels;
    uint32_t source_stride = g_game.render.info.pitch / 4u;
    uint32_t* target = g_game.wm.pixels;
    uint32_t stride = g_game.wm.stride;
    int width = (int)g_game.wm.width;
    int height = (int)g_game.wm.height;
    int amount = (int)(nw_clamp(static_level, 0.0f, 1.0f) * 256.0f);
    int shade = (int)(nw_clamp(dim, 0.0f, 1.0f) * 256.0f);
    int ry;

    if (g_game.beam_flashlight != g_game.flashlight) {
        nw_build_beam();
    }
    for (ry = 0; ry < g_game.render_height; ++ry) {
        const uint32_t* row = source + (size_t)ry * source_stride;
        const uint16_t* beam = g_game.beam + (size_t)ry * (size_t)g_game.render_width;
        int y = ry * 2;
        int rx;
        if (y >= height) {
            break;
        }
        for (rx = 0; rx < g_game.render_width; ++rx) {
            uint32_t pixel = row[rx];
            int light = ((int)beam[rx] * shade) >> 8;
            int r = (int)((pixel >> 16) & 0xffu);
            int g = (int)((pixel >> 8) & 0xffu);
            int b = (int)(pixel & 0xffu);
            uint32_t random = nw_noise_next();
            int grain = (int)(random & 7u) - 3;
            int x = rx * 2;
            uint32_t out;
            r = ((r * light) >> 8) + grain;
            g = ((g * light) >> 8) + grain;
            b = ((b * light) >> 8) + grain;
            if (amount > 0) {
                int snow = (int)((random >> 8) & 0xffu);
                r += ((snow - r) * amount) >> 8;
                g += ((snow - g) * amount) >> 8;
                b += ((snow - b) * amount) >> 8;
            }
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            out = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            if (x >= width) {
                break;
            }
            target[(size_t)y * stride + (size_t)x] = out;
            if (x + 1 < width) {
                target[(size_t)y * stride + (size_t)x + 1] = out;
            }
            if (y + 1 < height) {
                target[(size_t)(y + 1) * stride + (size_t)x] = out;
                if (x + 1 < width) {
                    target[(size_t)(y + 1) * stride + (size_t)x + 1] = out;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * The HUD
 * ------------------------------------------------------------------------ */

static void nw_text(struct sx_painter* painter, int x, int y, const char* text, uint32_t colour)
{
    sx_painter_draw_text(painter, x + 1, y + 1, text, 0x000000u);
    sx_painter_draw_text(painter, x, y, text, colour);
}

static void nw_text_centred(struct sx_painter* painter, int y, const char* text, uint32_t colour)
{
    int x = ((int)g_game.wm.width - sx_painter_text_width(painter, text)) / 2;
    nw_text(painter, x, y, text, colour);
}

static void nw_draw_hud(void)
{
    struct savanxp_fb_info info;
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    const uint32_t pale = 0xd8d4c8u;
    const uint32_t dim = 0x8a8678u;
    const uint32_t red = 0xc83c30u;
    int height = (int)g_game.wm.height;
    int line;

    memset(&info, 0, sizeof(info));
    info.width = g_game.wm.width;
    info.height = g_game.wm.height;
    info.pitch = g_game.wm.stride * 4u;
    info.bpp = 32u;
    info.buffer_size = info.pitch * info.height;
    sx_bitmap_wrap(&bitmap, g_game.wm.pixels, &info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    switch (g_game.screen) {
    case NW_SCREEN_TITLE:
        (void)sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
        line = sx_painter_text_height(&painter);
        nw_text_centred(&painter, height / 3, "N I G H T W O O D", pale);
        (void)sx_painter_set_font(&painter, SX_FONT_UI);
        nw_text_centred(&painter, height / 3 + line + 10, "Eight photographs are pinned somewhere in these woods.", dim);
        nw_text_centred(&painter, height / 3 + line + 30, "Something else is out here. Do not look at it for long.", dim);
        nw_text_centred(&painter, height - 120, "W S A D walk   the mouse or the arrows look   Shift runs", dim);
        nw_text_centred(&painter, height - 100, "E or a click takes a photograph   F the flashlight   Esc pauses", dim);
        if (((int)(g_game.time * 2.0f) & 1) == 0) {
            nw_text_centred(&painter, height - 64, "Press Enter", pale);
        }
        break;
    case NW_SCREEN_PLAY:
        if (g_game.prompt_landmark >= 0) {
            nw_text_centred(&painter, height / 2 + 40, "E  take the photograph", pale);
        }
        if (g_game.message_time > 0.0f) {
            nw_text_centred(&painter, 24, g_game.message, pale);
        }
        if (g_game.stamina < 0.98f) {
            int width = (int)(g_game.stamina * 80.0f);
            sx_painter_fill_rect(&painter, sx_rect_make(16, height - 20, 82, 6), 0x101010u);
            sx_painter_fill_rect(&painter, sx_rect_make(17, height - 19, width, 4), 0x6c6a60u);
        }
        break;
    case NW_SCREEN_PAUSE:
        (void)sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
        nw_text_centred(&painter, height / 2 - 30, "Paused", pale);
        (void)sx_painter_set_font(&painter, SX_FONT_UI);
        nw_text_centred(&painter, height / 2 + 4, "Enter goes back to the woods   Esc gives up the night", dim);
        break;
    case NW_SCREEN_CAUGHT:
        if (g_game.screen_time > 1.5f) {
            char text[48];
            nw_copy(text, sizeof(text), "Photographs: 0/8");
            text[13] = (char)('0' + g_game.photos);
            (void)sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
            nw_text_centred(&painter, height / 2 - 40, "It found you.", red);
            (void)sx_painter_set_font(&painter, SX_FONT_UI);
            nw_text_centred(&painter, height / 2 - 6, text, dim);
            nw_text_centred(&painter, height / 2 + 24, "Enter tries another night   Esc goes back", dim);
        }
        break;
    case NW_SCREEN_ESCAPED:
        (void)sx_painter_set_font(&painter, SX_FONT_UI_TITLE);
        nw_text_centred(&painter, height / 2 - 40, "Eight photographs.", pale);
        (void)sx_painter_set_font(&painter, SX_FONT_UI);
        nw_text_centred(&painter, height / 2 - 6, "The fog thins. You find the road, and you do not look back.", dim);
        if (g_game.screen_time > 1.5f) {
            nw_text_centred(&painter, height / 2 + 24, "Enter walks in again   Esc goes back", dim);
        }
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------------
 * A frame
 * ------------------------------------------------------------------------ */

static void nw_frame(void)
{
    float aspect = (float)g_game.render_width / (float)g_game.render_height;
    float static_level = 0.0f;
    float dim = 1.0f;
    const struct nw_warden* warden = g_game.screen == NW_SCREEN_TITLE ? 0 : &g_game.warden;
    struct sx_rect damage;

    switch (g_game.screen) {
    case NW_SCREEN_PLAY:
        static_level = g_game.fear * g_game.fear * 0.85f;
        /* A flicker of static when it is near, even unseen. */
        if (g_game.warden.present && g_game.warden_distance < 8.0f) {
            static_level += (8.0f - g_game.warden_distance) * 0.012f;
        }
        break;
    case NW_SCREEN_PAUSE:
        dim = 0.35f;
        break;
    case NW_SCREEN_TITLE:
        dim = 0.7f;
        break;
    case NW_SCREEN_CAUGHT:
        static_level = g_game.fear;
        dim = 0.5f;
        break;
    case NW_SCREEN_ESCAPED:
        dim = nw_clamp(1.0f - g_game.screen_time * 0.4f, 0.25f, 1.0f);
        break;
    default:
        break;
    }

    nw_world_render(&g_game.world, &g_game.camera, warden, g_game.flashlight, aspect);
    sxgl_swap_buffers(g_game.gl, &damage);

    if (g_game.audio_running) {
        float near = 0.0f;
        if (g_game.screen == NW_SCREEN_PLAY && g_game.warden.present) {
            near = nw_clamp((16.0f - g_game.warden_distance) / 14.0f, 0.0f, 1.0f);
        }
        g_game.audio.wind = g_game.screen == NW_SCREEN_PAUSE ? 0.25f : 0.7f;
        g_game.audio.steps = g_game.screen == NW_SCREEN_PLAY && !g_game.still ? g_game.steps : 0.0f;
        g_game.audio.heartbeat = g_game.screen == NW_SCREEN_PLAY ? (near > g_game.fear ? near : g_game.fear) : 0.0f;
        g_game.audio.static_level = static_level > 1.0f ? 1.0f : static_level;
        nw_audio_update(&g_game.audio);
    }

    /* The surface may only be written once the compositor has read the last
     * frame. */
    if (sxnwm_wait_idle(&g_game.wm, 250) != 0) {
        return;
    }
    nw_compose(static_level, dim);
    nw_draw_hud();
    (void)sxnwm_submit_full(&g_game.wm);
}


/* ------------------------------------------------------------------------
 * Self-test (--selftest): no window, deterministic, for the smoke harness
 * ------------------------------------------------------------------------ */

static int g_selftest_failures;

/* The kernel log reaches the serial line, which is what the harness reads. */
static void nw_report(const char* line)
{
    (void)sxn_log(line);
}

static void nw_check(int ok, const char* what)
{
    char line[96];
    nw_copy(line, sizeof(line), ok ? "nightwood: ok   " : "nightwood: FAIL ");
    nw_copy(line + 16, sizeof(line) - 16, what);
    nw_report(line);
    if (!ok) {
        g_selftest_failures += 1;
    }
}

/* Places the camera 1.4 m in front of a landmark's photograph, facing it. */
static void nw_face_photo(const struct nw_landmark* landmark)
{
    float yaw = landmark->photo_yaw * (NW_PI / 180.0f);
    g_game.camera.x = landmark->photo_x + sinf(yaw) * 1.4f;
    g_game.camera.z = landmark->photo_z + cosf(yaw) * 1.4f;
    g_game.camera.yaw = landmark->photo_yaw;
    g_game.camera.pitch = 0.0f;
}

static int nw_selftest(void)
{
    struct sxgl_stats stats;
    const uint32_t* pixels;
    int glow = 0;
    int lit = 0;
    int index;
    int reachable = 0;
    uint64_t start;
    uint64_t elapsed;

    g_game.wm.width = NW_WINDOW_WIDTH;
    g_game.wm.height = NW_WINDOW_HEIGHT;
    g_game.flashlight = 1;
    nw_check(nw_make_target() == 0, "a 320x240 render target and an SxGL context");
    if (g_game.gl == 0) {
        nw_report("NIGHTWOOD SELFTEST FAIL");
        return 1;
    }
    nw_world_generate(&g_game.world, 0x5eed);
    g_game.rng.state = 77;
    nw_check(nw_world_upload_textures(&g_game.world) == 0, "seven procedural textures upload");
    {
        /* No session in the smoke, so no daemon: the stream must reach the
         * device directly. A machine with no audio device skips this. */
        long mode = nw_audio_start();
        if (mode != -SAVANXP_ENOENT) {
            nw_check(mode == SXNAUDIO_MODE_DIRECT, "the sound thread starts on the device");
            nw_audio_stop();
        }
    }
    nw_check(g_game.world.tree_count > 150, "the forest has its trees");
    nw_check(!nw_world_blocked(&g_game.world, 0.0f, 0.0f, NW_PLAYER_RADIUS), "the spawn clearing is free");
    nw_check(nw_world_blocked(&g_game.world, g_game.world.trees[0].x, g_game.world.trees[0].z, NW_PLAYER_RADIUS),
        "a trunk blocks the way");

    /* Every photograph can be reached and taken: walk up to each one. */
    nw_set_screen(NW_SCREEN_PLAY);
    for (index = 0; index < NW_PHOTO_COUNT; ++index) {
        nw_face_photo(&g_game.world.landmarks[index]);
        nw_update_play(0.016f);
        if (g_game.prompt_landmark == index) {
            reachable += 1;
        }
    }
    nw_check(reachable == NW_PHOTO_COUNT, "all eight photographs are within reach of a player facing them");

    nw_face_photo(&g_game.world.landmarks[0]);
    nw_update_play(0.016f);
    nw_take_photo();
    nw_check(g_game.photos == 1 && g_game.world.landmarks[0].photo_taken, "E takes the photograph");
    g_game.camera.yaw += 180.0f;
    nw_update_play(0.016f);
    nw_check(g_game.warden.present, "the first photograph wakes the Warden, out of view");
    {
        struct savanxp_gui_pointer_motion motion;
        float yaw = g_game.camera.yaw;
        float pitch = g_game.camera.pitch;
        memset(&motion, 0, sizeof(motion));
        motion.dx = 100;
        motion.dy = -50;
        nw_pointer_motion(&motion);
        nw_check(fabsf(g_game.camera.yaw - (yaw - 12.0f)) < 0.01f && g_game.camera.pitch > pitch,
            "captured motion turns the head: right is right, up is up");
    }

    /* A frame with the Warden five metres ahead. */
    g_game.camera.x = 0.0f;
    g_game.camera.z = 0.0f;
    g_game.camera.yaw = 0.0f;
    g_game.camera.pitch = 0.0f;
    g_game.warden.present = 1;
    g_game.warden.x = 0.0f;
    g_game.warden.z = -5.0f;
    g_game.warden.yaw = 0.0f;
    sxgl_reset_stats(g_game.gl);
    start = sxn_clock_ns();
    for (index = 0; index < 10; ++index) {
        nw_world_render(&g_game.world, &g_game.camera, &g_game.warden, 1,
            (float)g_game.render_width / (float)g_game.render_height);
    }
    elapsed = sxn_clock_ns() - start;
    sxgl_get_stats(g_game.gl, &stats);
    nw_check(glGetError() == GL_NO_ERROR, "the frame raises no GL error");
    nw_check(stats.triangles_rasterized > 10u * 400u, "the frame draws hundreds of triangles");
    pixels = g_game.render.pixels;
    for (index = 0; index < g_game.render_width * g_game.render_height; ++index) {
        uint32_t pixel = pixels[index];
        int r = (int)((pixel >> 16) & 0xffu);
        int g = (int)((pixel >> 8) & 0xffu);
        int b = (int)(pixel & 0xffu);
        if (r > 140 && g < r / 2 && b < r / 2) {
            glow += 1;
        }
        if (r + g + b > 60) {
            lit += 1;
        }
    }
    nw_check(glow > 0, "the Warden's eyes glow");
    nw_check(lit > g_game.render_width * g_game.render_height / 20, "the flashlight lights the ground ahead");
    nw_log_count("nightwood: microseconds per frame ", (int)(elapsed / 10u / 1000u));
    nw_log_count("nightwood: triangles per frame ", (int)(stats.triangles_rasterized / 10u));

    nw_report(g_selftest_failures == 0 ? "NIGHTWOOD SELFTEST PASS" : "NIGHTWOOD SELFTEST FAIL");
    return g_selftest_failures == 0 ? 0 : 1;
}

static void nw_enter_still(void)
{
    nw_world_generate(&g_game.world, 0x5eed);
    g_game.still = 1;
    nw_set_screen(NW_SCREEN_PLAY);
    g_game.camera.x = 0.0f;
    g_game.camera.z = 0.0f;
    g_game.camera.yaw = 0.0f;
    g_game.warden.present = 1;
    g_game.warden.x = 0.6f;
    g_game.warden.z = -6.0f;
    g_game.warden.yaw = 0.0f;
    g_game.warden_distance = 6.0f;
    g_game.photos = 3;
    g_game.fear = 0.3f;
}

int main(int argc, char** argv)
{
    uint64_t last;
    int ready_logged = 0;
    int still = argc > 1 && strcmp(argv[1], "--still") == 0;

    memset(&g_game, 0, sizeof(g_game));
    g_game.flashlight = 1;
    g_game.noise = 0x1234567u;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        sxn_exit(nw_selftest());
    }

    if (sxnwm_connect(&g_game.wm) != 0) {
        (void)sxn_log("nightwood: no window");
        sxn_exit(1);
    }
    (void)sxnwm_set_presentation(&g_game.wm, &sun_resources);
    (void)sxnwm_request_size(&g_game.wm, NW_WINDOW_WIDTH, NW_WINDOW_HEIGHT);

    if (nw_make_target() != 0) {
        (void)sxn_log("nightwood: no memory for the render target");
        sxn_exit(1);
    }
    nw_new_night();
    if (nw_world_upload_textures(&g_game.world) != 0) {
        (void)sxn_log("nightwood: textures failed");
        sxn_exit(1);
    }
    nw_set_screen(NW_SCREEN_TITLE);
    if (still) {
        nw_enter_still();
    }
    {
        long mode = nw_audio_start();
        g_game.audio_running = mode >= 0;
        (void)sxn_log(mode == SXNAUDIO_MODE_DAEMON ? "nightwood: sound through audiod"
                : mode == SXNAUDIO_MODE_DIRECT     ? "nightwood: sound straight to the device"
                                                   : "nightwood: no sound");
    }

    last = sxn_clock_ns();
    while (!g_game.quit) {
        uint64_t now;
        float dt;

        if (sxnwm_close_requested(&g_game.wm)) {
            break;
        }
        if (sxnwm_check_resize(&g_game.wm) > 0) {
            if (nw_make_target() != 0) {
                (void)sxn_log("nightwood: resize failed");
                break;
            }
        }
        nw_drain_input();

        now = sxn_clock_ns();
        dt = (float)(now - last) / 1e9f;
        last = now;
        if (dt > 0.1f) {
            dt = 0.1f;
        }
        if (g_game.still) {
            g_game.time += dt;
        } else if (g_game.screen != NW_SCREEN_PAUSE) {
            nw_update(dt);
        }
        nw_frame();
        if (!ready_logged) {
            (void)sxn_log("nightwood: ready");
            ready_logged = 1;
        }
    }

    nw_audio_stop();
    sxgl_context_destroy(g_game.gl);
    sxn_exit(0);
}
