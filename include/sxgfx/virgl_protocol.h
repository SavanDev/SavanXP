#pragma once

/* The subset of the virgl command protocol SavanXP speaks
 * (docs/roadmaps/open/VIRGL_ROADMAP.md).
 *
 * These are the wire values virglrenderer decodes from a SUBMIT_3D buffer:
 * Gallium's enums plus virgl's command and object numbers. They are written
 * here from the protocol, not copied from a header, and grow only with what
 * the tree actually sends. A command is one header dword followed by `length`
 * payload dwords.
 */

#include <stdint.h>

#define VIRGL_CMD_HEADER(command, object, length) \
    ((uint32_t)(command) | ((uint32_t)(object) << 8) | ((uint32_t)(length) << 16))

/* Context commands */
#define VIRGL_CCMD_NOP 0u
#define VIRGL_CCMD_CREATE_OBJECT 1u
#define VIRGL_CCMD_BIND_OBJECT 2u
#define VIRGL_CCMD_DESTROY_OBJECT 3u
#define VIRGL_CCMD_SET_VIEWPORT_STATE 4u
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5u
#define VIRGL_CCMD_SET_VERTEX_BUFFERS 6u
#define VIRGL_CCMD_CLEAR 7u
#define VIRGL_CCMD_DRAW_VBO 8u
#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 9u
#define VIRGL_CCMD_SET_SCISSOR_STATE 15u
#define VIRGL_CCMD_BIND_SHADER 31u

/* Object types */
#define VIRGL_OBJECT_NULL 0u
#define VIRGL_OBJECT_BLEND 1u
#define VIRGL_OBJECT_RASTERIZER 2u
#define VIRGL_OBJECT_DSA 3u
#define VIRGL_OBJECT_SHADER 4u
#define VIRGL_OBJECT_VERTEX_ELEMENTS 5u
#define VIRGL_OBJECT_SAMPLER_VIEW 6u
#define VIRGL_OBJECT_SAMPLER_STATE 7u
#define VIRGL_OBJECT_SURFACE 8u

/* CREATE_OBJECT(BLEND): handle, S0, S1, S2 for each of 8 colour buffers */
#define VIRGL_OBJ_BLEND_SIZE 11u
#define VIRGL_OBJ_BLEND_S2_BLEND_ENABLE(x) (((uint32_t)(x) & 0x1u) << 0)
#define VIRGL_OBJ_BLEND_S2_RGB_FUNC(x) (((uint32_t)(x) & 0x7u) << 1)
#define VIRGL_OBJ_BLEND_S2_RGB_SRC_FACTOR(x) (((uint32_t)(x) & 0x1fu) << 4)
#define VIRGL_OBJ_BLEND_S2_RGB_DST_FACTOR(x) (((uint32_t)(x) & 0x1fu) << 9)
#define VIRGL_OBJ_BLEND_S2_ALPHA_FUNC(x) (((uint32_t)(x) & 0x7u) << 14)
#define VIRGL_OBJ_BLEND_S2_ALPHA_SRC_FACTOR(x) (((uint32_t)(x) & 0x1fu) << 17)
#define VIRGL_OBJ_BLEND_S2_ALPHA_DST_FACTOR(x) (((uint32_t)(x) & 0x1fu) << 22)
#define VIRGL_OBJ_BLEND_S2_COLORMASK(x) (((uint32_t)(x) & 0xfu) << 27)

/* CREATE_OBJECT(DSA): handle, S0, front stencil, back stencil, alpha ref */
#define VIRGL_OBJ_DSA_SIZE 5u
#define VIRGL_OBJ_DSA_S0_DEPTH_ENABLE(x) (((uint32_t)(x) & 0x1u) << 0)
#define VIRGL_OBJ_DSA_S0_DEPTH_WRITEMASK(x) (((uint32_t)(x) & 0x1u) << 1)
#define VIRGL_OBJ_DSA_S0_DEPTH_FUNC(x) (((uint32_t)(x) & 0x7u) << 2)
#define VIRGL_OBJ_DSA_S0_ALPHA_ENABLED(x) (((uint32_t)(x) & 0x1u) << 8)
#define VIRGL_OBJ_DSA_S0_ALPHA_FUNC(x) (((uint32_t)(x) & 0x7u) << 9)

/* CREATE_OBJECT(RASTERIZER): handle, S0, point size, sprite coord enable,
 * S3 (stipple and clip planes), line width, offset units, scale, clamp */
#define VIRGL_OBJ_RS_SIZE 9u
#define VIRGL_OBJ_RS_S0_DEPTH_CLIP(x) (((uint32_t)(x) & 0x1u) << 1)
#define VIRGL_OBJ_RS_S0_CULL_FACE(x) (((uint32_t)(x) & 0x3u) << 8)
#define VIRGL_OBJ_RS_S0_SCISSOR(x) (((uint32_t)(x) & 0x1u) << 14)
#define VIRGL_OBJ_RS_S0_FRONT_CCW(x) (((uint32_t)(x) & 0x1u) << 15)
#define VIRGL_OBJ_RS_S0_CLAMP_VERTEX_COLOR(x) (((uint32_t)(x) & 0x1u) << 16)
#define VIRGL_OBJ_RS_S0_CLAMP_FRAGMENT_COLOR(x) (((uint32_t)(x) & 0x1u) << 17)
#define VIRGL_OBJ_RS_S0_HALF_PIXEL_CENTER(x) (((uint32_t)(x) & 0x1u) << 29)

