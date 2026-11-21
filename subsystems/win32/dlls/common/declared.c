/*
 * declared.c -- What a declared entry point does when called (declared.h).
 * Compiled into every DLL that has a declared.txt.
 */
#include "declared.h"

#include "sxw_abi.h"

typedef unsigned int u32;

__declspec(dllimport) u32 __stdcall SxwService(u32 service, const u32 *arguments);

#define ERROR_CALL_NOT_IMPLEMENTED 120u

void __cdecl sxw_declared_hit(sxw_declared *record) {
    if (record->stdcall) {
        u32 error = ERROR_CALL_NOT_IMPLEMENTED;
        __asm__ volatile("movl %0, %%fs:%c1" : : "r"(error), "i"(SXW_TEB_LAST_ERROR) : "memory");
    }
    if (record->seen) {
        return;
    }
    record->seen = 1;
    static const char suffix[] = " is only declared";
    char line[160];
    u32 n = 0;
    for (const char *s = record->name; *s != '\0' && n < sizeof line - sizeof suffix; ++s) {
        line[n++] = *s;
    }
    for (u32 i = 0; i + 1 < sizeof suffix; ++i) {
        line[n++] = suffix[i];
    }
    u32 arguments[2] = {(u32)line, n};
    SxwService(SXW_SVC_DEBUG_OUTPUT, arguments);
}
