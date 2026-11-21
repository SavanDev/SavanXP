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

static int known_code_page(UINT code_page) {
    return code_page == CP_ACP || code_page == CP_OEMCP || code_page == CP_THREAD_ACP || code_page == 1252u ||
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
            unsigned char b = sxw_wide_to_ansi((unsigned short)c);
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
