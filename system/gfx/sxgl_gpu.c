/* SxGL's GPU backend: an encoder for the virgl command stream
 * (docs/roadmaps/open/VIRGL_ROADMAP.md, phase 4).
 *
 * One virgl context per SxGL context, with three resources: the colour render
 * target (backed by a section the readback lands in), a host-only depth
 * buffer, and a vertex buffer (backed by a section SxGL writes vertices into).
 *
 * Triangles are queued, not drawn: the vertices go into the vertex section and
 * the commands into a buffer. A run of triangles under one state is one
 * DRAW_VBO. A flush sends the vertices (TRANSFER_TO_HOST) and then the commands
 * (SUBMIT), both synchronous, so the vertex section and the command buffer are
 * free again as soon as the flush returns. Bound state survives a flush: the
 * host context keeps it.
 *
 * The render target is stored top row first, like the caller's bitmap: the
 * viewport flips y, and the scissor rectangle is given top-down with it.
 */

#include "sxgl_gpu.h"

#include <stdlib.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxgfx/virgl_protocol.h"

#define SXGL_GPU_COMMAND_DWORDS (SAVANXP_GPU_3D_MAX_SUBMIT_BYTES / 4u)
/* Room left for the draw that closes the open run, which a flush appends. */
#define SXGL_GPU_COMMAND_RESERVE (VIRGL_DRAW_VBO_SIZE + 1u)
#define SXGL_GPU_VERTEX_BUFFER_BYTES (1024u * 1024u)
/* position, colour, (s, t, 0, 1), (fog colour rgb, fog factor) */
#define SXGL_GPU_VERTEX_FLOATS 16u
#define SXGL_GPU_VERTEX_STRIDE (SXGL_GPU_VERTEX_FLOATS * 4u)
#define SXGL_GPU_VERTEX_CAPACITY (SXGL_GPU_VERTEX_BUFFER_BYTES / SXGL_GPU_VERTEX_STRIDE)
/* What the host allocates for parsing a shader; ours are a few dozen tokens. */
#define SXGL_GPU_SHADER_TOKENS 300u

static const char k_vertex_shader[] =
    "VERT\n"
    "DCL IN[0]\n"
    "DCL IN[1]\n"
    "DCL IN[2]\n"
    "DCL IN[3]\n"
    "DCL OUT[0], POSITION\n"
    "DCL OUT[1], COLOR\n"
    "DCL OUT[2], GENERIC[0]\n"
    "DCL OUT[3], GENERIC[1]\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: MOV OUT[1], IN[1]\n"
    "  2: MOV OUT[2], IN[2]\n"
    "  3: MOV OUT[3], IN[3]\n"
    "  4: END\n";

/* The software rasterizer interpolates colour and fog linearly in screen
 * space and texture coordinates with perspective; the shaders do the same, so
 * both backends agree. */
static const char k_fragment_shader_colour[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], COLOR, LINEAR\n"
    "DCL OUT[0], COLOR\n"
    "  0: MOV OUT[0], IN[0]\n"
    "  1: END\n";

/* colour.rgb = fog * colour + (1 - fog) * fog colour; alpha untouched. */
static const char k_fragment_shader_fog[] =
    "FRAG\n"
    "PROPERTY FS_COLOR0_WRITES_ALL_CBUFS 1\n"
    "DCL IN[0], COLOR, LINEAR\n"
    "DCL IN[1], GENERIC[1], LINEAR\n"
    "DCL OUT[0], COLOR\n"
    "DCL TEMP[0]\n"
    "  0: LRP TEMP[0].xyz, IN[1].wwww, IN[0], IN[1]\n"
    "  1: MOV TEMP[0].w, IN[0].wwww\n"
    "  2: MOV OUT[0], TEMP[0]\n"
    "  3: END\n";

struct sxgl_gpu {
    long device;
    uint32_t context;
    int width;
    int height;
    int failed;

    uint32_t depth_format;
    long colour_section;
    uint32_t* colour_pixels;
    uint32_t colour_resource;
    uint32_t depth_resource;
    long vertex_section;
    float* vertices;
    uint32_t vertex_resource;

    uint32_t next_handle;
    uint32_t colour_surface;
    uint32_t depth_surface;
    uint32_t vertex_shader;
    uint32_t fragment_colour;
    uint32_t fragment_fog;
    uint32_t vertex_elements;
    uint32_t blend;
    uint32_t dsa;
    uint32_t rasterizer;

    struct sxgl_gpu_state state;
    int state_valid;

    uint32_t vertex_count;
    uint32_t run_first;
    uint32_t commands[SXGL_GPU_COMMAND_DWORDS];
    uint32_t command_count;
};

static uint32_t sxgl_gpu_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int sxgl_gpu_control(struct sxgl_gpu* gpu, unsigned long code, void* argument)
{
    if (gpu->failed) {
        return -1;
    }
    if (sxgl_gpu_device_control(gpu->device, code, argument) != 0) {
        /* A lost device or a refused call: the context stops using the GPU
         * and SxGL falls back to software. */
        gpu->failed = 1;
        return -1;
    }
    return 0;
}

