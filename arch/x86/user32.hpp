#pragma once

namespace i386 {

/* Carga una imagen ELF32 del volumen montado y entra en ella como ring 3.
 * La ventana de usuario (paging.hpp) y los descriptores con DPL 3 (cpu.hpp)
 * son lo que hace posible el salto; aqui solo se copia el programa, se arma
 * la pila inicial y se hace el iret. */
namespace user32 {

/* Lee la imagen de `path`, la carga en la ventana de usuario con el registro
 * inicial que crt0 espera (edi = argc, esi = argv) y no vuelve: el proceso
 * corre hasta que llama a exit, y entonces el kernel se para. */
[[noreturn]] void run(const char* path);

} // namespace user32

} // namespace i386