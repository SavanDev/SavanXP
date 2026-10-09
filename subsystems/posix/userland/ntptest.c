#include "libc.h"
#include "ntpsync_lib.h"

#include <time.h>

/* La hora por red, de punta a punta: sincroniza contra el fixture del host,
 * verifica que el RTC quedo en lo aplicado (+-1 s por el borde de segundo),
 * verifica que un puerto cerrado falla en vez de colgar o fijar basura, y
 * restaura la hora original. Gemelo de datetest para ntpsync. */

#define NTPTEST_HOST_IPV4 ((10u << 24) | (0u << 16) | (2u << 8) | 2u)

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

static int read_rtc(struct savanxp_realtime *out)
{
    memset(out, 0, sizeof(*out));
    if (realtime(out) != 0 || out->valid == 0)
    {
        return 0;
    }
    return 1;
}

static long rtc_to_unix(const struct savanxp_realtime *now)
{
    struct tm fields;

    memset(&fields, 0, sizeof(fields));
    fields.tm_year = (int)now->year - 1900;
    fields.tm_mon = (int)now->month - 1;
    fields.tm_mday = (int)now->day;
    fields.tm_hour = (int)now->hour;
    fields.tm_min = (int)now->minute;
    fields.tm_sec = (int)now->second;
    return (long)timegm(&fields);
}

int main(int argc, char **argv)
{
    struct savanxp_realtime original;
    struct savanxp_realtime back;
    unsigned int port = 0;
    unsigned int closed_port;
    long applied = 0;
    long applied_closed = 0;
    unsigned long rtt = 0;
    unsigned long rtt_closed = 0;
    long back_unix;
    int failures = 0;
    long status;

    if (argc != 2 || !parse_uint(argv[1], &port) || port == 0 || port > 65535u)
    {
        puts_out("NTP SMOKE FAIL usage: ntptest <port>\n");
        return 1;
    }
    closed_port = port >= 65535u ? port - 1u : port + 1u;

    if (!read_rtc(&original))
    {
        puts_out("NTP SMOKE SKIP sin reloj de tiempo real que ajustar\n");
        return 0;
    }

    status = ntp_sync_once(NTPTEST_HOST_IPV4, port, 5000ul, &applied, &rtt);
    if (status != 0)
    {
        printf("NTP SMOKE FAIL sync contra el fixture (%s)\n", result_error_string(status));
        return 1;
    }
    if (!read_rtc(&back))
    {
        puts_out("NTP SMOKE FAIL no se pudo releer tras sincronizar\n");
        failures += 1;
    }
    else
    {
        back_unix = rtc_to_unix(&back);
        if (back_unix < applied - 1 || back_unix > applied + 1)
        {
            printf("NTP SMOKE FAIL RTC en %ld, aplicado %ld\n", back_unix, applied);
            failures += 1;
        }
        else
        {
            printf("ntp: aplicado %ld (rtt %lu ms), RTC en %ld\n", applied, rtt, back_unix);
        }
    }

    /* Puerto cerrado: tiene que fallar, no fijar nada ni colgar. */
    status = ntp_sync_once(NTPTEST_HOST_IPV4, closed_port, 2000ul, &applied_closed, &rtt_closed);
    if (status >= 0)
    {
        puts_out("NTP SMOKE FAIL el puerto cerrado no fallo\n");
        failures += 1;
    }
    else
    {
        printf("ntp: puerto cerrado falla (%s), como debe\n", result_error_string(status));
    }

    /* Restaurar la hora que habia, sea cual sea el resultado anterior. */
    status = set_realtime(&original);
    if (status != 0)
    {
        printf("NTP SMOKE FAIL no se pudo restaurar (%s)\n", result_error_string(status));
        return 1;
    }
    if (!read_rtc(&back))
    {
        puts_out("NTP SMOKE FAIL no se pudo releer tras restaurar\n");
        return 1;
    }
    if ((unsigned int)back.year != (unsigned int)original.year ||
        (unsigned int)back.month != (unsigned int)original.month ||
        (unsigned int)back.day != (unsigned int)original.day ||
        (unsigned int)back.hour != (unsigned int)original.hour)
    {
        puts_out("NTP SMOKE FAIL la restauracion no volvio\n");
        return 1;
    }

    if (failures != 0)
    {
        printf("NTP SMOKE FAIL %d checks\n", failures);
        return 1;
    }
    puts_out("NTP SMOKE PASS sincronizar, fallar y restaurar contra el fixture\n");
    return 0;
}
