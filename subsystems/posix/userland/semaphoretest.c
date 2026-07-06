#include "libc.h"

static int expect_timeout(long result, const char* label) {
    if (!result_is_error(result) || result_error_code(result) != SAVANXP_ETIMEDOUT) {
        eprintf("semaphoretest: expected timeout at %s, got %ld (%s)\n", label, result, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("semaphoretest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_value(long result, long expected, const char* label) {
    if (result != expected) {
        eprintf("semaphoretest: %s expected %ld got %ld\n", label, expected, result);
        return 0;
    }
    return 1;
}

static int expect_error(long result, const char* label) {
    if (result >= 0) {
        eprintf("semaphoretest: expected error at %s, got %ld\n", label, result);
        return 0;
    }
    return 1;
}

int main(void) {
    if (!expect_error(semaphore_create(1, 0), "create with max_count 0") ||
        !expect_error(semaphore_create(3, 2), "create with initial_count > max_count")) {
        return 1;
    }

    long semaphore = semaphore_create(0, 2);
    if (!expect_success(semaphore, "create semaphore")) {
        return 1;
    }

    if (!expect_timeout(wait_one((int)semaphore, 0), "wait on empty semaphore")) {
        return 1;
    }

    if (!expect_value(semaphore_release((int)semaphore, 1), 0, "release from empty (previous count)")) {
        return 1;
    }
    if (!expect_success(wait_one((int)semaphore, 0), "acquire after single release")) {
        return 1;
    }
    if (!expect_timeout(wait_one((int)semaphore, 0), "acquire consumed the only permit")) {
        return 1;
    }

    if (!expect_value(semaphore_release((int)semaphore, 2), 0, "release 2 (previous count)")) {
        return 1;
    }
    if (!expect_error(semaphore_release((int)semaphore, 1), "release beyond max_count")) {
        return 1;
    }
    if (!expect_success(wait_one((int)semaphore, 0), "acquire permit 1 of 2") ||
        !expect_success(wait_one((int)semaphore, 0), "acquire permit 2 of 2") ||
        !expect_timeout(wait_one((int)semaphore, 0), "acquire beyond available permits")) {
        return 1;
    }

    long pid = fork();
    if (pid < 0) {
        eprintf("semaphoretest: fork failed (%s)\n", result_error_string(pid));
        return 1;
    }
    if (pid == 0) {
        long child_wait = wait_one((int)semaphore, 1000);
        if (!expect_success(child_wait, "child wait for released permit")) {
            return 2;
        }
        return 0;
    }

    if (!expect_success(sleep_ms(50), "sleep before release") ||
        !expect_success(semaphore_release((int)semaphore, 1), "release for waiting child")) {
        return 1;
    }

    int status = -1;
    if (waitpid((int)pid, &status) < 0) {
        eprintf("semaphoretest: waitpid failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("semaphoretest: child status %d\n", status);
        return 1;
    }

    close((int)semaphore);
    return 0;
}
