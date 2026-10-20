/*
 * windowd on the native API: the wd_* functions windowd_sys.h renames the
 * POSIX calls of windowd's sources to. This unit does not include
 * windowd_sys.h; it speaks SXN only.
 */

#include <stdarg.h>
#include <stdio.h>

#include "libc.h"
#include "sxn/sxn.h"

int windowd_main(int argc, char **argv);

/* --- text output: /dev/serial, opened once ------------------------------- */

static long g_serial = -2;

static void wd_output(const char *text, size_t length)
{
    if (g_serial == -2)
    {
        g_serial = sxn_open_device("serial", SXN_DEVICE_WRITE);
    }
    if (g_serial >= 0 && length != 0u)
    {
        (void)sxn_write(g_serial, text, length);
    }
}

static void wd_voutput(const char *format, va_list args)
{
    char buffer[512];
    int length = vsnprintf(buffer, sizeof(buffer), format, args);

    if (length < 0)
    {
        return;
    }
    if ((size_t)length >= sizeof(buffer))
    {
        length = (int)sizeof(buffer) - 1;
    }
    wd_output(buffer, (size_t)length);
}

int wd_printf(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    wd_voutput(format, args);
    va_end(args);
    return 0;
}

void wd_eprintf(const char *format, ...)
{
    va_list args;

    va_start(args, format);
    wd_voutput(format, args);
    va_end(args);
}

void wd_puts_fd(int fd, const char *text)
{
    (void)fd;
    if (text != 0)
    {
        wd_output(text, strlen(text));
    }
}

/* --- time and identity ---------------------------------------------------- */

unsigned long wd_uptime_ms(void)
{
    return (unsigned long)(sxn_clock_ns() / 1000000ull);
}

unsigned long long wd_monotonic_ns(void)
{
    return (unsigned long long)sxn_clock_ns();
}

/* A sleep is a wait on an event nobody sets. */
long wd_sleep_ms(unsigned long milliseconds)
{
    static long sleeper = -1;

    if (sleeper < 0)
    {
        sleeper = sxn_event_create(SXN_EVENT_MANUAL_RESET);
        if (sleeper < 0)
        {
            return sleeper;
        }
    }
    (void)sxn_wait_one(sleeper, (long)milliseconds);
    return 0;
}

long wd_getpid(void)
{
    static long pid = 0;

    if (pid == 0)
    {
        struct sxn_native_info info;

        if (sxn_info(&info) == 0)
        {
            pid = (long)info.pid;
        }
    }
    return pid;
}

/* --- objects -------------------------------------------------------------- */

long wd_event_create(unsigned long flags)
{
    return sxn_event_create((unsigned)flags);
}

long wd_event_set(int handle)
{
    return sxn_event_set(handle);
}

long wd_event_reset(int handle)
{
    return sxn_event_reset(handle);
}

long wd_wait_one(int handle, long timeout_ms)
{
    return sxn_wait_one(handle, timeout_ms);
}

long wd_pipe_send_handle(int pipe_fd, int handle)
{
    return sxn_pipe_send_handle(pipe_fd, handle);
}

long wd_section_create(unsigned long size, unsigned long flags)
{
    return sxn_section_create((uint64_t)size, (unsigned)flags);
}

void *wd_map_view(int handle, unsigned long flags)
{
    return (void *)sxn_map_view(handle, (unsigned)flags);
}

long wd_unmap_view(void *base)
{
    return sxn_unmap_view((uint64_t)(uintptr_t)base);
}

long wd_read(int fd, void *buffer, size_t count)
{
    return sxn_read(fd, buffer, count);
}

long wd_write(int fd, const void *buffer, size_t count)
{
    return sxn_write(fd, buffer, count);
}

long wd_close(int fd)
{
    return sxn_close(fd);
}

long wd_dup(int fd)
{
    return sxn_duplicate(fd);
}

/* Only O_NONBLOCK through F_SETFL, the one use windowd has. */
long wd_fcntl(int fd, unsigned long command, unsigned long value)
{
    if (command != SAVANXP_F_SETFL)
    {
        return -SAVANXP_EINVAL;
    }
    return sxn_set_nonblocking(fd, (value & SAVANXP_OPEN_NONBLOCK) != 0u);
}