static uint32_t sxgl_gpu_handle(struct sxgl_gpu* gpu)
{
    gpu->next_handle += 1u;
    return gpu->next_handle;
}

/* ------------------------------------------------------------------------
 * The command buffer
 * ------------------------------------------------------------------------ */

static int sxgl_gpu_submit(struct sxgl_gpu* gpu)
{
    struct savanxp_gpu_3d_submit submit;

    if (gpu->command_count == 0u) {
        return gpu->failed ? -1 : 0;
    }
    submit.context_id = gpu->context;
    submit.size = gpu->command_count * 4u;
    submit.commands = (uint64_t)(uintptr_t)gpu->commands;
    gpu->command_count = 0;
    return sxgl_gpu_control(gpu, GPU_IOC_3D_SUBMIT, &submit);
}

static void sxgl_gpu_close_run(struct sxgl_gpu* gpu)
{
    uint32_t count = gpu->vertex_count - gpu->run_first;
    uint32_t* out;

    if (count == 0u) {
        return;
    }
    out = gpu->commands + gpu->command_count;
    out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_DRAW_VBO, 0, VIRGL_DRAW_VBO_SIZE);
    out[1] = gpu->run_first;
    out[2] = count;
    out[3] = PIPE_PRIM_TRIANGLES;
    out[4] = 0u; /* not indexed */
    out[5] = 1u; /* one instance */
    out[6] = 0u;
    out[7] = 0u;
    out[8] = 0u;
    out[9] = 0u;
    out[10] = gpu->run_first;
    out[11] = gpu->vertex_count - 1u;
    out[12] = 0u;
    gpu->command_count += VIRGL_DRAW_VBO_SIZE + 1u;
    gpu->run_first = gpu->vertex_count;
}

int sxgl_gpu_flush(struct sxgl_gpu* gpu)
{
    struct savanxp_gpu_3d_transfer transfer;

    if (gpu == 0 || gpu->failed) {
        return -1;
    }
    sxgl_gpu_close_run(gpu);
    if (gpu->vertex_count != 0u) {
        memset(&transfer, 0, sizeof(transfer));
        transfer.context_id = gpu->context;
        transfer.resource_id = gpu->vertex_resource;
        transfer.direction = SAVANXP_GPU_3D_TRANSFER_TO_HOST;
        transfer.width = gpu->vertex_count * SXGL_GPU_VERTEX_STRIDE;
        transfer.height = 1u;
        transfer.depth = 1u;
        if (sxgl_gpu_control(gpu, GPU_IOC_3D_TRANSFER, &transfer) != 0) {
            return -1;
        }
    }
    gpu->vertex_count = 0;
    gpu->run_first = 0;
    return sxgl_gpu_submit(gpu);
}

/* Room for `dwords` more, flushing first if needed. Returns where to write,
 * or 0 when the context failed. */
static uint32_t* sxgl_gpu_reserve(struct sxgl_gpu* gpu, uint32_t dwords)
{
    uint32_t* out;

    if (gpu->failed) {
        return 0;
    }
    if (gpu->command_count + dwords + SXGL_GPU_COMMAND_RESERVE > SXGL_GPU_COMMAND_DWORDS) {
        if (sxgl_gpu_flush(gpu) != 0) {
            return 0;
        }
    }
    out = gpu->commands + gpu->command_count;
    gpu->command_count += dwords;
    return out;
}

/* ------------------------------------------------------------------------
 * Objects
 * ------------------------------------------------------------------------ */

static uint32_t sxgl_gpu_create_shader(struct sxgl_gpu* gpu, uint32_t type, const char* text, uint32_t bytes)
{
    uint32_t dwords = (bytes + 3u) / 4u;
    uint32_t handle = sxgl_gpu_handle(gpu);
    uint32_t* out = sxgl_gpu_reserve(gpu, 1u + VIRGL_OBJ_SHADER_HEADER_SIZE + dwords);

    if (out == 0) {
        return 0;
    }
    out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SHADER, VIRGL_OBJ_SHADER_HEADER_SIZE + dwords);
    out[1] = handle;
    out[2] = type;
    out[3] = bytes;
    out[4] = SXGL_GPU_SHADER_TOKENS;
    out[5] = 0u; /* no stream output */
    out[6 + dwords - 1u] = 0u;
    memcpy(out + 6, text, bytes);
    return handle;
}

static uint32_t sxgl_gpu_create_surface(struct sxgl_gpu* gpu, uint32_t resource, uint32_t format)
{
    uint32_t handle = sxgl_gpu_handle(gpu);
    uint32_t* out = sxgl_gpu_reserve(gpu, 1u + VIRGL_OBJ_SURFACE_SIZE);

    if (out == 0) {
        return 0;
    }
    out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE);
    out[1] = handle;
    out[2] = resource;
    out[3] = format;
    out[4] = 0u;
    out[5] = 0u;
    return handle;
}

static void sxgl_gpu_bind(struct sxgl_gpu* gpu, uint32_t type, uint32_t handle)
{
    uint32_t* out = sxgl_gpu_reserve(gpu, 2u);
    if (out != 0) {
        out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_BIND_OBJECT, type, 1u);
        out[1] = handle;
    }
}

