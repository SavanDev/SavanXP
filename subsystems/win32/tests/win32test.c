/*
 * win32test.sxe -- The win32-hello smoke scenario's runner: a SUN parent that
 * runs each fixture through winhost, captures its standard output through a
 * pipe and compares the bytes and the exit status with what Windows XP
 * produced for the same files. The expected bytes are the files under
 * /system/win/drive_c/tests/expected, the same ones the host harness test
 * reads (docs/userland/WIN32.md, "Fixtures"). startup.exe runs twice on one
 * registry file: the second run reads back what the first wrote.
 *
 * Each case is reported as a line of the system log, which reaches the
 * serial port; exits 0 when every case matches.
 *
 * With --window it runs the win32-window scenario instead: it starts
 * compositord and windowd with no desktop, then runs
 * window.exe and logs "WIN32 WINDOW READY". The host plays the fixture's
 * input script over QMP and checks the pixels (tools/win32_window_smoke.py);
 * here the output and the status are compared with Windows XP's.
 *
 * With --dialog it runs dialog.exe the same way for win32-dialog, logging
 * "WIN32 DIALOG READY" (tools/win32_dialog_smoke.py), and with --controls
 * controls.exe for win32-controls, logging "WIN32 CONTROLS READY"
 * (tools/win32_controls_smoke.py).
 *
 * After the console fixtures it starts windowd and runs the windowed ones:
 * hooks.exe, clip.exe, and the clipboard across subsystems -- "clip.exe
 * put" leaves its formats, this program (a SUN one) checks them on
 * /dev/clipboard and leaves the same set itself, and "clip.exe get" must
 * then print what Windows XP printed after put.
 *
 * With --drop it runs win32-drop (docs/desktop/DRAG_AND_DROP.md): it starts
 * windowd and "clip.exe drop", logging "WIN32 DROP READY", and once its
 * window is up a copy of itself with --source, a small SUN window painted
 * yellow that starts a drag of two files when the left button is pressed on
 * it. The host presses on the yellow window and releases over clip.exe's
 * (tools/win32_drop_smoke.py);
 * clip.exe must print clip-drop.out, the harness's, and the source must hear
 * that the drop took a copy.
 *
 * With --cards it runs cards.exe, which draws every card with the user's own
 * cards.dll; the win32-cards scenario copies that file next to it.
 *
 * With --solitaire it runs the user's own sol.exe from where it is installed
 * (the win32-solitaire scenario copies it and cards.dll there) twice under
 * windowd, on a fresh registry and with the clock fixed, so its first deal is
 * always the same; "WIN32 SOLITAIRE READY" tells the host to play
 * fixtures/solitaire/input-1.txt, and "WIN32 SOLITAIRE RUN 2" input-2.txt
 * (tools/win32_solitaire_smoke.py). Both runs must exit with 0, and the
 * registry the first leaves must equal expected/solitaire.reg.
 */
#include <stdint.h>
#include <stdlib.h>

#include "abi/savanxp_abi.h"
#include "abi/savanxp_native_abi.h"
#include "sxn/sxn.h"
#include "sxn/sxnclip.h"
#include "sxn/sxnwm.h"

#define WINHOST "/system/winhost.sxe"
#define TESTS "/system/win/drive_c/tests"
#define EXPECTED TESTS "/expected"
#define REGISTRY TESTS "/registry.reg"
#define SOLITAIRE "/system/win/drive_c/program files/solitaire/sol.exe"
#define SOLITAIRE_REGISTRY TESTS "/solitaire.reg"
/* The clock sol.exe seeds its deal from, 2001-08-24 02:56 UTC; the same as
 * tests/host/win32_harness_test.py's. */
#define SOLITAIRE_TIME "998621760"

typedef struct fixture {
    const char *name;
    const char *expected; /* a file under EXPECTED, or 0 for no output */
    uint32_t status;
    int registry;
    int with_arguments; /* alpha "two words" 3 */
    const char *ready;  /* logged once it runs, or 0 */
} fixture;

#define WINDOW_READY "WIN32 WINDOW READY"
#define DIALOG_READY "WIN32 DIALOG READY"
#define SOLITAIRE_READY "WIN32 SOLITAIRE READY"
#define SOLITAIRE_SECOND "WIN32 SOLITAIRE RUN 2"

