/* Exercise the classic x86 C++ personality with compiler-generated tables. */
#include "trace.h"

/* Clang's x86 function descriptor extends the VC6 prefix with two words.
 * Present that common prefix to the classic personality in XP's msvcrt. */
__asm__(".text\n"
        ".globl ___CxxFrameHandler3\n"
        "___CxxFrameHandler3:\n\t"
        "pushl %ebp\n\t"
        "movl %esp, %ebp\n\t"
        "pushl %esi\n\t"
        "pushl %edi\n\t"
        "subl $28, %esp\n\t"
        "movl %eax, %esi\n\t"
        "movl %esp, %edi\n\t"
        "movl $7, %ecx\n\t"
        "cld\n\t"
        "rep movsl\n\t"
        "movl $0x19930520, (%esp)\n\t"
        "movl %esp, %eax\n\t"
        "pushl 20(%ebp)\n\t"
        "pushl 16(%ebp)\n\t"
        "pushl 12(%ebp)\n\t"
        "pushl 8(%ebp)\n\t"
        "calll ___CxxFrameHandler\n\t"
        "leal -8(%ebp), %esp\n\t"
        "popl %edi\n\t"
        "popl %esi\n\t"
        "popl %ebp\n\t"
        "retl\n"
        ".section .rdata,\"dr\"\n"
        ".globl \"??_7type_info@@6B@\"\n"
        "\"??_7type_info@@6B@\":\n\t"
        ".long 0\n"
        ".text");

extern "C" __declspec(noreturn) void __cdecl __std_terminate(void) {
    ExitProcess(22);
}

struct Trace {
    const char *text;
    DWORD size;

    ~Trace() {
        say(text, size);
    }
};

#define TRACE(text) {text "\r\n", sizeof(text "\r\n") - 1u}

struct Payload {
    int value;
    bool copied;

    explicit Payload(int number) : value(number), copied(false) {}
    Payload(const Payload &other) : value(other.value), copied(true) { SAY("payload copied"); }
    ~Payload() {
        if (copied) {
            SAY("catch copy destroyed");
        } else {
            SAY("thrown payload destroyed");
        }
    }
};

static __declspec(noinline) void leaf(void) {
    Trace local = TRACE("leaf destroyed");
    throw Payload(42);
}

static __declspec(noinline) void middle(void) {
    Trace local = TRACE("middle destroyed");
    leaf();
}

static __declspec(noinline) void outer(void) {
    Trace local = TRACE("outer destroyed");
    middle();
}

static __declspec(noinline) void rethrow(void) {
    try {
        outer();
    } catch (const Payload &payload) {
        if (payload.value != 42) {
            ExitProcess(20);
        }
        SAY("caught by reference");
        Trace local = TRACE("catch local destroyed");
        throw;
    }
}

extern "C" void WINAPI start(void) {
    try {
        rethrow();
    } catch (Payload payload) {
        if (payload.value != 42) {
            ExitProcess(21);
        }
        SAY("caught by value");
        try {
            throw 7;
        } catch (...) {
            SAY("nested catch all");
        }
        SAY("outer catch resumed");
    }
    SAY("C++ scope resumed");
    ExitProcess(0);
}

extern "C" void WINAPI uncaught(void) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX);
    throw 7;
}
