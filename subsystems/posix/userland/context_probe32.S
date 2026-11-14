/* Non-cooperating context stress: only UPTIME_MS syscalls, no yield/sleep.
 * Each worker owns distinct x87/SSE/rounding/GPR/stack patterns across PIT IRQs. */
.text
.global context_probe32
.type context_probe32, @function
context_probe32:
    pushl %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    subl $208, %esp
    movl 228(%esp), %eax
    movl %eax, 0(%esp)
    movl 232(%esp), %eax
    movl %eax, 4(%esp)
    fnstcw 8(%esp)
    cmpw $0x037f, 8(%esp)
    jne .Linitial_bad
    stmxcsr 12(%esp)
    cmpl $0x1f80, 12(%esp)
    jne .Linitial_bad
    movl 0(%esp), %eax
    imull $0x11111111, %eax
    movl %eax, 16(%esp)
    movd %eax, %xmm0
    pshufd $0, %xmm0, %xmm0
    movdqa %xmm0, %xmm1
    movdqa %xmm0, %xmm2
    movdqa %xmm0, %xmm3
    movdqa %xmm0, %xmm4
    movdqa %xmm0, %xmm5
    movdqa %xmm0, %xmm6
    movdqa %xmm0, %xmm7
    movl 0(%esp), %eax
    shll $10, %eax
    orl $0x037f, %eax
    movw %ax, 8(%esp)
    fldcw 8(%esp)
    movl 0(%esp), %eax
    shll $13, %eax
    orl $0x1f80, %eax
    movl %eax, 12(%esp)
    ldmxcsr 12(%esp)
    fildl 0(%esp)
    // Give the workers different FS/GS selectors without changing DS/ES.
    cmpl $1, 0(%esp)
    jne .Lsecond_segments
    movw $0x1b, %ax
    mov %ax, %fs
    xorl %eax, %eax
    mov %ax, %gs
    jmp .Lsegments_set
.Lsecond_segments:
    xorl %eax, %eax
    mov %ax, %fs
    movw $0x1b, %ax
    mov %ax, %gs
.Lsegments_set:
    movl $0xcafebabe, 32(%esp)
    movl 16(%esp), %ebx
    movl $0x51ab51ab, %ebp
    movl $0x62bc62bc, %esi
    movl $0x73cd73cd, %edi
    movl $0x95ef95ef, %edx
.Lloop:
    movl $0x84de84de, %ecx
    movl $9, %eax
    stc
    std
    int $0x80
    movl %eax, 36(%esp)
    pushfl
    popl %eax
    cld
    andl $0x401, %eax
    cmpl $0x401, %eax
    jne .Lregister_bad
    cmpl 16(%esp), %ebx
    jne .Lregister_bad
    cmpl $0x51ab51ab, %ebp
    jne .Lregister_bad
    cmpl $0x62bc62bc, %esi
    jne .Lregister_bad
    cmpl $0x73cd73cd, %edi
    jne .Lregister_bad
    cmpl $0x84de84de, %ecx
    jne .Lregister_bad
    cmpl $0x95ef95ef, %edx
    jne .Lregister_bad
    cmpl $0xcafebabe, 32(%esp)
    jne .Lstack_bad
    movdqu %xmm0, 64(%esp)
    movdqu %xmm1, 80(%esp)
    movdqu %xmm2, 96(%esp)
    movdqu %xmm3, 112(%esp)
    movdqu %xmm4, 128(%esp)
    movdqu %xmm5, 144(%esp)
    movdqu %xmm6, 160(%esp)
    movdqu %xmm7, 176(%esp)
    xorl %ecx, %ecx
.Lcheck_xmm:
    movl 64(%esp,%ecx,4), %eax
    cmpl 16(%esp), %eax
    jne .Lxmm_bad
    incl %ecx
    cmpl $32, %ecx
    jne .Lcheck_xmm
    xorl %eax, %eax
    mov %ds, %ax
    cmpw $0x1b, %ax
    jne .Lregister_bad
    mov %es, %ax
    cmpw $0x1b, %ax
    jne .Lregister_bad
    mov %fs, %ax
    cmpl $1, 0(%esp)
    jne .Lcheck_second_segments
    cmpw $0x1b, %ax
    jne .Lregister_bad
    mov %gs, %ax
    testw %ax, %ax
    jne .Lregister_bad
    jmp .Lsegments_ok
.Lcheck_second_segments:
    testw %ax, %ax
    jne .Lregister_bad
    mov %gs, %ax
    cmpw $0x1b, %ax
    jne .Lregister_bad
.Lsegments_ok:
    fnstcw 20(%esp)
    movw 20(%esp), %ax
    cmpw 8(%esp), %ax
    jne .Lcontrol_bad
    stmxcsr 24(%esp)
    movl 24(%esp), %eax
    cmpl 12(%esp), %eax
    jne .Lmxcsr_bad
    fstl 48(%esp)
    cmpl $0, 48(%esp)
    jne .Lx87_bad
    movl 0(%esp), %eax
    addl $1022, %eax
    shll $20, %eax
    cmpl 52(%esp), %eax
    jne .Lx87_bad
    movl 36(%esp), %eax
    cmpl 4(%esp), %eax
    jb .Lloop
    xorl %eax, %eax
    jmp .Lfinish
.Linitial_bad:
    movl $1, %eax
    jmp .Lfinish
.Lregister_bad:
    movl $2, %eax
    jmp .Lfinish
.Lxmm_bad:
    movl $3, %eax
    jmp .Lfinish
.Lcontrol_bad:
    movl $4, %eax
    jmp .Lfinish
.Lmxcsr_bad:
    movl $5, %eax
    jmp .Lfinish
.Lx87_bad:
    movl $6, %eax
    jmp .Lfinish
.Lstack_bad:
    movl $7, %eax
.Lfinish:
    cld
    pushl %eax
    movw $0x1b, %ax
    mov %ax, %fs
    mov %ax, %gs
    popl %eax
    fninit
    movl $0x1f80, 12(%esp)
    ldmxcsr 12(%esp)
    addl $208, %esp
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
.size context_probe32, . - context_probe32
.section .note.GNU-stack,"",@progbits
