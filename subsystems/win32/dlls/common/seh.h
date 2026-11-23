#ifndef SAVANXP_WIN32_SEH_H
#define SAVANXP_WIN32_SEH_H

#include <windows.h>

#include "sxw_abi.h"

typedef struct seh_frame seh_frame;
typedef EXCEPTION_DISPOSITION (__cdecl *seh_handler)(EXCEPTION_RECORD *, seh_frame *, CONTEXT *, seh_frame **);

struct seh_frame {
    seh_frame *next;
    seh_handler handler;
};

#define SEH_END ((seh_frame *)0xffffffffu)
#define SEH_UNWIND_FLAGS (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)

static inline seh_frame *seh_head(void) {
    seh_frame *frame;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(frame) : "i"(SXW_TEB_EXCEPTION_LIST) : "memory");
    return frame;
}

static inline void seh_set_head(seh_frame *frame) {
    __asm__ volatile("movl %0, %%fs:%c1" : : "r"(frame), "i"(SXW_TEB_EXCEPTION_LIST) : "memory");
}

#endif
