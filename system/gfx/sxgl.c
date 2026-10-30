/* SxGL: the software rasterizer behind sxgfx/sxgl.h.
 *
 * The design is in docs/desktop/SXGL_ROADMAP.md and the subset in
 * docs/desktop/SXGL.md. Four stages, in the order a vertex goes through them:
 *
 *   1. Vertex: modelview, lighting, fog factor, projection -- per vertex, in
 *      float, at the moment glVertex (or an array element) arrives.
 *   2. Assembly and clipping: the primitive becomes triangles, each is clipped
 *      against the six frustum planes in clip space, then mapped to the window.
 *   3. Setup: snap to 1/16 pixel, cull by window-space area, build the plane
 *      equation of every attribute, and pick the span routine.
 *   4. Span: the inner loop, one horizontal run of pixels at a time.
 *
 * Stage 4 never reads GL state. The state that matters to it is frozen into a
 * pipeline object (D3D10's PSO) the first time a triangle is drawn after a
 * change, and that object chooses a span routine compiled for exactly that
 * combination of features. glEnable on the outside, constants in the loop.
 *
 * No system call anywhere: the only things it asks of the runtime are malloc,
 * free, memset, memcpy and libm, so the same file links into POSIX programs,
 * SUN programs and the host test.
 */

#include "sxgfx/sxgl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SXGL_MODELVIEW_DEPTH 32
#define SXGL_PROJECTION_DEPTH 4
#define SXGL_MAX_LIGHTS 4
#define SXGL_MAX_TEXTURE_SIZE 1024
/* A triangle clipped by six planes has at most 3 + 6 vertices. */
#define SXGL_CLIP_MAX_VERTICES 9
/* Perspective-correct texturing divides once per this many pixels and
 * interpolates linearly in between (Quake's 16-pixel spans). */
#define SXGL_PERSPECTIVE_STEP 16
/* Window coordinates are snapped to 1/16 pixel before rasterization. */
#define SXGL_SUBPIXEL_BITS 4
#define SXGL_SUBPIXEL_ONE (1 << SXGL_SUBPIXEL_BITS)
/* Window coordinates beyond this are refused instead of overflowing the edge
 * functions; clipping keeps every legal vertex far inside it. */
#define SXGL_MAX_WINDOW_COORD 16384.0f

/* ------------------------------------------------------------------------
 * State
 * ------------------------------------------------------------------------ */

struct sxgl_matrix {
    float m[16]; /* column-major, as GL: m[column * 4 + row] */
};

struct sxgl_light {
    int enabled;
    float ambient[4];
    float diffuse[4];
    float position[4]; /* eye space, transformed when it was set */
    float spot_direction[3]; /* eye space */
    float spot_exponent;
    float spot_cutoff;
    float spot_cos_cutoff;
    float attenuation[3];
};

struct sxgl_texture {
    uint32_t* texels; /* 0xAARRGGBB, row 0 is t = 0 */
    int width;
    int height;
    GLenum min_filter;
    GLenum mag_filter;
    GLenum wrap_s;
    GLenum wrap_t;
};

struct sxgl_array {
    int enabled;
    GLint size;
    GLenum type;
    GLsizei stride;
    const unsigned char* pointer;
};

/* A vertex after stage 1, in clip space. */
struct sxgl_vertex {
    float clip[4];
    float colour[4]; /* 0..1 */
    float tex[2];
    float fog; /* 1 = no fog, 0 = all fog colour */
};

/* A vertex after projection and the viewport, ready for setup. */
struct sxgl_window_vertex {
    float x; /* colour buffer pixels, y down */
    float y;
    float z; /* window depth, 0..1 */
    float q; /* 1 / w */
    float sq; /* s / w */
    float tq; /* t / w */
    float colour[4]; /* 0..255 */
    float fog; /* 0..1 */
};

/* Features of the span routine: the key of the pipeline object. */
#define SXGL_F_DEPTH_TEST 0x0001u
#define SXGL_F_DEPTH_LEQUAL 0x0002u
#define SXGL_F_DEPTH_WRITE 0x0004u
#define SXGL_F_VARY 0x0008u /* colour varies across the triangle */
#define SXGL_F_TEX 0x0010u
#define SXGL_F_TEX_LINEAR 0x0020u
#define SXGL_F_TEX_REPLACE 0x0040u
#define SXGL_F_ALPHA_TEST 0x0080u
#define SXGL_F_BLEND 0x0100u
#define SXGL_F_BLEND_ADD 0x0200u
#define SXGL_F_FOG 0x0400u
/* Depth, alpha, blend or environment functions that have no specialized
 * routine: the generic one reads them from the pipeline. */
#define SXGL_F_GENERIC 0x8000u

struct sxgl_span {
    uint32_t* colour;
    float* depth;
    int count;
    float z;
    float dz;
    int32_t r, g, b, a; /* 16.16, 0..255 */
    int32_t dr, dg, db, da;
    int32_t fog; /* 16.16, 0..1 */
    int32_t dfog;
    float q, dq;
    float sq, dsq;
    float tq, dtq;
};

struct sxgl_pipeline;
typedef uint64_t (*sxgl_span_fn)(const struct sxgl_pipeline* pipeline, const struct sxgl_span* span);

struct sxgl_pipeline {
    uint32_t features; /* without VARY and TEX_LINEAR, which are per triangle */
    GLenum depth_func;
    GLenum alpha_func;
    int alpha_reference; /* 0..255 */
    int alpha_threshold; /* alpha >= threshold passes, for GREATER/GEQUAL */
    GLenum blend_source;
    GLenum blend_destination;
    GLenum texture_env;
    const struct sxgl_texture* texture;
    int fog_red;
    int fog_green;
    int fog_blue;
    uint32_t alpha_bits; /* what the colour buffer's top byte gets */
    /* [vary][linear], chosen at validation */
    sxgl_span_fn routine[2][2];
};

struct sxgl_context {
    struct sx_bitmap colour;
    uint32_t alpha_bits;
    float* depth;
    int width;
    int height;
    int depth_capacity;

    GLenum error;
    sxgl_debug_sink sink;
    void* sink_user;

    /* viewport and depth range */
    int viewport_x;
    int viewport_y;
    int viewport_width;
    int viewport_height;
    float depth_near;
    float depth_far;
    int scissor_x;
    int scissor_y;
    int scissor_width;
    int scissor_height;

    float clear_colour[4];
    float clear_depth;

    GLenum matrix_mode;
    struct sxgl_matrix modelview[SXGL_MODELVIEW_DEPTH];
    int modelview_top;
    struct sxgl_matrix projection[SXGL_PROJECTION_DEPTH];
    int projection_top;
    float normal_matrix[9];
    int normal_matrix_dirty;

    /* capabilities */
    int depth_test;
    int cull_face;
    int lighting;
    int colour_material;
    int fog;
    int normalize;
    int alpha_test;
    int blend;
    int scissor_test;
    int texture_2d;

    GLenum depth_func;
    int depth_mask;
    GLenum cull_mode;
    GLenum front_face;
    GLenum shade_model;
    GLenum alpha_func;
    float alpha_reference;
    GLenum blend_source;
    GLenum blend_destination;

    struct sxgl_light lights[SXGL_MAX_LIGHTS];
    float light_model_ambient[4];
    float material_ambient[4];
    float material_diffuse[4];
    float material_emission[4];
    GLenum colour_material_mode;

    GLenum fog_mode;
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_colour[4];

    struct sxgl_texture** textures; /* index = name - 1 */
    GLuint texture_capacity;
    GLuint bound_texture;
    GLenum texture_env;

    /* current attributes */
    float current_colour[4];
    float current_normal[3];
    float current_tex[2];

    /* glBegin/glEnd */
    int inside_begin;
    GLenum primitive;
    int primitive_count;
    struct sxgl_vertex primitive_vertices[4];

    struct sxgl_array vertex_array;
    struct sxgl_array colour_array;
    struct sxgl_array normal_array;
    struct sxgl_array tex_array;

    struct sxgl_pipeline pipeline;
    int pipeline_dirty;

    /* bounding box of what was written since the last swap */
    int damage_x0;
    int damage_y0;
    int damage_x1;
    int damage_y1;

    struct sxgl_stats stats;
};

static struct sxgl_context* g_sxgl_current;

/* ------------------------------------------------------------------------
 * Errors and the debug layer
 * ------------------------------------------------------------------------ */

static const char* sxgl_error_name(GLenum error)
{
    switch (error) {
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_STACK_OVERFLOW:
        return "GL_STACK_OVERFLOW";
    case GL_STACK_UNDERFLOW:
        return "GL_STACK_UNDERFLOW";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    default:
        return "GL_NO_ERROR";
    }
}

static size_t sxgl_append(char* out, size_t used, size_t capacity, const char* text)
{
    while (text != 0 && *text != '\0' && used + 1 < capacity) {
        out[used++] = *text++;
    }
    out[used] = '\0';
    return used;
}

/* Raises `error` from `call`: sticky for glGetError (the first one wins, as in
 * GL), and reported to the debug sink with the reason, every time. */
static void sxgl_raise(struct sxgl_context* context, GLenum error, const char* call, const char* reason)
{
    char message[160];
    size_t used = 0;

    if (context->error == GL_NO_ERROR) {
        context->error = error;
    }
    if (context->sink == 0) {
        return;
    }
    message[0] = '\0';
    used = sxgl_append(message, used, sizeof(message), call);
    used = sxgl_append(message, used, sizeof(message), ": ");
    used = sxgl_append(message, used, sizeof(message), sxgl_error_name(error));
    if (reason != 0) {
        used = sxgl_append(message, used, sizeof(message), " (");
        used = sxgl_append(message, used, sizeof(message), reason);
        used = sxgl_append(message, used, sizeof(message), ")");
    }
    context->sink(message, context->sink_user);
}

/* The prologue of every entry point that may not run between glBegin and
 * glEnd. Returns the current context, or 0 when the call must be ignored. */
static struct sxgl_context* sxgl_enter(const char* call)
{
    struct sxgl_context* context = g_sxgl_current;

    if (context == 0) {
        return 0;
    }
    if (context->inside_begin) {
        sxgl_raise(context, GL_INVALID_OPERATION, call, "inside glBegin/glEnd");
        return 0;
    }
    return context;
}

/* ------------------------------------------------------------------------
 * Matrices
 * ------------------------------------------------------------------------ */

static void sxgl_matrix_identity(struct sxgl_matrix* out)
{
    memset(out, 0, sizeof(*out));
    out->m[0] = 1.0f;
    out->m[5] = 1.0f;
    out->m[10] = 1.0f;
    out->m[15] = 1.0f;
}

/* out = left * right; out may alias neither. */
static void sxgl_matrix_multiply(struct sxgl_matrix* out, const struct sxgl_matrix* left, const struct sxgl_matrix* right)
{
    int column;
    int row;

    for (column = 0; column < 4; ++column) {
        for (row = 0; row < 4; ++row) {
            out->m[column * 4 + row] =
                left->m[0 * 4 + row] * right->m[column * 4 + 0] +
                left->m[1 * 4 + row] * right->m[column * 4 + 1] +
                left->m[2 * 4 + row] * right->m[column * 4 + 2] +
                left->m[3 * 4 + row] * right->m[column * 4 + 3];
        }
    }
}

static void sxgl_transform(const struct sxgl_matrix* matrix, const float in[4], float out[4])
{
    const float* m = matrix->m;

    out[0] = m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12] * in[3];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13] * in[3];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14] * in[3];
    out[3] = m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15] * in[3];
}

static struct sxgl_matrix* sxgl_current_matrix(struct sxgl_context* context)
{
    if (context->matrix_mode == GL_PROJECTION) {
        return &context->projection[context->projection_top];
    }
    return &context->modelview[context->modelview_top];
}

static void sxgl_matrix_changed(struct sxgl_context* context)
{
    if (context->matrix_mode == GL_MODELVIEW) {
        context->normal_matrix_dirty = 1;
    }
}

/* The current matrix times `factor`, the way every glRotate/glTranslate does. */
static void sxgl_multiply_current(struct sxgl_context* context, const struct sxgl_matrix* factor)
{
    struct sxgl_matrix* current = sxgl_current_matrix(context);
    struct sxgl_matrix product;

    sxgl_matrix_multiply(&product, current, factor);
    *current = product;
    sxgl_matrix_changed(context);
}

/* The inverse transpose of the modelview's upper 3x3, which carries normals
 * correctly under non-uniform scale. */
static void sxgl_update_normal_matrix(struct sxgl_context* context)
{
    const float* m = context->modelview[context->modelview_top].m;
    float a = m[0], b = m[4], c = m[8];
    float d = m[1], e = m[5], f = m[9];
    float g = m[2], h = m[6], i = m[10];
    float cofactor[9];
    float determinant;
    float inverse;
    int index;

    cofactor[0] = e * i - f * h;
    cofactor[1] = -(d * i - f * g);
    cofactor[2] = d * h - e * g;
    cofactor[3] = -(b * i - c * h);
    cofactor[4] = a * i - c * g;
    cofactor[5] = -(a * h - b * g);
    cofactor[6] = b * f - c * e;
    cofactor[7] = -(a * f - c * d);
    cofactor[8] = a * e - b * d;
    determinant = a * cofactor[0] + b * cofactor[1] + c * cofactor[2];
    inverse = fabsf(determinant) > 1e-12f ? 1.0f / determinant : 0.0f;
    /* The cofactor matrix over the determinant is the inverse transpose:
     * row r of cofactor[] is what multiplies the normal into component r. */
    for (index = 0; index < 9; ++index) {
        context->normal_matrix[index] = cofactor[index] * inverse;
    }
    context->normal_matrix_dirty = 0;
}

/* ------------------------------------------------------------------------
 * Context
 * ------------------------------------------------------------------------ */

static int sxgl_bitmap_usable(const struct sx_bitmap* colour)
{
    return colour != 0 && colour->pixels != 0 && colour->info.width > 0 && colour->info.height > 0 &&
        colour->info.width <= (uint32_t)SXGL_MAX_WINDOW_COORD &&
        colour->info.height <= (uint32_t)SXGL_MAX_WINDOW_COORD &&
        colour->info.pitch >= colour->info.width * 4u &&
        (colour->format == SX_PIXEL_FORMAT_BGRX8888 || colour->format == SX_PIXEL_FORMAT_BGRA8888);
}

static void sxgl_reset_rectangles(struct sxgl_context* context)
{
    context->viewport_x = 0;
    context->viewport_y = 0;
    context->viewport_width = context->width;
    context->viewport_height = context->height;
    context->scissor_x = 0;
    context->scissor_y = 0;
    context->scissor_width = context->width;
    context->scissor_height = context->height;
}