static void sxgl_gpu_destroy_object(struct sxgl_gpu* gpu, uint32_t type, uint32_t handle)
{
    uint32_t* out;

    if (handle == 0u) {
        return;
    }
    out = sxgl_gpu_reserve(gpu, 2u);
    if (out != 0) {
        out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_DESTROY_OBJECT, type, 1u);
        out[1] = handle;
    }
}

static void sxgl_gpu_bind_shader(struct sxgl_gpu* gpu, uint32_t handle, uint32_t type)
{
    uint32_t* out = sxgl_gpu_reserve(gpu, 1u + VIRGL_BIND_SHADER_SIZE);
    if (out != 0) {
        out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_BIND_SHADER, 0, VIRGL_BIND_SHADER_SIZE);
        out[1] = handle;
        out[2] = type;
    }
}

/* Replaces one of the three state objects: create, bind, destroy the old. */
static void sxgl_gpu_replace(struct sxgl_gpu* gpu, uint32_t type, uint32_t* current, const uint32_t* payload,
    uint32_t length)
{
    uint32_t handle = sxgl_gpu_handle(gpu);
    uint32_t* out = sxgl_gpu_reserve(gpu, 2u + length);

    if (out == 0) {
        return;
    }
    out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_CREATE_OBJECT, type, length + 1u);
    out[1] = handle;
    memcpy(out + 2, payload, length * 4u);
    sxgl_gpu_bind(gpu, type, handle);
    sxgl_gpu_destroy_object(gpu, type, *current);
    *current = handle;
}

static uint32_t sxgl_gpu_blend_factor(GLenum factor)
{
    switch (factor) {
    case GL_ZERO:
        return PIPE_BLENDFACTOR_ZERO;
    case GL_ONE:
        return PIPE_BLENDFACTOR_ONE;
    case GL_SRC_COLOR:
        return PIPE_BLENDFACTOR_SRC_COLOR;
    case GL_ONE_MINUS_SRC_COLOR:
        return PIPE_BLENDFACTOR_INV_SRC_COLOR;
    case GL_SRC_ALPHA:
        return PIPE_BLENDFACTOR_SRC_ALPHA;
    case GL_ONE_MINUS_SRC_ALPHA:
        return PIPE_BLENDFACTOR_INV_SRC_ALPHA;
    case GL_DST_ALPHA:
        return PIPE_BLENDFACTOR_DST_ALPHA;
    case GL_ONE_MINUS_DST_ALPHA:
        return PIPE_BLENDFACTOR_INV_DST_ALPHA;
    case GL_DST_COLOR:
        return PIPE_BLENDFACTOR_DST_COLOR;
    case GL_ONE_MINUS_DST_COLOR:
        return PIPE_BLENDFACTOR_INV_DST_COLOR;
    default:
        return PIPE_BLENDFACTOR_ONE;
    }
}

static void sxgl_gpu_emit_blend(struct sxgl_gpu* gpu, int enabled, GLenum source, GLenum destination, uint32_t mask)
{
    uint32_t payload[VIRGL_OBJ_BLEND_SIZE - 1u];
    uint32_t src = sxgl_gpu_blend_factor(source);
    uint32_t dst = sxgl_gpu_blend_factor(destination);

    memset(payload, 0, sizeof(payload));
    payload[2] = VIRGL_OBJ_BLEND_S2_BLEND_ENABLE(enabled) | VIRGL_OBJ_BLEND_S2_RGB_FUNC(PIPE_BLEND_ADD) |
        VIRGL_OBJ_BLEND_S2_RGB_SRC_FACTOR(src) | VIRGL_OBJ_BLEND_S2_RGB_DST_FACTOR(dst) |
        VIRGL_OBJ_BLEND_S2_ALPHA_FUNC(PIPE_BLEND_ADD) | VIRGL_OBJ_BLEND_S2_ALPHA_SRC_FACTOR(src) |
        VIRGL_OBJ_BLEND_S2_ALPHA_DST_FACTOR(dst) | VIRGL_OBJ_BLEND_S2_COLORMASK(mask);
    sxgl_gpu_replace(gpu, VIRGL_OBJECT_BLEND, &gpu->blend, payload, VIRGL_OBJ_BLEND_SIZE - 1u);
}

static void sxgl_gpu_emit_dsa(struct sxgl_gpu* gpu, int depth_test, GLenum depth_func, int depth_mask, int alpha_test,
    GLenum alpha_func, float alpha_reference)
{
    uint32_t payload[VIRGL_OBJ_DSA_SIZE - 1u];

    memset(payload, 0, sizeof(payload));
    /* GL writes depth only when the test is enabled. */
    payload[0] = VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(depth_test) |
        VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(depth_test && depth_mask) |
        VIRGL_OBJ_DSA_S0_DEPTH_FUNC(depth_func - GL_NEVER) | VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(alpha_test) |
        VIRGL_OBJ_DSA_S0_ALPHA_FUNC(alpha_func - GL_NEVER);
    payload[3] = sxgl_gpu_bits(alpha_reference);
    sxgl_gpu_replace(gpu, VIRGL_OBJECT_DSA, &gpu->dsa, payload, VIRGL_OBJ_DSA_SIZE - 1u);
}

