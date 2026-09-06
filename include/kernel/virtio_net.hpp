#pragma once

#include "kernel/nic.hpp"

// Driver moderno de virtio-net sobre virtio_pci. Sigue el mismo contrato que
// rtl8139::driver(): se registra en nic:: y, si el probe PCI no encuentra el
// dispositivo (maquina sin -Virtio), su probe() devuelve false sin tocar
// nada mas.
namespace virtio_net {

const nic::Driver& driver();

} // namespace virtio_net
