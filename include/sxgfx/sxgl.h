#pragma once

/* SxGL: the platform's 3D rasterizer (docs/roadmaps/done/SXGL_ROADMAP.md).
 *
 * An implementation of OpenGL fixed-function, in software and on the GPU
 * under virgl, beside SxGFX and not on top of it; what it implements and what
 * it targets are in docs/desktop/SXGL.md and
 * docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md. Its only system calls are the
 * GPU backend's native device calls, which every subsystem may make, so both
 * subsystems link it: a POSIX program presents what it drew with
 * gfx_present_region, a SUN program with sxnwm_submit_rects. SxGL never
 * presents anything itself.
 *
 * The colour buffer is the caller's sx_bitmap (normally the window surface);
 * the depth buffer is the context's own allocation. This header holds SxGL's
 * own calls, the context and its WGL; the OpenGL entry points are in
 * <GL/gl.h>, which it includes. What <GL/gl.h> does not declare is not
 * implemented: there are no stubs that accept a call and do nothing. What
 * each call does is in docs/desktop/SXGL.md.
 */

#include <stdint.h>

#include "GL/gl.h"
#include "sxgfx/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- The context: SxGL's WGL ------------------------------------------------ */

struct sxgl_context;

/* A context that renders into `colour` (BGRX8888 or BGRA8888). The bitmap's
 * pixels stay the caller's; the context allocates its depth buffer at the same
 * size. The viewport and scissor start as the whole bitmap. NULL when out of
 * memory or the bitmap is unusable. It draws on the GPU when the machine has a
 * 3D device and in software otherwise: either way the image is in `colour`
 * after sxgl_swap_buffers or glFinish, and only then. */
struct sxgl_context* sxgl_context_create(const struct sx_bitmap* colour);

/* The same, choosing the backend (docs/roadmaps/open/VIRGL_ROADMAP.md).
 * 0 or SXGL_CONTEXT_GPU take the virgl backend when there is one: the host
 * GPU rasterizes and sxgl_swap_buffers (or glFinish) copies the result into
 * `colour`. Without a 3D device, or if it fails later, the context rasterizes
 * in software. SXGL_CONTEXT_SOFTWARE never touches the GPU. */
#define SXGL_CONTEXT_SOFTWARE 0x1u
#define SXGL_CONTEXT_GPU 0x2u
struct sxgl_context* sxgl_context_create_ex(const struct sx_bitmap* colour, unsigned flags);

/* Which backend the context is drawing with right now. */
#define SXGL_BACKEND_SOFTWARE 0
#define SXGL_BACKEND_GPU 1
int sxgl_context_backend(const struct sxgl_context* context);
/* Retargets the context after the window was resized: new colour bitmap, depth
 * buffer reallocated (and cleared to 1.0), viewport and scissor reset to the
 * whole bitmap. Call it between frames, never between glBegin and glEnd. 0, or
 * -1 when out of memory (the context then keeps its previous size). */
int sxgl_context_resize(struct sxgl_context* context, const struct sx_bitmap* colour);
void sxgl_context_destroy(struct sxgl_context* context);
void sxgl_make_current(struct sxgl_context* context);
struct sxgl_context* sxgl_get_current(void);

/* SwapBuffers without the present: returns in *damage the part of the colour
 * buffer written since the previous swap (empty if nothing), and starts a new
 * frame. The caller presents that rectangle through its own subsystem. */
void sxgl_swap_buffers(struct sxgl_context* context, struct sx_rect* damage);

/* The debug layer: every error a call raises is also reported here, at the call
 * and by name ("glTexImage2D: GL_INVALID_VALUE (width is not a power of two)").
 * glGetError keeps its sticky semantics regardless. NULL turns it off. */
typedef void (*sxgl_debug_sink)(const char* message, void* user);
void sxgl_set_debug_sink(struct sxgl_context* context, sxgl_debug_sink sink, void* user);

/* Counters for the triangles-per-second figure the roadmap asks for. */
struct sxgl_stats {
    uint64_t triangles_submitted; /* assembled from vertices */
    uint64_t triangles_rasterized; /* survived culling and clipping */
    uint64_t fragments_written; /* passed every test and reached the buffer */
};
void sxgl_get_stats(const struct sxgl_context* context, struct sxgl_stats* out);
void sxgl_reset_stats(struct sxgl_context* context);

/* An OpenGL entry point by name, as wglGetProcAddress returns one: every
 * function <GL/gl.h> declares, and those of each extension in GL_EXTENSIONS.
 * NULL for a name SxGL does not implement, so a program can tell an absent
 * call from a present one. It needs no current context. */
typedef void (*sxgl_proc)(void);
sxgl_proc sxgl_get_proc_address(const char* name);

#ifdef __cplusplus
}
#endif
