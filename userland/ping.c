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

static void print_ipv4(uint32_t address) {
    printf(
        "%u.%u.%u.%u",
        (unsigned int)((address >> 24) & 0xffu),
        (unsigned int)((address >> 16) & 0xffu),
        (unsigned int)((address >> 8) & 0xffu),
        (unsigned int)(address & 0xffu)
    );
}

static void eprint_ipv4(uint32_t address) {
    eprintf(
        "%u.%u.%u.%u",
        (unsigned int)((address >> 24) & 0xffu),
        (unsigned int)((address >> 16) & 0xffu),
        (unsigned int)((address >> 8) & 0xffu),
        (unsigned int)(address & 0xffu)
    );
}

static void print_net_hint(uint32_t target, const struct savanxp_net_info* info) {
    if (info == 0) {
        return;
    }

    eprintf("ping: net0 state=%s\n", net_status_string(info->last_status));
    if (target != info->gateway) {
        eprintf("ping: note: with QEMU user-net, the reliable smoke test is ping ");
        eprint_ipv4(info->gateway);
        eprintf("\n");
    }
}

int main(int argc, char** argv) {
    long fd;
    uint32_t target;
    struct savanxp_net_ping_request request;
    struct savanxp_net_ping_result result;
    struct savanxp_net_info info;
    long status;

    if (argc < 2 || !parse_ipv4(argv[1], &target)) {
        puts_fd(2, "usage: ping <ipv4>\n");
        return 1;
    }

    fd = open_mode("/dev/net0", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        eprintf("ping: /dev/net0 unavailable (%s)\n", result_error_string(fd));
        return 1;
    }

    status = ioctl((int)fd, NET_IOC_UP, 0);
    if (status < 0) {
        eprintf("ping: NET_IOC_UP failed (%s)\n", result_error_string(status));
        close((int)fd);
        return 1;
    }

    memset(&request, 0, sizeof(request));
    memset(&result, 0, sizeof(result));
    memset(&info, 0, sizeof(info));
    request.ipv4 = target;
    request.timeout_ms = 1500;
    request.sequence = 1;
    request.payload_size = 32;
    request.result_ptr = (uint64_t)(unsigned long)&result;

    status = ioctl((int)fd, NET_IOC_PING, (unsigned long)&request);
    if (status < 0) {
        eprintf("ping: NET_IOC_PING failed (%s)\n", result_error_string(status));
        if (ioctl((int)fd, NET_IOC_GET_INFO, (unsigned long)&info) >= 0) {
            print_net_hint(target, &info);
        }
        close((int)fd);
        return 1;
    }

    puts("reply from ");
    print_ipv4(result.reply_ipv4);
    printf(": time=%u ms ttl=%u\n", (unsigned int)result.elapsed_ms, (unsigned int)result.ttl);
    close((int)fd);
    return 0;
}
