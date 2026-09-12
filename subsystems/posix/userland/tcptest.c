#include "libc.h"

/*
 * Harness headless del TCP del kernel, y a diferencia de nettest esta no mira
 * el driver: mira el protocolo. Lo que se valida es que el stream sobreviva a
 * una red que pierde y desordena segmentos, que es exactamente lo que ni slirp
 * ni una LAN de laboratorio hacen cuando uno los necesita. Por eso el kernel
 * expone NET_IOC_SET_TCP_FAULT: las perdidas y las inversiones las produce el
 * propio stack, aguas abajo del checksum, y el resto del camino no se entera.
 *
 * El otro extremo es tools/tcp_echo_server.ps1 corriendo en el host, al que se
 * llega por 10.0.2.2 -- la direccion con la que el user-net de QEMU representa
 * al host. Se prueban las dos direcciones porque los caminos son distintos:
 *
 *   BULK  el host manda 32 KiB de un tiron. Ejercita el reensamblado (llegan
 *         agujeros y tramos fuera de orden) y la ventana anunciada, que con
 *         8 KiB de buffer se cierra y se reabre varias veces.
 *   ECHO  mandamos 8 KiB de a 1 KiB y nos los devuelven. Ejercita la
 *         retransmision propia: cada segmento que el inyector tira tiene que
 *         volver a salir solo, y el byte tiene que llegar igual.
 *
 * La prueba de que las fallas de verdad ocurrieron son los contadores de
 * NET_IOC_GET_TCP_STATS: un stack que solo funcione con la red perfecta
 * terminaria el test con retransmits y rx_out_of_order en cero.
 *
 * Ojo: el printf de userland solo entiende %s %d %u %x, sin ancho ni relleno.
 */

#define BULK_BYTES 32768u
#define ECHO_BYTES 8192u
#define CHUNK_BYTES 1024u

/* El host visto desde el guest con el user-net de QEMU. */
static const uint32_t kHostIpv4 = (10u << 24) | (0u << 16) | (2u << 8) | 2u;

static int fail(const char* reason) {
    printf("TCP SMOKE FAIL %s\n", reason);
    return 1;
}

/* Mismo patron que genera el servidor del host. Cualquier byte fuera de lugar
 * -- un tramo reensamblado al reves, un duplicado contado de mas -- rompe la
 * comparacion. */
static unsigned char pattern_byte(unsigned int index) {
    return (unsigned char)((index * 31u + 7u) & 0xffu);
}

static int parse_uint(const char* text, unsigned int* value) {
    unsigned int result = 0;
    size_t index = 0;
    if (text == 0 || text[0] == '\0') {
        return 0;
    }
    while (text[index] != '\0') {
        if (text[index] < '0' || text[index] > '9') {
            return 0;
        }
        result = result * 10u + (unsigned int)(text[index] - '0');
        ++index;
    }
    *value = result;
    return 1;
}

static size_t append_text(char* buffer, size_t capacity, size_t length, const char* text) {
    size_t text_length = strlen(text);
    if (length + text_length >= capacity) {
        return capacity;
    }
    memcpy(buffer + length, text, text_length);
    length += text_length;
    buffer[length] = '\0';
    return length;
}

