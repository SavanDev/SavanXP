#pragma once

#include <stddef.h>
#include <stdint.h>

#include "kernel/block.hpp"

namespace partition {

// Driver de bloque particular: no habla con ningun hardware, rebana los devices
// que ya registraron los demas. Por eso su prioridad es la mas baja de todas y
// probe_all lo corre ultimo, cuando los discos fisicos y el ramdisk ya estan en
// la tabla. Cada particion entra como un device mas de block::, con su LBA 0
// propio, asi que sxfs:: (y manana fat::) montan una particion sin enterarse de
// que abajo hay un offset.
const block::Driver& driver();

struct Info {
    // Indice del device del que se recorto la particion.
    size_t parent_index;
    // Offset dentro del padre, en sectores.
    uint32_t start_lba;
    uint32_t sector_count;
    // Tipo del entry MBR, o 0 cuando la particion salio de una tabla GPT.
    uint8_t mbr_type;
    // EFI System Partition: type GUID C12A7328-... en GPT, tipo 0xEF en MBR.
    bool esp;
};

// false si el device no es una particion (un disco entero o el ramdisk). El
// instalador lo usa para no formatear el disco completo cuando el usuario
// eligio una particion, y para encontrar la ESP donde dejar el bootloader.
bool info(size_t device_index, Info& out);

} // namespace partition
