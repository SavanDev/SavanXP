#pragma once

/* The OpenGL extensions SxGL implements, by their registered names
 * (docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md).
 *
 * Each extension here is also in GL_EXTENSIONS, and its entry points, if it
 * has any, resolve through sxgl_get_proc_address. Both come from one table in
 * system/gfx/sxgl_procs.c. An extension is listed here only once it works on
 * both backends.
 */

#include "GL/gl.h"

/* GL_EXT_bgra: the pixel order of an sx_bitmap, so uploading one is a copy. */
#ifndef GL_EXT_bgra
#define GL_EXT_bgra 1
#define GL_BGRA_EXT 0x80E1
#endif
