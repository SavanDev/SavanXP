/* SxGL's GPU backend, the part that makes system calls
 * (docs/roadmaps/open/VIRGL_ROADMAP.md).
 *
 * It uses the native object calls, which the kernel opens to every
 * subsystem's processes: the same object serves POSIX programs (gears) and
 * SUN programs (nightwood). The host test links tests/host/sxgl_gpu_device_stub.c
 * in its place.
 */

#include "sxgl_gpu.h"

#include "sxn/sxn.h"

long sxgl_gpu_device_open(void)
{
    return sxn_open_device("gpu0", SXN_DEVICE_READ | SXN_DEVICE_WRITE);
}

void sxgl_gpu_device_close(long device)
{
    if (device >= 0) {
        (void)sxn_close(device);
    }
}

long sxgl_gpu_device_control(long device, unsigned long code, void* argument)
{
    return sxn_device_control(device, code, (unsigned long)argument);
}

long sxgl_gpu_device_section(uint64_t bytes, void** view)
{
    long section = sxn_section_create(bytes, SXN_SECTION_READ | SXN_SECTION_WRITE);
    long base;

    *view = 0;
    if (section < 0) {
        return section;
    }
    base = sxn_map_view(section, SXN_SECTION_READ | SXN_SECTION_WRITE);
    if (base <= 0) {
        (void)sxn_close(section);
        return base < 0 ? base : -1;
    }
    *view = (void*)base;
    return section;
}

void sxgl_gpu_device_section_free(long section, void* view)
{
    if (view != 0) {
        (void)sxn_unmap_view((uint64_t)(uintptr_t)view);
    }
    if (section >= 0) {
        (void)sxn_close(section);
    }
}
