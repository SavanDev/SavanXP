.text
.global _start

_start:
    # Bandera de direccion en 0: memcpy/memset usan instrucciones de string, que
    # con DF=1 caminan hacia atras. El kernel ya arranca los procesos con
    # rflags=0x202, pero dejarlo explicito ata la precondicion al mismo lado que
    # la depende.
    cld
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
