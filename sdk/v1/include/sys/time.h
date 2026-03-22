#pragma once

#include "sys/types.h"

struct timeval {
    long tv_sec;
    long tv_usec;
};

int sx_gettimeofday(struct timeval* value, void* timezone_ptr);

#define gettimeofday sx_gettimeofday
