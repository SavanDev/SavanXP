/* Demo de engranajes: el consumidor del batch 0 de docs/SXGL_ROADMAP.md.
 *
 * Rasteriza a mano, y ese es exactamente el punto. El archivo esta partido en
 * tres bloques, y la division no es estetica:
 *
 *   1. MATEMATICA    -- lo que en SxGL va a ser el stack de matrices.
 *   2. RASTERIZADOR  -- lo que el batch 2 reemplaza: triangulos con z-buffer.
 *   3. ESCENA        -- lo unico que sobrevive cuando SxGL exista. Ahi este
 *                       bloque pasa a ser el cliente de GL y los otros dos se
 *                       borran.
 *
 * Lo que el bloque 3 le pide a los bloques 1 y 2 ES la especificacion del
 * subset: matrices de modelo/vista/proyeccion, viewport, depth test, backface
 * culling y shading plano con una luz direccional. Nada mas que eso, a
 * proposito: el roadmap dice que el port define el subset, asi que todo lo que
 * se agregue aca sin necesitarlo es API que alguien va a tener que escribir.
 *
 * La escena es la de glxgears -- tres engranajes engranados -- porque es el
 * programa con el que se mide un GL desde 1996 y porque su geometria es
 * exigente en la forma correcta: muchos triangulos chicos, mucha silueta y
 * caras traseras que TIENEN que desaparecer para que se vea bien.
 */

#include "libc.h"

#include <math.h>

#define GEARS_PI 3.14159265358979323846f

/* El rasterizador es fill-rate bound (docs/SXGL_ROADMAP.md), asi que la escena
 * 3D vive en un viewport acotado y centrado en vez de ocupar la pantalla. Es la
 * decision honesta para software, y de paso ejercita el caso "glViewport dentro
 * de una ventana" que el roadmap da por gratis en el seam. */
#define VIEW_MAX_WIDTH 800
#define VIEW_MAX_HEIGHT 600

#define GEAR_MAX_TRIANGLES 420
#define GEAR_COUNT 3

/* ------------------------------------------------------------------------
 * 1. MATEMATICA -- el stack de matrices que SxGL va a tener
 * ------------------------------------------------------------------------ */

struct vec3 {
    float x;
    float y;
    float z;
};

/* Column-major, como GL: m[columna * 4 + fila]. Se respeta la convencion
 * aunque aca no haya nadie mas que la lea, porque el dia que esto sea
 * glLoadMatrixf el layout tiene que coincidir sin transponer. */
struct mat4 {
    float m[16];
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

static float vec3_dot(struct vec3 left, struct vec3 right)
{
    return (left.x * right.x) + (left.y * right.y) + (left.z * right.z);
}

static struct vec3 vec3_normalize(struct vec3 value)
{
    float length = sqrtf(vec3_dot(value, value));
    if (length <= 1e-6f) {
        return vec3_make(0.0f, 0.0f, 1.0f);
    }
    return vec3_make(value.x / length, value.y / length, value.z / length);
}

static struct mat4 mat4_identity(void)
{
    struct mat4 result;
    int index;
    for (index = 0; index < 16; ++index) {
        result.m[index] = 0.0f;
    }
    result.m[0] = 1.0f;
    result.m[5] = 1.0f;
    result.m[10] = 1.0f;
    result.m[15] = 1.0f;
    return result;
}

static struct mat4 mat4_multiply(struct mat4 left, struct mat4 right)
{
    struct mat4 result;
    int column;
    int row;
    int step;

