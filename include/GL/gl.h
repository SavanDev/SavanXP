#pragma once

/* OpenGL as SxGL implements it (docs/desktop/SXGL.md,
 * docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md).
 *
 * The header a program written for OpenGL includes. It declares the types,
 * the tokens and the entry points SxGL implements, and nothing else: what is
 * not declared here is not implemented, and sxgl_get_proc_address returns
 * null for it. The context calls (creating one over a bitmap, making it
 * current, swapping) are SxGL's own, in <sxgfx/sxgl.h>.
 *
 * GL_VERSION_1_1 is not defined, because the 1.1 here is not complete yet:
 * a program that tests the macro would assume every 1.1 call exists. It is
 * defined when GL_VERSION stops saying "subset".
 */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef APIENTRY
#define APIENTRY
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY*
#endif
#ifndef GLAPI
#define GLAPI extern
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
#define GL_LIGHT4 0x4004
#define GL_LIGHT5 0x4005
#define GL_LIGHT6 0x4006
#define GL_LIGHT7 0x4007

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
#define GL_SHADE_MODEL 0x0B54
#define GL_COLOR_MATERIAL_FACE 0x0B55
#define GL_COLOR_MATERIAL_PARAMETER 0x0B56

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
#define GL_SPECULAR 0x1202
#define GL_POSITION 0x1203
#define GL_SPOT_DIRECTION 0x1204
#define GL_SPOT_EXPONENT 0x1205
#define GL_SPOT_CUTOFF 0x1206
#define GL_CONSTANT_ATTENUATION 0x1207
#define GL_LINEAR_ATTENUATION 0x1208
#define GL_QUADRATIC_ATTENUATION 0x1209
#define GL_EMISSION 0x1600
#define GL_SHININESS 0x1601
#define GL_AMBIENT_AND_DIFFUSE 0x1602
#define GL_COLOR_INDEXES 0x1603
#define GL_LIGHT_MODEL_LOCAL_VIEWER 0x0B51
#define GL_LIGHT_MODEL_TWO_SIDE 0x0B52
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

GLAPI GLenum APIENTRY glGetError(void);
GLAPI const GLubyte* APIENTRY glGetString(GLenum name);
GLAPI void APIENTRY glGetIntegerv(GLenum name, GLint* out);
GLAPI void APIENTRY glGetFloatv(GLenum name, GLfloat* out);
GLAPI void APIENTRY glFlush(void);
GLAPI void APIENTRY glFinish(void);
GLAPI void APIENTRY glHint(GLenum target, GLenum mode);

GLAPI void APIENTRY glEnable(GLenum capability);
GLAPI void APIENTRY glDisable(GLenum capability);
GLAPI GLboolean APIENTRY glIsEnabled(GLenum capability);

GLAPI void APIENTRY glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI void APIENTRY glDepthRange(GLclampd near_value, GLclampd far_value);
GLAPI void APIENTRY glScissor(GLint x, GLint y, GLsizei width, GLsizei height);
GLAPI void APIENTRY glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
GLAPI void APIENTRY glClearDepth(GLclampd depth);
GLAPI void APIENTRY glClear(GLbitfield mask);

GLAPI void APIENTRY glMatrixMode(GLenum mode);
GLAPI void APIENTRY glLoadIdentity(void);
GLAPI void APIENTRY glLoadMatrixf(const GLfloat* matrix);
GLAPI void APIENTRY glMultMatrixf(const GLfloat* matrix);
GLAPI void APIENTRY glPushMatrix(void);
GLAPI void APIENTRY glPopMatrix(void);
GLAPI void APIENTRY glTranslatef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glScalef(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glRotatef(GLfloat angle_degrees, GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value);
GLAPI void APIENTRY glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble near_value, GLdouble far_value);

GLAPI void APIENTRY glDepthFunc(GLenum function);
GLAPI void APIENTRY glDepthMask(GLboolean flag);
GLAPI void APIENTRY glCullFace(GLenum mode);
GLAPI void APIENTRY glFrontFace(GLenum mode);
GLAPI void APIENTRY glShadeModel(GLenum mode);
GLAPI void APIENTRY glAlphaFunc(GLenum function, GLclampf reference);
GLAPI void APIENTRY glBlendFunc(GLenum source_factor, GLenum destination_factor);