static void sxgl_gpu_emit_rasterizer(struct sxgl_gpu* gpu, uint32_t cull, int front_ccw, int scissor)
{
    uint32_t payload[VIRGL_OBJ_RS_SIZE - 1u];

    memset(payload, 0, sizeof(payload));
    payload[0] = VIRGL_OBJ_RS_S0_DEPTH_CLIP(1) | VIRGL_OBJ_RS_S0_CULL_FACE(cull) | VIRGL_OBJ_RS_S0_SCISSOR(scissor) |
        VIRGL_OBJ_RS_S0_FRONT_CCW(front_ccw) | VIRGL_OBJ_RS_S0_CLAMP_VERTEX_COLOR(1) |
        VIRGL_OBJ_RS_S0_CLAMP_FRAGMENT_COLOR(1) | VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(1);
    payload[1] = sxgl_gpu_bits(1.0f); /* point size */
    payload[3] = 0xffffu;             /* line stipple pattern */
    payload[4] = sxgl_gpu_bits(1.0f); /* line width */
    sxgl_gpu_replace(gpu, VIRGL_OBJECT_RASTERIZER, &gpu->rasterizer, payload, VIRGL_OBJ_RS_SIZE - 1u);
}

/* x0..x1, y0..y1 in top-down pixels. */
static void sxgl_gpu_emit_scissor(struct sxgl_gpu* gpu, int x0, int y0, int x1, int y1)
{
    uint32_t* out = sxgl_gpu_reserve(gpu, 1u + VIRGL_SET_SCISSOR_STATE_SIZE(1u));
    if (out != 0) {
        out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_SET_SCISSOR_STATE, 0, VIRGL_SET_SCISSOR_STATE_SIZE(1u));
        out[1] = 0u;
        out[2] = (uint32_t)x0 | ((uint32_t)y0 << 16);
        out[3] = (uint32_t)x1 | ((uint32_t)y1 << 16);
    }
}

/* A GL viewport (y from the bottom) onto the top-down target. */
static void sxgl_gpu_emit_viewport(struct sxgl_gpu* gpu, const int viewport[4], float near_value, float far_value)
{
    uint32_t* out = sxgl_gpu_reserve(gpu, 1u + VIRGL_SET_VIEWPORT_STATE_SIZE(1u));
    float half_width = (float)viewport[2] * 0.5f;
    float half_height = (float)viewport[3] * 0.5f;
    float top = (float)(gpu->height - (viewport[1] + viewport[3]));

    if (out == 0) {
        return;
    }
    out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_SET_VIEWPORT_STATE, 0, VIRGL_SET_VIEWPORT_STATE_SIZE(1u));
    out[1] = 0u;
    out[2] = sxgl_gpu_bits(half_width);
    out[3] = sxgl_gpu_bits(-half_height);
    out[4] = sxgl_gpu_bits((far_value - near_value) * 0.5f);
    out[5] = sxgl_gpu_bits((float)viewport[0] + half_width);
    out[6] = sxgl_gpu_bits(top + half_height);
    out[7] = sxgl_gpu_bits((far_value + near_value) * 0.5f);
}

static uint32_t sxgl_gpu_cull(const struct sxgl_gpu_state* state)
{
    if (!state->cull_face) {
        return PIPE_FACE_NONE;
    }
    if (state->cull_mode == GL_FRONT) {
        return PIPE_FACE_FRONT;
    }
    if (state->cull_mode == GL_FRONT_AND_BACK) {
        return PIPE_FACE_FRONT_AND_BACK;
    }
    return PIPE_FACE_BACK;
}

/* Brings the host state to `state`, emitting only what changed. The open run
 * is drawn first: it was queued under the old state. */