/* "/dev/input0" -> the device "input0". Files are not windowd's business. */
long wd_open_mode(const char *path, unsigned long flags)
{
    static const char prefix[] = "/dev/";
    unsigned access = 0;
    size_t index;

    for (index = 0; index + 1u < sizeof(prefix); ++index)
    {
        if (path == 0 || path[index] != prefix[index])
        {
            return -SAVANXP_ENOENT;
        }
    }
    if ((flags & SAVANXP_OPEN_READ) != 0u)
    {
        access |= SXN_DEVICE_READ;
    }
    if ((flags & SAVANXP_OPEN_WRITE) != 0u)
    {
        access |= SXN_DEVICE_WRITE;
    }
    return sxn_open_device(path + sizeof(prefix) - 1u, access);
}

/*
 * poll as wait-many: sleep until any handle is ready (or the timeout), then
 * look at each with a zero-timeout wait. Every handle windowd polls is a
 * stream or a manual-reset event or a port, none of which such a look
 * consumes. A stream at its end reads as ready; the read that follows finds
 * the end, as a POLLHUP would have told. Writability is not waited for: a
 * POLLOUT is reported ready, and the write itself blocks or says EAGAIN.
 */
long wd_poll(struct savanxp_pollfd *fds, unsigned long count, long timeout_ms)
{
    int32_t handles[16];
    unsigned long index;
    unsigned long waited = 0;
    long result;
    long ready = 0;

    if (fds == 0 || count == 0 || count > 16u)
    {
        return -SAVANXP_EINVAL;
    }
    for (index = 0; index < count; ++index)
    {
        fds[index].revents = 0;
        if ((fds[index].events & SAVANXP_POLLOUT) != 0)
        {
            fds[index].revents = (short)SAVANXP_POLLOUT;
            ready += 1;
        }
        if ((fds[index].events & SAVANXP_POLLIN) != 0 && fds[index].fd >= 0)
        {
            handles[waited++] = fds[index].fd;
        }
    }
    if (ready != 0 || waited == 0)
    {
        return ready;
    }
    result = sxn_wait_many(handles, waited, timeout_ms);
    if (result == -SAVANXP_ETIMEDOUT)
    {
        return 0;
    }
    if (result < 0)
    {
        return result;
    }
    for (index = 0; index < count; ++index)
    {
        if ((fds[index].events & SAVANXP_POLLIN) != 0 && fds[index].fd >= 0 &&
            sxn_wait_one(fds[index].fd, 0) == 0)
        {
            fds[index].revents = (short)SAVANXP_POLLIN;
            ready += 1;
        }
    }
    return ready;
}

/* --- processes ------------------------------------------------------------ */

long wd_spawn(const char *path, const char *const *argv, int argc)
{
    return sxn_spawn_stdio(path, (char *const *)argv, argc);
}

long wd_waitpid(int pid, int *status)
{
    return sxn_waitpid(pid, status);
}

/* windowd ends only its own children; the status reads like a POSIX kill's. */
long wd_kill(int pid, int signal_number)
{
    return sxn_terminate(pid, 128 + signal_number);
}

/* Only the framebuffer geometry, which is all windowd asks for. */
long wd_system_info(struct savanxp_system_info *info)
{
    struct sxn_gfx_info gfx;

    if (info == 0)
    {
        return -SAVANXP_EINVAL;
    }
    memset(info, 0, sizeof(*info));
    if (sxn_gfx_info(&gfx) != 0)
    {
        return -SAVANXP_ENODEV;
    }
    info->framebuffer_ready = 1;
    info->framebuffer_width = gfx.width;
    info->framebuffer_height = gfx.height;
    info->framebuffer_bpp = gfx.bpp;
    return 0;
}

/* crt0 would end the process through POSIX EXIT, which a native process does
 * not have. */
int main(int argc, char **argv)
{
    sxn_exit(windowd_main(argc, argv));
}
