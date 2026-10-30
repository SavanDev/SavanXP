/* Gears: the first consumer of SxGL (docs/desktop/SXGL_ROADMAP.md).
 *
 * Until SxGL existed this file rasterized by hand, in three blocks: matrices,
 * a rasterizer and the scene. The first two were what batches 1 and 2 of the
 * roadmap absorbed, and are gone; what is left is the scene, now a GL client.
 * What it calls is still the specification of that part of the subset:
 * model/view/projection, a viewport inside a window, the depth test, backface
 * culling and flat shading with one directional light.
 *
 * The scene is glxgears' -- three meshed gears -- because it is the program a
 * GL has been measured with since 1996, and because its geometry is demanding
 * in the right way: many small triangles, a lot of silhouette, and back faces
 * that HAVE to disappear for it to look right.
 *
 * It is a POSIX program: it presents what SxGL drew with gfx_present_region.
 * The SUN side of the same seam is the forest game, which presents with
 * sxnwm_submit_rects.
 */

#include "libc.h"

#include <math.h>

#include "sxgfx/sxgl.h"

#define GEARS_PI 3.14159265358979323846f

/* The rasterizer is fill-rate bound, so the 3D scene lives in a bounded,
 * centred viewport instead of filling the screen. It is the honest choice for
 * software, and it exercises the glViewport-inside-a-window case the roadmap
 * says the seam gives for free. */
#define VIEW_MAX_WIDTH 800
#define VIEW_MAX_HEIGHT 600

#define GEAR_MAX_TRIANGLES 420
#define GEAR_COUNT 3

struct vec3 {
    float x;
    float y;
    float z;
};

static struct vec3 vec3_make(float x, float y, float z)
{
    struct vec3 result;
    result.x = x;
    result.y = y;
    result.z = z;
    return result;
}

static struct vec3 vec3_sub(struct vec3 left, struct vec3 right)
{
    return vec3_make(left.x - right.x, left.y - right.y, left.z - right.z);
}

static struct vec3 vec3_cross(struct vec3 left, struct vec3 right)
{
    return vec3_make(
        (left.y * right.z) - (left.z * right.y),
        (left.z * right.x) - (left.x * right.z),
        (left.x * right.y) - (left.y * right.x));
}

static struct vec3 vec3_normalize(struct vec3 value)
{
    float length = sqrtf((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
    if (length <= 1e-6f) {
        return vec3_make(0.0f, 0.0f, 1.0f);
    }
    return vec3_make(value.x / length, value.y / length, value.z / length);
}

struct triangle {
    struct vec3 a;
    struct vec3 b;
    struct vec3 c;
    struct vec3 normal;
};

struct gear {
    struct triangle triangles[GEAR_MAX_TRIANGLES];
    int triangle_count;
    struct vec3 colour;
    float position_x;
    float position_y;
    float phase_degrees;
    float speed;
};

static struct gear g_gears[GEAR_COUNT];

static struct vec3 polar(float radius, float angle, float z)
{
    return vec3_make(radius * cosf(angle), radius * sinf(angle), z);
}

static void push_triangle(struct gear* gear, struct vec3 a, struct vec3 b, struct vec3 c)
{
    struct triangle* triangle;

    if (gear->triangle_count >= GEAR_MAX_TRIANGLES) {
        return;
    }
    triangle = &gear->triangles[gear->triangle_count];
    triangle->a = a;
    triangle->b = b;
    triangle->c = c;
    /* The normal comes from the geometry and the vertex order, so the winding
     * is the only source of truth: a triangle loaded backwards is lit
     * backwards AND culled backwards, consistently. */
    triangle->normal = vec3_normalize(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));
    gear->triangle_count += 1;
}

static void push_quad(struct gear* gear, struct vec3 a, struct vec3 b, struct vec3 c, struct vec3 d)
{
    push_triangle(gear, a, b, c);
    push_triangle(gear, a, c, d);
}

/* The same parameters as glxgears' gear(): inner radius, outer radius, width,
 * number of teeth and tooth depth. */
static void build_gear(
    struct gear* gear,
    float inner_radius,
    float outer_radius,
    float width,
    int teeth,
    float tooth_depth,
    struct vec3 colour)
{
    float half_width = width * 0.5f;
    float root_radius = outer_radius - (tooth_depth * 0.5f);
    float tip_radius = outer_radius + (tooth_depth * 0.5f);
    float step = (2.0f * GEARS_PI) / (float)teeth;
    int tooth;