GLAPI void APIENTRY glLightf(GLenum light, GLenum name, GLfloat value);
GLAPI void APIENTRY glLighti(GLenum light, GLenum name, GLint value);
GLAPI void APIENTRY glLightfv(GLenum light, GLenum name, const GLfloat* values);
GLAPI void APIENTRY glLightiv(GLenum light, GLenum name, const GLint* values);
GLAPI void APIENTRY glGetLightfv(GLenum light, GLenum name, GLfloat* values);
GLAPI void APIENTRY glGetLightiv(GLenum light, GLenum name, GLint* values);
GLAPI void APIENTRY glLightModelf(GLenum name, GLfloat value);
GLAPI void APIENTRY glLightModeli(GLenum name, GLint value);
GLAPI void APIENTRY glLightModelfv(GLenum name, const GLfloat* values);
GLAPI void APIENTRY glLightModeliv(GLenum name, const GLint* values);
GLAPI void APIENTRY glMaterialf(GLenum face, GLenum name, GLfloat value);
GLAPI void APIENTRY glMateriali(GLenum face, GLenum name, GLint value);
GLAPI void APIENTRY glMaterialfv(GLenum face, GLenum name, const GLfloat* values);
GLAPI void APIENTRY glMaterialiv(GLenum face, GLenum name, const GLint* values);
GLAPI void APIENTRY glGetMaterialfv(GLenum face, GLenum name, GLfloat* values);
GLAPI void APIENTRY glGetMaterialiv(GLenum face, GLenum name, GLint* values);
GLAPI void APIENTRY glColorMaterial(GLenum face, GLenum mode);

GLAPI void APIENTRY glFogf(GLenum name, GLfloat value);
GLAPI void APIENTRY glFogi(GLenum name, GLint value);
GLAPI void APIENTRY glFogfv(GLenum name, const GLfloat* values);

GLAPI void APIENTRY glGenTextures(GLsizei count, GLuint* names);
GLAPI void APIENTRY glDeleteTextures(GLsizei count, const GLuint* names);
GLAPI void APIENTRY glBindTexture(GLenum target, GLuint name);
GLAPI void APIENTRY glTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border,
    GLenum format, GLenum type, const GLvoid* pixels);
GLAPI void APIENTRY glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
    GLenum format, GLenum type, const GLvoid* pixels);
GLAPI void APIENTRY glTexParameteri(GLenum target, GLenum name, GLint value);
GLAPI void APIENTRY glTexEnvi(GLenum target, GLenum name, GLint value);

GLAPI void APIENTRY glBegin(GLenum mode);
GLAPI void APIENTRY glEnd(void);
GLAPI void APIENTRY glVertex2f(GLfloat x, GLfloat y);
GLAPI void APIENTRY glVertex3f(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glVertex3fv(const GLfloat* vertex);
GLAPI void APIENTRY glColor3f(GLfloat red, GLfloat green, GLfloat blue);
GLAPI void APIENTRY glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha);
GLAPI void APIENTRY glColor3fv(const GLfloat* colour);
GLAPI void APIENTRY glColor4fv(const GLfloat* colour);
GLAPI void APIENTRY glColor3ub(GLubyte red, GLubyte green, GLubyte blue);
GLAPI void APIENTRY glColor4ub(GLubyte red, GLubyte green, GLubyte blue, GLubyte alpha);
GLAPI void APIENTRY glNormal3f(GLfloat x, GLfloat y, GLfloat z);
GLAPI void APIENTRY glNormal3fv(const GLfloat* normal);
GLAPI void APIENTRY glTexCoord2f(GLfloat s, GLfloat t);
GLAPI void APIENTRY glTexCoord2fv(const GLfloat* coordinates);

GLAPI void APIENTRY glEnableClientState(GLenum array);
GLAPI void APIENTRY glDisableClientState(GLenum array);
GLAPI void APIENTRY glVertexPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
GLAPI void APIENTRY glColorPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
GLAPI void APIENTRY glNormalPointer(GLenum type, GLsizei stride, const GLvoid* pointer);
GLAPI void APIENTRY glTexCoordPointer(GLint size, GLenum type, GLsizei stride, const GLvoid* pointer);
GLAPI void APIENTRY glDrawArrays(GLenum mode, GLint first, GLsizei count);
GLAPI void APIENTRY glDrawElements(GLenum mode, GLsizei count, GLenum type, const GLvoid* indices);

#ifdef __cplusplus
}
#endif
