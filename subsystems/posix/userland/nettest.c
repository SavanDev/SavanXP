#include "libc.h"

/*
 * Harness headless del camino de red, pensado como red de seguridad del DRIVER
 * del NIC y no del stack: valida el enlace completo -- presencia por PCI, MAC
 * propia, ARP, transmision y recepcion -- contra el gateway del user-net de
 * QEMU, que es el unico destino que slirp contesta de forma deterministica
 * (sin depender de DNS ni de salida real a internet).
 *
 * La prueba de que el hardware movio trafico de verdad son los contadores
 * tx_frames/rx_frames del driver: un stack que respondiera desde una cache sin
 * tocar el device no los haria avanzar.
 *
 * Imprime NET SMOKE PASS / NET SMOKE FAIL para el harness de build.ps1.
 *
 * Ojo: el printf de userland solo entiende %s %d %u %x, sin ancho ni relleno.
 */

static int fail(const char* reason) {
    printf("NET SMOKE FAIL %s\n", reason);
    return 1;
}

static void print_ipv4(uint32_t address) {
    printf(
        "%u.%u.%u.%u",
        (unsigned int)((address >> 24) & 0xffu),
        (unsigned int)((address >> 16) & 0xffu),
        (unsigned int)((address >> 8) & 0xffu),
        (unsigned int)(address & 0xffu)
    );
}

static void print_mac(const uint8_t* mac) {
    int index;
    for (index = 0; index < 6; ++index) {
        if (index != 0) {
            puts_out(":");
        }
        printf("%x", (unsigned int)mac[index]);
    }
}

static int mac_is_zero(const uint8_t* mac) {
    int index;
    for (index = 0; index < 6; ++index) {
        if (mac[index] != 0) {
            return 0;
        }
    }
    return 1;
}

static long query_info(int fd, struct savanxp_net_info* info) {
    memset(info, 0, sizeof(*info));
    return savanxp_ioctl(fd, NET_IOC_GET_INFO, (unsigned long)info);
}