static void sxgl_gpu_apply(struct sxgl_gpu* gpu, const struct sxgl_gpu_state* state)
{
    const struct sxgl_gpu_state* old = gpu->state_valid ? &gpu->state : 0;

    if (old != 0 && memcmp(old, state, sizeof(*state)) == 0) {
        return;
    }
    sxgl_gpu_close_run(gpu);
    if (old == 0 || memcmp(old->viewport, state->viewport, sizeof(state->viewport)) != 0 ||
        old->depth_range[0] != state->depth_range[0] || old->depth_range[1] != state->depth_range[1]) {
        sxgl_gpu_emit_viewport(gpu, state->viewport, state->depth_range[0], state->depth_range[1]);
    }
    if (state->scissor_test &&
        (old == 0 || !old->scissor_test || memcmp(old->scissor, state->scissor, sizeof(state->scissor)) != 0)) {
        int x0 = state->scissor[0] < 0 ? 0 : state->scissor[0];
        int x1 = state->scissor[0] + state->scissor[2];
        int y1 = gpu->height - state->scissor[1];
        int y0 = gpu->height - (state->scissor[1] + state->scissor[3]);
        if (x1 > gpu->width) {
            x1 = gpu->width;
        }
        if (y0 < 0) {
            y0 = 0;
        }
        if (y1 > gpu->height) {
            y1 = gpu->height;
        }
        if (x1 < x0) {
            x1 = x0;
        }
        if (y1 < y0) {
            y1 = y0;
        }
        sxgl_gpu_emit_scissor(gpu, x0, y0, x1, y1);
    }
    if (old == 0 || old->cull_face != state->cull_face || old->cull_mode != state->cull_mode ||
        old->front_face != state->front_face || old->scissor_test != state->scissor_test) {
        /* front_ccw is GL's own sense even with the flipped viewport:
         * virglrenderer accounts for the flip itself (checked by sxgltest). */
        sxgl_gpu_emit_rasterizer(gpu, sxgl_gpu_cull(state), state->front_face == GL_CCW, state->scissor_test);
    }
    if (old == 0 || old->depth_test != state->depth_test || old->depth_func != state->depth_func ||
        old->depth_mask != state->depth_mask || old->alpha_test != state->alpha_test ||
        old->alpha_func != state->alpha_func || old->alpha_reference != state->alpha_reference) {
        sxgl_gpu_emit_dsa(gpu, state->depth_test, state->depth_func, state->depth_mask, state->alpha_test,
            state->alpha_func, state->alpha_reference);
    }
    if (old == 0 || old->blend != state->blend || old->blend_source != state->blend_source ||
        old->blend_destination != state->blend_destination) {
        sxgl_gpu_emit_blend(gpu, state->blend, state->blend_source, state->blend_destination, PIPE_MASK_RGBA);
    }
    if (old == 0 || old->fog != state->fog) {
        sxgl_gpu_bind_shader(gpu, state->fog ? gpu->fragment_fog : gpu->fragment_colour, PIPE_SHADER_FRAGMENT);
    }
    gpu->state = *state;
    gpu->state_valid = 1;
}

/* ------------------------------------------------------------------------
 * Resources
 * ------------------------------------------------------------------------ */

static uint32_t sxgl_gpu_resource(struct sxgl_gpu* gpu, uint32_t target, uint32_t format, uint32_t bind,
    uint32_t width, uint32_t height, long section, uint32_t bytes)
{
    struct savanxp_gpu_3d_resource request;

    memset(&request, 0, sizeof(request));
    request.context_id = gpu->context;
    request.target = target;
    request.format = format;
    request.bind = bind;
    request.width = width;
    request.height = height;
    request.depth = 1u;
    request.array_size = 1u;
    request.section_handle = (int32_t)section;
    request.backing_size = bytes;
    if (sxgl_gpu_control(gpu, GPU_IOC_3D_CREATE_RESOURCE, &request) != 0) {
        return 0;
    }
    return request.resource_id;
}

static void sxgl_gpu_release_resource(struct sxgl_gpu* gpu, uint32_t* resource)
{
    struct savanxp_gpu_3d_resource_ref ref;

    if (*resource == 0u) {
        return;
    }
    ref.context_id = gpu->context;
    ref.resource_id = *resource;
    (void)sxgl_gpu_device_control(gpu->device, GPU_IOC_3D_DESTROY_RESOURCE, &ref);
    *resource = 0;
}

/* The colour target, its depth buffer and their surfaces, at the current
 * size, bound as the framebuffer. */
static int sxgl_gpu_make_targets(struct sxgl_gpu* gpu)
{
    uint32_t bytes = (uint32_t)gpu->width * (uint32_t)gpu->height * 4u;
    void* view = 0;
    uint32_t* out;

    gpu->colour_section = sxgl_gpu_device_section(bytes, &view);
    if (gpu->colour_section < 0) {
        gpu->failed = 1;
        return -1;
    }
    gpu->colour_pixels = (uint32_t*)view;
    gpu->colour_resource = sxgl_gpu_resource(gpu, PIPE_TEXTURE_2D, VIRGL_FORMAT_B8G8R8A8_UNORM,
        VIRGL_BIND_RENDER_TARGET, (uint32_t)gpu->width, (uint32_t)gpu->height, gpu->colour_section, bytes);
    gpu->depth_resource = sxgl_gpu_resource(gpu, PIPE_TEXTURE_2D, gpu->depth_format,
        VIRGL_BIND_DEPTH_STENCIL, (uint32_t)gpu->width, (uint32_t)gpu->height, -1, 0u);
    if (gpu->failed) {
        return -1;
    }
    gpu->colour_surface = sxgl_gpu_create_surface(gpu, gpu->colour_resource, VIRGL_FORMAT_B8G8R8A8_UNORM);
    gpu->depth_surface = sxgl_gpu_create_surface(gpu, gpu->depth_resource, gpu->depth_format);
    out = sxgl_gpu_reserve(gpu, 1u + VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1u));
    if (out == 0) {
        return -1;
    }
    out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1u));
    out[1] = 1u;
    out[2] = gpu->depth_surface;
    out[3] = gpu->colour_surface;
    /* Viewport and scissor depend on the height: emit them again. */
    gpu->state_valid = 0;
    return gpu->failed ? -1 : 0;
}

