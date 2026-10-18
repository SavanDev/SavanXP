#pragma once

#include <stddef.h>
#include <stdint.h>

namespace i386 {

/* Marco que los stubs de interrupts32.S dejan tras apilar los registros. El
 * puntero que recibe el manejador es la palabra del propio "push %esp", de ahi
 * el primer campo; despues vienen los dos segmentos y los ocho registros de
 * pusha, en el orden inverso al de empuje (pusha empuja eax, ecx, edx, ebx,
 * esp, ebp, esi y edi, y la pila crece hacia abajo, asi que de menor a mayor
 * direccion quedan edi primero).
 *
 * Lo que apilo el processor (eip, cs, eflags y, si hubo cambio de privilegio,
 * esp y ss, precedido del codigo de error cuando el vector lo genera) empieza
 * justo detras de esta estructura: el informe de excepciones lo lee como
 * palabras sueltas a partir de frame + kSyscallFrameWords. */
struct SyscallFrame {
    uint32_t frame_pointer;
    uint32_t es;
    uint32_t ds;
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t pusha_esp;
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
};

constexpr size_t kSyscallFrameWords = 11;

namespace syscall {

/* Numeros que entiende este kernel, los mismos del SDK (savanxp/syscall.h):
 * solo los que el primer programa de usuario necesita. */
constexpr uint32_t kSysWrite = 1;
constexpr uint32_t kSysExit = 7;
constexpr uint32_t kSysGetPid = 30;
constexpr uint32_t kSysSystemInfo = 35;

constexpr uint32_t kStdoutFd = 1;
constexpr uint32_t kStderrFd = 2;
constexpr uint32_t kInitPid = 1;

/* Escribe el resultado en frame.eax y devuelve al proceso con iret. */
void handle(SyscallFrame& frame);

}

}