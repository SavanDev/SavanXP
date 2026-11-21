#include "libc.h"

/*
 * Harness headless del TCP del kernel, y a diferencia de nettest esta no mira
 * el driver: mira el protocolo. Lo que se valida es que el stream sobreviva a
 * una red que pierde y desordena segmentos, que es exactamente lo que ni slirp
 * ni una LAN de laboratorio hacen cuando uno los necesita. Por eso el kernel
 * expone NET_IOC_SET_TCP_FAULT: las perdidas y las inversiones las produce el
 * propio stack, aguas abajo del checksum, y el resto del camino no se entera.
 *
 * El otro extremo es tools/tcp_echo_server.py corriendo en el host, al que se
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
 * La ultima fase no mira el protocolo sino el CONTRATO del socket, que es lo
 * que decide si un programa portado compila y anda: los plazos, las escrituras
 * cortas, y que el read bloqueante estacione al proceso en vez de girar
 * quemando CPU.
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

/* Ticks de CPU que consumio ESTE proceso, contra los que consumio la maquina.
 * Los dos solo significan algo juntos y del mismo par de muestras. */
static int sample_cpu(unsigned long* mine, unsigned long* total) {
    struct savanxp_system_info system;
    struct savanxp_process_info process;
    unsigned long index;
    long self = savanxp_getpid();

    memset(&system, 0, sizeof(system));
    if (system_info(&system) < 0) {
        return 0;
    }
    *total = (unsigned long)system.cpu_ticks_total;

    /* Las ranuras son ralas: se barren todas y se saltean las vacias, en vez de
     * confiar en que process_count sea el indice mas alto. */
    for (index = 0; index < 64u; ++index) {
        memset(&process, 0, sizeof(process));
        if (proc_info(index, &process) < 0) {
            continue;
        }
        if ((long)process.pid == self) {
            *mine = (unsigned long)process.cpu_ticks;
            return 1;
        }
    }
    return 0;
}

static int set_fault_profile(int net_fd, unsigned int drop_tx, unsigned int drop_rx, unsigned int reorder_rx,
    unsigned int corrupt_rx) {
    struct savanxp_net_tcp_fault fault;
    memset(&fault, 0, sizeof(fault));
    fault.drop_tx_every = drop_tx;
    fault.drop_rx_every = drop_rx;
    fault.reorder_rx_every = reorder_rx;
    fault.corrupt_rx_every = corrupt_rx;
    return savanxp_ioctl(net_fd, NET_IOC_SET_TCP_FAULT, (unsigned long)&fault) >= 0;
}

static int set_fault(int net_fd, unsigned int drop_tx, unsigned int drop_rx, unsigned int reorder_rx) {
    return set_fault_profile(net_fd, drop_tx, drop_rx, reorder_rx, 0);
}

/* Tres cosas que rompen a cualquier programa de red portado, y una cuarta que
 * decide si el sistema se puede usar mientras espera. */
