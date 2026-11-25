/*
 * paritybench: the disk and TCP figures of the v0.4.0 parity review.
 *
 * Built unchanged with the v0.4.0 SDK and with the i386 SDK, then booted as
 * /disk/bin/smoke under /SMOKE=smoke (docs/I386_PORT_STUDY.md, section 37).
 * It writes and syncs a 512 KiB file in 64 KiB chunks, reads it back and
 * verifies it, then receives 4 MiB from tools/tcp_echo_server.py on the host
 * (BULK) and sends 1 MiB through ECHO. Times come from uptime_ms().
 */
#include "savanxp/libc.h"

#include <stdint.h>
#include <string.h>

#define CHUNK (64u * 1024u)
#define FILE_BYTES (512u * 1024u)
#define BULK_BYTES (4u * 1024u * 1024u)
#define ECHO_BYTES (1u * 1024u * 1024u)
#define BENCH_PORT 40100u

static unsigned char g_buffer[CHUNK];
static unsigned char g_echo[CHUNK];

static unsigned char pattern(unsigned long offset) {
    return (unsigned char)((offset * 7u + 3u) & 0xffu);
}

static void report(const char* label, unsigned long bytes, unsigned long ms) {
    unsigned long kib_s = ms ? (unsigned long)((bytes / 1024u) * 1000u / ms) : 0;
    printf("paritybench: %s %lu KiB in %lu ms (%lu KiB/s)\n", label, bytes / 1024u, ms, kib_s);
}

static int disk(void) {
    const char* path = "/disk/paritybench.bin";
    long fd = savanxp_open_mode(path, SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE |
                                      SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (fd < 0) return 0;
    unsigned long start = uptime_ms();
    for (unsigned long done = 0; done < FILE_BYTES; done += CHUNK) {
        for (unsigned long i = 0; i < CHUNK; ++i) g_buffer[i] = pattern(done + i);
        if (savanxp_write((int)fd, g_buffer, CHUNK) != (long)CHUNK) return 0;
    }
    savanxp_close((int)fd);
    if (savanxp_sync() < 0) return 0;
    report("disk write+sync", FILE_BYTES, uptime_ms() - start);

    fd = savanxp_open_mode(path, SAVANXP_OPEN_READ);
    if (fd < 0) return 0;
    start = uptime_ms();
    for (unsigned long done = 0; done < FILE_BYTES; done += CHUNK) {
        if (savanxp_read((int)fd, g_buffer, CHUNK) != (long)CHUNK) return 0;
        for (unsigned long i = 0; i < CHUNK; ++i)
            if (g_buffer[i] != pattern(done + i)) return 0;
    }
    report("disk read", FILE_BYTES, uptime_ms() - start);
    savanxp_close((int)fd);
    return savanxp_unlink(path) == 0;
}

static int send_all(int fd, const void* data, unsigned long length) {
    const unsigned char* bytes = (const unsigned char*)data;
    while (length) {
        long sent = savanxp_write(fd, bytes, length);
        if (sent <= 0) return 0;
        bytes += sent;
        length -= (unsigned long)sent;
    }
    return 1;
}

static int receive(int fd, unsigned long length) {
    while (length) {
        long got = savanxp_read(fd, g_echo, length < CHUNK ? length : CHUNK);
        if (got <= 0) return 0;
        length -= (unsigned long)got;
    }
    return 1;
}

static int tcp(void) {
    long net = savanxp_open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (net < 0 || savanxp_ioctl((int)net, NET_IOC_UP, 0) < 0) return 0;
    long fd = savanxp_socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
    if (fd < 0) return 0;
    struct savanxp_sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.ipv4 = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
    address.port = (uint16_t)BENCH_PORT;
    if (savanxp_connect((int)fd, &address, 8000) < 0) return 0;

    unsigned long start = uptime_ms();
    if (!send_all((int)fd, "BULK 4194304\n", 13) || !receive((int)fd, BULK_BYTES)) return 0;
    report("tcp receive", BULK_BYTES, uptime_ms() - start);

    start = uptime_ms();
    if (!send_all((int)fd, "ECHO 1048576\n", 13)) return 0;
    for (unsigned long done = 0; done < ECHO_BYTES; done += CHUNK) {
        memset(g_buffer, (int)(done >> 16), CHUNK);
        if (!send_all((int)fd, g_buffer, CHUNK) || !receive((int)fd, CHUNK)) return 0;
    }
    report("tcp echo", ECHO_BYTES, uptime_ms() - start);
    (void)send_all((int)fd, "QUIT\n", 5);
    savanxp_close((int)fd);
    savanxp_close((int)net);
    return 1;
}

int main(void) {
    if (!disk()) {
        puts_out("paritybench: disk FAIL\nSMOKE FAIL\n");
        return 1;
    }
    if (!tcp()) {
        puts_out("paritybench: tcp FAIL\nSMOKE FAIL\n");
        return 1;
    }
    puts_out("SMOKE PASS\n");
    return 0;
}