    gear->triangle_count = 0;
    gear->colour = colour;

    for (tooth = 0; tooth < teeth; ++tooth) {
        float a0 = (float)tooth * step;
        float a1 = a0 + (step * 0.25f);
        float a2 = a0 + (step * 0.5f);
        float a3 = a0 + (step * 0.75f);
        float a4 = a0 + step;

        /* Front face: the ring up to the root of the tooth, plus the tooth. */
        push_quad(gear,
            polar(inner_radius, a0, half_width),
            polar(root_radius, a0, half_width),
            polar(root_radius, a4, half_width),
            polar(inner_radius, a4, half_width));
        push_quad(gear,
            polar(root_radius, a0, half_width),
            polar(tip_radius, a1, half_width),
            polar(tip_radius, a2, half_width),
            polar(root_radius, a3, half_width));

        /* Back face: the same with the winding reversed. */
        push_quad(gear,
            polar(inner_radius, a4, -half_width),
            polar(root_radius, a4, -half_width),
            polar(root_radius, a0, -half_width),
            polar(inner_radius, a0, -half_width));
        push_quad(gear,
            polar(root_radius, a3, -half_width),
            polar(tip_radius, a2, -half_width),
            polar(tip_radius, a1, -half_width),
            polar(root_radius, a0, -half_width));

        /* Outer walls, walking the outline by increasing angle: flank, tip,
         * flank and root arc. */
        {
            struct vec3 contour[5];
            int edge;

            contour[0] = polar(root_radius, a0, 0.0f);
            contour[1] = polar(tip_radius, a1, 0.0f);
            contour[2] = polar(tip_radius, a2, 0.0f);
            contour[3] = polar(root_radius, a3, 0.0f);
            contour[4] = polar(root_radius, a4, 0.0f);

            for (edge = 0; edge < 4; ++edge) {
                struct vec3 front_a = vec3_make(contour[edge].x, contour[edge].y, half_width);
                struct vec3 front_b = vec3_make(contour[edge + 1].x, contour[edge + 1].y, half_width);
                struct vec3 back_a = vec3_make(contour[edge].x, contour[edge].y, -half_width);
                struct vec3 back_b = vec3_make(contour[edge + 1].x, contour[edge + 1].y, -half_width);
                push_quad(gear, front_a, back_a, back_b, front_b);
            }
        }

        /* Inner bore: the normal points at the axis, which is what is seen
         * from inside the hole. */
        push_quad(gear,
            polar(inner_radius, a0, half_width),
            polar(inner_radius, a4, half_width),
            polar(inner_radius, a4, -half_width),
            polar(inner_radius, a0, -half_width));
    }
}

static void draw_gear(const struct gear* gear)
{
    int index;

    glColor3f(gear->colour.x, gear->colour.y, gear->colour.z);
    glBegin(GL_TRIANGLES);
    for (index = 0; index < gear->triangle_count; ++index) {
        const struct triangle* triangle = &gear->triangles[index];
        /* Flat shading lights each triangle at its last vertex, with the
         * normal current when it arrives: one normal per face. */
        glNormal3f(triangle->normal.x, triangle->normal.y, triangle->normal.z);
        glVertex3f(triangle->a.x, triangle->a.y, triangle->a.z);
        glVertex3f(triangle->b.x, triangle->b.y, triangle->b.z);
        glVertex3f(triangle->c.x, triangle->c.y, triangle->c.z);
    }
    glEnd();
}

static void build_scene(void)
{
    build_gear(&g_gears[0], 1.0f, 4.0f, 1.0f, 20, 0.7f, vec3_make(0.8f, 0.1f, 0.0f));
    g_gears[0].position_x = -3.0f;
    g_gears[0].position_y = -2.0f;
    g_gears[0].phase_degrees = 0.0f;
    g_gears[0].speed = 1.0f;

    build_gear(&g_gears[1], 0.5f, 2.0f, 2.0f, 10, 0.7f, vec3_make(0.0f, 0.8f, 0.2f));
    g_gears[1].position_x = 3.1f;
    g_gears[1].position_y = -2.0f;
    g_gears[1].phase_degrees = -9.0f;
    g_gears[1].speed = -2.0f;

    build_gear(&g_gears[2], 1.3f, 2.0f, 0.5f, 10, 0.7f, vec3_make(0.2f, 0.2f, 1.0f));
    g_gears[2].position_x = -3.1f;
    g_gears[2].position_y = 4.2f;
    g_gears[2].phase_degrees = -25.0f;
    g_gears[2].speed = -2.0f;
}

/* The fixed state of the scene, set once per context. */
static void setup_gl(void)
{
    /* A directional light in view space, as in glxgears: set while the
     * modelview is the identity. Ambient 0.25 and diffuse 0.75 reproduce the
     * hand-rolled shading this demo had before SxGL. */
    static const GLfloat light_position[] = {0.35f, 0.35f, 0.87f, 0.0f};
    static const GLfloat light_ambient[] = {0.25f, 0.25f, 0.25f, 1.0f};
    static const GLfloat light_diffuse[] = {0.75f, 0.75f, 0.75f, 1.0f};
    static const GLfloat no_ambient[] = {0.0f, 0.0f, 0.0f, 1.0f};

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, light_position);
    glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, no_ambient);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glShadeModel(GL_FLAT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(10.0f / 255.0f, 14.0f / 255.0f, 22.0f / 255.0f, 1.0f);
}

