#pragma once

#include "kernel/block.hpp"

// Driver moderno de virtio-blk sobre virtio_pci. Sigue el mismo contrato que
// ata::driver(): se registra en block:: y, si el probe PCI no encuentra el
// dispositivo (maquina sin -Virtio), su enumerate() no agrega devices.
namespace virtio_blk {

const block::Driver& driver();

} // namespace virtio_blk
