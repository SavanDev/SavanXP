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
 *     refuses to show it a prompt of its own, the names of the system's
 *     services are not its to take, and it asks six times
 *     (tools/qmp_elevation.py answers):
 *       1. with a forged "yes" written down its pipe behind the request; the
 *          host presses N, and the answer must be no;
 *       2. while the host holds Y down, so the prompt sees only its repeats,
 *          then presses N: no again;
 *       3. the host presses Y, and the program runs elevated: uid 1000,
 *          euid 0, able to write /system, and its exit status comes back;
 *       4. Add or Remove Programs' elevated half, `appwiz --uninstall`, on a
 *          program the system put in /bin and the user cannot delete; the
 *          host presses Y, and it is gone;
 *       5. Task Manager's elevated half, `taskmgr --end`, on a process the
 *          system started and the user's kill is refused (EPERM); the host
 *          presses Y, and the process ends;
 *       6. Disk Management's elevated half, `diskmgmt --rescan`, which reads
 *          again the partition table of every disk with nothing mounted and
 *          changes nothing; the host presses Y, and it reports success.
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
/* Not an ELF: Add or Remove Programs lists and removes it all the same. */
#define ELEVTEST_APP "/bin/elevtest-app"
#define ELEVTEST_START_MS 60000ul

static int g_failures = 0;
/* A process of the system's, started before the fork, for the user to end. */
static long g_victim = 0;

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
    request.version = SAVANXP_CONSENT_VERSION;
    request.requester_pid = (uint32_t)getpid();
    request.argc = 1;
    memcpy(request.args, "elevtest", sizeof("elevtest"));
    memcpy(request.path, "/bin/elevtest", sizeof("/bin/elevtest"));
    (void)savanxp_write(handles[0], &request, sizeof(request));
    memset(&reply, 0, sizeof(reply));
    check(savanxp_read(handles[1], &reply, sizeof(reply)) == (long)sizeof(reply) &&
              reply.magic == SAVANXP_CONSENT_MAGIC && reply.answer == -SAVANXP_EACCES,
        "windowd refuses to prompt for the user");
    (void)savanxp_close(handles[0]);
    (void)savanxp_close(handles[1]);
}

/* A service's name is the system's to take, running or not, and so is
 * everything under "sys."; the user's ports are its own to name, up to the
 * share of the table left to them (KERNEL_SECURITY.md, E3). */
static void take_service_names(void)
{
    long ports[16];
    int count = 0;
    long last = 0;

    check(sxn_port_create(SAVANXP_CONSENT_PORT_NAME) == -SAVANXP_EPERM,
        "the user cannot take the name of a running service");
    check(sxn_port_create("audiod") == -SAVANXP_EPERM, "the user cannot take a service's name while it is free");
    check(sxn_port_create("sys.elevtest") == -SAVANXP_EPERM, "the user cannot take a name under sys.");

    for (count = 0; count < 16; ++count)
    {
        char name[] = "elevtest-user-a";

        name[sizeof(name) - 2] = (char)('a' + count);
        last = sxn_port_create(name);
        if (last < 0)
        {
            break;
        }
        ports[count] = last;
    }
    check(count >= 1, "the user creates a port under a name of its own");
    check(count <= 8 && last == -SAVANXP_ENOMEM, "the user's ports stop at their share of the table");
    while (count > 0)
    {
        (void)savanxp_close((int)ports[--count]);
    }
}

/* The network device is everyone's to open; breaking every connection in the
 * system through its fault injector is not. Skipped on a machine with no
 * network card, where there is no /dev/net0 to ask. */
static void inject_network_faults(void)
{
    struct savanxp_net_tcp_fault fault;
    long fd = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);

    if (fd < 0)
    {
        return;
    }
    memset(&fault, 0, sizeof(fault));
    fault.drop_rx_every = 1;
    check(savanxp_ioctl((int)fd, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) == -SAVANXP_EPERM,
        "the user cannot inject TCP faults");
    (void)savanxp_close((int)fd);
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

/* The host holds Y down from before the prompt opens until after it arms:
 * repeats are not a press, so it stays up until the N that follows. */
static void ask_while_y_is_held(void)
{
    const char *true_argv[] = {"true", 0};
    int status = -1;

    check(elevate("/bin/true", true_argv, 1, &status) == -SAVANXP_EACCES, "a held Y is not a yes");
}

/* The elevated uninstall: what appwiz runs for the user. */
static void uninstall_elevated(void)
{
    const char *appwiz_argv[] = {"appwiz", "--uninstall", ELEVTEST_APP, 0};
    struct stat info;
    int status = -1;
    long pid;

    check(unlink(ELEVTEST_APP) < 0 && errno == EACCES, "the user cannot delete a program in /bin");
    pid = elevate("/bin/appwiz", appwiz_argv, 3, &status);
    check(pid > 0, "the uninstall request is accepted");
    check(pid > 0 && status == 0, "appwiz --uninstall reports the program removed");
    check(stat(ELEVTEST_APP, &info) < 0 && errno == ENOENT, "the program is gone from /bin");
}

/* Gone, or ended and waiting for its parent to reap it. */
static int process_ended(long pid)
{
    struct savanxp_process_info info;
    unsigned long index;

    for (index = 0; proc_info(index, &info) > 0; ++index)
    {
        if ((long)info.pid == pid)
        {
            return info.state == SAVANXP_PROC_ZOMBIE;
        }
    }
    return 1;
}

