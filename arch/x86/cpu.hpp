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

/* Bucle de parada del kernel. Lo usan el final de EXIT y las excepciones:
 * el proceso salio, no hay planificador todavia. */
[[noreturn]] void halt_forever();

}

}