    for (column = 0; column < 4; ++column) {
        for (row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (step = 0; step < 4; ++step) {
                sum += left.m[(step * 4) + row] * right.m[(column * 4) + step];
            }
            result.m[(column * 4) + row] = sum;
        }
    }
    return result;
}

static struct mat4 mat4_translate(float x, float y, float z)
{
    struct mat4 result = mat4_identity();
    result.m[12] = x;
    result.m[13] = y;
    result.m[14] = z;
    return result;
}

static struct mat4 mat4_rotate_x(float radians)
{
    struct mat4 result = mat4_identity();
    float c = cosf(radians);
    float s = sinf(radians);
    result.m[5] = c;
    result.m[6] = s;
    result.m[9] = -s;
    result.m[10] = c;
    return result;
}

static struct mat4 mat4_rotate_y(float radians)
{
    struct mat4 result = mat4_identity();
    float c = cosf(radians);
    float s = sinf(radians);
    result.m[0] = c;
    result.m[2] = -s;
    result.m[8] = s;
    result.m[10] = c;
    return result;
}

static struct mat4 mat4_rotate_z(float radians)
{
    struct mat4 result = mat4_identity();
    float c = cosf(radians);
    float s = sinf(radians);
    result.m[0] = c;
    result.m[1] = s;
    result.m[4] = -s;
    result.m[5] = c;
    return result;
}

/* glFrustum, tal cual. */
static struct mat4 mat4_frustum(float left, float right, float bottom, float top, float near_plane, float far_plane)
{
    struct mat4 result = mat4_identity();
    result.m[0] = (2.0f * near_plane) / (right - left);
    result.m[5] = (2.0f * near_plane) / (top - bottom);
    result.m[8] = (right + left) / (right - left);
    result.m[9] = (top + bottom) / (top - bottom);
    result.m[10] = -(far_plane + near_plane) / (far_plane - near_plane);
    result.m[11] = -1.0f;
    result.m[14] = -(2.0f * far_plane * near_plane) / (far_plane - near_plane);
    result.m[15] = 0.0f;
    return result;
}

static struct vec3 mat4_transform_point(struct mat4 matrix, struct vec3 point)
{
    struct vec3 result;
    result.x = (matrix.m[0] * point.x) + (matrix.m[4] * point.y) + (matrix.m[8] * point.z) + matrix.m[12];
    result.y = (matrix.m[1] * point.x) + (matrix.m[5] * point.y) + (matrix.m[9] * point.z) + matrix.m[13];
    result.z = (matrix.m[2] * point.x) + (matrix.m[6] * point.y) + (matrix.m[10] * point.z) + matrix.m[14];
    return result;
}

/* Una direccion ignora la traslacion. Vale porque las matrices de modelo de
 * esta escena son rotacion + traslacion, sin escala: con escala no uniforme
 * haria falta la inversa transpuesta, que es justo el tipo de cosa que SxGL va
 * a tener que resolver y esta demo no necesita. */
static struct vec3 mat4_transform_direction(struct mat4 matrix, struct vec3 direction)
{
    struct vec3 result;
    result.x = (matrix.m[0] * direction.x) + (matrix.m[4] * direction.y) + (matrix.m[8] * direction.z);
    result.y = (matrix.m[1] * direction.x) + (matrix.m[5] * direction.y) + (matrix.m[9] * direction.z);
    result.z = (matrix.m[2] * direction.x) + (matrix.m[6] * direction.y) + (matrix.m[10] * direction.z);
    return result;
}

/* ------------------------------------------------------------------------
 * 2. RASTERIZADOR -- lo que el batch 2 reemplaza
 * ------------------------------------------------------------------------ */

struct target {
    uint32_t* pixels;
    uint32_t stride_pixels;
    int origin_x;
    int origin_y;
    int width;
    int height;
    /* Hay lugar arriba y abajo del viewport para el titulo y la ayuda. Si no lo
     * hay, el viewport se queda con la ventana entera y no se dibuja texto: es
     * eso o blitear en coordenadas negativas. */
    int chrome;
};

/* Depth buffer propio: es el unico buffer que la plataforma no conoce, tal como
 * dice el seam del roadmap. Estatico y acotado a VIEW_MAX_* para no depender
 * del heap. */
static float g_depth[VIEW_MAX_WIDTH * VIEW_MAX_HEIGHT];

static void depth_clear(const struct target* target)
{
    int count = target->width * target->height;
    int index;
    for (index = 0; index < count; ++index) {
        g_depth[index] = 1.0f;
    }
}

static uint32_t shade(struct vec3 base_colour, float intensity)
{
    float red = base_colour.x * intensity;
    float green = base_colour.y * intensity;
    float blue = base_colour.z * intensity;

    if (red > 1.0f) {
        red = 1.0f;
    }
    if (green > 1.0f) {
        green = 1.0f;
    }
    if (blue > 1.0f) {
        blue = 1.0f;
    }
    return gfx_rgb((uint8_t)(red * 255.0f), (uint8_t)(green * 255.0f), (uint8_t)(blue * 255.0f));
}

/* Rasteriza un triangulo ya proyectado. (x, y) en pixeles dentro del viewport,
 * z en [-1, 1] de NDC -- que interpolado linealmente en pantalla es el valor
 * correcto para el depth test, y es la unica razon por la que no hace falta
 * dividir por w aca.
 *
 * El fill rule es "los tres baricentricos >= 0", que NO es top-left: un pixel
 * exactamente sobre una arista compartida se pinta dos veces. Con el z-buffer
 * eso no deja costura ni agujero, solo overdraw, asi que alcanza para esta
 * demo -- pero es justo la deuda que el batch 2 tiene que pagar bien, y esta
 * anotada en el roadmap como la leccion que el batch 3 de SxGFX aprendio a la
 * mala. */
static void raster_triangle(
    const struct target* target,
    struct vec3 v0,
    struct vec3 v1,
    struct vec3 v2,
    uint32_t colour)
{
    float area = ((v1.x - v0.x) * (v2.y - v0.y)) - ((v2.x - v0.x) * (v1.y - v0.y));
    float inverse_area;
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int y;

    if (area > -1e-6f && area < 1e-6f) {
        return;
    }
    inverse_area = 1.0f / area;

    min_x = (int)floorf(fminf(v0.x, fminf(v1.x, v2.x)));
    max_x = (int)ceilf(fmaxf(v0.x, fmaxf(v1.x, v2.x)));
    min_y = (int)floorf(fminf(v0.y, fminf(v1.y, v2.y)));
    max_y = (int)ceilf(fmaxf(v0.y, fmaxf(v1.y, v2.y)));

    if (min_x < 0) {
        min_x = 0;
    }
    if (min_y < 0) {
        min_y = 0;
    }
    if (max_x > target->width - 1) {
        max_x = target->width - 1;
    }
    if (max_y > target->height - 1) {
        max_y = target->height - 1;
    }

    for (y = min_y; y <= max_y; ++y) {
        int x;
        for (x = min_x; x <= max_x; ++x) {
            float px = (float)x + 0.5f;
            float py = (float)y + 0.5f;
            float w0 = (((v1.x - px) * (v2.y - py)) - ((v2.x - px) * (v1.y - py))) * inverse_area;
            float w1 = (((v2.x - px) * (v0.y - py)) - ((v0.x - px) * (v2.y - py))) * inverse_area;
            float w2 = 1.0f - w0 - w1;
            float depth;
            int depth_index;

            if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                continue;
            }

            depth = (w0 * v0.z) + (w1 * v1.z) + (w2 * v2.z);
            depth_index = (y * target->width) + x;
            if (depth >= g_depth[depth_index]) {
                continue;
            }
            g_depth[depth_index] = depth;
            target->pixels[(size_t)(target->origin_y + y) * target->stride_pixels + (size_t)(target->origin_x + x)] = colour;
        }
    }
}

