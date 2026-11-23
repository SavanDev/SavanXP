/* Exercises x86 software exceptions without a C runtime. */
#include <windows.h>

#define HANDLED_CODE 0xe0421001u
#define NULL_ARGUMENTS_CODE 0xe0421002u
#define UNCAUGHT_CODE 0xe0421003u

typedef EXCEPTION_DISPOSITION (__cdecl *handler)(EXCEPTION_RECORD *, void *, CONTEXT *, void *);

typedef struct exception_frame {
    struct exception_frame *next;
    handler function;
} exception_frame;

/* The PE32 load configuration points at lld-link's SafeSEH table and count. */
extern const BYTE __safe_se_handler_table[];
extern const BYTE __safe_se_handler_count[];
const struct {
    DWORD size;
    DWORD reserved[15];
    const BYTE *handler_table;
    const BYTE *handler_count;
} _load_config_used = {72, {0}, __safe_se_handler_table, __safe_se_handler_count};

__asm__(".safeseh _inner\n\t.safeseh _outer");

static void push_frame(exception_frame *frame, handler function) {
    frame->function = function;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(frame->next));
    __asm__ volatile("movl %0, %%fs:0" : : "r"(frame) : "memory");
}

static void pop_frame(const exception_frame *frame) {
    __asm__ volatile("movl %0, %%fs:0" : : "r"(frame->next) : "memory");
}

static void say(const char *text, DWORD length) {
    DWORD written;
    if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text, length, &written, NULL) || written != length) {
        ExitProcess(1);
    }
}

#define SAY(text) say(text "\r\n", sizeof(text "\r\n") - 1u)

static EXCEPTION_DISPOSITION __cdecl inner(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
                                           void *dispatcher) {
    (void)frame;
    (void)context;
    (void)dispatcher;
    if (record->ExceptionCode != HANDLED_CODE || record->ExceptionFlags != 0 ||
        record->NumberParameters != EXCEPTION_MAXIMUM_PARAMETERS) {
        ExitProcess(2);
    }
    for (DWORD i = 0; i < EXCEPTION_MAXIMUM_PARAMETERS; ++i) {
        if (record->ExceptionInformation[i] != 100u + i) {
            ExitProcess(3);
        }
    }
    SAY("inner frame searched");
    return ExceptionContinueSearch;
}

static EXCEPTION_DISPOSITION __cdecl outer(EXCEPTION_RECORD *record, void *frame, CONTEXT *context,
                                           void *dispatcher) {
    (void)frame;
    (void)dispatcher;
    if (record->ExceptionRecord != NULL || record->ExceptionAddress == NULL ||
        (context->ContextFlags & (CONTEXT_CONTROL | CONTEXT_INTEGER)) != (CONTEXT_CONTROL | CONTEXT_INTEGER) ||
        context->Eip == 0 || context->Esp == 0) {
        ExitProcess(4);
    }
    if (record->ExceptionCode == HANDLED_CODE) {
        SAY("outer frame continued");
    } else if (record->ExceptionCode == NULL_ARGUMENTS_CODE && record->NumberParameters == 0) {
        SAY("null arguments ignored");
    } else {
        ExitProcess(5);
    }
    return ExceptionContinueExecution;
}

/* Keep the younger registration record in a separate stack frame. */
static __declspec(noinline) void raise_with_inner_frame(void) {
    ULONG_PTR arguments[EXCEPTION_MAXIMUM_PARAMETERS + 1];
    for (DWORD i = 0; i < sizeof(arguments) / sizeof(arguments[0]); ++i) {
        arguments[i] = 100u + i;
    }
    exception_frame frame;
    push_frame(&frame, inner);
    RaiseException(HANDLED_CODE, 0, sizeof(arguments) / sizeof(arguments[0]), arguments);
    pop_frame(&frame);
}

void WINAPI start(void) {
    exception_frame frame;
    push_frame(&frame, outer);
    raise_with_inner_frame();
    RaiseException(NULL_ARGUMENTS_CODE, 0, 8, NULL);
    pop_frame(&frame);
    SAY("execution resumed");
    ExitProcess(0);
}

void WINAPI uncaught(void) {
    RaiseException(UNCAUGHT_CODE, EXCEPTION_NONCONTINUABLE, 0, NULL);
    ExitProcess(6);
}
