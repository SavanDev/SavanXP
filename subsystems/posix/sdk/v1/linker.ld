ENTRY(_start)

SECTIONS
{
    /* The ELF and program headers go in the first PT_LOAD, at 0x400000, and
     * the text starts on the next page. The kernel hands the process the
     * start of its first PT_LOAD as the address of its ELF header (r8, read
     * by crt0 into sx_image_base); that is only true when the headers are
     * mapped, and the runtime reads PT_TLS through it (runtime/pthread.c).
     * lld puts the headers in a segment only when they fit on the page of
     * the first section, so the text follows them on that page. */
    . = 0x400000 + SIZEOF_HEADERS;

    .text : {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    /* Thread-local storage: the PT_TLS image each thread's block is copied
     * from (runtime/pthread.c). The location is page aligned so .rodata's
     * page stays read-only; the section itself only needs its own alignment,
     * which is what every thread's block is aligned to. */
    . = ALIGN(0x1000);
    .tdata : {
        *(.tdata .tdata.*)
    }

    .tbss : {
        *(.tbss .tbss.*)
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
