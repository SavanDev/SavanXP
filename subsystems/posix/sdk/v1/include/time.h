#pragma once

#include <stddef.h>

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

#define CLOCKS_PER_SEC 1000

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;   /* 0-11 */
    int tm_year;  /* anios desde 1900 */
    int tm_wday;  /* 0 = domingo */
    int tm_yday;  /* 0-365 */
    int tm_isdst;
};

/* No hay base de datos de zonas horarias: localtime es gmtime y mktime es
 * timegm. Todo lo que sale de aca es UTC, que es lo que el RTC entrega. */
struct tm* gmtime_r(const time_t* value, struct tm* result);
struct tm* gmtime(const time_t* value);
struct tm* localtime_r(const time_t* value, struct tm* result);
struct tm* localtime(const time_t* value);
time_t timegm(struct tm* value);
time_t mktime(struct tm* value);
size_t strftime(char* buffer, size_t capacity, const char* format, const struct tm* value);
clock_t clock(void);

#if defined(__SSE2__)
double difftime(time_t later, time_t earlier);
#endif