static int check_socket_contract(int fd) {
    unsigned char buffer[64];
    unsigned long cpu_before = 0;
    unsigned long cpu_after = 0;
    unsigned long total_before = 0;
    unsigned long total_after = 0;
    int sampled;
    long status;

    /* 1. Una respuesta que tarda mas que el viejo tope fijo de 5 s. Con el plazo
     * por defecto (0 = esperar indefinidamente, como POSIX) tiene que llegar. */
    if (!send_command(fd, "DELAY 7000", 16u)) {
        return 0;
    }
    sampled = sample_cpu(&cpu_before, &total_before);
    if (!read_and_verify(fd, 0, 16u)) {
        eprintf("tcptest: a 7 s reply did not arrive\n");
        return 0;
    }

    /* 2. Y mientras esperaba esos 7 s el proceso tiene que haber estado
     * estacionado, no girando: si el read bloqueante gira con hlt en el
     * contexto del que llama, el proceso nunca sale de running y se lleva casi
     * todos los ticks de la espera. Es el mismo defecto que tenia poll(). */
    if (sampled && sample_cpu(&cpu_after, &total_after)) {
        const unsigned long mine = cpu_after - cpu_before;
        const unsigned long everyone = total_after - total_before;
        printf("tcptest: waiting cost %u of %u ticks\n", (unsigned int)mine, (unsigned int)everyone);
        if (everyone >= 100u && mine > everyone / 4u) {
            eprintf("tcptest: the blocking read spun instead of parking\n");
            return 0;
        }
    }

    /* 3. SO_RCVTIMEO se respeta. Se lee con NADA pendiente a proposito: pedirle
     * al servidor una respuesta demorada y correr contra ella hace depender el
     * resultado de cuanto avanza el guest en ese rato, y con el host cargado el
     * dato llega antes de que el read arranque. Sin nada del otro lado, lo unico
     * que puede terminar este read es el plazo. */
    if (savanxp_setsockopt(fd, SAVANXP_SO_RCVTIMEO, 500u) < 0) {
        eprintf("tcptest: setsockopt failed\n");
        return 0;
    }
    {
        const unsigned long before = uptime_ms();
        status = savanxp_read(fd, buffer, sizeof(buffer));
        printf("tcptest: timed read returned %d after %u ms\n",
               (int)status, (unsigned int)(uptime_ms() - before));
    }
    if (!result_is_error(status) || result_error_code(status) != SAVANXP_ETIMEDOUT) {
        eprintf("tcptest: SO_RCVTIMEO did not fire, read returned %d\n", (int)status);
        return 0;
    }

    /* Y el socket sigue vivo despues de que su propio plazo venciera. */
    if (savanxp_setsockopt(fd, SAVANXP_SO_RCVTIMEO, 0u) < 0) {
        return 0;
    }
    if (!send_command(fd, "DELAY 1000", 16u) || !read_and_verify(fd, 0, 16u)) {
        eprintf("tcptest: the socket did not survive its own timeout\n");
        return 0;
    }

    /* 4. Una escritura mas grande que un segmento devuelve lo que entro, no
     * EINVAL, y recv()/send() andan sobre un stream. */
    if (!send_command(fd, "ECHO", 4096u)) {
        return 0;
    }
    {
        unsigned char big[4096];
        unsigned int offset;
        for (offset = 0; offset < sizeof(big); ++offset) {
            big[offset] = pattern_byte(offset);
        }
        status = savanxp_write(fd, big, sizeof(big));
        if (status <= 0 || (size_t)status >= sizeof(big)) {
            eprintf("tcptest: a 4096 byte write returned %d\n", (int)status);
            return 0;
        }
        printf("tcptest: short write returned %u of 4096\n", (unsigned int)status);

        /* El resto sale por send() y vuelve por recv(): sin ellos, ningun
         * codigo de red portado enlaza siquiera. */
        {
            size_t written = (size_t)status;
            while (written < sizeof(big)) {
                size_t chunk = sizeof(big) - written;
                if (chunk > CHUNK_BYTES) {
                    chunk = CHUNK_BYTES;
                }
                status = savanxp_sendto(fd, big + written, chunk, 0);
                if (status <= 0) {
                    eprintf("tcptest: send failed (%s)\n", result_error_string(status));
                    return 0;
                }
                written += (size_t)status;
            }
        }
        {
            unsigned int received = 0;
            while (received < sizeof(big)) {
                unsigned int index;
                size_t want = sizeof(big) - received;
                if (want > CHUNK_BYTES) {
                    want = CHUNK_BYTES;
                }
                status = savanxp_recvfrom(fd, buffer, want < sizeof(buffer) ? want : sizeof(buffer), 0, 0);
                if (status <= 0) {
                    eprintf("tcptest: recv failed (%s)\n", result_error_string(status));
                    return 0;
                }
                for (index = 0; index < (unsigned int)status; ++index) {
                    if (buffer[index] != pattern_byte(received + index)) {
                        eprintf("tcptest: recv returned the wrong byte at %u\n", received + index);
                        return 0;
                    }
                }
                received += (unsigned int)status;
            }
        }
    }

    puts_out("tcptest: socket contract ok\n");
    return 1;
}

