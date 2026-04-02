#include "libc.h"

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

static void print_ipv4(uint32_t address) {
    printf(
        "%u.%u.%u.%u",
        (unsigned int)((address >> 24) & 0xffu),
        (unsigned int)((address >> 16) & 0xffu),
        (unsigned int)((address >> 8) & 0xffu),
        (unsigned int)(address & 0xffu)
    );
}

int main(int argc, char** argv) {
    long fd;
    long status;
    struct savanxp_sockaddr_in local_address;
    struct savanxp_sockaddr_in remote_address;
    unsigned int port = 0;
    unsigned int timeout_ms = 5000;
    char buffer[513];

    if (argc < 2 || !parse_uint(argv[1], &port) || port == 0 || port > 65535u) {
        puts_err("usage: udprecv <port> [timeout_ms]\n");
        return 1;
    }
    if (argc >= 3 && (!parse_uint(argv[2], &timeout_ms))) {
        puts_err("usage: udprecv <port> [timeout_ms]\n");
        return 1;
    }

    fd = socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (fd < 0) {
        eprintf("udprecv: socket failed (%s)\n", result_error_string(fd));
        return 1;
    }

    memset(&local_address, 0, sizeof(local_address));
    local_address.port = (uint16_t)port;
    status = bind((int)fd, &local_address);
    if (status < 0) {
        eprintf("udprecv: bind failed (%s)\n", result_error_string(status));
        close((int)fd);
        return 1;
    }

    memset(buffer, 0, sizeof(buffer));
    memset(&remote_address, 0, sizeof(remote_address));
    status = recvfrom((int)fd, buffer, sizeof(buffer) - 1, &remote_address, timeout_ms);
    if (status < 0) {
        eprintf("udprecv: recvfrom failed (%s)\n", result_error_string(status));
        close((int)fd);
        return 1;
    }

    buffer[status < (long)(sizeof(buffer) - 1) ? status : (long)(sizeof(buffer) - 1)] = '\0';
    puts("from ");
    print_ipv4(remote_address.ipv4);
    printf(":%u ", (unsigned int)remote_address.port);
    puts(buffer);
    putchar(1, '\n');
    close((int)fd);
    return 0;
}
