#include "libc.h"

/* Hora del sistema en UTC, que es lo que el RTC entrega (ver time.h: no hay
 * zonas horarias y localtime es gmtime). Sin argumentos la muestra; con seis
 * la fija. Es la configuracion manual que acompana al reloj de la taskbar. */

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

static void print_realtime(const struct savanxp_realtime *now)
{
    printf("%04u-%02u-%02u %02u:%02u:%02u\n",
           (unsigned int)now->year,
           (unsigned int)now->month,
           (unsigned int)now->day,
           (unsigned int)now->hour,
           (unsigned int)now->minute,
           (unsigned int)now->second);
}

int main(int argc, char **argv)
{
    struct savanxp_realtime now;
    long status;

    if (argc == 1)
    {
        memset(&now, 0, sizeof(now));
        status = realtime(&now);
        if (status != 0 || now.valid == 0)
        {
            eprintf("date: sin reloj de tiempo real (%s)\n", result_error_string(status));
            return 1;
        }
        print_realtime(&now);
        return 0;
    }
    if (argc == 7)
    {
        unsigned int fields[6];
        int index;

        for (index = 0; index < 6; ++index)
        {
            if (!parse_uint(argv[index + 1], &fields[index]))
            {
                puts_fd(2, "usage: date [YYYY MM DD HH MM SS] (UTC)\n");
                return 1;
            }
        }
        memset(&now, 0, sizeof(now));
        now.year = (uint16_t)fields[0];
        now.month = (uint8_t)fields[1];
        now.day = (uint8_t)fields[2];
        now.hour = (uint8_t)fields[3];
        now.minute = (uint8_t)fields[4];
        now.second = (uint8_t)fields[5];
        /* El kernel valida el rango (anio 2000..2099, dia existente en su
         * mes): -EINVAL es fecha mala, -EIO es el chip atascado. */
        status = set_realtime(&now);
        if (status != 0)
        {
            eprintf("date: no se pudo fijar (%s)\n", result_error_string(status));
            return 1;
        }
        memset(&now, 0, sizeof(now));
        status = realtime(&now);
        if (status != 0 || now.valid == 0)
        {
            eprintf("date: se fijo pero no se pudo releer (%s)\n", result_error_string(status));
            return 1;
        }
        print_realtime(&now);
        return 0;
    }
    puts_fd(2, "usage: date [YYYY MM DD HH MM SS] (UTC)\n");
    return 1;
}