static int sxgl_attach(struct sxgl_context* context, const struct sx_bitmap* colour)
{
    int width = (int)colour->info.width;
    int height = (int)colour->info.height;
    int needed = width * height;
    int index;

    if (needed > context->depth_capacity) {
        float* depth = (float*)malloc((size_t)needed * sizeof(float));
        if (depth == 0) {
            return -1;
        }
        free(context->depth);
        context->depth = depth;
        context->depth_capacity = needed;
    }
    context->colour = *colour;
    context->alpha_bits = colour->format == SX_PIXEL_FORMAT_BGRA8888 ? 0xff000000u : 0u;
    context->width = width;
    context->height = height;
    for (index = 0; index < needed; ++index) {
        context->depth[index] = 1.0f;
    }
    sxgl_reset_rectangles(context);
    context->pipeline_dirty = 1;
    return 0;
}

static void sxgl_set4(float out[4], float a, float b, float c, float d)
{
    out[0] = a;
    out[1] = b;
    out[2] = c;
    out[3] = d;
}

struct sxgl_context* sxgl_context_create(const struct sx_bitmap* colour)
{
    struct sxgl_context* context;
    int index;

    if (!sxgl_bitmap_usable(colour)) {
        return 0;
    }
    context = (struct sxgl_context*)malloc(sizeof(*context));
    if (context == 0) {
        return 0;
    }
    memset(context, 0, sizeof(*context));
    if (sxgl_attach(context, colour) != 0) {
        free(context);
        return 0;
    }

    context->depth_near = 0.0f;
    context->depth_far = 1.0f;
    context->clear_depth = 1.0f;
    context->matrix_mode = GL_MODELVIEW;
    sxgl_matrix_identity(&context->modelview[0]);
    sxgl_matrix_identity(&context->projection[0]);
    context->normal_matrix_dirty = 1;

    context->depth_func = GL_LESS;
    context->depth_mask = 1;
    context->cull_mode = GL_BACK;
    context->front_face = GL_CCW;
    context->shade_model = GL_SMOOTH;
    context->alpha_func = GL_ALWAYS;
    context->blend_source = GL_ONE;
    context->blend_destination = GL_ZERO;

    for (index = 0; index < SXGL_MAX_LIGHTS; ++index) {
        struct sxgl_light* light = &context->lights[index];
        sxgl_set4(light->ambient, 0.0f, 0.0f, 0.0f, 1.0f);
        if (index == 0) {
            sxgl_set4(light->diffuse, 1.0f, 1.0f, 1.0f, 1.0f);
        } else {
            sxgl_set4(light->diffuse, 0.0f, 0.0f, 0.0f, 1.0f);
        }
        sxgl_set4(light->position, 0.0f, 0.0f, 1.0f, 0.0f);
        light->spot_direction[0] = 0.0f;
        light->spot_direction[1] = 0.0f;
        light->spot_direction[2] = -1.0f;
        light->spot_cutoff = 180.0f;
        light->spot_cos_cutoff = -1.0f;
        light->attenuation[0] = 1.0f;
    }
    sxgl_set4(context->light_model_ambient, 0.2f, 0.2f, 0.2f, 1.0f);
    sxgl_set4(context->material_ambient, 0.2f, 0.2f, 0.2f, 1.0f);
    sxgl_set4(context->material_diffuse, 0.8f, 0.8f, 0.8f, 1.0f);
    sxgl_set4(context->material_emission, 0.0f, 0.0f, 0.0f, 1.0f);
    context->colour_material_mode = GL_AMBIENT_AND_DIFFUSE;

    context->fog_mode = GL_EXP;
    context->fog_density = 1.0f;
    context->fog_start = 0.0f;
    context->fog_end = 1.0f;

    context->texture_env = GL_MODULATE;
    sxgl_set4(context->current_colour, 1.0f, 1.0f, 1.0f, 1.0f);
    context->current_normal[2] = 1.0f;

    context->damage_x0 = context->width;
    context->damage_y0 = context->height;
    context->pipeline_dirty = 1;
    return context;
}

int sxgl_context_resize(struct sxgl_context* context, const struct sx_bitmap* colour)
{
    if (context == 0 || !sxgl_bitmap_usable(colour) || context->inside_begin) {
        return -1;
    }
    if (sxgl_attach(context, colour) != 0) {
        return -1;
    }
    context->damage_x0 = 0;
    context->damage_y0 = 0;
    context->damage_x1 = 0;
    context->damage_y1 = 0;
    return 0;
}

void sxgl_context_destroy(struct sxgl_context* context)
{
    GLuint index;

    if (context == 0) {
        return;
    }
    if (g_sxgl_current == context) {
        g_sxgl_current = 0;
    }
    for (index = 0; index < context->texture_capacity; ++index) {
        if (context->textures[index] != 0) {
            free(context->textures[index]->texels);
            free(context->textures[index]);
        }
    }
    free(context->textures);
    free(context->depth);
    free(context);
}

void sxgl_make_current(struct sxgl_context* context)
{
    g_sxgl_current = context;
}

struct sxgl_context* sxgl_get_current(void)
{
    return g_sxgl_current;
}

static void sxgl_damage(struct sxgl_context* context, int x0, int y0, int x1, int y1)
{
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (context->damage_x1 <= context->damage_x0 || context->damage_y1 <= context->damage_y0) {
        context->damage_x0 = x0;
        context->damage_y0 = y0;
        context->damage_x1 = x1;
        context->damage_y1 = y1;
        return;
    }
    if (x0 < context->damage_x0) {
        context->damage_x0 = x0;
    }
    if (y0 < context->damage_y0) {
        context->damage_y0 = y0;
    }
    if (x1 > context->damage_x1) {
        context->damage_x1 = x1;
    }
    if (y1 > context->damage_y1) {
        context->damage_y1 = y1;
    }
}

void sxgl_swap_buffers(struct sxgl_context* context, struct sx_rect* damage)
{
    if (damage != 0) {
        damage->x = 0;
        damage->y = 0;
        damage->width = 0;
        damage->height = 0;
    }
    if (context == 0) {
        return;
    }
    if (damage != 0 && context->damage_x1 > context->damage_x0 && context->damage_y1 > context->damage_y0) {
        damage->x = context->damage_x0;
        damage->y = context->damage_y0;
        damage->width = context->damage_x1 - context->damage_x0;
        damage->height = context->damage_y1 - context->damage_y0;
    }
    context->damage_x0 = 0;
    context->damage_y0 = 0;
    context->damage_x1 = 0;
    context->damage_y1 = 0;
}

void sxgl_set_debug_sink(struct sxgl_context* context, sxgl_debug_sink sink, void* user)
{
    if (context == 0) {
        return;
    }
    context->sink = sink;
    context->sink_user = user;
}

void sxgl_get_stats(const struct sxgl_context* context, struct sxgl_stats* out)
{
    if (out == 0) {
        return;
    }
    if (context == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = context->stats;
}

void sxgl_reset_stats(struct sxgl_context* context)
{
    if (context != 0) {
        memset(&context->stats, 0, sizeof(context->stats));
    }
}

/* ------------------------------------------------------------------------
 * Queries and capabilities
 * ------------------------------------------------------------------------ */

GLenum glGetError(void)
{
    struct sxgl_context* context = g_sxgl_current;
    GLenum error;

    if (context == 0) {
        return GL_NO_ERROR;
    }
    error = context->error;
    context->error = GL_NO_ERROR;
    return error;
}

const GLubyte* glGetString(GLenum name)
{
    struct sxgl_context* context = sxgl_enter("glGetString");

    if (context == 0) {
        return 0;
    }
    switch (name) {
    case GL_VENDOR:
        return (const GLubyte*)"SavanXP";
    case GL_RENDERER:
        return (const GLubyte*)"SxGL software rasterizer";
    case GL_VERSION:
        /* Not "1.1": the subset is documented, and a version string that
         * claims the whole of 1.1 would send a port looking for calls that
         * are not here. */
        return (const GLubyte*)"1.1 SxGL subset";
    case GL_EXTENSIONS:
        return (const GLubyte*)"GL_EXT_bgra";
    default:
        sxgl_raise(context, GL_INVALID_ENUM, "glGetString", 0);
        return 0;
    }
}

static int sxgl_get_matrix(struct sxgl_context* context, GLenum name, GLfloat* out)
{
    const struct sxgl_matrix* matrix;

    if (name == GL_MODELVIEW_MATRIX) {
        matrix = &context->modelview[context->modelview_top];
    } else if (name == GL_PROJECTION_MATRIX) {
        matrix = &context->projection[context->projection_top];
    } else {
        return 0;
    }
    memcpy(out, matrix->m, sizeof(matrix->m));
    return 1;
}

void glGetIntegerv(GLenum name, GLint* out)
{
    struct sxgl_context* context = sxgl_enter("glGetIntegerv");

    if (context == 0 || out == 0) {
        return;
    }
    switch (name) {
    case GL_VIEWPORT:
        out[0] = context->viewport_x;
        out[1] = context->viewport_y;
        out[2] = context->viewport_width;
        out[3] = context->viewport_height;
        return;
    case GL_MATRIX_MODE:
        out[0] = (GLint)context->matrix_mode;
        return;
    case GL_MAX_TEXTURE_SIZE:
        out[0] = SXGL_MAX_TEXTURE_SIZE;
        return;
    case GL_MAX_MODELVIEW_STACK_DEPTH:
        out[0] = SXGL_MODELVIEW_DEPTH;
        return;
    case GL_MAX_PROJECTION_STACK_DEPTH:
        out[0] = SXGL_PROJECTION_DEPTH;
        return;
    case GL_MAX_LIGHTS:
        out[0] = SXGL_MAX_LIGHTS;
        return;
    case GL_TEXTURE_BINDING_2D:
        out[0] = (GLint)context->bound_texture;
        return;
    default: {
        GLfloat matrix[16];
        int index;
        if (sxgl_get_matrix(context, name, matrix)) {
            for (index = 0; index < 16; ++index) {
                out[index] = (GLint)matrix[index];
            }
            return;
        }
        sxgl_raise(context, GL_INVALID_ENUM, "glGetIntegerv", 0);
        return;
    }
    }
}

void glGetFloatv(GLenum name, GLfloat* out)
{
    struct sxgl_context* context = sxgl_enter("glGetFloatv");

    if (context == 0 || out == 0) {
        return;
    }
    if (sxgl_get_matrix(context, name, out)) {
        return;
    }
    if (name == GL_VIEWPORT) {
        out[0] = (GLfloat)context->viewport_x;
        out[1] = (GLfloat)context->viewport_y;
        out[2] = (GLfloat)context->viewport_width;
        out[3] = (GLfloat)context->viewport_height;
        return;
    }
    sxgl_raise(context, GL_INVALID_ENUM, "glGetFloatv", 0);
}

void glFlush(void)
{
    /* Every call completes before it returns: there is nothing queued. */
}

void glFinish(void)
{
}

void glHint(GLenum target, GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glHint");

    if (context == 0) {
        return;
    }
    if ((target != GL_PERSPECTIVE_CORRECTION_HINT && target != GL_FOG_HINT) ||
        (mode != GL_DONT_CARE && mode != GL_FASTEST && mode != GL_NICEST)) {
        sxgl_raise(context, GL_INVALID_ENUM, "glHint", 0);
    }
}

static int* sxgl_capability(struct sxgl_context* context, GLenum capability)
{
    switch (capability) {
    case GL_DEPTH_TEST:
        return &context->depth_test;
    case GL_CULL_FACE:
        return &context->cull_face;
    case GL_LIGHTING:
        return &context->lighting;
    case GL_COLOR_MATERIAL:
        return &context->colour_material;
    case GL_FOG:
        return &context->fog;
    case GL_NORMALIZE:
        return &context->normalize;
    case GL_ALPHA_TEST:
        return &context->alpha_test;
    case GL_BLEND:
        return &context->blend;
    case GL_SCISSOR_TEST:
        return &context->scissor_test;
    case GL_TEXTURE_2D:
        return &context->texture_2d;
    default:
        if (capability >= GL_LIGHT0 && capability < GL_LIGHT0 + SXGL_MAX_LIGHTS) {
            return &context->lights[capability - GL_LIGHT0].enabled;
        }
        return 0;
    }
}

static void sxgl_set_capability(GLenum capability, int value, const char* call)
{
    struct sxgl_context* context = sxgl_enter(call);
    int* flag;

    if (context == 0) {
        return;
    }
    flag = sxgl_capability(context, capability);
    if (flag == 0) {
        sxgl_raise(context, GL_INVALID_ENUM, call, "capability not in the subset");
        return;
    }
    *flag = value;
    context->pipeline_dirty = 1;
}

void glEnable(GLenum capability)
{
    sxgl_set_capability(capability, 1, "glEnable");
}

void glDisable(GLenum capability)
{
    sxgl_set_capability(capability, 0, "glDisable");
}

GLboolean glIsEnabled(GLenum capability)
{
    struct sxgl_context* context = sxgl_enter("glIsEnabled");
    int* flag;

    if (context == 0) {
        return GL_FALSE;
    }
    flag = sxgl_capability(context, capability);
    if (flag == 0) {
        sxgl_raise(context, GL_INVALID_ENUM, "glIsEnabled", 0);
        return GL_FALSE;
    }
    return *flag ? GL_TRUE : GL_FALSE;
}

/* ------------------------------------------------------------------------
 * Viewport, scissor, clear
 * ------------------------------------------------------------------------ */

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    struct sxgl_context* context = sxgl_enter("glViewport");

    if (context == 0) {
        return;
    }
    if (width < 0 || height < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glViewport", "negative size");
        return;
    }
    context->viewport_x = x;
    context->viewport_y = y;
    context->viewport_width = width;
    context->viewport_height = height;
}

static float sxgl_clamp01(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

void glDepthRange(GLclampd near_value, GLclampd far_value)
{
    struct sxgl_context* context = sxgl_enter("glDepthRange");

    if (context == 0) {
        return;
    }
    context->depth_near = sxgl_clamp01((float)near_value);
    context->depth_far = sxgl_clamp01((float)far_value);
}

void glScissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    struct sxgl_context* context = sxgl_enter("glScissor");

    if (context == 0) {
        return;
    }
    if (width < 0 || height < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glScissor", "negative size");
        return;
    }
    context->scissor_x = x;
    context->scissor_y = y;
    context->scissor_width = width;
    context->scissor_height = height;
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    struct sxgl_context* context = sxgl_enter("glClearColor");

    if (context == 0) {
        return;
    }
    sxgl_set4(context->clear_colour, sxgl_clamp01(red), sxgl_clamp01(green), sxgl_clamp01(blue), sxgl_clamp01(alpha));
}

