# virtio: what the transport asks of a driver here

Every paravirtualized device SavanXP drives — `virtio-net`, `virtio-blk`,
`virtio-gpu`, `virtio-input`, `virtio-sound` — sits on the same modern
virtio-pci transport, implemented once in
[`kernel/virtio_pci.cpp`](../kernel/virtio_pci.cpp). This document records the
rules of that transport that are not visible from the code that uses it, and
that cost a debugging session each time they are rediscovered.

## A 64-bit field is touched in two 32-bit halves, low first

The virtio specification (1.x, §4.1.3.1) only guarantees that a device answers
accesses of the *natural width* of each field: 8 bits for a byte, 16 for a
word, 32 for a doubleword. For the 64-bit fields there is no natural width to
promise — the specification says the driver may access the two 32-bit halves
independently, and that is the only portable way to reach them. Linux does the
same thing (`vp_iowrite64_twopart`).

The fields that matter are the three queue addresses of the common
configuration — `queue_desc`, `queue_driver`, `queue_device` — plus any 64-bit
field of a device-specific configuration, such as the capacity of
`virtio-blk`.

Nobody writes them by hand. `virtio_pci::write_mmio_u64()` and
`virtio_pci::read_mmio_u64()` are the only way in, and they exist for this
reason alone.

## QEMU forgives it; VirtualBox kills the machine

A single 64-bit access compiles to one `movq` against the device's MMIO
window. QEMU splits an access wider than the device implements into accesses
the device does implement, so the driver looks correct there and the smoke
suites pass. VirtualBox does not split: its virtio core rejects the access,
returns `VERR_INVALID_PARAMETER` from the MMIO write handler, and PGM turns
that into a **guru meditation** — the whole virtual machine stops, not just
the device.

It looks like this in `VBox.log`:

```
AssertLogRel PGMAllPhys.cpp(4339) pgmPhysWriteHandler: PGM_HANDLER_PHYS_IS_VALID_STATUS(rcStrict, true)
rcStrict=VERR_INVALID_PARAMETER GCPhys=00000000f0000020
Changing the VM state from 'RUNNING' to 'GURU_MEDITATION'
!!         VCPU0: Guru Meditation -2 (VERR_INVALID_PARAMETER)
```

`GCPhys` is the give-away: subtract the base of the device's MMIO region (the
log lists it, `f0000000` for `virtio-net #0 (modern)`) and the remainder is the
offset inside the common configuration — `0x20` is `queue_desc`. The `rip` in
the same dump resolves against `build/kernel.elf` with
`llvm-symbolizer` from `PATH`, which names the function directly.

The consequence for the work: **the QEMU smokes cannot catch this class of
bug**. A change to `virtio_pci::` or to any virtio driver is only verified once
it has also brought a queue up under VirtualBox, which is the stricter of the
two and the one that behaves like real hardware would.

## A capability never stores a pointer into its own device

`CapabilityView` says *where* a virtio capability lives: the index of its BAR,
the offset inside that BAR, and the `base` pointer already resolved to the
mapping. It deliberately does **not** keep a `MappedBar*` into the `Device` it
was resolved from.

That is not a style rule, it is the fix for a bug that took the whole machine
down. `virtio-input` cannot initialize straight into its global `Device`: it has
to decide which of the pair of virtio-input functions is the tablet and which is
the keyboard, and a device that turns out to be the wrong one must be left
untouched. So it builds the `Device` in a local, and publishes it by value:

```c
virtio_pci::Device device = {};               // a stack frame
...
g_device = device;                            // copied. `device` is now history.
```

With a `MappedBar* bar` in the view, `g_device.notify_view.bar` was a pointer
into that dead frame. Every other field copied correctly — `base` is a plain
address, so the config, notify-off multiplier and ISR reads were all fine — which
is why the machine booted, ran the desktop, and survived everything until the
first pointer event. Then `notify_queue()` dereferenced the stale pointer,
computed a notify address out of reused stack memory, and stored the queue index
there: `#14 page fault`, `cr2` inside the low 4 GB, and `stop_on_exception()`'s
`halt_forever()`. Moving the mouse was enough to trigger it, because the
descriptor refill is the first thing that notifies *after* the copy.

