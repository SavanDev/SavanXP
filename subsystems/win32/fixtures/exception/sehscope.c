/* Compiler-produced x86 SEH scopes, including a nested exception in a filter. */
#include "trace.h"
#include "seh.h"

#define OUTER_CODE 0xe0421010u
#define INNER_CODE 0xe0421011u

typedef void (WINAPI *unwind_fn)(seh_frame *, void *, EXCEPTION_RECORD *, void *);

static EXCEPTION_DISPOSITION __cdecl expect_unwind(EXCEPTION_RECORD *record, seh_frame *frame,
                                                   CONTEXT *context, seh_frame **dispatcher) {
    (void)frame;
    (void)dispatcher;
    if (record->ExceptionCode != 0xc0000027u || (record->ExceptionFlags & SEH_UNWIND_FLAGS) != EXCEPTION_UNWINDING ||
        context->Eax != 123) {
        ExitProcess(16);
    }
    SAY("explicit unwind called inner handler");
    return ExceptionContinueSearch;
}

static EXCEPTION_DISPOSITION __cdecl reject_unwind(EXCEPTION_RECORD *record, seh_frame *frame,
                                                   CONTEXT *context, seh_frame **dispatcher) {
    (void)record;
    (void)frame;
    (void)context;
    (void)dispatcher;
    ExitProcess(17);
}

__asm__(".safeseh _expect_unwind\n\t.safeseh _reject_unwind");

static __declspec(noinline) void call_unwind(seh_frame *target, unwind_fn unwind) {
    seh_frame young = {target, expect_unwind};
    seh_frame *volatile expected_head = target;
    seh_set_head(&young);
    unwind(target, NULL, NULL, (void *)123);
    if (seh_head() != expected_head) {
        ExitProcess(18);
    }
}

static void check_unwind(void) {
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    unwind_fn unwind = (unwind_fn)GetProcAddress(kernel, "RtlUnwind");
    if (unwind == NULL) {
        ExitProcess(19);
    }
    seh_frame target = {seh_head(), reject_unwind};
    seh_set_head(&target);
    call_unwind(&target, unwind);
    seh_set_head(target.next);
    SAY("explicit unwind resumed at target registration");
}

static LONG reject(DWORD code) {
    if (code == INNER_CODE) {
        SAY("inner filter searched nested exception");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code != OUTER_CODE) {
        ExitProcess(10);
    }
    SAY("inner filter searched");
    return EXCEPTION_CONTINUE_SEARCH;
}

static __declspec(noinline) void raise_in_scope(void) {
    __try {
        __try {
            RaiseException(OUTER_CODE, 0, 0, NULL);
        } __except (reject(_exception_code())) {
            ExitProcess(11);
        }
    } __finally {
        if (!_abnormal_termination()) {
            ExitProcess(12);
        }
        SAY("inner finally unwound");
    }
}

static LONG select_handler(const EXCEPTION_POINTERS *pointers) {
    const EXCEPTION_RECORD *record = pointers->ExceptionRecord;
    if (record->ExceptionCode == INNER_CODE) {
        SAY("nested exception continued");
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (record->ExceptionCode != OUTER_CODE) {
        ExitProcess(13);
    }
    SAY("outer filter entered");
    RaiseException(INNER_CODE, 0, 0, NULL);
    SAY("outer filter resumed");
    return EXCEPTION_EXECUTE_HANDLER;
}

void WINAPI start(void) {
    __try {
        __try {
            raise_in_scope();
        } __finally {
            if (!_abnormal_termination()) {
                ExitProcess(14);
            }
            SAY("local finally unwound");
        }
    } __except (select_handler(_exception_info())) {
        SAY("outer handler ran");
    }
    SAY("SEH scope resumed");
    check_unwind();
    ExitProcess(0);
}
