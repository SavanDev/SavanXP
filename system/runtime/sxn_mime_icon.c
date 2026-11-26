#include "sxgfx/mime_icon.h"

#include "sxn/sxn.h"
#include "sxn/sxnprofile.h"

/* The SUN side of mime_icon (sxgfx/mime_icon.h), as mime_icon_posix.c is
 * POSIX's: its file reader, and the mapping from the person's profile, or the
 * system's when the profile has none. */

static long read_with_sun(const char *path, void *buffer, size_t capacity)
{
    long handle = sxn_open_file(path, SXN_FILE_READ);
    long total = 0;

    if (handle < 0)
    {
        return -1;
    }
    while ((size_t)total < capacity)
    {
        long got = sxn_read(handle, (char *)buffer + total, capacity - (size_t)total);

        if (got <= 0)
        {
            break;
        }
        total += got;
    }
    (void)sxn_close(handle);
    return total;
}

int mime_icon_load(void)
{
    char path[256];
    struct sxn_file_info info;

    mime_icon_set_reader(read_with_sun);
    if (sxn_settings_path(MIME_ICON_POLICY_NAME, 0, path, sizeof(path)) == 0 && sxn_query_path(path, &info) == 0)
    {
        return mime_icon_load_file(path);
    }
    return mime_icon_load_file(MIME_ICON_SYSTEM_POLICY);
}