static void sxgl_gpu_release_targets(struct sxgl_gpu* gpu)
{
    sxgl_gpu_destroy_object(gpu, VIRGL_OBJECT_SURFACE, gpu->colour_surface);
    sxgl_gpu_destroy_object(gpu, VIRGL_OBJECT_SURFACE, gpu->depth_surface);
    gpu->colour_surface = 0;
    gpu->depth_surface = 0;
    (void)sxgl_gpu_flush(gpu);
    sxgl_gpu_release_resource(gpu, &gpu->colour_resource);
    sxgl_gpu_release_resource(gpu, &gpu->depth_resource);
    sxgl_gpu_device_section_free(gpu->colour_section, gpu->colour_pixels);
    gpu->colour_section = -1;
    gpu->colour_pixels = 0;
}

/* ------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------ */

/* The first depth format the host can render to, from the capset; 0 if the
 * capset says none of them (or cannot be read). */
static uint32_t sxgl_gpu_pick_depth_format(struct sxgl_gpu* gpu)
{
    static const uint32_t k_candidates[] = {
        VIRGL_FORMAT_Z24X8_UNORM,
        VIRGL_FORMAT_S8_UINT_Z24_UNORM,
        VIRGL_FORMAT_Z32_FLOAT,
        VIRGL_FORMAT_Z16_UNORM,
    };
    uint32_t caps[VIRGL_CAPS_MASK_DWORDS];
    struct savanxp_gpu_3d_capset capset;
    unsigned mask;
    unsigned index;

    memset(caps, 0, sizeof(caps));
    memset(&capset, 0, sizeof(capset));
    capset.data = (uint64_t)(uintptr_t)caps;
    capset.size = sizeof(caps);
    if (sxgl_gpu_device_control(gpu->device, GPU_IOC_3D_GET_CAPSET, &capset) != 0 || capset.size < sizeof(caps)) {
        return 0;
    }
    /* virglrenderer 1.3 leaves the depth/stencil mask empty and lists the
     * depth formats it can attach among the sampler formats: read that one
     * when the other is blank. */
    mask = VIRGL_CAPS_DEPTH_STENCIL_MASK_DWORD;
    for (index = 0; index < 16u && caps[mask + index] == 0u; ++index) {
    }
    if (index == 16u) {
        mask = VIRGL_CAPS_SAMPLER_MASK_DWORD;
    }
    for (index = 0; index < sizeof(k_candidates) / sizeof(k_candidates[0]); ++index) {
        uint32_t format = k_candidates[index];
        if ((caps[mask + format / 32u] & (1u << (format % 32u))) != 0) {
            return format;
        }
    }
    return 0;
}

struct sxgl_gpu* sxgl_gpu_create(int width, int height)
{
    struct savanxp_gpu_info info;
    struct savanxp_gpu_3d_context context;
    struct sxgl_gpu* gpu;
    void* view = 0;
    uint32_t* out;
    static const uint32_t k_offsets[4] = {0u, 16u, 32u, 48u};
    int index;

    if (width <= 0 || height <= 0) {
        return 0;
    }
    gpu = (struct sxgl_gpu*)malloc(sizeof(*gpu));
    if (gpu == 0) {
        return 0;
    }
    memset(gpu, 0, sizeof(*gpu));
    gpu->colour_section = -1;
    gpu->vertex_section = -1;
    gpu->width = width;
    gpu->height = height;
    gpu->device = sxgl_gpu_device_open();
    if (gpu->device < 0) {
        free(gpu);
        return 0;
    }
    memset(&info, 0, sizeof(info));
    memset(&context, 0, sizeof(context));
    memcpy(context.debug_name, "sxgl", 5);
    if (sxgl_gpu_device_control(gpu->device, GPU_IOC_GET_INFO, &info) != 0 ||
        (info.flags & SAVANXP_GPU_INFO_FLAG_3D) == 0 ||
        sxgl_gpu_device_control(gpu->device, GPU_IOC_3D_CREATE_CONTEXT, &context) != 0) {
        sxgl_gpu_device_close(gpu->device);
        free(gpu);
        return 0;
    }
    gpu->context = context.context_id;
    gpu->depth_format = sxgl_gpu_pick_depth_format(gpu);
    if (gpu->depth_format == 0u) {
        sxgl_gpu_destroy(gpu);
        return 0;
    }

    gpu->vertex_section = sxgl_gpu_device_section(SXGL_GPU_VERTEX_BUFFER_BYTES, &view);
    gpu->vertices = (float*)view;
    if (gpu->vertex_section < 0) {
        sxgl_gpu_destroy(gpu);
        return 0;
    }
    gpu->vertex_resource = sxgl_gpu_resource(gpu, PIPE_BUFFER, VIRGL_FORMAT_R8_UNORM, VIRGL_BIND_VERTEX_BUFFER,
        SXGL_GPU_VERTEX_BUFFER_BYTES, 1u, gpu->vertex_section, SXGL_GPU_VERTEX_BUFFER_BYTES);