int main(void) {
    long fd;
    struct savanxp_net_info info;
    struct savanxp_net_info after;
    struct savanxp_net_ping_request request;
    struct savanxp_net_ping_result result;
    long status;
    int sequence;

    puts_out("NET SMOKE START\n");

    fd = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        return fail("/dev/net0 unavailable");
    }

    if (savanxp_ioctl((int)fd, NET_IOC_UP, 0) < 0) {
        savanxp_close((int)fd);
        return fail("NET_IOC_UP");
    }

    if (query_info((int)fd, &info) < 0) {
        savanxp_close((int)fd);
        return fail("NET_IOC_GET_INFO");
    }

    if (!info.present) {
        savanxp_close((int)fd);
        return fail("no NIC present");
    }
    if (!info.up) {
        savanxp_close((int)fd);
        return fail("link not up");
    }
    /* Una MAC en cero significa que el driver nunca leyo su identidad del
     * device: el struct quedo como lo dejo el memset. */
    if (mac_is_zero(info.mac)) {
        savanxp_close((int)fd);
        return fail("MAC all zero");
    }
    if (info.ipv4 == 0 || info.gateway == 0) {
        savanxp_close((int)fd);
        return fail("no address configured");
    }

    puts_out("nettest: ip=");
    print_ipv4(info.ipv4);
    puts_out(" gw=");
    print_ipv4(info.gateway);
    puts_out(" mac=");
    print_mac(info.mac);
    printf(" tx=%u rx=%u\n", (unsigned int)info.tx_frames, (unsigned int)info.rx_frames);

    /* Tres pings al gateway: el primero arrastra la resolucion ARP, los otros
     * dos ya van por la cache y confirman que el camino se sostiene. */
    for (sequence = 1; sequence <= 3; ++sequence) {
        memset(&request, 0, sizeof(request));
        memset(&result, 0, sizeof(result));
        request.ipv4 = info.gateway;
        request.timeout_ms = 2000;
        request.sequence = (uint16_t)sequence;
        request.payload_size = 32;
        request.result_ptr = (uint64_t)(unsigned long)&result;

        status = savanxp_ioctl((int)fd, NET_IOC_PING, (unsigned long)&request);
        if (status < 0) {
            struct savanxp_net_info diag;
            if (query_info((int)fd, &diag) >= 0) {
                printf("nettest: net0 state=%s\n", net_status_string(diag.last_status));
            }
            printf("nettest: ping %d failed (%s)\n", sequence, result_error_string(status));
            savanxp_close((int)fd);
            return fail("ICMP echo");
        }
        if (result.reply_ipv4 != info.gateway) {
            savanxp_close((int)fd);
            return fail("reply from wrong host");
        }

        printf("nettest: reply %d", sequence);
        printf(" time=%u ms ttl=%u\n", (unsigned int)result.elapsed_ms, (unsigned int)result.ttl);
    }

    /* Los contadores del driver tienen que haberse movido en los dos sentidos:
     * es lo que distingue "el stack contesto" de "el hardware transmitio". */
    if (query_info((int)fd, &after) < 0) {
        savanxp_close((int)fd);
        return fail("NET_IOC_GET_INFO after ping");
    }
    if (after.tx_frames <= info.tx_frames) {
        savanxp_close((int)fd);
        return fail("tx_frames did not advance");
    }
    if (after.rx_frames <= info.rx_frames) {
        savanxp_close((int)fd);
        return fail("rx_frames did not advance");
    }

    printf(
        "nettest: frames tx %u->%u rx %u->%u\n",
        (unsigned int)info.tx_frames,
        (unsigned int)after.tx_frames,
        (unsigned int)info.rx_frames,
        (unsigned int)after.rx_frames
    );

    /* Cache ARP con varias entradas. Hablar con un segundo host y volver al
     * gateway no puede costar una resolucion nueva: con la cache de una sola
     * entrada que habia antes, el segundo host desalojaba al gateway y este
     * ultimo ping volvia a salir a preguntar por el cable.
     *
     * El contador arp_requests solo cuenta las requests que EMITIMOS -- las
     * replies que mandamos a pedido de otro no lo mueven -- asi que es una
     * senial limpia de "hubo o no hubo resolucion".
     *
     * El segundo host es el .3 del user-net de QEMU (el DNS de slirp). Del
     * ping en si no esperamos nada: puede no contestar ICMP y no importa, lo
     * unico que este paso necesita es que conteste el ARP, que es lo que
     * mete la segunda entrada en la cache. */
    {
        struct savanxp_net_info mid;
        struct savanxp_net_info final;
        uint32_t second_host = info.gateway + 1u;

        memset(&request, 0, sizeof(request));
        memset(&result, 0, sizeof(result));
        request.ipv4 = second_host;
        request.timeout_ms = 2000;
        request.sequence = 4;
        request.payload_size = 32;
        request.result_ptr = (uint64_t)(unsigned long)&result;
        (void)savanxp_ioctl((int)fd, NET_IOC_PING, (unsigned long)&request);

        if (query_info((int)fd, &mid) < 0) {
            savanxp_close((int)fd);
            return fail("NET_IOC_GET_INFO after second host");
        }
        if (mid.arp_timeouts != after.arp_timeouts) {
            puts_out("nettest: no ARP reply from ");
            print_ipv4(second_host);
            puts_out("\n");
            savanxp_close((int)fd);
            return fail("second host did not resolve");
        }
        if (mid.arp_requests != after.arp_requests + 1u) {
            printf(
                "nettest: arp_requests %u->%u (esperaba una sola resolucion)\n",
                (unsigned int)after.arp_requests,
                (unsigned int)mid.arp_requests
            );
            savanxp_close((int)fd);
            return fail("unexpected ARP traffic for second host");
        }

        memset(&request, 0, sizeof(request));
        memset(&result, 0, sizeof(result));
        request.ipv4 = info.gateway;
        request.timeout_ms = 2000;
        request.sequence = 5;
        request.payload_size = 32;
        request.result_ptr = (uint64_t)(unsigned long)&result;
        if (savanxp_ioctl((int)fd, NET_IOC_PING, (unsigned long)&request) < 0) {
            savanxp_close((int)fd);
            return fail("gateway ping after second host");
        }

        if (query_info((int)fd, &final) < 0) {
            savanxp_close((int)fd);
            return fail("NET_IOC_GET_INFO after cache check");
        }
        if (final.arp_requests != mid.arp_requests) {
            printf(
                "nettest: arp_requests %u->%u, el gateway se cayo de la cache\n",
                (unsigned int)mid.arp_requests,
                (unsigned int)final.arp_requests
            );
            savanxp_close((int)fd);
            return fail("ARP cache holds a single entry");
        }

        printf(
            "nettest: cache arp ok, %u resolucion(es) para dos hosts\n",
            (unsigned int)(final.arp_requests - after.arp_requests)
        );
    }

    savanxp_close((int)fd);
    puts_out("NET SMOKE PASS\n");
    return 0;
}
