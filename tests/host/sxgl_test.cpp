/*
 * sxgl_test.cpp -- host test of SxGL (system/gfx/sxgl.c).
 *
 * The real rasterizer, compiled for the host, drawing into a bitmap in memory
 * and asserting pixels. A rasterizer is exactly the code where a picture a
 * human would sign off on hides a seam or a double-drawn edge, so the checks
 * here count pixels: every pixel of a mesh drawn once, never twice, never
 * skipped.
 *
 * It also prints the triangles-per-second and fragments-per-second figures
 * docs/desktop/SXGL_ROADMAP.md asks to record with every batch.
 */

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "sxgfx/sxgl.h"
}

namespace {

int g_failures = 0;
int g_checks = 0;

bool check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s\n", what);
    } else {
        std::printf("  ok   %s\n", what);
    }
    std::fflush(nullptr);
    return ok;
}

struct Surface {
    int width;
    int height;
    std::vector<uint32_t> pixels;
    savanxp_fb_info info{};
    sx_bitmap bitmap{};
    sxgl_context* context = nullptr;

    Surface(int w, int h) : width(w), height(h), pixels((size_t)w * (size_t)h, 0u)
    {
        info.width = (uint32_t)w;
        info.height = (uint32_t)h;
        info.pitch = (uint32_t)w * 4u;
        info.bpp = 32u;
        info.buffer_size = info.pitch * info.height;
        bitmap.pixels = pixels.data();
        bitmap.info = info;
        bitmap.format = SX_PIXEL_FORMAT_BGRX8888;
        bitmap.owns_pixels = 0;
        context = sxgl_context_create(&bitmap);
        sxgl_make_current(context);
    }

    ~Surface()
    {
        sxgl_context_destroy(context);
    }

    uint32_t at(int x, int y) const
    {
        return pixels[(size_t)y * (size_t)width + (size_t)x] & 0x00ffffffu;
    }
};

uint32_t rgb(int r, int g, int b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

/* Window coordinates straight through: an orthographic projection that maps
 * GL window pixels (y up) onto the buffer. */
void pixel_projection(const Surface& surface)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, surface.width, 0.0, surface.height, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void test_clear_and_scissor()
{
    std::printf("clear and scissor\n");
    Surface surface(16, 12);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    check(surface.at(0, 0) == rgb(255, 0, 0) && surface.at(15, 11) == rgb(255, 0, 0), "clear fills the buffer");

    glEnable(GL_SCISSOR_TEST);
    glScissor(2, 1, 4, 3); /* GL rows 1..3 from the bottom = buffer rows 8..10 */
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check(surface.at(2, 10) == rgb(0, 0, 255) && surface.at(5, 8) == rgb(0, 0, 255), "scissor clear inside");
    check(surface.at(1, 10) == rgb(255, 0, 0) && surface.at(2, 7) == rgb(255, 0, 0) &&
              surface.at(2, 11) == rgb(255, 0, 0) && surface.at(6, 8) == rgb(255, 0, 0),
        "scissor clear stops at the box, y counted from the bottom");

    sx_rect damage;
    sxgl_swap_buffers(surface.context, &damage);
    check(damage.x == 0 && damage.y == 0 && damage.width == 16 && damage.height == 12,
        "swap reports the whole cleared buffer as damage");
    sxgl_swap_buffers(surface.context, &damage);
    check(damage.width == 0 && damage.height == 0, "a second swap with nothing drawn reports nothing");
}

/* Draws a mesh additively, one unit per triangle, and requires every covered
 * pixel to hold exactly one unit: the fill rule on shared edges. */
void test_fill_rule()
{
    std::printf("fill rule\n");
    Surface surface(64, 64);
    pixel_projection(surface);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    glColor3ub(1, 1, 1);

    /* A fan around an off-grid centre, and a strip with subpixel offsets:
     * vertical, horizontal and diagonal shared edges, some exactly through
     * pixel centres. */
    const float cx = 31.37f;
    const float cy = 30.5f;
    const float ring[][2] = {{4.5f, 4.5f}, {32.0f, 2.25f}, {60.0f, 4.5f}, {61.5f, 32.0f}, {60.0f, 60.0f},
        {32.0f, 61.75f}, {4.5f, 60.0f}, {2.0f, 32.5f}};
    glBegin(GL_TRIANGLE_FAN);
    glVertex2f(cx, cy);
    for (int index = 0; index <= 8; ++index) {
        glVertex2f(ring[index % 8][0], ring[index % 8][1]);
    }
    glEnd();

    int overlap = 0;
    int inside = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint32_t pixel = surface.at(x, y);
            if (pixel == rgb(1, 1, 1)) {
                inside += 1;
            } else if (pixel != 0) {
                overlap += 1;
            }
        }
    }
    check(overlap == 0, "no pixel of the fan is drawn twice");
    check(inside > 3000, "the fan covers its area");

    /* A hole would show as a black pixel strictly inside the octagon. */
    int holes = 0;
    for (int y = 12; y < 52; ++y) {
        for (int x = 12; x < 52; ++x) {
            if (surface.at(x, y) == 0) {
                holes += 1;
            }
        }
    }
    check(holes == 0, "no pixel inside the fan is skipped");

    /* Two triangles meeting on an exactly vertical edge at a pixel centre. */
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glVertex2f(10.5f, 10.0f);
    glVertex2f(20.5f, 10.0f);
    glVertex2f(20.5f, 20.0f);
    glVertex2f(10.5f, 20.0f);
    glVertex2f(20.5f, 10.0f);
    glVertex2f(30.5f, 10.0f);
    glVertex2f(30.5f, 20.0f);
    glVertex2f(20.5f, 20.0f);
    glEnd();
    int column_hits = 0;
    bool clean = true;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            uint32_t pixel = surface.at(x, y);
            if (pixel != 0 && pixel != rgb(1, 1, 1)) {
                clean = false;
            }
        }
        if (surface.at(20, y) == rgb(1, 1, 1)) {
            column_hits += 1;
        }
    }
    check(clean, "abutting quads share their edge without overlap");
    check(column_hits == 10, "the shared column belongs to exactly one quad");
    glDisable(GL_BLEND);
}