void glClearDepth(GLclampd depth)
{
    struct sxgl_context* context = sxgl_enter("glClearDepth");

    if (context == 0) {
        return;
    }
    context->clear_depth = sxgl_clamp01((float)depth);
}

/* The region of the colour buffer writes may touch, in buffer rows (y down):
 * the whole buffer, cut by the scissor when it is on. */
static void sxgl_write_region(const struct sxgl_context* context, int* x0, int* y0, int* x1, int* y1)
{
    *x0 = 0;
    *y0 = 0;
    *x1 = context->width;
    *y1 = context->height;
    if (context->scissor_test) {
        int left = context->scissor_x;
        int right = context->scissor_x + context->scissor_width;
        int top = context->height - (context->scissor_y + context->scissor_height);
        int bottom = context->height - context->scissor_y;
        if (left > *x0) {
            *x0 = left;
        }
        if (right < *x1) {
            *x1 = right;
        }
        if (top > *y0) {
            *y0 = top;
        }
        if (bottom < *y1) {
            *y1 = bottom;
        }
    }
}

static int sxgl_to_byte(float value)
{
    return (int)(sxgl_clamp01(value) * 255.0f + 0.5f);
}

void glClear(GLbitfield mask)
{
    struct sxgl_context* context = sxgl_enter("glClear");
    int x0, y0, x1, y1;
    int x;
    int y;

    if (context == 0) {
        return;
    }
    if ((mask & ~(GLbitfield)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT)) != 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glClear", "only colour and depth buffers exist");
        return;
    }
    sxgl_write_region(context, &x0, &y0, &x1, &y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if ((mask & GL_COLOR_BUFFER_BIT) != 0) {
        uint32_t pixel = context->alpha_bits |
            ((uint32_t)sxgl_to_byte(context->clear_colour[0]) << 16) |
            ((uint32_t)sxgl_to_byte(context->clear_colour[1]) << 8) |
            (uint32_t)sxgl_to_byte(context->clear_colour[2]);
        uint32_t stride = context->colour.info.pitch / 4u;
        for (y = y0; y < y1; ++y) {
            uint32_t* row = context->colour.pixels + (size_t)y * stride;
            for (x = x0; x < x1; ++x) {
                row[x] = pixel;
            }
        }
        sxgl_damage(context, x0, y0, x1, y1);
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) != 0 && context->depth_mask) {
        for (y = y0; y < y1; ++y) {
            float* row = context->depth + (size_t)y * (size_t)context->width;
            for (x = x0; x < x1; ++x) {
                row[x] = context->clear_depth;
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * Matrix entry points
 * ------------------------------------------------------------------------ */

void glMatrixMode(GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glMatrixMode");

    if (context == 0) {
        return;
    }
    if (mode != GL_MODELVIEW && mode != GL_PROJECTION) {
        sxgl_raise(context, GL_INVALID_ENUM, "glMatrixMode", "only GL_MODELVIEW and GL_PROJECTION");
        return;
    }
    context->matrix_mode = mode;
}

void glLoadIdentity(void)
{
    struct sxgl_context* context = sxgl_enter("glLoadIdentity");

    if (context == 0) {
        return;
    }
    sxgl_matrix_identity(sxgl_current_matrix(context));
    sxgl_matrix_changed(context);
}

void glLoadMatrixf(const GLfloat* matrix)
{
    struct sxgl_context* context = sxgl_enter("glLoadMatrixf");

    if (context == 0 || matrix == 0) {
        return;
    }
    memcpy(sxgl_current_matrix(context)->m, matrix, sizeof(float) * 16);
    sxgl_matrix_changed(context);
}

void glMultMatrixf(const GLfloat* matrix)
{
    struct sxgl_context* context = sxgl_enter("glMultMatrixf");
    struct sxgl_matrix factor;

    if (context == 0 || matrix == 0) {
        return;
    }
    memcpy(factor.m, matrix, sizeof(factor.m));
    sxgl_multiply_current(context, &factor);
}

void glPushMatrix(void)
{
    struct sxgl_context* context = sxgl_enter("glPushMatrix");

    if (context == 0) {
        return;
    }
    if (context->matrix_mode == GL_PROJECTION) {
        if (context->projection_top + 1 >= SXGL_PROJECTION_DEPTH) {
            sxgl_raise(context, GL_STACK_OVERFLOW, "glPushMatrix", "projection stack");
            return;
        }
        context->projection[context->projection_top + 1] = context->projection[context->projection_top];
        context->projection_top += 1;
        return;
    }
    if (context->modelview_top + 1 >= SXGL_MODELVIEW_DEPTH) {
        sxgl_raise(context, GL_STACK_OVERFLOW, "glPushMatrix", "modelview stack");
        return;
    }
    context->modelview[context->modelview_top + 1] = context->modelview[context->modelview_top];
    context->modelview_top += 1;
}

void glPopMatrix(void)
{
    struct sxgl_context* context = sxgl_enter("glPopMatrix");

    if (context == 0) {
        return;
    }
    if (context->matrix_mode == GL_PROJECTION) {
        if (context->projection_top == 0) {
            sxgl_raise(context, GL_STACK_UNDERFLOW, "glPopMatrix", "projection stack");
            return;
        }
        context->projection_top -= 1;
        return;
    }
    if (context->modelview_top == 0) {
        sxgl_raise(context, GL_STACK_UNDERFLOW, "glPopMatrix", "modelview stack");
        return;
    }
    context->modelview_top -= 1;
    context->normal_matrix_dirty = 1;
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    struct sxgl_context* context = sxgl_enter("glTranslatef");
    struct sxgl_matrix factor;

    if (context == 0) {
        return;
    }
    sxgl_matrix_identity(&factor);
    factor.m[12] = x;
    factor.m[13] = y;
    factor.m[14] = z;
    sxgl_multiply_current(context, &factor);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    struct sxgl_context* context = sxgl_enter("glScalef");
    struct sxgl_matrix factor;

    if (context == 0) {
        return;
    }
    sxgl_matrix_identity(&factor);
    factor.m[0] = x;
    factor.m[5] = y;
    factor.m[10] = z;
    sxgl_multiply_current(context, &factor);
}

void glRotatef(GLfloat angle_degrees, GLfloat x, GLfloat y, GLfloat z)
{
    struct sxgl_context* context = sxgl_enter("glRotatef");
    struct sxgl_matrix factor;
    float length = sqrtf(x * x + y * y + z * z);
    float radians = angle_degrees * (3.14159265358979323846f / 180.0f);
    float c = cosf(radians);
    float s = sinf(radians);
    float k = 1.0f - c;

    if (context == 0) {
        return;
    }
    if (length <= 0.0f) {
        return;
    }
    x /= length;
    y /= length;
    z /= length;
    sxgl_matrix_identity(&factor);
    factor.m[0] = x * x * k + c;
    factor.m[1] = y * x * k + z * s;
    factor.m[2] = x * z * k - y * s;
    factor.m[4] = x * y * k - z * s;
    factor.m[5] = y * y * k + c;
    factor.m[6] = y * z * k + x * s;
    factor.m[8] = x * z * k + y * s;
    factor.m[9] = y * z * k - x * s;
    factor.m[10] = z * z * k + c;
    sxgl_multiply_current(context, &factor);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value)
{
    struct sxgl_context* context = sxgl_enter("glFrustum");
    struct sxgl_matrix factor;

    if (context == 0) {
        return;
    }
    if (near_value <= 0.0 || far_value <= 0.0 || left == right || bottom == top || near_value == far_value) {
        sxgl_raise(context, GL_INVALID_VALUE, "glFrustum", 0);
        return;
    }
    memset(&factor, 0, sizeof(factor));
    factor.m[0] = (float)((2.0 * near_value) / (right - left));
    factor.m[5] = (float)((2.0 * near_value) / (top - bottom));
    factor.m[8] = (float)((right + left) / (right - left));
    factor.m[9] = (float)((top + bottom) / (top - bottom));
    factor.m[10] = (float)(-(far_value + near_value) / (far_value - near_value));
    factor.m[11] = -1.0f;
    factor.m[14] = (float)(-(2.0 * far_value * near_value) / (far_value - near_value));
    sxgl_multiply_current(context, &factor);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value)
{
    struct sxgl_context* context = sxgl_enter("glOrtho");
    struct sxgl_matrix factor;

    if (context == 0) {
        return;
    }
    if (left == right || bottom == top || near_value == far_value) {
        sxgl_raise(context, GL_INVALID_VALUE, "glOrtho", 0);
        return;
    }
    sxgl_matrix_identity(&factor);
    factor.m[0] = (float)(2.0 / (right - left));
    factor.m[5] = (float)(2.0 / (top - bottom));
    factor.m[10] = (float)(-2.0 / (far_value - near_value));
    factor.m[12] = (float)(-(right + left) / (right - left));
    factor.m[13] = (float)(-(top + bottom) / (top - bottom));
    factor.m[14] = (float)(-(far_value + near_value) / (far_value - near_value));
    sxgl_multiply_current(context, &factor);
}

/* ------------------------------------------------------------------------
 * Per-fragment state
 * ------------------------------------------------------------------------ */

static int sxgl_valid_compare(GLenum function)
{
    return function >= GL_NEVER && function <= GL_ALWAYS;
}

void glDepthFunc(GLenum function)
{
    struct sxgl_context* context = sxgl_enter("glDepthFunc");

    if (context == 0) {
        return;
    }
    if (!sxgl_valid_compare(function)) {
        sxgl_raise(context, GL_INVALID_ENUM, "glDepthFunc", 0);
        return;
    }
    context->depth_func = function;
    context->pipeline_dirty = 1;
}

void glDepthMask(GLboolean flag)
{
    struct sxgl_context* context = sxgl_enter("glDepthMask");

    if (context == 0) {
        return;
    }
    context->depth_mask = flag ? 1 : 0;
    context->pipeline_dirty = 1;
}

void glCullFace(GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glCullFace");

    if (context == 0) {
        return;
    }
    if (mode != GL_FRONT && mode != GL_BACK && mode != GL_FRONT_AND_BACK) {
        sxgl_raise(context, GL_INVALID_ENUM, "glCullFace", 0);
        return;
    }
    context->cull_mode = mode;
}

void glFrontFace(GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glFrontFace");

    if (context == 0) {
        return;
    }
    if (mode != GL_CW && mode != GL_CCW) {
        sxgl_raise(context, GL_INVALID_ENUM, "glFrontFace", 0);
        return;
    }
    context->front_face = mode;
}

void glShadeModel(GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glShadeModel");

    if (context == 0) {
        return;
    }
    if (mode != GL_FLAT && mode != GL_SMOOTH) {
        sxgl_raise(context, GL_INVALID_ENUM, "glShadeModel", 0);
        return;
    }
    context->shade_model = mode;
}

void glAlphaFunc(GLenum function, GLclampf reference)
{
    struct sxgl_context* context = sxgl_enter("glAlphaFunc");

    if (context == 0) {
        return;
    }
    if (!sxgl_valid_compare(function)) {
        sxgl_raise(context, GL_INVALID_ENUM, "glAlphaFunc", 0);
        return;
    }
    context->alpha_func = function;
    context->alpha_reference = sxgl_clamp01(reference);
    context->pipeline_dirty = 1;
}

static int sxgl_valid_blend_factor(GLenum factor)
{
    return factor == GL_ZERO || factor == GL_ONE || (factor >= GL_SRC_COLOR && factor <= GL_ONE_MINUS_DST_COLOR);
}

void glBlendFunc(GLenum source_factor, GLenum destination_factor)
{
    struct sxgl_context* context = sxgl_enter("glBlendFunc");

    if (context == 0) {
        return;
    }
    if (!sxgl_valid_blend_factor(source_factor) || !sxgl_valid_blend_factor(destination_factor)) {
        sxgl_raise(context, GL_INVALID_ENUM, "glBlendFunc", 0);
        return;
    }
    context->blend_source = source_factor;
    context->blend_destination = destination_factor;
    context->pipeline_dirty = 1;
}

/* ------------------------------------------------------------------------
 * Lighting and fog state
 * ------------------------------------------------------------------------ */

static struct sxgl_light* sxgl_light_for(struct sxgl_context* context, GLenum light, const char* call)
{
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + SXGL_MAX_LIGHTS) {
        sxgl_raise(context, GL_INVALID_ENUM, call, "no such light");
        return 0;
    }
    return &context->lights[light - GL_LIGHT0];
}

static void sxgl_light_scalar(struct sxgl_context* context, struct sxgl_light* target, GLenum name, float value,
    const char* call)
{
    switch (name) {
    case GL_SPOT_EXPONENT:
        if (value < 0.0f || value > 128.0f) {
            sxgl_raise(context, GL_INVALID_VALUE, call, "spot exponent outside [0, 128]");
            return;
        }
        target->spot_exponent = value;
        return;
    case GL_SPOT_CUTOFF:
        if ((value < 0.0f || value > 90.0f) && value != 180.0f) {
            sxgl_raise(context, GL_INVALID_VALUE, call, "spot cutoff outside [0, 90] and not 180");
            return;
        }
        target->spot_cutoff = value;
        target->spot_cos_cutoff = value == 180.0f ? -1.0f : cosf(value * (3.14159265358979323846f / 180.0f));
        return;
    case GL_CONSTANT_ATTENUATION:
    case GL_LINEAR_ATTENUATION:
    case GL_QUADRATIC_ATTENUATION:
        if (value < 0.0f) {
            sxgl_raise(context, GL_INVALID_VALUE, call, "negative attenuation");
            return;
        }
        target->attenuation[name - GL_CONSTANT_ATTENUATION] = value;
        return;
    default:
        sxgl_raise(context, GL_INVALID_ENUM, call, "parameter not in the subset");
        return;
    }
}

void glLightf(GLenum light, GLenum name, GLfloat value)
{
    struct sxgl_context* context = sxgl_enter("glLightf");
    struct sxgl_light* target;

    if (context == 0) {
        return;
    }
    target = sxgl_light_for(context, light, "glLightf");
    if (target != 0) {
        sxgl_light_scalar(context, target, name, value, "glLightf");
    }
}

void glLightfv(GLenum light, GLenum name, const GLfloat* values)
{
    struct sxgl_context* context = sxgl_enter("glLightfv");
    struct sxgl_light* target;
    const float* m;

    if (context == 0 || values == 0) {
        return;
    }
    target = sxgl_light_for(context, light, "glLightfv");
    if (target == 0) {
        return;
    }
    m = context->modelview[context->modelview_top].m;
    switch (name) {
    case GL_AMBIENT:
        memcpy(target->ambient, values, sizeof(target->ambient));
        return;
    case GL_DIFFUSE:
        memcpy(target->diffuse, values, sizeof(target->diffuse));
        return;
    case GL_POSITION:
        /* GL stores the position in eye space, transformed by the modelview
         * current at the time of the call. */
        sxgl_transform(&context->modelview[context->modelview_top], values, target->position);
        return;
    case GL_SPOT_DIRECTION:
        target->spot_direction[0] = m[0] * values[0] + m[4] * values[1] + m[8] * values[2];
        target->spot_direction[1] = m[1] * values[0] + m[5] * values[1] + m[9] * values[2];
        target->spot_direction[2] = m[2] * values[0] + m[6] * values[1] + m[10] * values[2];
        return;
    default:
        sxgl_light_scalar(context, target, name, values[0], "glLightfv");
        return;
    }
}

void glLightModelfv(GLenum name, const GLfloat* values)
{
    struct sxgl_context* context = sxgl_enter("glLightModelfv");

    if (context == 0 || values == 0) {
        return;
    }
    if (name != GL_LIGHT_MODEL_AMBIENT) {
        sxgl_raise(context, GL_INVALID_ENUM, "glLightModelfv", "only GL_LIGHT_MODEL_AMBIENT");
        return;
    }
    memcpy(context->light_model_ambient, values, sizeof(context->light_model_ambient));
}

void glMaterialfv(GLenum face, GLenum name, const GLfloat* values)
{
    struct sxgl_context* context = g_sxgl_current;

    /* Legal between glBegin and glEnd, like glColor. */
    if (context == 0 || values == 0) {
        return;
    }
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        sxgl_raise(context, GL_INVALID_ENUM, "glMaterialfv", 0);
        return;
    }
    /* One material for both faces: there is no two-sided lighting. A call for
     * GL_BACK alone would change what the front faces show, so it is refused
     * rather than applied. */
    if (face == GL_BACK) {
        sxgl_raise(context, GL_INVALID_ENUM, "glMaterialfv", "no two-sided lighting: GL_BACK alone");
        return;
    }
    switch (name) {
    case GL_AMBIENT:
        memcpy(context->material_ambient, values, sizeof(context->material_ambient));
        return;
    case GL_DIFFUSE:
        memcpy(context->material_diffuse, values, sizeof(context->material_diffuse));
        return;
    case GL_AMBIENT_AND_DIFFUSE:
        memcpy(context->material_ambient, values, sizeof(context->material_ambient));
        memcpy(context->material_diffuse, values, sizeof(context->material_diffuse));
        return;
    case GL_EMISSION:
        memcpy(context->material_emission, values, sizeof(context->material_emission));
        return;
    default:
        sxgl_raise(context, GL_INVALID_ENUM, "glMaterialfv", "parameter not in the subset");
        return;
    }
}

void glColorMaterial(GLenum face, GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glColorMaterial");

    if (context == 0) {
        return;
    }
    if (face != GL_FRONT && face != GL_FRONT_AND_BACK) {
        sxgl_raise(context, GL_INVALID_ENUM, "glColorMaterial", "no two-sided lighting");
        return;
    }
    if (mode != GL_AMBIENT && mode != GL_DIFFUSE && mode != GL_AMBIENT_AND_DIFFUSE && mode != GL_EMISSION) {
        sxgl_raise(context, GL_INVALID_ENUM, "glColorMaterial", 0);
        return;
    }
    context->colour_material_mode = mode;
}

static void sxgl_fog_scalar(struct sxgl_context* context, GLenum name, float value, const char* call)
{
    switch (name) {
    case GL_FOG_MODE: {
        GLenum mode = (GLenum)value;
        if (mode != GL_LINEAR && mode != GL_EXP && mode != GL_EXP2) {
            sxgl_raise(context, GL_INVALID_ENUM, call, "fog mode");
            return;
        }
        context->fog_mode = mode;
        return;
    }
    case GL_FOG_DENSITY:
        if (value < 0.0f) {
            sxgl_raise(context, GL_INVALID_VALUE, call, "negative density");
            return;
        }
        context->fog_density = value;
        return;
    case GL_FOG_START:
        context->fog_start = value;
        return;
    case GL_FOG_END:
        context->fog_end = value;
        return;
    default:
        sxgl_raise(context, GL_INVALID_ENUM, call, "parameter not in the subset");
        return;
    }
}

void glFogf(GLenum name, GLfloat value)
{
    struct sxgl_context* context = sxgl_enter("glFogf");

    if (context != 0) {
        sxgl_fog_scalar(context, name, value, "glFogf");
    }
}

void glFogi(GLenum name, GLint value)
{
    struct sxgl_context* context = sxgl_enter("glFogi");

    if (context != 0) {
        sxgl_fog_scalar(context, name, (float)value, "glFogi");
    }
}

void glFogfv(GLenum name, const GLfloat* values)
{
    struct sxgl_context* context = sxgl_enter("glFogfv");

    if (context == 0 || values == 0) {
        return;
    }
    if (name == GL_FOG_COLOR) {
        sxgl_set4(context->fog_colour, sxgl_clamp01(values[0]), sxgl_clamp01(values[1]), sxgl_clamp01(values[2]),
            sxgl_clamp01(values[3]));
        context->pipeline_dirty = 1;
        return;
    }
    sxgl_fog_scalar(context, name, values[0], "glFogfv");
}

/* ------------------------------------------------------------------------
 * Textures
 * ------------------------------------------------------------------------ */

static struct sxgl_texture* sxgl_texture_object(struct sxgl_context* context, GLuint name)
{
    if (name == 0 || name > context->texture_capacity) {
        return 0;
    }
    return context->textures[name - 1];
}

static int sxgl_texture_slot_free(const struct sxgl_context* context, GLuint name)
{
    return name > context->texture_capacity || context->textures[name - 1] == 0;
}

/* Grows the name table so that `name` has a slot. */
static int sxgl_texture_reserve(struct sxgl_context* context, GLuint name)
{
    GLuint capacity;
    struct sxgl_texture** grown;

    if (name <= context->texture_capacity) {
        return 0;
    }
    capacity = context->texture_capacity == 0 ? 16u : context->texture_capacity;
    while (capacity < name) {
        capacity *= 2u;
    }
    grown = (struct sxgl_texture**)malloc(sizeof(*grown) * capacity);
    if (grown == 0) {
        return -1;
    }
    memset(grown, 0, sizeof(*grown) * capacity);
    if (context->textures != 0) {
        memcpy(grown, context->textures, sizeof(*grown) * context->texture_capacity);
        free(context->textures);
    }
    context->textures = grown;
    context->texture_capacity = capacity;
    return 0;
}

/* Names handed out by glGenTextures are marked used with an empty object, so
 * that a second glGenTextures does not return them again. */
void glGenTextures(GLsizei count, GLuint* names)
{
    struct sxgl_context* context = sxgl_enter("glGenTextures");
    GLuint candidate = 1;
    GLsizei index;

    if (context == 0 || names == 0) {
        return;
    }
    if (count < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glGenTextures", "negative count");
        return;
    }
    for (index = 0; index < count; ++index) {
        struct sxgl_texture* texture;

        while (!sxgl_texture_slot_free(context, candidate)) {
            candidate += 1;
        }
        if (sxgl_texture_reserve(context, candidate) != 0) {
            sxgl_raise(context, GL_OUT_OF_MEMORY, "glGenTextures", 0);
            return;
        }
        texture = (struct sxgl_texture*)malloc(sizeof(*texture));
        if (texture == 0) {
            sxgl_raise(context, GL_OUT_OF_MEMORY, "glGenTextures", 0);
            return;
        }
        memset(texture, 0, sizeof(*texture));
        texture->min_filter = GL_LINEAR;
        texture->mag_filter = GL_LINEAR;
        texture->wrap_s = GL_REPEAT;
        texture->wrap_t = GL_REPEAT;
        context->textures[candidate - 1] = texture;
        names[index] = candidate;
    }
}

void glDeleteTextures(GLsizei count, const GLuint* names)
{
    struct sxgl_context* context = sxgl_enter("glDeleteTextures");
    GLsizei index;

    if (context == 0 || names == 0) {
        return;
    }
    if (count < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glDeleteTextures", "negative count");
        return;
    }
    for (index = 0; index < count; ++index) {
        struct sxgl_texture* texture = sxgl_texture_object(context, names[index]);
        if (texture == 0) {
            continue;
        }
        free(texture->texels);
        free(texture);
        context->textures[names[index] - 1] = 0;
        if (context->bound_texture == names[index]) {
            context->bound_texture = 0;
        }
    }
    context->pipeline_dirty = 1;
}

void glBindTexture(GLenum target, GLuint name)
{
    struct sxgl_context* context = sxgl_enter("glBindTexture");

    if (context == 0) {
        return;
    }
    if (target != GL_TEXTURE_2D) {
        sxgl_raise(context, GL_INVALID_ENUM, "glBindTexture", "only GL_TEXTURE_2D");
        return;
    }
    if (name != 0 && sxgl_texture_object(context, name) == 0) {
        /* Binding an unused name creates the object, as in GL. */
        struct sxgl_texture* texture;
        if (sxgl_texture_reserve(context, name) != 0) {
            sxgl_raise(context, GL_OUT_OF_MEMORY, "glBindTexture", 0);
            return;
        }
        texture = (struct sxgl_texture*)malloc(sizeof(*texture));
        if (texture == 0) {
            sxgl_raise(context, GL_OUT_OF_MEMORY, "glBindTexture", 0);
            return;
        }
        memset(texture, 0, sizeof(*texture));
        texture->min_filter = GL_LINEAR;
        texture->mag_filter = GL_LINEAR;
        texture->wrap_s = GL_REPEAT;
        texture->wrap_t = GL_REPEAT;
        context->textures[name - 1] = texture;
    }
    context->bound_texture = name;
    context->pipeline_dirty = 1;
}

static int sxgl_power_of_two(GLsizei value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

/* One texel of the client's data, as 0xAARRGGBB. */
static uint32_t sxgl_fetch_texel(GLenum format, const unsigned char* source)
{
    switch (format) {
    case GL_RGBA:
        return ((uint32_t)source[3] << 24) | ((uint32_t)source[0] << 16) | ((uint32_t)source[1] << 8) | source[2];
    case GL_RGB:
        return 0xff000000u | ((uint32_t)source[0] << 16) | ((uint32_t)source[1] << 8) | source[2];
    case GL_BGRA:
        return ((uint32_t)source[3] << 24) | ((uint32_t)source[2] << 16) | ((uint32_t)source[1] << 8) | source[0];
    case GL_LUMINANCE:
        return 0xff000000u | ((uint32_t)source[0] << 16) | ((uint32_t)source[0] << 8) | source[0];
    case GL_ALPHA:
    default:
        return ((uint32_t)source[0] << 24) | 0x00ffffffu;
    }
}

static int sxgl_format_bytes(GLenum format)
{
    switch (format) {
    case GL_RGBA:
    case GL_BGRA:
        return 4;
    case GL_RGB:
        return 3;
    case GL_LUMINANCE:
    case GL_ALPHA:
        return 1;
    default:
        return 0;
    }
}

static int sxgl_valid_internal_format(GLint format)
{
    return (format >= 1 && format <= 4) || format == GL_RGB || format == GL_RGBA || format == GL_ALPHA ||
        format == GL_LUMINANCE || format == GL_BGRA;
}

static void sxgl_copy_texels(struct sxgl_texture* texture, int x, int y, int width, int height, GLenum format,
    const unsigned char* pixels)
{
    int bytes = sxgl_format_bytes(format);
    int row;
    int column;

    for (row = 0; row < height; ++row) {
        uint32_t* out = texture->texels + (size_t)(y + row) * (size_t)texture->width + (size_t)x;
        const unsigned char* in = pixels + (size_t)row * (size_t)width * (size_t)bytes;
        for (column = 0; column < width; ++column) {
            out[column] = sxgl_fetch_texel(format, in + (size_t)column * (size_t)bytes);
        }
    }
}

void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const GLvoid* pixels)
{
    struct sxgl_context* context = sxgl_enter("glTexImage2D");
    struct sxgl_texture* texture;
    uint32_t* texels;

    if (context == 0) {
        return;
    }
    if (target != GL_TEXTURE_2D || type != GL_UNSIGNED_BYTE || sxgl_format_bytes(format) == 0 ||
        !sxgl_valid_internal_format(internal_format)) {
        sxgl_raise(context, GL_INVALID_ENUM, "glTexImage2D", "target, format or type not in the subset");
        return;
    }
    if (level != 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glTexImage2D", "no mipmaps: only level 0");
        return;
    }
    if (border != 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glTexImage2D", "no texture borders");
        return;
    }
    if (!sxgl_power_of_two(width) || !sxgl_power_of_two(height) || width > SXGL_MAX_TEXTURE_SIZE ||
        height > SXGL_MAX_TEXTURE_SIZE) {
        sxgl_raise(context, GL_INVALID_VALUE, "glTexImage2D", "size is not a power of two up to 1024");
        return;
    }
    texture = sxgl_texture_object(context, context->bound_texture);
    if (texture == 0) {
        sxgl_raise(context, GL_INVALID_OPERATION, "glTexImage2D", "texture 0 is bound");
        return;
    }
    /* Built aside and swapped in: the sampler never sees a half-made image
     * (the immutable resource of D3D, behind GL's mutable call). */
    texels = (uint32_t*)malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    if (texels == 0) {
        sxgl_raise(context, GL_OUT_OF_MEMORY, "glTexImage2D", 0);
        return;
    }
    free(texture->texels);
    texture->texels = texels;
    texture->width = width;
    texture->height = height;
    if (pixels != 0) {
        sxgl_copy_texels(texture, 0, 0, width, height, format, (const unsigned char*)pixels);
    } else {
        memset(texels, 0, (size_t)width * (size_t)height * sizeof(uint32_t));
    }
    context->pipeline_dirty = 1;
}

void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
    GLenum type, const GLvoid* pixels)
{
    struct sxgl_context* context = sxgl_enter("glTexSubImage2D");
    struct sxgl_texture* texture;

    if (context == 0) {
        return;
    }
    if (target != GL_TEXTURE_2D || type != GL_UNSIGNED_BYTE || sxgl_format_bytes(format) == 0) {
        sxgl_raise(context, GL_INVALID_ENUM, "glTexSubImage2D", "target, format or type not in the subset");
        return;
    }
    texture = sxgl_texture_object(context, context->bound_texture);
    if (texture == 0 || texture->texels == 0) {
        sxgl_raise(context, GL_INVALID_OPERATION, "glTexSubImage2D", "no image defined");
        return;
    }
    if (level != 0 || x < 0 || y < 0 || width < 0 || height < 0 || x + width > texture->width ||
        y + height > texture->height) {
        sxgl_raise(context, GL_INVALID_VALUE, "glTexSubImage2D", "outside the image");
        return;
    }
    if (pixels != 0) {
        sxgl_copy_texels(texture, x, y, width, height, format, (const unsigned char*)pixels);
    }
}

