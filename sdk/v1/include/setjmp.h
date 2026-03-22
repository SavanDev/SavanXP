#pragma once

typedef long jmp_buf[8];

#define setjmp sx_setjmp
#define longjmp sx_longjmp

int sx_setjmp(jmp_buf env);
void sx_longjmp(jmp_buf env, int value) __attribute__((noreturn));
