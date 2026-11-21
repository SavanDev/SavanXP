/* locktest: POSIX record locks through fcntl (docs/kernel/FILE_LOCKS.md).
 *
 * Standard headers only, as a port sees them. The parent holds locks and a
 * forked child probes them: two processes are two owners, and a child never
 * inherits its parent's locks. Covers conflicts and shared ranges, F_GETLK's
 * report, splitting and merging an owner's ranges, F_SETLKW parking until the
 * range frees, EDEADLK, the release on closing any descriptor of the file
 * and at exit, ranges from SEEK_END and negative lengths, and the refusals.
 * Exits 0 when everything held, and prints each failure. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LOCKTEST_PATH "/tmp/locktest.dat"

static int g_failures = 0;

static void check(int condition, const char* what) {
    if (!condition) {
        printf("locktest: FAIL %s (errno %d)\n", what, errno);
        g_failures += 1;
    }
}

static int set_lock(int fd, int command, short type, short whence, off_t start, off_t length) {
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = whence;
    lock.l_start = start;
    lock.l_len = length;
    return fcntl(fd, command, &lock);
}

/* F_GETLK for a write lock over [start, start + length). */
static struct flock query(int fd, off_t start, off_t length) {
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = start;
    lock.l_len = length;
    lock.l_pid = -1;
    if (fcntl(fd, F_GETLK, &lock) != 0) {
        lock.l_type = -1;
    }
    return lock;
}

/* Runs `probe` in a child with its own descriptor of the file and returns
 * its exit status: 0 when every check in it held, else the number of the
 * first that did not. */
static int in_child(int (*probe)(int fd, pid_t parent)) {
    const pid_t parent = getpid();
    const pid_t child = fork();
    int status = -1;

    if (child == 0) {
        const int fd = open(LOCKTEST_PATH, O_RDWR);
        _exit(fd < 0 ? 100 : probe(fd, parent));
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int probe_conflicts(int fd, pid_t parent) {
    struct flock seen;

    if (set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 5, 3) == 0 || errno != EAGAIN) return 1;
    if (set_lock(fd, F_SETLK, F_RDLCK, SEEK_SET, 9, 1) == 0 || errno != EAGAIN) return 2;
    seen = query(fd, 0, 0);
    if (seen.l_type != F_WRLCK || seen.l_whence != SEEK_SET || seen.l_start != 0 || seen.l_len != 10 ||
        seen.l_pid != parent) return 3;
    /* Outside the parent's range: free. */
    if (set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 10, 5) != 0) return 4;
    return 0;
}

static int probe_shared(int fd, pid_t parent) {
    (void)parent;
    if (set_lock(fd, F_SETLK, F_RDLCK, SEEK_SET, 0, 10) != 0) return 1;
    if (set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1) == 0 || errno != EAGAIN) return 2;
    return 0;
}

/* The parent holds [0, 40) and [60, ...) after unlocking [40, 60) out of the
 * middle of [0, ...). */
static int probe_split(int fd, pid_t parent) {
    struct flock seen;

    (void)parent;
    seen = query(fd, 45, 1);
    if (seen.l_type != F_UNLCK) return 1;
    seen = query(fd, 30, 1);
    if (seen.l_type != F_WRLCK || seen.l_start != 0 || seen.l_len != 40) return 2;
    seen = query(fd, 70, 1);
    if (seen.l_type != F_WRLCK || seen.l_start != 60 || seen.l_len != 0) return 3;
    return 0;
}

/* Two adjacent read locks of the parent read back as one range. */
static int probe_merged(int fd, pid_t parent) {
    struct flock seen;

    (void)parent;
    seen = query(fd, 0, 1);
    if (seen.l_type != F_RDLCK || seen.l_start != 0 || seen.l_len != 20) return 1;
    return 0;
}

/* The parent holds the last 10 bytes of a 100-byte file, named from SEEK_END
 * with a negative length. */
