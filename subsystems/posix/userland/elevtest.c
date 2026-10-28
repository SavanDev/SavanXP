#include "libc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elevation/elevation_protocol.h"
#include "sxn/sxn.h"

/*
 * elevation-smoke: phase 3 of docs/kernel/USERS_AND_PERMISSIONS.md, end to
 * end. Starts the broker and the session, then:
 *
 *   - as the system: /dev/input0 is windowd's (a read and a claim are
 *     -EBUSY), and asking for elevation starts the program without a prompt;
 *   - lowered to the user (uid 1000): it cannot read the keyboard, windowd
 *     refuses to show it a prompt of its own, and it asks twice. The first
 *     time it also writes a forged "yes" down its pipe; the host presses N
 *     (tools/qmp_elevation.py), and the answer must be no. The second time the
 *     host presses Y, and the program runs elevated: uid 1000, euid 0, able
 *     to write /system, and its exit status comes back.
 *
 *     elevtest              the test (as init's smoke command)
 *     elevtest --elevated   what the broker starts: checks its own token
 *
 * The keys are real: QMP -> PS/2 -> /dev/input0 -> windowd. windowd logs
 * "consent prompt shown" when the prompt is up, which is the host's cue.
 */

#define ELEVTEST_USER 1000u
#define ELEVTEST_GROUP 100u
#define ELEVTEST_FLAG "/system/elevtest.flag"
#define ELEVTEST_START_MS 60000ul

static int g_failures = 0;

static void check(int ok, const char *what)
{
    if (!ok)
    {
        printf("ELEVATION SMOKE FAIL %s\n", what);
        g_failures += 1;
    }
}

/* --- The elevated program ---------------------------------------------- */

static int run_elevated(void)
{
    struct stat info;
    int fd;

    check(getuid() == ELEVTEST_USER, "elevated: the real uid is still the user's");
    check(geteuid() == 0, "elevated: it acts as the system");
    check(getgid() == ELEVTEST_GROUP, "elevated: the group is the user's");

    /* What the user may not do and the system may. */
    fd = open(ELEVTEST_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "elevated: it creates a file in /system");
    if (fd >= 0)
    {
        close(fd);
        /* Files an elevated program creates are the system's. */
        check(stat(ELEVTEST_FLAG, &info) == 0 && info.st_uid == 0, "elevated: the file belongs to the system");
        check(unlink(ELEVTEST_FLAG) == 0, "elevated: it removes it");
    }
    printf("ELEVTEST elevated: uid %u euid %u\n", (unsigned)getuid(), (unsigned)geteuid());
    return g_failures == 0 ? 0 : 1;
}

/* --- Setup ------------------------------------------------------------- */

/* A port is ready once a connection to it goes through. The services drop a
 * connection that hangs up without a word. */
static int wait_port(const char *name)
{
    unsigned long deadline = uptime_ms() + ELEVTEST_START_MS;

    while (uptime_ms() < deadline)
    {
        int32_t handles[2] = {-1, -1};

        if (sxn_port_connect(name, handles) == 0)
        {
            (void)savanxp_close(handles[0]);
            (void)savanxp_close(handles[1]);
            return 0;
        }
        sleep_ms(50);
    }
    return -1;
}

static void fill_request(struct savanxp_elevation_request *request)
{
    static const char args[] = "elevtest\0--elevated";

    memset(request, 0, sizeof(*request));
    request->magic = SAVANXP_ELEVATION_MAGIC;
    request->version = SAVANXP_ELEVATION_VERSION;
    request->flags = SAVANXP_ELEVATION_WAIT;
    request->argc = 2;
    memcpy(request->path, "/bin/elevtest", sizeof("/bin/elevtest"));
    memcpy(request->args, args, sizeof(args));
}

/* --- As the user --------------------------------------------------------- */

/* windowd hears consent requests from the system only. */
static void ask_windowd_directly(void)
{
    struct savanxp_consent_request request;
    struct savanxp_consent_reply reply;
    int32_t handles[2] = {-1, -1};

    check(sxn_port_connect(SAVANXP_CONSENT_PORT_NAME, handles) == 0, "the user reaches the consent port");
    if (handles[0] < 0)
    {
        return;
    }
    memset(&request, 0, sizeof(request));
    request.magic = SAVANXP_CONSENT_MAGIC;
    request.version = SAVANXP_ELEVATION_VERSION;
    request.requester_pid = (uint32_t)getpid();
    memcpy(request.path, "/bin/elevtest", sizeof("/bin/elevtest"));
    (void)savanxp_write(handles[0], &request, sizeof(request));
    memset(&reply, 0, sizeof(reply));
    check(savanxp_read(handles[1], &reply, sizeof(reply)) == (long)sizeof(reply) &&
              reply.magic == SAVANXP_CONSENT_MAGIC && reply.answer == -SAVANXP_EACCES,
        "windowd refuses to prompt for the user");
    (void)savanxp_close(handles[0]);
    (void)savanxp_close(handles[1]);
}

/* Asks, and right behind the request writes what a forged answer would look
 * like -- windowd's and the broker's. The host presses N: the answer has to
 * be no, whatever the pipe carried. */
