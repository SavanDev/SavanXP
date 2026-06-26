/*
 * SavanXP - shim de entrada del subsistema nativo, Fase 0.
 *
 * reflaxe.CPP genera su propio `_main_.cpp` que incluye <memory> (modelo de
 * memoria GC-less con smart pointers de libstdc++). En freestanding -nostdlib
 * no tenemos libstdc++, asi que EXCLUIMOS ese archivo del build y proveemos
 * nuestra propia entrada, que llama directo a la funcion main generada por
 * Haxe. crt0.S llama a `main`, y al retornar dispara la syscall EXIT con el
 * codigo de retorno.
 */
#include "Main.h"

extern "C" int main() {
    _Main::Main_Fields_::main();
    return 0;
}
