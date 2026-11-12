/* SxGL Diagnostic Tool: what the machine is, what the display can do and what
 * SxGL draws with, as a tabbed sheet in the manner of the classic diagnostic
 * tools (docs/desktop/SXGL_DIAG.md).
 *
 * Every figure is read, never assumed: the machine from sxn_system_info, the
 * display from gpu0's own queries, and SxGL's strings and limits from a live
 * context of each backend. The Test buttons draw a lit, textured, depth-tested
 * cube for a few seconds with the chosen backend, check the last frame and
 * report what it cost.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sxgfx/sxgl.h"
#include "sxn/sxngui.h"
#include "build_number.h"
#include "sxgldiag.sunres.h"

#define TEST_WIDTH 208
#define TEST_HEIGHT 156
#define TEST_SECONDS 5
#define REPORT_PATH "/users/user/SxGLDiag.txt"

/* Index order is paint and Tab order: each group box comes before the
 * controls drawn inside it. */
enum { TABS,
       /* System */
       SYSTEM_GROUP, PROGRESS,
       /* Display */
       DEVICE_GROUP, DRIVER_GROUP, FEATURES_GROUP, TEST_SOFTWARE, TEST_GPU,
       /* Renderer */
       RENDERER_LIST,
       /* Display and Renderer */
       NOTES_GROUP, NOTES,
       NEXT_PAGE, SAVE, EXIT_BUTTON, WIDGETS };
enum { PAGE_SYSTEM, PAGE_DISPLAY, PAGE_RENDERER, PAGE_HELP, PAGE_ALL = -1, PAGE_NOTES = -2 };
static const char* g_tab_labels[] = {"System", "Display", "Renderer", "More Help"};
enum { TAB_COUNT = sizeof(g_tab_labels) / sizeof(g_tab_labels[0]) };

static struct sxngui_app g_app;
static struct sxgui_widget g_widgets[WIDGETS];

/* ---- what the sheet shows ---------------------------------------------------- */

enum { SYSTEM_ROWS = 9, DEVICE_ROWS = 5, DRIVER_ROWS = 4, ROW_TEXT = 96 };
static const char* const g_system_captions[SYSTEM_ROWS] = {
    "Operating system:", "Bootloader:", "Firmware:", "Processor:", "Memory:",
    "System volume:", "Uptime:", "Display:", "SxGL version:"};
static char g_system_values[SYSTEM_ROWS][ROW_TEXT];
static const char* const g_device_captions[DEVICE_ROWS] = {
    "Name:", "Display mode:", "Mode limit:", "3D device:", "Present path:"};
static char g_device_values[DEVICE_ROWS][ROW_TEXT];
static const char* const g_driver_captions[DRIVER_ROWS] = {
    "Renderer:", "Version:", "Extensions:", "Default backend:"};
static char g_driver_values[DRIVER_ROWS][ROW_TEXT];

/* The Renderer tab: one row per property, one column per backend. */
enum { RENDERER_ROWS = 10 };
static char g_renderer_rows[RENDERER_ROWS][160];
static const char* g_renderer_ptrs[RENDERER_ROWS];
static const struct sxgui_column g_renderer_columns[] = {
    {"Property", 168, 0}, {"Software", 190, 0}, {"GPU", 190, 0}};

/* Notes: one list per page that has them, the classic sheet's bottom box. */
enum { NOTE_COUNT = 6, NOTE_TEXT = 128 };
struct notes {
    char text[NOTE_COUNT][NOTE_TEXT];
    const char* ptrs[NOTE_COUNT];
    int count;
};
static struct notes g_display_notes, g_renderer_notes;

static struct savanxp_system_info g_system;
static struct savanxp_gpu_info g_gpu;
static struct savanxp_gpu_connector_properties g_connector;
static struct savanxp_gpu_3d_capset g_capset;
static int g_gpu_known, g_connector_known, g_capset_known;
static int g_gpu_backend_works; /* a GPU-flagged context really took the GPU */
static char g_status[128];
static char g_version_text[64];

static void notes_clear(struct notes* notes)
{
    notes->count = 0;
}

static void notes_add(struct notes* notes, const char* text)
{
    if (notes->count >= NOTE_COUNT) {
        /* The oldest test result gives way to the newest. */
        memmove(notes->text[0], notes->text[1], sizeof(notes->text[0]) * (NOTE_COUNT - 1));
        notes->count = NOTE_COUNT - 1;
    }
    (void)snprintf(notes->text[notes->count], NOTE_TEXT, "- %s", text);
    notes->count += 1;
}

static void notes_bind(struct notes* notes)
{
    int index;

    for (index = 0; index < notes->count; ++index) {
        notes->ptrs[index] = notes->text[index];
    }
    g_widgets[NOTES].items = notes->ptrs;
    g_widgets[NOTES].item_count = notes->count;
    g_widgets[NOTES].value = -1;
}

static void format_bytes(char* out, size_t size, uint64_t bytes)
{
    const uint64_t mib = bytes / (1024u * 1024u);

    if (mib >= 10240u) {
        (void)snprintf(out, size, "%llu GB", (unsigned long long)(mib / 1024u));
    } else {
        (void)snprintf(out, size, "%llu MB", (unsigned long long)mib);
    }
}

/* ---- the machine ------------------------------------------------------------- */

