#include "desktop.h"

#include "sxn/sxn.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* What the desktop's modules ask of the system themselves (desktop.h). */

unsigned long desktop_uptime_ms(void)
{
    return (unsigned long)(sxn_clock_ns() / 1000000ull);
}

void desktop_log(const char *format, ...)
{
    char line[256];
    va_list arguments;
    int length;

    va_start(arguments, format);
    length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length <= 0)
    {
        return;
    }
    if ((size_t)length >= sizeof(line))
    {
        length = (int)sizeof(line) - 1;
    }
    /* Standard error, as the POSIX desktop wrote it: sessiond starts the
     * desktop with the session's console, so the line lands in the serial log
     * as before. */
    (void)sxn_write(2, line, (unsigned long)length);
}

int desktop_realtime(struct savanxp_realtime *out)
{
    uint64_t seconds = sxn_system_time();
    time_t stamp = (time_t)seconds;
    struct tm broken;

    memset(out, 0, sizeof(*out));
    /* 0 is the kernel's "the machine never gave a date". */
    if (seconds == 0 || gmtime_r(&stamp, &broken) == 0)
    {
        return -1;
    }
    out->year = (uint16_t)(broken.tm_year + 1900);
    out->month = (uint8_t)(broken.tm_mon + 1);
    out->day = (uint8_t)broken.tm_mday;
    out->hour = (uint8_t)broken.tm_hour;
    out->minute = (uint8_t)broken.tm_min;
    out->second = (uint8_t)broken.tm_sec;
    out->valid = 1;
    return 0;
}
