/*
 * strings.c -- kernel32's string functions: the lstr family and the code
 * page conversions.
 *
 * The "ANSI" and OEM code pages are both Windows-1252 here (codepage.h);
 * Windows XP in Spanish uses 850 for OEM, which only console programs see.
 * UTF-8 is converted exactly. lstrcmp and lstrcmpi compare by code unit,
 * folding case for ASCII and Latin-1, rather than by the user locale's
 * collation as Windows does.
 */
#define SXW_BUILDING_KERNEL32
#include <windows.h>

#include "codepage.h"

typedef unsigned int u32;

/* ---- lstr ------------------------------------------------------------------------ */

LPWSTR WINAPI lstrcpyW(LPWSTR lpString1, LPCWSTR lpString2) {
    u32 i = 0;
    do {
        lpString1[i] = lpString2[i];
    } while (lpString2[i++] != 0);
    return lpString1;
}

/* Copies at most iMaxLength - 1 units and always terminates. */
LPWSTR WINAPI lstrcpynW(LPWSTR lpString1, LPCWSTR lpString2, int iMaxLength) {
    if (iMaxLength <= 0) {
        return lpString1;
    }
    int i = 0;
    for (; i + 1 < iMaxLength && lpString2[i] != 0; ++i) {
        lpString1[i] = lpString2[i];
    }
    lpString1[i] = 0;
    return lpString1;
}

LPWSTR WINAPI lstrcatW(LPWSTR lpString1, LPCWSTR lpString2) {
    u32 n = 0;
    while (lpString1[n] != 0) {
        ++n;
    }
    lstrcpyW(lpString1 + n, lpString2);
    return lpString1;
}

LPSTR WINAPI lstrcpyA(LPSTR lpString1, LPCSTR lpString2) {
    u32 i = 0;
    do {
        lpString1[i] = lpString2[i];
    } while (lpString2[i++] != 0);
    return lpString1;
}

LPSTR WINAPI lstrcatA(LPSTR lpString1, LPCSTR lpString2) {
    u32 n = 0;
    while (lpString1[n] != 0) {
        ++n;
    }
    lstrcpyA(lpString1 + n, lpString2);
    return lpString1;
}

int WINAPI lstrlenA(LPCSTR lpString) {
    int n = 0;
    if (lpString == NULL) {
        return 0;
    }
    while (lpString[n] != 0) {
        ++n;
    }
    return n;
}

/* Upper case for ASCII and Latin-1 (U+00E0..U+00FE except U+00F7). */
static u32 fold(u32 c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0u && c <= 0xfeu && c != 0xf7u)) {
        return c - 32u;
    }
    return c;
}

static int compare_wide(LPCWSTR a, LPCWSTR b, int ignore_case) {
    a = a != NULL ? a : (LPCWSTR) L"";
    b = b != NULL ? b : (LPCWSTR) L"";
    for (u32 i = 0;; ++i) {
        u32 x = ignore_case ? fold(a[i]) : a[i];
        u32 y = ignore_case ? fold(b[i]) : b[i];
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (x == 0) {
            return 0;
        }
    }
}

