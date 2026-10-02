#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/vmm.hpp"
#include "abi/savanxp_native_abi.h"

namespace elf {

// e_ident[EI_OSABI] (byte 7 del ELF) con el que se marca un binario del
// subsistema nativo. El valor vive en el contrato ABI neutral para que el
// loader, el generator de SXE y el standalone build no lo dupliquen.
constexpr uint8_t kOsAbiNative = SXN_ELF_OSABI_NATIVE;

struct LoadResult {
    uint64_t entry_point;
    uint64_t stack_pointer;
    uint8_t os_abi; // e_ident[EI_OSABI] de la imagen cargada
    // Argumentos que de verdad entraron en el stack inicial. Puede ser menor
    // que el argc pedido si el argv no entraba; el proceso tiene que arrancar
    // con ESTE, no con el original.
    int accepted_argc;
    // Ruta del intérprete (PT_INTERP) ya copiada al stack inicial, o 0 si la
    // imagen no declara uno. Es lo que crt0 lee de rcx para saber que tiene que
    // arrancar el cargador antes de llegar a main.
    uint64_t interpreter_address;
    // Dirección donde quedo la cabecera ELF de la imagen: el inicio de la
    // pagina del primer PT_LOAD. El kernel lo sabe porque acaba de mapearlo.
    //
    // Sin esto, el cargador tiene que adivinarlo retrocediendo pagina a pagina
    // desde una funcion suya buscando la magia, y esa cuenta supone que TODA
    // pagina entre la funcion y la base esta mapeada. No lo esta: un enlazador
    // puede dejar un hueco entre dos PT_LOAD, y con seltest --cuyo texto empezo
    // en 0xc000 y su primer segmento termina en 0xaff3-- la pagina 0xb000 no
    // existe. El escaneo leia la pagina cero y el proceso moria antes de main.
    uint64_t image_base;
    // Ver abajo: indice del segmento que fallo, o 0xffff.
    uint16_t fail_index;
};

// Motivo de fallo de la carga. Existe para que el llamador pueda distinguir
// "el ELF esta mal" de "no habia memoria": colapsar ambos en un bool hacia el
// syscall convertia un ENOMEM en un ENOENT enganoso.
enum class LoadFailure : uint8_t {
    none = 0,
    bad_header,   // magia/clase/tipo/maquina invalidos, o phdrs fuera de la imagen
    bad_segment,  // PT_LOAD inconsistente, solapado, fuera de usuario o sin W^X
    truncated,    // un PT_LOAD apunta mas alla del final de la imagen
    out_of_memory // no se pudo reservar/mapear una pagina del segmento o del stack
};

// Donde fallo dentro de la imagen: 0xffff es cabecera o final, otro valor es
// el indice del program header que no gusto. Sin esto, "bad elf segment" no
// dice cual de los N segmentos es.

const char* load_failure_string(LoadFailure failure);

// Lee `size` bytes de la imagen desde `offset` hacia `destination`. Devuelve
// false si no se pudo leer todo.
//
// El loader consume la imagen por aca y no como un bloque de memoria: asi una
// imagen en disco se lee DIRECTO sobre las paginas del proceso, sin una copia
// intermedia del ejecutable entero -- que ademas se pedia en paginas fisicamente
// contiguas, y eso es lo que hacia fallar un binario grande sobre memoria
// fragmentada.
using ImageReader = bool (*)(void* context, uint64_t offset, void* destination, size_t size);

bool load_user_image(
    ImageReader read_image,
    void* context,
    size_t size,
    vm::VmSpace& address_space,
    int argc,
    const char* const* argv,
    LoadResult& result,
    LoadFailure& failure
);

} // namespace elf