/* A blocking connect deadline must disarm its SYN retry slot as well as
 * return ETIMEDOUT. Service the stack after the initial RTO to catch retries
 * accidentally left alive after the caller's shorter deadline. */
static int check_connect_timeout(const struct savanxp_sockaddr_in* peer) {
    long net_fd = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    long fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
    struct savanxp_net_tcp_stats before = {0}, after = {0};
    unsigned char byte;
    int ok = 0;
    if (net_fd < 0 || fd < 0) goto cleanup;
    if (!set_fault((int)net_fd, 1, 0, 0)) goto cleanup;
    if (savanxp_connect((int)fd, peer, 20) != -SAVANXP_ETIMEDOUT) goto cleanup;
    if (savanxp_ioctl((int)net_fd, NET_IOC_GET_TCP_STATS, (unsigned long)&before) < 0) goto cleanup;
    sleep_ms(500);
    if (savanxp_poll(0, 0, 0) != 0) goto cleanup;
    if (savanxp_ioctl((int)net_fd, NET_IOC_GET_TCP_STATS, (unsigned long)&after) < 0) goto cleanup;
    if (after.retransmits != before.retransmits || after.segments_sent != before.segments_sent) goto cleanup;
    if (savanxp_read((int)fd, &byte, 1) != -SAVANXP_ETIMEDOUT) goto cleanup;
    {
        struct savanxp_pollfd ready = {(int)fd, 0, 0};
        if (savanxp_poll(&ready, 1, 0) != 1 || !(ready.revents & SAVANXP_POLLERR)) goto cleanup;
        ready.events = SAVANXP_POLLIN | SAVANXP_POLLOUT;
        if (savanxp_poll(&ready, 1, 0) != 1 ||
            (ready.revents & (SAVANXP_POLLIN | SAVANXP_POLLOUT | SAVANXP_POLLERR)) !=
                (SAVANXP_POLLIN | SAVANXP_POLLOUT | SAVANXP_POLLERR)) goto cleanup;
    }
    ok = 1;
cleanup:
    if (net_fd >= 0) {
        if (!set_fault((int)net_fd, 0, 0, 0)) ok = 0;
        savanxp_close((int)net_fd);
    }
    if (fd >= 0) savanxp_close((int)fd);
    if (ok) puts_out("tcptest: connect timeout stops SYN retransmission\n");
    return ok;
}

/* Nothing below polls a socket during sleep. The read-only diagnostics do
 * not service the stack either: retransmission must come from the heartbeat. */