void glTexParameteri(GLenum target, GLenum name, GLint value)
{
    struct sxgl_context* context = sxgl_enter("glTexParameteri");
    struct sxgl_texture* texture;

    if (context == 0) {
        return;
    }
    if (target != GL_TEXTURE_2D) {
        sxgl_raise(context, GL_INVALID_ENUM, "glTexParameteri", "only GL_TEXTURE_2D");
        return;
    }
    texture = sxgl_texture_object(context, context->bound_texture);
    if (texture == 0) {
        sxgl_raise(context, GL_INVALID_OPERATION, "glTexParameteri", "texture 0 is bound");
        return;
    }
    switch (name) {
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_MAG_FILTER:
        if (value != GL_NEAREST && value != GL_LINEAR) {
            sxgl_raise(context, GL_INVALID_ENUM, "glTexParameteri", "no mipmaps: GL_NEAREST or GL_LINEAR");
            return;
        }
        if (name == GL_TEXTURE_MIN_FILTER) {
            texture->min_filter = (GLenum)value;
        } else {
            texture->mag_filter = (GLenum)value;
        }
        break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if (value != GL_REPEAT && value != GL_CLAMP && value != GL_CLAMP_TO_EDGE) {
            sxgl_raise(context, GL_INVALID_ENUM, "glTexParameteri", "wrap mode");
            return;
        }
        /* With no border texels, GL_CLAMP and GL_CLAMP_TO_EDGE sample the
         * same: both end at the edge texel. */
        if (name == GL_TEXTURE_WRAP_S) {
            texture->wrap_s = (GLenum)value;
        } else {
            texture->wrap_t = (GLenum)value;
        }
        break;
    default:
        sxgl_raise(context, GL_INVALID_ENUM, "glTexParameteri", "parameter not in the subset");
        return;
    }
    context->pipeline_dirty = 1;
}

