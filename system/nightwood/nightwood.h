#pragma once

/* Nightwood: a night in a forest, eight photographs, and something that
 * watches. The first game written against SUN (docs/userland/SUN.md) and the
 * consumer that drove batches 3 and 4 of SxGL (docs/desktop/SXGL_ROADMAP.md).
 * The design of the game itself is in docs/userland/NIGHTWOOD.md.
 *
 * Three units:
 *   nightwood.c        the SUN side: window, input, the loop, the HUD
 *   nightwood_world.c  the forest: generation, collision, textures, drawing
 *   nightwood_audio.c  the sound: every voice synthesized, on a thread of its own
 */

#include <stdint.h>

#include "sxgfx/sxgl.h"

#define NW_WORLD_HALF 60.0f
#define NW_PHOTO_COUNT 8
#define NW_MAX_TREES 320
#define NW_EYE_HEIGHT 1.65f
#define NW_PLAYER_RADIUS 0.35f
#define NW_PI 3.14159265358979323846f

struct nw_rng {
    uint64_t state;
};

uint32_t nw_random(struct nw_rng* rng);
/* [0, 1) */
float nw_random_unit(struct nw_rng* rng);

struct nw_tree {
    float x;
    float z;
    float radius;
    float height;
    float foliage_width;
    int dead; /* a bare trunk, no needles */
};

enum nw_landmark_kind {
    NW_LANDMARK_ROCK = 0,
    NW_LANDMARK_SHED,
    NW_LANDMARK_TANK,
    NW_LANDMARK_CAR,
    NW_LANDMARK_WALL,
    NW_LANDMARK_OAK,
    NW_LANDMARK_SIGN,
    NW_LANDMARK_ARCH,
    NW_LANDMARK_KIND_COUNT,
};

struct nw_landmark {
    int kind;
    float x;
    float z;
    float yaw; /* degrees */
    float radius; /* bounding: what keeps trees off it */
    /* What the player collides with: up to three circles (x, z, radius) in
     * world space, so that a long wall or an arch is not one big disc that
     * keeps the photograph out of reach. */
    float circles[3][3];
    int circle_count;
    /* The photograph pinned to it: centre, and the direction its face looks. */
    float photo_x;
    float photo_y;
    float photo_z;
    float photo_yaw;
    int photo_taken;
};

struct nw_world {
    struct nw_tree trees[NW_MAX_TREES];
    int tree_count;
    struct nw_landmark landmarks[NW_PHOTO_COUNT];
    GLuint texture_ground;
    GLuint texture_bark;
    GLuint texture_foliage;
    GLuint texture_stone;
    GLuint texture_wood;
    GLuint texture_metal;
    GLuint texture_photo;
};

struct nw_camera {
    float x;
    float z;
    float yaw; /* degrees, 0 looks down -z */
    float pitch; /* degrees, positive looks up */
    float bob; /* head bob offset, metres */
};

struct nw_warden {
    int present; /* standing somewhere in the forest */
    float x;
    float z;
    float yaw; /* degrees: it always faces the player */
};

float nw_ground_height(float x, float z);
void nw_world_generate(struct nw_world* world, uint64_t seed);
/* Needs the current SxGL context. 0, or -1 if a texture could not be made. */
int nw_world_upload_textures(struct nw_world* world);
void nw_world_render(const struct nw_world* world, const struct nw_camera* camera, const struct nw_warden* warden,
    int flashlight, float aspect);
/* Pushes a circle of `radius` at (*x, *z) out of every trunk and landmark and
 * back inside the fence. */
void nw_world_collide(const struct nw_world* world, float* x, float* z, float radius);
/* 1 if a circle at (x, z) overlaps a trunk, a landmark or the fence. */
int nw_world_blocked(const struct nw_world* world, float x, float z, float radius);
/* 1 if a trunk stands between the two points (at eye height). */
int nw_world_occluded(const struct nw_world* world, float ax, float az, float bx, float bz);

/* --- Sound (nightwood_audio.c) -------------------------------------------- */

/* What the game wants heard right now. The two events are counters: the sound
 * thread plays the chime or the end once each time one of them goes up. */
struct nw_audio_mix {
    float wind; /* 0..1 */
    float steps; /* footsteps per second, 0 standing still */
    float heartbeat; /* 0..1: how near the Warden is, and how watched */
    float static_level; /* 0..1: the screen's static, heard */
    int pickup; /* photographs taken this session */
    int caught; /* nights lost this session */
};

/* Starts the sound thread: the stream's mode (SXNAUDIO_MODE_DAEMON or
 * _DIRECT) on success, negative when there is no sound to be had -- the game
 * is then silent, never broken. */
long nw_audio_start(void);
void nw_audio_update(const struct nw_audio_mix* mix);
void nw_audio_stop(void);
