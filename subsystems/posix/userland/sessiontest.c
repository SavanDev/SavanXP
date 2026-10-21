#include "libc.h"

/*
 * session-smoke: the session manager end to end
 * (docs/desktop/DESKTOP_ARCHITECTURE.md#the-session-manager-sessiond).
 *
 * Starts /bin/sessiond and watches its children through the process table:
 * compositord, windowd and the desktop come up, and Program Manager does not
 * (it is an ordinary application, started from the Start menu); the desktop
 * stays up, which it only does if windowd granted it the shell role
 * (gfx_open_shell fails otherwise and it exits); killed, the desktop comes
 * back; killed, windowd takes the session with it and the whole session comes
 * back; and so does compositord.
 */

#define SESSIONTEST_START_MS 60000ul
#define SESSIONTEST_STAYS_UP_MS 3000ul

static int g_failures = 0;

static void fail(const char *what)
{
    printf("SESSION SMOKE FAIL %s\n", what);
    g_failures += 1;
}

/* A live (not zombie) child of `parent` named `name` other than `not_pid`, or 0. */
static long find_child(long parent, const char *name, long not_pid)
{
    struct savanxp_process_info info;
    unsigned long index = 0;

    while (proc_info(index, &info) > 0)
    {
        ++index;
        if (info.state == SAVANXP_PROC_UNUSED || info.state == SAVANXP_PROC_ZOMBIE)
        {
            continue;
        }
        info.name[sizeof(info.name) - 1u] = '\0';
        if ((long)info.parent_pid == parent && (long)info.pid != not_pid && strcmp(info.name, name) == 0)
        {
            return (long)info.pid;
        }
    }
    return 0;
}

static long wait_child(long parent, const char *name, long not_pid)
{
    unsigned long deadline = uptime_ms() + SESSIONTEST_START_MS;

    while (uptime_ms() < deadline)
    {
        long pid = find_child(parent, name, not_pid);

        if (pid > 0)
        {
            return pid;
        }
        sleep_ms(50);
    }
    return 0;
}

static int alive(long pid)
{
    struct savanxp_process_info info;
    unsigned long index = 0;

    while (proc_info(index, &info) > 0)
    {
        ++index;
        if ((long)info.pid == pid)
        {
            return info.state != SAVANXP_PROC_UNUSED && info.state != SAVANXP_PROC_ZOMBIE;
        }
    }
    return 0;
}

/* The desktop exits at once when it is refused the shell role: still being
 * there after a while is the proof it got it. */
static int stays_up(long pid)
{
    unsigned long deadline = uptime_ms() + SESSIONTEST_STAYS_UP_MS;

    while (uptime_ms() < deadline)
    {
        if (!alive(pid))
        {
            return 0;
        }
        sleep_ms(100);
    }
    return 1;
}

int main(void)
{
    const char *argv[] = {"/bin/sessiond", 0};
    long sessiond = spawn("/bin/sessiond", argv, 1);
    long compositord = 0;
    long windowd = 0;
    long desktop = 0;
    long next = 0;
    int status = 0;

    if (sessiond < 0)
    {
        fail("sessiond did not start");
        return 1;
    }

    compositord = wait_child(sessiond, "compositord", 0);
    windowd = wait_child(sessiond, "windowd", 0);
    desktop = wait_child(sessiond, "desktop", 0);
    if (compositord <= 0 || windowd <= 0 || desktop <= 0)
    {
        fail("the session did not come up");
        return 1;
    }
    if (!stays_up(desktop))
    {
        fail("the desktop did not stay up (shell role refused?)");
    }
    if (find_child(sessiond, "progman", 0) > 0)
    {
        fail("the session started Program Manager");
    }
    printf("SESSION SMOKE up windowd=%ld desktop=%ld\n", windowd, desktop);

    /* The desktop dies: sessiond starts another, which becomes the shell. */
    (void)savanxp_kill((int)desktop, SAVANXP_SIGKILL);
    next = wait_child(sessiond, "desktop", desktop);
    if (next <= 0 || !stays_up(next))
    {
        fail("the desktop did not come back");
    }
    else if (!alive(windowd))
    {
        fail("windowd went down with the desktop");
    }
    desktop = next;
    printf("SESSION SMOKE desktop restarted pid=%ld\n", desktop);

    /* windowd dies: the session is over and starts again, all of it. */
    (void)savanxp_kill((int)windowd, SAVANXP_SIGKILL);
    next = wait_child(sessiond, "windowd", windowd);
    if (next <= 0)
    {
        fail("windowd did not come back");
    }
    windowd = next;
    next = wait_child(sessiond, "desktop", desktop);
    if (next <= 0 || !stays_up(next))
    {
        fail("the desktop did not come back with the session");
    }
    else if (alive(desktop))
    {
        fail("the old desktop outlived its session");
    }
    desktop = next;
    printf("SESSION SMOKE session restarted windowd=%ld desktop=%ld\n", windowd, desktop);

    /* compositord dies: windowd cannot go on without it, and the session
     * starts again with a new pair. */
    compositord = wait_child(sessiond, "compositord", compositord);
    (void)savanxp_kill((int)compositord, SAVANXP_SIGKILL);
    next = wait_child(sessiond, "compositord", compositord);
    if (next <= 0)
    {
        fail("compositord did not come back");
    }
    compositord = next;
    next = wait_child(sessiond, "windowd", windowd);
    if (next <= 0)
    {
        fail("windowd did not come back with compositord");
    }
    else if (alive(windowd))
    {
        fail("the old windowd outlived its compositord");
    }
    windowd = next;
    next = wait_child(sessiond, "desktop", desktop);
    if (next <= 0 || !stays_up(next))
    {
        fail("the desktop did not come back after compositord");
    }
    desktop = next;
    printf("SESSION SMOKE compositord restarted compositord=%ld windowd=%ld\n", compositord, windowd);

    /* Done: sessiond first, so nothing is restarted; its children are orphans
     * then, reaped by init. */
    (void)savanxp_kill((int)sessiond, SAVANXP_SIGKILL);
    (void)savanxp_waitpid((int)sessiond, &status);
    (void)savanxp_kill((int)desktop, SAVANXP_SIGKILL);
    (void)savanxp_kill((int)windowd, SAVANXP_SIGKILL);
    (void)savanxp_kill((int)compositord, SAVANXP_SIGKILL);

    return g_failures == 0 ? 0 : 1;
}
