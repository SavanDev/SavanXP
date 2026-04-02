#pragma once

#include "sys/types.h"

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

#define time sx_time
#define nanosleep sx_nanosleep
#define clock_gettime sx_clock_gettime

time_t sx_time(time_t* out_value);
int sx_nanosleep(const struct timespec* request, struct timespec* remaining);
int sx_clock_gettime(int clock_id, struct timespec* value);
