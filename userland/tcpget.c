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

static int append_text(char* buffer, size_t capacity, size_t* length, const char* text) {
    size_t text_length = strlen(text);
    if (*length + text_length >= capacity) {
        return 0;
    }
    memcpy(buffer + *length, text, text_length);
    *length += text_length;
    buffer[*length] = '\0';
    return 1;
}

int main(int argc, char** argv) {
    long fd;
    long status;
    long net_fd;
    uint32_t ipv4 = 0;
    unsigned int port = 0;
    struct savanxp_sockaddr_in address;
    struct savanxp_net_info info;
    char request[512];
    char response[512];
    size_t request_length = 0;

    if (argc < 5 || !parse_ipv4(argv[1], &ipv4) || !parse_uint(argv[2], &port) || port == 0 || port > 65535u) {
        puts_err("usage: tcpget <ipv4> <port> <host> <path>\n");
        return 1;
    }

    fd = socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP);
    if (fd < 0) {
        eprintf("tcpget: socket failed (%s)\n", result_error_string(fd));
        return 1;
    }

    memset(&address, 0, sizeof(address));
    address.ipv4 = ipv4;
    address.port = (uint16_t)port;
    status = connect((int)fd, &address, 4000);
    if (status < 0) {
        eprintf("tcpget: connect failed (%s)\n", result_error_string(status));
        net_fd = open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
        if (net_fd >= 0) {
            memset(&info, 0, sizeof(info));
            if (ioctl((int)net_fd, NET_IOC_GET_INFO, (unsigned long)&info) >= 0) {
                eprintf("tcpget: net0 state=%s tx=%u rx=%u txerr=%u rxerr=%u\n",
                    net_status_string(info.last_status),
                    (unsigned int)info.tx_frames,
                    (unsigned int)info.rx_frames,
                    (unsigned int)info.tx_errors,
                    (unsigned int)info.rx_errors);
            }
            close((int)net_fd);
        }
        close((int)fd);
        return 1;
    }

    memset(request, 0, sizeof(request));
    if (!append_text(request, sizeof(request), &request_length, "GET ") ||
        !append_text(request, sizeof(request), &request_length, argv[4]) ||
        !append_text(request, sizeof(request), &request_length, " HTTP/1.0\r\nHost: ") ||
        !append_text(request, sizeof(request), &request_length, argv[3]) ||
        !append_text(request, sizeof(request), &request_length, "\r\nConnection: close\r\n\r\n")) {
        puts_err("tcpget: request too large\n");
        close((int)fd);
        return 1;
    }

    status = write((int)fd, request, request_length);
    if (status < 0) {
        eprintf("tcpget: write failed (%s)\n", result_error_string(status));
        close((int)fd);
        return 1;
    }

    for (;;) {
        status = read((int)fd, response, sizeof(response));
        if (status == 0) {
            break;
        }
        if (status < 0) {
            eprintf("tcpget: read failed (%s)\n", result_error_string(status));
            close((int)fd);
            return 1;
        }
        if (write(1, response, (size_t)status) < 0) {
            close((int)fd);
            return 1;
        }
    }

    close((int)fd);
    return 0;
}