void test_depth_and_culling()
{
    std::printf("depth and culling\n");
    Surface surface(32, 32);
    pixel_projection(surface);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    /* Near (z = 0.5 -> depth 0.25) first, far (z = -0.5) after: the near one
     * must survive. glOrtho maps z to -z in NDC, so +0.5 is nearer. */
    glColor3f(0.0f, 1.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex3f(0.0f, 0.0f, 0.5f);
    glVertex3f(32.0f, 0.0f, 0.5f);
    glVertex3f(0.0f, 32.0f, 0.5f);
    glEnd();
    glColor3f(1.0f, 0.0f, 0.0f);
    glBegin(GL_TRIANGLES);
    glVertex3f(0.0f, 0.0f, -0.5f);
    glVertex3f(32.0f, 0.0f, -0.5f);
    glVertex3f(0.0f, 32.0f, -0.5f);
    glEnd();
    check(surface.at(4, 27) == rgb(0, 255, 0), "the nearer triangle wins regardless of order");

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_CULL_FACE);
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_TRIANGLES); /* clockwise in GL window space: a back face */
    glVertex2f(0.0f, 0.0f);
    glVertex2f(0.0f, 32.0f);
    glVertex2f(32.0f, 0.0f);
    glEnd();
    check(surface.at(4, 27) == 0, "a clockwise triangle is culled with GL_BACK");
    glFrontFace(GL_CW);
    glBegin(GL_TRIANGLES);
    glVertex2f(0.0f, 0.0f);
    glVertex2f(0.0f, 32.0f);
    glVertex2f(32.0f, 0.0f);
    glEnd();
    check(surface.at(4, 27) == rgb(255, 255, 255), "the same triangle is drawn with glFrontFace(GL_CW)");
    glFrontFace(GL_CCW);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
}

void test_near_clipping()
{
    std::printf("near-plane clipping\n");
    Surface surface(64, 64);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* A floor running from behind the eye to far away: before batch 2 the
     * whole thing was dropped because one vertex was behind the eye. */
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-4.0f, -1.0f, 5.0f);
    glVertex3f(4.0f, -1.0f, 5.0f);
    glVertex3f(4.0f, -1.0f, -50.0f);
    glVertex3f(-4.0f, -1.0f, -50.0f);
    glEnd();
    check(surface.at(32, 63) == rgb(255, 255, 255), "the part in front of the eye is drawn");
    check(surface.at(32, 10) == 0, "nothing above the horizon");
}

