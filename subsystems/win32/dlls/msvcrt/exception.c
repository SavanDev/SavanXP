/* Visual C++ x86 C exception scopes and termination handlers. */
#include "exception.h"

typedef struct seh_scope {
    int parent;
    seh_funclet filter;
    seh_funclet body;
} seh_scope;

typedef struct crt_seh_frame {
    seh_frame registration;
    const seh_scope *scopes;
    int level;
    DWORD base;
} crt_seh_frame;

__declspec(naked) ULONG_PTR __cdecl seh_call_funclet(seh_funclet code, void *base, void *argument) {
    __asm__("pushl %ebp\n\t"
            "pushl %ebx\n\t"
            "pushl %esi\n\t"
            "pushl %edi\n\t"
            "movl 24(%esp), %ebp\n\t"
            "pushl 28(%esp)\n\t"
            "calll *24(%esp)\n\t"
            "addl $4, %esp\n\t"
            "popl %edi\n\t"
            "popl %esi\n\t"
            "popl %ebx\n\t"
            "popl %ebp\n\t"
            "retl");
}

/* The compiled __except body restores its own ESP and rejoins its function. */
static __declspec(naked) __declspec(noreturn) void __cdecl enter_except(seh_funclet code, void *base) {
    __asm__("movl 8(%esp), %ebp\n\t"
            "jmpl *4(%esp)");
}

static void unwind_scopes(crt_seh_frame *frame, int target) {
    while (frame->level > target) {
        const seh_scope *scope = &frame->scopes[frame->level];
        /* Retire the scope before invoking user code so a new unwind does
         * not call the same termination handler again. */
        if (scope->parent >= frame->level || scope->parent < -1) {
            break;
        }
        frame->level = scope->parent;
        if (scope->filter == NULL) {
            seh_call_funclet(scope->body, &frame->base, NULL);
        }
    }
}

EXCEPTION_DISPOSITION __cdecl _except_handler3(EXCEPTION_RECORD *record, seh_frame *registration,
                                                CONTEXT *context, seh_frame **dispatcher) {
    (void)dispatcher;
    crt_seh_frame *frame = (crt_seh_frame *)registration;
    if (frame->level < 0) {
        return ExceptionContinueSearch;
    }
    if ((record->ExceptionFlags & SEH_UNWIND_FLAGS) != 0) {
        unwind_scopes(frame, -1);
        return ExceptionContinueSearch;
    }

    EXCEPTION_POINTERS pointers = {record, context};
    /* The compiler reads this slot from its filter funclet. */
    EXCEPTION_POINTERS **slot = (EXCEPTION_POINTERS **)((BYTE *)frame - sizeof(void *));
    EXCEPTION_POINTERS *previous = *slot;
    *slot = &pointers;
    for (int level = frame->level; level >= 0;) {
        const seh_scope *scope = &frame->scopes[level];
        if (scope->filter != NULL) {
            const LONG result = (LONG)seh_call_funclet(scope->filter, &frame->base, &pointers);
            if (result == EXCEPTION_CONTINUE_EXECUTION) {
                *slot = previous;
                return ExceptionContinueExecution;
            }
            if (result == EXCEPTION_EXECUTE_HANDLER) {
                RtlUnwind(registration, NULL, NULL, NULL);
                unwind_scopes(frame, level);
                frame->level = scope->parent;
                enter_except(scope->body, &frame->base);
            }
        }
        if (scope->parent >= level) {
            break;
        }
        level = scope->parent;
    }
    *slot = previous;
    return ExceptionContinueSearch;
}

__asm__(".safeseh __except_handler3");

int __cdecl _XcptFilter(DWORD code, EXCEPTION_POINTERS *pointers) {
    (void)code;
    (void)pointers;
    return EXCEPTION_CONTINUE_SEARCH;
}