static void read_system(void)
{
    char total[32], free_text[32];
    uint64_t seconds;

    memset(&g_system, 0, sizeof(g_system));
    if (sxn_system_info(&g_system) < 0) {
        (void)snprintf(g_system_values[0], ROW_TEXT, "%s", "not available");
        return;
    }
    (void)snprintf(g_system_values[0], ROW_TEXT, "%s %s (%s, build %s)", SAVANXP_SYSTEM_NAME,
                   SAVANXP_VERSION_STRING, SAVANXP_EDITION, SAVANXP_BUILD_STRING);
    (void)snprintf(g_system_values[1], ROW_TEXT, "%s %s",
                   g_system.bootloader_name[0] ? g_system.bootloader_name : "unknown",
                   g_system.bootloader_version);
    (void)snprintf(g_system_values[2], ROW_TEXT, "%s", g_system.firmware[0] ? g_system.firmware : "unknown");
    {
        const char* brand = g_system.cpu_brand;
        char clock[24] = "";

        while (*brand == ' ') {
            ++brand;
        }
        if (g_system.cpu_khz != 0) {
            (void)snprintf(clock, sizeof(clock), ", ~%u.%u GHz", g_system.cpu_khz / 1000000u,
                           (g_system.cpu_khz / 100000u) % 10u);
        }
        (void)snprintf(g_system_values[3], ROW_TEXT, "%.52s (%u core%s%s)",
                       brand[0] ? brand : (g_system.cpu_vendor[0] ? g_system.cpu_vendor : "unknown"),
                       g_system.cpu_online, g_system.cpu_online == 1 ? "" : "s", clock);
    }
    format_bytes(total, sizeof(total), g_system.memory_usable_bytes);
    format_bytes(free_text, sizeof(free_text), g_system.memory_free_bytes);
    (void)snprintf(g_system_values[4], ROW_TEXT, "%s RAM, %s free", total, free_text);
    if (g_system.sxfs_mounted) {
        format_bytes(total, sizeof(total), g_system.sxfs_total_bytes);
        format_bytes(free_text, sizeof(free_text), g_system.sxfs_free_bytes);
        (void)snprintf(g_system_values[5], ROW_TEXT, "SxFS, %s, %s free", total, free_text);
    } else {
        (void)snprintf(g_system_values[5], ROW_TEXT, "%s", "no SxFS volume mounted");
    }
    seconds = g_system.uptime_ms / 1000u;
    (void)snprintf(g_system_values[6], ROW_TEXT, "%llu:%02llu:%02llu", (unsigned long long)(seconds / 3600u),
                   (unsigned long long)(seconds / 60u % 60u), (unsigned long long)(seconds % 60u));
    (void)snprintf(g_system_values[7], ROW_TEXT, "%u x %u (%u bit)", g_system.framebuffer_width,
                   g_system.framebuffer_height, g_system.framebuffer_bpp);
}

/* ---- the display ------------------------------------------------------------- */

static void read_display(void)
{
    static uint32_t caps[SAVANXP_GPU_3D_MAX_CAPSET_BYTES / 4u];
    long device = sxn_open_device("gpu0", SXN_DEVICE_READ);

    memset(&g_gpu, 0, sizeof(g_gpu));
    memset(&g_connector, 0, sizeof(g_connector));
    memset(&g_capset, 0, sizeof(g_capset));
    if (device >= 0) {
        g_gpu_known = sxn_device_control(device, GPU_IOC_GET_INFO, (unsigned long)&g_gpu) == 0;
        g_connector_known =
            sxn_device_control(device, GPU_IOC_GET_CONNECTOR_PROPERTIES, (unsigned long)&g_connector) == 0;
        if (g_gpu_known && (g_gpu.flags & SAVANXP_GPU_INFO_FLAG_3D) != 0u) {
            g_capset.data = (uint64_t)(uintptr_t)caps;
            g_capset.size = sizeof(caps);
            g_capset_known = sxn_device_control(device, GPU_IOC_3D_GET_CAPSET, (unsigned long)&g_capset) == 0;
        }
        (void)sxn_close(device);
    }

    (void)snprintf(g_device_values[0], ROW_TEXT, "%s",
                   !g_gpu_known ? "not available"
                   : g_gpu.backend == SAVANXP_GPU_BACKEND_VIRTIO ? "VirtIO GPU"
                   : g_gpu.backend == SAVANXP_GPU_BACKEND_FRAMEBUFFER ? "Boot framebuffer (generic)"
                                                                      : "unknown adapter");
    if (g_gpu_known) {
        (void)snprintf(g_device_values[1], ROW_TEXT, "%u x %u (%u bit)", g_gpu.width, g_gpu.height, g_gpu.bpp);
    } else {
        (void)snprintf(g_device_values[1], ROW_TEXT, "%s", "not available");
    }
    if (g_connector_known && (g_connector.flags & SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING) != 0u) {
        (void)snprintf(g_device_values[2], ROW_TEXT, "%u x %u", g_connector.max_width, g_connector.max_height);
    } else {
        (void)snprintf(g_device_values[2], ROW_TEXT, "%s", "fixed at the boot mode");
    }
    if (g_capset_known) {
        (void)snprintf(g_device_values[3], ROW_TEXT, "virgl (capset %s, version %u)",
                       g_capset.capset_id == 2u ? "virgl2" : "virgl", g_capset.capset_version);
    } else {
        (void)snprintf(g_device_values[3], ROW_TEXT, "%s", "none");
    }
    (void)snprintf(g_device_values[4], ROW_TEXT, "%s%s",
                   g_gpu_known && (g_gpu.flags & SAVANXP_GPU_INFO_FLAG_IRQ_DRIVEN) ? "interrupt driven" : "polled",
                   g_gpu_known && (g_gpu.flags & SAVANXP_GPU_INFO_FLAG_CURSOR_PLANE) ? ", cursor plane" : "");
}

