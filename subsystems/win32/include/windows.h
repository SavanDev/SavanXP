/*
 * windows.h -- The subsystem's own Windows header: the types, constants and
 * declarations the DLLs and fixtures under subsystems/win32/ use, and
 * nothing else. It grows with the W0 matrix; it is not the Windows SDK.
 */
#ifndef SAVANXP_WIN32_WINDOWS_H
#define SAVANXP_WIN32_WINDOWS_H

#define WINAPI __stdcall
#define DECLSPEC_IMPORT __declspec(dllimport)
#define DECLSPEC_EXPORT __declspec(dllexport)

typedef int BOOL;
typedef long LONG;
typedef unsigned int UINT;
typedef unsigned long ULONG_PTR;
typedef ULONG_PTR SIZE_T;
typedef char CHAR;
typedef unsigned short WCHAR;
typedef CHAR *LPSTR;
typedef const CHAR *LPCSTR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef unsigned char BYTE;
typedef BYTE *LPBYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef void *HINSTANCE;
typedef void *HMODULE;
typedef void *HLOCAL;
typedef struct HKEY__ *HKEY;
typedef HKEY *PHKEY;
typedef DWORD REGSAM;
typedef int(WINAPI *FARPROC)(void);
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef DWORD *LPDWORD;
typedef struct _OVERLAPPED OVERLAPPED, *LPOVERLAPPED;
typedef struct _SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

typedef struct _STARTUPINFOA {
    DWORD cb;
    LPSTR lpReserved;
    LPSTR lpDesktop;
    LPSTR lpTitle;
    DWORD dwX;
    DWORD dwY;
    DWORD dwXSize;
    DWORD dwYSize;
    DWORD dwXCountChars;
    DWORD dwYCountChars;
    DWORD dwFillAttribute;
    DWORD dwFlags;
    WORD wShowWindow;
    WORD cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;
typedef void(WINAPI *PIMAGE_TLS_CALLBACK)(LPVOID DllHandle, DWORD Reason, LPVOID Reserved);

typedef struct _IMAGE_TLS_DIRECTORY32 {
    DWORD StartAddressOfRawData;
    DWORD EndAddressOfRawData;
    DWORD AddressOfIndex;
    DWORD AddressOfCallBacks;
    DWORD SizeOfZeroFill;
    DWORD Characteristics;
} IMAGE_TLS_DIRECTORY32;

#define TRUE 1
#define FALSE 0
#define NULL ((void *)0)
#define INVALID_HANDLE_VALUE ((HANDLE)(long)-1)

#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define STD_ERROR_HANDLE ((DWORD)-12)

#define ERROR_SUCCESS 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_HANDLE 6u
#define ERROR_NOT_ENOUGH_MEMORY 8u
#define ERROR_INVALID_PARAMETER 87u
#define ERROR_MOD_NOT_FOUND 126u
#define ERROR_PROC_NOT_FOUND 127u
#define ERROR_MORE_DATA 234L

#define LMEM_FIXED 0x0000u
#define LMEM_MOVEABLE 0x0002u
#define LMEM_NOCOMPACT 0x0010u
#define LMEM_NODISCARD 0x0020u
#define LMEM_ZEROINIT 0x0040u
#define LPTR (LMEM_FIXED | LMEM_ZEROINIT)

#define STARTF_USESHOWWINDOW 0x00000001u
#define SW_SHOWDEFAULT 10

#define HKEY_CLASSES_ROOT ((HKEY)(ULONG_PTR)0x80000000u)
#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)0x80000001u)
#define HKEY_LOCAL_MACHINE ((HKEY)(ULONG_PTR)0x80000002u)
#define REG_NONE 0u
#define REG_SZ 1u
#define REG_EXPAND_SZ 2u
#define REG_BINARY 3u
#define REG_DWORD 4u
#define REG_MULTI_SZ 7u
#define REG_OPTION_NON_VOLATILE 0u
#define REG_CREATED_NEW_KEY 1u
#define REG_OPENED_EXISTING_KEY 2u
#define KEY_QUERY_VALUE 0x0001u
#define KEY_SET_VALUE 0x0002u
#define KEY_READ 0x20019u
#define KEY_ALL_ACCESS 0xf003fu

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1

#ifndef SXW_BUILDING_KERNEL32
#define SXW_KERNEL32_API DECLSPEC_IMPORT
#else
#define SXW_KERNEL32_API
#endif

SXW_KERNEL32_API HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
SXW_KERNEL32_API LPWSTR WINAPI GetCommandLineW(void);
SXW_KERNEL32_API void WINAPI GetStartupInfoA(LPSTARTUPINFOA lpStartupInfo);
SXW_KERNEL32_API HMODULE WINAPI GetModuleHandleA(LPCSTR lpModuleName);
SXW_KERNEL32_API HMODULE WINAPI LoadLibraryA(LPCSTR lpLibFileName);
SXW_KERNEL32_API FARPROC WINAPI GetProcAddress(HMODULE hModule, LPCSTR lpProcName);
SXW_KERNEL32_API HLOCAL WINAPI LocalAlloc(UINT uFlags, SIZE_T uBytes);
SXW_KERNEL32_API HLOCAL WINAPI LocalFree(HLOCAL hMem);
SXW_KERNEL32_API int WINAPI lstrlenW(LPCWSTR lpString);
SXW_KERNEL32_API int WINAPI MulDiv(int nNumber, int nNumerator, int nDenominator);
SXW_KERNEL32_API BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                                       LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
SXW_KERNEL32_API __declspec(noreturn) void WINAPI ExitProcess(unsigned int uExitCode);


#ifndef SXW_BUILDING_ADVAPI32
#define SXW_ADVAPI32_API DECLSPEC_IMPORT
#else
#define SXW_ADVAPI32_API
#endif

SXW_ADVAPI32_API LONG WINAPI RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass,
                                             DWORD dwOptions, REGSAM samDesired,
                                             LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                                             LPDWORD lpdwDisposition);
SXW_ADVAPI32_API LONG WINAPI RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired,
                                           PHKEY phkResult);
SXW_ADVAPI32_API LONG WINAPI RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
                                              LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
SXW_ADVAPI32_API LONG WINAPI RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
                                              LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
SXW_ADVAPI32_API LONG WINAPI RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType,
                                            const BYTE *lpData, DWORD cbData);
SXW_ADVAPI32_API LONG WINAPI RegCloseKey(HKEY hKey);

#endif
