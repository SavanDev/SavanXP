/*
 * harness.c -- win32-harness: run a PE32 executable on the Linux x86-64
 * build host through the same loader and DLLs as winhost.
 *
 *     win32-harness [--dlls DIR] [--registry FILE] [--time SECONDS] [--input SCRIPT]
 *                   program.exe [arguments...]
 *
 * The arguments follow the program in its command line. HKEY_CURRENT_USER
 * lives in FILE (in memory only without it); --time fixes the guest's clock
 * at that second, so a run that seeds from the time repeats.
 *
 * Windows are offscreen surfaces of a 1024x768 display. Their input comes from
 * SCRIPT, one action per line, given whenever the guest waits for input: a
 * guest that only polls sees none. Coordinates are the main window's client
 * coordinates, counted from its marker as on a screenshot (docs/userland/
 * WIN32.md, "Input scripts"); a popup over the point gets the pointer, keys
 * go to the newest dialog, or to the main window:
 *
 *     move X Y                 the pointer moves
 *     down|up left|right X Y   a button goes down or up there
 *     keydown VK [CHAR]        a key goes down (virtual key and the UTF-16
 *     keyup VK                 character it types, numbers in C syntax)
 *     wait MS                  nothing happens for MS milliseconds
 *     close                    the frame's close button
 *     window N                 from here on, the N-th dialog open (0: the
 *                              main window again)
 *     dump FILE [WxH]          the window's client area from its marker,
 *                              W x H or all of it, its popups over it, as a
 *                              binary PPM: what a screenshot shows
 *
 * With a script the guest's millisecond clock is virtual: it advances only
 * through waits, so timers fire at the same point of every run.
 *
 * DLLs are looked up next to the executable, then in DIR. The guest's
 * standard output and error are this process's. The guest's 32-bit exit
 * status goes to standard error as `win32: exit status 0x...`, and its low
 * byte is this process's exit status. A load that fails prints
 * `win32: <cause>` and ends the same way with the NT status Windows XP gives
 * that failure (0xc0000135 for a missing DLL, 0xc0000139 for a missing
 * function). It is also checked to have released every mapping and file it
 * took; if not, the harness exits with 124.
 *
 * See docs/userland/WIN32.md, "The host harness".
 */
#define _GNU_SOURCE
#include <asm/ldt.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "loader.h"
#include "services.h"
#include "sxw_abi.h"

#define TRAMPOLINE 0x7ffe0000u
#define TRAMPOLINE_BACK32 0x00u
#define TRAMPOLINE_LANDING_RETURN 0x10u
#define TRAMPOLINE_SERVICE32 0x20u
#define TRAMPOLINE_LANDING_SERVICE 0x30u
#define TRAMPOLINE_SVC_RET32 0x40u
#define LINUX_CODE64 0x33u
#define TEB_LDT_ENTRY 0
#define TEB_SELECTOR ((TEB_LDT_ENTRY << 3) | 4 | 3) /* LDT, ring 3 */

/* State shared with gate_x86_64.S. */
uint64_t sxw_host_rsp;
uint64_t sxw_host_fsbase;
uint64_t sxw_guest_esp;
uint64_t sxw_svc_ret32;
uint32_t sxw_guest_regs[4];
uint32_t sxw_result;
uint32_t sxw_exit_requested;
uint16_t sxw_teb_selector = TEB_SELECTOR;

uint32_t sxw_gate_enter(uint32_t entry, uint32_t guest_esp);
void sxw_gate_return(void);
void sxw_gate_service(void);

static sxw_process g_process;
static const char *g_dll_dir;
static char g_exe_dir[4096];
static uint64_t g_live_bytes;
static int g_open_files;
static uint32_t g_exit_status;

/* ---- Platform ----------------------------------------------------------- */