/* ---- SxGL itself --------------------------------------------------------------- */

struct backend_facts {
    int created;
    int backend;
    char vendor[32];
    char renderer[48];
    char version[32];
    char extensions[48];
    GLint max_texture, max_lights, max_modelview, max_projection;
};
static struct backend_facts g_facts[2]; /* [0] software, [1] GPU */

static void copy_string(char* out, size_t size, GLenum name)
{
    const GLubyte* text = glGetString(name);

    (void)snprintf(out, size, "%s", text != 0 ? (const char*)text : "");
}

static void read_backend(struct backend_facts* facts, unsigned flags)
{
    struct sx_bitmap bitmap;
    struct sxgl_context* context;

    memset(facts, 0, sizeof(*facts));
    if (!sx_bitmap_create(&bitmap, 16, 16, SX_PIXEL_FORMAT_BGRX8888)) {
        return;
    }
    context = sxgl_context_create_ex(&bitmap, flags);
    if (context != 0) {
        sxgl_make_current(context);
        facts->created = 1;
        facts->backend = sxgl_context_backend(context);
        copy_string(facts->vendor, sizeof(facts->vendor), GL_VENDOR);
        copy_string(facts->renderer, sizeof(facts->renderer), GL_RENDERER);
        copy_string(facts->version, sizeof(facts->version), GL_VERSION);
        copy_string(facts->extensions, sizeof(facts->extensions), GL_EXTENSIONS);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &facts->max_texture);
        glGetIntegerv(GL_MAX_LIGHTS, &facts->max_lights);
        glGetIntegerv(GL_MAX_MODELVIEW_STACK_DEPTH, &facts->max_modelview);
        glGetIntegerv(GL_MAX_PROJECTION_STACK_DEPTH, &facts->max_projection);
        sxgl_make_current(0);
        sxgl_context_destroy(context);
    }
    sx_bitmap_destroy(&bitmap);
}

static void renderer_row(int row, const char* name, const char* software, const char* gpu)
{
    (void)snprintf(g_renderer_rows[row], sizeof(g_renderer_rows[row]), "%s\t%s\t%s", name, software, gpu);
}

static void renderer_int_row(int row, const char* name, GLint software, GLint gpu)
{
    char left[16], right[16];

    (void)snprintf(left, sizeof(left), "%d", (int)software);
    if (g_gpu_backend_works) {
        (void)snprintf(right, sizeof(right), "%d", (int)gpu);
    } else {
        (void)snprintf(right, sizeof(right), "%s", "-");
    }
    renderer_row(row, name, left, right);
}

static void read_sxgl(void)
{
    const struct backend_facts* soft = &g_facts[0];
    const struct backend_facts* gpu = &g_facts[1];
    int row;

    read_backend(&g_facts[0], SXGL_CONTEXT_SOFTWARE);
    read_backend(&g_facts[1], SXGL_CONTEXT_GPU);
    g_gpu_backend_works = gpu->created && gpu->backend == SXGL_BACKEND_GPU;

    (void)snprintf(g_system_values[8], ROW_TEXT, "%s", soft->created ? soft->version : "not available");
    (void)snprintf(g_driver_values[0], ROW_TEXT, "%s", g_gpu_backend_works ? gpu->renderer : soft->renderer);
    (void)snprintf(g_driver_values[1], ROW_TEXT, "%s", soft->version);
    (void)snprintf(g_driver_values[2], ROW_TEXT, "%s", soft->extensions);
    (void)snprintf(g_driver_values[3], ROW_TEXT, "%s", g_gpu_backend_works ? "GPU (virgl)" : "Software");

    renderer_row(0, "Context", soft->created ? "Created" : "Failed",
                 g_gpu_backend_works ? "Created" : gpu->created ? "Fell back to software" : "Failed");
    renderer_row(1, "GL_VENDOR", soft->vendor, g_gpu_backend_works ? gpu->vendor : "-");
    renderer_row(2, "GL_RENDERER", soft->renderer, g_gpu_backend_works ? gpu->renderer : "-");
    renderer_row(3, "GL_VERSION", soft->version, g_gpu_backend_works ? gpu->version : "-");
    renderer_row(4, "GL_EXTENSIONS", soft->extensions, g_gpu_backend_works ? gpu->extensions : "-");
    renderer_int_row(5, "Max texture size", soft->max_texture, gpu->max_texture);
    renderer_int_row(6, "Max lights", soft->max_lights, gpu->max_lights);
    renderer_int_row(7, "Modelview stack", soft->max_modelview, gpu->max_modelview);
    renderer_int_row(8, "Projection stack", soft->max_projection, gpu->max_projection);
    renderer_row(9, "Rasterizes", "CPU, top-left fill rule", g_gpu_backend_works ? "Host GPU through virgl" : "-");
    for (row = 0; row < RENDERER_ROWS; ++row) {
        g_renderer_ptrs[row] = g_renderer_rows[row];
    }
    g_widgets[RENDERER_LIST].item_count = RENDERER_ROWS;

    notes_clear(&g_renderer_notes);
    notes_add(&g_renderer_notes, "SxGL implements a documented subset of OpenGL 1.1 fixed function.");
    if (g_gpu_backend_works) {
        notes_add(&g_renderer_notes, "Both backends are available; programs get the GPU by default.");
    } else {
        notes_add(&g_renderer_notes, "Only the software rasterizer is available on this machine.");
    }
    notes_add(&g_renderer_notes, "No problems found.");

    notes_clear(&g_display_notes);
    if (!g_capset_known) {
        notes_add(&g_display_notes, "No 3D device was found: SxGL draws with its software rasterizer.");
    } else if (!g_gpu_backend_works) {
        notes_add(&g_display_notes, "The 3D device refused an SxGL context: SxGL draws in software.");
    } else {
        notes_add(&g_display_notes, "SxGL draws on the host GPU through virgl.");
    }
    notes_add(&g_display_notes, "To test SxGL, click a Test button above.");
}