void glTexEnvi(GLenum target, GLenum name, GLint value)
{
    struct sxgl_context* context = sxgl_enter("glTexEnvi");

    if (context == 0) {
        return;
    }
    if (target != GL_TEXTURE_ENV || name != GL_TEXTURE_ENV_MODE) {
        sxgl_raise(context, GL_INVALID_ENUM, "glTexEnvi", "only GL_TEXTURE_ENV_MODE");
        return;
    }
    if (value != GL_MODULATE && value != GL_REPLACE && value != GL_DECAL) {
        sxgl_raise(context, GL_INVALID_ENUM, "glTexEnvi", "GL_MODULATE, GL_REPLACE or GL_DECAL");
        return;
    }
    context->texture_env = (GLenum)value;
    context->pipeline_dirty = 1;
}

/* ------------------------------------------------------------------------
 * 4. Span routines
 * ------------------------------------------------------------------------ */

static inline int sxgl_clamp_byte(int32_t value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (int)value;
}

static inline int sxgl_compare(GLenum function, float left, float right)
{
    switch (function) {
    case GL_NEVER:
        return 0;
    case GL_LESS:
        return left < right;
    case GL_EQUAL:
        return left == right;
    case GL_LEQUAL:
        return left <= right;
    case GL_GREATER:
        return left > right;
    case GL_NOTEQUAL:
        return left != right;
    case GL_GEQUAL:
        return left >= right;
    default:
        return 1;
    }
}

static inline int sxgl_wrap(int64_t coordinate, int size, GLenum wrap)
{
    if (wrap == GL_REPEAT) {
        return (int)(coordinate & (int64_t)(size - 1));
    }
    if (coordinate < 0) {
        return 0;
    }
    if (coordinate >= size) {
        return size - 1;
    }
    return (int)coordinate;
}

/* u, v in texels, 16.16. */
static inline uint32_t sxgl_sample_nearest(const struct sxgl_texture* texture, int64_t u, int64_t v)
{
    int x = sxgl_wrap(u >> 16, texture->width, texture->wrap_s);
    int y = sxgl_wrap(v >> 16, texture->height, texture->wrap_t);
    return texture->texels[(size_t)y * (size_t)texture->width + (size_t)x];
}

static inline uint32_t sxgl_lerp_texel(uint32_t left, uint32_t right, uint32_t weight)
{
    /* Two channels per multiply: 0x00ff00ff lanes. */
    uint32_t inverse = 256u - weight;
    uint32_t rb = (((left & 0x00ff00ffu) * inverse + (right & 0x00ff00ffu) * weight) >> 8) & 0x00ff00ffu;
    uint32_t ag = ((((left >> 8) & 0x00ff00ffu) * inverse + ((right >> 8) & 0x00ff00ffu) * weight)) & 0xff00ff00u;
    return rb | ag;
}

static inline uint32_t sxgl_sample_linear(const struct sxgl_texture* texture, int64_t u, int64_t v)
{
    int64_t uu = u - 0x8000;
    int64_t vv = v - 0x8000;
    int x0 = sxgl_wrap(uu >> 16, texture->width, texture->wrap_s);
    int x1 = sxgl_wrap((uu >> 16) + 1, texture->width, texture->wrap_s);
    int y0 = sxgl_wrap(vv >> 16, texture->height, texture->wrap_t);
    int y1 = sxgl_wrap((vv >> 16) + 1, texture->height, texture->wrap_t);
    uint32_t fx = (uint32_t)((uu >> 8) & 0xff);
    uint32_t fy = (uint32_t)((vv >> 8) & 0xff);
    const uint32_t* row0 = texture->texels + (size_t)y0 * (size_t)texture->width;
    const uint32_t* row1 = texture->texels + (size_t)y1 * (size_t)texture->width;
    uint32_t top = sxgl_lerp_texel(row0[x0], row0[x1], fx);
    uint32_t bottom = sxgl_lerp_texel(row1[x0], row1[x1], fx);
    return sxgl_lerp_texel(top, bottom, fy);
}

/* A blend factor for one channel: source (s) and destination (d) colour, both
 * 0..255, and the source alpha; the result is 0..256. */
static inline int sxgl_blend_factor(GLenum factor, int s, int d, int alpha)
{
    switch (factor) {
    case GL_ZERO:
        return 0;
    case GL_ONE:
        return 256;
    case GL_SRC_COLOR:
        return s + (s >> 7);
    case GL_ONE_MINUS_SRC_COLOR:
        return 256 - (s + (s >> 7));
    case GL_SRC_ALPHA:
        return alpha + (alpha >> 7);
    case GL_ONE_MINUS_SRC_ALPHA:
        return 256 - (alpha + (alpha >> 7));
    case GL_DST_ALPHA:
        /* No alpha in the colour buffer: destination alpha is 1. */
        return 256;
    case GL_ONE_MINUS_DST_ALPHA:
        return 0;
    case GL_DST_COLOR:
        return d + (d >> 7);
    case GL_ONE_MINUS_DST_COLOR:
        return 256 - (d + (d >> 7));
    default:
        return 0;
    }
}

/* The one inner loop. Every specialized routine is this function with
 * `features` a compile-time constant, so each `if (features & ...)` folds away;
 * the generic routine is the same code with the features read at run time. */
static inline __attribute__((always_inline)) uint64_t sxgl_span_core(
    const struct sxgl_pipeline* pipeline, const struct sxgl_span* span, const uint32_t features)
{
    uint32_t* colour = span->colour;
    float* depth = span->depth;
    const struct sxgl_texture* texture = pipeline->texture;
    float z = span->z;
    int32_t r = span->r, g = span->g, b = span->b, a = span->a;
    int32_t fog = span->fog;
    float q = span->q, sq = span->sq, tq = span->tq;
    int64_t u = 0, v = 0, du = 0, dv = 0;
    float texture_width = texture != 0 ? (float)texture->width * 65536.0f : 0.0f;
    float texture_height = texture != 0 ? (float)texture->height * 65536.0f : 0.0f;
    uint64_t written = 0;
    int x = 0;
    int count = span->count;

    while (x < count) {
        int segment = count - x < SXGL_PERSPECTIVE_STEP ? count - x : SXGL_PERSPECTIVE_STEP;
        int end = x + segment;

        if (features & SXGL_F_TEX) {
            float inverse = 1.0f / q;
            float next_q = q + span->dq * (float)segment;
            float next_sq = sq + span->dsq * (float)segment;
            float next_tq = tq + span->dtq * (float)segment;
            float next_inverse = 1.0f / next_q;
            float u0 = sq * inverse * texture_width;
            float v0 = tq * inverse * texture_height;
            float u1 = next_sq * next_inverse * texture_width;
            float v1 = next_tq * next_inverse * texture_height;
            u = (int64_t)u0;
            v = (int64_t)v0;
            du = (int64_t)((u1 - u0) / (float)segment);
            dv = (int64_t)((v1 - v0) / (float)segment);
            q = next_q;
            sq = next_sq;
            tq = next_tq;
        }

        for (; x < end; ++x) {
            int cr, cg, cb, ca;

            if (features & SXGL_F_DEPTH_TEST) {
                int pass;
                if (features & SXGL_F_GENERIC) {
                    pass = sxgl_compare(pipeline->depth_func, z, depth[x]);
                } else if (features & SXGL_F_DEPTH_LEQUAL) {
                    pass = z <= depth[x];
                } else {
                    pass = z < depth[x];
                }
                if (!pass) {
                    goto next;
                }
            }

            if (features & SXGL_F_VARY) {
                cr = sxgl_clamp_byte(r >> 16);
                cg = sxgl_clamp_byte(g >> 16);
                cb = sxgl_clamp_byte(b >> 16);
                ca = sxgl_clamp_byte(a >> 16);
            } else {
                cr = r >> 16;
                cg = g >> 16;
                cb = b >> 16;
                ca = a >> 16;
            }

            if (features & SXGL_F_TEX) {
                uint32_t texel = (features & SXGL_F_TEX_LINEAR) ? sxgl_sample_linear(texture, u, v)
                                                                : sxgl_sample_nearest(texture, u, v);
                int tr = (int)((texel >> 16) & 0xffu);
                int tg = (int)((texel >> 8) & 0xffu);
                int tb = (int)(texel & 0xffu);
                int ta = (int)(texel >> 24);
                GLenum env = (features & SXGL_F_GENERIC) ? pipeline->texture_env
                    : (features & SXGL_F_TEX_REPLACE) ? GL_REPLACE
                                                      : GL_MODULATE;
                if (env == GL_REPLACE) {
                    cr = tr;
                    cg = tg;
                    cb = tb;
                    ca = ta;
                } else if (env == GL_DECAL) {
                    cr = cr + (((tr - cr) * (ta + (ta >> 7))) >> 8);
                    cg = cg + (((tg - cg) * (ta + (ta >> 7))) >> 8);
                    cb = cb + (((tb - cb) * (ta + (ta >> 7))) >> 8);
                } else {
                    cr = (cr * (tr + 1)) >> 8;
                    cg = (cg * (tg + 1)) >> 8;
                    cb = (cb * (tb + 1)) >> 8;
                    ca = (ca * (ta + 1)) >> 8;
                }
            }

            if (features & SXGL_F_ALPHA_TEST) {
                int pass;
                if (features & SXGL_F_GENERIC) {
                    pass = sxgl_compare(pipeline->alpha_func, (float)ca, (float)pipeline->alpha_reference);
                } else {
                    pass = ca >= pipeline->alpha_threshold;
                }
                if (!pass) {
                    goto next;
                }
            }

            if (features & SXGL_F_FOG) {
                int32_t f = fog >> 8; /* 0..256 */
                if (f < 0) {
                    f = 0;
                } else if (f > 256) {
                    f = 256;
                }
                cr = (cr * f + pipeline->fog_red * (256 - f)) >> 8;
                cg = (cg * f + pipeline->fog_green * (256 - f)) >> 8;
                cb = (cb * f + pipeline->fog_blue * (256 - f)) >> 8;
            }

            if (features & SXGL_F_BLEND) {
                uint32_t destination = colour[x];
                int dr = (int)((destination >> 16) & 0xffu);
                int dg = (int)((destination >> 8) & 0xffu);
                int db = (int)(destination & 0xffu);
                if (features & SXGL_F_GENERIC) {
                    GLenum sf = pipeline->blend_source;
                    GLenum df = pipeline->blend_destination;
                    cr = (cr * sxgl_blend_factor(sf, cr, dr, ca) + dr * sxgl_blend_factor(df, cr, dr, ca)) >> 8;
                    cg = (cg * sxgl_blend_factor(sf, cg, dg, ca) + dg * sxgl_blend_factor(df, cg, dg, ca)) >> 8;
                    cb = (cb * sxgl_blend_factor(sf, cb, db, ca) + db * sxgl_blend_factor(df, cb, db, ca)) >> 8;
                    if (cr > 255) {
                        cr = 255;
                    }
                    if (cg > 255) {
                        cg = 255;
                    }
                    if (cb > 255) {
                        cb = 255;
                    }
                } else if (features & SXGL_F_BLEND_ADD) {
                    cr = cr + dr > 255 ? 255 : cr + dr;
                    cg = cg + dg > 255 ? 255 : cg + dg;
                    cb = cb + db > 255 ? 255 : cb + db;
                } else {
                    int weight = ca + (ca >> 7);
                    cr = dr + (((cr - dr) * weight) >> 8);
                    cg = dg + (((cg - dg) * weight) >> 8);
                    cb = db + (((cb - db) * weight) >> 8);
                }
            }

            colour[x] = pipeline->alpha_bits | ((uint32_t)cr << 16) | ((uint32_t)cg << 8) | (uint32_t)cb;
            if (features & SXGL_F_DEPTH_WRITE) {
                depth[x] = z;
            }
            written += 1;

        next:
            if (features & (SXGL_F_DEPTH_TEST | SXGL_F_DEPTH_WRITE)) {
                z += span->dz;
            }
            if (features & SXGL_F_VARY) {
                r += span->dr;
                g += span->dg;
                b += span->db;
                a += span->da;
            }
            if (features & SXGL_F_FOG) {
                fog += span->dfog;
            }
            if (features & SXGL_F_TEX) {
                u += du;
                v += dv;
            }
        }
    }
    return written;
}

#define SXGL_SPECIALIZE(name, features)                                                                   \
    static uint64_t name(const struct sxgl_pipeline* pipeline, const struct sxgl_span* span)              \
    {                                                                                                     \
        return sxgl_span_core(pipeline, span, (features));                                                \
    }

#define SXGL_DEPTH (SXGL_F_DEPTH_TEST | SXGL_F_DEPTH_WRITE)
#define SXGL_WORLD (SXGL_DEPTH | SXGL_F_VARY | SXGL_F_TEX | SXGL_F_FOG)