struct target {
    int origin_x;
    int origin_y;
    int width;
    int height;
    /* There is room above and below the viewport for the title and the help.
     * When there is not, the viewport takes the whole window and no text is
     * drawn: the alternative is blitting at negative coordinates. */
    int chrome;
};

#define CHROME_TOP 28
#define CHROME_BOTTOM 72

static struct target make_target(const struct savanxp_gfx_context* gfx)
{
    struct target target;
    int window_width = (int)gfx->info.width;
    int window_height = (int)gfx->info.height;
    int top = CHROME_TOP;
    int available = window_height - CHROME_TOP - CHROME_BOTTOM;

    target.chrome = (available >= 240 && window_width >= 320);
    if (!target.chrome) {
        top = 0;
        available = window_height;
    }

    target.width = window_width > VIEW_MAX_WIDTH ? VIEW_MAX_WIDTH : window_width;
    target.height = available > VIEW_MAX_HEIGHT ? VIEW_MAX_HEIGHT : available;
    if (target.width < 1) {
        target.width = 1;
    }
    if (target.height < 1) {
        target.height = 1;
    }
    target.origin_x = (window_width - target.width) / 2;
    target.origin_y = top + ((available - target.height) / 2);
    return target;
}

/* The viewport and the scissor in GL's window coordinates, which count rows
 * from the bottom; the clear then stays inside the 3D area and leaves the
 * text around it alone. */
static void apply_target(const struct savanxp_gfx_context* gfx, const struct target* target)
{
    int gl_y = (int)gfx->info.height - (target->origin_y + target->height);

    glViewport(target->origin_x, gl_y, target->width, target->height);
    glScissor(target->origin_x, gl_y, target->width, target->height);
}

static void draw_scene(const struct target* target, float angle_degrees, float view_rot_x, float view_rot_y)
{
    /* "Contain" framing: the short side keeps the reference field of view and
     * the long one widens. glxgears fixes the horizontal and lets the vertical
     * follow the aspect, which in a wide window cut the gears off at the top
     * and bottom. */
    float aspect = (float)target->width / (float)target->height;
    double half_x = aspect >= 1.0f ? (double)aspect : 1.0;
    double half_y = aspect >= 1.0f ? 1.0 : 1.0 / (double)aspect;
    int index;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-half_x, half_x, -half_y, half_y, 5.0, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -44.0f);
    glRotatef(view_rot_x, 1.0f, 0.0f, 0.0f);
    glRotatef(view_rot_y, 0.0f, 1.0f, 0.0f);

    for (index = 0; index < GEAR_COUNT; ++index) {
        const struct gear* gear = &g_gears[index];
        glPushMatrix();
        glTranslatef(gear->position_x, gear->position_y, 0.0f);
        glRotatef((angle_degrees * gear->speed) + gear->phase_degrees, 0.0f, 0.0f, 1.0f);
        draw_gear(gear);
        glPopMatrix();
    }
}

static void draw_chrome(struct savanxp_gfx_context* gfx, const struct target* target)
{
    uint32_t text_colour = gfx_rgb(213, 244, 223);
    int help_y = target->origin_y + target->height + 10;

    gfx_clear(gfx->pixels, &gfx->info, gfx_rgb(16, 24, 36));
    if (!target->chrome) {
        return;
    }
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, 6, "SavanXP gears -- SxGL rendering test", text_colour);
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, help_y, "ARROWS rotate the view", text_colour);
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, help_y + 20, "S pauses the rotation", text_colour);
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, help_y + 40, "ESC returns to shell", text_colour);
}

