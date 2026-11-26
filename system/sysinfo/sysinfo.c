/* sysinfo: the system's identity and what the kernel reports about the
 * machine, printed on standard output. A SUN program: the report comes from
 * sxn_system_info and goes out through handle 1, which the shell that started
 * it gave it. It is also one of the rescue system's tools
 * (docs/kernel/FILESYSTEMS.md#the-rescue-system). */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "sxn/sxn.h"

#include "shared/version.h"
#include "build_number.h"

/* Set when standard output refused a write: the report did not reach whoever
 * asked for it, and the exit status says so. */
static int g_write_failed;

static void write_text(int handle, const char* text) {
    const long length = (long)strlen(text);
    if (sxn_write(handle, text, (unsigned long)length) != length && handle == 1) {
        g_write_failed = 1;
    }
}

static void puts_out(const char* text) {
    write_text(1, text);
}

static void print_u64(uint64_t value) {
    char buffer[24];
    (void)snprintf(buffer, sizeof(buffer), "%llu", (unsigned long long)value);
    puts_out(buffer);
}

static void print_label(const char* label) {
    puts_out("  ");
    puts_out(label);
    puts_out(": ");
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
        case SAVANXP_TIMER_PIT:
            return "pit";
        case SAVANXP_TIMER_NONE:
        default:
            return "none";
    }
}

static void print_text_line(const char* label, const char* value) {
    print_label(label);
    puts_out(text_or_unknown(value));
    puts_out("\n");
}

static void print_bool_line(const char* label, uint8_t value) {
    print_label(label);
    puts_out(yes_no(value));
    puts_out("\n");
}

static void print_u64_line(const char* label, uint64_t value) {
    print_label(label);
    print_u64(value);
    puts_out("\n");
}

static void print_mib_line(const char* label, uint64_t bytes) {
    print_label(label);
    print_u64(bytes / (1024ULL * 1024ULL));
    puts_out(" MiB\n");
}

int main(void) {
    struct savanxp_system_info info = {};
    const long status = sxn_system_info(&info);
    if (status < 0) {
        char message[96];
        (void)snprintf(message, sizeof(message), "sysinfo: system_info failed (%s)\n", strerror((int)-status));
        write_text(2, message);
        return 1;
    }

    puts_out("system:\n");
    print_text_line("name", SAVANXP_SYSTEM_NAME);
    print_text_line("version", SAVANXP_VERSION_STRING);
    print_text_line("build", SAVANXP_BUILD_STRING);
    print_text_line("entry", "/bin/sh");

    puts_out("boot:\n");
    print_text_line("bootloader", text_or_unknown(info.bootloader_name));
    print_text_line("bootloader_version", text_or_unknown(info.bootloader_version));
    print_text_line("firmware", text_or_unknown(info.firmware));
    print_u64_line("initramfs_bytes", info.initramfs_size);

    puts_out("memory:\n");
    print_mib_line("usable", info.memory_usable_bytes);
    print_mib_line("reclaimable", info.memory_reclaimable_bytes);
    print_u64_line("pages", info.memory_total_pages);

    puts_out("video/input:\n");
    print_bool_line("framebuffer", info.framebuffer_ready);
    print_bool_line("input", info.input_ready);
    print_label("framebuffer_mode");
    print_u64(info.framebuffer_width);
    puts_out("x");
    print_u64(info.framebuffer_height);
    puts_out("x");
    print_u64(info.framebuffer_bpp);
    puts_out("\n");

    puts_out("timer:\n");
    print_text_line("backend", timer_backend_string(info.timer_backend));
    print_u64_line("frequency_hz", info.timer_frequency_hz);

    puts_out("devices:\n");
    print_u64_line("pci_devices", info.pci_device_count);
    print_bool_line("net", info.net_present);
    print_bool_line("speaker", info.speaker_ready);
    print_bool_line("block", info.block_ready);

    puts_out("disk:\n");
    print_bool_line("mounted", info.sxfs_mounted);
    print_u64_line("sxfs_files", info.sxfs_file_count);
    print_mib_line("sxfs_total", info.sxfs_total_bytes);
    print_mib_line("sxfs_used", info.sxfs_used_bytes);
    print_mib_line("sxfs_free", info.sxfs_free_bytes);

    puts_out("uptime:\n");
    print_u64_line("milliseconds", info.uptime_ms);

    return g_write_failed ? 1 : 0;
}