/* The specializations: the combinations the consumers in the tree draw with.
 * Gears is flat and Gouraud over a depth buffer; the forest of the SUN game is
 * textured, fogged and Gouraud-lit, with alpha-tested foliage and an additive
 * glow. Anything else takes the generic routine, correct and slower. */
SXGL_SPECIALIZE(sxgl_span_flat_depth, SXGL_DEPTH)
SXGL_SPECIALIZE(sxgl_span_smooth_depth, SXGL_DEPTH | SXGL_F_VARY)
SXGL_SPECIALIZE(sxgl_span_world_nearest, SXGL_WORLD)
SXGL_SPECIALIZE(sxgl_span_world_linear, SXGL_WORLD | SXGL_F_TEX_LINEAR)
SXGL_SPECIALIZE(sxgl_span_world_cutout, SXGL_WORLD | SXGL_F_ALPHA_TEST)
SXGL_SPECIALIZE(sxgl_span_world_cutout_linear, SXGL_WORLD | SXGL_F_ALPHA_TEST | SXGL_F_TEX_LINEAR)
SXGL_SPECIALIZE(sxgl_span_glow, SXGL_F_DEPTH_TEST | SXGL_F_VARY | SXGL_F_BLEND | SXGL_F_BLEND_ADD | SXGL_F_FOG)
SXGL_SPECIALIZE(sxgl_span_smooth, SXGL_F_VARY)

static uint64_t sxgl_span_generic(const struct sxgl_pipeline* pipeline, const struct sxgl_span* span);

struct sxgl_specialization {
    uint32_t features;
    sxgl_span_fn routine;
};

static const struct sxgl_specialization g_sxgl_specializations[] = {
    {SXGL_DEPTH, sxgl_span_flat_depth},
    {SXGL_DEPTH | SXGL_F_VARY, sxgl_span_smooth_depth},
    {SXGL_WORLD, sxgl_span_world_nearest},
    {SXGL_WORLD | SXGL_F_TEX_LINEAR, sxgl_span_world_linear},
    {SXGL_WORLD | SXGL_F_ALPHA_TEST, sxgl_span_world_cutout},
    {SXGL_WORLD | SXGL_F_ALPHA_TEST | SXGL_F_TEX_LINEAR, sxgl_span_world_cutout_linear},
    {SXGL_F_DEPTH_TEST | SXGL_F_VARY | SXGL_F_BLEND | SXGL_F_BLEND_ADD | SXGL_F_FOG, sxgl_span_glow},
    {SXGL_F_VARY, sxgl_span_smooth},
};

/* The features of the routine being run, for the generic one: it is the only
 * routine that reads them at run time. Set right before each call. */
static uint32_t g_sxgl_generic_features;

static uint64_t sxgl_span_generic(const struct sxgl_pipeline* pipeline, const struct sxgl_span* span)
{
    return sxgl_span_core(pipeline, span, g_sxgl_generic_features | SXGL_F_GENERIC);
}

static sxgl_span_fn sxgl_find_routine(uint32_t features)
{
    size_t index;

    if ((features & SXGL_F_GENERIC) == 0) {
        for (index = 0; index < sizeof(g_sxgl_specializations) / sizeof(g_sxgl_specializations[0]); ++index) {
            if (g_sxgl_specializations[index].features == features) {
                return g_sxgl_specializations[index].routine;
            }
        }
    }
    return sxgl_span_generic;
}

/* Freezes the state the span loop depends on (the PSO). */
static void sxgl_validate(struct sxgl_context* context)
{
    struct sxgl_pipeline* pipeline = &context->pipeline;
    const struct sxgl_texture* texture = sxgl_texture_object(context, context->bound_texture);
    uint32_t features = 0;
    int generic = 0;
    int vary;
    int linear;

    if (!context->pipeline_dirty) {
        return;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->alpha_bits = context->alpha_bits;

    if (context->depth_test) {
        features |= SXGL_F_DEPTH_TEST;
        if (context->depth_mask) {
            features |= SXGL_F_DEPTH_WRITE;
        }
        pipeline->depth_func = context->depth_func;
        if (context->depth_func == GL_LEQUAL) {
            features |= SXGL_F_DEPTH_LEQUAL;
        } else if (context->depth_func != GL_LESS) {
            generic = 1;
        }
    }
    if (context->texture_2d && texture != 0 && texture->texels != 0) {
        features |= SXGL_F_TEX;
        pipeline->texture = texture;
        pipeline->texture_env = context->texture_env;
        if (context->texture_env == GL_REPLACE) {
            features |= SXGL_F_TEX_REPLACE;
        } else if (context->texture_env != GL_MODULATE) {
            generic = 1;
        }
    }
    if (context->alpha_test && context->alpha_func != GL_ALWAYS) {
        int reference = sxgl_to_byte(context->alpha_reference);
        features |= SXGL_F_ALPHA_TEST;
        pipeline->alpha_func = context->alpha_func;
        pipeline->alpha_reference = reference;
        if (context->alpha_func == GL_GEQUAL) {
            pipeline->alpha_threshold = reference;
        } else if (context->alpha_func == GL_GREATER) {
            pipeline->alpha_threshold = reference + 1;
        } else {
            generic = 1;
        }
    }
    if (context->blend && !(context->blend_source == GL_ONE && context->blend_destination == GL_ZERO)) {
        features |= SXGL_F_BLEND;
        pipeline->blend_source = context->blend_source;
        pipeline->blend_destination = context->blend_destination;
        if (context->blend_source == GL_ONE && context->blend_destination == GL_ONE) {
            features |= SXGL_F_BLEND_ADD;
        } else if (!(context->blend_source == GL_SRC_ALPHA && context->blend_destination == GL_ONE_MINUS_SRC_ALPHA)) {
            generic = 1;
        }
    }
    if (context->fog) {
        features |= SXGL_F_FOG;
        pipeline->fog_red = sxgl_to_byte(context->fog_colour[0]);
        pipeline->fog_green = sxgl_to_byte(context->fog_colour[1]);
        pipeline->fog_blue = sxgl_to_byte(context->fog_colour[2]);
    }
    if (generic) {
        features |= SXGL_F_GENERIC;
    }
    pipeline->features = features;
    for (vary = 0; vary < 2; ++vary) {
        for (linear = 0; linear < 2; ++linear) {
            uint32_t key = features;
            if (vary) {
                key |= SXGL_F_VARY;
            }
            if (linear && (features & SXGL_F_TEX) != 0) {
                key |= SXGL_F_TEX_LINEAR;
            }
            pipeline->routine[vary][linear] = sxgl_find_routine(key);
        }
    }
    context->pipeline_dirty = 0;
}

/* ------------------------------------------------------------------------
 * 3. Setup and rasterization
 * ------------------------------------------------------------------------ */

static int64_t sxgl_floor_div(int64_t numerator, int64_t denominator)
{
    int64_t quotient = numerator / denominator;
    if ((numerator % denominator) != 0 && ((numerator < 0) != (denominator < 0))) {
        quotient -= 1;
    }
    return quotient;
}

static int64_t sxgl_ceil_div(int64_t numerator, int64_t denominator)
{
    return -sxgl_floor_div(-numerator, denominator);
}

struct sxgl_edge {
    int64_t x0;
    int64_t y0;
    int64_t dx;
    int64_t dy;
    int64_t bias; /* 0 on a top or left edge, -1 elsewhere: the fill rule */
};

static void sxgl_edge_init(struct sxgl_edge* edge, int64_t ax, int64_t ay, int64_t bx, int64_t by)
{
    edge->x0 = ax;
    edge->y0 = ay;
    edge->dx = bx - ax;
    edge->dy = by - ay;
    /* With the triangle wound so its area is positive in buffer coordinates
     * (y down), a top edge runs right along a horizontal and a left edge runs
     * up. A pixel centre exactly on an edge belongs to the triangle only when
     * the edge is top or left, so two triangles sharing the edge never both
     * draw it and never both skip it. */
    edge->bias = ((edge->dy == 0 && edge->dx > 0) || edge->dy < 0) ? 0 : -1;
}

/* Narrows [*first, *last] (pixel columns) to where the edge function at the
 * centres of row `centre_y` (subpixels) is inside. */
static void sxgl_edge_span(const struct sxgl_edge* edge, int64_t centre_y, int* first, int* last)
{
    const int64_t half = SXGL_SUBPIXEL_ONE / 2;
    /* E(px) = dx * (cy - y0) - dy * (px * ONE + half - x0) = c + a * px */
    int64_t c = edge->dx * (centre_y - edge->y0) - edge->dy * (half - edge->x0) + edge->bias;
    int64_t a = -edge->dy * SXGL_SUBPIXEL_ONE;

    if (a == 0) {
        if (c < 0) {
            *first = 1;
            *last = 0;
        }
        return;
    }
    if (a > 0) {
        int64_t bound = sxgl_ceil_div(-c, a);
        if (bound > *first) {
            *first = bound > (int64_t)*last + 1 ? *last + 1 : (int)bound;
        }
    } else {
        int64_t bound = sxgl_floor_div(c, -a);
        if (bound < *last) {
            *last = bound < (int64_t)*first - 1 ? *first - 1 : (int)bound;
        }
    }
}

struct sxgl_gradient {
    float value; /* at the reference vertex */
    float dx;
    float dy;
};

static struct sxgl_gradient sxgl_plane(float a0, float a1, float a2, float dx1, float dy1, float dx2, float dy2,
    float inverse_determinant)
{
    struct sxgl_gradient gradient;
    float d1 = a1 - a0;
    float d2 = a2 - a0;

    gradient.value = a0;
    gradient.dx = (d1 * dy2 - d2 * dy1) * inverse_determinant;
    gradient.dy = (d2 * dx1 - d1 * dx2) * inverse_determinant;
    return gradient;
}

static float sxgl_at(const struct sxgl_gradient* gradient, float x, float y)
{
    return gradient->value + gradient->dx * x + gradient->dy * y;
}

static int sxgl_colours_differ(const struct sxgl_window_vertex* a, const struct sxgl_window_vertex* b,
    const struct sxgl_window_vertex* c)
{
    int channel;

    for (channel = 0; channel < 4; ++channel) {
        if (a->colour[channel] != b->colour[channel] || a->colour[channel] != c->colour[channel]) {
            return 1;
        }
    }
    return 0;
}

/* Minification or magnification, decided per triangle by comparing the
 * texel area it covers with its pixel area: the subset has no mipmaps and no
 * per-pixel level of detail, so a triangle is minified or magnified as a whole. */
static int sxgl_use_linear(const struct sxgl_texture* texture, const struct sxgl_window_vertex* v[3], float pixel_area)
{
    float s[3];
    float t[3];
    float texel_area;
    int index;

    if (texture == 0) {
        return 0;
    }
    if (texture->min_filter == texture->mag_filter) {
        return texture->mag_filter == GL_LINEAR;
    }
    for (index = 0; index < 3; ++index) {
        s[index] = v[index]->sq / v[index]->q * (float)texture->width;
        t[index] = v[index]->tq / v[index]->q * (float)texture->height;
    }
    texel_area = fabsf((s[1] - s[0]) * (t[2] - t[0]) - (s[2] - s[0]) * (t[1] - t[0]));
    if (texel_area > pixel_area) {
        return texture->min_filter == GL_LINEAR;
    }
    return texture->mag_filter == GL_LINEAR;
}

static int32_t sxgl_fixed(float value)
{
    return (int32_t)(value * 65536.0f);
}

static void sxgl_raster_triangle(struct sxgl_context* context, const struct sxgl_window_vertex* v0,
    const struct sxgl_window_vertex* v1, const struct sxgl_window_vertex* v2)
{
    const struct sxgl_window_vertex* v[3];
    int64_t X[3];
    int64_t Y[3];
    int64_t area;
    struct sxgl_edge edges[3];
    int region_x0, region_y0, region_x1, region_y1;
    int row_first;
    int row_last;
    int row;
    float dx1, dy1, dx2, dy2, determinant, inverse_determinant;
    struct sxgl_gradient z, q, sq, tq, r, g, b, a, fog;
    const struct sxgl_pipeline* pipeline = &context->pipeline;
    int vary;
    int linear;
    sxgl_span_fn routine;
    uint32_t routine_features;
    uint32_t stride = context->colour.info.pitch / 4u;
    int index;
    int touched_x0 = context->width;
    int touched_x1 = 0;
    int touched_y0 = context->height;
    int touched_y1 = 0;

    v[0] = v0;
    v[1] = v1;
    v[2] = v2;
    for (index = 0; index < 3; ++index) {
        if (!(fabsf(v[index]->x) < SXGL_MAX_WINDOW_COORD) || !(fabsf(v[index]->y) < SXGL_MAX_WINDOW_COORD)) {
            return;
        }
        X[index] = (int64_t)lrintf(v[index]->x * (float)SXGL_SUBPIXEL_ONE);
        Y[index] = (int64_t)lrintf(v[index]->y * (float)SXGL_SUBPIXEL_ONE);
    }
    area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        const struct sxgl_window_vertex* swap_vertex = v[1];
        int64_t swap_x = X[1];
        int64_t swap_y = Y[1];
        v[1] = v[2];
        X[1] = X[2];
        Y[1] = Y[2];
        v[2] = swap_vertex;
        X[2] = swap_x;
        Y[2] = swap_y;
    }
    sxgl_edge_init(&edges[0], X[0], Y[0], X[1], Y[1]);
    sxgl_edge_init(&edges[1], X[1], Y[1], X[2], Y[2]);
    sxgl_edge_init(&edges[2], X[2], Y[2], X[0], Y[0]);

    sxgl_write_region(context, &region_x0, &region_y0, &region_x1, &region_y1);
    {
        int64_t min_y = Y[0] < Y[1] ? (Y[0] < Y[2] ? Y[0] : Y[2]) : (Y[1] < Y[2] ? Y[1] : Y[2]);
        int64_t max_y = Y[0] > Y[1] ? (Y[0] > Y[2] ? Y[0] : Y[2]) : (Y[1] > Y[2] ? Y[1] : Y[2]);
        int64_t first = sxgl_ceil_div(min_y - SXGL_SUBPIXEL_ONE / 2, SXGL_SUBPIXEL_ONE);
        int64_t last = sxgl_floor_div(max_y - SXGL_SUBPIXEL_ONE / 2, SXGL_SUBPIXEL_ONE);
        row_first = first < region_y0 ? region_y0 : (int)first;
        row_last = last > region_y1 - 1 ? region_y1 - 1 : (int)last;
    }
    if (row_first > row_last || region_x0 >= region_x1) {
        return;
    }

    /* Plane equations in pixel units, relative to the snapped first vertex. */
    dx1 = (float)(X[1] - X[0]) / (float)SXGL_SUBPIXEL_ONE;
    dy1 = (float)(Y[1] - Y[0]) / (float)SXGL_SUBPIXEL_ONE;
    dx2 = (float)(X[2] - X[0]) / (float)SXGL_SUBPIXEL_ONE;
    dy2 = (float)(Y[2] - Y[0]) / (float)SXGL_SUBPIXEL_ONE;
    determinant = dx1 * dy2 - dx2 * dy1;
    inverse_determinant = 1.0f / determinant;
#define SXGL_PLANE(field) sxgl_plane(v[0]->field, v[1]->field, v[2]->field, dx1, dy1, dx2, dy2, inverse_determinant)
    z = SXGL_PLANE(z);
    q = SXGL_PLANE(q);
    sq = SXGL_PLANE(sq);
    tq = SXGL_PLANE(tq);
    r = SXGL_PLANE(colour[0]);
    g = SXGL_PLANE(colour[1]);
    b = SXGL_PLANE(colour[2]);
    a = SXGL_PLANE(colour[3]);
    fog = SXGL_PLANE(fog);
#undef SXGL_PLANE

    vary = sxgl_colours_differ(v[0], v[1], v[2]);
    linear = (pipeline->features & SXGL_F_TEX) != 0 &&
        sxgl_use_linear(pipeline->texture, v, determinant > 0.0f ? determinant * 0.5f : -determinant * 0.5f);
    routine = pipeline->routine[vary][linear];
    routine_features = pipeline->features | (vary ? SXGL_F_VARY : 0u) | (linear ? SXGL_F_TEX_LINEAR : 0u);
    g_sxgl_generic_features = routine_features & ~SXGL_F_GENERIC;
    if (!vary) {
        /* A constant colour is the vertex colour itself, rounded once. */
        r.dx = r.dy = g.dx = g.dy = b.dx = b.dy = a.dx = a.dy = 0.0f;
    }

    context->stats.triangles_rasterized += 1;

    for (row = row_first; row <= row_last; ++row) {
        int64_t centre_y = (int64_t)row * SXGL_SUBPIXEL_ONE + SXGL_SUBPIXEL_ONE / 2;
        int first = region_x0;
        int last = region_x1 - 1;
        float px;
        float py;
        struct sxgl_span span;

        sxgl_edge_span(&edges[0], centre_y, &first, &last);
        sxgl_edge_span(&edges[1], centre_y, &first, &last);
        sxgl_edge_span(&edges[2], centre_y, &first, &last);
        if (first > last) {
            continue;
        }

        px = (float)first + 0.5f - (float)X[0] / (float)SXGL_SUBPIXEL_ONE;
        py = (float)row + 0.5f - (float)Y[0] / (float)SXGL_SUBPIXEL_ONE;
        span.colour = context->colour.pixels + (size_t)row * stride + (size_t)first;
        span.depth = context->depth + (size_t)row * (size_t)context->width + (size_t)first;
        span.count = last - first + 1;
        span.z = sxgl_at(&z, px, py);
        span.dz = z.dx;
        span.q = sxgl_at(&q, px, py);
        span.dq = q.dx;
        span.sq = sxgl_at(&sq, px, py);
        span.dsq = sq.dx;
        span.tq = sxgl_at(&tq, px, py);
        span.dtq = tq.dx;
        if (vary) {
            span.r = sxgl_fixed(sxgl_at(&r, px, py));
            span.g = sxgl_fixed(sxgl_at(&g, px, py));
            span.b = sxgl_fixed(sxgl_at(&b, px, py));
            span.a = sxgl_fixed(sxgl_at(&a, px, py));
        } else {
            span.r = sxgl_clamp_byte((int32_t)(v[0]->colour[0] + 0.5f)) << 16;
            span.g = sxgl_clamp_byte((int32_t)(v[0]->colour[1] + 0.5f)) << 16;
            span.b = sxgl_clamp_byte((int32_t)(v[0]->colour[2] + 0.5f)) << 16;
            span.a = sxgl_clamp_byte((int32_t)(v[0]->colour[3] + 0.5f)) << 16;
        }
        span.dr = sxgl_fixed(r.dx);
        span.dg = sxgl_fixed(g.dx);
        span.db = sxgl_fixed(b.dx);
        span.da = sxgl_fixed(a.dx);
        span.fog = sxgl_fixed(sxgl_at(&fog, px, py));
        span.dfog = sxgl_fixed(fog.dx);

        context->stats.fragments_written += routine(pipeline, &span);

        if (first < touched_x0) {
            touched_x0 = first;
        }
        if (last + 1 > touched_x1) {
            touched_x1 = last + 1;
        }
        if (row < touched_y0) {
            touched_y0 = row;
        }
        touched_y1 = row + 1;
    }
    (void)routine_features;
    sxgl_damage(context, touched_x0, touched_y0, touched_x1, touched_y1);
}

