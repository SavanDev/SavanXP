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

Today there are three drivers: `virtio-input` (tablet and keyboard), `ps2`
(the 8042 keyboard and mouse) and `usb-hid` (USB keyboards, mice and tablets,
[USB_ROADMAP.md](../roadmaps/done/USB_ROADMAP.md)).

## Registration order is priority

QEMU feeds the host's keyboard and mouse to every backend at once. If two
drivers reported the same keyboard, every key would arrive twice. The rule:
**a driver that finds a keyboard (or pointer) already registered leaves its own
silent.** `kernel_main` initializes `virtio_input` before `ps2`, so on a virtio
machine the 8042 keeps its keyboard and mouse ports quiet and logs
`ps2: skipping keyboard, another input driver provides one`. The order of the
initialize calls is the policy. A new driver goes into that order according to
how much it should be trusted over the others.

**USB is the exception: it coexists with everything.** On real hardware a
laptop's PS/2 keyboard and a USB keyboard plugged into it are both real, and
both must work, so `usb-hid` registers last (`usb::start_input()`, after
`ps2`) and silences nothing; and it registers whenever a controller started,
with or without a keyboard, so one plugged in later works too. QEMU does not
deliver an event twice: each one goes to a single device, so a machine with
PS/2 and USB keyboards gets every key once (`kbd-smoke` passes with
`SAVANXP_USB=ON`). What QEMU does do is send relative moves to a relative
mouse and buttons to whichever pointer it activated last, so a USB tablet next
to the PS/2 mouse would take clicks the mouse moved for; smokes therefore get
a relative `usb-mouse` unless they ask for the tablet.

A machine may have no 8042 at all (recent hardware, QEMU's `i8042=off`): the
status port then reads `0xff`, and `ps2` logs `ps2: no 8042 controller` and
stays out.

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

The core also knows which positions are down, so a make code for a key that
already is is the keyboard repeating it (8042 typematic, or the host's repeat
for `virtio-input`). It still goes out as `KEY_DOWN`, with
`SAVANXP_KEY_MOD_REPEAT` in the mask: a text field wants the repeats, and
something that must act once per press, as the elevation prompt does, skips
them. The bit is not a modifier, so a consumer comparing masks for a hotkey
leaves it out.

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
`INPUT_IOC_SET_LAYOUT` / `INPUT_IOC_GET_LAYOUT` reach it through
`input::set_keyboard_layout`, on `/dev/input0` or on `/dev/keyboard`, which has
those ioctls and no events: it is 0666, so the user's desktop can choose the
layout without being able to read keys.

## Pointer speed

The pointer speed is a user preference too, and it also lives in the input
core (`kernel/input.cpp`): `input::submit_mouse_event` scales each relative
event before it is queued, so `windowd`, a game that captures the pointer and
an X11 client all see the same speed. It runs from 1 to 20
(`SAVANXP_POINTER_SPEED_MIN`/`_MAX`), and 10 (`_DEFAULT`) is one to one. The
multipliers are the steps of Windows' pointer speed slider without "enhance
pointer precision": 1/32 at 1, 7/8 at 9, then a quarter more per step up to
3.5 at 20 (`include/kernel/pointer_motion.hpp`). Each axis carries the
fraction a scaled event did not move into the next one, so at slow speeds a
run of single counts still adds up to the right distance. A move in the other
direction drops that fraction, and changing the speed clears it.

**Only relative pointers are scaled.** A PS/2 or USB mouse reports counts; a
tablet (`virtio-tablet`, a USB tablet, QEMU's default pointer in most
scenarios) reports where the pointer is, and scaling its delta would move the
cursor away from its own position. Its events go through unchanged, so the
slider does nothing visible under a tablet. To see it work in QEMU, use a
relative pointer (PS/2 or `usb-mouse`).

`INPUT_IOC_SET_POINTER_SPEED` / `INPUT_IOC_GET_POINTER_SPEED` reach it on
`/dev/pointer`, which has those ioctls and no events. Like `/dev/keyboard` it
is 0666, so Mouse Properties can set the speed without reading the mouse;
setting needs write access, and a speed out of range is `-EINVAL`. The ioctls
also work on `/dev/mouse0`. SUN wraps them in `sxn/sxnpointer.h`
(`sxn_pointer_speed`, `sxn_set_pointer_speed`), POSIX in
`input_get_pointer_speed` / `input_set_pointer_speed`.

Mouse Properties' Motion tab sets the speed while the slider moves, so the
user feels it before choosing; Apply or OK saves it to
`/users/user/Settings/mouse.cfg` (the speed in decimal), and Cancel or closing
the window puts back the speed from before. `init` applies the saved speed at
boot, before `windowd` starts, as it does with the keyboard layout. A missing
or broken file leaves the default.

### Who reads the events

`/dev/input0` and `/dev/mouse0` each have one queue, and a read drains it. So
the reader is claimed: `INPUT_IOC_CLAIM` on either device makes the caller its
only reader until it exits, and a read by anybody else is `-EBUSY`. Only the
system may claim, and only a device nobody holds. `windowd` claims both at
startup. Opening and the layout ioctls stay open to anyone the device mode
allows, and a device nobody
claimed reads as before, which is how `kbdtest` and `mousetest` run without a
session. The reason is the elevation prompt: its answer has to be what
`windowd` read ([USERS_AND_PERMISSIONS.md](USERS_AND_PERMISSIONS.md#elevation)).
Nothing writes events into the queues but the drivers.

## The tick and PS/2 interrupts

`ps2` routes IRQ1 and IRQ12, and their handlers drain the 8042 into the
driver's raw queue; `poll` decodes that queue. With the interrupts routed,
`poll` reads the controller's status port itself only once every 64 calls,
as a safety net. If two such checks in a row find bytes with no interrupt
since the previous one, the interrupts are not arriving: the log says `ps2:
input arrives without its interrupts; polling the controller every tick` and
`poll` reads the controller every time, as it did before. One check alone
proves nothing: the tick runs with interrupts off, so an interrupt can be
pending while it looks.

Why: the status read cost about 20 µs of every tick under KVM, where a port
read leaves the VM ([SMP_ROADMAP.md](../roadmaps/open/SMP_ROADMAP.md), slice
5.1). `virtio-input`'s `poll` reads only its rings in memory while nothing
arrives. What `usb-hid`'s costs depends on the host controller and was not
measured.

## Verification

`./build.sh smoke kbd-smoke` drives the keyboard over QMP: plain and shifted
letters, Ctrl, an extended key, Enter, Caps Lock on and off, and AltGr on the
Spanish layout. Run it twice, once plain (PS/2) and once with `--virtio`: the
two runs exercise the two drivers through the same core. `pointer-smoke
--virtio` covers the absolute pointer, and `native-hw-smoke` the PS/2 keys
into a native program. `elevation-smoke` checks the claim: a second reader
and a second claim are both `-EBUSY`.