void test_flat_and_smooth()
{
    std::printf("flat and smooth shading\n");
    Surface surface(32, 32);
    pixel_projection(surface);
    glClear(GL_COLOR_BUFFER_BIT);
    glShadeModel(GL_FLAT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex2f(32.0f, 0.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex2f(0.0f, 32.0f);
    glEnd();
    check(surface.at(1, 30) == rgb(0, 0, 255) && surface.at(20, 30) == rgb(0, 0, 255),
        "flat shading takes the last vertex's colour");

    glShadeModel(GL_SMOOTH);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glColor3f(0.0f, 1.0f, 0.0f);
    glVertex2f(32.0f, 0.0f);
    glColor3f(0.0f, 0.0f, 1.0f);
    glVertex2f(0.0f, 32.0f);
    glEnd();
    uint32_t near_red = surface.at(0, 31);
    uint32_t middle = surface.at(10, 21);
    check(((near_red >> 16) & 0xff) > 230, "smooth shading is red at the red corner");
    check(((middle >> 16) & 0xff) > 40 && ((middle >> 8) & 0xff) > 40 && (middle & 0xff) > 40,
        "and a blend of the three inside");
}

void test_texture_and_perspective()
{
    std::printf("textures\n");
    Surface surface(64, 64);
    pixel_projection(surface);
    glClear(GL_COLOR_BUFFER_BIT);

    const unsigned char checker[] = {
        255, 0, 0, 255, 0, 255, 0, 255, /* row t = 0: red, green */
        0, 0, 255, 255, 255, 255, 255, 0, /* row t = 1: blue, transparent white */
    };
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, checker);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0f, 1.0f, 1.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(64.0f, 0.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(64.0f, 64.0f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(0.0f, 64.0f);
    glEnd();
    /* t = 0 is the bottom of the quad (GL y up): the bottom rows of the
     * buffer. */
    check(surface.at(10, 54) == rgb(255, 0, 0), "texel (0,0) at the bottom left");
    check(surface.at(54, 54) == rgb(0, 255, 0), "texel (1,0) at the bottom right");
    check(surface.at(10, 10) == rgb(0, 0, 255), "texel (0,1) at the top left");

    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.5f);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex2f(0.0f, 0.0f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex2f(64.0f, 0.0f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex2f(64.0f, 64.0f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex2f(0.0f, 64.0f);
    glEnd();
    check(surface.at(54, 10) == 0, "the alpha test discards the transparent texel");
    check(surface.at(10, 10) == rgb(0, 0, 255), "and keeps the opaque ones");
    glDisable(GL_ALPHA_TEST);

    /* Perspective: a floor quad with a two-texel texture along t. Affine
     * interpolation would put the boundary at the middle screen row of the
     * quad; perspective-correct puts it where the world-space midpoint
     * projects, much nearer the horizon. */
    const unsigned char stripes[] = {255, 0, 0, 255, 0, 255, 0, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, stripes);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f);
    glVertex3f(-8.0f, -4.0f, -1.5f);
    glTexCoord2f(1.0f, 0.0f);
    glVertex3f(8.0f, -4.0f, -1.5f);
    glTexCoord2f(1.0f, 1.0f);
    glVertex3f(8.0f, -4.0f, -21.5f);
    glTexCoord2f(0.0f, 1.0f);
    glVertex3f(-8.0f, -4.0f, -21.5f);
    glEnd();
    /* The midpoint z = -11.5 projects to ndc y = -4/11.5: buffer row
     * (1 - (ndc + 1) / 2) * 64, about 43. Affine interpolation would put it
     * halfway down the visible part of the floor, near row 48. */
    double boundary = (1.0 - ((-4.0 / 11.5) + 1.0) / 2.0) * 64.0;
    int expected = (int)std::floor(boundary);
    bool below_green = surface.at(32, expected - 2) == rgb(0, 255, 0);
    bool above_red = surface.at(32, expected + 2) == rgb(255, 0, 0);
    check(below_green && above_red, "the stripe boundary is where perspective puts it");
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &name);
}

void test_fog_and_lighting()
{
    std::printf("fog and lighting\n");
    Surface surface(32, 32);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-1.0, 1.0, -1.0, 1.0, 1.0, 100.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_FOG);
    glFogi(GL_FOG_MODE, GL_LINEAR);
    glFogf(GL_FOG_START, 2.0f);
    glFogf(GL_FOG_END, 10.0f);
    const float fog_colour[] = {0.0f, 0.0f, 1.0f, 1.0f};
    glFogfv(GL_FOG_COLOR, fog_colour);
    glColor3f(1.0f, 0.0f, 0.0f);
    glBegin(GL_QUADS); /* a wall at z = -20, beyond the fog end */
    glVertex3f(-30.0f, -30.0f, -20.0f);
    glVertex3f(30.0f, -30.0f, -20.0f);
    glVertex3f(30.0f, 30.0f, -20.0f);
    glVertex3f(-30.0f, 30.0f, -20.0f);
    glEnd();
    check(surface.at(16, 16) == rgb(0, 0, 255), "beyond GL_FOG_END only the fog colour remains");
    glDisable(GL_FOG);

    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    const float position[] = {0.0f, 0.0f, 1.0f, 0.0f};
    glLightfv(GL_LIGHT0, GL_POSITION, position);
    const float white[] = {1.0f, 1.0f, 1.0f, 1.0f};
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, white);
    glNormal3f(0.0f, 0.0f, 1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-30.0f, -30.0f, -20.0f);
    glVertex3f(30.0f, -30.0f, -20.0f);
    glVertex3f(30.0f, 30.0f, -20.0f);
    glVertex3f(-30.0f, 30.0f, -20.0f);
    glEnd();
    check(surface.at(16, 16) == rgb(255, 255, 255), "a surface facing a white light is fully lit");
    glNormal3f(0.0f, 0.0f, -1.0f);
    glBegin(GL_QUADS);
    glVertex3f(-30.0f, -30.0f, -10.0f);
    glVertex3f(30.0f, -30.0f, -10.0f);
    glVertex3f(30.0f, 30.0f, -10.0f);
    glVertex3f(-30.0f, 30.0f, -10.0f);
    glEnd();
    check(surface.at(16, 16) == rgb(51, 51, 51), "facing away, only the 0.2 ambient is left");
    glDisable(GL_LIGHTING);
}

std::string g_last_debug;

void remember(const char* message, void*)
{
    g_last_debug = message;
}

void test_errors()
{
    std::printf("errors and the debug layer\n");
    Surface surface(8, 8);
    sxgl_set_debug_sink(surface.context, remember, nullptr);
    check(glGetError() == GL_NO_ERROR, "a new context has no error");
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    unsigned char pixels[3 * 3 * 4] = {};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 3, 3, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    check(glGetError() == GL_INVALID_VALUE, "a non-power-of-two texture is GL_INVALID_VALUE");
    check(g_last_debug.find("glTexImage2D") == 0 && g_last_debug.find("power of two") != std::string::npos,
        "the debug sink names the call and the reason");
    check(glGetError() == GL_NO_ERROR, "glGetError clears the error");
    glEnable(0x0B10); /* GL_POINT_SMOOTH: not in the subset */
    check(glGetError() == GL_INVALID_ENUM, "a capability outside the subset is refused, not ignored");
    glBegin(GL_TRIANGLES);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnd();
    check(glGetError() == GL_INVALID_OPERATION, "state calls inside glBegin/glEnd are refused");
    glMatrixMode(GL_PROJECTION);
    for (int index = 0; index < 4; ++index) {
        glPushMatrix();
    }
    check(glGetError() == GL_STACK_OVERFLOW, "the projection stack is four deep");
    check(std::strcmp((const char*)glGetString(GL_VERSION), "1.1 SxGL subset") == 0,
        "GL_VERSION says it is a subset");
}

void test_arrays()
{
    std::printf("client arrays\n");
    Surface surface(16, 16);
    pixel_projection(surface);
    glClear(GL_COLOR_BUFFER_BIT);
    const float positions[] = {0.0f, 0.0f, 16.0f, 0.0f, 16.0f, 16.0f, 0.0f, 16.0f};
    const unsigned char colours[] = {0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
    const unsigned short indices[] = {0, 1, 2, 0, 2, 3};
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(2, GL_FLOAT, 0, positions);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, colours);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);
    check(surface.at(0, 0) == rgb(0, 255, 0) && surface.at(15, 15) == rgb(0, 255, 0),
        "glDrawElements fills the quad from the arrays");
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}

