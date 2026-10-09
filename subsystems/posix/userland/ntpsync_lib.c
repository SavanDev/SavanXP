#include "libc.h"
#include "ntpsync_lib.h"

#include <time.h>

/* Hora de red a reloj de pared, con desconfianza: lo que viene por UDP decide
 * la hora del sistema, asi que cada campo se valida antes de creerle. El
 * retardo se compensa a lo SNTP simplificado (transmit + rtt/2): en una LAN
 * son milesimas y el RTC solo tiene segundos, pero gratis no se deja nada.
 *
 * Rangos: segundos NTP (era 1900) menos 2208988800 dan Unix. Se acepta
 * 2020-01-01..2036-02-07: abajo de 2020 es basura o la ambiguedad de era del
 * contador de 32 bits (que envuelve en 2036), y arriba no hay contador. El
 * SET_REALTIME ya valida el dia contra su mes, aca solo el ano grueso. */

#define NTPSYNC_PACKET_BYTES 48u
#define NTPSYNC_UNIX_OFFSET 2208988800ul
#define NTPSYNC_MIN_UNIX 1577836800l
#define NTPSYNC_MAX_UNIX 2085978495l

static uint32_t read_be32(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static long net_up(void)
{
    long fd = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    long status;

    if (fd < 0)
    {
        return fd;
    }
    status = savanxp_ioctl((int)fd, NET_IOC_UP, 0);
    savanxp_close((int)fd);
    return status;
}

long ntp_sync_once(uint32_t ipv4, unsigned int port, unsigned long timeout_ms,
                   long *applied_unix, unsigned long *rtt_ms)
{
    unsigned char request[NTPSYNC_PACKET_BYTES];
    unsigned char reply[NTPSYNC_PACKET_BYTES];
    struct savanxp_sockaddr_in server;
    struct savanxp_sockaddr_in from;
    struct savanxp_realtime now;
    struct tm fields;
    unsigned int version;
    unsigned long start_ms;
    unsigned long end_ms;
    unsigned long rtt;
    unsigned long long applied_ms;
    time_t applied;
    uint32_t seconds;
    uint32_t fraction;
    long fd;
    long status;

    if (applied_unix == 0 || rtt_ms == 0 || port == 0 || port > 65535u || timeout_ms == 0)
    {
        return -(long)SAVANXP_EINVAL;
    }

    status = net_up();
    if (status < 0)
    {
        return status;
    }

    fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (fd < 0)
    {
        return fd;
    }

    memset(request, 0, sizeof(request));
    request[0] = 0x1bu; /* LI=0, VN=3, modo 3 (cliente). */
    memset(&server, 0, sizeof(server));
    server.ipv4 = ipv4;
    server.port = (uint16_t)port;

    start_ms = uptime_ms();
    status = savanxp_sendto((int)fd, request, sizeof(request), &server);
    if (status >= 0)
    {
        memset(&from, 0, sizeof(from));
        status = savanxp_recvfrom((int)fd, reply, sizeof(reply), &from, timeout_ms);
    }
    end_ms = uptime_ms();
    rtt = end_ms >= start_ms ? end_ms - start_ms : 0ul;
    savanxp_close((int)fd);
    if (status < 0)
    {
        return status;
    }
    if (status != (long)sizeof(reply))
    {
        return -(long)SAVANXP_EIO;
    }

    /* Modo 4 (servidor), version 3 o 4, estrato 1..15: 0 es kiss-of-death y
     * 16 es "no sincronizado". Transmit en cero es "no hay hora". */
    if ((reply[0] & 0x07u) != 4u)
    {
        return -(long)SAVANXP_EIO;
    }
    version = ((unsigned int)reply[0] >> 3) & 0x07u;
    if (version < 3u || version > 4u)
    {
        return -(long)SAVANXP_EIO;
    }
    if (reply[1] == 0 || reply[1] > 15)
    {
        return -(long)SAVANXP_EIO;
    }

    seconds = read_be32(reply + 40);
    fraction = read_be32(reply + 44);
    if (seconds == 0)
    {
        return -(long)SAVANXP_EIO;
    }

    applied_ms = (unsigned long long)seconds * 1000ull +
                 (((unsigned long long)fraction * 1000ull) >> 32) +
                 (unsigned long long)(rtt / 2ul);
    /* applied_ms sigue en era NTP (1900): a Unix restando el offset. */
    applied = (time_t)((applied_ms + 500ull) / 1000ull) - (time_t)NTPSYNC_UNIX_OFFSET;
    if (applied < NTPSYNC_MIN_UNIX || applied > NTPSYNC_MAX_UNIX)
    {
        return -(long)SAVANXP_EIO;
    }

    if (gmtime_r(&applied, &fields) == 0)
    {
        return -(long)SAVANXP_EIO;
    }
    memset(&now, 0, sizeof(now));
    now.year = (uint16_t)(fields.tm_year + 1900);
    now.month = (uint8_t)(fields.tm_mon + 1);
    now.day = (uint8_t)fields.tm_mday;
    now.hour = (uint8_t)fields.tm_hour;
    now.minute = (uint8_t)fields.tm_min;
    now.second = (uint8_t)fields.tm_sec;
    status = set_realtime(&now);
    if (status != 0)
    {
        return status;
    }

    *applied_unix = (long)applied;
    *rtt_ms = rtt;
    return 0;
}