static void ask_and_forge(void)
{
    struct savanxp_elevation_request request;
    struct savanxp_elevation_reply reply;
    struct savanxp_consent_reply forged_consent;
    struct savanxp_elevation_reply forged_reply;
    int32_t handles[2] = {-1, -1};

    check(sxn_port_connect(SAVANXP_ELEVATION_PORT_NAME, handles) == 0, "the user reaches the broker");
    if (handles[0] < 0)
    {
        return;
    }
    fill_request(&request);
    forged_consent.magic = SAVANXP_CONSENT_MAGIC;
    forged_consent.answer = 1;
    memset(&forged_reply, 0, sizeof(forged_reply));
    forged_reply.magic = SAVANXP_ELEVATION_MAGIC;
    forged_reply.record = SAVANXP_ELEVATION_RECORD_STARTED;
    forged_reply.status = 1;
    (void)savanxp_write(handles[0], &request, sizeof(request));
    (void)savanxp_write(handles[0], &forged_consent, sizeof(forged_consent));
    (void)savanxp_write(handles[0], &forged_reply, sizeof(forged_reply));

    memset(&reply, 0, sizeof(reply));
    check(savanxp_read(handles[1], &reply, sizeof(reply)) == (long)sizeof(reply) &&
              reply.magic == SAVANXP_ELEVATION_MAGIC && reply.record == SAVANXP_ELEVATION_RECORD_STARTED,
        "the broker answers the first request");
    check(reply.status == -SAVANXP_EACCES, "the first request is declined: the forged yes was ignored");
    (void)savanxp_close(handles[0]);
    (void)savanxp_close(handles[1]);
}

static int run_user(void)
{
    const char *elevated_argv[] = {"elevtest", "--elevated", 0};
    int status = -1;
    long pid;

    check(setgid(ELEVTEST_GROUP) == 0 && setuid(ELEVTEST_USER) == 0, "lowering to the user");
    check(geteuid() == ELEVTEST_USER, "the child is the user");

    check(open("/dev/input0", O_RDONLY) < 0 && errno == EACCES, "the user cannot open the keyboard");
    ask_windowd_directly();
    ask_and_forge();

    pid = elevate("/bin/elevtest", elevated_argv, 2, &status);
    check(pid > 0, "the second request is accepted");
    check(pid > 0 && status == 0, "the elevated program passed its checks");

    /* Asking changed nothing about the asker. */
    check(getuid() == ELEVTEST_USER && geteuid() == ELEVTEST_USER, "the requester is still the user");
    check(open(ELEVTEST_FLAG, O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES,
        "the requester still cannot write /system");
    return g_failures == 0 ? 0 : 1;
}

/* --- As the system ------------------------------------------------------- */

static void check_input_owned(void)
{
    struct savanxp_input_event event;
    long fd = savanxp_open_mode("/dev/input0", SAVANXP_OPEN_READ);

    check(fd >= 0, "the system opens /dev/input0");
    if (fd < 0)
    {
        return;
    }
    /* Opening is allowed (the desktop does, for the layout); reading is
     * windowd's. */
    check(savanxp_read((int)fd, &event, sizeof(event)) == -SAVANXP_EBUSY, "a read of windowd's keyboard is -EBUSY");
    check(savanxp_ioctl((int)fd, INPUT_IOC_CLAIM, 0) == -SAVANXP_EBUSY, "a second claim is -EBUSY");
    check(savanxp_ioctl((int)fd, INPUT_IOC_GET_LAYOUT, 0) >= 0, "the layout ioctl still works");
    (void)savanxp_close((int)fd);
}

int main(int argc, char **argv)
{
    const char *elevationd_argv[] = {"/bin/elevationd.sxe", 0};
    const char *sessiond_argv[] = {"/bin/sessiond.sxe", 0};
    const char *true_argv[] = {"true", 0};
    long elevationd = 0;
    long sessiond = 0;
    long child = 0;
    int status = -1;

    if (argc > 1 && strcmp(argv[1], "--elevated") == 0)
    {
        return run_elevated();
    }

    elevationd = spawn("/bin/elevationd.sxe", elevationd_argv, 1);
    sessiond = spawn("/bin/sessiond.sxe", sessiond_argv, 1);
    check(elevationd > 0 && sessiond > 0, "starting elevationd and sessiond");
    check(wait_port(SAVANXP_ELEVATION_PORT_NAME) == 0, "the broker opened its port");
    check(wait_port(SAVANXP_CONSENT_PORT_NAME) == 0, "windowd opened the consent port");
    if (g_failures != 0)
    {
        puts_out("ELEVATION SMOKE FAIL setup\n");
        return 1;
    }

    check_input_owned();

    /* The system is not asked: it could start it itself. */
    {
        long pid = elevate("/bin/true", true_argv, 1, &status);

        check(pid > 0 && status == 0, "the system's request runs without a prompt");
    }

    child = savanxp_fork();
    check(child >= 0, "fork");
    if (child == 0)
    {
        exit(run_user());
    }
    if (child > 0)
    {
        check(savanxp_waitpid((int)child, &status) >= 0 && status == 0, "the checks as the user");
    }

    (void)savanxp_kill((int)sessiond, SAVANXP_SIGKILL);
    (void)savanxp_kill((int)elevationd, SAVANXP_SIGKILL);
    puts_out(g_failures == 0 ? "ELEVATION SMOKE PASS\n" : "ELEVATION SMOKE FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
