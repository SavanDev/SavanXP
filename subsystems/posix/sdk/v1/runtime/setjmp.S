/* i386 SysV callee-saved registers, caller stack and return address. */
.text
.global sx_setjmp
.global sx_longjmp

sx_setjmp:
    movl 4(%esp), %edx
    movl %ebx, 0(%edx)
    movl %esi, 4(%edx)
    movl %edi, 8(%edx)
    movl %ebp, 12(%edx)
    leal 4(%esp), %eax
    movl %eax, 16(%edx)
    movl (%esp), %eax
    movl %eax, 20(%edx)
    xorl %eax, %eax
    ret

sx_longjmp:
    movl 4(%esp), %edx
    movl 8(%esp), %eax
    testl %eax, %eax
    jne 1f
    movl $1, %eax
1:
    movl 0(%edx), %ebx
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp
    jmp *20(%edx)
.section .note.GNU-stack,"",@progbits
