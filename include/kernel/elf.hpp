#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/vmm.hpp"

namespace elf {

// e_ident[EI_OSABI] (byte 7 del ELF) con el que se marca un binario del
// subsistema nativo (Haxe). Los binarios posix usan 0 (System V). El build del
// nativo (subsystems/native/build.ps1) estampa este valor; el espejo en
// userland vive en subsystems/native/sdk/include/savanxp_native.h.
constexpr uint8_t kOsAbiNative = 0x53; // 'S' de SavanXP

struct LoadResult {
    uint64_t entry_point;
    uint64_t stack_pointer;
    uint8_t os_abi; // e_ident[EI_OSABI] de la imagen cargada
    // Argumentos que de verdad entraron en el stack inicial. Puede ser menor
    // que el argc pedido si el argv no entraba; el proceso tiene que arrancar
    // con ESTE, no con el original.
    int accepted_argc;
};

// Motivo de fallo de la carga. Existe para que el llamador pueda distinguir
// "el ELF esta mal" de "no habia memoria": colapsar ambos en un bool hacia el
// syscall convertia un ENOMEM en un ENOENT enganoso.
enum class LoadFailure : uint8_t {
    none = 0,
    bad_header,   // magia/clase/tipo/maquina invalidos, o phdrs fuera de la imagen
    truncated,    // un PT_LOAD apunta mas alla del final de la imagen
    out_of_memory // no se pudo reservar/mapear una pagina del segmento o del stack
};

const char* load_failure_string(LoadFailure failure);

bool load_user_image(
    const void* image,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result,
    LoadFailure& failure
);

} // namespace elf
