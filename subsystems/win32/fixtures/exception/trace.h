#ifndef SAVANXP_WIN32_EXCEPTION_FIXTURE_TRACE_H
#define SAVANXP_WIN32_EXCEPTION_FIXTURE_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif
#include <windows.h>
DECLSPEC_IMPORT UINT WINAPI SetErrorMode(UINT mode);
#ifdef __cplusplus
}
#endif

#define SEM_NOGPFAULTERRORBOX 0x0002u

static void say(const char *text, DWORD length) {
    DWORD written;
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, 0) || written != length) {
        ExitProcess(1);
    }
}

#define SAY(text) say(text "\r\n", sizeof(text "\r\n") - 1u)

#endif