static int probe_from_end(int fd, pid_t parent) {
    struct flock seen;

    (void)parent;
    seen = query(fd, 95, 1);
    if (seen.l_type != F_WRLCK || seen.l_start != 90 || seen.l_len != 10) return 1;
    if (query(fd, 89, 1).l_type != F_UNLCK) return 2;
    return 0;
}

static int probe_free(int fd, pid_t parent) {
    (void)parent;
    return set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 0) == 0 ? 0 : 1;
}

static int probe_hold_and_exit(int fd, pid_t parent) {
    (void)parent;
    return set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 50, 1) == 0 ? 0 : 1;
}

static int read_byte(int fd, int timeout_ms) {
    struct pollfd entry = {fd, POLLIN, 0};
    unsigned char byte = 0;

    if (poll(&entry, 1, timeout_ms) != 1 || read(fd, &byte, 1) != 1) {
        return -1;
    }
    return byte;
}

/* F_SETLKW parks the child until the parent unlocks, and not before. */
static void test_wait(int fd) {
    int channel[2];
    pid_t child;
    int status = -1;

    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 10) == 0, "hold for the waiter");
    if (pipe(channel) != 0) {
        check(0, "pipe for the waiter");
        return;
    }
    child = fork();
    if (child == 0) {
        const int own = open(LOCKTEST_PATH, O_RDWR);
        close(channel[0]);
        if (own < 0 || write(channel[1], "w", 1) != 1) _exit(1);
        if (set_lock(own, F_SETLKW, F_WRLCK, SEEK_SET, 0, 10) != 0) _exit(2);
        if (write(channel[1], "g", 1) != 1) _exit(3);
        _exit(0);
    }
    close(channel[1]);
    check(read_byte(channel[0], 5000) == 'w', "waiter started");
    check(read_byte(channel[0], 300) == -1, "waiter parked while the range is held");
    check(set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 10) == 0, "unlock for the waiter");
    check(read_byte(channel[0], 5000) == 'g', "waiter granted after the unlock");
    check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "waiter exit status");
    close(channel[0]);
}

/* Parent holds A and the child B; each then waits for the other's. Whichever
 * waits second closes the cycle and gets EDEADLK; the other is granted once
 * the first gives its lock up. */
static void test_deadlock(int fd) {
    int channel[2];
    pid_t child;
    int status = -1;
    int parent_result;

    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 1) == 0, "deadlock: hold A");
    if (pipe(channel) != 0) {
        check(0, "pipe for the deadlock");
        return;
    }
    child = fork();
    if (child == 0) {
        const int own = open(LOCKTEST_PATH, O_RDWR);
        int result;
        close(channel[0]);
        if (own < 0 || set_lock(own, F_SETLK, F_WRLCK, SEEK_SET, 10, 1) != 0) _exit(1);
        if (write(channel[1], "b", 1) != 1) _exit(2);
        result = set_lock(own, F_SETLKW, F_WRLCK, SEEK_SET, 0, 1);
        /* 0: granted after the parent's EDEADLK. 10: this one was second. */
        _exit(result == 0 ? 0 : (errno == EDEADLK ? 10 : 3));
    }
    close(channel[1]);
    check(read_byte(channel[0], 5000) == 'b', "deadlock: child holds B");
    /* Long enough for the child to park on A first, almost always. */
    usleep(200000);
    parent_result = set_lock(fd, F_SETLKW, F_WRLCK, SEEK_SET, 10, 1);
    if (parent_result != 0) {
        check(errno == EDEADLK, "deadlock: parent's wait refused with EDEADLK");
        check(set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 1) == 0, "deadlock: give A up");
    }
    check(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status), "deadlock: child ended");
    /* Exactly one of the two saw EDEADLK. */
    check((parent_result != 0) == (WEXITSTATUS(status) == 0) &&
              (WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 10),
          "deadlock: one side refused, the other granted");
    close(channel[0]);
    set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 0);
}