static const fixture k_fixtures[] = {
    {"hello.exe", "hello.out", 42u, 0, 0, 0},
    {"exception.exe", "exception.out", 0u, 0, 0, 0},
    {"sehfail.exe", 0, 0xe0421003u, 0, 0, 0},
    {"sehscope.exe", "sehscope.out", 0u, 0, 0, 0},
    {"cxxthrow.exe", "cxxthrow.out", 0u, 0, 0, 0},
    {"cxxfail.exe", 0, 3u, 0, 0, 0},
    {"initorder.exe", "initorder.out", 7u, 0, 0, 0},
    {"missingdll.exe", 0, 0xc0000135u, 0, 0, 0},
    {"missingsym.exe", 0, 0xc0000139u, 0, 0, 0},
    {"startup.exe", "startup-1.out", 5u, 1, 1, 0},
    {"startup.exe", "startup-2.out", 5u, 1, 1, 0},
    {"resource.exe", "resource.out", 0u, 0, 0, 0},
    {"wstartup.exe", "wstartup.out", 5u, 1, 1, 0},
    {"files.exe", "files.out", 0u, 0, 0, 0},
    {"memory.exe", "memory.out", 0u, 0, 0, 0},
    {"locale.exe", "locale.out", 0u, 0, 0, 0},
    {"threads.exe", "threads.out", 5u, 0, 0, 0},
};

static const fixture k_windowed[] = {
    {"hooks.exe", "hooks.out", 0u, 0, 0, 0},
    {"clip.exe", "clip.out", 0u, 0, 0, 0},
};
static const fixture k_clip_put = {"clip.exe", "clip-put.out", 0u, 0, 0, 0};
/* gdi32's text over the host's faces in /system/fonts; text-savanxp.out is
 * the host harness's, which tests/host/win32_harness_test.py holds against
 * Windows XP's (docs/userland/WIN32.md, "Text"). */
static const fixture k_text = {"text.exe", "text-savanxp.out", 0u, 0, 0, 0};
static const fixture k_clip_get = {"clip.exe", "clip-get.out", 0u, 0, 0, 0};

#define DROP_READY "WIN32 DROP READY"
static const fixture k_clip_drop = {"clip.exe", "clip-drop.out", 0u, 0, 0, DROP_READY};

#define CONTROLS_READY "WIN32 CONTROLS READY"
static const fixture k_controls = {"controls.exe", "controls.out", 0u, 0, 0, CONTROLS_READY};
static const fixture k_window = {"window.exe", "window.out", 3u, 0, 0, WINDOW_READY};
static const fixture k_dialog = {"dialog.exe", "dialog.out", 5u, 0, 0, DIALOG_READY};
/* Needs the user's cards.dll next to it, which only the win32-cards
 * scenario puts there. */
static const fixture k_cards = {"cards.exe", "cards.out", 0u, 0, 0, 0};

static uint32_t length_of(const char *text) {
    uint32_t n = 0;
    while (text[n] != 0) {
        ++n;
    }
    return n;
}

static void say(const char *a, const char *b, const char *c) {
    char line[160];
    uint32_t n = 0;
    const char *parts[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        for (const char *p = parts[i]; *p != 0 && *p != '\n' && n + 1u < sizeof(line); ++p) {
            line[n++] = *p;
        }
    }
    line[n] = 0;
    (void)sxn_log(line);
}

static void join(char *out, uint32_t capacity, const char *dir, const char *name) {
    uint32_t n = 0;
    for (const char *p = dir; *p != 0 && n + 2u < capacity; ++p) {
        out[n++] = *p;
    }
    out[n++] = '/';
    for (const char *p = name; *p != 0 && n + 1u < capacity; ++p) {
        out[n++] = *p;
    }
    out[n] = 0;
}

/* A whole small file into buffer; its size, or -1. */
static long read_file(const char *path, char *buffer, uint32_t capacity) {
    long handle = sxn_open_file(path, SXN_FILE_READ);
    if (handle < 0) {
        return -1;
    }
    uint32_t total = 0;
    for (;;) {
        long count = sxn_read(handle, buffer + total, capacity - total);
        if (count <= 0) {
            break;
        }
        total += (uint32_t)count;
    }
    (void)sxn_close(handle);
    return (long)total;
}

/* Runs one fixture, with one argument or none; 1 when its output and
 * status match. */
static int run_with(const fixture *f, char *argument);

