/* Guion de enlace del kernel de i386 (docs/REAL_HARDWARE.md).

   A diferencia de x86_64, donde Limine carga el kernel en la mitad alta de un
   espacio ya paginado, el arranque de 32 bits todavia no tiene paginacion: el
   kernel vive en 1 MiB y se recorre con la identidad fisica que GRUB2
   construye con Multiboot2.

   La cabecera Multiboot2 tiene que caer en los primeros 32 KiB del archivo; el
   KEEP de .multiboot2 la fija al principio de .text, que es la primera seccion
   con contenido. */
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;

    .text : ALIGN(0x1000) {
        KEEP(*(.multiboot2))
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        *(.sdata2 .sdata2.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
        *(.sdata .sdata.*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(.sbss .sbss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.interp)
    }
}
