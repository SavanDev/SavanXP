#ifndef SAVANXP_WIN32_MSVCRT_EXCEPTION_H
#define SAVANXP_WIN32_MSVCRT_EXCEPTION_H

#include "seh.h"

DECLSPEC_IMPORT void WINAPI SxwRaiseExceptionWithExitStatus(EXCEPTION_RECORD *record, DWORD status);
DECLSPEC_IMPORT void WINAPI RtlUnwind(seh_frame *target, void *ip, EXCEPTION_RECORD *record, void *value);

/* MSVC funclets address the parent frame through EBP. */
typedef ULONG_PTR (__cdecl *seh_funclet)(void);
ULONG_PTR __cdecl seh_call_funclet(seh_funclet code, void *base, void *argument);

#endif