static int run(const fixture *f) {
    return run_with(f, 0);
}

/* Called once when the fixture has written "ready" (clip.exe drop, whose
 * window is then up), or 0. */
static void (*g_on_ready)(void);

/* Removes the lines from the one starting `from` up to the one starting
 * `to`: what text.exe prints for the families nobody has, which Windows
 * substitutes from whatever C:\windows\fonts holds. */
static void cut_lines(char *text, long *size, const char *from, const char *to) {
    long start = -1;
    for (long i = 0; i < *size; ++i) {
        if (i != 0 && text[i - 1] != '\n') {
            continue;
        }
        const char *marker = start < 0 ? from : to;
        long n = 0;
        while (marker[n] != 0 && i + n < *size && text[i + n] == marker[n]) {
            ++n;
        }
        if (marker[n] != 0) {
            continue;
        }
        if (start < 0) {
            start = i;
            continue;
        }
        for (long j = i; j < *size; ++j) {
            text[start + j - i] = text[j];
        }
        *size -= i - start;
        return;
    }
}

static int run_with(const fixture *f, char *argument) {
    static char output[196608];
    static char expected[196608];
    char path[128];
    join(path, sizeof(path), TESTS, f->name);

    long expected_size = 0;
    if (f->expected != 0) {
        char expected_path[128];
        join(expected_path, sizeof(expected_path), EXPECTED, f->expected);
        expected_size = read_file(expected_path, expected, sizeof(expected));
        if (expected_size < 0) {
            say("win32test: FAIL ", f->name, ": no expected output file");
            return 0;
        }
    }

    int32_t pipe[2] = {-1, -1};
    if (sxn_pipe_create(pipe) != 0) {
        say("win32test: FAIL ", f->name, ": no pipe");
        return 0;
    }
    char *argv[9];
    int argc = 0;
    argv[argc++] = WINHOST;
    if (f->registry) {
        argv[argc++] = "--registry";
        argv[argc++] = REGISTRY;
    }
    argv[argc++] = path;
    if (argument != 0) {
        argv[argc++] = argument;
    }
    if (f->with_arguments) {
        argv[argc++] = "alpha";
        argv[argc++] = "two words";
        argv[argc++] = "3";
    }
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, WINHOST, argv, argc);
    request.std_handles[1] = pipe[1];
    long child = sxn_spawn_ex(&request);
    (void)sxn_close(pipe[1]);
    if (child <= 0) {
        (void)sxn_close(pipe[0]);
        say("win32test: FAIL ", f->name, ": cannot start winhost");
        return 0;
    }
    if (f->ready != 0) {
        (void)sxn_log(f->ready);
    }
    uint32_t total = 0;
    for (;;) {
        long count = sxn_read(pipe[0], output + total, sizeof(output) - total);
        if (count <= 0) {
            break;
        }
        total += (uint32_t)count;
        for (uint32_t i = 0; g_on_ready != 0 && i + 5u <= total; ++i) {
            if (output[i] == 'r' && output[i + 1] == 'e' && output[i + 2] == 'a' && output[i + 3] == 'd' &&
                output[i + 4] == 'y') {
                void (*ready)(void) = g_on_ready;
                g_on_ready = 0;
                ready();
            }
        }
    }
    (void)sxn_close(pipe[0]);
    int status = -1;
    long reaped = sxn_waitpid(child, &status);

    if (f == &k_text) {
        long size = (long)total;
        cut_lines(output, &size, "missing:", "weights:");
        total = (uint32_t)size;
        cut_lines(expected, &expected_size, "missing:", "weights:");
    }
    int same = (long)total == expected_size;
    for (uint32_t i = 0; same && i < total; ++i) {
        same = output[i] == expected[i];
    }
    if (!same) {
        /* The first line that differs, both ways, for whoever reads the log. */
        uint32_t at = 0;
        uint32_t start = 0;
        while (at < total && (long)at < expected_size && output[at] == expected[at]) {
            if (output[at] == '\n') {
                start = at + 1u;
            }
            ++at;
        }
        char got[100];
        char wanted[100];
        uint32_t n = 0;
        for (uint32_t i = start; i < total && output[i] != '\n' && n + 1u < sizeof(got); ++i) {
            got[n++] = output[i];
        }
        got[n] = 0;
        n = 0;
        for (uint32_t i = start; (long)i < expected_size && expected[i] != '\n' && n + 1u < sizeof(wanted); ++i) {
            wanted[n++] = expected[i];
        }
        wanted[n] = 0;
        say("win32test: got    ", got, "");
        say("win32test: wanted ", wanted, "");
    }
    int ok = same && reaped == child && (uint32_t)status == f->status;
    say(ok ? "win32test: ok   " : "win32test: FAIL ", f->expected != 0 ? f->expected : f->name,
        !same ? " (output differs)" : (uint32_t)status != f->status ? " (status differs)" : "");
    return ok;
}

