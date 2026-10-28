#pragma once

#include <stdint.h>

/*
 * WM <-> shell client protocol (the `desktop` process).
 *
 * An OPTIONAL extension of savanxp/wm_protocol.h. Only the connection the WM
 * recognizes as the shell gets it: the window list section and a request pipe
 * that stays open. Channels are paid PER CLIENT against the limit of 64 per
 * process (see docs/desktop/WM_SUBSYSTEM.md), so this is one descriptor for
 * the whole system, not one per window.
 *
 * Why it exists: the taskbar is a CLIENT, not WM chrome -- the explorer.exe
 * model (docs/desktop/DESKTOP_ARCHITECTURE.md). But a client cannot know which
 * windows exist: the z-order, the titles and the focus belong to the WM. This
 * is the channel through which it tells.
 *
 * Having the shell role is also what lets a process create windows with a
 * role other than NORMAL (savanxp_wm_window_role) and register global hotkeys.
 * A client asks for it in its hello (SAVANXP_WM_HELLO_SHELL, gfx_open_shell)
 * and the WM grants it to a process the session started -- its parent is the
 * WM's parent or the WM itself -- one at a time
 * (docs/desktop/DESKTOP_ARCHITECTURE.md#who-is-the-shell-decided-by-parentage).
 */

/*
 * The window list: a shared SECTION, not a pipe.
 *
 * It weighs more than ten KiB -- every entry carries its icon -- and an 8 KiB
 * pipe does not take it whole. A partial read desynchronizes the stream for
 * good. With a section there are no messages to split: the WM overwrites the
 * content and the client reads the latest state, which is exactly what a
 * taskbar needs -- not the history, only the now.
 *
 * Protocol v5: it arrives as the last handle before the welcome on the shell's
 * event pipe (savanxp/wm_protocol.h). The client maps it READ-ONLY.
 *
 * The shell's requests (savanxp_wm_shell_request) go on its request pipe, the
 * one the hello went on: small, sporadic messages produced by clicks, in the
 * direction without the "the WM never blocks" restriction.
 */

/* 3: the descriptors moved to 7 and 8 with v4 of savanxp/wm_protocol.h.
 * 4: hotkey registration; the request carries a key and its modifiers.
 * 5: no fixed descriptors; the list arrives as a handle and requests go on the
 *    connection's request pipe (protocol v5). */
#define SAVANXP_WM_SHELL_PROTOCOL_VERSION 5u
#define SAVANXP_WM_WINDOW_TITLE_CAPACITY 64u
/* Same as WINDOWD_MAX_OVERLAY_CLIENTS: every task is an application window. */
#define SAVANXP_WM_MAX_WINDOWS 12u

/* Same size as WINDOWD_PRESENTATION_ICON_EXTENT: the entry carries the small
 * icon exactly as the WM already resolved it, without rescaling. */
#define SAVANXP_WM_WINDOW_ICON_EXTENT 16u
#define SAVANXP_WM_WINDOW_ICON_PIXELS \
    (SAVANXP_WM_WINDOW_ICON_EXTENT * SAVANXP_WM_WINDOW_ICON_EXTENT)

enum savanxp_wm_window_flags {
    SAVANXP_WM_WINDOW_FLAG_NONE = 0,
    SAVANXP_WM_WINDOW_FLAG_ACTIVE = 1u << 0,
    SAVANXP_WM_WINDOW_FLAG_MINIMIZED = 1u << 1,
    /* Asked to close, and still running after a while (the WM never ends a
     * process). The shell offers to end it; window_id is its pid. */
    SAVANXP_WM_WINDOW_FLAG_NOT_RESPONDING = 1u << 2,
};

struct savanxp_wm_window_entry {
    /* Stable identity while the window lives. It is NOT the index in the
     * array: the array is reordered with the z-order, so a click arriving
     * with a stale index would activate ANOTHER window. The WM resolves the id
     * against its clients when it receives the request. */
    uint32_t window_id;
    uint32_t flags; /* savanxp_wm_window_flags */
    /* Last resort when the window brought no icon of its own: the client
     * draws it with desktop_icon_small(). */
    uint32_t icon_id;
    /* 0 = no icon of its own, icon_id is used. Otherwise, the side of
     * icon_pixels.
     *
     * The PIXELS travel rather than a reference to the binary because the WM
     * already read them from the .sxe when it created the window
     * (docs/userland/SXE_FORMAT.md): sending the path would force the shell
     * to reopen and parse every executable itself, and keep its own cache, to
     * arrive at exactly the same bitmap. It costs 1 KiB per entry -- a 13 KiB
     * section -- and that is the whole debt.
     */
    uint32_t icon_extent;
    char title[SAVANXP_WM_WINDOW_TITLE_CAPACITY];
    /* BGRA8888, icon_extent x icon_extent, at the top of the array. */
    uint32_t icon_pixels[SAVANXP_WM_WINDOW_ICON_PIXELS];
};

/*
 * Seqlock. The WM bumps `sequence` to odd before touching the entries and to
 * even when it is done; the client reads the sequence, copies, and reads it
 * again: if it changed or is odd, it retries. It is really needed -- the WM is
 * another process and can be preempted in the middle of the write.
 */
struct savanxp_wm_window_list {
    uint32_t version; /* SAVANXP_WM_SHELL_PROTOCOL_VERSION */
    uint32_t count;
    uint64_t sequence;
    struct savanxp_wm_window_entry windows[SAVANXP_WM_MAX_WINDOWS];
};

enum savanxp_wm_shell_action {
    /* Bring to the front, restoring it if it was minimized. */
    SAVANXP_WM_SHELL_ACTIVATE = 1,
    /* Minimize. It is what a click on the button of the window that is ALREADY
     * active does, as in Win95. */
    SAVANXP_WM_SHELL_MINIMIZE = 2,
    /* Register a global hotkey (RegisterHotKey): `key` with exactly the
     * modifiers in `modifiers` (SAVANXP_KEY_MOD_SHIFT, _CTRL, _ALT; lock keys
     * are ignored). From then on the WM delivers that key -- down and up -- to
     * the shell's main window, whichever window has focus, and nobody else
     * sees it. The combinations the WM keeps for itself (Alt+Tab, Alt+F4, F11)
     * cannot be registered; the request is dropped with a log line.
     * `window_id` is unused. */
    SAVANXP_WM_SHELL_REGISTER_HOTKEY = 3,
};

struct savanxp_wm_shell_request {
    uint32_t action; /* savanxp_wm_shell_action */
    uint32_t window_id;
    uint32_t key;       /* REGISTER_HOTKEY: enum savanxp_key_code */
    uint32_t modifiers; /* REGISTER_HOTKEY: enum savanxp_key_modifier */
};
