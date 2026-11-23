/* x86 software exception dispatch, entirely on the guest thread. */
#include "seh.h"

unsigned int WINAPI SxwService(unsigned int service, const unsigned int *arguments);

#define STATUS_UNWIND 0xc0000027u
#define STATUS_BAD_STACK 0xc0000028u
#define STATUS_INVALID_UNWIND_TARGET 0xc0000029u

/* Stack at the naked entry point after pushfl and pushal. */
typedef struct saved_registers {
    DWORD edi, esi, ebp, ignored_esp, ebx, edx, ecx, eax;
    DWORD eflags, return_address;
    DWORD args[4];
} saved_registers;

static __declspec(noreturn) void end_exception(DWORD status) {
    const unsigned int args[] = {status};
    SxwService(SXW_SVC_EXIT_PROCESS, args);
    __builtin_unreachable();
}

/* Use the Windows x86 CONTEXT offsets even when entering through a hand-
 * written frame handler. Resume on the saved stack, using popal's register
 * order and returning to the saved instruction pointer. */
_Static_assert(__builtin_offsetof(CONTEXT, Edi) == 156 && __builtin_offsetof(CONTEXT, Eip) == 184 &&
               __builtin_offsetof(CONTEXT, Esp) == 196 && sizeof(CONTEXT) == 716, "x86 CONTEXT layout");
static __declspec(naked) __declspec(noreturn) void __cdecl resume(CONTEXT *context) {
    __asm__("movl 4(%esp), %eax\n\t"
            "movl 196(%eax), %esp\n\t"
            "pushl 184(%eax)\n\t"  /* Eip */
            "pushl 192(%eax)\n\t"  /* EFlags */
            "pushl 176(%eax)\n\t"  /* Eax */
            "pushl 172(%eax)\n\t"  /* Ecx */
            "pushl 168(%eax)\n\t"  /* Edx */
            "pushl 164(%eax)\n\t"  /* Ebx */
            "pushl $0\n\t"         /* popal skips Esp */
            "pushl 180(%eax)\n\t"  /* Ebp */
            "pushl 160(%eax)\n\t"  /* Esi */
            "pushl 156(%eax)\n\t"  /* Edi */
            "popal\n\t"
            "popfl\n\t"
            "retl");
}

static BOOL valid_frame(const seh_frame *frame, DWORD low, DWORD high) {
    const DWORD address = (DWORD)frame;
    return (address & 3u) == 0 && address >= low && address < high && high - address >= sizeof(*frame);
}

static BOOL allowed_handler(seh_handler handler) {
    const unsigned int args[] = {(unsigned int)handler};
    return SxwService(SXW_SVC_SEH_HANDLER, args) != 0;
}

typedef struct handler_guard {
    seh_frame registration;
    seh_frame *frame;
    BOOL unwinding;
} handler_guard;

/* A handler may itself raise or start another unwind. Its guard identifies
 * the interrupted frame to the new search or unwind. */
static EXCEPTION_DISPOSITION __cdecl guard_handler(EXCEPTION_RECORD *record, seh_frame *registration,
                                                   CONTEXT *context, seh_frame **dispatcher) {
    (void)context;
    const handler_guard *guard = (const handler_guard *)registration;
    const BOOL unwinding = (record->ExceptionFlags & SEH_UNWIND_FLAGS) != 0;
    if (guard->unwinding != unwinding) {
        return ExceptionContinueSearch;
    }
    *dispatcher = guard->frame;
    return unwinding ? ExceptionCollidedUnwind : ExceptionNestedException;
}

__asm__(".safeseh _guard_handler");

/* x86 callbacks in the wild use both stack cleanup conventions. */
static __declspec(naked) EXCEPTION_DISPOSITION __cdecl call_handler(seh_handler handler,
        EXCEPTION_RECORD *record, seh_frame *frame, CONTEXT *context, seh_frame **dispatcher) {
    __asm__("pushl %ebp\n\t"
            "movl %esp, %ebp\n\t"
            "pushl 24(%ebp)\n\t"
            "pushl 20(%ebp)\n\t"
            "pushl 16(%ebp)\n\t"
            "pushl 12(%ebp)\n\t"
            "calll *8(%ebp)\n\t"
            "leave\n\t"
            "retl");
}