/* ------------------------------------------------------------------------
 * 3. ESCENA -- el unico bloque que sobrevive a SxGL
 * ------------------------------------------------------------------------ */

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
    /* La normal sale de la geometria y del orden de los vertices, asi que el
     * winding es la unica fuente de verdad: si un triangulo se carga al reves,
     * se ilumina al reves Y se descarta al reves, de forma consistente. */
    triangle->normal = vec3_normalize(vec3_cross(vec3_sub(b, a), vec3_sub(c, a)));
    gear->triangle_count += 1;
}

static void push_quad(struct gear* gear, struct vec3 a, struct vec3 b, struct vec3 c, struct vec3 d)
{
    push_triangle(gear, a, b, c);
    push_triangle(gear, a, c, d);
}

/* Misma parametrizacion que el gear() de glxgears: radio interno, radio
 * externo, ancho, cantidad de dientes y profundidad del diente. */
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

        /* Cara frontal: el anillo hasta la raiz del diente, mas el diente. */
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

        /* Cara trasera: lo mismo con el winding invertido. */
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

        /* Paredes exteriores, recorriendo el contorno en sentido creciente de
         * angulo: flanco, punta, flanco y arco de raiz. */
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

        /* Barreno interior: la normal apunta hacia el eje, que es lo que se ve
         * desde adentro del agujero. */
        push_quad(gear,
            polar(inner_radius, a0, half_width),
            polar(inner_radius, a4, half_width),
            polar(inner_radius, a4, -half_width),
            polar(inner_radius, a0, -half_width));
    }
}