/* ------------------------------------------------------------------------
 * 2. Clipping and the viewport
 * ------------------------------------------------------------------------ */

static float sxgl_plane_distance(const struct sxgl_vertex* vertex, int plane)
{
    const float* c = vertex->clip;
    switch (plane) {
    case 0:
        return c[3] + c[0];
    case 1:
        return c[3] - c[0];
    case 2:
        return c[3] + c[1];
    case 3:
        return c[3] - c[1];
    case 4:
        return c[3] + c[2];
    default:
        return c[3] - c[2];
    }
}

static void sxgl_lerp_vertex(struct sxgl_vertex* out, const struct sxgl_vertex* a, const struct sxgl_vertex* b, float t)
{
    int index;

    for (index = 0; index < 4; ++index) {
        out->clip[index] = a->clip[index] + (b->clip[index] - a->clip[index]) * t;
        out->colour[index] = a->colour[index] + (b->colour[index] - a->colour[index]) * t;
    }
    out->tex[0] = a->tex[0] + (b->tex[0] - a->tex[0]) * t;
    out->tex[1] = a->tex[1] + (b->tex[1] - a->tex[1]) * t;
    out->fog = a->fog + (b->fog - a->fog) * t;
}

/* Sutherland-Hodgman against one plane. The intersection is always computed
 * from the inside vertex towards the outside one, so an edge shared by two
 * triangles is cut at exactly the same point in both. */
static int sxgl_clip_plane(const struct sxgl_vertex* in, int count, struct sxgl_vertex* out, int plane)
{
    int produced = 0;
    int index;

    for (index = 0; index < count; ++index) {
        const struct sxgl_vertex* current = &in[index];
        const struct sxgl_vertex* next = &in[(index + 1) % count];
        float current_distance = sxgl_plane_distance(current, plane);
        float next_distance = sxgl_plane_distance(next, plane);

        if (current_distance >= 0.0f) {
            out[produced++] = *current;
        }
        if ((current_distance >= 0.0f) != (next_distance >= 0.0f)) {
            if (current_distance >= 0.0f) {
                sxgl_lerp_vertex(&out[produced], current, next, current_distance / (current_distance - next_distance));
            } else {
                sxgl_lerp_vertex(&out[produced], next, current, next_distance / (next_distance - current_distance));
            }
            produced += 1;
        }
    }
    return produced;
}

static void sxgl_to_window(const struct sxgl_context* context, const struct sxgl_vertex* in, struct sxgl_window_vertex* out)
{
    float inverse_w = 1.0f / in->clip[3];
    float ndc_x = in->clip[0] * inverse_w;
    float ndc_y = in->clip[1] * inverse_w;
    float ndc_z = in->clip[2] * inverse_w;
    float window_y = (float)context->viewport_y + (ndc_y + 1.0f) * 0.5f * (float)context->viewport_height;
    int index;

    out->x = (float)context->viewport_x + (ndc_x + 1.0f) * 0.5f * (float)context->viewport_width;
    /* GL's window origin is the bottom-left corner; the buffer's is the top. */
    out->y = (float)context->height - window_y;
    out->z = context->depth_near + (ndc_z + 1.0f) * 0.5f * (context->depth_far - context->depth_near);
    out->q = inverse_w;
    out->sq = in->tex[0] * inverse_w;
    out->tq = in->tex[1] * inverse_w;
    for (index = 0; index < 4; ++index) {
        out->colour[index] = sxgl_clamp01(in->colour[index]) * 255.0f;
    }
    out->fog = in->fog;
}

static void sxgl_draw_triangle(struct sxgl_context* context, const struct sxgl_vertex* a, const struct sxgl_vertex* b,
    const struct sxgl_vertex* c, const struct sxgl_vertex* provoking)
{
    struct sxgl_vertex buffers[2][SXGL_CLIP_MAX_VERTICES];
    struct sxgl_window_vertex window[SXGL_CLIP_MAX_VERTICES];
    int count = 3;
    int current = 0;
    int plane;
    int index;
    unsigned outside_all = 0x3fu;
    unsigned outside_any = 0;
    float area = 0.0f;
    int front;

    context->stats.triangles_submitted += 1;
    buffers[0][0] = *a;
    buffers[0][1] = *b;
    buffers[0][2] = *c;
    if (context->shade_model == GL_FLAT && provoking != 0) {
        for (index = 0; index < 3; ++index) {
            memcpy(buffers[0][index].colour, provoking->colour, sizeof(provoking->colour));
        }
    }

    for (index = 0; index < 3; ++index) {
        unsigned outside = 0;
        for (plane = 0; plane < 6; ++plane) {
            if (sxgl_plane_distance(&buffers[0][index], plane) < 0.0f) {
                outside |= 1u << plane;
            }
        }
        outside_all &= outside;
        outside_any |= outside;
    }
    if (outside_all != 0) {
        return;
    }
    for (plane = 0; plane < 6 && count >= 3; ++plane) {
        if ((outside_any & (1u << plane)) == 0) {
            continue;
        }
        count = sxgl_clip_plane(buffers[current], count, buffers[current ^ 1], plane);
        current ^= 1;
    }
    if (count < 3) {
        return;
    }

    for (index = 0; index < count; ++index) {
        sxgl_to_window(context, &buffers[current][index], &window[index]);
    }
    /* Facing from the window-space area of the whole clipped polygon, in GL's
     * y-up orientation: positive is counter-clockwise. */
    for (index = 0; index < count; ++index) {
        const struct sxgl_window_vertex* p = &window[index];
        const struct sxgl_window_vertex* n = &window[(index + 1) % count];
        area += p->x * (-n->y) - n->x * (-p->y);
    }
    if (area == 0.0f) {
        return;
    }
    front = (area > 0.0f) == (context->front_face == GL_CCW);
    if (context->cull_face) {
        if (context->cull_mode == GL_FRONT_AND_BACK) {
            return;
        }
        if ((context->cull_mode == GL_BACK && !front) || (context->cull_mode == GL_FRONT && front)) {
            return;
        }
    }

    sxgl_validate(context);
    for (index = 1; index + 1 < count; ++index) {
        sxgl_raster_triangle(context, &window[0], &window[index], &window[index + 1]);
    }
}

/* ------------------------------------------------------------------------
 * 1. Vertices
 * ------------------------------------------------------------------------ */