static EXCEPTION_DISPOSITION invoke(seh_handler handler, EXCEPTION_RECORD *record, seh_frame *frame,
                                    CONTEXT *context, seh_frame **dispatcher) {
    handler_guard guard = {{seh_head(), guard_handler}, frame, (record->ExceptionFlags & SEH_UNWIND_FLAGS) != 0};
    seh_set_head(&guard.registration);
    const EXCEPTION_DISPOSITION disposition = call_handler(handler, record, frame, context, dispatcher);
    seh_set_head(guard.registration.next);
    return disposition;
}

static DWORD stack_limit(void) {
    DWORD value;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(value) : "i"(SXW_TEB_STACK_LIMIT));
    return value;
}

static DWORD stack_base(void) {
    DWORD value;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(value) : "i"(SXW_TEB_STACK_BASE));
    return value;
}

/* Zero means the search ended unhandled. A nonzero result is a new exception
 * raised by the dispatcher itself. Successful continuation does not return. */
static DWORD dispatch(EXCEPTION_RECORD *record, CONTEXT *context) {
    DWORD low = stack_limit();
    const DWORD high = stack_base();
    seh_frame *nested = NULL;
    for (seh_frame *frame = seh_head(); frame != SEH_END; frame = frame->next) {
        if (!valid_frame(frame, low, high)) {
            record->ExceptionFlags |= EXCEPTION_STACK_INVALID;
            break;
        }
        low = (DWORD)frame + sizeof(*frame);
        const seh_handler handler = frame->handler;
        if (!allowed_handler(handler)) {
            break;
        }

        seh_frame *dispatcher = NULL;
        const EXCEPTION_DISPOSITION disposition = invoke(handler, record, frame, context, &dispatcher);
        if (frame == nested) {
            record->ExceptionFlags &= ~EXCEPTION_NESTED_CALL;
            nested = NULL;
        }
        if (disposition == ExceptionContinueExecution) {
            if ((record->ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0u) {
                return EXCEPTION_NONCONTINUABLE_EXCEPTION;
            }
            resume(context);
        }
        if (disposition == ExceptionNestedException && valid_frame(dispatcher, low, high)) {
            if ((DWORD)dispatcher > (DWORD)nested) {
                nested = dispatcher;
            }
            record->ExceptionFlags |= EXCEPTION_NESTED_CALL;
        } else if (disposition != ExceptionContinueSearch) {
            return EXCEPTION_INVALID_DISPOSITION;
        }
    }
    return 0;
}

static void capture(CONTEXT *context, const saved_registers *saved, unsigned int arguments) {
    context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context->Edi = saved->edi;
    context->Esi = saved->esi;
    context->Ebp = saved->ebp;
    context->Ebx = saved->ebx;
    context->Edx = saved->edx;
    context->Ecx = saved->ecx;
    context->Eax = saved->eax;
    context->EFlags = saved->eflags;
    context->Eip = saved->return_address;
    context->Esp = (DWORD)&saved->args[arguments];
    unsigned short cs, ss;
    __asm__("movw %%cs, %0" : "=r"(cs));
    __asm__("movw %%ss, %0" : "=r"(ss));
    context->SegCs = cs;
    context->SegSs = ss;
}

static __attribute__((used)) __declspec(noreturn) void __cdecl dispatch_raised(const saved_registers *saved,
                                                                             unsigned int arguments) {
    CONTEXT context = {0};
    capture(&context, saved, arguments);
    EXCEPTION_RECORD *record = (EXCEPTION_RECORD *)saved->args[0];
    record->ExceptionAddress = (void *)context.Eip;
    DWORD status = dispatch(record, &context);
    if (status == 0) {
        end_exception(arguments == 2 ? saved->args[1] : record->ExceptionCode);
    }

    EXCEPTION_RECORD secondary = {0};
    secondary.ExceptionCode = status;
    secondary.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    secondary.ExceptionRecord = record;
    secondary.ExceptionAddress = (void *)context.Eip;
    DWORD secondary_status = dispatch(&secondary, &context);
    end_exception(secondary_status != 0 ? secondary_status : status);
}

static __declspec(noreturn) void unwind_failure(DWORD status, EXCEPTION_RECORD *record, CONTEXT *context) {
    EXCEPTION_RECORD failure = {0};
    failure.ExceptionCode = status;
    failure.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    failure.ExceptionRecord = record;
    failure.ExceptionAddress = (void *)context->Eip;
    const DWORD dispatch_status = dispatch(&failure, context);
    end_exception(dispatch_status != 0 ? dispatch_status : status);
}

static __attribute__((used)) __declspec(noreturn) void __cdecl unwind(const saved_registers *saved) {
    CONTEXT context = {0};
    capture(&context, saved, 4);
    context.Eax = saved->args[3];
    seh_frame *target = (seh_frame *)saved->args[0];
    EXCEPTION_RECORD *record = (EXCEPTION_RECORD *)saved->args[2];
    EXCEPTION_RECORD synthetic = {0};
    if (record == NULL) {
        synthetic.ExceptionCode = STATUS_UNWIND;
        synthetic.ExceptionAddress = (void *)context.Eip;
        record = &synthetic;
    }

    record->ExceptionFlags |= EXCEPTION_UNWINDING;
    if (target == NULL) {
        record->ExceptionFlags |= EXCEPTION_EXIT_UNWIND;
        target = SEH_END;
    }
    DWORD low = stack_limit();
    const DWORD high = stack_base();
    for (seh_frame *frame = seh_head(); frame != target; frame = seh_head()) {
        if ((DWORD)frame > (DWORD)target) {
            unwind_failure(STATUS_INVALID_UNWIND_TARGET, record, &context);
        }
        if (!valid_frame(frame, low, high)) {
            unwind_failure(STATUS_BAD_STACK, record, &context);
        }
        const seh_handler handler = frame->handler;
        if (!allowed_handler(handler)) {
            unwind_failure(EXCEPTION_INVALID_DISPOSITION, record, &context);
        }

        seh_frame *dispatcher = NULL;
        const EXCEPTION_DISPOSITION disposition = invoke(handler, record, frame, &context, &dispatcher);
        if (disposition == ExceptionCollidedUnwind && valid_frame(dispatcher, (DWORD)frame, high) &&
            (DWORD)dispatcher < (DWORD)target) {
            frame = dispatcher;
        } else if (disposition != ExceptionContinueSearch) {
            unwind_failure(EXCEPTION_INVALID_DISPOSITION, record, &context);
        }
        low = (DWORD)frame + sizeof(*frame);
        seh_set_head(frame->next);
    }
    /* x86 resumes at the call site, with the target registration at fs:[0]. */
    resume(&context);
}

__declspec(naked) void WINAPI RtlRaiseException(EXCEPTION_RECORD *record) {
    __asm__("pushfl\n\t"
            "pushal\n\t"
            "movl %esp, %eax\n\t"
            "pushl $1\n\t"
            "pushl %eax\n\t"
            "calll _dispatch_raised");
}

/* The CRT reports uncaught C++ exceptions as termination, while handlers
 * still see the C++ exception code and object during the search. */
__declspec(naked) void WINAPI SxwRaiseExceptionWithExitStatus(EXCEPTION_RECORD *record, DWORD status) {
    __asm__("pushfl\n\t"
            "pushal\n\t"
            "movl %esp, %eax\n\t"
            "pushl $2\n\t"
            "pushl %eax\n\t"
            "calll _dispatch_raised");
}

__declspec(naked) void WINAPI RtlUnwind(seh_frame *target, void *ip, EXCEPTION_RECORD *record, void *result) {
    __asm__("pushfl\n\t"
            "pushal\n\t"
            "pushl %esp\n\t"
            "calll _unwind");
}
