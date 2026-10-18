/* Puestos de entrada de 32 bits. El gate de int $0x80 es el mismo numero y el
 * mismo atributo (presente, DPL 3, interrupt gate) que en arch/x86_64, asi que
 * el lado del SDK que usa "int $0x80" con el numero en eax y los argumentos en
 * edi/esi/edx no cambia al compilarse para i386. */

.section .text

/* int $0x80 desde ring 3: el processor empuja ss, esp, eflags, cs y eip y
 * cambia a la pila de ring 0 (TSS.esp0). Encima el stub apila pusha, los dos
 * segmentos de datos (que el processor no salva) y un puntero al marco, y lo
 * pasa a i386_handle_syscall, que escribe el resultado en eax. Volver es
 * deshacer exactamente eso y ejecutar iret de vuelta al proceso.
 *
 * Ojo con el orden del marco: pusha empuja eax, ecx, edx, ebx, esp, ebp, esi
 * y edi, asi que de menor a mayor direccion quedan edi primero; el puntero que
 * pasa el stub es la palabra del propio "push %esp". Lo que apilo el processor
 * viene detras de los registros, y a partir de ahi el eje de esta lista de
 * palabras ya no es el del marco de SyscallFrame. */
.global i386_syscall_entry
.type i386_syscall_entry, @function
i386_syscall_entry:
    cld
    pusha
    push %ds
    push %es
    push %esp
    call i386_handle_syscall
    /* La palabra del puntero quedo por debajo de los segmentos (se empuja
     * despues), asi que se descarta antes de los pops: sin esto el primer
     * pop %es carga ese puntero como selector y el processor responde #GP. */
    add $4, %esp
    pop %es
    pop %ds
    popa
    /* Sin suma antes del iret: esas 20 palabras (ss, esp, eflags, cs y eip)
     * las saca el propio iret, igual que iretq en x86_64. */
    iret
.size i386_syscall_entry, . - i386_syscall_entry

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
    movl (%esp), %eax       /* el numero de IRQ */
    pusha
    push %ds
    push %es
    /* El manejador solo necesita el numero de IRQ. El marco se deshace con los
     * pops y los dos add siguientes: dejar una palabra de mas hacia que el
     * pop %es lea un valor de pila como selector y acabe en #GP. */
    subl $4, %esp           /* relleno: elabi de 32 bits pide 16 en la llamada */
    pushl %eax              /* unico argumento: el numero de IRQ */
    call i386_handle_irq
    addl $8, %esp           /* relleno y argumento */
    pop %es
    pop %ds
    popa
    addl $4, %esp           /* el numero de IRQ apilado por la entrada */
    /* Sin suma antes del iret: el marco del processor (eflags, cs y eip, mas
     * ss y esp si hubo cambio de privilegio) lo saca el propio iret, igual que
     * iretq en x86_64 y que el iret del marco de excepcion de arriba. */
    iret

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