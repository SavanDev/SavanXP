/* Puestos de entrada de 32 bits. El gate de int $0x80 es el mismo numero y el
 * mismo atributo (presente, DPL 3, interrupt gate) que en arch/x86_64, asi que
 * el lado del SDK que usa "int $0x80" con el numero en eax y los argumentos en
 * edi/esi/edx no cambia al compilarse para i386. */

.section .text

/* Save all user registers before loading kernel selectors. Align a separate
 * call stack without assuming the interrupted kernel stack's alignment. */
.macro save_context
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    movw $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    movl %esp, %ebx
    andl $-16, %esp
    subl $12, %esp
    pushl %ebx
.endm

.global i386_syscall_entry
.type i386_syscall_entry, @function
i386_syscall_entry:
    cld
    pushl $0x80
    save_context
    call i386_handle_syscall
    movl %eax, %esp
    jmp restore_context
.size i386_syscall_entry, . - i386_syscall_entry

.global i386_resume
.type i386_resume, @function
i386_resume:
    movl 4(%esp), %esp
restore_context:
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    addl $4, %esp
    iret
.size i386_resume, . - i386_resume

/* Excepciones de CPU. El processor no apila el numero de vector, asi que hay
 * una entrada por vector que lo empuja y salta al marco comun. Un kernel sin
 * excepciones installadas que continuam es peor que uno que se para y lo
 * dice. */
.altmacro
.macro i386_exception_stub number
    .global i386_exception_\number
    .type i386_exception_\number, @function
i386_exception_\number:
    cld
    pushl $\number
    jmp i386_exception_common
.size i386_exception_\number, . - i386_exception_\number
.endm
.altmacro

i386_exception_stub 0
i386_exception_stub 1
i386_exception_stub 2
i386_exception_stub 3
i386_exception_stub 4
i386_exception_stub 5
i386_exception_stub 6
i386_exception_stub 7
i386_exception_stub 8
i386_exception_stub 9
i386_exception_stub 10
i386_exception_stub 11
i386_exception_stub 12
i386_exception_stub 13
i386_exception_stub 14
i386_exception_stub 15
i386_exception_stub 16
i386_exception_stub 17
i386_exception_stub 18
i386_exception_stub 19
i386_exception_stub 20
i386_exception_stub 21
i386_exception_stub 22
i386_exception_stub 23
i386_exception_stub 24
i386_exception_stub 25
i386_exception_stub 26
i386_exception_stub 27
i386_exception_stub 28
i386_exception_stub 29
i386_exception_stub 30
i386_exception_stub 31

/* Tabla de direcciones para la IDT: una por vector de excepcion. */
.global i386_exception_table
i386_exception_table:
    .long i386_exception_0
    .long i386_exception_1
    .long i386_exception_2
    .long i386_exception_3
    .long i386_exception_4
    .long i386_exception_5
    .long i386_exception_6
    .long i386_exception_7
    .long i386_exception_8
    .long i386_exception_9
    .long i386_exception_10
    .long i386_exception_11
    .long i386_exception_12
    .long i386_exception_13
    .long i386_exception_14
    .long i386_exception_15
    .long i386_exception_16
    .long i386_exception_17
    .long i386_exception_18
    .long i386_exception_19
    .long i386_exception_20
    .long i386_exception_21
    .long i386_exception_22
    .long i386_exception_23
    .long i386_exception_24
    .long i386_exception_25
    .long i386_exception_26
    .long i386_exception_27
    .long i386_exception_28
    .long i386_exception_29
    .long i386_exception_30
    .long i386_exception_31

/* Carga los selectores de datos de usuario en los cuatro registros de datos.
 * Va en un .S porque el ensamblador en linea de clang convierte "mov %ax, %ds"
 * en la forma de 32 bits, que no existe en modo 32 y acaba en #GP. Se llama
 * desde ring 0 justo antes del iret de entrada: cargar un selector de DPL 3
 * desde ring 0 esta permitido. */
.global i386_load_user_segments
.type i386_load_user_segments, @function
i386_load_user_segments:
    movw $0x1b, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    ret
.size i386_load_user_segments, . - i386_load_user_segments

/* IRQ del PIC legacy. Las 16 vectores (0x20..0x2F tras el remapeo) tienen su
 * propia entrada por el mismo motivo que las excepciones: el processor no
 * apila el numero. El marco comun salva los registros y los segmentos, llama a
 * i386_handle_irq (que despacha al driver y acusa al PIC) y devuelve con iret,
 * igual que hacia el proceso. */
.altmacro
.macro i386_irq_stub number
    .global i386_irq_\number
    .type i386_irq_\number, @function
i386_irq_\number:
    cld
    pushl $\number
    jmp i386_irq_common
.size i386_irq_\number, . - i386_irq_\number
.endm
.altmacro

i386_irq_stub 0
i386_irq_stub 1
i386_irq_stub 2
i386_irq_stub 3
i386_irq_stub 4
i386_irq_stub 5
i386_irq_stub 6
i386_irq_stub 7
i386_irq_stub 8
i386_irq_stub 9
i386_irq_stub 10
i386_irq_stub 11
i386_irq_stub 12
i386_irq_stub 13
i386_irq_stub 14
i386_irq_stub 15

.global i386_irq_table
i386_irq_table:
    .long i386_irq_0
    .long i386_irq_1
    .long i386_irq_2
    .long i386_irq_3
    .long i386_irq_4
    .long i386_irq_5
    .long i386_irq_6
    .long i386_irq_7
    .long i386_irq_8
    .long i386_irq_9
    .long i386_irq_10
    .long i386_irq_11
    .long i386_irq_12
    .long i386_irq_13
    .long i386_irq_14
    .long i386_irq_15

i386_irq_common:
    save_context
    call i386_handle_irq
    movl %eax, %esp
    jmp restore_context

/* Marco comun: el vector esta en la cima; se copia a eax antes de pusha
 * (pusha lo sobrescribiria) y se pasa como segundo argumento. Detras del
 * puntero al marco queda lo que apilo el processor: codigo de error si lo hay,
 * y despues eflags, cs y eip (mas esp y ss si hubo cambio de privilegio), de
 * menor a mayor direccion. */
i386_exception_common:
    movl (%esp), %eax       /* el vector */
    pusha
    push %ds
    push %es
    movl %esp, %edx         /* puntero al marco */
    subl $8, %esp           /* relleno de alineacion: va antes de los argumentos */
    pushl %eax              /* segundo argumento: se apila primero */
    pushl %edx              /* primer argumento */
    call i386_report_exception
2:
    hlt
    jmp 2b