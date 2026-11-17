/* GRUB does not start APs. Copy this position-independent page to 0x7000,
 * then send INIT/SIPI. Only the three data words are patched, once for all
 * APs: a late AP always finds its own stack by its initial APIC ID. */
.section .rodata.ap_boot,"a",@progbits
.balign 16
.global i386_ap_boot_start, i386_ap_boot_end
.global i386_ap_boot_cr3, i386_ap_boot_stacks, i386_ap_boot_entry
.code16
i386_ap_boot_start:
    cli
    cld
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    lgdtl (0x7000 + .Lgdt_pointer - i386_ap_boot_start)
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    .byte 0x66, 0xea
    .long 0x7000 + .Lprotected - i386_ap_boot_start
    .word 0x08
.code32
.Lprotected:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $1, %eax
    cpuid
    shrl $24, %ebx
    movl (0x7000 + i386_ap_boot_stacks - i386_ap_boot_start), %esi
    movl (%esi,%ebx,4), %ebp
    testl %ebp, %ebp
    jz .Lhalt
    movl %cr4, %eax
    orl $0x10, %eax
    movl %eax, %cr4
    movl (0x7000 + i386_ap_boot_cr3 - i386_ap_boot_start), %eax
    movl %eax, %cr3
    movl %cr0, %eax
    wbinvd
    andl $0x9fffffff, %eax  /* INIT leaves CD/NW set; enable normal caches. */
    orl $0x80010000, %eax
    movl %eax, %cr0
    movl %ebp, %esp
    xorl %ebp, %ebp
    call *(0x7000 + i386_ap_boot_entry - i386_ap_boot_start)
.Lhalt:
    cli
    hlt
    jmp .Lhalt
.balign 8
.Lgdt:
    .quad 0
    .quad 0x00cf9a000000ffff
    .quad 0x00cf92000000ffff
.Lgdt_pointer:
    .word 23
    .long 0x7000 + .Lgdt - i386_ap_boot_start
.balign 4
i386_ap_boot_cr3: .long 0
i386_ap_boot_stacks: .long 0
i386_ap_boot_entry: .long 0
i386_ap_boot_end:
.code32
.section .note.GNU-stack,"",@progbits
