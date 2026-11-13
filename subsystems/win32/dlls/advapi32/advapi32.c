/*
 * advapi32.dll -- The six registry functions of the W0 matrix, over the
 * host's registry services (sxw_abi.h). The ANSI ones convert through the
 * subsystem's code page (Windows-1252) and call the same services.
 */
#define SXW_BUILDING_ADVAPI32
#include <windows.h>

#include "codepage.h"
#include "sxw_abi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 WINAPI SxwService(u32 service, const u32 *arguments);
__declspec(dllimport) void *WINAPI SxwHeapAllocate(u32 size, u32 zero);
__declspec(dllimport) int WINAPI SxwHeapFree(void *pointer);

#define MAX_NAME 256

/* An ANSI string as UTF-16 into out[MAX_NAME]. 0 when it does not fit. */
static int widen(const char *text, WCHAR *out) {
    for (u32 i = 0; i < MAX_NAME; ++i) {
        out[i] = sxw_ansi_to_wide((unsigned char)text[i]);
        if (text[i] == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_string_type(DWORD type) {
    return type == REG_SZ || type == REG_EXPAND_SZ || type == REG_MULTI_SZ;
}

/* Volatile keys, a class and security attributes are not kept: only the
 * default options are accepted. */
LONG WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions,
                            REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                            LPDWORD lpdwDisposition) {
    (void)lpClass;
    (void)samDesired;
    (void)lpSecurityAttributes;
    if (Reserved != 0 || phkResult == NULL || dwOptions != REG_OPTION_NON_VOLATILE) {
        return ERROR_INVALID_PARAMETER;
    }
    u32 arguments[5] = {(u32)hKey, (u32)lpSubKey, 1u, (u32)phkResult, (u32)lpdwDisposition};
    return (LONG)SxwService(SXW_SVC_REG_OPEN, arguments);
}

LONG WINAPI RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult) {
    (void)samDesired;
    WCHAR subkey[MAX_NAME];
    if (ulOptions != 0 || phkResult == NULL || (lpSubKey != NULL && !widen(lpSubKey, subkey))) {
        return ERROR_INVALID_PARAMETER;
    }
    u32 arguments[5] = {(u32)hKey, lpSubKey != NULL ? (u32)subkey : 0u, 0u, (u32)phkResult, 0u};
    return (LONG)SxwService(SXW_SVC_REG_OPEN, arguments);
}

LONG WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
                             LPBYTE lpData, LPDWORD lpcbData) {
    if (lpReserved != NULL) {
        return ERROR_INVALID_PARAMETER;
    }
    u32 arguments[5] = {(u32)hKey, (u32)lpValueName, (u32)lpType, (u32)lpData, (u32)lpcbData};
    return (LONG)SxwService(SXW_SVC_REG_QUERY, arguments);
}

/* Strings come back in the ANSI code page, sized in bytes of it. */
LONG WINAPI RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType,
                             LPBYTE lpData, LPDWORD lpcbData) {
    WCHAR name[MAX_NAME];
    if (lpReserved != NULL || (lpValueName != NULL && !widen(lpValueName, name)) ||
        (lpData != NULL && lpcbData == NULL)) {
        return ERROR_INVALID_PARAMETER;
    }
    const u32 wide_name = lpValueName != NULL ? (u32)name : 0u;
    DWORD type = 0;
    DWORD size = 0;
    u32 query[5] = {(u32)hKey, wide_name, (u32)&type, 0u, (u32)&size};
    LONG error = (LONG)SxwService(SXW_SVC_REG_QUERY, query);
    if (error != ERROR_SUCCESS) {
        return error;
    }
    if (lpType != NULL) {
        *lpType = type;
    }
    if (!is_string_type(type)) {
        u32 arguments[5] = {(u32)hKey, wide_name, 0u, (u32)lpData, (u32)lpcbData};
        return lpcbData == NULL ? ERROR_SUCCESS : (LONG)SxwService(SXW_SVC_REG_QUERY, arguments);
    }
    const DWORD ansi_size = size / 2u;
    if (lpcbData == NULL) {
        return ERROR_SUCCESS;
    }
    if (lpData == NULL) {
        *lpcbData = ansi_size;
        return ERROR_SUCCESS;
    }
    if (*lpcbData < ansi_size) {
        *lpcbData = ansi_size;
        return ERROR_MORE_DATA;
    }
    WCHAR *wide = SxwHeapAllocate(size + 2u, 1);
    if (wide == NULL) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    DWORD got = size;
    u32 arguments[5] = {(u32)hKey, wide_name, 0u, (u32)wide, (u32)&got};
    error = (LONG)SxwService(SXW_SVC_REG_QUERY, arguments);
    if (error == ERROR_SUCCESS) {
        for (DWORD i = 0; i < got / 2u; ++i) {
            lpData[i] = sxw_wide_to_ansi(wide[i]);
        }
        *lpcbData = got / 2u;
    }
    SxwHeapFree(wide);
    return error;
}

LONG WINAPI RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE *lpData,
                           DWORD cbData) {
    if (Reserved != 0) {
        return ERROR_INVALID_PARAMETER;
    }
    u32 arguments[5] = {(u32)hKey, (u32)lpValueName, dwType, (u32)lpData, cbData};
    return (LONG)SxwService(SXW_SVC_REG_SET, arguments);
}

LONG WINAPI RegCloseKey(HKEY hKey) {
    u32 arguments[1] = {(u32)hKey};
    return (LONG)SxwService(SXW_SVC_REG_CLOSE, arguments);
}
