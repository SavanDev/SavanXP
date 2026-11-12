#include "sxgfx/mime_icon.h"

#include "libc.h"

/* The POSIX side of mime_icon (sxgfx/mime_icon.h): its file reader, and the
 * mapping from the user's profile. */

static long read_with_posix(const char *path, void *buffer, size_t capacity)
{
    long total = 0;
    int fd = (int)savanxp_open_mode(path, SAVANXP_OPEN_READ);

    if (fd < 0)
    {
        return -1;
    }
    while ((size_t)total < capacity)
    {
        long got = savanxp_read(fd, (char *)buffer + total, capacity - (size_t)total);

        if (got <= 0)
        {
            break;
        }
        total += got;
    }
    (void)savanxp_close(fd);
    return total;
}

int mime_icon_load(void)
{
    char path[256];

    mime_icon_set_reader(read_with_posix);
    if (savanxp_settings_path(MIME_ICON_POLICY_NAME, 0, path, sizeof(path)) != 0)
    {
        return mime_icon_load_file(0);
    }
    return mime_icon_load_file(path);
}
