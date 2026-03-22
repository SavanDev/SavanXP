#include "libc.h"

static int expect_success(long result, const char* label) {
    if (result < 0) {
        eprintf("timertest: %s failed (%s)\n", label, result_error_string(result));
        return 0;
    }
    return 1;
}

static int expect_timeout(long result, const char* label) {
    if (!result_is_error(result) || result_error_code(result) != SAVANXP_ETIMEDOUT) {
        eprintf("timertest: expected timeout at %s, got %ld (%s)\n", label, result, result_error_string(result));
        return 0;
    }
    return 1;
}

int main(void) {
    long auto_timer = timer_create(SAVANXP_TIMER_AUTO_RESET);
    if (!expect_success(auto_timer, "create auto timer")) {
        return 1;
    }
    if (!expect_timeout(wait_one((int)auto_timer, 0), "initial auto timer wait")) {
        return 1;
    }
    if (!expect_success(timer_set((int)auto_timer, 50, 0), "arm auto timer")) {
        return 1;
    }
    if (!expect_success(wait_one((int)auto_timer, 1000), "wait auto timer")) {
        return 1;
    }
    if (!expect_timeout(wait_one((int)auto_timer, 0), "auto timer consumed")) {
        return 1;
    }

    long periodic_timer = timer_create(SAVANXP_TIMER_AUTO_RESET);
    if (!expect_success(periodic_timer, "create periodic timer")) {
        return 1;
    }
    if (!expect_success(timer_set((int)periodic_timer, 20, 20), "arm periodic timer")) {
        return 1;
    }
    if (!expect_success(wait_one((int)periodic_timer, 1000), "wait periodic timer first")) {
        return 1;
    }
    if (!expect_success(wait_one((int)periodic_timer, 1000), "wait periodic timer second")) {
        return 1;
    }
    if (!expect_success(timer_cancel((int)periodic_timer), "cancel periodic timer")) {
        return 1;
    }
    if (!expect_timeout(wait_one((int)periodic_timer, 0), "periodic timer cancelled")) {
        return 1;
    }

    long manual_timer = timer_create(SAVANXP_TIMER_MANUAL_RESET);
    if (!expect_success(manual_timer, "create manual timer")) {
        return 1;
    }
    if (!expect_success(timer_set((int)manual_timer, 0, 0), "arm manual timer immediate")) {
        return 1;
    }
    if (!expect_success(wait_one((int)manual_timer, 0), "manual timer first wait")) {
        return 1;
    }
    if (!expect_success(wait_one((int)manual_timer, 0), "manual timer second wait")) {
        return 1;
    }
    if (!expect_success(timer_cancel((int)manual_timer), "cancel manual timer")) {
        return 1;
    }
    if (!expect_timeout(wait_one((int)manual_timer, 0), "manual timer cancelled")) {
        return 1;
    }

    if (!expect_success(sleep_ms(20), "sleep via timer object")) {
        return 1;
    }

    close((int)manual_timer);
    close((int)periodic_timer);
    close((int)auto_timer);
    return 0;
}
