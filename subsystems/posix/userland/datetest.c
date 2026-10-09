#include "libc.h"

/* Ida y vuelta del RTC por la syscall de escritura: fija una fecha conocida,
 * la relee y la compara, rechaza una fecha imposible y restaura la original.
 * El segundo se tolera con +1 porque puede caer un borde entre el set y el
 * read (resolucion de un segundo); por eso el objetivo va a mitad de minuto
 * y nunca cruza uno. Es el gemelo de clocktest para SET_REALTIME. */

#define DATETEST_YEAR 2031u
#define DATETEST_MONTH 6u
#define DATETEST_DAY 15u
#define DATETEST_HOUR 12u
#define DATETEST_MINUTE 34u
#define DATETEST_SECOND 30u

static int read_rtc(struct savanxp_realtime *out)
{
    memset(out, 0, sizeof(*out));
    if (realtime(out) != 0 || out->valid == 0)
    {
        return 0;
    }
    return 1;
}

/* Campos exactos salvo el segundo, que admite el borde descrito arriba. */
static int matches_target(const struct savanxp_realtime *got,
                          unsigned int year, unsigned int month, unsigned int day,
                          unsigned int hour, unsigned int minute, unsigned int second)
{
    if ((unsigned int)got->year != year || (unsigned int)got->month != month ||
        (unsigned int)got->day != day || (unsigned int)got->hour != hour ||
        (unsigned int)got->minute != minute)
    {
        return 0;
    }
    return (unsigned int)got->second == second ||
           (unsigned int)got->second == second + 1u;
}

int main(void)
{
    struct savanxp_realtime original;
    struct savanxp_realtime target;
    struct savanxp_realtime back;
    int failures = 0;
    long status;

    if (!read_rtc(&original))
    {
        puts_out("DATE SMOKE SKIP sin reloj de tiempo real que ajustar\n");
        return 0;
    }

    memset(&target, 0, sizeof(target));
    target.year = DATETEST_YEAR;
    target.month = DATETEST_MONTH;
    target.day = DATETEST_DAY;
    target.hour = DATETEST_HOUR;
    target.minute = DATETEST_MINUTE;
    target.second = DATETEST_SECOND;
    status = set_realtime(&target);
    if (status != 0)
    {
        printf("DATE SMOKE FAIL set_realtime rechazo la fecha (%s)\n",
               result_error_string(status));
        return 1;
    }
    if (!read_rtc(&back))
    {
        puts_out("DATE SMOKE FAIL no se pudo releer tras fijar\n");
        failures += 1;
    }
    else if (!matches_target(&back, DATETEST_YEAR, DATETEST_MONTH, DATETEST_DAY,
                             DATETEST_HOUR, DATETEST_MINUTE, DATETEST_SECOND))
    {
        printf("DATE SMOKE FAIL releido %04u-%02u-%02u %02u:%02u:%02u, esperado 2031-06-15 12:34:30\n",
               (unsigned int)back.year, (unsigned int)back.month, (unsigned int)back.day,
               (unsigned int)back.hour, (unsigned int)back.minute, (unsigned int)back.second);
        failures += 1;
    }
    else
    {
        printf("date: fijado y releido %04u-%02u-%02u %02u:%02u:%02u\n",
               (unsigned int)back.year, (unsigned int)back.month, (unsigned int)back.day,
               (unsigned int)back.hour, (unsigned int)back.minute, (unsigned int)back.second);
    }

    /* Una fecha imposible tiene que volver -EINVAL, no tocar el chip. */
    target.month = 13;
    status = set_realtime(&target);
    if (status != (long)(-(long)SAVANXP_EINVAL))
    {
        printf("DATE SMOKE FAIL mes 13 devolvio %d (%s), esperado -EINVAL\n",
               (int)status, result_error_string(status));
        failures += 1;
    }
    if (!read_rtc(&back))
    {
        puts_out("DATE SMOKE FAIL no se pudo releer tras el rechazo\n");
        failures += 1;
    }
    else if (!matches_target(&back, DATETEST_YEAR, DATETEST_MONTH, DATETEST_DAY,
                             DATETEST_HOUR, DATETEST_MINUTE, DATETEST_SECOND))
    {
        puts_out("DATE SMOKE FAIL el rechazo toco el reloj\n");
        failures += 1;
    }

    /* Restaurar la hora que habia, sea cual sea el resultado anterior. */
    status = set_realtime(&original);
    if (status != 0)
    {
        printf("DATE SMOKE FAIL no se pudo restaurar (%s)\n", result_error_string(status));
        return 1;
    }
    if (!read_rtc(&back))
    {
        puts_out("DATE SMOKE FAIL no se pudo releer tras restaurar\n");
        return 1;
    }
    if ((unsigned int)back.year != (unsigned int)original.year ||
        (unsigned int)back.month != (unsigned int)original.month ||
        (unsigned int)back.day != (unsigned int)original.day ||
        (unsigned int)back.hour != (unsigned int)original.hour)
    {
        puts_out("DATE SMOKE FAIL la restauracion no volvio\n");
        return 1;
    }

    if (failures != 0)
    {
        printf("DATE SMOKE FAIL %d checks\n", failures);
        return 1;
    }
    puts_out("DATE SMOKE PASS fijar, rechazar y restaurar el RTC\n");
    return 0;
}
