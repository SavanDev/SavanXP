#include "libc.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Identities and permissions, phase 2 of docs/kernel/USERS_AND_PERMISSIONS.md:
 * a process the system starts acts as the system; once it lowers itself to the
 * user (uid 1000) the VFS refuses what the user may not do and allows what it
 * may.
 *
 *     idtest           the parent: runs as the system, prepares a profile,
 *                      forks the child and cleans up
 *     (the child)      setuid(1000), then every check below
 *
 * Everything runs on the smoke's disposable copy of the disk.
 */

#define IDTEST_USER 1000u
#define IDTEST_GROUP 100u
#define IDTEST_PROFILE "/users/idtest"
#define IDTEST_PROFILE_FILE IDTEST_PROFILE "/notes.txt"
#define IDTEST_TMP_OWN "/tmp/idtest-user.txt"
#define IDTEST_TMP_SYSTEM "/tmp/idtest-system.txt"

static int g_failures = 0;

static void check(int ok, const char* what) {
    if (!ok) {
        printf("IDTEST FAIL %s\n", what);
        g_failures += 1;
    }
}

/* The call failed with exactly `expected`. */
static void check_refused(int result, int expected, const char* what) {
    if (result >= 0) {
        printf("IDTEST FAIL %s: it was allowed\n", what);
        g_failures += 1;
    } else if (errno != expected) {
        printf("IDTEST FAIL %s: errno %d, expected %d\n", what, errno, expected);
        g_failures += 1;
    }
}

static int create_file(const char* path, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) {
        return -1;
    }
    (void)write(fd, "idtest\n", 7);
    close(fd);
    return 0;
}

static int run_child(void) {
    check(getuid() == 0 && geteuid() == 0, "the child starts as the system");

    /* Lower to the user, for good. The gid first: once the uid is not the
     * system's, setgid can no longer change it. */
    check(setgid(IDTEST_GROUP) == 0, "setgid(100) as the system");
    check(setuid(IDTEST_USER) == 0, "setuid(1000) as the system");
    check(getuid() == IDTEST_USER && geteuid() == IDTEST_USER && getgid() == IDTEST_GROUP,
          "the identity is the user's after setuid");

    /* Nothing raises it back. */
    check_refused(setuid(0), EPERM, "setuid(0) as the user");
    check_refused(seteuid(0), EPERM, "seteuid(0) as the user");
    check_refused(setgid(0), EPERM, "setgid(0) as the user");

    /* The system's files: readable and runnable, not writable. */
    check(open("/bin/ls", O_RDONLY) >= 0, "the user reads /bin/ls");
    check_refused(open("/bin/ls", O_WRONLY), EACCES, "the user opens /bin/ls for writing");
    check_refused(open("/bin/idtest-new", O_WRONLY | O_CREAT, 0644), EACCES, "the user creates a file in /bin");
    check_refused(mkdir("/bin/idtest-dir", 0755), EACCES, "the user makes a directory in /bin");
    check_refused(unlink("/bin/cat"), EACCES, "the user deletes /bin/cat");
    check_refused(rename("/bin/cat", "/bin/cat2"), EACCES, "the user renames /bin/cat");
    check_refused(chmod("/bin/ls", 0777), EPERM, "the user chmods /bin/ls");
    check_refused(chown(IDTEST_PROFILE, 0, 0), EPERM, "the user gives its profile away");
    check(access("/bin/ls", X_OK) == 0, "access(/bin/ls, X_OK) as the user");
    check_refused(access("/bin/ls", W_OK), EACCES, "access(/bin/ls, W_OK) as the user");
    check(open("/system/programs.txt", O_RDONLY) >= 0, "the user reads /system/programs.txt");
    check_refused(open("/system/programs.txt", O_WRONLY), EACCES, "the user writes /system/programs.txt");

    /* Running something still works: /bin is 0755. */
    {
        const char* true_argv[] = {"/bin/true", 0};
        int status = -1;
        long pid = spawn("/bin/true", true_argv, 1);
        check(pid > 0, "the user spawns /bin/true");
        if (pid > 0) {
            check(savanxp_waitpid((int)pid, &status) >= 0 && status == 0, "/bin/true ran as the user");
        }
    }

    /* The keyboard is the system's only. */
    check_refused(open("/dev/input0", O_RDONLY), EACCES, "the user reads /dev/input0");

    /* Its profile is its own. */
    check(create_file(IDTEST_PROFILE_FILE, 0600) == 0, "the user creates a file in its profile");
    {
        /* Closed again: an open file cannot be deleted (EBUSY). */
        const int fd = open(IDTEST_PROFILE_FILE, O_RDWR);
        check(fd >= 0, "the user reopens its own file");
        if (fd >= 0) {
            close(fd);
        }
    }
    check(chmod(IDTEST_PROFILE_FILE, 0640) == 0, "the user chmods its own file");
    check(unlink(IDTEST_PROFILE_FILE) == 0, "the user deletes its own file");

    /* /tmp is 1777: anyone creates, only the owner removes. */
    check(create_file(IDTEST_TMP_OWN, 0644) == 0, "the user creates a file in /tmp");
    check(unlink(IDTEST_TMP_OWN) == 0, "the user deletes its own file in /tmp");
    check_refused(unlink(IDTEST_TMP_SYSTEM), EACCES, "the user deletes the system's file in /tmp");

    return g_failures == 0 ? 0 : 1;
}

int main(void) {
    int status = -1;

    check(getuid() == 0 && geteuid() == 0 && getgid() == 0, "a process the system starts is the system");
    {
        const struct passwd* system = getpwuid(0);
        const struct passwd* user = getpwuid(IDTEST_USER);
        check(system != 0 && strcmp(system->pw_name, "root") == 0, "getpwuid(0) is root");
        check(user != 0 && user->pw_gid == IDTEST_GROUP && getpwnam(user->pw_name) == user,
              "getpwuid(1000) and getpwnam agree on the user");
    }

    /* The profile the child will own. /users itself stays the system's. */
    (void)mkdir("/users", 0755);
    (void)mkdir(IDTEST_PROFILE, 0700);
    check(chown(IDTEST_PROFILE, IDTEST_USER, IDTEST_GROUP) == 0, "the system gives the profile to the user");
    check(create_file(IDTEST_TMP_SYSTEM, 0644) == 0, "the system creates a file in /tmp");

    {
        long pid = savanxp_fork();
        check(pid >= 0, "fork");
        if (pid == 0) {
            exit(run_child());
        }
        if (pid > 0) {
            check(savanxp_waitpid((int)pid, &status) >= 0 && status == 0, "the checks as the user");
        }
    }

    /* The parent never lowered itself: fork copies the identity, it does not
     * share it. */
    check(getuid() == 0 && geteuid() == 0, "the parent is still the system");
    check(unlink(IDTEST_TMP_SYSTEM) == 0, "the system deletes its file in /tmp");
    check(rmdir(IDTEST_PROFILE) == 0, "the system removes the profile");

    puts_out(g_failures == 0 ? "IDTEST PASS\n" : "IDTEST FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
