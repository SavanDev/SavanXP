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
};

bool load_user_image(
    const void* image,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result
);

} // namespace elf