Two rules fall out of that, and they generalize past virtio:

- **A struct that gets published by value carries no interior pointers.** Store
  the index, or recompute the pointer at the point of use against the object you
  were handed. A copy that silently invalidates part of itself is the kind of bug
  that reads as "works until it doesn't".
- **A pointer field that is only read on a rare path hides this until the rare
  path runs.** `notify_view.base` was correct, so every probe, every queue setup
  and every status read agreed. Only `notify_view.bar->base`, on the refill,
  disagreed. When a struct mixes "copied by value" data with "cached pointer"
  data, the cached pointer is the one that is wrong.

The regression is `./build.sh smoke pointer-smoke --virtio`. It moves the pointer
through QMP, which is the only way to reach the refill, and asserts the position
arrives as a position rather than a delta.

## Where a queue comes up

`probe()` only reads the configuration space: the device identity, the feature
bits and whatever the device publishes about itself (the MAC of `virtio-net`,
the capacity of `virtio-blk`). Queues are armed later, in `bring_up()`.

That is why a bad access to a queue address does not show up during boot for a
NIC: nothing sets up the network queues until userland asks the interface to go
up — `netinfo` on `/dev/net0` is normally the first thing that does — and so a
transport bug surfaces as "running this program kills the machine" rather than
as a boot failure.

## A dropped delta is not a dropped position

This is the rule the pointer path got wrong for a while, and it is worth
stating as a rule because the mistake is invisible from the code that makes it.

`virtio-tablet` is an **absolute** device: it reports where the pointer is, not
how far it moved. That is strictly more information than PS/2's deltas, and it
is the reason a `virtio-tablet` guest stays aligned with the host cursor at all.
The temptation is to collapse it to deltas at the device boundary, since every
consumer downstream accumulates. **Doing so throws the absolute information
away, and then no consumer can recover from losing an event.**

`virtio_input::submit_screen_position()` used to emit only
`screen_x - g_last_screen_x`, and the `/dev/mouse0` queue dropped the oldest
event when it filled. The comment in `ui::enqueue_mouse_event` justified that
as harmless — *"un delta de movimiento perdido se corrige solo, el cursor es una
posicion absoluta y el proximo evento la reubica"* — and it reads as true right
up until you notice that the absolute position had already been thrown away one
layer earlier, in the line that produced the delta. So it was a dropped
position: permanent, and the cursor stayed offset from the host's for the rest
of the session, which is what the user sees as *"queda corrido"*.

The fix is to let the position travel: `savanxp_mouse_event` carries
`absolute_x`/`absolute_y` and `SAVANXP_MOUSE_FLAG_ABSOLUTE`, a relative device
(`ps2::emit_mouse_event`) leaves the flag clear, and `windowd` **assigns** the
position when the flag is set and only sums deltas when it is not. A dropped
delta is then a gap that the next event closes, and the queue only has to
preserve the wheel — which is a magnitude, and a lost tick is scroll that never
happens.

Two consequences worth keeping:

- **The kernel queue may drop events; it may not drop the position.** Any new
  field that says "this is where the pointer is" has to survive the overflow
  path, or the bug comes back under load.
- **Coalescing has to take the last position, never a sum and never the first.**
  Summing absolute positions invents a point nobody was at; keeping the first
  leaves the cursor off by however far the host moved in between.
  `windowd_pointer_coalesce_selftest` is the regression for both.

## The host cursor is still drawn

The guest and the host both draw a cursor, and SavanXP has no way to ask QEMU
to stop. Hiding it is the QEMU "wm" mouse protocol, carried over the `fw_cfg`
`boot-fw-wm/*` channel; the tree has no `fw_cfg` support at all today, so this
is boot-loader work rather than driver work.

Until that exists, the two cursors agree on the pixel (the tablet is absolute,
so the guest plane is placed at the position the host reported) and what
remains is cosmetic. The guest cursor is a real hardware plane on `virtio-gpu`
(`MOVE_CURSOR`, one RPC per pointer event); where the backend has no cursor
plane, the desktop falls back to a software cursor.