static int check_independent_close(const struct savanxp_sockaddr_in* peer) {
    long device = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    long a = -1, b = -1, probe = -1, held = -1, lost = -1;
    struct savanxp_sockaddr_in local = {0};
    struct savanxp_net_tcp_stats before = {0}, after = {0};
    struct savanxp_net_tcp_lifetimes life = {0}, baseline = {0};
    unsigned char bytes[64];
    unsigned i;
    int ok = 0;
    local.port = 41001;
    if (device < 0) goto cleanup;
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&baseline) < 0) goto cleanup;
    a = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, 0);
    b = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, 0);
    if (a < 0 || b < 0 || savanxp_bind((int)a, &local) < 0) goto cleanup;
    if (savanxp_connect((int)a, peer, 5000) < 0 || savanxp_connect((int)b, peer, 5000) < 0) goto cleanup;
    if (!write_all((int)a, (const unsigned char*)"SAVE close 64\n", 14) || !send_command((int)b, "ECHO", 64)) goto cleanup;
    if (savanxp_fcntl((int)a, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0 ||
        savanxp_fcntl((int)b, SAVANXP_F_SETFL, SAVANXP_OPEN_NONBLOCK) < 0) goto cleanup;
    for (i = 0; i < sizeof(bytes); ++i) bytes[i] = pattern_byte(i);
    if (!set_fault((int)device, 1, 0, 0)) goto cleanup;
    if (savanxp_write((int)a, bytes, sizeof(bytes)) != sizeof(bytes) ||
        savanxp_write((int)b, bytes, sizeof(bytes)) != sizeof(bytes)) goto cleanup;
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_STATS, (unsigned long)&before) < 0) goto cleanup;
    sleep_ms(500);
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_STATS, (unsigned long)&after) < 0 ||
        after.retransmits < before.retransmits + 2) goto cleanup;
    puts_out("tcptest: simultaneous transmissions retried without consumer polling\n");
    if (savanxp_close((int)a) < 0) goto cleanup;
    a = -1;
    if (!set_fault((int)device, 0, 0, 0)) goto cleanup;
    sleep_ms(2000);
    if (savanxp_fcntl((int)b, SAVANXP_F_SETFL, 0) < 0 || !read_and_verify((int)b, 0, 64)) goto cleanup;
    if (!write_all((int)b, (const unsigned char*)"SAVED close\n", 12) || !read_and_verify((int)b, 0, 64)) goto cleanup;
    puts_out("tcptest: close drains accepted data before FIN\n");
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&life) < 0) goto cleanup;
    {
        const unsigned fin_before = life.fin_retransmits;
        if (!set_fault((int)device, 1, 0, 0)) goto cleanup;
        if (savanxp_close((int)b) < 0) goto cleanup;
        b = -1;
        sleep_ms(500);
        if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&life) < 0 ||
            life.fin_retransmits <= fin_before) goto cleanup;
    }
    if (!set_fault((int)device, 0, 0, 0)) goto cleanup;
    sleep_ms(2000);
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&life) < 0 ||
        life.time_wait < 2 || life.closing != 0 || life.close_timeouts != baseline.close_timeouts) goto cleanup;
    probe = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, 0);
    if (probe < 0 || savanxp_bind((int)probe, &local) != -SAVANXP_EBUSY) goto cleanup;
    puts_out("tcptest: lost FIN retried; TIME_WAIT reserves the tuple\n");
    // One peer ACKs FIN but keeps its write half open; another never sees
    // FIN at all. Both orphan paths must release their slots independently.
    held = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, 0);
    lost = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, 0);
    if (held < 0 || lost < 0 || savanxp_connect((int)held, peer, 5000) < 0 ||
        savanxp_connect((int)lost, peer, 5000) < 0) goto cleanup;
    if (!send_command((int)held, "HOLD", 120000)) goto cleanup;
    {
        unsigned got = 0;
        while (got < 6) {
            long n = savanxp_read((int)held, bytes + got, 6 - got);
            if (n <= 0) goto cleanup;
            got += (unsigned)n;
        }
        for (got = 0; got < 6; ++got) if (bytes[got] != (unsigned char)"READY\n"[got]) goto cleanup;
    }
    if (savanxp_close((int)held) < 0) goto cleanup;
    held = -1;
    sleep_ms(500);
    if (!set_fault((int)device, 1, 0, 0) || savanxp_close((int)lost) < 0) goto cleanup;
    lost = -1;
    sleep_ms(18000);
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&life) < 0 ||
        life.closing != 1 || life.close_timeouts != baseline.close_timeouts + 1) goto cleanup;
    if (!set_fault((int)device, 0, 0, 0)) goto cleanup;
    puts_out("tcptest: exhausted FIN retries reclaimed; half-closed peer remains bounded\n");
    sleep_ms(61000);
    if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&life) < 0 ||
        life.closing != 0 || life.time_wait != 0 || life.attached != 1 ||
        life.close_timeouts != baseline.close_timeouts + 2 || savanxp_bind((int)probe, &local) != 0) goto cleanup;
    puts_out("tcptest: TIME_WAIT expired and protocol slots reclaimed\n");
    ok = 1;
