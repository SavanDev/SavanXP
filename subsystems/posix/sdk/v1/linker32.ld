/* Static ELF32 images occupy the private user window (0x08000000..0x09000000).
 * The loader maps PT_LOAD pages with their ELF write permissions, leaving
 * unused addresses and a guard below the 64 KiB stack absent. The base stays
 * above the kernel's low identity mappings. */
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