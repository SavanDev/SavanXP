// Static native applications enter their own runtime without a POSIX loader.
.text
.global _start
_start:
    cld
    andl $-16, %esp
    movl %edx, __stack_chk_guard
    subl $8, %esp
    pushl %esi
    pushl %edi
    call main
    movl %eax, %edi
    movl $7, %eax
    int $0x80
    ud2
.section .note.GNU-stack,"",@progbits
