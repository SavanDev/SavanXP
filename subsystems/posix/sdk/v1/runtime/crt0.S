.text
.global _start

_start:
    # Alinear el stack a 16 bytes antes del primer call: el ABI System V exige
    # rsp alineado a 16 en cada call (rsp % 16 == 0), y SSE (doubles de Haxe en
    # el subsistema nativo) puede emitir movaps que fallan con #GP si no lo esta.
    andq $-16, %rsp
    call main
    movq %rax, %rdi
    movq $7, %rax
    int $0x80
1:
    hlt
    jmp 1b
