/* sxgltest: SxGL's GPU backend against its software rasterizer
 * (docs/roadmaps/open/VIRGL_ROADMAP.md).
 *
 * Draws each scene twice, into two bitmaps, once per backend, and compares
 * them pixel by pixel. The scenes touch what the GPU backend translates:
 * depth test and depth range, culling and front faces, smooth colour, fog,
 * blending, the alpha test, the scissor and a scissored clear; then textures
 * under each environment and filter, an update between two draws, and a
 * perspective textured quad under fog. The two
 * rasterizers do not agree on every edge pixel, so the check is a tolerance
 * per channel and a budget of pixels allowed past it.
 *
 * Without a 3D device the GPU context falls back to software, which the test
 * reports and accepts. Prints SXGLTEST PASS or SXGLTEST FAIL.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sxgfx/gfx2d.h"
#include "sxgfx/sxgl.h"
#include "sxn/sxn.h"

#define WIDTH 160
#define HEIGHT 120
/* A channel this far apart is a different pixel. */
#define TOLERANCE 6
/* Edge pixels the two rasterizers may disagree on: 0.5% of the image. */
#define MISMATCH_BUDGET (WIDTH * HEIGHT / 200)

static int g_failures;

static void report(const char* text)
{
    (void)sxn_log(text);
}

static void check(int ok, const char* what)
{
    char line[160];

    if (ok) {
        return;
    }
    g_failures += 1;
    snprintf(line, sizeof(line), "sxgltest: FAILED %s", what);
    report(line);
}

static void quad(float x0, float y0, float x1, float y1, float z)
{
    glVertex3f(x0, y0, z);
    glVertex3f(x1, y0, z);
    glVertex3f(x1, y1, z);
    glVertex3f(x0, y1, z);
}

