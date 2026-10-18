/* Igual que linker.ld, pero el destino i386 se enlaza dentro de la ventana de
 * usuario (0x08000000..0x09000000) que arch/x86/paging.cpp marca con el bit de
 * usuario en los PDEs. La base no es 0x400000 porque el kernel ya vive en el
 * mapa de identidad del primer GiB. */
ENTRY(_start)

SECTIONS
{
    . = 0x08000000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame*)
        *(.note .note.*)
        *(.comment)
    }
}