static void draw_gear(const struct target* target, const struct gear* gear, struct mat4 model_view, struct mat4 projection)
{
    /* Luz direccional en espacio de vista, como la de glxgears. */
    struct vec3 light = vec3_normalize(vec3_make(0.35f, 0.35f, 0.87f));
    int index;

    for (index = 0; index < gear->triangle_count; ++index) {
        const struct triangle* triangle = &gear->triangles[index];
        struct vec3 view_a = mat4_transform_point(model_view, triangle->a);
        struct vec3 view_b = mat4_transform_point(model_view, triangle->b);
        struct vec3 view_c = mat4_transform_point(model_view, triangle->c);
        struct vec3 view_normal = mat4_transform_direction(model_view, triangle->normal);
        struct vec3 screen[3];
        struct vec3 view[3];
        float intensity;
        uint32_t colour;
        int corner;

        /* Backface culling en espacio de vista y no por area en pantalla: el
         * ojo esta en el origen mirando hacia -Z, asi que la cara mira al ojo
         * cuando su normal y el vector al triangulo apuntan en contra. Es la
         * misma normal que usa la luz, con lo cual iluminacion y descarte no
         * se pueden desincronizar. */
        if (vec3_dot(view_normal, view_a) >= 0.0f) {
            continue;
        }

        view[0] = view_a;
        view[1] = view_b;
        view[2] = view_c;

        /* Sin clipping contra el near plane: se descarta el triangulo entero si
         * alguno de sus vertices queda detras. Alcanza porque la camara nunca
         * se acerca, y el clipping de verdad es trabajo del batch 2. */
        for (corner = 0; corner < 3; ++corner) {
            if (view[corner].z > -0.001f) {
                break;
            }
        }
        if (corner != 3) {
            continue;
        }

        for (corner = 0; corner < 3; ++corner) {
            struct vec3 point = view[corner];
            float clip_w = -point.z;
            float clip_x = (projection.m[0] * point.x) + (projection.m[8] * point.z);
            float clip_y = (projection.m[5] * point.y) + (projection.m[9] * point.z);
            float clip_z = (projection.m[10] * point.z) + projection.m[14];

            screen[corner].x = (((clip_x / clip_w) * 0.5f) + 0.5f) * (float)target->width;
            screen[corner].y = ((0.5f - ((clip_y / clip_w) * 0.5f))) * (float)target->height;
            screen[corner].z = clip_z / clip_w;
        }

        intensity = vec3_dot(view_normal, light);
        if (intensity < 0.0f) {
            intensity = 0.0f;
        }
        colour = shade(gear->colour, 0.25f + (0.75f * intensity));
        raster_triangle(target, screen[0], screen[1], screen[2], colour);
    }
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

static void draw_scene(const struct target* target, float angle_degrees, float view_rot_x, float view_rot_y)
{
    /* Encuadre "contain": el lado corto se queda con el campo de vision de
     * referencia y el largo se ensancha. glxgears fija el horizontal y deja que
     * el vertical caiga con el aspecto, que en una ventana ancha corta los
     * engranajes de arriba y de abajo -- que es exactamente lo que pasaba. */
    float aspect = (float)target->width / (float)target->height;
    float half_x = aspect >= 1.0f ? aspect : 1.0f;
    float half_y = aspect >= 1.0f ? 1.0f : (1.0f / aspect);
    struct mat4 projection = mat4_frustum(-half_x, half_x, -half_y, half_y, 5.0f, 60.0f);
    struct mat4 view = mat4_multiply(mat4_translate(0.0f, 0.0f, -44.0f), mat4_multiply(mat4_rotate_x(view_rot_x), mat4_rotate_y(view_rot_y)));
    int index;

    depth_clear(target);

    for (index = 0; index < GEAR_COUNT; ++index) {
        const struct gear* gear = &g_gears[index];
        float spin = ((angle_degrees * gear->speed) + gear->phase_degrees) * (GEARS_PI / 180.0f);
        struct mat4 model = mat4_multiply(mat4_translate(gear->position_x, gear->position_y, 0.0f), mat4_rotate_z(spin));
        draw_gear(target, gear, mat4_multiply(view, model), projection);
    }
}

static void fill_viewport(const struct target* target, uint32_t colour)
{
    int y;
    for (y = 0; y < target->height; ++y) {
        int x;
        uint32_t* row = target->pixels + ((size_t)(target->origin_y + y) * target->stride_pixels) + (size_t)target->origin_x;
        for (x = 0; x < target->width; ++x) {
            row[x] = colour;
        }
    }
}

#define CHROME_TOP 28
#define CHROME_BOTTOM 72

static struct target make_target(struct savanxp_gfx_context* gfx)
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
    target.pixels = gfx->pixels;
    target.stride_pixels = gfx_stride_pixels(&gfx->info);
    target.origin_x = (window_width - target.width) / 2;
    target.origin_y = top + ((available - target.height) / 2);
    return target;
}

