#pragma once

#include "kernel/block.hpp"

namespace lifecycle {
struct Subject;
}

// Driver moderno de virtio-blk sobre virtio_pci. Sigue el mismo contrato que
// ata::driver(): se registra en block:: y, si el probe PCI no encuentra el
// dispositivo (maquina sin --virtio), su enumerate() no agrega devices.
namespace virtio_blk {

const block::Driver& driver();
/* Its start and stop, for `lifecycle-selftest=virtio-blk`
 * (docs/kernel/DRIVER_LIFECYCLE.md). */
const lifecycle::Subject& lifecycle_subject();

} // namespace virtio_blk