static int start(char *path) {
    char *argv[1] = {path};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, path, argv, 1);
    return sxn_spawn_ex(&request) > 0;
}

/* compositord and windowd as sessiond starts them, with no desktop. windowd
 * gives up when compositord's port is not there yet (under TCG it can take a
 * few seconds), so it is started again until its own port takes a
 * connection, as sessiond restarts it. */
static int start_windowd(void) {
    if (!start("/system/compositord.sxe")) {
        return 0;
    }
    long idle = sxn_event_create(0);
    for (int attempt = 0; attempt < 300; ++attempt) {
        if (attempt % 30 == 0 && !start("/system/windowd.sxe")) {
            break;
        }
        int32_t handles[2] = {-1, -1};
        if (sxn_port_connect("windowd", handles) == 0) {
            (void)sxn_close(handles[0]);
            (void)sxn_close(handles[1]);
            (void)sxn_close(idle);
            return 1;
        }
        (void)sxn_wait_one(idle, 100);
    }
    (void)sxn_close(idle);
    return 0;
}

/* The set "clip.exe put" leaves, as SavanXP's clipboard names it
 * (docs/desktop/CLIPBOARD.md): its text as UTF-8 with LF, its Rich Text
 * Format without the NUL, and its own format under Win32's prefix. */
static const char k_clip_text[] = "Hola\nmundo \xc3\xb1\xc3\xa9";
static const char k_clip_rtf[] = "{\\rtf1\\ansi Hola {\\b mundo}}";
static const char k_clip_private[] = "\x01\x02\x03\xfe";
static const char *const k_clip_names[3] = {SAVANXP_CLIPBOARD_TEXT, "text/rtf", "application/x-win32-SavanXP Test"};

/* What put left, read and written back by a SUN program: 1 when it is the
 * set above, in that order, and the same set was written again. */
static int clip_across(void) {
    const char *const data[3] = {k_clip_text, k_clip_rtf, k_clip_private};
    const uint32_t lengths[3] = {length_of(k_clip_text), length_of(k_clip_rtf), 4u};
    long handle = sxn_clipboard_open(1);
    if (handle < 0) {
        say("win32test: FAIL ", "clipboard", ": cannot be opened");
        return 0;
    }
    int ok = 1;
    struct savanxp_clipboard_info info;
    ok = sxn_clipboard_info(handle, &info) == 0 && info.format_count == 3u;
    for (uint32_t i = 0; ok && i < 3u; ++i) {
        char name[SAVANXP_CLIPBOARD_NAME_CAPACITY];
        uint32_t length = 0;
        ok = sxn_clipboard_format(handle, i, name, &length) == 0;
        for (uint32_t k = 0; ok && (k == 0 || k_clip_names[i][k - 1u] != 0); ++k) {
            ok = name[k] == k_clip_names[i][k];
        }
        char bytes[64];
        ok = ok && length == lengths[i] &&
             sxn_clipboard_get(handle, k_clip_names[i], bytes, sizeof(bytes), &length) == (long)lengths[i];
        for (uint32_t k = 0; ok && k < lengths[i]; ++k) {
            ok = bytes[k] == data[i][k];
        }
    }
    say(ok ? "win32test: ok   " : "win32test: FAIL ", "clipboard put by clip.exe", ok ? "" : " (set differs)");
    /* The same set, written by this program: get must not tell the two
     * apart. */
    struct savanxp_clipboard_entry entries[3];
    for (uint32_t i = 0; i < 3u; ++i) {
        uint32_t k = 0;
        for (; k_clip_names[i][k] != 0; ++k) {
            entries[i].name[k] = k_clip_names[i][k];
        }
        for (; k < SAVANXP_CLIPBOARD_NAME_CAPACITY; ++k) {
            entries[i].name[k] = 0;
        }
        entries[i].data = (uint64_t)(uintptr_t)data[i];
        entries[i].length = lengths[i];
    }
    const int written = sxn_clipboard_set(handle, 0, 0) == 0 && sxn_clipboard_set(handle, entries, 3) == 0;
    say(written ? "win32test: ok   " : "win32test: FAIL ", "clipboard set by win32test", "");
    (void)sxn_close(handle);
    return ok && written;
}

