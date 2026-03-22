#include "libc.h"

static int expect_timeout(long result, const char* label) {
    if (!result_is_error(result) || result_error_code(result) != SAVANXP_ETIMEDOUT) {
        eprintf("eventtest: expected timeout at %s, got %ld (%s)\n", label, result, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("eventtest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_value(long result, long expected, const char* label) {
    if (result != expected) {
        eprintf("eventtest: %s expected %ld got %ld\n", label, expected, result);
        return 0;
    }
    return 1;
}

int main(void) {
    long auto_event_a = event_create(SAVANXP_EVENT_AUTO_RESET);
    long auto_event_b = event_create(SAVANXP_EVENT_AUTO_RESET);
    if (!expect_success(auto_event_a, "create auto event a") ||
        !expect_success(auto_event_b, "create auto event b")) {
        return 1;
    }

    int auto_handles[2] = {(int)auto_event_a, (int)auto_event_b};
    if (!expect_timeout(wait_one((int)auto_event_a, 0), "initial auto wait")) {
        return 1;
    }
    if (!expect_timeout(wait_many(auto_handles, 2, SAVANXP_WAIT_FLAG_ANY, 0), "initial wait_many any")) {
        return 1;
    }

    long pid = fork();
    if (pid < 0) {
        eprintf("eventtest: fork failed (%s)\n", result_error_string(pid));
        return 1;
    }
    if (pid == 0) {
        long child_wait = wait_many(auto_handles, 2, SAVANXP_WAIT_FLAG_ANY, 1000);
        if (!expect_value(child_wait, 1, "child wait_many any")) {
            return 2;
        }
        return 0;
    }

    if (!expect_success(sleep_ms(50), "sleep before set")) {
        return 1;
    }
    if (!expect_success(event_set((int)auto_event_b), "set auto event b")) {
        return 1;
    }

    int status = -1;
    if (waitpid((int)pid, &status) < 0) {
        eprintf("eventtest: waitpid failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("eventtest: child status %d\n", status);
        return 1;
    }

    if (!expect_timeout(wait_one((int)auto_event_b, 0), "auto reset consumed")) {
        return 1;
    }

    long manual_event_a = event_create(SAVANXP_EVENT_MANUAL_RESET);
    long manual_event_b = event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (!expect_success(manual_event_a, "create manual event a") ||
        !expect_success(manual_event_b, "create manual event b")) {
        return 1;
    }
    int manual_handles[2] = {(int)manual_event_a, (int)manual_event_b};

    if (!expect_success(event_set((int)manual_event_a), "set manual event a") ||
        !expect_success(event_set((int)manual_event_b), "set manual event b")) {
        return 1;
    }
    if (!expect_success(wait_one((int)manual_event_a, 0), "manual wait first")) {
        return 1;
    }
    if (!expect_success(wait_one((int)manual_event_a, 0), "manual wait second")) {
        return 1;
    }
    if (!expect_success(wait_many(manual_handles, 2, SAVANXP_WAIT_FLAG_ALL, 0), "manual wait_many all immediate")) {
        return 1;
    }

    if (!expect_success(event_reset((int)manual_event_b), "reset manual event b")) {
        return 1;
    }
    if (!expect_timeout(wait_many(manual_handles, 2, SAVANXP_WAIT_FLAG_ALL, 0), "manual wait_many all timeout")) {
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        eprintf("eventtest: second fork failed (%s)\n", result_error_string(pid));
        return 1;
    }
    if (pid == 0) {
        long child_wait = wait_many(manual_handles, 2, SAVANXP_WAIT_FLAG_ALL, 1000);
        if (!expect_value(child_wait, 0, "child wait_many all")) {
            return 3;
        }
        return 0;
    }

    if (!expect_success(sleep_ms(50), "sleep before manual sets")) {
        return 1;
    }
    if (!expect_success(event_set((int)manual_event_b), "set manual event b second round")) {
        return 1;
    }

    status = -1;
    if (waitpid((int)pid, &status) < 0) {
        eprintf("eventtest: second waitpid failed\n");
        return 1;
    }
    if (status != 0) {
        eprintf("eventtest: second child status %d\n", status);
        return 1;
    }

    if (!expect_success(wait_many(manual_handles, 2, SAVANXP_WAIT_FLAG_ALL, 0), "manual wait_many all stays signaled")) {
        return 1;
    }
    if (!expect_success(event_reset((int)manual_event_a), "reset manual event a")) {
        return 1;
    }
    if (!expect_success(event_reset((int)manual_event_b), "reset manual event b final")) {
        return 1;
    }
    if (!expect_timeout(wait_many(manual_handles, 2, SAVANXP_WAIT_FLAG_ANY, 0), "manual wait_many any after reset")) {
        return 1;
    }

    close((int)manual_event_b);
    close((int)manual_event_a);
    close((int)auto_event_b);
    close((int)auto_event_a);
    return 0;
}