static void sxgl_light_vertex(struct sxgl_context* context, const float eye[4], float out[4])
{
    const float* ambient = context->material_ambient;
    const float* diffuse = context->material_diffuse;
    const float* emission = context->material_emission;
    float normal[3];
    float length;
    int index;
    int light_index;

    if (context->colour_material) {
        switch (context->colour_material_mode) {
        case GL_AMBIENT:
            ambient = context->current_colour;
            break;
        case GL_DIFFUSE:
            diffuse = context->current_colour;
            break;
        case GL_EMISSION:
            emission = context->current_colour;
            break;
        default:
            ambient = context->current_colour;
            diffuse = context->current_colour;
            break;
        }
    }
    if (context->normal_matrix_dirty) {
        sxgl_update_normal_matrix(context);
    }
    for (index = 0; index < 3; ++index) {
        normal[index] = context->normal_matrix[index * 3 + 0] * context->current_normal[0] +
            context->normal_matrix[index * 3 + 1] * context->current_normal[1] +
            context->normal_matrix[index * 3 + 2] * context->current_normal[2];
    }
    /* Always normalized, as if GL_NORMALIZE were on: with the inverse
     * transpose above, a scaled modelview would otherwise shade too dark or
     * too bright, and nobody wants that answer. */
    length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (length > 1e-12f) {
        normal[0] /= length;
        normal[1] /= length;
        normal[2] /= length;
    }

    for (index = 0; index < 3; ++index) {
        out[index] = emission[index] + ambient[index] * context->light_model_ambient[index];
    }
    out[3] = diffuse[3];

    for (light_index = 0; light_index < SXGL_MAX_LIGHTS; ++light_index) {
        const struct sxgl_light* light = &context->lights[light_index];
        float direction[3];
        float attenuation = 1.0f;
        float spot = 1.0f;
        float n_dot_l;

        if (!light->enabled) {
            continue;
        }
        if (light->position[3] == 0.0f) {
            direction[0] = light->position[0];
            direction[1] = light->position[1];
            direction[2] = light->position[2];
            length = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        } else {
            float distance;
            direction[0] = light->position[0] / light->position[3] - eye[0];
            direction[1] = light->position[1] / light->position[3] - eye[1];
            direction[2] = light->position[2] / light->position[3] - eye[2];
            distance = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
            length = distance;
            attenuation = light->attenuation[0] + light->attenuation[1] * distance +
                light->attenuation[2] * distance * distance;
            attenuation = attenuation > 0.0f ? 1.0f / attenuation : 1.0f;
        }
        if (length > 1e-12f) {
            direction[0] /= length;
            direction[1] /= length;
            direction[2] /= length;
        }
        if (light->spot_cutoff != 180.0f && light->position[3] != 0.0f) {
            const float* sd = light->spot_direction;
            float sd_length = sqrtf(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
            float cosine = sd_length > 1e-12f
                ? -(direction[0] * sd[0] + direction[1] * sd[1] + direction[2] * sd[2]) / sd_length
                : 0.0f;
            if (cosine < light->spot_cos_cutoff) {
                spot = 0.0f;
            } else {
                spot = light->spot_exponent == 0.0f ? 1.0f : powf(cosine, light->spot_exponent);
            }
        }
        if (spot == 0.0f) {
            continue;
        }
        n_dot_l = normal[0] * direction[0] + normal[1] * direction[1] + normal[2] * direction[2];
        if (n_dot_l < 0.0f) {
            n_dot_l = 0.0f;
        }
        for (index = 0; index < 3; ++index) {
            out[index] += attenuation * spot *
                (light->ambient[index] * ambient[index] + n_dot_l * light->diffuse[index] * diffuse[index]);
        }
    }
}

static float sxgl_fog_factor(const struct sxgl_context* context, float distance)
{
    float factor;

    switch (context->fog_mode) {
    case GL_LINEAR:
        factor = context->fog_end == context->fog_start
            ? 1.0f
            : (context->fog_end - distance) / (context->fog_end - context->fog_start);
        break;
    case GL_EXP2: {
        float exponent = context->fog_density * distance;
        factor = expf(-(exponent * exponent));
        break;
    }
    default:
        factor = expf(-(context->fog_density * distance));
        break;
    }
    return sxgl_clamp01(factor);
}

static void sxgl_assemble(struct sxgl_context* context, const struct sxgl_vertex* vertex);

static void sxgl_vertex4(struct sxgl_context* context, float x, float y, float z, float w)
{
    struct sxgl_vertex vertex;
    float object[4];
    float eye[4];

    object[0] = x;
    object[1] = y;
    object[2] = z;
    object[3] = w;
    sxgl_transform(&context->modelview[context->modelview_top], object, eye);
    sxgl_transform(&context->projection[context->projection_top], eye, vertex.clip);
    if (context->lighting) {
        sxgl_light_vertex(context, eye, vertex.colour);
    } else {
        memcpy(vertex.colour, context->current_colour, sizeof(vertex.colour));
    }
    vertex.tex[0] = context->current_tex[0];
    vertex.tex[1] = context->current_tex[1];
    /* GL lets the fog distance be the eye-space depth; it is what keeps fog
     * from swimming as the view turns. */
    vertex.fog = context->fog ? sxgl_fog_factor(context, fabsf(eye[2] / (eye[3] != 0.0f ? eye[3] : 1.0f))) : 1.0f;
    sxgl_assemble(context, &vertex);
}

/* ------------------------------------------------------------------------
 * 2. Primitive assembly
 * ------------------------------------------------------------------------ */

static int sxgl_valid_primitive(GLenum mode)
{
    return mode >= GL_TRIANGLES && mode <= GL_POLYGON;
}

static void sxgl_assemble(struct sxgl_context* context, const struct sxgl_vertex* vertex)
{
    struct sxgl_vertex* held = context->primitive_vertices;
    int count = context->primitive_count;

    switch (context->primitive) {
    case GL_TRIANGLES:
        held[count] = *vertex;
        if (count == 2) {
            sxgl_draw_triangle(context, &held[0], &held[1], &held[2], &held[2]);
            context->primitive_count = 0;
            return;
        }
        break;
    case GL_TRIANGLE_STRIP:
        if (count < 2) {
            held[count] = *vertex;
            break;
        }
        /* held[2] tracks the parity: every other triangle is wound the other
         * way round so that the whole strip keeps one facing. */
        if ((count & 1) == 0) {
            sxgl_draw_triangle(context, &held[0], &held[1], vertex, vertex);
        } else {
            sxgl_draw_triangle(context, &held[1], &held[0], vertex, vertex);
        }
        held[0] = held[1];
        held[1] = *vertex;
        break;
    case GL_TRIANGLE_FAN:
    case GL_POLYGON:
        if (count < 2) {
            held[count] = *vertex;
            break;
        }
        /* A polygon's flat colour is its first vertex's, a fan's the last. */
        sxgl_draw_triangle(context, &held[0], &held[1], vertex,
            context->primitive == GL_POLYGON ? &held[0] : vertex);
        held[1] = *vertex;
        break;
    case GL_QUADS:
        held[count] = *vertex;
        if (count == 3) {
            sxgl_draw_triangle(context, &held[0], &held[1], &held[2], &held[3]);
            sxgl_draw_triangle(context, &held[0], &held[2], &held[3], &held[3]);
            context->primitive_count = 0;
            return;
        }
        break;
    case GL_QUAD_STRIP:
        if (count < 2) {
            held[count] = *vertex;
            break;
        }
        if ((count & 1) == 0) {
            held[2] = *vertex;
            break;
        }
        /* held[0], held[1] the previous pair; held[2], vertex the new one. */
        sxgl_draw_triangle(context, &held[0], &held[1], vertex, vertex);
        sxgl_draw_triangle(context, &held[0], vertex, &held[2], vertex);
        held[0] = held[2];
        held[1] = *vertex;
        break;
    default:
        return;
    }
    context->primitive_count = count + 1;
}

void glBegin(GLenum mode)
{
    struct sxgl_context* context = sxgl_enter("glBegin");

    if (context == 0) {
        return;
    }
    if (!sxgl_valid_primitive(mode)) {
        sxgl_raise(context, GL_INVALID_ENUM, "glBegin", "triangle primitives only");
        return;
    }
    context->inside_begin = 1;
    context->primitive = mode;
    context->primitive_count = 0;
}

void glEnd(void)
{
    struct sxgl_context* context = g_sxgl_current;

    if (context == 0) {
        return;
    }
    if (!context->inside_begin) {
        sxgl_raise(context, GL_INVALID_OPERATION, "glEnd", "without glBegin");
        return;
    }
    /* Leftover vertices of an incomplete primitive are dropped, as in GL. */
    context->inside_begin = 0;
    context->primitive_count = 0;
}

static void sxgl_immediate_vertex(float x, float y, float z)
{
    struct sxgl_context* context = g_sxgl_current;

    if (context == 0) {
        return;
    }
    if (!context->inside_begin) {
        /* Undefined in GL; here, a reported no-op. */
        sxgl_raise(context, GL_INVALID_OPERATION, "glVertex", "outside glBegin/glEnd");
        return;
    }
    sxgl_vertex4(context, x, y, z, 1.0f);
}

void glVertex2f(GLfloat x, GLfloat y)
{
    sxgl_immediate_vertex(x, y, 0.0f);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    sxgl_immediate_vertex(x, y, z);
}

void glVertex3fv(const GLfloat* vertex)
{
    if (vertex != 0) {
        sxgl_immediate_vertex(vertex[0], vertex[1], vertex[2]);
    }
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    struct sxgl_context* context = g_sxgl_current;

    if (context == 0) {
        return;
    }
    sxgl_set4(context->current_colour, red, green, blue, alpha);
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue)
{
    glColor4f(red, green, blue, 1.0f);
}

void glColor3fv(const GLfloat* colour)
{
    if (colour != 0) {
        glColor4f(colour[0], colour[1], colour[2], 1.0f);
    }
}

void glColor4fv(const GLfloat* colour)
{
    if (colour != 0) {
        glColor4f(colour[0], colour[1], colour[2], colour[3]);
    }
}

void glColor3ub(GLubyte red, GLubyte green, GLubyte blue)
{
    glColor4f((float)red / 255.0f, (float)green / 255.0f, (float)blue / 255.0f, 1.0f);
}

void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha)
{
    glColor4f((float)red / 255.0f, (float)green / 255.0f, (float)blue / 255.0f, (float)alpha / 255.0f);
}

void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
    struct sxgl_context* context = g_sxgl_current;

    if (context == 0) {
        return;
    }
    context->current_normal[0] = x;
    context->current_normal[1] = y;
    context->current_normal[2] = z;
}

void glNormal3fv(const GLfloat* normal)
{
    if (normal != 0) {
        glNormal3f(normal[0], normal[1], normal[2]);
    }
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    struct sxgl_context* context = g_sxgl_current;

    if (context == 0) {
        return;
    }
    context->current_tex[0] = s;
    context->current_tex[1] = t;
}

void glTexCoord2fv(const GLfloat* coordinates)
{
    if (coordinates != 0) {
        glTexCoord2f(coordinates[0], coordinates[1]);
    }
}

/* ------------------------------------------------------------------------
 * Client arrays
 * ------------------------------------------------------------------------ */

static struct sxgl_array* sxgl_array_for(struct sxgl_context* context, GLenum array)
{
    switch (array) {
    case GL_VERTEX_ARRAY:
        return &context->vertex_array;
    case GL_COLOR_ARRAY:
        return &context->colour_array;
    case GL_NORMAL_ARRAY:
        return &context->normal_array;
    case GL_TEXTURE_COORD_ARRAY:
        return &context->tex_array;
    default:
        return 0;
    }
}

static void sxgl_client_state(GLenum array, int enabled, const char* call)
{
    struct sxgl_context* context = sxgl_enter(call);
    struct sxgl_array* target;

    if (context == 0) {
        return;
    }
    target = sxgl_array_for(context, array);
    if (target == 0) {
        sxgl_raise(context, GL_INVALID_ENUM, call, 0);
        return;
    }
    target->enabled = enabled;
}

void glEnableClientState(GLenum array)
{
    sxgl_client_state(array, 1, "glEnableClientState");
}

void glDisableClientState(GLenum array)
{
    sxgl_client_state(array, 0, "glDisableClientState");
}

static void sxgl_set_array(struct sxgl_array* target, GLint size, GLenum type, GLsizei stride, const GLvoid* pointer)
{
    target->size = size;
    target->type = type;
    target->stride = stride;
    target->pointer = (const unsigned char*)pointer;
}

void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer)
{
    struct sxgl_context* context = sxgl_enter("glVertexPointer");

    if (context == 0) {
        return;
    }
    if (type != GL_FLOAT) {
        sxgl_raise(context, GL_INVALID_ENUM, "glVertexPointer", "GL_FLOAT only");
        return;
    }
    if (size < 2 || size > 4 || stride < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glVertexPointer", 0);
        return;
    }
    sxgl_set_array(&context->vertex_array, size, type, stride, pointer);
}

void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer)
{
    struct sxgl_context* context = sxgl_enter("glColorPointer");

    if (context == 0) {
        return;
    }
    if (type != GL_FLOAT && type != GL_UNSIGNED_BYTE) {
        sxgl_raise(context, GL_INVALID_ENUM, "glColorPointer", "GL_FLOAT or GL_UNSIGNED_BYTE");
        return;
    }
    if ((size != 3 && size != 4) || stride < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glColorPointer", 0);
        return;
    }
    sxgl_set_array(&context->colour_array, size, type, stride, pointer);
}

void glNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer)
{
    struct sxgl_context* context = sxgl_enter("glNormalPointer");

    if (context == 0) {
        return;
    }
    if (type != GL_FLOAT) {
        sxgl_raise(context, GL_INVALID_ENUM, "glNormalPointer", "GL_FLOAT only");
        return;
    }
    if (stride < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glNormalPointer", 0);
        return;
    }
    sxgl_set_array(&context->normal_array, 3, type, stride, pointer);
}

void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer)
{
    struct sxgl_context* context = sxgl_enter("glTexCoordPointer");

    if (context == 0) {
        return;
    }
    if (type != GL_FLOAT) {
        sxgl_raise(context, GL_INVALID_ENUM, "glTexCoordPointer", "GL_FLOAT only");
        return;
    }
    if (size != 2 || stride < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, "glTexCoordPointer", "two coordinates only");
        return;
    }
    sxgl_set_array(&context->tex_array, size, type, stride, pointer);
}

static const unsigned char* sxgl_element(const struct sxgl_array* array, GLint index, int element_size)
{
    GLsizei stride = array->stride != 0 ? array->stride : array->size * element_size;
    return array->pointer + (size_t)index * (size_t)stride;
}

/* glArrayElement: the arrays set the current attributes, then the vertex. */
static void sxgl_array_element(struct sxgl_context* context, GLint index)
{
    const float* position;

    if (context->colour_array.enabled && context->colour_array.pointer != 0) {
        if (context->colour_array.type == GL_FLOAT) {
            const float* c = (const float*)sxgl_element(&context->colour_array, index, (int)sizeof(float));
            sxgl_set4(context->current_colour, c[0], c[1], c[2], context->colour_array.size == 4 ? c[3] : 1.0f);
        } else {
            const unsigned char* c = sxgl_element(&context->colour_array, index, 1);
            sxgl_set4(context->current_colour, (float)c[0] / 255.0f, (float)c[1] / 255.0f, (float)c[2] / 255.0f,
                context->colour_array.size == 4 ? (float)c[3] / 255.0f : 1.0f);
        }
    }
    if (context->normal_array.enabled && context->normal_array.pointer != 0) {
        const float* n = (const float*)sxgl_element(&context->normal_array, index, (int)sizeof(float));
        memcpy(context->current_normal, n, sizeof(context->current_normal));
    }
    if (context->tex_array.enabled && context->tex_array.pointer != 0) {
        const float* t = (const float*)sxgl_element(&context->tex_array, index, (int)sizeof(float));
        context->current_tex[0] = t[0];
        context->current_tex[1] = t[1];
    }
    position = (const float*)sxgl_element(&context->vertex_array, index, (int)sizeof(float));
    sxgl_vertex4(context, position[0], position[1], context->vertex_array.size >= 3 ? position[2] : 0.0f,
        context->vertex_array.size == 4 ? position[3] : 1.0f);
}

static struct sxgl_context* sxgl_enter_draw(GLenum mode, GLsizei count, const char* call)
{
    struct sxgl_context* context = sxgl_enter(call);

    if (context == 0) {
        return 0;
    }
    if (!sxgl_valid_primitive(mode)) {
        sxgl_raise(context, GL_INVALID_ENUM, call, "triangle primitives only");
        return 0;
    }
    if (count < 0) {
        sxgl_raise(context, GL_INVALID_VALUE, call, "negative count");
        return 0;
    }
    if (!context->vertex_array.enabled || context->vertex_array.pointer == 0) {
        /* No vertex array: GL draws nothing. */
        return 0;
    }
    context->primitive = mode;
    context->primitive_count = 0;
    return context;
}

void glDrawArrays(GLenum mode, GLint first, GLsizei count)
{
    struct sxgl_context* context = sxgl_enter_draw(mode, count, "glDrawArrays");
    GLsizei index;

    if (context == 0) {
        return;
    }
    for (index = 0; index < count; ++index) {
        sxgl_array_element(context, first + index);
    }
    context->primitive_count = 0;
}

void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices)
{
    struct sxgl_context* context;
    GLsizei index;

    if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT) {
        if (g_sxgl_current != 0) {
            sxgl_raise(g_sxgl_current, GL_INVALID_ENUM, "glDrawElements", "index type");
        }
        return;
    }
    context = sxgl_enter_draw(mode, count, "glDrawElements");
    if (context == 0 || indices == 0) {
        return;
    }
    for (index = 0; index < count; ++index) {
        GLint element;
        if (type == GL_UNSIGNED_BYTE) {
            element = ((const GLubyte*)indices)[index];
        } else if (type == GL_UNSIGNED_SHORT) {
            element = ((const GLushort*)indices)[index];
        } else {
            element = (GLint)((const GLuint*)indices)[index];
        }
        sxgl_array_element(context, element);
    }
    context->primitive_count = 0;
}