static int platform_map(void *context, uint32_t base, uint32_t size) {
    (void)context;
    void *want = (void *)(uintptr_t)base;
    void *got = mmap(want, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        return 1;
    }
    if (got != want) {
        munmap(got, size);
        return 1;
    }
    g_live_bytes += size;
    return 0;
}

static int platform_protect(void *context, uint32_t base, uint32_t size, pe_protection protection) {
    (void)context;
    int flags = protection == PE_PROT_RX ? PROT_READ | PROT_EXEC
                : protection == PE_PROT_RW ? PROT_READ | PROT_WRITE
                                           : PROT_READ;
    return mprotect((void *)(uintptr_t)base, size, flags) == 0 ? 0 : 1;
}

static void platform_unmap(void *context, uint32_t base, uint32_t size) {
    (void)context;
    munmap((void *)(uintptr_t)base, size);
    g_live_bytes -= size;
}

typedef struct file_buffer {
    uint8_t *data;
    size_t size;
} file_buffer;

static int read_whole(const char *path, file_buffer *out) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (1 << 28)) {
        close(fd);
        return 1;
    }
    out->size = (size_t)st.st_size;
    out->data = malloc(out->size);
    size_t done = 0;
    while (out->data != NULL && done < out->size) {
        ssize_t n = read(fd, out->data + done, out->size - done);
        if (n <= 0) {
            break;
        }
        done += (size_t)n;
    }
    close(fd);
    if (out->data == NULL || done != out->size) {
        free(out->data);
        return 1;
    }
    return 0;
}

static int platform_open(void *context, const char *name, const uint8_t **data, size_t *size,
                         void **handle) {
    (void)context;
    file_buffer *file = calloc(1, sizeof(*file));
    if (file == NULL) {
        return 1;
    }
    int found;
    if (strchr(name, '/') != NULL) {
        found = read_whole(name, file) == 0;
    } else {
        /* Next to the executable, then the DLL folder; names are installed
         * lower-case. */
        char lower[256];
        size_t n = strlen(name);
        if (n >= sizeof(lower)) {
            free(file);
            return 1;
        }
        for (size_t i = 0; i <= n; ++i) {
            lower[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] - 'A' + 'a' : name[i]);
        }
        char path[4096 + 256];
        snprintf(path, sizeof(path), "%s/%s", g_exe_dir, lower);
        found = read_whole(path, file) == 0;
        if (!found && g_dll_dir != NULL) {
            snprintf(path, sizeof(path), "%s/%s", g_dll_dir, lower);
            found = read_whole(path, file) == 0;
        }
    }
    if (!found) {
        free(file);
        return 1;
    }
    *data = file->data;
    *size = file->size;
    *handle = file;
    ++g_open_files;
    return 0;
}

static void platform_close(void *context, void *handle) {
    (void)context;
    file_buffer *file = handle;
    free(file->data);
    free(file);
    --g_open_files;
}

/* ---- Services ------------------------------------------------------------ */

static long write_stream(void *context, int stream, const void *buffer, uint32_t length) {
    (void)context;
    return write(stream, buffer, length);
}

static const char *g_registry_path;
static sxw_registry g_registry;

static uint64_t now_seconds(void *context) {
    (void)context;
    return (uint64_t)time(NULL);
}

static int save_registry_file(void *context, const char *text, size_t length) {
    (void)context;
    int fd = open(g_registry_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return 1;
    }
    size_t done = 0;
    while (done < length) {
        ssize_t n = write(fd, text + done, length - done);
        if (n <= 0) {
            close(fd);
            return 1;
        }
        done += (size_t)n;
    }
    return close(fd) == 0 ? 0 : 1;
}

/* ---- Windows and input --------------------------------------------------- */

#define DISPLAY_WIDTH 1024u
#define DISPLAY_HEIGHT 768u
#define MAX_WINDOWS 8u
/* How long a guest may keep waiting on timers after the script ended. */
#define AFTER_SCRIPT_MS 60000u

