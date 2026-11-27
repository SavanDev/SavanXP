/* A program written for OpenGL that maps SxGL as a library: the done
 * criterion of step 0 of docs/roadmaps/open/SXGL_VERSIONS_ROADMAP.md.
 *
 *     ./tools/build-user.sh --source sdk/glsmoke --name glsmoke --pie
 *
 * It includes <GL/gl.h>, opens /lib/libsxgl.so.0.4 with dlopen, and reaches
 * every call it draws with through the library's sxgl_get_proc_address. The
 * same scene is drawn a second time through the SxGL linked into the program
 * (--pie implies --sse, which brings the static runtime's gl group): the twin
 * the library's frame is compared with, pixel for pixel. The library is
 * opened without RTLD_GLOBAL, so its relocations resolve its own SxGL before
 * the program's, and the two copies keep separate contexts.
 *
 * It is a POSIX program because dlopen is POSIX's (<dlfcn.h>); the SUN side,
 * a native program mapping libsxgl by DT_NEEDED, is sunlibtest.
 *
 * Run headless from init: ./build.sh smoke gl-smoke. init prints
 * "GL SMOKE PASS" when it exits with 0.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#include <GL/gl.h>
#include <GL/glext.h>
#include <sxgfx/sxgl.h>

#define WIDTH 64
#define HEIGHT 48

static int g_failures = 0;

static void report(const char* what, int ok) {
    printf("GLSMOKE: %s: %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) {
        g_failures += 1;
    }
}

/* What the scene draws with, so one function draws it through either copy. */
struct gl_api {
    struct sxgl_context* (*context_create_ex)(const struct sx_bitmap*, unsigned);
    void (*make_current)(struct sxgl_context*);
    void (*context_destroy)(struct sxgl_context*);
    sxgl_proc (*get_proc_address)(const char*);
    void (APIENTRYP Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (APIENTRYP ClearColor)(GLclampf, GLclampf, GLclampf, GLclampf);
    void (APIENTRYP Clear)(GLbitfield);
    void (APIENTRYP Enable)(GLenum);
    void (APIENTRYP MatrixMode)(GLenum);
    void (APIENTRYP LoadIdentity)(void);
    void (APIENTRYP Frustum)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
    void (APIENTRYP Translatef)(GLfloat, GLfloat, GLfloat);
    void (APIENTRYP Rotatef)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (APIENTRYP Fogi)(GLenum, GLint);
    void (APIENTRYP Fogf)(GLenum, GLfloat);
    void (APIENTRYP Fogfv)(GLenum, const GLfloat*);
    void (APIENTRYP GenTextures)(GLsizei, GLuint*);
    void (APIENTRYP BindTexture)(GLenum, GLuint);
    void (APIENTRYP TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
    void (APIENTRYP TexParameteri)(GLenum, GLenum, GLint);
    void (APIENTRYP Disable)(GLenum);
    void (APIENTRYP Begin)(GLenum);
    void (APIENTRYP End)(void);
    void (APIENTRYP Color3f)(GLfloat, GLfloat, GLfloat);
    void (APIENTRYP TexCoord2f)(GLfloat, GLfloat);
    void (APIENTRYP Vertex3f)(GLfloat, GLfloat, GLfloat);
    void (APIENTRYP Finish)(void);
    GLenum (APIENTRYP GetError)(void);
    const GLubyte* (APIENTRYP GetString)(GLenum);
};

/* The calls the scene uses, by name: the program resolves them the way a
 * renderer written elsewhere does, and each lookup that fails is reported. */
static int resolve(struct gl_api* api) {
    int missing = 0;
#define RESOLVE(field, name)                                       \
    do {                                                           \
        *(sxgl_proc*)&api->field = api->get_proc_address(name);    \
        if (api->field == 0) {                                     \
            printf("GLSMOKE: %s does not resolve\n", name);        \
            missing += 1;                                          \
        }                                                          \
    } while (0)
    RESOLVE(Viewport, "glViewport");
    RESOLVE(ClearColor, "glClearColor");
    RESOLVE(Clear, "glClear");
    RESOLVE(Enable, "glEnable");
    RESOLVE(MatrixMode, "glMatrixMode");
    RESOLVE(LoadIdentity, "glLoadIdentity");
    RESOLVE(Frustum, "glFrustum");
    RESOLVE(Translatef, "glTranslatef");
    RESOLVE(Rotatef, "glRotatef");
    RESOLVE(Fogi, "glFogi");
    RESOLVE(Fogf, "glFogf");
    RESOLVE(Fogfv, "glFogfv");
    RESOLVE(GenTextures, "glGenTextures");
    RESOLVE(BindTexture, "glBindTexture");
    RESOLVE(TexImage2D, "glTexImage2D");
    RESOLVE(TexParameteri, "glTexParameteri");
    RESOLVE(Disable, "glDisable");
    RESOLVE(Begin, "glBegin");
    RESOLVE(End, "glEnd");
    RESOLVE(Color3f, "glColor3f");
    RESOLVE(TexCoord2f, "glTexCoord2f");
    RESOLVE(Vertex3f, "glVertex3f");
    RESOLVE(Finish, "glFinish");
    RESOLVE(GetError, "glGetError");
    RESOLVE(GetString, "glGetString");
#undef RESOLVE
    return missing;
}

/* Depth, Gouraud colour, a BGRA texture and linear fog: enough of the
 * pipeline that a copy compiled differently would show it. */
static GLenum draw_scene(const struct gl_api* api) {
    static const GLfloat fog_colour[4] = {0.25f, 0.25f, 0.3f, 1.0f};
    uint32_t texels[8 * 8];
    GLuint texture = 0;
    int index;

    for (index = 0; index < 8 * 8; ++index) {
        texels[index] = ((index / 8 + index % 8) & 1) != 0 ? 0xFFE0C040u : 0xFF2050A0u;
    }
    api->Viewport(0, 0, WIDTH, HEIGHT);
    api->ClearColor(0.1f, 0.1f, 0.15f, 1.0f);
    api->Enable(GL_DEPTH_TEST);
    api->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    api->MatrixMode(GL_PROJECTION);
    api->LoadIdentity();
    api->Frustum(-1.0, 1.0, -0.75, 0.75, 1.0, 20.0);
    api->MatrixMode(GL_MODELVIEW);
    api->LoadIdentity();
    api->Translatef(0.0f, 0.0f, -4.0f);
    api->Rotatef(30.0f, 0.3f, 1.0f, 0.0f);
    api->Enable(GL_FOG);
    api->Fogi(GL_FOG_MODE, GL_LINEAR);
    api->Fogf(GL_FOG_START, 3.0f);
    api->Fogf(GL_FOG_END, 6.0f);
    api->Fogfv(GL_FOG_COLOR, fog_colour);

    api->Begin(GL_TRIANGLES);
    api->Color3f(1.0f, 0.0f, 0.0f);
    api->Vertex3f(-1.5f, -1.0f, 0.5f);
    api->Color3f(0.0f, 1.0f, 0.0f);
    api->Vertex3f(1.5f, -1.0f, -0.5f);
    api->Color3f(0.0f, 0.0f, 1.0f);
    api->Vertex3f(0.0f, 1.2f, 0.0f);
    api->End();

    api->GenTextures(1, &texture);
    api->BindTexture(GL_TEXTURE_2D, texture);
    api->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    api->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    api->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, texels);
    api->Enable(GL_TEXTURE_2D);
    api->Begin(GL_QUADS);
    api->Color3f(1.0f, 1.0f, 1.0f);
    api->TexCoord2f(0.0f, 0.0f);
    api->Vertex3f(-1.0f, -0.8f, -1.0f);
    api->TexCoord2f(2.0f, 0.0f);
    api->Vertex3f(1.0f, -0.8f, -1.0f);
    api->TexCoord2f(2.0f, 2.0f);
    api->Vertex3f(1.0f, 0.8f, 1.0f);
    api->TexCoord2f(0.0f, 2.0f);
    api->Vertex3f(-1.0f, 0.8f, 1.0f);
    api->End();
    api->Disable(GL_TEXTURE_2D);
    api->Finish();
    return api->GetError();
}

/* Draws the scene into `bitmap` with a software context of `api`'s copy. */
static int render(const struct gl_api* api, struct sx_bitmap* bitmap) {
    struct sxgl_context* context = api->context_create_ex(bitmap, SXGL_CONTEXT_SOFTWARE);
    GLenum error;

    if (context == 0) {
        return 0;
    }
    api->make_current(context);
    error = draw_scene(api);
    api->make_current(0);
    api->context_destroy(context);
    return error == GL_NO_ERROR;
}

static int distinct_colours(const struct sx_bitmap* bitmap) {
    uint32_t seen[8];
    int count = 0;
    int index;
    int known;

    for (index = 0; index < WIDTH * HEIGHT && count < 8; ++index) {
        uint32_t pixel = bitmap->pixels[index] & 0x00FFFFFFu;
        for (known = 0; known < count && seen[known] != pixel; ++known) {
        }
        if (known == count) {
            seen[count++] = pixel;
        }
    }
    return count;
}

/* Whether dladdr places `address` in an image whose name contains `name`. */
static int lives_in(const void* address, const char* name) {
    Dl_info info;

    if (dladdr(address, &info) == 0 || info.dli_fname == 0) {
        return 0;
    }
    return strstr(info.dli_fname, name) != 0;
}

int main(void) {
    struct gl_api shared;
    struct gl_api linked;
    struct sx_bitmap from_library;
    struct sx_bitmap from_program;
    void* image;

    memset(&shared, 0, sizeof(shared));
    memset(&linked, 0, sizeof(linked));

    image = dlopen("libsxgl.so.0.4", RTLD_NOW);
    report("dlopen libsxgl.so.0.4", image != 0);
    if (image == 0) {
        printf("GLSMOKE: dlerror: %s\n", dlerror());
        printf("GLSMOKE: %d failures\n", g_failures);
        return 1;
    }
    *(void**)&shared.context_create_ex = dlsym(image, "sxgl_context_create_ex");
    *(void**)&shared.make_current = dlsym(image, "sxgl_make_current");
    *(void**)&shared.context_destroy = dlsym(image, "sxgl_context_destroy");
    *(void**)&shared.get_proc_address = dlsym(image, "sxgl_get_proc_address");
    report("dlsym the context calls and sxgl_get_proc_address",
           shared.context_create_ex != 0 && shared.make_current != 0 && shared.context_destroy != 0 &&
               shared.get_proc_address != 0);
    if (shared.get_proc_address == 0 || shared.context_create_ex == 0 || shared.make_current == 0 ||
        shared.context_destroy == 0) {
        printf("GLSMOKE: %d failures\n", g_failures);
        return 1;
    }
    report("every call the scene draws with resolves by name", resolve(&shared) == 0);
    report("glClear resolves inside libsxgl.so.0.4", lives_in((const void*)shared.Clear, "libsxgl"));
    report("the library's glClear is not the program's", (const void*)shared.Clear != (const void*)glClear);

    /* Absent is absent: a GL 1.1 call SxGL does not implement yet, and a
     * name that is no call at all. */
    report("glPointSize is absent", shared.get_proc_address("glPointSize") == 0);
    report("glNotACall is absent", shared.get_proc_address("glNotACall") == 0);

    linked.context_create_ex = sxgl_context_create_ex;
    linked.make_current = sxgl_make_current;
    linked.context_destroy = sxgl_context_destroy;
    linked.get_proc_address = sxgl_get_proc_address;
    report("the program's own SxGL resolves the same calls", resolve(&linked) == 0);
    report("the program's glClear is its own", linked.Clear == glClear);

    if (!sx_bitmap_create(&from_library, WIDTH, HEIGHT, SX_PIXEL_FORMAT_BGRX8888) ||
        !sx_bitmap_create(&from_program, WIDTH, HEIGHT, SX_PIXEL_FORMAT_BGRX8888)) {
        report("two bitmaps", 0);
        return 1;
    }
    report("the scene draws through libsxgl.so.0.4 with no error", render(&shared, &from_library));
    report("the scene draws through the program's SxGL with no error", render(&linked, &from_program));
    report("the frame is a scene, not a clear (at least 4 colours)", distinct_colours(&from_library) >= 4);
    report("the library's frame matches the linked twin pixel for pixel",
           memcmp(from_library.pixels, from_program.pixels, (size_t)WIDTH * HEIGHT * 4u) == 0);

    {
        struct sxgl_context* context = shared.context_create_ex(&from_library, SXGL_CONTEXT_SOFTWARE);
        const char* version;
        const char* extensions;

        shared.make_current(context);
        version = (const char*)shared.GetString(GL_VERSION);
        extensions = (const char*)shared.GetString(GL_EXTENSIONS);
        report("GL_VERSION from the library", version != 0 && strcmp(version, "1.1 SxGL subset") == 0);
        report("GL_EXTENSIONS lists GL_EXT_bgra", extensions != 0 && strstr(extensions, "GL_EXT_bgra") != 0);
        shared.make_current(0);
        shared.context_destroy(context);
    }

    sx_bitmap_destroy(&from_program);
    sx_bitmap_destroy(&from_library);

    if (g_failures == 0) {
        printf("GLSMOKE: all ok\n");
        return 0;
    }
    printf("GLSMOKE: %d failures\n", g_failures);
    return 1;
}