/* CREATE_OBJECT(SHADER): handle, type, length in bytes (bit 31: more
 * chunks follow), token count, stream-output count (0), then the TGSI text,
 * NUL-terminated and padded to a dword. */
#define VIRGL_OBJ_SHADER_HEADER_SIZE 5u

/* CREATE_OBJECT(VERTEX_ELEMENTS): handle, then per element: offset, instance
 * divisor, vertex buffer index, format */
#define VIRGL_OBJ_VERTEX_ELEMENTS_SIZE(n) (4u * (n) + 1u)

/* SET_VIEWPORT_STATE: first slot, then scale xyz and translate xyz each */
#define VIRGL_SET_VIEWPORT_STATE_SIZE(n) (6u * (n) + 1u)
/* SET_SCISSOR_STATE: first slot, then minx | miny << 16, maxx | maxy << 16 */
#define VIRGL_SET_SCISSOR_STATE_SIZE(n) (2u * (n) + 1u)
/* SET_VERTEX_BUFFERS: per buffer stride, offset, resource */
#define VIRGL_SET_VERTEX_BUFFERS_SIZE(n) (3u * (n))
/* DRAW_VBO: start, count, mode, indexed, instance count, index bias, start
 * instance, primitive restart, restart index, min index, max index,
 * count from stream output */
#define VIRGL_DRAW_VBO_SIZE 12u
/* BIND_SHADER: handle, type */
#define VIRGL_BIND_SHADER_SIZE 2u

#define PIPE_SHADER_VERTEX 0u
#define PIPE_SHADER_FRAGMENT 1u
#define PIPE_PRIM_TRIANGLES 4u

/* Comparison functions: GL's GL_NEVER..GL_ALWAYS minus 0x200 */
#define PIPE_FUNC_ALWAYS 7u
/* Faces for culling */
#define PIPE_FACE_NONE 0u
#define PIPE_FACE_FRONT 1u
#define PIPE_FACE_BACK 2u
#define PIPE_FACE_FRONT_AND_BACK 3u
#define PIPE_BLEND_ADD 0u
#define PIPE_MASK_RGBA 0xfu
/* Blend factors */
#define PIPE_BLENDFACTOR_ONE 0x01u
#define PIPE_BLENDFACTOR_SRC_COLOR 0x02u
#define PIPE_BLENDFACTOR_SRC_ALPHA 0x03u
#define PIPE_BLENDFACTOR_DST_ALPHA 0x04u
#define PIPE_BLENDFACTOR_DST_COLOR 0x05u
#define PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE 0x06u
#define PIPE_BLENDFACTOR_ZERO 0x11u
#define PIPE_BLENDFACTOR_INV_SRC_COLOR 0x12u
#define PIPE_BLENDFACTOR_INV_SRC_ALPHA 0x13u
#define PIPE_BLENDFACTOR_INV_DST_ALPHA 0x14u
#define PIPE_BLENDFACTOR_INV_DST_COLOR 0x15u

/* CREATE_OBJECT(SURFACE): handle, resource, format, level, first | last << 16 */
#define VIRGL_OBJ_SURFACE_SIZE 5u

/* SET_FRAMEBUFFER_STATE: nr_cbufs, zsurf, cbufs[nr_cbufs] */
#define VIRGL_SET_FRAMEBUFFER_STATE_SIZE(nr_cbufs) ((nr_cbufs) + 2u)

/* CLEAR: buffers, rgba as four floats, depth as a double (two dwords, low
 * first), stencil */
#define VIRGL_OBJ_CLEAR_SIZE 8u
#define PIPE_CLEAR_DEPTH (1u << 0)
#define PIPE_CLEAR_STENCIL (1u << 1)
#define PIPE_CLEAR_COLOR0 (1u << 2)

/* Resource targets (Gallium's pipe_texture_target) */
#define PIPE_BUFFER 0u
#define PIPE_TEXTURE_2D 2u

/* Formats (virgl_formats) */
#define VIRGL_FORMAT_B8G8R8A8_UNORM 1u
#define VIRGL_FORMAT_B8G8R8X8_UNORM 2u
#define VIRGL_FORMAT_Z16_UNORM 16u
#define VIRGL_FORMAT_Z32_FLOAT 18u
#define VIRGL_FORMAT_Z24_UNORM_S8_UINT 19u
#define VIRGL_FORMAT_S8_UINT_Z24_UNORM 20u
#define VIRGL_FORMAT_Z24X8_UNORM 21u
#define VIRGL_FORMAT_R32G32B32A32_FLOAT 31u
#define VIRGL_FORMAT_R8_UNORM 64u

/* Resource bind flags */
#define VIRGL_BIND_DEPTH_STENCIL (1u << 0)
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW (1u << 3)
#define VIRGL_BIND_VERTEX_BUFFER (1u << 4)
#define VIRGL_BIND_INDEX_BUFFER (1u << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1u << 6)

/* The capset (both virgl and virgl2 start with the v1 block): a version
 * dword, then four 16-dword bitmasks of the formats the host can sample,
 * render to, use as depth/stencil and fetch vertices from. Bit f of a mask is
 * dword f / 32, bit f % 32. */
#define VIRGL_CAPS_SAMPLER_MASK_DWORD 1u
#define VIRGL_CAPS_RENDER_MASK_DWORD 17u
#define VIRGL_CAPS_DEPTH_STENCIL_MASK_DWORD 33u
#define VIRGL_CAPS_VERTEX_BUFFER_MASK_DWORD 49u
#define VIRGL_CAPS_MASK_DWORDS 65u