cleanup:
    if (device >= 0) (void)set_fault((int)device, 0, 0, 0);
    if (a >= 0) savanxp_close((int)a);
    if (b >= 0) savanxp_close((int)b);
    if (probe >= 0) savanxp_close((int)probe);
    if (held >= 0) savanxp_close((int)held);
    if (lost >= 0) savanxp_close((int)lost);
    if (device >= 0) savanxp_close((int)device);
    return ok;
}

/* More active closes than the socket table has slots: each leaves a
 * TIME_WAIT behind, and once the table is full a new socket must take the
 * TIME_WAIT closest to expiry instead of failing with ENOMEM. */
#define RECLAIM_CONNECTIONS 40

static int check_time_wait_reclaim(const struct savanxp_sockaddr_in* peer) {
    long device = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    struct savanxp_net_tcp_lifetimes life = {0};
    const unsigned long started = uptime_ms();
    unsigned i;
    int ok = 0;
    if (device < 0) goto cleanup;
    for (i = 0; i < RECLAIM_CONNECTIONS; ++i) {
        long fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, 0);
        unsigned waited = 0;
        if (fd < 0) {
            eprintf("tcptest: socket %u of %u failed (%s)\n", i + 1, RECLAIM_CONNECTIONS,
                result_error_string(fd));
            goto cleanup;
        }
        if (savanxp_connect((int)fd, peer, 5000) < 0) {
            savanxp_close((int)fd);
            goto cleanup;
        }
        /* We close first; the echo server sees EOF and closes too, which
         * leaves this side in TIME_WAIT. */
        savanxp_close((int)fd);
        do {
            sleep_ms(50);
            waited += 50;
            if (savanxp_ioctl((int)device, NET_IOC_GET_TCP_LIFETIMES, (unsigned long)&life) < 0) goto cleanup;
        } while (life.closing != 0 && waited < 5000);
        if (life.closing != 0) goto cleanup;
    }
    /* Within the 60 s a TIME_WAIT lasts, so none expired on its own: every
     * socket past the table's size had to reclaim one. */
    if (uptime_ms() - started >= 55000) {
        eprintf("tcptest: %u connections took too long to prove reclamation\n", RECLAIM_CONNECTIONS);
        goto cleanup;
    }
    /* The table really filled with TIME_WAIT on the way. */
    if (life.time_wait < 24 || life.attached != 0) {
        eprintf("tcptest: time_wait=%u attached=%u after reclaiming\n", life.time_wait, life.attached);
        goto cleanup;
    }
    puts_out("tcptest: a full table reclaims the TIME_WAIT closest to expiry\n");
    ok = 1;
cleanup:
    if (device >= 0) savanxp_close((int)device);
    return ok;
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

    /* Steady-state profile: loss in both directions plus swapped pairs, which
     * is what makes the reassembly work, and one data segment in seven with a
     * flipped byte. The checksum has to drop those: read_and_verify compares
     * every byte, so one that got through fails the test. */
    if (!set_fault_profile((int)net_fd, 3, 11, 4, 7)) {
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

    /* Las fallas se apagan: lo que sigue mide el contrato del socket, no la
     * resistencia del protocolo, y los plazos tienen que ser legibles. */
    (void)set_fault((int)net_fd, 0, 0, 0);

    if (!check_socket_contract((int)fd)) {
        savanxp_close((int)fd);
        savanxp_close((int)net_fd);
        return fail("socket contract");
    }

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

    if (!check_connect_timeout(&address)) return fail("connect timeout cleanup");

    if (!check_independent_close(&address)) return fail("independent progress and close lifetime");

    if (!check_time_wait_reclaim(&address)) return fail("TIME_WAIT reclaimed when the table is full");

    puts_out("TCP SMOKE PASS\n");
    return 0;
}
