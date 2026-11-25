#pragma once

#include "kernel/nic.hpp"

namespace lifecycle {
struct Subject;
}

// Driver moderno de virtio-net sobre virtio_pci. Sigue el mismo contrato que
// rtl8139::driver(): se registra en nic:: y, si el probe PCI no encuentra el
// dispositivo (maquina sin --virtio), su probe() devuelve false sin tocar
// nada mas.
namespace virtio_net {

const nic::Driver& driver();
/* Its start and stop, for `lifecycle-selftest=virtio-net`
 * (docs/kernel/DRIVER_LIFECYCLE.md). */
const lifecycle::Subject& lifecycle_subject();

} // namespace virtio_net