int main(void) {
    char bytes[100];
    int fd;
    int other;
    int reader;
    int channel[2];

    memset(bytes, 'x', sizeof(bytes));
    fd = open(LOCKTEST_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || write(fd, bytes, sizeof(bytes)) != (long)sizeof(bytes)) {
        printf("locktest: FAIL setup (errno %d)\n", errno);
        return 1;
    }

    /* Another process's write lock blocks writers and readers; the owner's
     * own locks never block it. */
    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 10) == 0, "parent write lock");
    check(query(fd, 0, 0).l_type == F_UNLCK, "own lock is no conflict");
    check(in_child(probe_conflicts) == 0, "child sees the parent's write lock");

    /* Turned into a read lock in place: readers share it, writers wait. */
    check(set_lock(fd, F_SETLK, F_RDLCK, SEEK_SET, 0, 10) == 0, "write lock turned shared");
    check(in_child(probe_shared) == 0, "read locks are shared");

    check(set_lock(fd, F_SETLK, F_RDLCK, SEEK_SET, 10, 10) == 0, "adjacent read lock");
    check(in_child(probe_merged) == 0, "adjacent ranges merged");

    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 0) == 0, "lock to the end");
    check(set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 40, 20) == 0, "unlock from the middle");
    check(in_child(probe_split) == 0, "range split around the unlock");
    check(set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 0) == 0, "unlock everything");
    check(in_child(probe_free) == 0, "file free after unlocking");

    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_END, 0, -10) == 0, "lock the last 10 bytes from SEEK_END");
    check(in_child(probe_from_end) == 0, "SEEK_END and a negative length");
    set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 0);

    test_wait(fd);
    test_deadlock(fd);

    /* Closing ANY descriptor of the file drops the process's locks on it. */
    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 10) == 0, "lock before closing a second descriptor");
    other = open(LOCKTEST_PATH, O_RDONLY);
    check(other >= 0 && close(other) == 0, "second descriptor closed");
    check(in_child(probe_free) == 0, "close of another descriptor released the lock");

    /* A process's locks go when it exits. */
    check(in_child(probe_hold_and_exit) == 0, "child locks and exits");
    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 50, 1) == 0, "exit released the child's lock");
    set_lock(fd, F_SETLK, F_UNLCK, SEEK_SET, 0, 0);

    /* Refusals. */
    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, -1, 1) == -1 && errno == EINVAL, "negative start refused");
    check(set_lock(fd, F_SETLK, F_WRLCK, SEEK_SET, 0, -1) == -1 && errno == EINVAL, "range before byte 0 refused");
    check(set_lock(fd, F_SETLK, 7, SEEK_SET, 0, 1) == -1 && errno == EINVAL, "unknown l_type refused");
    check(set_lock(fd, F_SETLK, F_WRLCK, 9, 0, 1) == -1 && errno == EINVAL, "unknown l_whence refused");
    reader = open(LOCKTEST_PATH, O_RDONLY);
    check(reader >= 0 && set_lock(reader, F_SETLK, F_WRLCK, SEEK_SET, 0, 1) == -1 && errno == EBADF,
          "write lock on a read-only descriptor refused");
    check(set_lock(reader, F_SETLK, F_RDLCK, SEEK_SET, 0, 1) == 0, "read lock on a read-only descriptor");
    close(reader);
    check(set_lock(99, F_SETLK, F_RDLCK, SEEK_SET, 0, 1) == -1 && errno == EBADF, "bad descriptor refused");
    if (pipe(channel) == 0) {
        check(set_lock(channel[0], F_SETLK, F_RDLCK, SEEK_SET, 0, 1) == -1 && errno == EINVAL,
              "a pipe has no locks");
        close(channel[0]);
        close(channel[1]);
    }

    close(fd);
    unlink(LOCKTEST_PATH);
    if (g_failures != 0) {
        printf("locktest: %d failures\n", g_failures);
        return 1;
    }
    printf("locktest: PASS\n");
    return 0;
}
