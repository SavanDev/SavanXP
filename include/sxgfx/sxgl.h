#pragma once

/* SxGL: the platform's 3D rasterizer (docs/desktop/SXGL_ROADMAP.md).
 *
 * A software implementation of a documented subset of OpenGL 1.1
 * fixed-function, beside SxGFX and not on top of it. Like SxGFX it makes no
 * system call, so both subsystems link it: a POSIX program presents what it
 * drew with gfx_present_region, a SUN program with sxnwm_submit_rects. SxGL
 * never presents anything itself.
 *
 * The colour buffer is the caller's sx_bitmap (normally the window surface);
 * the depth buffer is the context's own allocation. The entry points below are
 * the whole subset. What is not declared here is not implemented: there are no
 * stubs that accept a call and do nothing. The subset and what each call does
 * are listed in docs/desktop/SXGL.md.
 */

#include <stdint.h>

#include "sxgfx/gfx2d.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef void GLvoid;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef int GLsizei;
typedef float GLfloat;
typedef float GLclampf;
typedef double GLdouble;
typedef double GLclampd;

#define GL_FALSE 0
#define GL_TRUE 1

/* Errors */
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_STACK_OVERFLOW 0x0503
#define GL_STACK_UNDERFLOW 0x0504
#define GL_OUT_OF_MEMORY 0x0505

/* Primitives (no points or lines in the subset) */
#define GL_TRIANGLES 0x0004
#define GL_TRIANGLE_STRIP 0x0005
#define GL_TRIANGLE_FAN 0x0006
#define GL_QUADS 0x0007
#define GL_QUAD_STRIP 0x0008
#define GL_POLYGON 0x0009

/* glClear */
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000

/* Comparison functions (depth and alpha test) */
#define GL_NEVER 0x0200
#define GL_LESS 0x0201
#define GL_EQUAL 0x0202
#define GL_LEQUAL 0x0203
#define GL_GREATER 0x0204
#define GL_NOTEQUAL 0x0205
#define GL_GEQUAL 0x0206
#define GL_ALWAYS 0x0207

/* Blend factors */
#define GL_ZERO 0
#define GL_ONE 1
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307

/* Faces and winding */
#define GL_FRONT 0x0404
#define GL_BACK 0x0405
#define GL_FRONT_AND_BACK 0x0408
#define GL_CW 0x0900
#define GL_CCW 0x0901

/* Capabilities (glEnable/glDisable/glIsEnabled) */
#define GL_CULL_FACE 0x0B44
#define GL_LIGHTING 0x0B50
#define GL_COLOR_MATERIAL 0x0B57
#define GL_FOG 0x0B60
#define GL_DEPTH_TEST 0x0B71
#define GL_NORMALIZE 0x0BA1
#define GL_ALPHA_TEST 0x0BC0
#define GL_BLEND 0x0BE2
#define GL_SCISSOR_TEST 0x0C11
#define GL_TEXTURE_2D 0x0DE1
#define GL_LIGHT0 0x4000
#define GL_LIGHT1 0x4001
#define GL_LIGHT2 0x4002
#define GL_LIGHT3 0x4003

/* Queries (glGetIntegerv/glGetFloatv) */
#define GL_VIEWPORT 0x0BA2
#define GL_MATRIX_MODE 0x0BA0
#define GL_MODELVIEW_MATRIX 0x0BA6
#define GL_PROJECTION_MATRIX 0x0BA7
#define GL_MAX_TEXTURE_SIZE 0x0D33
#define GL_MAX_MODELVIEW_STACK_DEPTH 0x0D36
#define GL_MAX_PROJECTION_STACK_DEPTH 0x0D38
#define GL_MAX_LIGHTS 0x0D31
#define GL_TEXTURE_BINDING_2D 0x8069

/* glGetString */
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_EXTENSIONS 0x1F03

/* Matrices */
#define GL_MODELVIEW 0x1700
#define GL_PROJECTION 0x1701

/* Shading */
#define GL_FLAT 0x1D00
#define GL_SMOOTH 0x1D01

/* Lights and materials */
#define GL_AMBIENT 0x1200
#define GL_DIFFUSE 0x1201
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION 0x1600
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_LIGHT_MODEL_AMBIENT 0x0B53

/* Fog */
#define GL_FOG_DENSITY 0x0B62
#define GL_FOG_START 0x0B63
#define GL_FOG_END 0x0B64
#define GL_FOG_MODE 0x0B65
#define GL_FOG_COLOR 0x0B66
#define GL_EXP 0x0800
#define GL_EXP2 0x0801
#define GL_LINEAR 0x2601

