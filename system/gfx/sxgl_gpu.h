#pragma once

/* SxGL's GPU backend (docs/roadmaps/open/VIRGL_ROADMAP.md). Internal to SxGL.
 *
 * SxGL still transforms, lights and fogs every vertex on the CPU; this backend
 * receives clip-space vertices and lets the host GPU clip, cull, rasterize,
 * test depth and alpha, blend and apply fog. What it draws lives in a host
 * render target until sxgl_gpu_read copies it into the caller's colour buffer.
 *
 * The backend talks to /dev/gpu0 through sxgl_gpu_device.c. The host test
 * links a stand-in for that file that has no device, so the encoder still
 * compiles there and sxgl_gpu_create always answers "no GPU".
 */

#include <stdint.h>

#include "sxgfx/gfx2d.h"
#include "sxgfx/sxgl.h"

struct sxgl_gpu;
/* A texture's copy on the host, owned by the backend; SxGL keeps a pointer to
 * it in each texture object and the backend clears that pointer when it lets
 * the copy go. */
struct sxgl_gpu_texture;

/* A vertex as SxGL hands it over: clip-space position, colour 0..1, texture
 * coordinates and the fog factor (1 = no fog). */
struct sxgl_gpu_vertex {
    float clip[4];
    float colour[4];
    float tex[2];
    float fog;
    float unused;
};

/* Everything a draw depends on, in GL's terms. The backend turns a change
 * into Gallium state objects; an unchanged state costs a comparison. */
struct sxgl_gpu_state {
    int viewport[4]; /* x, y (GL: from the bottom), width, height */
    float depth_range[2];
    int scissor_test;
    int scissor[4]; /* GL: x, y from the bottom, width, height */
    int depth_test;
    GLenum depth_func;
    int depth_mask;
    int cull_face;
    GLenum cull_mode;
    GLenum front_face;
    int alpha_test;
    GLenum alpha_func;
    float alpha_reference;
    int blend;
    GLenum blend_source;
    GLenum blend_destination;
    int fog;
    float fog_colour[4];
    /* Texturing: `texture` is NULL when it is off. It points at the texture
     * object's slot for its host copy; the texels are uploaded again whenever
     * `texture_generation` moves. */
    struct sxgl_gpu_texture** texture;
    const uint32_t* texels; /* 0xAARRGGBB, row 0 is t = 0 */
    int texture_width;
    int texture_height;
    uint32_t texture_generation;
    GLenum texture_env;
    GLenum min_filter;
    GLenum mag_filter;
    GLenum wrap_s;
    GLenum wrap_t;
};

/* NULL when there is no 3D device (or it refused): SxGL then rasterizes in
 * software, which is always correct. */
struct sxgl_gpu* sxgl_gpu_create(int width, int height);
void sxgl_gpu_destroy(struct sxgl_gpu* gpu);
int sxgl_gpu_resize(struct sxgl_gpu* gpu, int width, int height);

/* Clears the region (top-down pixels, x0..x1 by y0..y1) of the colour and
 * depth buffers; the whole target when it covers it, a quad otherwise. */
int sxgl_gpu_clear(struct sxgl_gpu* gpu, int clear_colour, int clear_depth, const float colour[4], float depth,
    int x0, int y0, int x1, int y1);
int sxgl_gpu_triangle(struct sxgl_gpu* gpu, const struct sxgl_gpu_state* state, const struct sxgl_gpu_vertex* a,
    const struct sxgl_gpu_vertex* b, const struct sxgl_gpu_vertex* c);
/* Lets a texture's host copy go (glDeleteTextures). */
void sxgl_gpu_texture_free(struct sxgl_gpu* gpu, struct sxgl_gpu_texture** texture);
/* Sends what is queued. */
int sxgl_gpu_flush(struct sxgl_gpu* gpu);
/* Flushes, waits, and copies the region of the render target into `colour`,
 * forcing the top byte of each pixel to `alpha_bits`. */
int sxgl_gpu_read(struct sxgl_gpu* gpu, const struct sx_bitmap* colour, uint32_t alpha_bits, int x0, int y0, int x1,
    int y1);

/* The device layer, sxgl_gpu_device.c. Each answers a negative value on
 * failure. A section is created and mapped in one step. */
long sxgl_gpu_device_open(void);
void sxgl_gpu_device_close(long device);
long sxgl_gpu_device_control(long device, unsigned long code, void* argument);
long sxgl_gpu_device_section(uint64_t bytes, void** view);
void sxgl_gpu_device_section_free(long section, void* view);