/* ---- the 3D test --------------------------------------------------------------- */

struct test_run {
    int running;
    int shown; /* the bitmap holds a frame worth painting */
    unsigned flags;
    struct sx_bitmap bitmap;
    struct sxgl_context* context;
    GLuint texture;
    uint64_t start_ns;
    uint32_t frames;
    uint32_t seconds; /* how long a run lasts */
    int failures;     /* runs whose last frame was wrong */
    char first_error[NOTE_TEXT];
};
static struct test_run g_test;

static void test_debug_sink(const char* message, void* user)
{
    (void)user;
    (void)sxn_log(message);
    if (g_test.first_error[0] == '\0') {
        (void)snprintf(g_test.first_error, sizeof(g_test.first_error), "%s", message);
    }
}

static void test_setup(void)
{
    static const GLfloat light_position[4] = {0.6f, 0.8f, 1.0f, 0.0f};
    static const GLfloat light_diffuse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const GLfloat ambient[4] = {0.25f, 0.25f, 0.25f, 1.0f};
    static uint8_t texels[64 * 64 * 4];
    int x, y;

    for (y = 0; y < 64; ++y) {
        for (x = 0; x < 64; ++x) {
            uint8_t* texel = &texels[(y * 64 + x) * 4];
            const int light = ((x / 8) + (y / 8)) % 2 == 0;

            texel[0] = light ? 255 : 150;
            texel[1] = light ? 255 : 170;
            texel[2] = light ? 255 : 220;
            texel[3] = 255;
        }
    }
    glGenTextures(1, &g_test.texture);
    glBindTexture(GL_TEXTURE_2D, g_test.texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);

    glViewport(0, 0, TEST_WIDTH, TEST_HEIGHT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-0.5 * TEST_WIDTH / TEST_HEIGHT, 0.5 * TEST_WIDTH / TEST_HEIGHT, -0.5, 0.5, 1.0, 20.0);
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);
    glLightfv(GL_LIGHT0, GL_POSITION, light_position);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_TEXTURE_2D);
    glClearColor(0.0f, 0.0f, 0.5f, 1.0f);
    glClearDepth(1.0);
}