/* Data types */
#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406

/* Pixel formats for glTexImage2D */
#define GL_ALPHA 0x1906
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_LUMINANCE 0x1909
#define GL_BGRA 0x80E1 /* GL 1.2 / EXT_bgra: what an sx_bitmap already is */

/* Textures */
#define GL_NEAREST 0x2600
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_CLAMP 0x2900
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_DECAL 0x2101
#define GL_REPLACE 0x1E01

/* Client arrays */
#define GL_VERTEX_ARRAY 0x8074
#define GL_NORMAL_ARRAY 0x8075
#define GL_COLOR_ARRAY 0x8076
#define GL_TEXTURE_COORD_ARRAY 0x8078

/* Hints are accepted and, as the specification allows, ignored. */
#define GL_PERSPECTIVE_CORRECTION_HINT 0x0C50
#define GL_FOG_HINT 0x0C54
#define GL_DONT_CARE 0x1100
#define GL_FASTEST 0x1101
#define GL_NICEST 0x1102

/* --- The context: SxGL's WGL ------------------------------------------------ */

struct sxgl_context;

/* A context that renders into `colour` (BGRX8888 or BGRA8888). The bitmap's
 * pixels stay the caller's; the context allocates its depth buffer at the same
 * size. The viewport and scissor start as the whole bitmap. NULL when out of
 * memory or the bitmap is unusable. */
struct sxgl_context* sxgl_context_create(const struct sx_bitmap* colour);
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

/* --- The GL 1.1 subset ------------------------------------------------------ */

GLenum glGetError(void);
const GLubyte* glGetString(GLenum name);
void glGetIntegerv(GLenum name, GLint* out);
void glGetFloatv(GLenum name, GLfloat* out);
void glFlush(void);
void glFinish(void);
void glHint(GLenum target, GLenum mode);

void glEnable(GLenum capability);
void glDisable(GLenum capability);
GLboolean glIsEnabled(GLenum capability);

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
void glDepthRange(GLclampd near_value, GLclampd far_value);
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
void glClearDepth(GLclampd depth);
void glClear(GLbitfield mask);

void glMatrixMode(GLenum mode);
void glLoadIdentity(void);
void glLoadMatrixf(const GLfloat* matrix);
void glMultMatrixf(const GLfloat* matrix);
void glPushMatrix(void);
void glPopMatrix(void);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glRotatef(GLfloat angle_degrees, GLfloat x, GLfloat y, GLfloat z);
void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value);
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value);

void glDepthFunc(GLenum function);
void glDepthMask(GLboolean flag);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glAlphaFunc(GLenum function, GLclampf reference);
void glBlendFunc(GLenum source_factor, GLenum destination_factor);

void glLightf(GLenum light, GLenum name, GLfloat value);
void glLightfv(GLenum light, GLenum name, const GLfloat* values);
void glLightModelfv(GLenum name, const GLfloat* values);
void glMaterialfv(GLenum face, GLenum name, const GLfloat* values);
void glColorMaterial(GLenum face, GLenum mode);

void glFogf(GLenum name, GLfloat value);
void glFogi(GLenum name, GLint value);
void glFogfv(GLenum name, const GLfloat* values);

void glGenTextures(GLsizei count, GLuint* names);
void glDeleteTextures(GLsizei count, const GLuint* names);
void glBindTexture(GLenum target, GLuint name);
void glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const GLvoid* pixels);
void glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const GLvoid* pixels);
void glTexParameteri(GLenum target, GLenum name, GLint value);
void glTexEnvi(GLenum target, GLenum name, GLint value);

void glBegin(GLenum mode);
void glEnd(void);
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glVertex3fv(const GLfloat* vertex);
void glColor3f(GLfloat red, GLfloat green, GLfloat blue);
void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
void glColor3fv(const GLfloat* colour);
void glColor4fv(const GLfloat* colour);
void glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
void glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glNormal3fv(const GLfloat* normal);
void glTexCoord2f(GLfloat s, GLfloat t);
void glTexCoord2fv(const GLfloat* coordinates);

void glEnableClientState(GLenum array);
void glDisableClientState(GLenum array);
void glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void glNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer);
void glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
void glDrawArrays(GLenum mode, GLint first, GLsizei count);
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices);

#ifdef __cplusplus
}
#endif
