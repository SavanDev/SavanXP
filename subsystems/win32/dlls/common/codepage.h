/*
 * codepage.h -- The "ANSI" code page of the subsystem's DLLs: Windows-1252,
 * the one Windows XP uses in Western European languages (the user's edition
 * is Spanish). Compiled into each DLL that converts; not exported.
 */
#ifndef SXW_DLLS_CODEPAGE_H
#define SXW_DLLS_CODEPAGE_H

/* One 1252 byte as UTF-16. Undefined bytes map to themselves, as Windows'
 * MultiByteToWideChar maps them. */
unsigned short sxw_ansi_to_wide(unsigned char c);

/* One UTF-16 unit as a 1252 byte, or '?' when 1252 has no such character. */
unsigned char sxw_wide_to_ansi(unsigned short c);

#endif
