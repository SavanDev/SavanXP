#include "libc.h"
#include "ntpsync_lib.h"

#include <time.h>

/* Sincronizacion con un servidor de hora: `ntpsync <ipv4> [puerto]`. Fija el
 * RTC con lo que conteste y lo informa con el retardo y el ajuste. Todo UTC;
 * sin DNS (direcciones numericas, como el resto del stack). */

static int parse_uint(const char *text, unsigned int *value)
{
    unsigned int result = 0;
    size_t index = 0;

    if (text == 0 || text[0] == '\0')
    {
        return 0;
    }
    while (text[index] != '\0')
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return 0;
        }
        result = result * 10u + (unsigned int)(text[index] - '0');
        ++index;
    }
    *value = result;
    return 1;
}

static int parse_ipv4(const char *text, uint32_t *address)
{
    unsigned int parts[4];
    unsigned int part = 0;
    unsigned int count = 0;
    const char *cursor = text;

    while (*cursor != '\0' && count < 4)
    {
        const char *start = cursor;
        char chunk[4];
        size_t length;

        while (*cursor != '\0' && *cursor != '.')
        {
            ++cursor;
        }
        length = (size_t)(cursor - start);
        if (length == 0 || length >= sizeof(chunk))
        {
            return 0;
        }
        memcpy(chunk, start, length);
        chunk[length] = '\0';
        if (!parse_uint(chunk, &part) || part > 255u)
        {
            return 0;
        }
        parts[count++] = part;
        if (*cursor == '.')
        {
            ++cursor;
        }
    }
    if (*cursor != '\0' || count != 4)
    {
        return 0;
    }
    *address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

int main(int argc, char **argv)
{
    struct savanxp_realtime before;
    struct tm before_fields;
    struct tm *applied_fields;
    uint32_t ipv4 = 0;
    unsigned int port = 123;
    time_t before_unix = 0;
    long applied = 0;
    unsigned long rtt = 0;
    int have_before = 0;
    long status;

    if ((argc != 2 && argc != 3) || !parse_ipv4(argv[1], &ipv4))
    {
        puts_fd(2, "usage: ntpsync <ipv4> [port] (UTC)\n");
        return 1;
    }
    if (argc == 3 && (!parse_uint(argv[2], &port) || port == 0 || port > 65535u))
    {
        puts_fd(2, "usage: ntpsync <ipv4> [port] (UTC)\n");
        return 1;
    }

    /* La hora local de antes, solo para informar el ajuste. */
    memset(&before, 0, sizeof(before));
    if (realtime(&before) == 0 && before.valid != 0)
    {
        memset(&before_fields, 0, sizeof(before_fields));
        before_fields.tm_year = (int)before.year - 1900;
        before_fields.tm_mon = (int)before.month - 1;
        before_fields.tm_mday = (int)before.day;
        before_fields.tm_hour = (int)before.hour;
        before_fields.tm_min = (int)before.minute;
        before_fields.tm_sec = (int)before.second;
        before_unix = timegm(&before_fields);
        have_before = before_unix != (time_t)-1;
    }

    status = ntp_sync_once(ipv4, port, 5000ul, &applied, &rtt);
    if (status != 0)
    {
        eprintf("ntpsync: sin hora (%s)\n", result_error_string(status));
        return 1;
    }

    applied_fields = gmtime(&applied);
    printf("ntpsync: %04d-%02d-%02d %02d:%02d:%02d UTC desde %u.%u.%u.%u (rtt %lu ms",
           applied_fields->tm_year + 1900, applied_fields->tm_mon + 1, applied_fields->tm_mday,
           applied_fields->tm_hour, applied_fields->tm_min, applied_fields->tm_sec,
           (unsigned int)((ipv4 >> 24) & 0xffu), (unsigned int)((ipv4 >> 16) & 0xffu),
           (unsigned int)((ipv4 >> 8) & 0xffu), (unsigned int)(ipv4 & 0xffu), rtt);
    if (have_before)
    {
        long delta = (long)(applied - before_unix);

        if (delta < 0)
        {
            printf(", ajuste -%lu s", (unsigned long)(-delta));
        }
        else
        {
            printf(", ajuste +%lu s", (unsigned long)delta);
        }
    }
    printf(")\n");
    return 0;
}
