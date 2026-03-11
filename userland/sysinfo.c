#include "libc.h"

#include "shared/version.h"

static void print_u64(uint64_t value) {
    char buffer[32];
    int index = 0;

    if (value == 0) {
        putchar(1, '0');
        return;
    }

    while (value != 0 && index < (int)sizeof(buffer)) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        putchar(1, buffer[--index]);
    }
}

static void print_label(const char* label) {
    puts("  ");
    puts(label);
    puts(": ");
}

static const char* text_or_unknown(const char* text) {
    return (text != 0 && text[0] != '\0') ? text : "unknown";
}

static const char* yes_no(uint8_t value) {
    return value != 0 ? "yes" : "no";
}

static const char* timer_backend_string(uint32_t backend) {
    switch (backend) {
        case SAVANXP_TIMER_LOCAL_APIC:
            return "local apic";
        case SAVANXP_TIMER_NONE:
        default:
            return "none";
    }
}

static void print_text_line(const char* label, const char* value) {
    print_label(label);
    puts(text_or_unknown(value));
    putchar(1, '\n');
}

static void print_bool_line(const char* label, uint8_t value) {
    print_label(label);
    puts(yes_no(value));
    putchar(1, '\n');
}

static void print_u64_line(const char* label, uint64_t value) {
    print_label(label);
    print_u64(value);
    putchar(1, '\n');
}

static void print_mib_line(const char* label, uint64_t bytes) {
    print_label(label);
    print_u64(bytes / (1024ULL * 1024ULL));
    puts(" MiB\n");
}

int main(void) {
    struct savanxp_system_info info = {};
    const long status = system_info(&info);
    if (status < 0) {
        eprintf("sysinfo: system_info failed (%s)\n", result_error_string(status));
        return 1;
    }

    puts("system:\n");
    print_text_line("name", SAVANXP_SYSTEM_NAME);
    print_text_line("version", SAVANXP_VERSION_STRING);
    print_text_line("entry", "/bin/sh");

    puts("boot:\n");
    print_text_line("bootloader", text_or_unknown(info.bootloader_name));
    print_text_line("bootloader_version", text_or_unknown(info.bootloader_version));
    print_text_line("firmware", text_or_unknown(info.firmware));
    print_u64_line("initramfs_bytes", info.initramfs_size);

    puts("memory:\n");
    print_mib_line("usable", info.memory_usable_bytes);
    print_mib_line("reclaimable", info.memory_reclaimable_bytes);
    print_u64_line("pages", info.memory_total_pages);

    puts("video/input:\n");
    print_bool_line("framebuffer", info.framebuffer_ready);
    print_bool_line("input", info.input_ready);
    print_label("framebuffer_mode");
    print_u64(info.framebuffer_width);
    putchar(1, 'x');
    print_u64(info.framebuffer_height);
    putchar(1, 'x');
    print_u64(info.framebuffer_bpp);
    putchar(1, '\n');

    puts("timer:\n");
    print_text_line("backend", timer_backend_string(info.timer_backend));
    print_u64_line("frequency_hz", info.timer_frequency_hz);

    puts("devices:\n");
    print_u64_line("pci_devices", info.pci_device_count);
    print_bool_line("net", info.net_present);
    print_bool_line("speaker", info.speaker_ready);
    print_bool_line("block", info.block_ready);

    puts("disk:\n");
    print_bool_line("mounted", info.svfs_mounted);
    print_u64_line("svfs_files", info.svfs_file_count);

    puts("uptime:\n");
    print_u64_line("milliseconds", info.uptime_ms);

    return 0;
}
