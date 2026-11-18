#include <stdlib.h>

#include "libc.h"

/*
 * panictest: asks the kernel to panic (POWER_IOC_PANIC on /dev/power).
 *
 * panic-smoke runs it and waits for the line the panic prints after its
 * message, "smp: other cores stopped (n/n)": every other core took the NMI
 * and halted, instead of running on or spinning for the kernel lock the
 * panicking core still holds (docs/roadmaps/open/SMP_ROADMAP.md, phase 4).
 *
 * With `--display VARIANT` it first takes the graphics session and leaves the
 * screen in a known state, so the panic-display-* scenarios can check from a
 * screenshot that the panic text is what the screen shows (FB4 in
 * docs/roadmaps/open/FRAMEBUFFER_ROADMAP.md):
 *
 *   front     double buffered, the first buffer shown (two flips)
 *   back      double buffered, the second buffer shown (one flip)
 *   single    a mode too large for two buffers, so single buffered
 *   released  one flip, then the session released, as when the desktop exits
 *
 * The frame it shows is solid magenta: any of it left on screen after the
 * panic means the console drew where nobody looks.
 *
 * It needs the system identity, so the smoke runs it straight from init.
 * Returning at all is the failure.
 */

#define PANICTEST_FILL 0x00ff00ffu

static int fail(const char* what, long result) {
    printf("PANICTEST FAIL %s (%ld)\n", what, result);
    return 1;
}

/* A mode whose frame fits in the mapped VRAM once but not twice. */
static int pick_single_mode(const struct savanxp_gpu_connector_properties* properties,
                            uint32_t* width, uint32_t* height) {
    static const uint32_t widths[] = {2560u, 1920u, 1600u, 1280u, 1024u};
    unsigned index;

    for (index = 0; index < sizeof(widths) / sizeof(widths[0]); ++index) {
        const uint64_t pitch = (uint64_t)widths[index] * 4u;
        uint64_t rows;
        if (widths[index] > properties->max_width) {
            continue;
        }
        rows = properties->max_bytes / pitch;
        if (rows > properties->max_height) {
            rows = properties->max_height;
        }
        if (rows >= 480u && rows * pitch * 2u > properties->max_bytes) {
            *width = widths[index];
            *height = (uint32_t)rows;
            return 1;
        }
    }
    return 0;
}

static int prepare_display(const char* variant) {
    struct savanxp_gpu_info info;
    struct savanxp_gpu_connector_properties properties;
    struct savanxp_gpu_mode mode;
    uint32_t* pixels;
    uint32_t count;
    uint32_t index;
    int flips;
    long gpu_fd;
    long result;

    if (strcmp(variant, "front") == 0) {
        flips = 2;
    } else if (strcmp(variant, "back") == 0 || strcmp(variant, "released") == 0 ||
               strcmp(variant, "single") == 0) {
        flips = 1;
    } else {
        return fail("unknown display variant", 0);
    }

    gpu_fd = gpu_open();
    if (gpu_fd < 0) {
        return fail("open /dev/gpu", gpu_fd);
    }
    if ((result = gpu_acquire((int)gpu_fd)) < 0) {
        return fail("GPU_IOC_ACQUIRE", result);
    }
    if ((result = gpu_get_info((int)gpu_fd, &info)) < 0) {
        return fail("GPU_IOC_GET_INFO", result);
    }
    if ((result = gpu_get_connector_properties((int)gpu_fd, &properties)) < 0) {
        return fail("GPU_IOC_GET_CONNECTOR_PROPERTIES", result);
    }
    if ((properties.flags & SAVANXP_GPU_CONNECTOR_FLAG_MUTABLE_MODE_SETTING) == 0) {
        return fail("the adapter cannot set modes", 0);
    }

    memset(&mode, 0, sizeof(mode));
    mode.width = info.width;
    mode.height = info.height;
    mode.bpp = 32u;
    if (strcmp(variant, "single") == 0 && !pick_single_mode(&properties, &mode.width, &mode.height)) {
        return fail("no mode fits once but not twice", 0);
    }
    /* Asking for a mode programs it through DISPI with a second buffer when
     * one fits; the kernel logs "doble buffer si/no". */
    if ((result = gpu_set_mode((int)gpu_fd, &mode)) < 0) {
        return fail("GPU_IOC_SET_MODE", result);
    }

    count = mode.buffer_size / 4u;
    pixels = (uint32_t*)malloc(mode.buffer_size);
    if (pixels == 0) {
        return fail("malloc frame", (long)mode.buffer_size);
    }
    for (index = 0; index < count; ++index) {
        pixels[index] = PANICTEST_FILL;
    }
    while (flips-- > 0) {
        if ((result = gpu_present((int)gpu_fd, pixels)) < 0) {
            return fail("GPU_IOC_PRESENT", result);
        }
    }
    if (strcmp(variant, "released") == 0 && (result = gpu_release((int)gpu_fd)) < 0) {
        return fail("GPU_IOC_RELEASE", result);
    }
    printf("PANICTEST display=%s mode=%ux%u ready\n", variant,
           (unsigned)mode.width, (unsigned)mode.height);
    return 0;
}

int main(int argc, char** argv) {
    long fd = savanxp_open_mode("/dev/power", SAVANXP_OPEN_WRITE);
    long result;

    if (fd < 0) {
        printf("PANICTEST FAIL open /dev/power (%ld)\n", fd);
        return 1;
    }
    if (argc >= 3 && strcmp(argv[1], "--display") == 0 && prepare_display(argv[2]) != 0) {
        return 1;
    }
    printf("PANICTEST requesting a kernel panic\n");
    result = savanxp_ioctl((int)fd, POWER_IOC_PANIC, 0);
    printf("PANICTEST FAIL the kernel did not panic (%ld)\n", result);
    savanxp_close((int)fd);
    return 1;
}
