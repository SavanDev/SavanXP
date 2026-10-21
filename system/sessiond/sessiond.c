#include <stdint.h>

#include "compositor_protocol.h"
#include "savanxp/wm_protocol.h"
#include "sxn/sxn.h"

/*
 * sessiond: the session manager (docs/desktop/DESKTOP_ARCHITECTURE.md#the-
 * session-manager-sessiond). SMSS and Winlogon/Userinit in one process: init
 * starts it, it starts compositord, then windowd, then the desktop, restarts
 * the desktop when it dies and the whole session when compositord or windowd
 * dies. Program Manager is not part of the session: it is an ordinary
 * application, reached from the Start menu.
 *
 * Native: it opens no file and only starts and watches its children, so it
 * needs nothing but spawn, waitpid, terminate, the clock and the log. Being
 * their parent is also what makes its desktop the shell: windowd grants the
 * role to a client whose parent is windowd's own parent, and compositord
 * takes as its window manager only a sibling.
 */

static const char* const k_compositord_path = "/bin/compositord";
static const char* const k_windowd_path = "/bin/windowd";
static const char* const k_desktop_path = "/bin/desktop";

/* A child that ends within this long after starting counts as a crash at
 * startup; after SESSIOND_MAX_QUICK_EXITS in a row it is not restarted, so a
 * broken binary cannot become a spin loop. */
#define SESSIOND_QUICK_EXIT_NS (10ull * 1000000000ull)
#define SESSIOND_MAX_QUICK_EXITS 3

/* How long compositord and windowd each get to open their port before the
 * session counts as failed. */
#define SESSIOND_READY_MS 20000ul
#define SESSIOND_READY_POLL_MS 50ul

/* After windowd or compositord dies, what is left of the session gets this long to notice
 * (the runtime treats the WM going away as a close request) before it is ended. */
#define SESSIOND_GRACE_MS 1000ul

/* The status init reads as "the session will not come up": it falls back to
 * the text shell instead of restarting sessiond. */
#define SESSIOND_EXIT_GAVE_UP 3

struct sessiond_child {
    const char* path;
    long pid;
    uint64_t started_ns;
    int quick_exits;
};

static struct sessiond_child g_compositord = {0, 0, 0ull, 0};
static struct sessiond_child g_windowd = {0, 0, 0ull, 0};
static struct sessiond_child g_desktop = {0, 0, 0ull, 0};

/* One event nobody sets: waiting on it with a timeout is a sleep. */
static long g_sleep_event = -1;

static void sessiond_sleep(unsigned long ms) {
    if (g_sleep_event >= 0) {
        (void)sxn_wait_one(g_sleep_event, (long)ms);
    }
}

static void log_line(const char* text) {
    (void)sxn_log(text);
}

static long start_child(struct sessiond_child* child) {
    char* argv[2];

    argv[0] = (char*)child->path;
    argv[1] = 0;
    /* Stdio only: its own handles are nothing a session program should hold. */
    child->pid = sxn_spawn_stdio(child->path, argv, 1);
    child->started_ns = sxn_clock_ns();
    if (child->pid < 0) {
        log_line("sessiond: a session program did not start");
        child->pid = 0;
        return -1;
    }
    return child->pid;
}

/* Counts an exit against the quick-exit limit. 1 if it may be restarted. */
static int may_restart(struct sessiond_child* child) {
    if (sxn_clock_ns() - child->started_ns < SESSIOND_QUICK_EXIT_NS) {
        child->quick_exits += 1;
    } else {
        child->quick_exits = 0;
    }
    return child->quick_exits < SESSIOND_MAX_QUICK_EXITS;
}

/*
 * windowd connects to compositord's port and the session's clients to
 * windowd's, so each may only start once the one before has opened its port.
 * A connection that hangs up before its first message is dropped by both, so
 * probing with one costs nothing. 0 when ready, -1 if it never got there.
 */
