#pragma once

/* libsxfont's API lives with the platform's headers, since SUN, POSIX and
 * Win32's gdi32 (through winhost) share it: include/sxfont/font.h,
 * shipped in the SDK as sxfont/font.h. This name stays for the POSIX
 * programs that include it. */
#include "sxfont/font.h"