typedef struct offscreen {
    int used;
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
    uint32_t owner;
    uint32_t kind;
    int32_t x; /* a popup's place in its owner's client area */
    int32_t y;
    uint32_t order; /* when it was created */
} offscreen;

static offscreen g_windows[MAX_WINDOWS];
static uint32_t g_created;
static FILE *g_script;
static const char *g_script_path;
static unsigned g_script_line;
static uint32_t g_clock;      /* virtual milliseconds, with a script */
static uint32_t g_wait_left;  /* of the script's current wait */
static uint32_t g_after_script;
static uint32_t g_buttons;
static uint32_t g_target;     /* the script's "window N" */
static struct timespec g_started;

static uint32_t ticks(void *context) {
    (void)context;
    if (g_script_path != NULL) {
        return g_clock;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((now.tv_sec - g_started.tv_sec) * 1000 + (now.tv_nsec - g_started.tv_nsec) / 1000000);
}

static void display(void *context, uint32_t *width, uint32_t *height) {
    (void)context;
    *width = DISPLAY_WIDTH;
    *height = DISPLAY_HEIGHT;
}

static offscreen *window_of(uint32_t id) {
    return id >= 1u && id <= MAX_WINDOWS && g_windows[id - 1u].used ? &g_windows[id - 1u] : NULL;
}

static uint32_t window_create(void *context, const char *title, uint32_t *width, uint32_t *height, uint32_t owner,
                              uint32_t kind, int32_t x, int32_t y) {
    (void)context;
    (void)title;
    if (kind != SXW_WINDOW_MAIN && window_of(owner) == NULL) {
        return 0;
    }
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        if (!g_windows[i].used) {
            uint32_t w = *width > DISPLAY_WIDTH ? DISPLAY_WIDTH : *width;
            uint32_t h = *height > DISPLAY_HEIGHT ? DISPLAY_HEIGHT : *height;
            g_windows[i].pixels = calloc((size_t)w * h, 4);
            if (g_windows[i].pixels == NULL) {
                return 0;
            }
            g_windows[i].used = 1;
            g_windows[i].width = *width = w;
            g_windows[i].height = *height = h;
            g_windows[i].owner = owner;
            g_windows[i].kind = kind;
            g_windows[i].x = x;
            g_windows[i].y = y;
            g_windows[i].order = ++g_created;
            return i + 1u;
        }
    }
    return 0;
}

static void window_destroy(void *context, uint32_t id) {
    (void)context;
    offscreen *w = window_of(id);
    if (w != NULL) {
        free(w->pixels);
        memset(w, 0, sizeof(*w));
    }
}

static int window_title(void *context, uint32_t id, const char *title) {
    (void)context;
    (void)title;
    return window_of(id) != NULL ? 0 : 1;
}

static int window_icon(void *context, uint32_t id, const uint32_t *pixels) {
    (void)context;
    (void)pixels;
    return window_of(id) != NULL ? 0 : 1;
}

static int window_present(void *context, uint32_t id, const uint32_t *pixels, uint32_t stride, uint32_t width,
                          uint32_t height, const int32_t *rects, uint32_t count) {
    (void)context;
    (void)height;
    offscreen *w = window_of(id);
    if (w == NULL) {
        return 1;
    }
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t *r = &rects[4u * i];
        uint32_t right = (uint32_t)r[2] < w->width ? (uint32_t)r[2] : w->width;
        uint32_t bottom = (uint32_t)r[3] < w->height ? (uint32_t)r[3] : w->height;
        (void)width;
        for (uint32_t y = (uint32_t)r[1]; y < bottom; ++y) {
            for (uint32_t x = (uint32_t)r[0]; x < right; ++x) {
                w->pixels[y * w->width + x] = pixels[y * stride + x];
            }
        }
    }
    return 0;
}

/* The oldest open window of a kind, or with index > 0 the index-th one;
 * 0 when there is none. */