static void draw_chrome(struct savanxp_gfx_context* gfx, const struct target* target)
{
    uint32_t text_colour = gfx_rgb(213, 244, 223);
    int help_y = target->origin_y + target->height + 10;

    gfx_clear(gfx->pixels, &gfx->info, gfx_rgb(16, 24, 36));
    if (!target->chrome) {
        return;
    }
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, 6, "SavanXP gears -- 3D rendering test", text_colour);
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, help_y, "ARROWS rotate the view", text_colour);
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, help_y + 20, "S pauses the rotation", text_colour);
    gfx_blit_text(gfx->pixels, &gfx->info, target->origin_x, help_y + 40, "ESC returns to shell", text_colour);
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct target target;
    float angle = 0.0f;
    float view_rot_x = 20.0f * (GEARS_PI / 180.0f);
    float view_rot_y = 30.0f * (GEARS_PI / 180.0f);
    /* Arranca girando: un demo de engranajes quieto no dice nada. S lo pausa,
     * que es la tecla con la que shoot.ps1 ya maneja a Gfx Demo. */
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

    build_scene();
    target = make_target(&gfx);
    draw_chrome(&gfx, &target);
    /* El chrome se presenta entero una sola vez; de ahi en mas cada frame
     * presenta solo el viewport. */
    if (gfx_present(&gfx, gfx.pixels) < 0) {
        gfx_release(&gfx);
        gfx_close(&gfx);
        puts_fd(2, "gears: present failed\n");
        return 1;
    }

    for (;;) {
        int redraw = spinning;

        while (gfx_poll_event(&gfx, &event) > 0) {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED) {
                (void)gfx_apply_resize_event(&gfx, &event);
                target = make_target(&gfx);
                draw_chrome(&gfx, &target);
                if (gfx_present(&gfx, gfx.pixels) < 0) {
                    gfx_release(&gfx);
                    gfx_close(&gfx);
                    puts_fd(2, "gears: present failed\n");
                    return 1;
                }
                redraw = 1;
                continue;
            }
            if (event.type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
                continue;
            }
            if (event.key == SAVANXP_KEY_ESC) {
                gfx_release(&gfx);
                gfx_close(&gfx);
                return 0;
            }
            if (event.ascii == 's' || event.ascii == 'S') {
                spinning = !spinning;
                continue;
            }
            if (event.key == SAVANXP_KEY_LEFT) {
                view_rot_y -= 5.0f * (GEARS_PI / 180.0f);
                redraw = 1;
            } else if (event.key == SAVANXP_KEY_RIGHT) {
                view_rot_y += 5.0f * (GEARS_PI / 180.0f);
                redraw = 1;
            } else if (event.key == SAVANXP_KEY_UP) {
                view_rot_x -= 5.0f * (GEARS_PI / 180.0f);
                redraw = 1;
            } else if (event.key == SAVANXP_KEY_DOWN) {
                view_rot_x += 5.0f * (GEARS_PI / 180.0f);
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

        fill_viewport(&target, gfx_rgb(10, 14, 22));
        draw_scene(&target, angle, view_rot_x, view_rot_y);

        /* Se presenta solo el viewport: el chrome alrededor no cambia. */
        if (gfx_present_region(
                &gfx,
                gfx.pixels,
                (uint32_t)target.origin_x,
                (uint32_t)target.origin_y,
                (uint32_t)target.width,
                (uint32_t)target.height) < 0) {
            break;
        }
    }

    gfx_release(&gfx);
    gfx_close(&gfx);
    puts_fd(2, "gears: present failed\n");
    return 1;
}