static int run_window(const fixture *f) {
    if (!start_windowd()) {
        say("win32test: FAIL ", "windowd", ": did not start");
        return 0;
    }
    return run(f);
}

/* sol.exe once under windowd on the solitaire registry; 1 when it exits 0. */
static int run_solitaire_once(int pass) {
    char *argv[] = {WINHOST, "--registry", SOLITAIRE_REGISTRY, "--time", SOLITAIRE_TIME, SOLITAIRE};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, WINHOST, argv, 6);
    long child = sxn_spawn_ex(&request);
    if (child <= 0) {
        say("win32test: FAIL ", "sol.exe", ": cannot start winhost");
        return 0;
    }
    (void)sxn_log(pass == 1 ? SOLITAIRE_READY : SOLITAIRE_SECOND);
    int status = -1;
    long reaped = sxn_waitpid(child, &status);
    const int ok = reaped == child && status == 0;
    say(ok ? "win32test: ok   " : "win32test: FAIL ", pass == 1 ? "sol.exe run 1" : "sol.exe run 2",
        ok ? "" : " (status differs)");
    return ok;
}

static int run_solitaire(void) {
    static const char empty[] = "SavanXP registry 1\n";
    if (sxn_write_file(SOLITAIRE_REGISTRY, empty, length_of(empty)) != 0) {
        say("win32test: FAIL ", SOLITAIRE_REGISTRY, ": cannot be written");
        return 0;
    }
    if (!start_windowd()) {
        say("win32test: FAIL ", "windowd", ": did not start");
        return 0;
    }
    int ok = run_solitaire_once(1);
    /* What run 1 saved: the deck back and the options. */
    static char got[1024];
    static char wanted[1024];
    const long got_size = read_file(SOLITAIRE_REGISTRY, got, sizeof(got));
    const long wanted_size = read_file(EXPECTED "/solitaire.reg", wanted, sizeof(wanted));
    int same = got_size >= 0 && got_size == wanted_size;
    for (long i = 0; same && i < got_size; ++i) {
        same = got[i] == wanted[i];
    }
    say(same ? "win32test: ok   " : "win32test: FAIL ", "solitaire.reg", same ? "" : " (registry differs)");
    ok = run_solitaire_once(2) && same && ok;
    return ok;
}

/* --source: a yellow window whose left-button press starts a drag of two
 * files; exits 0 when the drag ends with a copy, 1 otherwise or after a
 * minute. */
