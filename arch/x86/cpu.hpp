#pragma once

#include <stdint.h>

namespace i386 {

/* Los selectores son los mismos que usa arch/x86_64 para que el codigo que
 * los menciona signifique lo mismo en las dos arquitecturas: el kernel vive
 * en 0x08/0x10, un proceso de usuario corre con 0x1b/0x23 y el TSS esta en
 * 0x28 (0x28 + 8 * indice de processor, como alli). */
namespace cpu {

constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint16_t kKernelDataSelector = 0x10;
constexpr uint16_t kUserDataSelector = 0x1b;
constexpr uint16_t kUserCodeSelector = 0x23;
constexpr uint16_t kTssSelector = 0x28;

constexpr uint32_t kSyscallVector = 0x80;

/* Instala la GDT completa (codigo y datos de ring 0, codigo y datos de ring 3
 * y el descriptor del TSS), la IDT con el gate de int $0x80 y enmascara el
 * PIC. La GDT del arranque en entry32.S solo sirve para pasar a modo
 * protegido; aqui ya se puede recargar porque los descriptores de ring 0 son
 * los mismos y CS sigue apuntando a 0x08. */
void initialize();

/* esp0/ss0 son la pila de ring 0 que el processor carga cuando un proceso de
 * usuario ejecuta int $0x80. */
void set_ring0_stack(uint32_t stack_top);

/* Tope de esa pila. El manejador de syscall lee el marco del proceso desde
 * ahi en vez de fiarse del puntero que empuja el stub (ver syscall.cpp). */
uint32_t ring0_stack_top();

/* Carga ds/es/fs/gs con el selector de datos de usuario (0x1b). Tiene que
 * vivir en un fichero .S: el ensamblador en linea de clang convierte
 * "mov %ax, %ds" en la forma de 32 bits, que no existe en modo 32. */
void load_user_segments();

/* CR0.EM=0, CR0.MP=1 y CR4.OSFXSR: el userland se compila con SSE/SSE2, asi
 * que sin esto la primera instruccion xmm de un proceso aborta. El kernel se
 * compila con -mno-sse y no toca el estado del FPU. */
void enable_fpu();

/* Bucle de parada del kernel con las interrupciones apagadas. Lo usan las
 * excepciones y los fallos: la maquina se para y lo dice. */
[[noreturn]] void halt_forever();

/* Espera ociosa con las interrupciones vivas. Todavia no hay planificador,
 * pero cuando el proceso unico termina el teclado (IRQ1) tiene que seguir
 * llegando: parar la maquina con cli lo apagaria. */
[[noreturn]] void idle_with_interrupts();

/* Tipo de los manejadores de IRQ: void(void), como en kernel/cpu.hpp. Los IRQ
 * 0..15 llegan por el PIC legacy, remapeado a 0x20..0x2F en initialize(). */
using IrqHandler = void (*)();

/* Registra el manejador de un IRQ y abre su puerta en la IDT. Si el IRQ ya
 * tenia manejador, este lo sustituye. */
void register_irq_handler(uint8_t irq, IrqHandler handler);

/* Quita la mascara del PIC para ese IRQ. Las interrupciones siguen apagadas
 * hasta que alguien las encienda con enable_interrupts(). */
void enable_irq(uint8_t irq);

/* Envia EOI al PIC. El kernel no reencola IRQ: quien recibe la interrupcion
 * la termina y la da por buena. */
void acknowledge_irq(uint8_t irq);

/* cli / sti. El kernel arranca con las interrupciones apagadas (no hay PIT) y
 * solo las enciende quien sabe lo que hace, como el teclado. */
void disable_interrupts();
void enable_interrupts();

}

}