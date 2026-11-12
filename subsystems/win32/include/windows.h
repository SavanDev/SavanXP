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
typedef unsigned char BYTE;
typedef unsigned short WORD;
typedef unsigned long DWORD;
typedef void *HANDLE;
typedef void *HINSTANCE;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef DWORD *LPDWORD;
typedef struct _OVERLAPPED OVERLAPPED, *LPOVERLAPPED;
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

#define ERROR_INVALID_HANDLE 6u
#define ERROR_INVALID_PARAMETER 87u

#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1

#ifndef SXW_BUILDING_KERNEL32
#define SXW_KERNEL32_API DECLSPEC_IMPORT
#else
#define SXW_KERNEL32_API
#endif

SXW_KERNEL32_API HANDLE WINAPI GetStdHandle(DWORD nStdHandle);
SXW_KERNEL32_API BOOL WINAPI WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite,
                                       LPDWORD lpNumberOfBytesWritten, LPOVERLAPPED lpOverlapped);
SXW_KERNEL32_API __declspec(noreturn) void WINAPI ExitProcess(unsigned int uExitCode);

#endif