/* End Process on a process that is not the user's: what taskmgr runs. */
static void end_elevated(void)
{
    char pid_text[16];
    const char *taskmgr_argv[] = {"taskmgr", "--end", pid_text, 0};
    int status = -1;
    long pid;

    snprintf(pid_text, sizeof(pid_text), "%ld", g_victim);
    check(g_victim > 0, "the system's process to end is running");
    check(savanxp_kill((int)g_victim, SAVANXP_SIGKILL) == -SAVANXP_EPERM,
        "the user cannot end the system's process");
    pid = elevate("/system/taskmgr.sxe", taskmgr_argv, 3, &status);
    check(pid > 0, "the end request is accepted");
    check(pid > 0 && status == 0, "taskmgr --end reports the process ended");
    check(process_ended(g_victim), "the system's process has ended");
}

/* Rescan Disks: Disk Management's one elevated operation that writes
 * nothing, so it can run against the smoke's own disks. */
static void rescan_elevated(void)
{
    const char *diskmgmt_argv[] = {"diskmgmt", "--rescan", 0};
    int status = -1;
    long pid;

    pid = elevate("/system/diskmgmt.sxe", diskmgmt_argv, 2, &status);
    check(pid > 0, "the rescan request is accepted");
    check(pid > 0 && status == 0, "diskmgmt --rescan reports success");
}

static int run_user(void)
{
    const char *elevated_argv[] = {"elevtest", "--elevated", 0};
    int status = -1;
    long pid;

    check(setgid(ELEVTEST_GROUP) == 0 && setuid(ELEVTEST_USER) == 0, "lowering to the user");
    check(geteuid() == ELEVTEST_USER, "the child is the user");

    check(open("/dev/input0", O_RDONLY) < 0 && errno == EACCES, "the user cannot open the keyboard");
    /* POSIX has no open without an access mode: to the VFS it is a query that
     * checks only reachability, and it used to come back as a read handle. */
    check(savanxp_open_mode("/dev/input0", 0) == -SAVANXP_EINVAL,
        "the user cannot open the keyboard without an access mode");
    check(savanxp_open_mode("/bin/true", 0) == -SAVANXP_EINVAL,
        "an open without an access mode is refused even where reading is allowed");
    ask_windowd_directly();
    take_service_names();
    inject_network_faults();
    ask_and_forge();
    ask_while_y_is_held();

    pid = elevate("/bin/elevtest", elevated_argv, 2, &status);
    check(pid > 0, "the second request is accepted");
    check(pid > 0 && status == 0, "the elevated program passed its checks");

    /* Asking changed nothing about the asker. */
    check(getuid() == ELEVTEST_USER && geteuid() == ELEVTEST_USER, "the requester is still the user");
    check(open(ELEVTEST_FLAG, O_WRONLY | O_CREAT, 0644) < 0 && errno == EACCES,
        "the requester still cannot write /system");

    uninstall_elevated();
    end_elevated();
    rescan_elevated();
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
    const char *elevationd_argv[] = {"/system/elevationd.sxe", 0};
    const char *sessiond_argv[] = {"/system/sessiond.sxe", 0};
    const char *true_argv[] = {"true", 0};
    const char *sleep_argv[] = {"sleep", "120", 0};
    long elevationd = 0;
    long sessiond = 0;
    long child = 0;
    int status = -1;

    if (argc > 1 && strcmp(argv[1], "--elevated") == 0)
    {
        return run_elevated();
    }

    elevationd = spawn("/system/elevationd.sxe", elevationd_argv, 1);
    sessiond = spawn("/system/sessiond.sxe", sessiond_argv, 1);
    check(elevationd > 0 && sessiond > 0, "starting elevationd and sessiond");
    check(wait_port(SAVANXP_ELEVATION_PORT_NAME) == 0, "the broker opened its port");
    check(wait_port(SAVANXP_CONSENT_PORT_NAME) == 0, "windowd opened the consent port");
    if (g_failures != 0)
    {
        puts_out("ELEVATION SMOKE FAIL setup\n");
        return 1;
    }

    check_input_owned();
    {
        long port = sxn_port_create("sys.elevtest");

        check(port >= 0, "the system takes a name under sys.");
        if (port >= 0)
        {
            (void)savanxp_close((int)port);
        }
    }

    /* What the user will ask Add or Remove Programs to remove. */
    {
        int fd = open(ELEVTEST_APP, O_WRONLY | O_CREAT | O_TRUNC, 0755);

        check(fd >= 0 && write(fd, "elevtest", 8) == 8, "installing the program to uninstall");
        if (fd >= 0)
        {
            close(fd);
        }
    }

    /* The system is not asked: it could start it itself. */
    {
        long pid = elevate("/bin/true", true_argv, 1, &status);

        check(pid > 0 && status == 0, "the system's request runs without a prompt");
    }

    /* What the user will ask Task Manager to end. */
    g_victim = spawn("/bin/sleep", sleep_argv, 2);

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

    (void)unlink(ELEVTEST_APP);
    if (g_victim > 0)
    {
        int victim_status = -1;

        (void)savanxp_kill((int)g_victim, SAVANXP_SIGKILL);
        (void)savanxp_waitpid((int)g_victim, &victim_status);
    }
    (void)savanxp_kill((int)sessiond, SAVANXP_SIGKILL);
    (void)savanxp_kill((int)elevationd, SAVANXP_SIGKILL);
    puts_out(g_failures == 0 ? "ELEVATION SMOKE PASS\n" : "ELEVATION SMOKE FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