static int drag_source(void) {
    static const char uris[] = "file:///docs/notas.txt\r\nfile:///docs/mi%20carta.rtf\r\n";
    static const char text[] = "/docs/notas.txt\n/docs/mi carta.rtf";
    struct sxnwm_client wm;
    if (sxnwm_connect(&wm) != 0) {
        say("win32test: FAIL ", "drag source", ": no window");
        return 0;
    }
    /* Small: windowd centres it over clip.exe's window, which it must leave
     * uncovered at the marker and where the drop is. */
    (void)sxnwm_request_size(&wm, 16, 16);
    for (int round = 0; round < 20 && !sxnwm_check_resize(&wm); ++round) {
        (void)sxn_event_reset(wm.wake_fd);
        (void)sxn_wait_one(wm.wake_fd, 50);
    }
    for (uint32_t y = 0; y < wm.height; ++y) {
        for (uint32_t x = 0; x < wm.width; ++x) {
            wm.pixels[y * wm.stride + x] = 0x00ffff00u;
        }
    }
    (void)sxnwm_submit_full(&wm);
    int started = 0;
    const uint64_t deadline = sxn_clock_ns() + 60ull * 1000000000ull;
    while (sxn_clock_ns() < deadline) {
        struct savanxp_wm_event event;
        while (sxnwm_poll_input(&wm, &event) == 0) {
            if (event.kind == SAVANXP_WM_EVENT_POINTER && !started && (event.payload.pointer.buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
                struct savanxp_clipboard_entry entries[2];
                for (uint32_t i = 0; i < sizeof(entries); ++i) {
                    ((char *)entries)[i] = 0;
                }
                sxnwm_copy_text(entries[0].name, sizeof(entries[0].name), SAVANXP_CLIPBOARD_URI_LIST);
                entries[0].data = (uint64_t)(uintptr_t)uris;
                entries[0].length = length_of(uris);
                sxnwm_copy_text(entries[1].name, sizeof(entries[1].name), SAVANXP_CLIPBOARD_TEXT);
                entries[1].data = (uint64_t)(uintptr_t)text;
                entries[1].length = length_of(text);
                const long store = sxn_dragdata_open(1);
                const long set = store >= 0 ? sxn_clipboard_set(store, entries, 2) : store;
                if (store >= 0) {
                    (void)sxn_close(store);
                }
                started = set == 0 && sxnwm_drag_start(&wm, SAVANXP_DRAG_EFFECT_COPY) == 0;
                say("win32test: ", "drag source", started ? ": drag started" : ": cannot start the drag");
            } else if (event.kind == SAVANXP_WM_EVENT_DRAG && event.payload.drag.phase == SAVANXP_WM_DRAG_END) {
                const int copied = event.payload.drag.effect == SAVANXP_DRAG_EFFECT_COPY;
                say(copied ? "win32test: ok   " : "win32test: FAIL ", "drag source",
                    copied ? ": the drop took a copy" : ": the drag ended without a copy");
                return copied;
            }
        }
        (void)sxn_event_reset(wm.wake_fd);
        (void)sxn_wait_one(wm.wake_fd, 100);
    }
    say("win32test: FAIL ", "drag source", ": the drag never ended");
    return 0;
}

static const char *g_self;
static long g_source;

/* The source opens after clip.exe's window, so it is the one on top. */
static void start_source(void) {
    char *argv[2] = {(char *)g_self, "--source"};
    struct sxn_spawn_request request;
    sxn_spawn_request_init(&request, g_self, argv, 2);
    g_source = sxn_spawn_ex(&request);
}

static int run_drop(const char *self) {
    if (!start_windowd()) {
        say("win32test: FAIL ", "windowd", ": did not start");
        return 0;
    }
    g_self = self;
    g_on_ready = start_source;
    int ok = run_with(&k_clip_drop, "drop");
    if (g_source <= 0) {
        say("win32test: FAIL ", "drag source", ": cannot start");
        return 0;
    }
    int status = -1;
    ok = sxn_waitpid(g_source, &status) == g_source && status == 0 && ok;
    return ok;
}

int main(int argc, char **argv) {
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 's' && argv[1][3] == 'o') {
        sxn_exit(drag_source() ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'd' && argv[1][3] == 'r') {
        sxn_exit(run_drop(argv[0]) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'w') {
        sxn_exit(run_window(&k_window) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'd') {
        sxn_exit(run_window(&k_dialog) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'c' && argv[1][3] == 'o') {
        sxn_exit(run_window(&k_controls) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 'c') {
        sxn_exit(run(&k_cards) ? 0 : 1);
    }
    if (argc > 1 && argv[1][0] == '-' && argv[1][1] == '-' && argv[1][2] == 's') {
        sxn_exit(run_solitaire() ? 0 : 1);
    }
    /* A fresh registry, so the counter starts where Windows XP's did. */
    static const char empty[] = "SavanXP registry 1\n";
    if (sxn_write_file(REGISTRY, empty, length_of(empty)) != 0) {
        say("win32test: FAIL ", REGISTRY, ": cannot be written");
        sxn_exit(1);
    }
    int failures = 0;
    for (uint32_t i = 0; i < sizeof(k_fixtures) / sizeof(k_fixtures[0]); ++i) {
        failures += !run(&k_fixtures[i]);
    }
    /* The windowed fixtures, under windowd. */
    if (!start_windowd()) {
        say("win32test: FAIL ", "windowd", ": did not start");
        sxn_exit(1);
    }
    for (uint32_t i = 0; i < sizeof(k_windowed) / sizeof(k_windowed[0]); ++i) {
        failures += !run(&k_windowed[i]);
    }
    failures += !run_with(&k_clip_put, "put");
    failures += !clip_across();
    failures += !run_with(&k_clip_get, "get");
    failures += !run_with(&k_text, "liberation");
    sxn_exit(failures == 0 ? 0 : 1);
}
