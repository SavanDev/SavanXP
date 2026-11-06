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
#define VIRGL_FORMAT_Z24_UNORM_S8_UINT 19u

/* Resource bind flags */
#define VIRGL_BIND_DEPTH_STENCIL (1u << 0)
#define VIRGL_BIND_RENDER_TARGET (1u << 1)
#define VIRGL_BIND_SAMPLER_VIEW (1u << 3)
#define VIRGL_BIND_VERTEX_BUFFER (1u << 4)
#define VIRGL_BIND_INDEX_BUFFER (1u << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1u << 6)