static void draw_scene(void)
{
    static const float fog_colour[4] = {0.3f, 0.3f, 0.6f, 1.0f};

    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClearDepth(1.0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    /* Depth: the green quad is nearer and drawn second, the blue one is
     * farther and drawn last; green must win where they overlap. */
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBegin(GL_QUADS);
    glColor3f(1.0f, 0.0f, 0.0f);
    quad(-0.9f, 0.1f, -0.1f, 0.9f, 0.5f);
    glColor3f(0.0f, 1.0f, 0.0f);
    quad(-0.6f, 0.3f, 0.2f, 0.8f, -0.5f);
    glColor3f(0.0f, 0.0f, 1.0f);
    quad(-0.4f, 0.2f, 0.4f, 0.6f, 0.0f);
    glEnd();
    glDisable(GL_DEPTH_TEST);

    /* Smooth colour, and a clockwise triangle that culling removes. */
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex3f(0.1f, 0.1f, 0.0f);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex3f(0.9f, 0.1f, 0.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex3f(0.5f, 0.9f, 0.0f);
    glColor3f(1.0f, 1.0f, 1.0f);
    glVertex3f(0.1f, -0.1f, 0.0f);
    glVertex3f(0.9f, -0.1f, 0.0f);
    glVertex3f(0.5f, -0.9f, 0.0f);
    glEnd();
    glDisable(GL_CULL_FACE);

    /* Fog over a quad whose depth goes from near to far. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 1.0f);
    glFogf(GL_FOG_END, 6.0f);
    glFogfv(GL_FOG_COLOR, fog_colour);
    glBegin(GL_QUADS);
    glColor3f(1.0f, 0.8f, 0.2f);
    glVertex3f(-0.9f, -0.9f, -1.2f);
    glVertex3f(-0.1f, -0.9f, -1.2f);
    glVertex3f(-0.1f, -0.1f, -5.0f);
    glVertex3f(-0.9f, -0.1f, -5.0f);
    glEnd();
    glDisable(GL_FOG);

    /* Blending, then the alpha test, back in orthographic projection. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBegin(GL_QUADS);
    glColor4f(1.0f, 1.0f, 1.0f, 0.5f);
    quad(-0.75f, -0.75f, -0.25f, -0.25f, 0.0f);
    glEnd();
    glDisable(GL_BLEND);
    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glBegin(GL_QUADS);
    glColor4f(0.0f, 1.0f, 1.0f, 0.25f); /* rejected */
    quad(0.2f, -0.9f, 0.5f, -0.6f, 0.0f);
    glColor4f(1.0f, 0.0f, 1.0f, 0.75f); /* kept */
    quad(0.6f, -0.9f, 0.9f, -0.6f, 0.0f);
    glEnd();
    glDisable(GL_ALPHA_TEST);

    /* A scissored clear and a scissored draw. */
    glEnable(GL_SCISSOR_TEST);
    glScissor(4, 4, 20, 12);
    glClearColor(1.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glScissor(130, 90, 24, 24);
    glBegin(GL_TRIANGLES);
    glColor3f(0.0f, 1.0f, 1.0f);
    glVertex3f(-1.0f, -1.0f, 0.0f);
    glVertex3f(1.0f, -1.0f, 0.0f);
    glVertex3f(1.0f, 1.0f, 0.0f);
    glEnd();
    glDisable(GL_SCISSOR_TEST);
}

/* An 8x8 RGBA checker: opaque red and half-transparent white, with a green
 * column so orientation shows. */
static void make_texels(unsigned char* texels, int flip)
{
    int x;
    int y;

    for (y = 0; y < 8; ++y) {
        for (x = 0; x < 8; ++x) {
            unsigned char* texel = texels + (y * 8 + x) * 4;
            int odd = ((x + y) & 1) ^ flip;
            texel[0] = x == 0 ? 0 : (odd ? 255 : 240);
            texel[1] = x == 0 ? 255 : (odd ? 255 : 30);
            texel[2] = x == 0 ? 0 : (odd ? 255 : 30);
            texel[3] = odd ? 128 : 255;
        }
    }
}

/* The corners are nudged off round numbers: where a texel edge falls exactly
 * on a pixel centre, the two rasterizers may break the tie differently, and
 * that is not a difference worth failing on. */
static void textured_quad(float x0, float y0, float x1, float y1, float repeat)
{
    x0 += 0.0131f;
    x1 -= 0.0071f;
    y0 += 0.0173f;
    y1 -= 0.0093f;
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(x0, y0, 0.0f);
    glTexCoord2f(repeat, 0.0f);
    glVertex3f(x1, y0, 0.0f);
    glTexCoord2f(repeat, repeat);
    glVertex3f(x1, y1, 0.0f);
    glTexCoord2f(0.0f, repeat);
    glVertex3f(x0, y1, 0.0f);
    glEnd();
}

static void draw_textures(void)
{
    static const float fog_colour[4] = {0.2f, 0.2f, 0.2f, 1.0f};
    unsigned char texels[8 * 8 * 4];
    GLuint names[2];

    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(0.0f, 0.0f, 0.3f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glGenTextures(2, names);
    make_texels(texels, 0);
    glBindTexture(GL_TEXTURE_2D, names[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glEnable(GL_TEXTURE_2D);

    /* MODULATE, nearest, repeated twice, tinted. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(0.5f, 1.0f, 1.0f, 1.0f);
    textured_quad(-0.95f, 0.05f, -0.35f, 0.95f, 2.0f);
    /* REPLACE: the texel's colour and alpha, under blending. */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    textured_quad(-0.3f, 0.05f, 0.3f, 0.95f, 1.0f);
    glDisable(GL_BLEND);
    /* DECAL over a coloured fragment. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    glColor4f(0.0f, 0.0f, 1.0f, 1.0f);
    textured_quad(0.35f, 0.05f, 0.95f, 0.95f, 1.0f);

    /* The texels change between two draws: each draw has to see its own. */
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    textured_quad(-0.95f, -0.95f, -0.65f, -0.05f, 1.0f);
    make_texels(texels, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 8, 8, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    textured_quad(-0.6f, -0.95f, -0.3f, -0.05f, 1.0f);

    /* A second texture, linear and clamped, magnified. */
    glBindTexture(GL_TEXTURE_2D, names[1]);
    make_texels(texels, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    textured_quad(-0.25f, -0.95f, 0.25f, -0.05f, 1.5f);

    /* Perspective texturing under fog. */
    glBindTexture(GL_TEXTURE_2D, names[0]);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 10.0);
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 1.0f);
    glFogf(GL_FOG_END, 6.0f);
    glFogfv(GL_FOG_COLOR, fog_colour);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(0.3f, -0.9f, -1.2f);
    glTexCoord2f(4.0f, 0.0f);
    glVertex3f(0.9f, -0.9f, -1.2f);
    glTexCoord2f(4.0f, 4.0f);
    glVertex3f(0.9f, -0.1f, -5.0f);
    glTexCoord2f(0.0f, 4.0f);
    glVertex3f(0.3f, -0.1f, -5.0f);
    glEnd();
    glDisable(GL_FOG);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(2, names);
}

static int channel_distance(uint32_t a, uint32_t b, int shift)
{
    int left = (int)((a >> shift) & 0xffu);
    int right = (int)((b >> shift) & 0xffu);
    return left > right ? left - right : right - left;
}

static uint32_t pixel_at(const struct sx_bitmap* bitmap, int x, int y)
{
    return bitmap->pixels[(size_t)y * (bitmap->info.pitch / 4u) + (size_t)x] & 0x00ffffffu;
}

/* Draws `scene` with both contexts and compares the two images. */
static void compare_scene(const char* name, void (*scene)(void), struct sxgl_context* software_context,
    struct sx_bitmap* software, struct sxgl_context* gpu_context, struct sx_bitmap* gpu)
{
    struct sx_rect damage;
    char line[200];
    int mismatches = 0;
    int worst = 0;
    int x;
    int y;

    sxgl_make_current(software_context);
    scene();
    check(glGetError() == GL_NO_ERROR, "the software scene raises no error");
    sxgl_swap_buffers(software_context, &damage);

    sxgl_make_current(gpu_context);
    scene();
    check(glGetError() == GL_NO_ERROR, "the GPU scene raises no error");
    sxgl_swap_buffers(gpu_context, &damage);
    check(damage.width == WIDTH && damage.height == HEIGHT, "the GPU frame reports the whole buffer as damage");

    for (y = 0; y < HEIGHT; ++y) {
        for (x = 0; x < WIDTH; ++x) {
            uint32_t a = pixel_at(software, x, y);
            uint32_t b = pixel_at(gpu, x, y);
            int distance = channel_distance(a, b, 0);
            int green = channel_distance(a, b, 8);
            int red = channel_distance(a, b, 16);
            if (green > distance) {
                distance = green;
            }
            if (red > distance) {
                distance = red;
            }
            if (distance > worst) {
                worst = distance;
            }
            if (distance > TOLERANCE) {
                if (mismatches < 6) {
                    snprintf(line, sizeof(line), "sxgltest: %s (%d,%d) software %06x gpu %06x", name, x, y,
                        (unsigned)a, (unsigned)b);
                    report(line);
                }
                mismatches += 1;
            }
        }
    }
    snprintf(line, sizeof(line), "sxgltest: %s: %d of %d pixels differ by more than %d (worst %d, budget %d)", name,
        mismatches, WIDTH * HEIGHT, TOLERANCE, worst, MISMATCH_BUDGET);
    report(line);
    check(mismatches <= MISMATCH_BUDGET, "the GPU image matches the software one");
}

int main(void)
{
    struct sx_bitmap software;
    struct sx_bitmap gpu;
    struct sxgl_context* software_context;
    struct sxgl_context* gpu_context;

    if (!sx_bitmap_create(&software, WIDTH, HEIGHT, SX_PIXEL_FORMAT_BGRX8888) ||
        !sx_bitmap_create(&gpu, WIDTH, HEIGHT, SX_PIXEL_FORMAT_BGRX8888)) {
        report("SXGLTEST FAIL");
        sxn_exit(1);
    }
    software_context = sxgl_context_create_ex(&software, SXGL_CONTEXT_SOFTWARE);
    gpu_context = sxgl_context_create_ex(&gpu, SXGL_CONTEXT_GPU);
    check(software_context != 0 && gpu_context != 0, "both contexts are created");
    if (software_context == 0 || gpu_context == 0) {
        report("SXGLTEST FAIL");
        sxn_exit(1);
    }
    check(sxgl_context_backend(software_context) == SXGL_BACKEND_SOFTWARE, "the software context stays software");
    if (sxgl_context_backend(gpu_context) != SXGL_BACKEND_GPU) {
        report("sxgltest: no 3D device; the GPU context fell back to software");
    } else {
        sxgl_make_current(gpu_context);
        report((const char*)glGetString(GL_RENDERER));
    }

    compare_scene("scene", draw_scene, software_context, &software, gpu_context, &gpu);
    /* Spot checks that do not depend on the other backend being right. */
    check(pixel_at(&gpu, 10, HEIGHT - 10) == 0xffff00u, "the scissored clear is yellow at its corner");
    check(pixel_at(&gpu, 30, HEIGHT - 30) != 0xffff00u, "and only inside the scissor");
    check(channel_distance(pixel_at(&gpu, 120, 90), 0x1a1a1au, 0) <= 1 &&
            channel_distance(pixel_at(&gpu, 120, 90), 0x1a1a1au, 8) <= 1 &&
            channel_distance(pixel_at(&gpu, 120, 90), 0x1a1a1au, 16) <= 1,
        "the clockwise triangle is culled");

    compare_scene("textures", draw_textures, software_context, &software, gpu_context, &gpu);
    check(sxgl_context_backend(gpu_context) == SXGL_BACKEND_GPU ||
            sxgl_context_backend(software_context) == SXGL_BACKEND_SOFTWARE,
        "the backends are what they were");

    sxgl_context_destroy(gpu_context);
    sxgl_context_destroy(software_context);
    sx_bitmap_destroy(&gpu);
    sx_bitmap_destroy(&software);
    report(g_failures == 0 ? "SXGLTEST PASS" : "SXGLTEST FAIL");
    sxn_exit(g_failures == 0 ? 0 : 1);
    return 0;
}
