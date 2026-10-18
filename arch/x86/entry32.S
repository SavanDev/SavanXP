/*
 * Entrada Multiboot2 del kernel i386 (docs/REAL_HARDWARE.md).
 *
 * El estado al entrar lo fija GRUB2: modo protegido de 32 bits, paginacion
 * apagada, interrupciones apagadas, EAX = firma 0x36D76289 y EBX = direccion
 * fisica de la informacion Multiboot2. Aqui solo se monta una pila propia,
 * se carga una GDT plana (la de GRUB no tiene por que ser plana y no es
 * nuestra) y se salta al bootstrap en C++.
 *
 * La cabecera Multiboot2 tiene que estar en los primeros 32 KiB del fichero;
 * linker32.ld mantiene .multiboot2 al principio de .text, que arranca en la
 * primera pagina del ELF.
 */

.section .multiboot2, "a"
.align 8
multiboot2_header:
    .long 0xE85250D6                       /* firma */
    .long 0                                /* arquitectura: i386, modo protegido */
    .long multiboot2_header_end - multiboot2_header
    .long 0x17ADAF12                       /* -(firma + longitud 24), precalculado */
    /* Etiqueta de fin: tipo 0, tamano 8. */
    .word 0
    .word 0
    .long 8
multiboot2_header_end:

.section .bss
.align 16
boot_stack_bottom:
    .skip 16384
boot_stack_top:

.section .text
.global _start
.type _start, @function
_start:
    cli
    cld
    movl $boot_stack_top, %esp
    andl $-16, %esp

    lgdt gdt_descriptor
    ljmpl $0x08, $1f

1:
    /* %cx, no %ax: EAX lleva la firma de Multiboot2 y no se toca hasta el
     * push de la llamada. */
    movw $0x10, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %fs
    movw %cx, %gs
    movw %cx, %ss

    /* cdecl (magic, info) con la pila alineada a 16 en el call: primero el
     * relleno, despues los argumentos de derecha a izquierda. El bootstrap no
     * vuelve nunca, pero si volviera se queda en el bucle de parada. */
    subl $8, %esp
    pushl %ebx
    pushl %eax
    call savanxp_i386_bootstrap

2:
    hlt
    jmp 2b
.size _start, . - _start

.align 8
gdt_start:
    .quad 0x0000000000000000               /* nula */
    .quad 0x00CF9A000000FFFF               /* codigo: base 0, limite 4 GiB, DPL 0, 32 bits */
    .quad 0x00CF92000000FFFF               /* datos: base 0, limite 4 GiB, DPL 0 */
gdt_end:

gdt_descriptor:
    .word gdt_end - gdt_start - 1
    .long gdt_start
