/* x86 software exception dispatch, entirely on the guest thread. */
#include <windows.h>

#include "sxw_abi.h"

unsigned int WINAPI SxwService(unsigned int service, const unsigned int *arguments);

typedef EXCEPTION_DISPOSITION (__cdecl *exception_handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *);

typedef struct exception_frame {
    struct exception_frame *next;
    exception_handler handler;
} exception_frame;

/* Stack at the naked entry point after pushfl and pushal. */
typedef struct saved_registers {
    DWORD edi, esi, ebp, ignored_esp, ebx, edx, ecx, eax;
    DWORD eflags, return_address;
    EXCEPTION_RECORD *record;
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

/* Zero means the search ended unhandled. A nonzero result is a new exception
 * raised by the dispatcher itself. Successful continuation does not return. */
static DWORD dispatch(EXCEPTION_RECORD *record, CONTEXT *context) {
    unsigned int head, low, high;
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(head) : "i"(SXW_TEB_EXCEPTION_LIST) : "memory");
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(low) : "i"(SXW_TEB_STACK_LIMIT));
    __asm__ volatile("movl %%fs:%c1, %0" : "=r"(high) : "i"(SXW_TEB_STACK_BASE));

    while (head != 0xffffffffu) {
        if ((head & 3u) != 0 || head < low || head >= high || high - head < sizeof(exception_frame)) {
            break;
        }
        exception_frame *frame = (exception_frame *)head;
        const unsigned int arguments[] = {(unsigned int)frame->handler};
        if (!SxwService(SXW_SVC_SEH_HANDLER, arguments)) {
            break;
        }

        void *dispatcher = NULL;
        EXCEPTION_DISPOSITION disposition = frame->handler(record, frame, context, &dispatcher);
        if (disposition == ExceptionContinueExecution) {
            if ((record->ExceptionFlags & EXCEPTION_NONCONTINUABLE) != 0u) {
                return EXCEPTION_NONCONTINUABLE_EXCEPTION;
            }
            resume(context);
        }
        if (disposition != ExceptionContinueSearch) {
            return EXCEPTION_INVALID_DISPOSITION;
        }

        low = head + sizeof(*frame);
        head = (unsigned int)frame->next;
    }
    return 0;
}

static __attribute__((used)) __declspec(noreturn) void __cdecl dispatch_raised(const saved_registers *saved) {
    CONTEXT context = {0};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    context.Edi = saved->edi;
    context.Esi = saved->esi;
    context.Ebp = saved->ebp;
    context.Ebx = saved->ebx;
    context.Edx = saved->edx;
    context.Ecx = saved->ecx;
    context.Eax = saved->eax;
    context.EFlags = saved->eflags;
    context.Eip = saved->return_address;
    context.Esp = (DWORD)(&saved->record + 1);
    unsigned short cs, ss;
    __asm__("movw %%cs, %0" : "=r"(cs));
    __asm__("movw %%ss, %0" : "=r"(ss));
    context.SegCs = cs;
    context.SegSs = ss;

    saved->record->ExceptionAddress = (void *)context.Eip;
    DWORD status = dispatch(saved->record, &context);
    if (status == 0) {
        end_exception(saved->record->ExceptionCode);
    }

    EXCEPTION_RECORD secondary = {0};
    secondary.ExceptionCode = status;
    secondary.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
    secondary.ExceptionRecord = saved->record;
    secondary.ExceptionAddress = (void *)context.Eip;
    DWORD secondary_status = dispatch(&secondary, &context);
    end_exception(secondary_status != 0 ? secondary_status : status);
}

__declspec(naked) void WINAPI RtlRaiseException(EXCEPTION_RECORD *record) {
    __asm__("pushfl\n\t"
            "pushal\n\t"
            "pushl %esp\n\t"
            "calll _dispatch_raised");
}
