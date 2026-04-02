.text
.global _start

_start:
    call main
    movq %rax, %rdi
    movq $7, %rax
    int $0x80
1:
    hlt
    jmp 1b
