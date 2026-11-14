/* L2 fixture "tls": thread-local data of the main thread (arch_prctl). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static _Thread_local int initialized = 42;
static _Thread_local char zeroed[4096];
static _Thread_local _Alignas(64) unsigned long aligned = 0x5a5a5a5a5a5a5a5aul;

int main(void)
{
    int fail = 0;
    if (initialized != 42) {
        printf("initialized=%d\n", initialized);
        fail = 1;
    }
    for (size_t i = 0; i < sizeof zeroed; i++) {
        if (zeroed[i] != 0) {
            printf("zeroed[%zu]=%d\n", i, zeroed[i]);
            fail = 1;
            break;
        }
    }
    if (((uintptr_t)&aligned & 63) != 0 || aligned != 0x5a5a5a5a5a5a5a5aul) {
        printf("aligned wrong\n");
        fail = 1;
    }
    initialized = 7;
    memset(zeroed, 0x11, sizeof zeroed);
    aligned = 1;
    if (initialized != 7 || zeroed[4095] != 0x11 || aligned != 1) {
        printf("tls writes lost\n");
        fail = 1;
    }
    /* %fs:0 holds the thread pointer itself, as the x86-64 TLS ABI requires. */
    uintptr_t tp;
    __asm__("mov %%fs:0, %0" : "=r"(tp));
    if (tp != (uintptr_t)__builtin_thread_pointer()) {
        printf("thread pointer self-reference wrong\n");
        fail = 1;
    }
    printf("tls %s\n", fail ? "FAILED" : "ok");
    return fail;
}
