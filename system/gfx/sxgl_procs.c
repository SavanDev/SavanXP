/* The one table of what SxGL implements (docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md).
 *
 * Every OpenGL entry point SxGL exports is registered here, grouped by where
 * it comes from: the core version, then one group per extension. The same
 * table answers sxgl_get_proc_address and builds GL_EXTENSIONS, so an
 * extension cannot be advertised without its entry points or the other way
 * round. The host test checks that the core group is exactly what <GL/gl.h>
 * declares.
 */

#include "sxgfx/sxgl.h"

#include "sxgl_procs.h"

#include <stddef.h>
#include <string.h>

struct sxgl_proc_entry {
    const char* name;
    sxgl_proc proc;
};

struct sxgl_extension_entry {
    const char* name; /* NULL for the core version */
    const struct sxgl_proc_entry* procs;
    size_t count;
};

#define SXGL_PROC(function) {#function, (sxgl_proc)function}

static const struct sxgl_proc_entry g_core_procs[] = {
    SXGL_PROC(glGetError),
    SXGL_PROC(glGetString),
    SXGL_PROC(glGetIntegerv),
    SXGL_PROC(glGetFloatv),
    SXGL_PROC(glFlush),
    SXGL_PROC(glFinish),
    SXGL_PROC(glHint),
    SXGL_PROC(glEnable),
    SXGL_PROC(glDisable),
    SXGL_PROC(glIsEnabled),
    SXGL_PROC(glViewport),
    SXGL_PROC(glDepthRange),
    SXGL_PROC(glScissor),
    SXGL_PROC(glClearColor),
    SXGL_PROC(glClearDepth),
    SXGL_PROC(glClear),
    SXGL_PROC(glMatrixMode),
    SXGL_PROC(glLoadIdentity),
    SXGL_PROC(glLoadMatrixf),
    SXGL_PROC(glMultMatrixf),
    SXGL_PROC(glPushMatrix),
    SXGL_PROC(glPopMatrix),
    SXGL_PROC(glTranslatef),
    SXGL_PROC(glScalef),
    SXGL_PROC(glRotatef),
    SXGL_PROC(glFrustum),
    SXGL_PROC(glOrtho),
    SXGL_PROC(glDepthFunc),
    SXGL_PROC(glDepthMask),
    SXGL_PROC(glCullFace),
    SXGL_PROC(glFrontFace),
    SXGL_PROC(glShadeModel),
    SXGL_PROC(glAlphaFunc),
    SXGL_PROC(glBlendFunc),
    SXGL_PROC(glLightf),
    SXGL_PROC(glLightfv),
    SXGL_PROC(glLightModelfv),
    SXGL_PROC(glMaterialfv),
    SXGL_PROC(glColorMaterial),
    SXGL_PROC(glFogf),
    SXGL_PROC(glFogi),
    SXGL_PROC(glFogfv),
    SXGL_PROC(glGenTextures),
    SXGL_PROC(glDeleteTextures),
    SXGL_PROC(glBindTexture),
    SXGL_PROC(glTexImage2D),
    SXGL_PROC(glTexSubImage2D),
    SXGL_PROC(glTexParameteri),
    SXGL_PROC(glTexEnvi),
    SXGL_PROC(glBegin),
    SXGL_PROC(glEnd),
    SXGL_PROC(glVertex2f),
    SXGL_PROC(glVertex3f),
    SXGL_PROC(glVertex3fv),
    SXGL_PROC(glColor3f),
    SXGL_PROC(glColor4f),
    SXGL_PROC(glColor3fv),
    SXGL_PROC(glColor4fv),
    SXGL_PROC(glColor3ub),
    SXGL_PROC(glColor4ub),
    SXGL_PROC(glNormal3f),
    SXGL_PROC(glNormal3fv),
    SXGL_PROC(glTexCoord2f),
    SXGL_PROC(glTexCoord2fv),
    SXGL_PROC(glEnableClientState),
    SXGL_PROC(glDisableClientState),
    SXGL_PROC(glVertexPointer),
    SXGL_PROC(glColorPointer),
    SXGL_PROC(glNormalPointer),
    SXGL_PROC(glTexCoordPointer),
    SXGL_PROC(glDrawArrays),
    SXGL_PROC(glDrawElements),
};

/* GL_EXT_bgra is a pixel format: it adds a token and no entry point. */
static const struct sxgl_extension_entry g_extensions[] = {
    {NULL, g_core_procs, sizeof(g_core_procs) / sizeof(g_core_procs[0])},
    {"GL_EXT_bgra", NULL, 0},
};

#define SXGL_EXTENSION_COUNT (sizeof(g_extensions) / sizeof(g_extensions[0]))

sxgl_proc sxgl_get_proc_address(const char* name)
{
    size_t extension;
    size_t index;

    if (name == NULL) {
        return NULL;
    }
    for (extension = 0; extension < SXGL_EXTENSION_COUNT; ++extension) {
        const struct sxgl_extension_entry* entry = &g_extensions[extension];
        for (index = 0; index < entry->count; ++index) {
            if (strcmp(entry->procs[index].name, name) == 0) {
                return entry->procs[index].proc;
            }
        }
    }
    return NULL;
}

/* Built on first use. SxGL is single-threaded, and the string never changes
 * once built. Its size is checked against the table by the host test. */
static char g_extension_string[512];

const char* sxgl_extension_string(void)
{
    size_t extension;
    size_t used = 0;

    if (g_extension_string[0] != '\0') {
        return g_extension_string;
    }
    for (extension = 0; extension < SXGL_EXTENSION_COUNT; ++extension) {
        const char* name = g_extensions[extension].name;
        size_t length;

        if (name == NULL) {
            continue;
        }
        length = strlen(name);
        if (used + (used != 0 ? 1u : 0u) + length + 1u > sizeof(g_extension_string)) {
            break;
        }
        if (used != 0) {
            g_extension_string[used++] = ' ';
        }
        memcpy(g_extension_string + used, name, length);
        used += length;
    }
    g_extension_string[used] = '\0';
    return g_extension_string;
}