static size_t append_uint(char* buffer, size_t capacity, size_t length, unsigned int value) {
    char digits[16];
    size_t count = 0;
    if (value == 0) {
        digits[count++] = '0';
    }
    while (value != 0) {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (count != 0) {
        if (length + 1 >= capacity) {
            return capacity;
        }
        buffer[length++] = digits[--count];
    }
    buffer[length] = '\0';
    return length;
}

/* write() manda un segmento por llamada y ya, asi que hasta un comando corto
 * puede salir cortado si algun dia sube el tope. */
static int write_all(int fd, const unsigned char* data, size_t length) {
    size_t written = 0;
    while (written < length) {
        size_t chunk = length - written;
        long status;
        if (chunk > CHUNK_BYTES) {
            chunk = CHUNK_BYTES;
        }
        status = savanxp_write(fd, data + written, chunk);
        if (status <= 0) {
            eprintf("tcptest: write failed (%s)\n", result_error_string(status));
            return 0;
        }
        written += (size_t)status;
    }
    return 1;
}

static int send_command(int fd, const char* verb, unsigned int count) {
    char command[64];
    size_t length = 0;
    command[0] = '\0';
    length = append_text(command, sizeof(command), length, verb);
    length = append_text(command, sizeof(command), length, " ");
    length = append_uint(command, sizeof(command), length, count);
    length = append_text(command, sizeof(command), length, "\n");
    if (length >= sizeof(command)) {
        return 0;
    }
    return write_all(fd, (const unsigned char*)command, length);
}

/* Lee count bytes y los compara contra el patron a partir de first_index. El
 * offset importa: en el ECHO lo que vuelve arranca donde arranco el chunk que
 * se mando, no en cero. */
static int read_and_verify(int fd, unsigned int first_index, unsigned int count) {
    unsigned char buffer[CHUNK_BYTES];
    unsigned int received = 0;
    while (received < count) {
        unsigned int remaining = count - received;
        unsigned int index;
        long status = savanxp_read(fd, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (status == 0) {
            eprintf("tcptest: peer closed after %u of %u bytes\n", received, count);
            return 0;
        }
        if (status < 0) {
            eprintf("tcptest: read failed (%s)\n", result_error_string(status));
            return 0;
        }
        for (index = 0; index < (unsigned int)status; ++index) {
            const unsigned char expected = pattern_byte(first_index + received + index);
            if (buffer[index] != expected) {
                eprintf(
                    "tcptest: byte %u is %x, expected %x\n",
                    first_index + received + index,
                    (unsigned int)buffer[index],
                    (unsigned int)expected
                );
                return 0;
            }
        }
        received += (unsigned int)status;
    }
    return 1;
}

static int set_fault(int net_fd, unsigned int drop_tx, unsigned int drop_rx, unsigned int reorder_rx) {
    struct savanxp_net_tcp_fault fault;
    memset(&fault, 0, sizeof(fault));
    fault.drop_tx_every = drop_tx;
    fault.drop_rx_every = drop_rx;
    fault.reorder_rx_every = reorder_rx;
    return savanxp_ioctl(net_fd, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) >= 0;
}

int main(int argc, char** argv) {
    long net_fd;
    long fd;
    long status;
    unsigned int port = 0;
    unsigned int index;
    struct savanxp_sockaddr_in address;
    struct savanxp_net_tcp_stats stats;

    puts_out("TCP SMOKE START\n");

    if (argc < 2 || !parse_uint(argv[1], &port) || port == 0 || port > 65535u) {
        return fail("usage: tcptest <port>");
    }

    net_fd = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (net_fd < 0) {
        return fail("/dev/net0 unavailable");
    }
    if (savanxp_ioctl((int)net_fd, NET_IOC_UP, 0) < 0) {
        savanxp_close((int)net_fd);
        return fail("NET_IOC_UP");
    }

    /* Primero el handshake con la mitad de los segmentos en el piso: sin
     * retransmision del SYN, un connect con perdidas no llega nunca. */
    if (!set_fault((int)net_fd, 2, 0, 0)) {
        savanxp_close((int)net_fd);
        return fail("NET_IOC_SET_TCP_FAULT");
    }

    fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
    if (fd < 0) {
        savanxp_close((int)net_fd);
        return fail("socket");
    }

    memset(&address, 0, sizeof(address));
    address.ipv4 = kHostIpv4;
    address.port = (uint16_t)port;
    status = savanxp_connect((int)fd, &address, 8000);
    if (status < 0) {
        eprintf("tcptest: connect failed (%s)\n", result_error_string(status));
        (void)set_fault((int)net_fd, 0, 0, 0);
        savanxp_close((int)fd);
        savanxp_close((int)net_fd);
        return fail("connect with 50% loss");
    }
    puts_out("tcptest: connected through 50% tx loss\n");

    /* Perfil de regimen: perdida en las dos direcciones mas inversiones de
     * pares, que es lo que obliga al reensamblado a trabajar. */
    if (!set_fault((int)net_fd, 3, 11, 4)) {
        savanxp_close((int)fd);
        savanxp_close((int)net_fd);
        return fail("NET_IOC_SET_TCP_FAULT");
    }

    /* Freno a proposito antes de leer: el host llena los 8 KiB del buffer de
     * recepcion, la ventana anunciada se cierra y hay que ver que se reabra
     * sola cuando la app se pone al dia. Es el caso que traba una conexion sin
     * hacer ruido, porque los dos lados quedan esperando al otro. */
    if (!send_command((int)fd, "BULK", BULK_BYTES)) {
        (void)set_fault((int)net_fd, 0, 0, 0);
        savanxp_close((int)fd);
        savanxp_close((int)net_fd);
        return fail("bulk command");
    }
    sleep_ms(250);

    if (!read_and_verify((int)fd, 0, BULK_BYTES)) {
        (void)set_fault((int)net_fd, 0, 0, 0);
        savanxp_close((int)fd);
        savanxp_close((int)net_fd);
        return fail("bulk receive");
    }
    printf("tcptest: bulk %u bytes verified\n", BULK_BYTES);

    if (!send_command((int)fd, "ECHO", ECHO_BYTES)) {
        (void)set_fault((int)net_fd, 0, 0, 0);
        savanxp_close((int)fd);
        savanxp_close((int)net_fd);
        return fail("echo command");
    }

    /* De a un chunk por vez y leyendo lo que vuelve: mandar los 8 KiB de
     * corrido llenaria la ventana de recepcion con el eco y trabaria a los dos
     * lados esperandose. */
    for (index = 0; index < ECHO_BYTES; index += CHUNK_BYTES) {
        unsigned char chunk[CHUNK_BYTES];
        unsigned int offset;
        for (offset = 0; offset < CHUNK_BYTES; ++offset) {
            chunk[offset] = pattern_byte(index + offset);
        }
        if (!write_all((int)fd, chunk, CHUNK_BYTES) || !read_and_verify((int)fd, index, CHUNK_BYTES)) {
            (void)set_fault((int)net_fd, 0, 0, 0);
            savanxp_close((int)fd);
            savanxp_close((int)net_fd);
            return fail("echo round trip");
        }
    }
    printf("tcptest: echo %u bytes verified\n", ECHO_BYTES);

    (void)set_fault((int)net_fd, 0, 0, 0);
    savanxp_close((int)fd);

    memset(&stats, 0, sizeof(stats));
    if (savanxp_ioctl((int)net_fd, NET_IOC_GET_TCP_STATS, (unsigned long)&stats) < 0) {
        savanxp_close((int)net_fd);
        return fail("NET_IOC_GET_TCP_STATS");
    }
    savanxp_close((int)net_fd);

    printf(
        "tcptest: sent=%u received=%u retransmits=%u ooo=%u dup=%u oow=%u winupd=%u aborts=%u\n",
        (unsigned int)stats.segments_sent,
        (unsigned int)stats.segments_received,
        (unsigned int)stats.retransmits,
        (unsigned int)stats.rx_out_of_order,
        (unsigned int)stats.rx_duplicates,
        (unsigned int)stats.rx_out_of_window,
        (unsigned int)stats.window_updates,
        (unsigned int)stats.aborts
    );

    /* Los datos ya se verificaron byte a byte; esto verifica que se hayan
     * verificado en el escenario dificil y no en uno donde el inyector no
     * llego a hacer nada. */
    if (stats.retransmits == 0) {
        return fail("no retransmissions happened");
    }
    if (stats.rx_out_of_order == 0) {
        return fail("no out-of-order segments happened");
    }
    if (stats.window_updates == 0) {
        return fail("the receive window never had to reopen");
    }
    if (stats.aborts != 0) {
        return fail("a connection was aborted");
    }

    puts_out("TCP SMOKE PASS\n");
    return 0;
}