static uint32_t nth_window(uint32_t kind, uint32_t index) {
    uint32_t found[MAX_WINDOWS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        if (g_windows[i].used && g_windows[i].kind == kind) {
            found[n++] = i + 1u;
        }
    }
    for (uint32_t i = 1; i < n; ++i) {
        for (uint32_t j = i; j > 0 && g_windows[found[j] - 1u].order < g_windows[found[j - 1u] - 1u].order; --j) {
            const uint32_t t = found[j];
            found[j] = found[j - 1u];
            found[j - 1u] = t;
        }
    }
    const uint32_t at = index > 0 ? index - 1u : 0u;
    return at < n ? found[at] : 0u;
}

/* The window the script means: the main window, or with "window N" the
 * N-th dialog open. */
static uint32_t target_window(void) {
    return g_target == 0 ? nth_window(SXW_WINDOW_MAIN, 0) : nth_window(SXW_WINDOW_OWNED, g_target);
}

/* Where a window's client coordinates start on its pixels: at its marker,
 * as the drivers find it on a screenshot (a 4x4 square, magenta for the
 * main window and cyan for a dialog), or at its corner without one. */
static void marker_of(const offscreen *w, int32_t *mx, int32_t *my) {
    const uint32_t color = w->kind == SXW_WINDOW_MAIN ? 0xff00ffu : 0x00ffffu;
    *mx = 0;
    *my = 0;
    for (uint32_t y = 0; y + 4u <= w->height; ++y) {
        for (uint32_t x = 0; x + 4u <= w->width; ++x) {
            int square = 1;
            for (uint32_t i = 0; i < 16u && square; ++i) {
                square = w->pixels[(y + i / 4u) * w->width + x + i % 4u] == color;
            }
            if (square) {
                *mx = (int32_t)x;
                *my = (int32_t)y;
                return;
            }
        }
    }
}

/* Script coordinates to the window an event goes to, and its coordinates
 * there: a popup of the target window takes a pointer over it. */
static uint32_t route_pointer(int32_t x, int32_t y, int32_t *out_x, int32_t *out_y) {
    const uint32_t id = target_window();
    offscreen *w = window_of(id);
    int32_t mx;
    int32_t my;
    marker_of(w, &mx, &my);
    x += mx;
    y += my;
    uint32_t best = 0;
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        const offscreen *p = &g_windows[i];
        if (p->used && p->kind == SXW_WINDOW_POPUP && p->owner == id && x >= p->x && y >= p->y &&
            x < p->x + (int32_t)p->width && y < p->y + (int32_t)p->height &&
            (best == 0 || p->order > g_windows[best - 1u].order)) {
            best = i + 1u;
        }
    }
    if (best != 0) {
        *out_x = x - g_windows[best - 1u].x;
        *out_y = y - g_windows[best - 1u].y;
        return best;
    }
    *out_x = x;
    *out_y = y;
    return id;
}

/* Keys go to the newest dialog, or to the main window. */
static uint32_t active_window(void) {
    uint32_t best = nth_window(SXW_WINDOW_MAIN, 0);
    for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
        if (g_windows[i].used && g_windows[i].kind == SXW_WINDOW_OWNED &&
            (best == 0 || g_windows[i].order > g_windows[best - 1u].order)) {
            best = i + 1u;
        }
    }
    return best;
}

/* The window's client area as a screenshot shows it: from its marker,
 * width x height (0 for the rest of the window), its popups over it. */
