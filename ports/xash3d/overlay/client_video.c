#include "common.h"
#include "client.h"
#include "input.h"
#include "platform/platform.h"
#include "vid_common.h"
#include "savanxp/libc.h"

static struct savanxp_gfx_context window;
static uint32_t *pixels;
static int width, height;
static int mouse_x, mouse_y;
static float motion_x, motion_y;
static uint32_t mouse_buttons;
static qboolean opened;
static qboolean mouse_grabbed;
static qboolean fullscreen;

qboolean R_Init_Video(ref_graphic_apis_t type)
{
    if (type != REF_SOFTWARE)
        return false;
    if (gfx_open(&window) < 0)
        return false;
    if (gfx_acquire(&window) < 0) {
        gfx_close(&window);
        return false;
    }
    opened = true;
    gfx_handle_close(1);
    gfx_set_title(&window, "Xash3D");
    return VID_SetMode();
}

void R_Free_Video(void)
{
    free(pixels);
    pixels = NULL;
    width = height = 0;
    if (opened) {
        Platform_SetMouseGrab(false);
        gfx_release(&window);
        gfx_close(&window);
        opened = false;
        fullscreen = false;
    }
}

static void apply_mode(void)
{
    int w = window.info.width;
    int h = window.info.height;
    VID_SetDisplayTransform(&w, &h);
    R_SaveVideoMode(window.info.width, window.info.height, w, h, false);
}

qboolean VID_SetMode(void)
{
    return R_ChangeDisplaySettings(window_width.value, window_height.value, vid_fullscreen.value) == rserr_ok;
}

rserr_t R_ChangeDisplaySettings(int w, int h, window_mode_t mode)
{
    if (!opened)
        return rserr_invalid_context;

    if (w < VID_MIN_WIDTH || h < VID_MIN_HEIGHT) {
        w = 640;
        h = 480;
    }
    qboolean want_fullscreen = mode != WINDOW_MODE_WINDOWED;
    if (want_fullscreen != fullscreen) {
        if (gfx_set_fullscreen(&window, want_fullscreen) < 0)
            return rserr_invalid_fullscreen;
        fullscreen = want_fullscreen;
    }
    if (!fullscreen && (w != (int)window.info.width || h != (int)window.info.height)) {
        if (gfx_request_content_size(&window, w, h) < 0)
            return rserr_invalid_mode;
        gfx_wait_content_size(&window, 250);
    }
    apply_mode();
    return rserr_ok;
}

qboolean SW_CreateBuffer(int w, int h, uint *stride, uint *bpp, uint *r, uint *g, uint *b)
{
    if (!opened || w != (int)window.info.width || h != (int)window.info.height)
        return false;
    uint32_t *next = calloc(1, window.info.buffer_size);
    if (!next)
        return false;
    free(pixels);
    pixels = next;
    width = w;
    height = h;
    *stride = window.info.pitch / sizeof(*pixels);
    *bpp = sizeof(*pixels);
    *r = 0x00ff0000;
    *g = 0x0000ff00;
    *b = 0x000000ff;
    return true;
}

void *SW_LockBuffer(void) { return pixels; }
void SW_UnlockBuffer(void)
{
    if (opened && pixels && width == (int)window.info.width && height == (int)window.info.height)
        gfx_present(&window, pixels);
}

int R_MaxVideoModes(void) { return 0; }
vidmode_t *R_GetVideoMode(int mode) { return NULL; }
void *GL_GetProcAddress(const char *name) { return NULL; }
int GL_SetAttribute(int attr, int value) { return 0; }
int GL_GetAttribute(int attr, int *value) { return 0; }
void GL_SwapBuffers(void) {}
void GL_UpdateSwapInterval(void) {}
void Platform_Minimize_f(void) {}
ref_window_type_t R_GetWindowHandle(void **handle, ref_window_type_t type)
{
    if (handle)
        *handle = NULL;
    return REF_WINDOW_TYPE_NULL;
}

