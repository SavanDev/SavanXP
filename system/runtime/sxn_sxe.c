#include <stddef.h>
#include <stdint.h>

#include "savanxp/sxe.h"
#include "sxn/sxn.h"

/*
 * The .sxe reader's file, in SUN's terms (docs/userland/SXE_FORMAT.md). The
 * reader itself is the runtime's (runtime/sxe.c); what it reads with is a
 * runtime hook, weak and POSIX there, strong and SXN here, so a SUN program
 * reads another program's name, icons and launch flags with sxe_file_open,
 * sxe_load_meta and sxe_load_icon_file as a POSIX one does. Linked into every
 * native program; the linker drops it from the ones that read no .sxe.
 */

long sx_sxe_open(const char* path)
{
    return sxn_open_file(path, SXN_FILE_READ);
}

long sx_sxe_read(long handle, void* buffer, size_t count)
{
    return sxn_read(handle, buffer, count);
}

long sx_sxe_seek(long handle, uint64_t offset)
{
    if (offset > (uint64_t)INT64_MAX)
    {
        return -SAVANXP_EINVAL;
    }
    return sxn_seek_file(handle, (int64_t)offset, SXN_SEEK_START);
}

void sx_sxe_close(long handle)
{
    (void)sxn_close(handle);
}
