/*
 * SavanXP - glue C++ freestanding del subsistema nativo, Fase 2.
 *
 * Provee lo minimo de runtime C++ que el codigo generado por reflaxe.CPP
 * necesita sin libstdc++: operator new/delete sobre el heap del runtime y el
 * stub de pure-virtual. Se compila con -fno-exceptions -fno-rtti
 * -fno-threadsafe-statics, asi que no hace falta soporte de unwinding ni
 * guards de estaticos.
 */
#include "savanxp_native.h"

#include <stddef.h>

namespace {

void* allocate_or_die(size_t size) {
    void* result = sxn_alloc(size);
    if (result == nullptr) {
        // Sin excepciones, new no puede devolver null: OOM es fatal y ruidoso.
        sxn_log("cxx: operator new sin memoria (heap nativo agotado)");
        sxn_exit(133);
    }
    return result;
}

} // namespace

void* operator new(size_t size) { return allocate_or_die(size); }
void* operator new[](size_t size) { return allocate_or_die(size); }
void operator delete(void* ptr) noexcept { sxn_free(ptr); }
void operator delete[](void* ptr) noexcept { sxn_free(ptr); }
void operator delete(void* ptr, size_t) noexcept { sxn_free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { sxn_free(ptr); }

extern "C" void __cxa_pure_virtual() {
    sxn_log("cxx: llamada a metodo virtual puro");
    sxn_exit(134);
}
