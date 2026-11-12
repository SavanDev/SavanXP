/*
 * gate_x86_64.S -- The 64/32-bit gate of the host harness (Linux x86-64).
 *
 * Linux's own selectors: 0x23 is 32-bit user code, 0x2b user data, 0x33
 * 64-bit user code. FS holds an LDT selector whose base is the guest's TEB
 * while guest code runs, and glibc's FS base is put back before any C code
 * runs on the host side.
 *
 * The 32-bit halves of each transition live in a trampoline page below
 * 4 GiB that harness.c fills (a far jump cannot reach this file's
 * addresses from 32-bit code): back32 and service32 far-jump to 64-bit
 * landings that jump here, and svc_ret32 is the guest's `ret 8`.
 *
 * Single thread only: the saved state is in globals.
 */
    .text

/* uint32_t sxw_gate_enter(uint32_t entry, uint32_t guest_esp)
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
    mov %rsp, sxw_host_rsp(%rip)
    movl $0x2b, %eax
    mov %eax, %ds
    mov %eax, %es
    movzwl sxw_teb_selector(%rip), %eax
    mov %eax, %fs
    mov %edi, %edi
    mov %esi, %esp
    pushq $0x23
    push %rdi
    lretq
    .size sxw_gate_enter, . - sxw_gate_enter

/* Reached from back32 when the guest function returns: eax is its result. */
    .globl sxw_gate_return
    .type sxw_gate_return, @function
sxw_gate_return:
    mov %esp, %esp /* the upper half is undefined after 32-bit code */
    mov %eax, sxw_result(%rip)
sxw_gate_leave:
    call restore_host_fs
    mov sxw_host_rsp(%rip), %rsp
    add $8, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    movl sxw_result(%rip), %eax
    ret
    .size sxw_gate_return, . - sxw_gate_return

/* Reached from service32: the guest stack holds
 * [return address][service][arguments]. */
    .globl sxw_gate_service
    .type sxw_gate_service, @function
sxw_gate_service:
    mov %esp, %eax
    mov %rax, sxw_guest_esp(%rip)
    mov %ebx, sxw_guest_regs + 0(%rip)
    mov %ebp, sxw_guest_regs + 4(%rip)
    mov %esi, sxw_guest_regs + 8(%rip)
    mov %edi, sxw_guest_regs + 12(%rip)
    mov sxw_host_rsp(%rip), %rsp
    call restore_host_fs
    mov sxw_guest_esp(%rip), %rdi
    call sxw_host_service
    mov %eax, sxw_result(%rip)
    cmpl $0, sxw_exit_requested(%rip)
    jne sxw_gate_leave
    movl $0x2b, %eax
    mov %eax, %ds
    mov %eax, %es
    movzwl sxw_teb_selector(%rip), %eax
    mov %eax, %fs
    mov sxw_guest_regs + 0(%rip), %ebx
    mov sxw_guest_regs + 4(%rip), %ebp
    mov sxw_guest_regs + 8(%rip), %esi
    mov sxw_guest_regs + 12(%rip), %edi
    mov sxw_svc_ret32(%rip), %r11
    mov sxw_guest_esp(%rip), %rsp
    movl sxw_result(%rip), %eax
    pushq $0x23
    push %r11
    lretq
    .size sxw_gate_service, . - sxw_gate_service

/* Null FS, then glibc's FS base back with arch_prctl(ARCH_SET_FS). Data
 * selectors go back to null, as Linux runs 64-bit code. Clobbers rax, rcx,
 * rdx, rsi, rdi, r11. */
    .type restore_host_fs, @function
restore_host_fs:
    xor %eax, %eax
    mov %eax, %fs
    mov %eax, %ds
    mov %eax, %es
    movl $158, %eax
    movl $0x1002, %edi
    mov sxw_host_fsbase(%rip), %rsi
    syscall
    ret
    .size restore_host_fs, . - restore_host_fs

    .section .note.GNU-stack, "", @progbits