static int compare_ansi(LPCSTR a, LPCSTR b, int ignore_case) {
    a = a != NULL ? a : "";
    b = b != NULL ? b : "";
    for (u32 i = 0;; ++i) {
        u32 x = sxw_ansi_to_wide((unsigned char)a[i]);
        u32 y = sxw_ansi_to_wide((unsigned char)b[i]);
        if (ignore_case) {
            x = fold(x);
            y = fold(y);
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (x == 0) {
            return 0;
        }
    }
}

int WINAPI lstrcmpW(LPCWSTR lpString1, LPCWSTR lpString2) {
    return compare_wide(lpString1, lpString2, 0);
}

int WINAPI lstrcmpiW(LPCWSTR lpString1, LPCWSTR lpString2) {
    return compare_wide(lpString1, lpString2, 1);
}

int WINAPI lstrcmpA(LPCSTR lpString1, LPCSTR lpString2) {
    return compare_ansi(lpString1, lpString2, 0);
}

int WINAPI lstrcmpiA(LPCSTR lpString1, LPCSTR lpString2) {
    return compare_ansi(lpString1, lpString2, 1);
}

/* ---- Code pages ------------------------------------------------------------------ */

/* The OEM code page of an English Windows XP, 437: what CF_OEMTEXT and a
 * console hold. Bytes 0x80-0xff as UTF-16; below them it is ASCII. */
static const WCHAR k_cp437[128] = {
    0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7,
    0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5,
    0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9,
    0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192,
    0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba,
    0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556,
    0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
    0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f,
    0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b,
    0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
    0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4,
    0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229,
    0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248,
    0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0,
};

static int is_oem(UINT code_page) {
    return code_page == CP_OEMCP || code_page == 437u;
}

static WCHAR oem_to_wide(unsigned char c) {
    return c < 0x80u ? (WCHAR)c : k_cp437[c - 0x80u];
}

/* The OEM byte for a character, or '?' when code page 437 lacks it. */
static unsigned char wide_to_oem(WCHAR c) {
    if (c < 0x80u) {
        return (unsigned char)c;
    }
    for (unsigned i = 0; i < 128u; ++i) {
        if (k_cp437[i] == c) {
            return (unsigned char)(0x80u + i);
        }
    }
    return (unsigned char)'?';
}

static int known_code_page(UINT code_page) {
    return code_page == CP_ACP || is_oem(code_page) || code_page == CP_THREAD_ACP || code_page == 1252u ||
           code_page == CP_UTF8;
}

/* One UTF-8 sequence at s[0..length): the code point, or -1 when malformed;
 * *used is the bytes it took. */
static long decode_utf8(const unsigned char *s, int length, int *used) {
    u32 c = s[0];
    int extra = c < 0x80u ? 0 : c >= 0xc2u && c < 0xe0u ? 1 : c >= 0xe0u && c < 0xf0u ? 2 : c >= 0xf0u && c < 0xf5u ? 3 : -1;
    if (extra < 0 || extra >= length) {
        *used = 1; /* a byte that cannot start a sequence, or one cut short */
        return -1;
    }
    if (extra == 0) {
        *used = 1;
        return (long)c;
    }
    c &= 0x3fu >> extra;
    for (int i = 1; i <= extra; ++i) {
        if ((s[i] & 0xc0u) != 0x80u) {
            *used = i;
            return -1;
        }
        c = (c << 6) | (s[i] & 0x3fu);
    }
    *used = extra + 1;
    if ((extra == 2 && (c < 0x800u || (c >= 0xd800u && c < 0xe000u))) || (extra == 3 && (c < 0x10000u || c > 0x10ffffu))) {
        return -1;
    }
    return (long)c;
}

int WINAPI MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte,
                               LPWSTR lpWideCharStr, int cchWideChar) {
    if (!known_code_page(CodePage) || lpMultiByteStr == NULL || cbMultiByte == 0 || cchWideChar < 0 ||
        (cchWideChar != 0 && lpWideCharStr == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const unsigned char *in = (const unsigned char *)lpMultiByteStr;
    int length = cbMultiByte;
    if (length < 0) {
        length = lstrlenA(lpMultiByteStr) + 1;
    }
    int out = 0;
    for (int i = 0; i < length;) {
        WCHAR units[2];
        int count = 1;
        if (CodePage == CP_UTF8) {
            int used = 1;
            long c = decode_utf8(in + i, length - i, &used);
            i += used;
            if (c < 0) {
                if ((dwFlags & MB_ERR_INVALID_CHARS) != 0u) {
                    SetLastError(ERROR_NO_UNICODE_TRANSLATION);
                    return 0;
                }
                c = 0xfffd;
            }
            if (c >= 0x10000) {
                c -= 0x10000;
                units[0] = (WCHAR)(0xd800 + (c >> 10));
                units[1] = (WCHAR)(0xdc00 + (c & 0x3ff));
                count = 2;
            } else {
                units[0] = (WCHAR)c;
            }
        } else if (is_oem(CodePage)) {
            units[0] = oem_to_wide(in[i++]);
        } else {
            units[0] = sxw_ansi_to_wide(in[i++]);
        }
        for (int k = 0; k < count; ++k) {
            if (cchWideChar != 0) {
                if (out >= cchWideChar) {
                    SetLastError(ERROR_INSUFFICIENT_BUFFER);
                    return 0;
                }
                lpWideCharStr[out] = units[k];
            }
            ++out;
        }
    }
    return out;
}

int WINAPI WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar,
                               LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar,
                               BOOL *lpUsedDefaultChar) {
    (void)dwFlags;
    if (!known_code_page(CodePage) || lpWideCharStr == NULL || cchWideChar == 0 || cbMultiByte < 0 ||
        (cbMultiByte != 0 && lpMultiByteStr == NULL)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    int length = cchWideChar;
    if (length < 0) {
        length = 0;
        while (lpWideCharStr[length] != 0) {
            ++length;
        }
        ++length;
    }
    if (lpUsedDefaultChar != NULL) {
        *lpUsedDefaultChar = FALSE;
    }
    int out = 0;
    for (int i = 0; i < length;) {
        unsigned char bytes[4];
        int count = 0;
        u32 c = lpWideCharStr[i++];
        if (CodePage == CP_UTF8) {
            if (c >= 0xd800u && c < 0xdc00u && i < length && lpWideCharStr[i] >= 0xdc00u && lpWideCharStr[i] < 0xe000u) {
                c = 0x10000u + ((c - 0xd800u) << 10) + (lpWideCharStr[i++] - 0xdc00u);
            } else if (c >= 0xd800u && c < 0xe000u) {
                c = 0xfffdu;
            }
            if (c < 0x80u) {
                bytes[count++] = (unsigned char)c;
            } else if (c < 0x800u) {
                bytes[count++] = (unsigned char)(0xc0u | (c >> 6));
                bytes[count++] = (unsigned char)(0x80u | (c & 0x3fu));
            } else if (c < 0x10000u) {
                bytes[count++] = (unsigned char)(0xe0u | (c >> 12));
                bytes[count++] = (unsigned char)(0x80u | ((c >> 6) & 0x3fu));
                bytes[count++] = (unsigned char)(0x80u | (c & 0x3fu));
            } else {
                bytes[count++] = (unsigned char)(0xf0u | (c >> 18));
                bytes[count++] = (unsigned char)(0x80u | ((c >> 12) & 0x3fu));
                bytes[count++] = (unsigned char)(0x80u | ((c >> 6) & 0x3fu));
                bytes[count++] = (unsigned char)(0x80u | (c & 0x3fu));
            }
        } else {
            unsigned char b = is_oem(CodePage) ? wide_to_oem((WCHAR)c) : sxw_wide_to_ansi((unsigned short)c);
            if (b == '?' && c != '?') {
                b = lpDefaultChar != NULL ? (unsigned char)lpDefaultChar[0] : (unsigned char)'?';
                if (lpUsedDefaultChar != NULL) {
                    *lpUsedDefaultChar = TRUE;
                }
            }
            bytes[count++] = b;
        }
        for (int k = 0; k < count; ++k) {
            if (cbMultiByte != 0) {
                if (out >= cbMultiByte) {
                    SetLastError(ERROR_INSUFFICIENT_BUFFER);
                    return 0;
                }
                lpMultiByteStr[out] = (CHAR)bytes[k];
            }
            ++out;
        }
    }
    return out;
}
