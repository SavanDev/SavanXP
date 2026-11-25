#pragma once

// virtio-rng (virtio 1.x section 5.4): the host's entropy, for the kernel
// generator (docs/kernel/RANDOM.md). One queue; the driver keeps one
// device-writable buffer posted, and poll() hands what came back to random::
// and posts it again. Polled from the BSP tick, no interrupt.
namespace lifecycle {
struct Subject;
}

namespace virtio_rng {

// Probes, sets the queue up and posts the first request. False when there is
// no virtio-rng device or it could not be brought up.
bool initialize();
bool present();
// Takes a filled buffer, if any, into the entropy pool and posts it again.
// True when bytes arrived.
bool poll();
/* Its start and stop, for `lifecycle-selftest=virtio-rng`
 * (docs/kernel/DRIVER_LIFECYCLE.md). */
const lifecycle::Subject& lifecycle_subject();

} // namespace virtio_rng
