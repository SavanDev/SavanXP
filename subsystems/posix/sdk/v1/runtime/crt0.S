.text
.global _start

_start:
    # String instructions in the runtime require DF to be clear.
    cld
    # The kernel supplies argc/argv in EDI/ESI, the guard in EDX,
    # PT_INTERP in ECX and the image header in EBP. Establish the GOT without
    # reading any unrelocated pointer before entering the embedded loader.
    andl $-16, %esp
#if defined(__PIC__)
    call 2f
2:  popl %ebx
    addl $_GLOBAL_OFFSET_TABLE_+(.-2b), %ebx
    movl %edx, __stack_chk_guard@GOTOFF(%ebx)
    movl %ecx, sx_interpreter_path@GOTOFF(%ebx)
    movl %ebp, sx_image_base@GOTOFF(%ebx)
    subl $12, %esp
    pushl %ebp
    call sx_bootstrap_relative
    addl $16, %esp
    testl %eax, %eax
    jz 3f
    movl $126, %edi
    movl $7, %eax
    int $0x80
3:
#else
    movl %edx, __stack_chk_guard
    movl %ecx, sx_interpreter_path
    movl %ebp, sx_image_base
#endif
    # Push two arguments right to left and add eight bytes of padding to
    # preserve the 16-byte call-site alignment.
    subl $8, %esp
    pushl %esi
    pushl %edi
    call sx_start_dynamic
    addl $16, %esp
    subl $8, %esp
    pushl %esi
    pushl %edi
    call main
    movl %eax, %edi
    movl $7, %eax
    int $0x80
1:
    hlt
    jmp 1b
