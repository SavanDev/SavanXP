#pragma once

#include "sys/types.h"

typedef long time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1


time_t time(time_t* out_value);
int nanosleep(const struct timespec* request, struct timespec* remaining);
int clock_gettime(int clock_id, struct timespec* value);
