#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A Windows program's own icon, read from its PE32 or PE32+ file
 * (docs/userland/WIN32.md, "Launching"): the first RT_GROUP_ICON of its
 * resources, as Windows' shell shows a .exe. The shell uses it for a
 * shortcut to a .exe, which has no .sxicon.
 *
 * From the group it takes the image of exactly `extent` pixels with the most
 * colours, or else the nearest larger one (the nearest smaller when there is
 * none), and samples it by nearest pixel into extent x extent pixels
 * 0xAARRGGBB, top row first (the .sxicon's BGRA8888). A 32-bit image with any
 * alpha keeps it; any other takes its transparency from the AND mask. A PNG
 * image (Windows Vista's 256x256) is skipped.
 *
 * It reads a file already in memory and makes no system call, so the
 * desktop, Files and the host tests share it. Every offset is checked
 * against `size`: a damaged file gives no icon, never a read outside it.
 */

#define PE_ICON_MAX_EXTENT 48u

/* 1 with `out` (extent * extent words) filled, 0 when the file has no
 * usable icon. extent is 1 to PE_ICON_MAX_EXTENT. */
int pe_icon_from_file(const uint8_t* file, size_t size, uint32_t extent, uint32_t* out);

#ifdef __cplusplus
}
#endif