static void wrap_surface(const struct savanxp_gfx_context* gfx, struct sx_bitmap* bitmap)
{
    sx_bitmap_wrap(bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
}

/* SxGL's debug layer goes to the serial line, the channel that does not
 * disturb what is being measured (docs/desktop/GRAPHICS_PERF.md). */
static void report_gl_error(const char* message, void* user)
{
    (void)user;
    eprintf("gears: %s\n", message);
}

static int fail(struct savanxp_gfx_context* gfx, struct sxgl_context* gl, const char* what)
{
    sxgl_context_destroy(gl);
    gfx_release(gfx);
    gfx_close(gfx);
    eprintf("gears: %s\n", what);
    return 1;
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct target target;
    struct sx_bitmap bitmap;
    struct sxgl_context* gl;
    float angle = 0.0f;
    float view_rot_x = 20.0f;
    float view_rot_y = 30.0f;
    /* It starts spinning: a still gears demo says nothing. S pauses it, the
     * key `tools/shoot.sh` drives Gfx Demo with. */
    int spinning = 1;

    if (gfx_open(&gfx) < 0) {
        puts_fd(2, "gears: open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0) {
        puts_fd(2, "gears: acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    wrap_surface(&gfx, &bitmap);
    gl = sxgl_context_create(&bitmap);
    if (gl == 0) {
        return fail(&gfx, gl, "no GL context");
    }
    sxgl_make_current(gl);
    sxgl_set_debug_sink(gl, report_gl_error, 0);
    setup_gl();
    build_scene();

    target = make_target(&gfx);
    apply_target(&gfx, &target);
    draw_chrome(&gfx, &target);
    /* The chrome is presented whole once; from then on each frame presents
     * only what SxGL reports as drawn. */
    if (gfx_present(&gfx, gfx.pixels) < 0) {
        return fail(&gfx, gl, "present failed");
    }

    for (;;) {
        int redraw = spinning;
        struct sx_rect damage;

        while (gfx_poll_event(&gfx, &event) > 0) {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED) {
                (void)gfx_apply_resize_event(&gfx, &event);
                wrap_surface(&gfx, &bitmap);
                if (sxgl_context_resize(gl, &bitmap) != 0) {
                    return fail(&gfx, gl, "resize failed");
                }
                target = make_target(&gfx);
                apply_target(&gfx, &target);
                draw_chrome(&gfx, &target);
                if (gfx_present(&gfx, gfx.pixels) < 0) {
                    return fail(&gfx, gl, "present failed");
                }
                redraw = 1;
                continue;
            }
            if (event.type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
                continue;
            }
            if (event.key == SAVANXP_KEY_ESC) {
                sxgl_context_destroy(gl);
                gfx_release(&gfx);
                gfx_close(&gfx);
                return 0;
            }
            if (event.ascii == 's' || event.ascii == 'S') {
                spinning = !spinning;
                continue;
            }
            if (event.key == SAVANXP_KEY_LEFT) {
                view_rot_y -= 5.0f;
                redraw = 1;
            } else if (event.key == SAVANXP_KEY_RIGHT) {
                view_rot_y += 5.0f;
                redraw = 1;
            } else if (event.key == SAVANXP_KEY_UP) {
                view_rot_x -= 5.0f;
                redraw = 1;
            } else if (event.key == SAVANXP_KEY_DOWN) {
                view_rot_x += 5.0f;
                redraw = 1;
            }
        }

        if (!redraw) {
            sleep_ms(8);
            continue;
        }

        if (spinning) {
            angle += 2.0f;
            if (angle >= 360.0f) {
                angle -= 360.0f;
            }
        }

        draw_scene(&target, angle, view_rot_x, view_rot_y);
        sxgl_swap_buffers(gl, &damage);
        if (damage.width <= 0 || damage.height <= 0) {
            continue;
        }
        if (gfx_present_region(
                &gfx,
                gfx.pixels,
                (uint32_t)damage.x,
                (uint32_t)damage.y,
                (uint32_t)damage.width,
                (uint32_t)damage.height) < 0) {
            break;
        }
    }

    return fail(&gfx, gl, "present failed");
}