    gpu->vertex_shader =
        sxgl_gpu_create_shader(gpu, PIPE_SHADER_VERTEX, k_vertex_shader, (uint32_t)sizeof(k_vertex_shader));
    gpu->fragment_colour = sxgl_gpu_create_shader(
        gpu, PIPE_SHADER_FRAGMENT, k_fragment_shader_colour, (uint32_t)sizeof(k_fragment_shader_colour));
    gpu->fragment_fog = sxgl_gpu_create_shader(
        gpu, PIPE_SHADER_FRAGMENT, k_fragment_shader_fog, (uint32_t)sizeof(k_fragment_shader_fog));
    sxgl_gpu_bind_shader(gpu, gpu->vertex_shader, PIPE_SHADER_VERTEX);

    gpu->vertex_elements = sxgl_gpu_handle(gpu);
    out = sxgl_gpu_reserve(gpu, 1u + VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(4u));
    if (out != 0) {
        out[0] = VIRGL_CMD_HEADER(
            VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_VERTEX_ELEMENTS, VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(4u));
        out[1] = gpu->vertex_elements;
        for (index = 0; index < 4; ++index) {
            out[2 + index * 4] = k_offsets[index];
            out[3 + index * 4] = 0u;
            out[4 + index * 4] = 0u;
            out[5 + index * 4] = VIRGL_FORMAT_R32G32B32A32_FLOAT;
        }
    }
    sxgl_gpu_bind(gpu, VIRGL_OBJECT_VERTEX_ELEMENTS, gpu->vertex_elements);
    out = sxgl_gpu_reserve(gpu, 1u + VIRGL_SET_VERTEX_BUFFERS_SIZE(1u));
    if (out != 0) {
        out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_SET_VERTEX_BUFFERS, 0, VIRGL_SET_VERTEX_BUFFERS_SIZE(1u));
        out[1] = SXGL_GPU_VERTEX_STRIDE;
        out[2] = 0u;
        out[3] = gpu->vertex_resource;
    }

    if (sxgl_gpu_make_targets(gpu) != 0 || sxgl_gpu_flush(gpu) != 0) {
        sxgl_gpu_destroy(gpu);
        return 0;
    }
    return gpu;
}

void sxgl_gpu_destroy(struct sxgl_gpu* gpu)
{
    if (gpu == 0) {
        return;
    }
    /* Destroying the context destroys every resource in it. */
    if (gpu->context != 0u) {
        (void)sxgl_gpu_device_control(gpu->device, GPU_IOC_3D_DESTROY_CONTEXT, (void*)(uintptr_t)gpu->context);
    }
    sxgl_gpu_device_section_free(gpu->colour_section, gpu->colour_pixels);
    sxgl_gpu_device_section_free(gpu->vertex_section, gpu->vertices);
    sxgl_gpu_device_close(gpu->device);
    free(gpu);
}

int sxgl_gpu_resize(struct sxgl_gpu* gpu, int width, int height)
{
    if (gpu == 0 || gpu->failed || width <= 0 || height <= 0) {
        return -1;
    }
    sxgl_gpu_release_targets(gpu);
    gpu->width = width;
    gpu->height = height;
    if (sxgl_gpu_make_targets(gpu) != 0) {
        return -1;
    }
    return sxgl_gpu_flush(gpu);
}

/* ------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------ */

static void sxgl_gpu_put(float* out, const struct sxgl_gpu_vertex* vertex, const float fog_colour[4])
{
    memcpy(out, vertex->clip, 4u * sizeof(float));
    memcpy(out + 4, vertex->colour, 4u * sizeof(float));
    out[8] = vertex->tex[0];
    out[9] = vertex->tex[1];
    out[10] = 0.0f;
    out[11] = 1.0f;
    memcpy(out + 12, fog_colour, 3u * sizeof(float));
    out[15] = vertex->fog;
}

int sxgl_gpu_triangle(struct sxgl_gpu* gpu, const struct sxgl_gpu_state* state, const struct sxgl_gpu_vertex* a,
    const struct sxgl_gpu_vertex* b, const struct sxgl_gpu_vertex* c)
{
    float* out;

    if (gpu == 0 || gpu->failed) {
        return -1;
    }
    if (gpu->vertex_count + 3u > SXGL_GPU_VERTEX_CAPACITY && sxgl_gpu_flush(gpu) != 0) {
        return -1;
    }
    sxgl_gpu_apply(gpu, state);
    if (gpu->failed) {
        return -1;
    }
    out = gpu->vertices + (size_t)gpu->vertex_count * SXGL_GPU_VERTEX_FLOATS;
    sxgl_gpu_put(out, a, state->fog_colour);
    sxgl_gpu_put(out + SXGL_GPU_VERTEX_FLOATS, b, state->fog_colour);
    sxgl_gpu_put(out + 2u * SXGL_GPU_VERTEX_FLOATS, c, state->fog_colour);
    gpu->vertex_count += 3u;
    return 0;
}

