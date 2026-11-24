/* Exercise the renderer's 2D API through a POSIX window without game data. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <savanxp/libc.h>
#include "xash3d_mathlib.h"
#include "ref_api.h"

static struct savanxp_gfx_context window;
static ref_globals_t globals;
static uint32_t *pixels;

static _Noreturn void fail(const char *format, ...)
{
    va_list args;
    fputs("xash3d-refprobe: ", stderr);
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    exit(1);
}

static intptr_t engine_parameter(int parameter, int argument)
{
    return 0;
}

static cvar_t *get_cvar(const char *name)
{
    static cvar_t zero;
    return &zero;
}

static void register_cvar(convar_t *variable)
{
}

static int add_command(const char *name, void (*run)(void), const char *description)
{
    return 1;
}

static void remove_command(const char *name)
{
}

static int check_parameter(const char *name)
{
    return 0;
}

static void print_message(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

static poolhandle_t allocate_pool(const char *name, unsigned int flags, const char *file, int line)
{
    return 1;
}

static void *allocate(poolhandle_t pool, size_t size, qboolean clear, const char *file, int line)
{
    void *memory = clear ? calloc(1, size) : malloc(size);
    if (!memory)
        fail("could not allocate %zu bytes", size);
    return memory;
}

static void release(void *memory, const char *file, int line)
{
    free(memory);
}

static int random_long(int minimum, int maximum)
{
    return minimum;
}

static void set_random_seed(int seed)
{
}

static void extra_update(void)
{
}

static qboolean open_video(ref_graphic_apis_t type)
{
    if (type != REF_SOFTWARE)
        return false;
    if (gfx_open(&window) < 0)
        fail("could not open a POSIX window");
    if (gfx_acquire(&window) < 0)
        fail("could not acquire the window surface");
    gfx_handle_close(1);
    gfx_set_title(&window, "Xash3D Software Renderer");
    if (gfx_request_content_size(&window, 320, 240) < 0 ||
        gfx_wait_content_size(&window, 2000) < 0)
        fail("could not request a 320 x 240 surface");
    globals.width = window.info.width;
    globals.height = window.info.height;
    return true;
}

static void close_video(void)
{
    free(pixels);
    pixels = NULL;
    gfx_release(&window);
    gfx_close(&window);
}

static qboolean create_buffer(int width, int height, uint *stride, uint *bpp,
    uint *red, uint *green, uint *blue)
{
    if (width != (int)window.info.width || height != (int)window.info.height)
        return false;
    pixels = calloc(1, window.info.buffer_size);
    if (!pixels)
        fail("could not allocate the window buffer");
    *stride = window.info.pitch / sizeof(*pixels);
    *bpp = sizeof(*pixels);
    *red = 0x00ff0000;
    *green = 0x0000ff00;
    *blue = 0x000000ff;
    return true;
}

static void *lock_buffer(void)
{
    return pixels;
}

static void unlock_buffer(void)
{
    if (gfx_present(&window, pixels) < 0)
        fail("could not present the rendered frame");
}

static void draw_swatches(const ref_interface_t *renderer, qboolean captured_motion_received)
{
    byte red = captured_motion_received ? 0 : 224;
    byte blue = captured_motion_received ? 224 : 0;
    renderer->R_BeginFrame(false);
    renderer->FillRGBA(kRenderNormal, 0, 0, globals.width, globals.height, 0, 0, 0, 255);
    renderer->FillRGBA(kRenderNormal, 32, 32, 64, 64, red, 0, blue, 255);
    renderer->FillRGBA(kRenderNormal, 128, 32, 64, 64, 0, 224, 0, 255);
    renderer->FillRGBA(kRenderNormal, 224, 32, 64, 64, blue, 0, red, 255);
    renderer->R_EndFrame();
}

static qboolean check_capture(void)
{
    struct savanxp_input_event key;
    int result;
    while ((result = gfx_poll_event(&window, &key)) > 0) {
        if (key.type == SAVANXP_INPUT_EVENT_KEY_DOWN && key.key == 'm')
            break;
    }
    if (result < 0)
        fail("could not read the keyboard event");
    if (result == 0)
        return false;

    /* The keyboard poll passed over the motion records in the shared channel.
     * All captured events must still be available to the motion reader. */
    struct savanxp_gui_pointer_motion motion;
    int64_t dx = 0, dy = 0, wheel = 0;
    qboolean pressed = false;
    while ((result = gfx_poll_window_motion(&window, &motion)) > 0) {
        dx += motion.dx;
        dy += motion.dy;
        wheel += motion.wheel;
        pressed |= (motion.buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0;
    }
    if (result < 0 || dx <= 0 || dy >= 0 || wheel <= 0 || !pressed)
        fail("missing captured pointer motion, wheel, or button");
    if (gfx_set_pointer_capture(&window, 0) < 0)
        fail("could not release pointer capture");
    return true;
}

int main(int argc, char **argv)
{
    qboolean capture = argc == 2 && strcmp(argv[1], "--capture") == 0;
    void *module = dlopen("/games/xash3d/ref_soft.so", RTLD_NOW | RTLD_LOCAL);
    if (!module)
        fail("dlopen: %s", dlerror());
    REFAPI get_api = (REFAPI)dlsym(module, GET_REF_API);
    if (!get_api)
        fail("dlsym: %s", dlerror());

    ref_api_t engine = {
        .EngineGetParm = engine_parameter,
        .pfnGetCvarPointer = get_cvar,
        .Cvar_RegisterVariable = register_cvar,
        .Cmd_AddCommand = add_command,
        .Cmd_RemoveCommand = remove_command,
        .Con_Printf = print_message,
        .CL_ExtraUpdate = extra_update,
        .Host_Error = fail,
        .COM_RandomLong = random_long,
        .COM_SetRandomSeed = set_random_seed,
        .Sys_CheckParm = check_parameter,
        ._Mem_AllocPool = allocate_pool,
        ._Mem_Alloc = allocate,
        ._Mem_Free = release,
        .R_Init_Video = open_video,
        .R_Free_Video = close_video,
        .SW_CreateBuffer = create_buffer,
        .SW_LockBuffer = lock_buffer,
        .SW_UnlockBuffer = unlock_buffer,
    };
    ref_interface_t renderer = {0};
    if (get_api(REF_API_VERSION, &renderer, &engine, &globals) != REF_API_VERSION)
        fail("renderer API version mismatch");
    if (!renderer.R_Init())
        fail("renderer initialization failed");

    if (capture && gfx_set_pointer_capture(&window, 1) < 0)
        fail("could not request pointer capture");
    draw_swatches(&renderer, false);
    while (!gfx_should_close(&window)) {
        if (capture && check_capture()) {
            draw_swatches(&renderer, true);
            capture = false;
        }
        sleep_ms(20);
    }
    renderer.R_Shutdown();
    dlclose(module);
    return 0;
}