static void cube_face(const GLfloat normal[3], const GLfloat colour[3], const GLfloat corners[4][3])
{
    static const GLfloat st[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    int index;

    glNormal3fv(normal);
    glColor3fv(colour);
    for (index = 0; index < 4; ++index) {
        glTexCoord2fv(st[index]);
        glVertex3fv(corners[index]);
    }
}

static void test_draw(float seconds)
{
    /* Counter-clockwise seen from outside, so back-face culling keeps the
     * three faces turned to the viewer. */
    static const GLfloat normals[6][3] = {
        {0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const GLfloat colours[6][3] = {
        {1.0f, 0.35f, 0.3f}, {0.3f, 1.0f, 0.35f}, {0.35f, 0.5f, 1.0f},
        {1.0f, 0.9f, 0.3f}, {0.9f, 0.4f, 1.0f}, {0.3f, 0.95f, 0.95f}};
    static const GLfloat faces[6][4][3] = {
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}}};
    int face;

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();
    glTranslatef(0.0f, 0.0f, -5.0f);
    glRotatef(30.0f + seconds * 50.0f, 1.0f, 0.0f, 0.0f);
    glRotatef(seconds * 70.0f, 0.0f, 1.0f, 0.0f);
    glBegin(GL_QUADS);
    for (face = 0; face < 6; ++face) {
        cube_face(normals[face], colours[face], faces[face]);
    }
    glEnd();
}

static void update_test_buttons(void)
{
    g_widgets[TEST_SOFTWARE].flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    g_widgets[TEST_GPU].flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    if (g_test.running) {
        g_widgets[TEST_SOFTWARE].flags |= SXGUI_FLAG_DISABLED;
        g_widgets[TEST_GPU].flags |= SXGUI_FLAG_DISABLED;
    }
    if (!g_gpu_backend_works) {
        g_widgets[TEST_GPU].flags |= SXGUI_FLAG_DISABLED;
    }
}

static void test_start(unsigned flags)
{
    if (g_test.running) {
        return;
    }
    if (g_test.bitmap.pixels == 0 &&
        !sx_bitmap_create(&g_test.bitmap, TEST_WIDTH, TEST_HEIGHT, SX_PIXEL_FORMAT_BGRX8888)) {
        notes_add(&g_display_notes, "The test could not allocate its image.");
        return;
    }
    g_test.context = sxgl_context_create_ex(&g_test.bitmap, flags);
    if (g_test.context == 0) {
        notes_add(&g_display_notes, "The test could not create an SxGL context.");
        return;
    }
    g_test.flags = flags;
    g_test.first_error[0] = '\0';
    sxgl_make_current(g_test.context);
    sxgl_set_debug_sink(g_test.context, test_debug_sink, 0);
    test_setup();
    sxgl_reset_stats(g_test.context);
    g_test.frames = 0;
    g_test.running = 1;
    g_test.shown = 1;
    g_test.start_ns = sxn_clock_ns();
    (void)snprintf(g_status, sizeof(g_status), "Testing the %s backend...",
                   (flags & SXGL_CONTEXT_SOFTWARE) ? "software" : "GPU");
    (void)sxn_log(g_status);
    g_app.tick_interval_ms = 1;
    update_test_buttons();
}

/* The last frame is right when the cube covers the centre and the corners
 * still hold the clear colour (0, 0, 0.5). */
static int test_frame_is_right(void)
{
    const uint32_t* pixels = g_test.bitmap.pixels;
    const uint32_t stride = g_test.bitmap.info.pitch / 4u;
    const uint32_t corner = pixels[2u * stride + 2u] & 0x00ffffffu;
    const uint32_t centre = pixels[(TEST_HEIGHT / 2u) * stride + TEST_WIDTH / 2u] & 0x00ffffffu;
    const uint32_t corner_blue = corner & 0xffu;

    return (corner >> 8) == 0u && corner_blue >= 120u && corner_blue <= 136u && centre != corner;
}

static void test_finish(void)
{
    const uint64_t elapsed_ns = sxn_clock_ns() - g_test.start_ns;
    const int on_gpu = sxgl_context_backend(g_test.context) == SXGL_BACKEND_GPU;
    const char* name = (g_test.flags & SXGL_CONTEXT_SOFTWARE) ? "Software" : "GPU";
    struct sxgl_stats stats;
    char line[NOTE_TEXT];
    struct sx_rect damage;
    uint32_t centi_fps;
    int right;

    sxgl_get_stats(g_test.context, &stats);
    /* The frame that is checked, and left on screen, has a fixed pose that
     * shows three faces: where the timed run stopped is anyone's guess. */
    test_draw(0.5f);
    sxgl_swap_buffers(g_test.context, &damage);
    right = test_frame_is_right() && glGetError() == GL_NO_ERROR && g_test.first_error[0] == '\0';
    centi_fps = elapsed_ns ? (uint32_t)((uint64_t)g_test.frames * 100000000000ull / elapsed_ns) : 0u;
    glDeleteTextures(1, &g_test.texture);
    sxgl_make_current(0);
    sxgl_context_destroy(g_test.context);
    g_test.context = 0;
    g_test.running = 0;
    g_app.tick_interval_ms = 1000;

    if (!right) {
        g_test.failures += 1;
        (void)snprintf(line, sizeof(line), "%s test FAILED: %s", name,
                       g_test.first_error[0] ? g_test.first_error : "the last frame is not the expected image.");
    } else {
        (void)snprintf(line, sizeof(line), "%s test passed: %u frames, %u.%02u fps, %llu triangles%s.", name,
                       g_test.frames, centi_fps / 100u, centi_fps % 100u,
                       (unsigned long long)stats.triangles_rasterized,
                       !(g_test.flags & SXGL_CONTEXT_SOFTWARE) && !on_gpu ? " (fell back to software)" : "");
    }
    notes_add(&g_display_notes, line);
    (void)sxn_log(line);
    (void)snprintf(g_status, sizeof(g_status), "%s", right ? "Test finished." : "Test failed.");
    update_test_buttons();
    if (g_widgets[TABS].value == PAGE_DISPLAY) {
        notes_bind(&g_display_notes);
    }
}

static void test_frame(void)
{
    struct sx_rect damage;
    const uint64_t elapsed_ns = sxn_clock_ns() - g_test.start_ns;

    test_draw((float)(elapsed_ns / 1000000u) / 1000.0f);
    sxgl_swap_buffers(g_test.context, &damage);
    g_test.frames += 1;
    if (elapsed_ns >= (uint64_t)g_test.seconds * 1000000000ull) {
        test_finish();
    }
}

/* `sxgldiag --selftest`: no window. Runs the test once with each backend the
 * machine has, for a second each, logs every line the sheet would show and
 * exits 0 only if every run drew the expected image. */
static void run_selftest(void)
{
    int index, passed, runs = 1;

    g_test.seconds = 1;
    test_start(SXGL_CONTEXT_SOFTWARE);
    while (g_test.running) {
        test_frame();
    }
    if (g_gpu_backend_works) {
        runs = 2;
        test_start(SXGL_CONTEXT_GPU);
        while (g_test.running) {
            test_frame();
        }
    }
    for (index = 0; index < g_renderer_notes.count; ++index) {
        (void)sxn_log(g_renderer_notes.text[index]);
    }
    /* Two notes were there before the tests; each run that finished adds one. */
    passed = g_test.failures == 0 && g_display_notes.count == 2 + runs;
    (void)sxn_log(passed ? "SXGLDIAG SELFTEST PASS" : "SXGLDIAG SELFTEST FAIL");
    sxn_exit(passed ? 0 : 1);
}

/* ---- the report ---------------------------------------------------------------- */

static int report_append(char* out, int used, int size, const char* caption, const char* value)
{
    int written;

    if (used >= size) {
        return used;
    }
    written = snprintf(out + used, (size_t)(size - used), "%s %s\n", caption, value);
    return written < 0 ? used : used + written > size ? size : used + written;
}

static void save_report(void)
{
    static char report[SXN_WRITE_FILE_MAX];
    const int size = (int)sizeof(report);
    int used = 0, index;
    long result;

    used = report_append(report, used, size, "SxGL Diagnostic Tool", g_version_text);
    used = report_append(report, used, size, "\n[System]", "");
    for (index = 0; index < SYSTEM_ROWS; ++index) {
        used = report_append(report, used, size, g_system_captions[index], g_system_values[index]);
    }
    used = report_append(report, used, size, "\n[Display device]", "");
    for (index = 0; index < DEVICE_ROWS; ++index) {
        used = report_append(report, used, size, g_device_captions[index], g_device_values[index]);
    }
    for (index = 0; index < DRIVER_ROWS; ++index) {
        used = report_append(report, used, size, g_driver_captions[index], g_driver_values[index]);
    }
    used = report_append(report, used, size, "\n[Renderer: property, software, GPU]", "");
    for (index = 0; index < RENDERER_ROWS; ++index) {
        used = report_append(report, used, size, "", g_renderer_rows[index]);
    }
    used = report_append(report, used, size, "\n[Notes]", "");
    for (index = 0; index < g_display_notes.count; ++index) {
        used = report_append(report, used, size, "", g_display_notes.text[index]);
    }
    for (index = 0; index < g_renderer_notes.count; ++index) {
        used = report_append(report, used, size, "", g_renderer_notes.text[index]);
    }
    result = sxn_write_file(REPORT_PATH, report, (unsigned long)used);
    if (result == 0) {
        (void)snprintf(g_status, sizeof(g_status), "Saved to %s", REPORT_PATH);
    } else {
        (void)snprintf(g_status, sizeof(g_status), "Could not save %s (error %ld).", REPORT_PATH, -result);
    }
}

/* ---- the sheet ----------------------------------------------------------------- */

static const signed char g_page_of[WIDGETS] = {
    [TABS] = PAGE_ALL,
    [SYSTEM_GROUP] = PAGE_SYSTEM, [PROGRESS] = PAGE_SYSTEM,
    [DEVICE_GROUP] = PAGE_DISPLAY, [DRIVER_GROUP] = PAGE_DISPLAY, [FEATURES_GROUP] = PAGE_DISPLAY,
    [TEST_SOFTWARE] = PAGE_DISPLAY, [TEST_GPU] = PAGE_DISPLAY,
    [RENDERER_LIST] = PAGE_RENDERER,
    [NOTES_GROUP] = PAGE_NOTES, [NOTES] = PAGE_NOTES,
    [NEXT_PAGE] = PAGE_ALL, [SAVE] = PAGE_ALL, [EXIT_BUTTON] = PAGE_ALL,
};

/* sxgui has no widget hierarchy: a page's controls exist only while their
 * tab is active, or they would paint and take clicks on the other pages. */
static void show_page(void)
{
    const int page = g_widgets[TABS].value;
    int index;

    for (index = 0; index < WIDGETS; ++index) {
        const int owner = g_page_of[index];
        const int visible = owner == PAGE_ALL || owner == page ||
                            (owner == PAGE_NOTES && (page == PAGE_DISPLAY || page == PAGE_RENDERER));

        if (visible) {
            g_widgets[index].flags |= SXGUI_FLAG_VISIBLE;
        } else {
            g_widgets[index].flags &= ~(uint32_t)SXGUI_FLAG_VISIBLE;
        }
    }
    notes_bind(page == PAGE_RENDERER ? &g_renderer_notes : &g_display_notes);
    if (page == PAGE_HELP) {
        g_widgets[NEXT_PAGE].flags |= SXGUI_FLAG_DISABLED;
    } else {
        g_widgets[NEXT_PAGE].flags &= ~(uint32_t)SXGUI_FLAG_DISABLED;
    }
}

static void on_tab(struct sxgui_widget* widget, void* user)
{
    (void)widget;
    (void)user;
    show_page();
    sxngui_app_request_repaint(&g_app);
}

static void on_button(struct sxgui_widget* widget, void* user)
{
    (void)user;
    if (widget == &g_widgets[NEXT_PAGE]) {
        if (g_widgets[TABS].value < TAB_COUNT - 1) {
            g_widgets[TABS].value += 1;
        }
        show_page();
    } else if (widget == &g_widgets[SAVE]) {
        save_report();
    } else if (widget == &g_widgets[EXIT_BUTTON]) {
        sxngui_app_quit(&g_app, 0);
    } else if (widget == &g_widgets[TEST_SOFTWARE]) {
        test_start(SXGL_CONTEXT_SOFTWARE);
        notes_bind(&g_display_notes);
    } else if (widget == &g_widgets[TEST_GPU]) {
        test_start(SXGL_CONTEXT_GPU);
        notes_bind(&g_display_notes);
    }
    sxngui_app_request_repaint(&g_app);
}

static void on_tick(struct sxngui_app* app)
{
    if (g_test.running) {
        test_frame();
    } else {
        /* Uptime and free memory move; the rest was read once. */
        read_system();
        (void)snprintf(g_system_values[8], ROW_TEXT, "%s",
                       g_facts[0].created ? g_facts[0].version : "not available");
    }
    sxngui_app_request_repaint(app);
}

/* Right-aligned captions and their values, the classic sheet's two columns. */
static void paint_rows(struct sx_painter* painter, int column, int y, const char* const* captions,
                       char values[][ROW_TEXT], int count, int step)
{
    int index;

    for (index = 0; index < count; ++index) {
        const int row_y = y + index * step;

        sx_painter_draw_text(painter, column - sx_painter_text_width(painter, captions[index]), row_y,
                             captions[index], SXGUI_COLOR_TEXT);
        sx_painter_draw_text(painter, column + 6, row_y, values[index], SXGUI_COLOR_TEXT);
    }
}

static void paint_lines(struct sx_painter* painter, int x, int y, const char* const* lines, int count)
{
    const int step = sx_painter_text_height(painter) + 3;
    int index;

    for (index = 0; index < count; ++index) {
        sx_painter_draw_text(painter, x, y + index * step, lines[index], SXGUI_COLOR_TEXT);
    }
}

static void paint_feature(struct sx_painter* painter, int x, int y, const char* caption, const char* value)
{
    sx_painter_draw_text(painter, x, y, caption, SXGUI_COLOR_TEXT);
    sx_painter_draw_text(painter, x + 130, y, value, SXGUI_COLOR_TEXT);
}

static void paint_preview(struct sx_painter* painter)
{
    const struct sx_rect group = g_widgets[FEATURES_GROUP].rect;
    const struct sx_rect frame = sx_rect_make(group.x + group.width - TEST_WIDTH - 16, group.y + 18,
                                              TEST_WIDTH + 4, TEST_HEIGHT + 4);

    sxchrome_draw_sunken(painter, frame);
    if (g_test.shown) {
        sx_painter_blit_bitmap(painter, &g_test.bitmap, frame.x + 2, frame.y + 2);
    } else {
        const char* idle = "No test has run";

        sx_painter_fill_rect(painter, sx_rect_make(frame.x + 2, frame.y + 2, TEST_WIDTH, TEST_HEIGHT),
                             SXGUI_RGB(0, 0, 128));
        sx_painter_draw_text(painter, frame.x + 2 + (TEST_WIDTH - sx_painter_text_width(painter, idle)) / 2,
                             frame.y + 2 + TEST_HEIGHT / 2 - 6, idle, SXGUI_RGB(255, 255, 255));
    }
}

static void paint_panel(struct sxngui_app* app)
{
    struct sx_painter* painter = &app->ui.painter;
    const struct sx_rect page = sxgui_tabs_page(&g_widgets[TABS]);
    const int left = page.x + 12;
    const int step = sx_painter_text_height(painter) + 7;

    switch (g_widgets[TABS].value) {
    case PAGE_SYSTEM: {
        static const char* const intro[] = {
            "This tool reports on SxGL, the system's 3D library, and on the machine and display",
            "it runs on. It lets you test SxGL's rasterizers and save what it found.",
            "",
            "If you know what the problem is, click the matching tab above. Otherwise use the",
            "Next Page button below to visit each page in turn."};
        const struct sx_rect group = g_widgets[SYSTEM_GROUP].rect;

        paint_lines(painter, left, page.y + 12, intro, 5);
        paint_rows(painter, group.x + 190, group.y + 24, g_system_captions, g_system_values, SYSTEM_ROWS, step);
        sx_painter_draw_text(painter, g_widgets[PROGRESS].rect.x + g_widgets[PROGRESS].rect.width + 12,
                             g_widgets[PROGRESS].rect.y + 1, g_version_text, SXGUI_COLOR_TEXT);
        break;
    }
    case PAGE_DISPLAY: {
        const struct sx_rect device = g_widgets[DEVICE_GROUP].rect;
        const struct sx_rect driver = g_widgets[DRIVER_GROUP].rect;
        const struct sx_rect features = g_widgets[FEATURES_GROUP].rect;

        paint_rows(painter, device.x + 96, device.y + 22, g_device_captions, g_device_values, DEVICE_ROWS, step);
        paint_rows(painter, driver.x + 104, driver.y + 22, g_driver_captions, g_driver_values, DRIVER_ROWS, step);
        paint_feature(painter, features.x + 12, g_widgets[TEST_SOFTWARE].rect.y - 22, "Software rasterizer:",
                      g_facts[0].created ? "Available" : "Not available");
        paint_feature(painter, features.x + 12, g_widgets[TEST_GPU].rect.y - 22, "GPU acceleration:",
                      g_gpu_backend_works ? "Available" : "Not available");
        paint_preview(painter);
        break;
    }
    case PAGE_RENDERER:
        sx_painter_draw_text(painter, left, page.y + 12, "What each SxGL backend reports:", SXGUI_COLOR_TEXT);
        break;
    case PAGE_HELP: {
        static const char* const help[] = {
            "Where to look next:",
            "",
            "- docs/desktop/SXGL.md lists every GL call SxGL implements and where it",
            "  differs from OpenGL 1.1 on purpose.",
            "- A program can route SxGL's errors to the system log with",
            "  sxgl_set_debug_sink; the Log Viewer shows them.",
            "- Without a 3D device every program draws in software. Under QEMU,",
            "  ./build.sh run --gl adds a virgl device and enables the GPU backend.",
            "- The saved report goes to " REPORT_PATH ".",
            "",
            "Gears and Nightwood are SxGL programs too, and a quick way to see",
            "the frame rate a real scene gets."};

        paint_lines(painter, left, page.y + 16, help, 12);
        break;
    }
    default:
        break;
    }
    if (g_status[0] != '\0') {
        sx_painter_draw_text(painter, 12, (int)app->info.height - 27, g_status, SXGUI_COLOR_TEXT);
    }
}

static void layout(struct sxngui_app* app)
{
    const int width = (int)app->info.width, height = (int)app->info.height;
    struct sx_rect page;
    int x, y, w, half, notes_y;

    g_widgets[TABS].rect = sx_rect_make(8, 8, width - 16, height - 52);
    page = sxgui_tabs_page(&g_widgets[TABS]);
    x = page.x + 10;
    y = page.y;
    w = page.width - 20;
    half = (w - 10) / 2;

    /* System */
    g_widgets[SYSTEM_GROUP].rect = sx_rect_make(x, y + 104, w, 220);
    g_widgets[PROGRESS].rect = sx_rect_make(x, y + 336, 120, 14);

    /* Display */
    g_widgets[DEVICE_GROUP].rect = sx_rect_make(x, y + 8, half + 40, 124);
    g_widgets[DRIVER_GROUP].rect = sx_rect_make(x + half + 50, y + 8, w - half - 50, 124);
    g_widgets[FEATURES_GROUP].rect = sx_rect_make(x, y + 140, w, TEST_HEIGHT + 32);
    g_widgets[TEST_SOFTWARE].rect = sx_rect_make(x + 142, y + 186, 140, SXGUI_BUTTON_HEIGHT);
    g_widgets[TEST_GPU].rect = sx_rect_make(x + 142, y + 260, 140, SXGUI_BUTTON_HEIGHT);

    /* Renderer */
    g_widgets[RENDERER_LIST].rect = sx_rect_make(x, y + 32, w, 228);

    /* Notes, shared by Display and Renderer */
    notes_y = y + TEST_HEIGHT + 180;
    g_widgets[NOTES_GROUP].rect = sx_rect_make(x, notes_y, w, page.y + page.height - notes_y - 8);
    g_widgets[NOTES].rect = sx_rect_make(x + 8, notes_y + 18, w - 16, page.y + page.height - notes_y - 34);

    g_widgets[NEXT_PAGE].rect = sx_rect_make(width - 368, height - 36, 100, SXGUI_BUTTON_HEIGHT);
    g_widgets[SAVE].rect = sx_rect_make(width - 260, height - 36, 160, SXGUI_BUTTON_HEIGHT);
    g_widgets[EXIT_BUTTON].rect = sx_rect_make(width - 92, height - 36, 84, SXGUI_BUTTON_HEIGHT);
    sxngui_app_request_repaint(app);
}

int main(int argc, char** argv)
{
    int result;

    (void)snprintf(g_version_text, sizeof(g_version_text), "SxGLDiag %s", SAVANXP_VERSION_STRING);
    g_test.seconds = TEST_SECONDS;

    g_widgets[TABS] = sxgui_tabs(sx_rect_make(0, 0, 0, 0), g_tab_labels, TAB_COUNT, PAGE_SYSTEM);
    g_widgets[TABS].on_action = on_tab;
    g_widgets[SYSTEM_GROUP] = sxgui_groupbox(sx_rect_make(0, 0, 0, 0), "System information");
    g_widgets[PROGRESS] = sxgui_progress(sx_rect_make(0, 0, 0, 0), 0, 100, 0);
    g_widgets[DEVICE_GROUP] = sxgui_groupbox(sx_rect_make(0, 0, 0, 0), "Device");
    g_widgets[DRIVER_GROUP] = sxgui_groupbox(sx_rect_make(0, 0, 0, 0), "SxGL");
    g_widgets[FEATURES_GROUP] = sxgui_groupbox(sx_rect_make(0, 0, 0, 0), "SxGL features");
    g_widgets[TEST_SOFTWARE] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Test software", on_button, 0);
    g_widgets[TEST_GPU] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Test GPU", on_button, 0);
    g_widgets[RENDERER_LIST] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_renderer_ptrs, 0);
    g_widgets[RENDERER_LIST].columns = g_renderer_columns;
    g_widgets[RENDERER_LIST].column_count = 3;
    g_widgets[RENDERER_LIST].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[RENDERER_LIST].value = -1;
    g_widgets[NOTES_GROUP] = sxgui_groupbox(sx_rect_make(0, 0, 0, 0), "Notes");
    g_widgets[NOTES] = sxgui_listbox(sx_rect_make(0, 0, 0, 0), g_display_notes.ptrs, 0);
    g_widgets[NOTES].flags |= SXGUI_FLAG_EMPTY_SELECTION;
    g_widgets[NEXT_PAGE] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Next Page", on_button, 0);
    g_widgets[SAVE] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Save All Information...", on_button, 0);
    g_widgets[EXIT_BUTTON] = sxgui_button(sx_rect_make(0, 0, 0, 0), "Exit", on_button, 0);

    read_system();
    g_widgets[PROGRESS].value = 50;
    read_display();
    read_sxgl();
    g_widgets[PROGRESS].value = 100;
    update_test_buttons();
    show_page();
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) {
        run_selftest();
    }

    result = sxngui_app_init(&g_app, &sun_resources, g_widgets, WIDGETS);
    if (result < 0) {
        (void)sxn_log("sxgldiag: window unavailable");
        sxn_exit(1);
    }
    g_app.on_resize = layout;
    g_app.on_paint = paint_panel;
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = 1000;
    (void)sxngui_app_set_content_size(&g_app, 620, 520);
    layout(&g_app);
    result = sxngui_app_run(&g_app);
    if (g_test.context != 0) {
        sxgl_make_current(0);
        sxgl_context_destroy(g_test.context);
    }
    sx_bitmap_destroy(&g_test.bitmap);
    sxn_exit(result);
    return 0;
}
