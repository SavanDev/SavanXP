/* The host has no /dev/gpu0: every call fails, so SxGL's GPU backend always
 * declines and the host test exercises the software rasterizer. Stands in for
 * system/gfx/sxgl_gpu_device.c. */

#include "sxgl_gpu.h"

long sxgl_gpu_device_open(void)
{
    return -1;
}

void sxgl_gpu_device_close(long device)
{
    (void)device;
}

long sxgl_gpu_device_control(long device, unsigned long code, void* argument)
{
    (void)device;
    (void)code;
    (void)argument;
    return -1;
}

long sxgl_gpu_device_section(uint64_t bytes, void** view)
{
    (void)bytes;
    *view = 0;
    return -1;
}

void sxgl_gpu_device_section_free(long section, void* view)
{
    (void)section;
    (void)view;
}
