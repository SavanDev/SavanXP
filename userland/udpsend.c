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

static int parse_ipv4(const char* text, uint32_t* address) {
    unsigned int parts[4];
    unsigned int part = 0;
    unsigned int count = 0;
    const char* cursor = text;

    while (*cursor != '\0' && count < 4) {
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != '.') {
            ++cursor;
        }

        {
            char chunk[4];
            size_t length = (size_t)(cursor - start);
            if (length == 0 || length >= sizeof(chunk)) {
                return 0;
            }
            memcpy(chunk, start, length);
            chunk[length] = '\0';
            if (!parse_uint(chunk, &part) || part > 255u) {
                return 0;
            }
            parts[count++] = part;
        }

        if (*cursor == '.') {
            ++cursor;
        }
    }

    if (*cursor != '\0' || count != 4) {
        return 0;
    }

    *address = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

int main(int argc, char** argv) {
    long fd;
    long status;
    struct savanxp_sockaddr_in address;
    uint32_t ipv4 = 0;
    unsigned int port = 0;
    char message[512];
    size_t message_length = 0;
    int index;

    if (argc < 4 || !parse_ipv4(argv[1], &ipv4) || !parse_uint(argv[2], &port) || port == 0 || port > 65535u) {
        puts_err("usage: udpsend <ipv4> <port> <message>\n");
        return 1;
    }

    memset(message, 0, sizeof(message));
    for (index = 3; index < argc; ++index) {
        const size_t part_length = strlen(argv[index]);
        if (message_length != 0) {
            if (message_length + 1 >= sizeof(message)) {
                break;
            }
            message[message_length++] = ' ';
        }
        if (message_length + part_length >= sizeof(message)) {
            break;
        }
        memcpy(message + message_length, argv[index], part_length);
        message_length += part_length;
    }

    fd = socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP);
    if (fd < 0) {
        eprintf("udpsend: socket failed (%s)\n", result_error_string(fd));
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.ipv4 = ipv4;
    address.port = (uint16_t)port;
    status = sendto((int)fd, message, message_length, &address);
    if (status < 0) {
        eprintf("udpsend: sendto failed (%s)\n", result_error_string(status));
        close((int)fd);
        return 1;
    }

    printf("sent %u bytes\n", (unsigned int)message_length);
    close((int)fd);
    return 0;
}
