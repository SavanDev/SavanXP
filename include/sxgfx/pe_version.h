#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A string of a Windows program's version resource (VS_VERSIONINFO), read
 * from its PE32 or PE32+ file: "FileDescription" is what Windows XP's shell
 * names a program by when it is pinned to the Start menu ("Juego Solitario"
 * for the Spanish sol.exe), docs/desktop/START_MENU.md, "Shortcuts".
 *
 * The value comes from the first string table of the first RT_VERSION
 * resource, converted from UTF-16 to UTF-8. It reads a file already in
 * memory and makes no system call; every offset is checked against `size`,
 * so a damaged file gives no string, never a read outside it.
 */

/* 1 with `out` holding the value (truncated on a character boundary to fit
 * `capacity`, always terminated), 0 when there is none or it is empty. `key`
 * is ASCII and matched exactly. */
int pe_version_string(const uint8_t* file, size_t size, const char* key, char* out, size_t capacity);

#ifdef __cplusplus
}
#endif