static int keycode(const struct savanxp_input_event *event)
{
    if (event->key >= SAVANXP_KEY_F1 && event->key <= SAVANXP_KEY_F12)
        return K_F1 + event->key - SAVANXP_KEY_F1;
    switch (event->key) {
    case SAVANXP_KEY_BACKSPACE: return K_BACKSPACE;
    case SAVANXP_KEY_TAB: return K_TAB;
    case SAVANXP_KEY_ENTER: return K_ENTER;
    case SAVANXP_KEY_ESC: return K_ESCAPE;
    case SAVANXP_KEY_UP: return K_UPARROW;
    case SAVANXP_KEY_DOWN: return K_DOWNARROW;
    case SAVANXP_KEY_LEFT: return K_LEFTARROW;
    case SAVANXP_KEY_RIGHT: return K_RIGHTARROW;
    case SAVANXP_KEY_SHIFT: return K_SHIFT;
    case SAVANXP_KEY_CTRL: return K_CTRL;
    case SAVANXP_KEY_ALT: return K_ALT;
    case SAVANXP_KEY_HOME: return K_HOME;
    case SAVANXP_KEY_END: return K_END;
    case SAVANXP_KEY_PAGE_UP: return K_PGUP;
    case SAVANXP_KEY_PAGE_DOWN: return K_PGDN;
    case SAVANXP_KEY_INSERT: return K_INS;
    case SAVANXP_KEY_DELETE: return K_DEL;
    default:
        if (event->key >= 32 && event->key < 127)
            return Q_tolower(event->key);
        return 0;
    }
}

static void pointer_buttons(uint32_t buttons, int32_t wheel)
{
    for (int i = 0; i < 3; ++i) {
        uint32_t bit = 1u << i;
        if ((mouse_buttons ^ buttons) & bit)
            IN_MouseEvent(i, (buttons & bit) != 0);
    }
    mouse_buttons = buttons;
    int key = wheel > 0 ? K_MWHEELUP : K_MWHEELDOWN;
    for (; wheel != 0; wheel += wheel > 0 ? -1 : 1) {
        Key_Event(key, true);
        Key_Event(key, false);
    }
}

void Platform_RunEvents(void)
{
    if (!opened)
        return;
    if (gfx_should_close(&window))
        Sys_Quit("Window closed");

    struct savanxp_input_event key;
    while (gfx_poll_event(&window, &key) > 0) {
        if (key.type == SAVANXP_INPUT_EVENT_RESIZED) {
            if (gfx_apply_resize_event(&window, &key) >= 0)
                apply_mode();
            continue;
        }
        int code = keycode(&key);
        qboolean down = key.type == SAVANXP_INPUT_EVENT_KEY_DOWN;
        if (code)
            Key_Event(code, down);
        if (down && host.textmode && key.ascii >= 32 && key.ascii != 127 && code != '\x60')
            CL_CharEvent(key.ascii);
    }

    struct savanxp_gui_pointer_event pointer;
    while (gfx_poll_window_pointer(&window, &pointer) > 0) {
        mouse_x = pointer.x;
        mouse_y = pointer.y;
        if (!mouse_grabbed)
            pointer_buttons(pointer.buttons, pointer.wheel);
    }
    struct savanxp_gui_pointer_motion motion;
    while (gfx_poll_window_motion(&window, &motion) > 0) {
        if (mouse_grabbed) {
            motion_x += motion.dx;
            motion_y += motion.dy;
            pointer_buttons(motion.buttons, motion.wheel);
        }
    }
}

void Platform_MouseMove(float *x, float *y)
{
    *x = motion_x;
    *y = motion_y;
    motion_x = motion_y = 0;
}
void Platform_GetMousePos(int *x, int *y)
{
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}
void Platform_SetMousePos(int x, int y) {}
void Platform_SetCursorType(VGUI_DefaultCursor type)
{
    if (!opened || type == dc_none)
        return;

    uint32_t shape = SAVANXP_CURSOR_ARROW;
    switch (type) {
    case dc_ibeam: shape = SAVANXP_CURSOR_TEXT; break;
    case dc_hourglass: shape = SAVANXP_CURSOR_WAIT; break;
    case dc_sizenwse: shape = SAVANXP_CURSOR_RESIZE_NWSE; break;
    case dc_sizenesw: shape = SAVANXP_CURSOR_RESIZE_NESW; break;
    case dc_sizewe: shape = SAVANXP_CURSOR_RESIZE_H; break;
    case dc_sizens: shape = SAVANXP_CURSOR_RESIZE_V; break;
    case dc_sizeall: shape = SAVANXP_CURSOR_MOVE; break;
    case dc_no: shape = SAVANXP_CURSOR_UNAVAILABLE; break;
    case dc_hand: shape = SAVANXP_CURSOR_LINK; break;
    default: break;
    }
    gfx_desktop_set_cursor_shape(&window, shape);
}
void Platform_SetMouseGrab(qboolean enable)
{
    if (opened && enable != mouse_grabbed && gfx_set_pointer_capture(&window, enable) >= 0) {
        mouse_grabbed = enable;
        motion_x = motion_y = 0;
    }
}
qboolean Platform_GetMouseGrab(void) { return mouse_grabbed; }

platform_orientation_t Platform_GetDisplayOrientation(void)
{
    return ORIENTATION_LANDSCAPE;
}
void VID_Info_f(void)
{
    Con_Printf("SavanXP software surface: %d x %d\n", width, height);
}