void test_resize()
{
    std::printf("resize\n");
    Surface surface(8, 8);
    std::vector<uint32_t> larger(32 * 16, 0u);
    sx_bitmap bitmap = surface.bitmap;
    bitmap.pixels = larger.data();
    bitmap.info.width = 32;
    bitmap.info.height = 16;
    bitmap.info.pitch = 32 * 4;
    bitmap.info.buffer_size = bitmap.info.pitch * 16;
    check(sxgl_context_resize(surface.context, &bitmap) == 0, "a context follows a larger surface");
    GLint viewport[4] = {};
    glGetIntegerv(GL_VIEWPORT, viewport);
    check(viewport[2] == 32 && viewport[3] == 16, "and its viewport is reset to it");
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    check((larger[31 + 15 * 32] & 0xffffffu) == rgb(0, 255, 0), "clear reaches the new corner");
}

/* The figure the roadmap wants recorded per batch: a lit, depth-tested scene
 * of many small triangles, plus a textured and fogged one. */
void benchmark()
{
    std::printf("benchmark\n");
    Surface surface(640, 480);
    sxgl_stats stats{};

    auto run = [&](const char* label, bool textured) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-0.8, 0.8, -0.6, 0.6, 1.0, 100.0);
        glMatrixMode(GL_MODELVIEW);
        glEnable(GL_DEPTH_TEST);
        GLuint name = 0;
        if (textured) {
            std::vector<unsigned char> image(64 * 64 * 4);
            for (int index = 0; index < 64 * 64; ++index) {
                unsigned char value = (unsigned char)(((index & 7) ^ ((index >> 6) & 7)) * 32);
                image[(size_t)index * 4 + 0] = value;
                image[(size_t)index * 4 + 1] = (unsigned char)(255 - value);
                image[(size_t)index * 4 + 2] = 64;
                image[(size_t)index * 4 + 3] = 255;
            }
            glGenTextures(1, &name);
            glBindTexture(GL_TEXTURE_2D, name);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.data());
            glEnable(GL_TEXTURE_2D);
            glEnable(GL_FOG);
            glFogi(GL_FOG_MODE, GL_LINEAR);
            glFogf(GL_FOG_START, 5.0f);
            glFogf(GL_FOG_END, 40.0f);
        } else {
            glEnable(GL_LIGHTING);
            glEnable(GL_LIGHT0);
            glEnable(GL_COLOR_MATERIAL);
        }
        sxgl_reset_stats(surface.context);
        auto start = std::chrono::steady_clock::now();
        const int frames = 30;
        for (int frame = 0; frame < frames; ++frame) {
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glLoadIdentity();
            glTranslatef(0.0f, 0.0f, -12.0f);
            glRotatef((float)frame * 3.0f, 0.3f, 1.0f, 0.1f);
            glBegin(GL_TRIANGLES);
            for (int ring = 0; ring < 40; ++ring) {
                for (int segment = 0; segment < 60; ++segment) {
                    float a0 = (float)segment * 6.2831853f / 60.0f;
                    float a1 = (float)(segment + 1) * 6.2831853f / 60.0f;
                    float y0 = -4.0f + (float)ring * 0.2f;
                    float y1 = y0 + 0.2f;
                    float r = 3.0f + 0.5f * std::sin((float)ring * 0.4f);
                    glColor3f(0.4f + 0.01f * (float)ring, 0.6f, 0.3f);
                    glNormal3f(std::cos(a0), 0.0f, std::sin(a0));
                    glTexCoord2f(a0, y0);
                    glVertex3f(r * std::cos(a0), y0, r * std::sin(a0));
                    glTexCoord2f(a1, y0);
                    glVertex3f(r * std::cos(a1), y0, r * std::sin(a1));
                    glTexCoord2f(a1, y1);
                    glVertex3f(r * std::cos(a1), y1, r * std::sin(a1));
                }
            }
            glEnd();
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        sxgl_get_stats(surface.context, &stats);
        std::printf("  %-10s %8.0f triangles/s  %10.0f fragments/s  %6.1f frames/s at 640x480\n", label,
            (double)stats.triangles_rasterized / seconds, (double)stats.fragments_written / seconds,
            frames / seconds);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_FOG);
        glDisable(GL_LIGHTING);
        glDisable(GL_COLOR_MATERIAL);
        if (name != 0) {
            glDeleteTextures(1, &name);
        }
    };
    run("lit", false);
    run("textured", true);
}

} // namespace

int main(int argc, char** argv)
{
    bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    test_clear_and_scissor();
    test_fill_rule();
    test_depth_and_culling();
    test_near_clipping();
    test_flat_and_smooth();
    test_texture_and_perspective();
    test_fog_and_lighting();
    test_errors();
    test_arrays();
    test_resize();
    benchmark();
    (void)bench;
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
