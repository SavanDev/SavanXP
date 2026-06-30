/*
 * SavanXP - SDK del subsistema nativo (Haxe), v0 / Fase 0.
 *
 * Este header es la semilla del ABI nativo. Hoy expone solo el minimo para el
 * "puntapie": una envoltura de syscalls sobre `int $0x80` y el primitivo
 * sx_hello() que usa el programa Haxe de validacion.
 *
 * IMPORTANTE: por ahora estas syscalls usan la MISMA convencion que el
 * subsistema posix (numero en rax, args en rdi/rsi/rdx/r10/r8, resultado en
 * rax). Un binario nativo lanzado por un padre posix corre con identidad posix,
 * asi que WRITE/EXIT funcionan tal cual. El dispatcher nativo
 * (subsystems/native/kernel/syscall_dispatch.inc) todavia responde ENOSYS: el
 * ABI propiamente nativo se disena en la Fase 2.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Marca e_ident[EI_OSABI] (byte 7 del ELF) con la que el kernel reconoce un
 * binario nativo y lo corre con identidad de subsistema "native". El build
 * (subsystems/native/build.ps1) estampa este valor en el ELF; el kernel lo lee
 * en elf.cpp y lo compara contra elf::kOsAbiNative (deben coincidir). */
#define SXN_ELF_OSABI_NATIVE 0x53 /* 'S' de SavanXP */

/* Numeros de syscall compartidos con posix mientras el ABI nativo no exista.
 * Espejo de subsystems/posix/sdk/v1/include/savanxp/syscall.h. */
#define SXN_SYS_WRITE 1
#define SXN_SYS_EXIT  7

/* Envolturas crudas de syscall (int $0x80). */
long sxn_syscall3(long number, long a, long b, long c);

/* Primitivos del runtime nativo usados por el codigo Haxe generado. */
long sxn_write(int fd, const char *buf, int len);
void sxn_exit(int code) __attribute__((noreturn));

/* Demo de la Fase 0: escribe un saludo por stdout. El string vive aca (en C)
 * para que el C++ generado por reflaxe.CPP no arrastre <string>/<iostream>. */
void sxn_hello(void);

#ifdef __cplusplus
}
#endif
