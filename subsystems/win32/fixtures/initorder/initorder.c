/*
 * initorder.exe -- The second W1 fixture. It imports from alpha.dll and
 * beta.dll, which share a preferred base, and carries static TLS with two
 * callbacks and no C runtime. Its output records the order Windows runs
 * DllMain, the TLS callbacks and the entry point in, on the way in and on
 * the way out, and the TLS variable shows the template and the callbacks
 * both reached this thread's block.
 */
#include "say.h"

__declspec(dllimport) unsigned WINAPI AlphaValue(void);
__declspec(dllimport) unsigned WINAPI BetaValue(void);

/* What the C runtime's tlssup.c provides, written out. */
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLB", read)
#pragma section(".CRT$XLZ", read)
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)

/* The compiler reaches a thread's TLS block through fs:[__tls_array], the
 * TEB's ThreadLocalStoragePointer; the C runtime defines it as 0x2c. */
__asm__(".globl __tls_array\n.set __tls_array, 0x2c");

DWORD _tls_index = 0;
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
__declspec(allocate(".CRT$XLA")) PIMAGE_TLS_CALLBACK __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) PIMAGE_TLS_CALLBACK __xl_z = 0;

const IMAGE_TLS_DIRECTORY32 _tls_used = {
    (DWORD)&_tls_start, (DWORD)&_tls_end, (DWORD)&_tls_index, (DWORD)(&__xl_a + 1), 0, 0,
};

__declspec(thread) unsigned thread_value = 5;

static void WINAPI first_callback(LPVOID instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    thread_value += 1;
    say_digit("initorder: TLS callback 1, reason ", reason, "\r\n");
}

static void WINAPI second_callback(LPVOID instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    thread_value += 1;
    say_digit("initorder: TLS callback 2, reason ", reason, "\r\n");
}

__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK callbacks[] = {first_callback, second_callback};

void WINAPI start(void) {
    say_digit("initorder: entry, thread value ", thread_value, "");
    say_digit(", alpha says ", AlphaValue(), "");
    say_digit(", beta says ", BetaValue(), "\r\n");
    ExitProcess(thread_value == 7 ? 7 : 1);
}