int sxgl_gpu_clear(struct sxgl_gpu* gpu, int clear_colour, int clear_depth, const float colour[4], float depth,
    int x0, int y0, int x1, int y1)
{
    uint32_t* out;
    uint64_t depth_bits;
    double depth_value = (double)depth;

    if (gpu == 0 || gpu->failed) {
        return -1;
    }
    if (!clear_colour && !clear_depth) {
        return 0;
    }
    if (x0 <= 0 && y0 <= 0 && x1 >= gpu->width && y1 >= gpu->height) {
        sxgl_gpu_close_run(gpu);
        out = sxgl_gpu_reserve(gpu, 1u + VIRGL_OBJ_CLEAR_SIZE);
        if (out == 0) {
            return -1;
        }
        memcpy(&depth_bits, &depth_value, sizeof(depth_bits));
        out[0] = VIRGL_CMD_HEADER(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE);
        out[1] = (clear_colour ? PIPE_CLEAR_COLOR0 : 0u) | (clear_depth ? PIPE_CLEAR_DEPTH : 0u);
        out[2] = sxgl_gpu_bits(colour[0]);
        out[3] = sxgl_gpu_bits(colour[1]);
        out[4] = sxgl_gpu_bits(colour[2]);
        out[5] = sxgl_gpu_bits(colour[3]);
        out[6] = (uint32_t)depth_bits;
        out[7] = (uint32_t)(depth_bits >> 32);
        out[8] = 0u;
        return gpu->failed ? -1 : 0;
    }

    /* CLEAR ignores the scissor: a part of the target is cleared by drawing
     * a quad over it, with depth always passing and written as `depth`. */
    {
        struct sxgl_gpu_state state;
        struct sxgl_gpu_vertex corners[4];
        float z = depth * 2.0f - 1.0f;
        int corner;

        memset(&state, 0, sizeof(state));
        state.viewport[2] = gpu->width;
        state.viewport[3] = gpu->height;
        state.depth_range[1] = 1.0f;
        state.scissor_test = 1;
        state.scissor[0] = x0;
        state.scissor[1] = gpu->height - y1;
        state.scissor[2] = x1 - x0;
        state.scissor[3] = y1 - y0;
        state.depth_test = clear_depth;
        state.depth_func = GL_ALWAYS;
        state.depth_mask = 1;
        state.alpha_func = GL_ALWAYS;
        state.blend_source = GL_ONE;
        state.blend_destination = GL_ZERO;
        state.front_face = GL_CCW;
        for (corner = 0; corner < 4; ++corner) {
            memset(&corners[corner], 0, sizeof(corners[corner]));
            corners[corner].clip[0] = (corner & 1) ? 1.0f : -1.0f;
            corners[corner].clip[1] = (corner & 2) ? 1.0f : -1.0f;
            corners[corner].clip[2] = z;
            corners[corner].clip[3] = 1.0f;
            memcpy(corners[corner].colour, colour, 4u * sizeof(float));
            corners[corner].fog = 1.0f;
        }
        sxgl_gpu_apply(gpu, &state);
        /* A depth-only clear writes no colour. The quad's triangles find the
         * state already applied, so the masked blend stays bound for them. */
        if (!clear_colour) {
            sxgl_gpu_emit_blend(gpu, 0, GL_ONE, GL_ZERO, 0u);
        }
        if (sxgl_gpu_triangle(gpu, &state, &corners[0], &corners[1], &corners[3]) != 0 ||
            sxgl_gpu_triangle(gpu, &state, &corners[0], &corners[3], &corners[2]) != 0) {
            return -1;
        }
        sxgl_gpu_close_run(gpu);
        if (!clear_colour) {
            /* The bound blend is not what the recorded state says: make the
             * next draw emit its own. */
            gpu->state.blend_source = 0;
        }
        return 0;
    }
}

int sxgl_gpu_read(struct sxgl_gpu* gpu, const struct sx_bitmap* colour, uint32_t alpha_bits, int x0, int y0, int x1,
    int y1)
{
    struct savanxp_gpu_3d_transfer transfer;
    uint32_t stride = (uint32_t)gpu->width * 4u;
    uint32_t pitch;
    int x;
    int y;

    if (gpu == 0 || sxgl_gpu_flush(gpu) != 0) {
        return -1;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > gpu->width) {
        x1 = gpu->width;
    }
    if (y1 > gpu->height) {
        y1 = gpu->height;
    }
    if (x0 >= x1 || y0 >= y1) {
        return 0;
    }
    memset(&transfer, 0, sizeof(transfer));
    transfer.context_id = gpu->context;
    transfer.resource_id = gpu->colour_resource;
    transfer.direction = SAVANXP_GPU_3D_TRANSFER_FROM_HOST;
    transfer.x = (uint32_t)x0;
    transfer.y = (uint32_t)y0;
    transfer.width = (uint32_t)(x1 - x0);
    transfer.height = (uint32_t)(y1 - y0);
    transfer.depth = 1u;
    transfer.stride = stride;
    transfer.offset = (uint64_t)y0 * stride + (uint64_t)x0 * 4u;
    if (sxgl_gpu_control(gpu, GPU_IOC_3D_TRANSFER, &transfer) != 0) {
        return -1;
    }
    pitch = colour->info.pitch / 4u;
    for (y = y0; y < y1; ++y) {
        const uint32_t* source = gpu->colour_pixels + (size_t)y * (size_t)gpu->width;
        uint32_t* destination = colour->pixels + (size_t)y * pitch;
        for (x = x0; x < x1; ++x) {
            destination[x] = (source[x] & 0x00ffffffu) | alpha_bits;
        }
    }
    return 0;
}
