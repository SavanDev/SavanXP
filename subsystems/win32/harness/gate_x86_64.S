/*
 * gate_x86_64.S -- The 64/32-bit gate of the host harness (Linux x86-64).
 *
 * Linux's own selectors: 0x23 is 32-bit user code, 0x2b user data, 0x33
 * 64-bit user code. FS holds an LDT selector whose base is the guest
 * thread's TEB while guest code runs, and the host thread's glibc FS base
 * is put back before any C code runs on the host side.
 *
 * The 32-bit halves of each transition live in a trampoline page below
 * 4 GiB that harness.c fills (a far jump cannot reach this file's
 * addresses from 32-bit code): back32 and service32 far-jump to 64-bit
 * landings that jump here, and svc_ret32 is the guest's `ret 8`.
 *
 * Each guest thread has its gate state in sxw_gates[], found on the way
 * back from the guest by the index its TEB holds (SXW_TEB_HOST_THREAD,
 * 0xff0), read through FS while FS is still the TEB. %r12 holds the state's
 * address on the host side; guest code cannot see it.
 */
    .set STATE_HOST_RSP, 0
    .set STATE_HOST_FSBASE, 8
    .set STATE_GUEST_ESP, 16
    .set STATE_GUEST_REGS, 24
    .set STATE_RESULT, 40
    .set STATE_LEAVE, 44
    .set STATE_SELECTOR, 48
    .set STATE_SHIFT, 6 /* 64 bytes each */
    .set TEB_HOST_THREAD, 0xff0

    .text

/* uint32_t sxw_gate_enter(uint32_t entry, uint32_t guest_esp, gate_state *state)
 * guest_esp points at [return address = back32][arguments...]. */
    .globl sxw_gate_enter
    .type sxw_gate_enter, @function
sxw_gate_enter:
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    sub $8, %rsp
    mov %rsp, STATE_HOST_RSP(%rdx)
    movl $0x2b, %eax
    mov %eax, %ds
    mov %eax, %es
    movzwl STATE_SELECTOR(%rdx), %eax
    mov %eax, %fs
    mov %edi, %edi
    mov %esi, %esp
    pushq $0x23
    push %rdi
    lretq
    .size sxw_gate_enter, . - sxw_gate_enter

/* %r12 = this thread's gate state, from the index in its TEB. */
    .macro find_state
    movl %fs:TEB_HOST_THREAD, %r12d
    shl $STATE_SHIFT, %r12
    lea sxw_gates(%rip), %r11
    add %r11, %r12
    .endm

/* Reached from back32 when the guest function returns: eax is its result. */
    .globl sxw_gate_return
    .type sxw_gate_return, @function
sxw_gate_return:
    mov %esp, %esp /* the upper half is undefined after 32-bit code */
    find_state
    mov %eax, STATE_RESULT(%r12)
sxw_gate_leave:
    call restore_host_fs
    movl STATE_RESULT(%r12), %eax
    mov STATE_HOST_RSP(%r12), %rsp
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
    .size sxw_gate_return, . - sxw_gate_return

/* Reached from service32: the guest stack holds
 * [return address][service][arguments]. */
    .globl sxw_gate_service
    .type sxw_gate_service, @function
sxw_gate_service:
    mov %esp, %eax
    find_state
    mov %rax, STATE_GUEST_ESP(%r12)
    mov %ebx, STATE_GUEST_REGS + 0(%r12)
    mov %ebp, STATE_GUEST_REGS + 4(%r12)
    mov %esi, STATE_GUEST_REGS + 8(%r12)
    mov %edi, STATE_GUEST_REGS + 12(%r12)
    mov STATE_HOST_RSP(%r12), %rsp
    call restore_host_fs
    mov %r12, %rdi
    call sxw_host_service
    mov %eax, STATE_RESULT(%r12)
    cmpl $0, STATE_LEAVE(%r12)
    jne sxw_gate_leave
    movl $0x2b, %eax
    mov %eax, %ds
    mov %eax, %es
    movzwl STATE_SELECTOR(%r12), %eax
    mov %eax, %fs
    mov STATE_GUEST_REGS + 0(%r12), %ebx
    mov STATE_GUEST_REGS + 4(%r12), %ebp
    mov STATE_GUEST_REGS + 8(%r12), %esi
    mov STATE_GUEST_REGS + 12(%r12), %edi
    mov sxw_svc_ret32(%rip), %r11
    mov STATE_GUEST_ESP(%r12), %rsp
    movl STATE_RESULT(%r12), %eax
    pushq $0x23
    push %r11
    lretq
    .size sxw_gate_service, . - sxw_gate_service

/* Null FS, then this host thread's glibc FS base back with
 * arch_prctl(ARCH_SET_FS). Data selectors go back to null, as Linux runs
 * 64-bit code. Needs %r12; clobbers rax, rcx, rdx, rsi, rdi, r11. */
    .type restore_host_fs, @function
restore_host_fs:
    xor %eax, %eax
    mov %eax, %fs
    mov %eax, %ds
    mov %eax, %es
    movl $158, %eax
    movl $0x1002, %edi
    mov STATE_HOST_FSBASE(%r12), %rsi
    syscall
    ret
    .size restore_host_fs, . - restore_host_fs

    .section .note.GNU-stack, "", @progbits