static void dump(const char *path, uint32_t width, uint32_t height) {
    const uint32_t id = target_window();
    offscreen *w = window_of(id);
    FILE *f = w != NULL ? fopen(path, "wb") : NULL;
    if (f == NULL) {
        fprintf(stderr, "win32: %s:%u: cannot dump the window to %s\n", g_script_path, g_script_line, path);
        exit(125);
    }
    int32_t mx;
    int32_t my;
    marker_of(w, &mx, &my);
    if (width == 0u || width > w->width - (uint32_t)mx) {
        width = w->width - (uint32_t)mx;
    }
    if (height == 0u || height > w->height - (uint32_t)my) {
        height = w->height - (uint32_t)my;
    }
    fprintf(f, "P6\n%u %u\n255\n", width, height);
    for (uint32_t y = (uint32_t)my; y < (uint32_t)my + height; ++y) {
        for (uint32_t x = (uint32_t)mx; x < (uint32_t)mx + width; ++x) {
            uint32_t p = w->pixels[y * w->width + x];
            uint32_t order = 0;
            for (uint32_t i = 0; i < MAX_WINDOWS; ++i) {
                const offscreen *o = &g_windows[i];
                const int32_t px = (int32_t)x - o->x;
                const int32_t py = (int32_t)y - o->y;
                if (o->used && o->kind == SXW_WINDOW_POPUP && o->owner == id && o->order > order && px >= 0 &&
                    py >= 0 && px < (int32_t)o->width && py < (int32_t)o->height) {
                    p = o->pixels[(uint32_t)py * o->width + (uint32_t)px];
                    order = o->order;
                }
            }
            const unsigned char rgb[3] = {(unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

static __attribute__((noreturn)) void script_error(const char *what) {
    fprintf(stderr, "win32: %s:%u: %s\n", g_script_path, g_script_line, what);
    exit(125);
}

static uint32_t button_bit(const char *name) {
    if (strcmp(name, "left") == 0) {
        return SXW_BUTTON_LEFT;
    }
    if (strcmp(name, "right") == 0) {
        return SXW_BUTTON_RIGHT;
    }
    if (strcmp(name, "middle") == 0) {
        return SXW_BUTTON_MIDDLE;
    }
    script_error("unknown button");
}

/* The next action of the script: 1 with an event, 0 after a wait (in
 * g_wait_left) or at its end (g_script closed). */
static int next_action(sxw_event *event) {
    char line[512];
    while (fgets(line, sizeof(line), g_script) != NULL) {
        ++g_script_line;
        char verb[32] = "";
        char word[400] = "";
        long x = 0;
        long y = 0;
        if (sscanf(line, "%31s", verb) != 1 || verb[0] == '#') {
            continue;
        }
        memset(event, 0, sizeof(*event));
        if (strcmp(verb, "window") == 0) {
            if (sscanf(line, "%*s %ld", &x) != 1 || x < 0 || x >= (long)MAX_WINDOWS) {
                script_error("window needs a number");
            }
            g_target = (uint32_t)x;
            continue;
        }
        event->window = target_window();
        const int keyed = strcmp(verb, "keydown") == 0 || strcmp(verb, "keyup") == 0;
        if (event->window == 0u && strcmp(verb, "wait") != 0 && !keyed) {
            script_error("there is no such window");
        }
        if (strcmp(verb, "move") == 0 && sscanf(line, "%*s %ld %ld", &x, &y) == 2) {
            event->kind = SXW_EVENT_POINTER;
        } else if ((strcmp(verb, "down") == 0 || strcmp(verb, "up") == 0) &&
                   sscanf(line, "%*s %31s %ld %ld", word, &x, &y) == 3) {
            const uint32_t bit = button_bit(word);
            g_buttons = verb[0] == 'd' ? g_buttons | bit : g_buttons & ~bit;
            event->kind = SXW_EVENT_POINTER;
        } else if (strcmp(verb, "keydown") == 0 || strcmp(verb, "keyup") == 0) {
            unsigned long vk = 0;
            unsigned long ch = 0;
            if (sscanf(line, "%*s %li %li", (long *)&vk, (long *)&ch) < 1 || vk == 0 || vk > 0xff) {
                script_error("a key needs a virtual key from 1 to 0xff");
            }
            event->kind = SXW_EVENT_KEY;
            event->window = active_window();
            event->a = (uint32_t)vk;
            event->b = verb[3] == 'd' ? 1u : 0u;
            event->c = event->b ? (uint32_t)ch : 0u;
        } else if (strcmp(verb, "close") == 0) {
            event->kind = SXW_EVENT_CLOSE;
        } else if (strcmp(verb, "wait") == 0 && sscanf(line, "%*s %ld", &x) == 1 && x > 0) {
            g_wait_left = (uint32_t)x;
            return 0;
        } else if (strcmp(verb, "dump") == 0 && sscanf(line, "%*s %399s", word) == 1) {
            unsigned width = 0;
            unsigned height = 0;
            (void)sscanf(line, "%*s %*s %ux%u", &width, &height);
            dump(word, width, height);
            continue;
        } else {
            script_error("not an action");
        }
        if (event->kind == SXW_EVENT_POINTER) {
            int32_t px;
            int32_t py;
            event->window = route_pointer((int32_t)x, (int32_t)y, &px, &py);
            event->a = (uint32_t)px;
            event->b = (uint32_t)py;
            event->c = g_buttons;
        }
        event->time = g_clock;
        return 1;
    }
    fclose(g_script);
    g_script = NULL;
    return 0;
}

static int wait_event(void *context, uint32_t timeout, sxw_event *event) {
    (void)context;
    if (g_script_path == NULL) {
        if (timeout == SXW_INFINITE) {
            fprintf(stderr, "win32: the guest waits for input and there is no --input script\n");
            exit(125);
        }
        struct timespec pause = {(time_t)(timeout / 1000u), (long)(timeout % 1000u) * 1000000L};
        nanosleep(&pause, NULL);
        return 0;
    }
    if (timeout == 0u) {
        return 0;
    }
    for (;;) {
        if (g_wait_left != 0u) {
            const uint32_t step = timeout < g_wait_left ? timeout : g_wait_left;
            g_clock += step;
            g_wait_left -= step;
            return 0;
        }
        if (g_script == NULL) {
            if (timeout == SXW_INFINITE || g_after_script >= AFTER_SCRIPT_MS) {
                fprintf(stderr, "win32: the input script ended and the guest still waits\n");
                exit(125);
            }
            g_after_script += timeout;
            g_clock += timeout;
            return 0;
        }
        if (next_action(event)) {
            return 1;
        }
    }
}

static const sxw_window_platform g_window_platform = {
    .ticks = ticks,
    .display = display,
    .create = window_create,
    .destroy = window_destroy,
    .set_title = window_title,
    .set_icon = window_icon,
    .present = window_present,
    .wait_event = wait_event,
};

static sxw_services g_services = {
    .process = &g_process,
    .write = write_stream,
    .now = now_seconds,
    .registry = &g_registry,
    .windows = &g_window_platform,
};

uint32_t sxw_host_service(uint64_t guest_esp) {
    uint32_t result = sxw_service(&g_services, (uint32_t)guest_esp);
    if (g_services.exit_requested) {
        g_exit_status = g_services.exit_status;
        sxw_exit_requested = 1;
    }
    return result;
}

/* ---- Faults ------------------------------------------------------------- */

/* FS may still hold the TEB selector here, so no libc: raw system calls. */
static long raw_syscall3(long number, long a, long b, long c) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return result;
}

static void raw_hex(char *out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        unsigned nibble = (unsigned)(value >> (28 - 4 * i)) & 0xfu;
        out[i] = (char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
}

static void on_fault(int signal, siginfo_t *info, void *context) {
    (void)signal;
    ucontext_t *uc = context;
    char line[] = "win32: guest fault at eip 0x________, address 0x________\n";
    raw_hex(line + 28, (uint64_t)uc->uc_mcontext.gregs[REG_RIP]);
    raw_hex(line + 49, (uint64_t)(uintptr_t)info->si_addr);
    raw_syscall3(SYS_write, 2, (long)line, (long)sizeof(line) - 1);
    raw_syscall3(SYS_exit_group, 125, 0, 0);
}

/* ---- Set-up ------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

static void put64(uint8_t *p, uint64_t v) {
    memcpy(p, &v, 8);
}

static int build_trampoline(void) {
    if (platform_map(NULL, TRAMPOLINE, 0x1000) != 0) {
        return 1;
    }
    g_live_bytes -= 0x1000; /* not the loader's */
    uint8_t *t = (uint8_t *)(uintptr_t)TRAMPOLINE;
    /* back32 and service32: jmp far 0x33:landing */
    const uint32_t far_jumps[2][2] = {{TRAMPOLINE_BACK32, TRAMPOLINE_LANDING_RETURN},
                                      {TRAMPOLINE_SERVICE32, TRAMPOLINE_LANDING_SERVICE}};
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = t + far_jumps[i][0];
        p[0] = 0xea;
        put32(p + 1, TRAMPOLINE + far_jumps[i][1]);
        p[5] = LINUX_CODE64;
        p[6] = 0;
    }
    /* The landings: movabs r11, target; jmp r11 */
    const struct {
        uint32_t at;
        void (*target)(void);
    } landings[2] = {{TRAMPOLINE_LANDING_RETURN, sxw_gate_return},
                     {TRAMPOLINE_LANDING_SERVICE, sxw_gate_service}};
    for (int i = 0; i < 2; ++i) {
        uint8_t *p = t + landings[i].at;
        p[0] = 0x49;
        p[1] = 0xbb;
        put64(p + 2, (uint64_t)(uintptr_t)landings[i].target);
        p[10] = 0x41;
        p[11] = 0xff;
        p[12] = 0xe3;
    }
    /* svc_ret32: ret 8, the gate's two stdcall arguments */
    t[TRAMPOLINE_SVC_RET32 + 0] = 0xc2;
    t[TRAMPOLINE_SVC_RET32 + 1] = 0x08;
    t[TRAMPOLINE_SVC_RET32 + 2] = 0x00;
    sxw_svc_ret32 = TRAMPOLINE + TRAMPOLINE_SVC_RET32;
    return mprotect(t, 0x1000, PROT_READ | PROT_EXEC) == 0 ? 0 : 1;
}

static int set_teb_segment(uint32_t teb) {
    struct user_desc d;
    memset(&d, 0, sizeof(d));
    d.entry_number = TEB_LDT_ENTRY;
    d.base_addr = teb;
    d.limit = SXW_TEB_SIZE - 1;
    d.seg_32bit = 1;
    d.contents = 0; /* data, expand-up */
    d.read_exec_only = 0;
    d.limit_in_pages = 0;
    d.seg_not_present = 0;
    d.useable = 1;
    return syscall(SYS_modify_ldt, 1, &d, sizeof(d)) == 0 ? 0 : 1;
}

static void usage(void) {
    fprintf(stderr, "usage: win32-harness [--dlls DIR] [--registry FILE] [--time SECONDS] [--input SCRIPT] "
                    "program.exe [arguments...]\n");
    exit(2);
}

/* The arguments as a command-line tail: each quoted when it holds a blank
 * or is empty, which is what a program splitting it gets back. */
static void join_arguments(char **arguments, int count, char *out, size_t capacity) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < count; ++i) {
        int quote = arguments[i][0] == 0 || strpbrk(arguments[i], " \t") != NULL;
        size_t need = strlen(arguments[i]) + (quote ? 2u : 0u) + (i != 0 ? 1u : 0u);
        if (n + need + 1u > capacity) {
            break;
        }
        n += (size_t)snprintf(out + n, capacity - n, "%s%s%s%s", i != 0 ? " " : "", quote ? "\"" : "",
                              arguments[i], quote ? "\"" : "");
    }
}

static void load_registry(void) {
    sxw_reg_init(&g_registry);
    if (g_registry_path == NULL) {
        return;
    }
    file_buffer file;
    if (read_whole(g_registry_path, &file) == 0) {
        if (sxw_reg_parse(&g_registry, (const char *)file.data, file.size) != 0) {
            fprintf(stderr, "win32: %s is not a registry file; starting empty\n", g_registry_path);
        }
        free(file.data);
    }
    g_services.save_registry = save_registry_file;
}

int main(int argc, char **argv) {
    const char *exe = NULL;
    static char tail[2048];
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dlls") == 0 && i + 1 < argc) {
            g_dll_dir = argv[++i];
        } else if (strcmp(argv[i], "--registry") == 0 && i + 1 < argc) {
            g_registry_path = argv[++i];
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            g_services.fixed_time = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            g_script_path = argv[++i];
        } else if (argv[i][0] != '-') {
            exe = argv[i];
            join_arguments(argv + i + 1, argc - i - 1, tail, sizeof(tail));
            break;
        } else {
            usage();
        }
    }
    if (exe == NULL) {
        usage();
    }
    load_registry();
    clock_gettime(CLOCK_MONOTONIC, &g_started);
    if (g_script_path != NULL && (g_script = fopen(g_script_path, "r")) == NULL) {
        fprintf(stderr, "win32: cannot read %s\n", g_script_path);
        return 2;
    }
    const char *slash = strrchr(exe, '/');
    if (slash == NULL) {
        snprintf(g_exe_dir, sizeof(g_exe_dir), ".");
    } else {
        snprintf(g_exe_dir, sizeof(g_exe_dir), "%.*s", (int)(slash - exe), exe);
    }

    if (syscall(SYS_arch_prctl, 0x1003 /* ARCH_GET_FS */, &sxw_host_fsbase) != 0 ||
        build_trampoline() != 0) {
        fprintf(stderr, "win32: cannot set up the gate\n");
        return 126;
    }

    sxw_platform platform = {
        .context = NULL,
        .guest_low = 0x10000,
        .map = platform_map,
        .protect = platform_protect,
        .unmap = platform_unmap,
        .open_image = platform_open,
        .close_image = platform_close,
        .gate = TRAMPOLINE + TRAMPOLINE_SERVICE32,
    };
    if (sxw_load(&g_process, &platform, exe, tail) != 0) {
        fprintf(stderr, "win32: %s\n", g_process.diagnostic);
        if (g_live_bytes != 0 || g_open_files != 0) {
            fprintf(stderr, "win32: failed load left %llu bytes mapped and %d files open\n",
                    (unsigned long long)g_live_bytes, g_open_files);
            return 124;
        }
        fprintf(stderr, "win32: exit status 0x%08x\n", g_process.status);
        return (int)(g_process.status & 0xffu);
    }
    if (g_open_files != 0) {
        fprintf(stderr, "win32: %d files left open after the load\n", g_open_files);
        return 124;
    }
    if (set_teb_segment(g_process.teb) != 0) {
        fprintf(stderr, "win32: modify_ldt failed\n");
        return 126;
    }

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGFPE, &action, NULL);

    /* ntdll!SxwProcessStart(start block), returning to back32. */
    uint32_t esp = g_process.stack_top - 16u;
    put32((uint8_t *)(uintptr_t)esp, TRAMPOLINE + TRAMPOLINE_BACK32);
    put32((uint8_t *)(uintptr_t)(esp + 4u), g_process.start_block);
    sxw_services_start(&g_services);
    uint32_t returned = sxw_gate_enter(g_process.start_entry, esp);
    if (!sxw_exit_requested) {
        fprintf(stderr, "win32: SxwProcessStart returned 0x%08x without exiting\n", returned);
        return 125;
    }
    fprintf(stderr, "win32: exit status 0x%08x\n", g_exit_status);
    sxw_unload(&g_process);
    return (int)(g_exit_status & 0xffu);
}
