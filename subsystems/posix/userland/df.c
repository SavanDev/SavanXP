#include "libc.h"

static void print_u64(uint64_t value) {
    char buffer[32];
    int index = 0;

    if (value == 0) {
        putchar_fd(1, '0');
        return;
    }

    while (value != 0 && index < (int)sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        putchar_fd(1, buffer[--index]);
    }
}

static void print_padded(const char* text, int width) {
    int length = 0;
    while (text[length] != '\0') {
        putchar_fd(1, text[length]);
        ++length;
    }
    while (length < width) {
        putchar_fd(1, ' ');
        ++length;
    }
}

static uint64_t mib_from_bytes(uint64_t bytes) {
    return bytes / (1024ULL * 1024ULL);
}

int main(void) {
    struct savanxp_system_info info = {};
    const long status = system_info(&info);
    if (status < 0) {
        eprintf("df: system_info failed (%s)\n", result_error_string(status));
        return 1;
    }

    if (!info.sxfs_mounted) {
        puts_out("df: /disk not mounted\n");
        return 1;
    }

    puts_out("Filesystem  Total(MiB)  Used(MiB)  Free(MiB)  Mount\n");
    print_padded("sxfs", 12);
    print_u64(mib_from_bytes(info.sxfs_total_bytes));
    puts_out("          ");
    print_u64(mib_from_bytes(info.sxfs_used_bytes));
    puts_out("         ");
    print_u64(mib_from_bytes(info.sxfs_free_bytes));
    puts_out("         /disk\n");
    return 0;
}
