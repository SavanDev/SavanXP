.text

.global x86_64_syscall_entry
.global x86_64_timer_entry
.global x86_64_reschedule_entry
.global _ZN4arch6x86_6414resume_contextEPN7process12SavedContextEm

/*
 * Entrada con contexto: guarda los registros como un process::SavedContext,
 * llama al handler con el puntero y retoma el contexto que el handler devuelva,
 * que puede ser el de otro proceso sobre otra pila de kernel.
 *
 * El lock del kernel se toma antes del handler y se suelta DESPUES de
 * `movq %rax, %rsp`, ya sobre la pila del proceso que se retoma. Soltarlo antes,
 * todavia sobre la pila del saliente, dejaria que otro core lo retome: sus
 * syscalls aterrizarian en el tope de esa misma pila, encima de los frames que
 * este core todavia esta usando.
 *
 * Con IF=0 de punta a punta (interrupt gates), asi que tomar y soltar el lock
 * aca no abre ninguna ventana en este core. Clobberear los registros volatiles
 * en las llamadas no importa: todos salen del frame.
 */
.macro CONTEXT_ENTRY name, handler
\name:
    cld
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    subq $8, %rsp
    call savanxp_kernel_lock_acquire
    leaq 8(%rsp), %rdi
    call \handler
    movq %rax, %rsp
    subq $8, %rsp
    call savanxp_kernel_lock_release
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
.endm

CONTEXT_ENTRY x86_64_syscall_entry, savanxp_handle_syscall
CONTEXT_ENTRY x86_64_timer_entry, savanxp_handle_timer_interrupt
CONTEXT_ENTRY x86_64_reschedule_entry, savanxp_handle_reschedule_ipi

/*
 * resume_context(context, cr3): salir del kernel hacia un contexto sin haber
 * entrado por uno de los stubs de arriba (el primer proceso de cada core, o la
 * vuelta de terminate_current). Se llama siempre con el lock del kernel tomado,
 * y lo suelta igual que ellos: recien sobre la pila nueva.
 */
_ZN4arch6x86_6414resume_contextEPN7process12SavedContextEm:
    movq %rsi, %cr3
    movq %rdi, %rsp
    subq $8, %rsp
    call savanxp_kernel_lock_release
    addq $8, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    iretq
