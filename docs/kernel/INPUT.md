# Input: drivers, the keyboard core, and who wins

The input layer (`kernel/input.cpp`, `include/kernel/input.hpp`) sits between
the input drivers and the two consumers of their events: the text console
(`tty`) and the graphics session (`ui`, `/dev/input0`, `/dev/mouse0`). No
driver knows another one exists, and nothing outside the drivers names a
driver. Adding one (USB HID, for instance) means writing the driver and calling
`input::register_driver`. Nothing else changes.

## Drivers

A driver registers an `input::Driver`: whether it provides a keyboard, a
pointer or both, plus its callbacks.

| Callback | Required | Purpose |
|---|---|---|
| `poll` | yes | Drain what the device has queued. Runs from the timer tick and from blocking reads. |
| `set_keyboard_leds` | no | The lock state changed. Record it and do the device I/O from `poll`: it is called with the kernel lock held, possibly from an interrupt. |
| `set_screen_extent` | absolute pointers | The screen size the device's range maps onto. Sent again after every mode change. |
| `reset_pointer` | absolute pointers | Forget the last position at the start and end of a graphics session, so the next sample is a jump, not a delta. |

Today there are two drivers: `virtio-input` (tablet and keyboard) and `ps2`
(the 8042 keyboard and mouse).

## Registration order is priority

QEMU feeds the host's keyboard and mouse to every backend at once. If two
drivers reported the same keyboard, every key would arrive twice. The rule:
**a driver that finds a keyboard (or pointer) already registered leaves its own
silent.** `kernel_main` initializes `virtio_input` before `ps2`, so on a virtio
machine the 8042 keeps its keyboard and mouse ports quiet and logs
`ps2: skipping keyboard, another input driver provides one`. The order of the
initialize calls is the policy. A new driver goes into that order according to
how much it should be trusted over the others.

`input::keyboard_available()` is what `savanxp_system_info.input_ready`
reports.

## The keyboard core

`kernel/keyboard.cpp` decides what a key means. Keyboard drivers report
**physical key positions as PC/AT scancode set 1** (`input::submit_scancode`:
7-bit code, `extended` for the `0xE0` prefix, pressed or released). The core
keeps the shift/ctrl/alt state and the Caps/Num/Scroll lock state, applies the
active layout (`SAVANXP_KEYBOARD_LAYOUT_ES` or `_EN`), and emits the
`SAVANXP_KEY_*` code, the text character and the modifier mask.

Keys with no set 1 position of their own (Pause, Print Screen) go through
`input::submit_key`.

Why set 1 rather than a neutral code: both existing drivers already speak it.
The 8042 translates to it, and evdev codes 1–0x58 are set 1 by design, so
`virtio-input` maps only the extended keys. A driver whose hardware speaks
something else maps to set 1 at its edge. A USB HID keyboard would need a
usage → set 1 table. If set 1 ever runs out (keys it has no position for),
the core's input code is the thing to change, not each driver.

Lock keys change state in the core and then reach every keyboard driver
through `set_keyboard_leds`. Only the driver whose keyboard is active is told,
so a quiet 8042 is not sent LED commands on behalf of a virtio keyboard.

The layout is a user preference that lives in the core:
`INPUT_IOC_SET_LAYOUT` / `INPUT_IOC_GET_LAYOUT` on `/dev/input0` reach it
through `input::set_keyboard_layout`.

## Verification

`./build.sh smoke kbd-smoke` drives the keyboard over QMP: plain and shifted
letters, Ctrl, an extended key, Enter, Caps Lock on and off, and AltGr on the
Spanish layout. Run it twice, once plain (PS/2) and once with `--virtio`: the
two runs exercise the two drivers through the same core. `pointer-smoke
--virtio` covers the absolute pointer, and `native-hw-smoke` the PS/2 keys
into a native program.
