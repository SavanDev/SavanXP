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

int main(int argc, char** argv) {
    unsigned int frequency = 0;
    unsigned int duration = 0;
    struct savanxp_pcspk_beep beep;
    long fd;

    if (argc < 3 || !parse_uint(argv[1], &frequency) || !parse_uint(argv[2], &duration)) {
        puts_fd(2, "usage: beep <freq> <ms>\n");
        return 1;
    }

    fd = open_mode("/dev/pcspk", SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE);
    if (fd < 0) {
        eprintf("beep: /dev/pcspk unavailable (%s)\n", result_error_string(fd));
        return 1;
    }

    beep.frequency_hz = frequency;
    beep.duration_ms = duration;
    {
        long status = ioctl((int)fd, PCSPK_IOC_BEEP, (unsigned long)&beep);
        if (status < 0) {
            eprintf("beep: PCSPK_IOC_BEEP failed (%s)\n", result_error_string(status));
            close((int)fd);
            return 1;
        }
    }

    close((int)fd);
    return 0;
}