static int wait_port_ready(const char* name) {
    unsigned long waited = 0;

    while (waited < SESSIOND_READY_MS) {
        int32_t handles[2] = {-1, -1};
        long result = sxn_port_connect(name, handles);

        if (result == 0) {
            (void)sxn_close(handles[0]);
            (void)sxn_close(handles[1]);
            return 0;
        }
        sessiond_sleep(SESSIOND_READY_POLL_MS);
        waited += SESSIOND_READY_POLL_MS;
    }
    return -1;
}

/* Ends a child that may still be running and reaps it. */
static void end_child(struct sessiond_child* child) {
    int status = 0;

    if (child->pid <= 0) {
        return;
    }
    (void)sxn_terminate(child->pid, 1);
    (void)sxn_waitpid(child->pid, &status);
    child->pid = 0;
}

static void start_shell(void) {
    if (start_child(&g_desktop) < 0) {
        g_desktop.quick_exits += 1;
    }
}

/* 0 once compositord and windowd are up and the shell started, -1 if one of
 * them did not come up. */
static int start_session(void) {
    if (start_child(&g_compositord) < 0) {
        return -1;
    }
    if (wait_port_ready(SAVANXP_COMPOSITOR_PORT_NAME) < 0) {
        log_line("sessiond: compositord did not open its port");
        end_child(&g_compositord);
        return -1;
    }
    if (start_child(&g_windowd) < 0) {
        end_child(&g_compositord);
        return -1;
    }
    if (wait_port_ready(SAVANXP_WM_PORT_NAME) < 0) {
        log_line("sessiond: windowd did not open its port");
        end_child(&g_windowd);
        end_child(&g_compositord);
        return -1;
    }
    start_shell();
    log_line("sessiond: session started");
    return 0;
}

/* compositord or windowd is gone: give the rest a moment to close on their
 * own (each of the two ends without the other), then end it. */
static void end_session(void) {
    sessiond_sleep(SESSIOND_GRACE_MS);
    end_child(&g_desktop);
    end_child(&g_windowd);
    end_child(&g_compositord);
}

int main(void) {
    g_compositord.path = k_compositord_path;
    g_windowd.path = k_windowd_path;
    g_desktop.path = k_desktop_path;
    g_sleep_event = sxn_event_create(SXN_EVENT_MANUAL_RESET);

    for (;;) {
        int status = 0;
        long pid = 0;

        if (g_windowd.pid <= 0) {
            if (start_session() < 0 && !may_restart(&g_windowd)) {
                log_line("sessiond: the session will not come up; giving up");
                sxn_exit(SESSIOND_EXIT_GAVE_UP);
            }
            continue;
        }

        pid = sxn_waitpid(-1, &status);
        if (pid < 0) {
            /* No child: nothing to watch. Start over. */
            g_windowd.pid = 0;
            continue;
        }

        if (pid == g_windowd.pid || pid == g_compositord.pid) {
            /* Either one takes the session with it: the scene lives in both
             * and neither reconnects. Both count against the same limit, on
             * windowd's record. */
            log_line(pid == g_windowd.pid ? "sessiond: windowd exited; restarting the session"
                                          : "sessiond: compositord exited; restarting the session");
            if (pid == g_windowd.pid) {
                g_windowd.pid = 0;
            } else {
                g_compositord.pid = 0;
            }
            end_session();
            if (!may_restart(&g_windowd)) {
                log_line("sessiond: the session keeps ending; giving up");
                sxn_exit(SESSIOND_EXIT_GAVE_UP);
            }
            /* The new desktop is a new start, not one more crash of the old. */
            g_desktop.quick_exits = 0;
        } else if (pid == g_desktop.pid) {
            g_desktop.pid = 0;
            if (may_restart(&g_desktop)) {
                log_line("sessiond: the desktop exited; restarting it");
                (void)start_child(&g_desktop);
            } else if (g_desktop.quick_exits == SESSIOND_MAX_QUICK_EXITS) {
                /* windowd lives on with a solid background; Alt+Tab still works. */
                log_line("sessiond: the desktop keeps exiting at startup; going on without it");
            }
        }
    }
}
