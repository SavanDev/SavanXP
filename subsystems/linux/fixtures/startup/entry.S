/* L1 fixture "startup", entry and register checks. Libc-free.
 *
 * _start records every general register as the kernel left it, then calls
 * startup_main(stack pointer, saved registers). regcheck() makes `write`
 * calls of zero bytes with sentinels in every register the Linux ABI keeps
 * across `syscall`, and counts the calls after which one of them changed.
 */
    .text
    .globl _start
_start:
    lea  initial_regs(%rip), %rax      /* rax itself is recorded below */
    mov  %rbx,  8(%rax)
    mov  %rcx, 16(%rax)
    mov  %rdx, 24(%rax)
    mov  %rsi, 32(%rax)
    mov  %rdi, 40(%rax)
    mov  %rbp, 48(%rax)
    mov  %r8,  56(%rax)
    mov  %r9,  64(%rax)
    mov  %r10, 72(%rax)
    mov  %r11, 80(%rax)
    mov  %r12, 88(%rax)
    mov  %r13, 96(%rax)
    mov  %r14, 104(%rax)
    mov  %r15, 112(%rax)
    mov  %rsp, 120(%rax)
    movq $0, (%rax)                    /* rax was clobbered by the lea */
    mov  %rsp, %rdi
    mov  %rax, %rsi
    and  $-16, %rsp
    call startup_main
    ud2

/* uint64_t regcheck(uint64_t iterations, void *buffer)
 * Returns the number of calls after which a preserved register, RCX, R11 or
 * the result differed from what Linux guarantees. */
    .globl regcheck
regcheck:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %rdi, %rcx                    /* iterations */
    mov  %rsi, %rax
    sub  $48, %rsp
    mov  %rcx, 0(%rsp)                 /* remaining */
    mov  %rax, 8(%rsp)                 /* buffer */
    movq $0, 16(%rsp)                  /* failures */
    mov  $0x1111111111111111, %rax
    movq %rax, %xmm0
    mov  $0x2222222222222222, %rax
    movq %rax, %xmm7
    mov  $0x3333333333333333, %rax
    movq %rax, %xmm15
1:
    mov  $0xb0b0b0b0b0b0b0b0, %rbx
    mov  $0xb1b1b1b1b1b1b1b1, %rbp
    mov  $0xc2c2c2c2c2c2c2c2, %r12
    mov  $0xc3c3c3c3c3c3c3c3, %r13
    mov  $0xc4c4c4c4c4c4c4c4, %r14
    mov  $0xc5c5c5c5c5c5c5c5, %r15
    mov  $0xa4a4a4a4a4a4a4a4, %r10     /* fourth to sixth arguments: */
    mov  $0xa5a5a5a5a5a5a5a5, %r8      /* write ignores them and must */
    mov  $0xa6a6a6a6a6a6a6a6, %r9      /* keep them */
    mov  $1, %edi
    mov  8(%rsp), %rsi
    xor  %edx, %edx
    mov  $1, %eax                      /* write(1, buffer, 0) */
    syscall
2:
    test %rdx, %rdx                    /* rdx was 0 and must still be */
    jnz  9f
    test %rax, %rax
    jnz  9f
    lea  2b(%rip), %rax
    cmp  %rax, %rcx                    /* RCX holds the return address */
    jne  9f
    test $0x2, %r11                    /* R11 holds RFLAGS: bit 1 is always set */
    jz   9f
    cmp  $1, %rdi
    jne  9f
    cmp  8(%rsp), %rsi
    jne  9f
    mov  $0xb0b0b0b0b0b0b0b0, %rax
    cmp  %rax, %rbx
    jne  9f
    mov  $0xb1b1b1b1b1b1b1b1, %rax
    cmp  %rax, %rbp
    jne  9f
    mov  $0xc2c2c2c2c2c2c2c2, %rax
    cmp  %rax, %r12
    jne  9f
    mov  $0xc3c3c3c3c3c3c3c3, %rax
    cmp  %rax, %r13
    jne  9f
    mov  $0xc4c4c4c4c4c4c4c4, %rax
    cmp  %rax, %r14
    jne  9f
    mov  $0xc5c5c5c5c5c5c5c5, %rax
    cmp  %rax, %r15
    jne  9f
    mov  $0xa4a4a4a4a4a4a4a4, %rax
    cmp  %rax, %r10
    jne  9f
    mov  $0xa5a5a5a5a5a5a5a5, %rax
    cmp  %rax, %r8
    jne  9f
    mov  $0xa6a6a6a6a6a6a6a6, %rax
    cmp  %rax, %r9
    jne  9f
    movq %xmm0, %rax
    mov  $0x1111111111111111, %rdx
    cmp  %rdx, %rax
    jne  9f
    movq %xmm7, %rax
    mov  $0x2222222222222222, %rdx
    cmp  %rdx, %rax
    jne  9f
    movq %xmm15, %rax
    mov  $0x3333333333333333, %rdx
    cmp  %rdx, %rax
    jne  9f
    jmp  3f
9:
    incq 16(%rsp)
3:
    decq 0(%rsp)
    jnz  1b
    mov  16(%rsp), %rax
    add  $48, %rsp
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %rbp
    pop  %rbx
    ret

    .bss
    .globl initial_regs
    .balign 8
initial_regs:
    .skip 128
