# System limits roadmap

> **Status: investigated; the heap's arena limit is gone (2026-11-28), the
> kernel tables are not raised yet.** This records how many windows,
> descriptors and kernel objects the system allows today, how the limits depend
> on each other, and the order to raise them in. It began with a second
> cascade level in the Start menu ([START_MENU.md](../../desktop/START_MENU.md)),
> which needs a fifth owned window. The limits matter more as the system
> stops being only an experiment.

## The chain

How many windows can exist is not one number. It is a chain of limits, and the
lowest one decides. From the top down:

| Limit | Value | Where | What it counts |
|---|---|---|---|
| `SAVANXP_WM_MAX_OWNED_WINDOWS` | 4 | `include/abi/savanxp_abi.h` | Owned windows (dialogs, popups) per process, besides its main window |
| `WINDOWD_DESKTOP_WINDOW_COUNT` | 1 + 4 | `system/windowd/windowd_session.h` | The desktop's windows: background, taskbar, three popups |
| `WINDOWD_MAX_OVERLAY_CLIENTS` | 12 | `system/windowd/windowd_session.h` | **Every** application window, including the applications' owned windows |
| `SAVANXP_WM_MAX_WINDOWS` | 12 | `include/wm/wm_shell_protocol.h` | Entries in the window list the taskbar and Task Manager read |
| `SAVANXP_COMPOSITOR_MAX_SURFACES` | 32 | `system/include/compositor_protocol.h` | Surface ids; 0 and 1 are taken, so 30 client surfaces |
| `SAVANXP_COMPOSITOR_MAX_LAYERS` | 48 | same | Layers in one composed scene |
| `process::kMaxFileHandles` | 64 | `include/kernel/process.hpp` | Descriptors **per process**: `windowd` keeps 2 per window |
| `process::detail::kMaxPipeCount` | 64 | `include/kernel/process_detail.hpp` | Pipes in the **whole system**, 8 KiB each |
| `object::kMaxEventObjects` | 128 | `include/kernel/object.hpp` | Event objects in the whole system: one wake event per window, plus every program's own |
| `object::kMaxSectionObjects` | 512 | same | Sections in the whole system: one surface per window, plus every shared library and buffer |
| `process::kMaxProcesses` | 64 | `include/kernel/process.hpp` | Processes |
| `object::kMaxPortObjects` | 16 | `include/kernel/object.hpp` | Named ports (`windowd`, `audiod`, ...) |
| `FD_SETSIZE` | 64 | `subsystems/posix/sdk/v1/include/sys/select.h` | Descriptors `select()` can name in POSIX programs |

## How full it is today

`windowd --selftest` fills every slot: twelve application windows plus the
desktop's background, taskbar and a popup. In that state it measures **43 of
64 descriptors** and asserts that at least 8 stay free for a connection in
progress. `compositord` holds 26 ([WM_SUBSYSTEM.md](../../desktop/WM_SUBSYSTEM.md#descriptor-budget)).
So `windowd`'s table has room for about **six more windows** before anything
else moves:

```
(64 - 8 headroom - 43 in use) / 2 per window = 6
```

At most 5 + 12 = 17 surfaces are in use, against the compositor's 30.

What users run into first is the shared pool of 12. A program's dialogs come
out of the same twelve slots as everyone's main windows: three programs with
three dialogs each fill it. That number is more pressing than the four owned
windows per process.

## What each raise costs

- **Owned windows per process, 4 → 8.**
  - The ABI constant, the check in `include/sxn/sxnwm.h`, and windowd's two
    range checks follow from it. A binary built against 4 keeps working: it
    only refuses ids above 4 itself.
  - The desktop's table grows by 4 slots. An open window costs 2 of
    windowd's descriptors and 1 surface; a closed slot costs only its
    `struct windowd_client`.
  - This is the cheapest raise, and the one the Start menu's second cascade
    level needs.
- **Application windows, 12 → 24.**
  - `SAVANXP_WM_MAX_WINDOWS` must follow, which changes the window list
    section from about 13 to about 26 KiB. That is a new layout, so
    `SAVANXP_WM_SHELL_PROTOCOL_VERSION` goes to 6, and the taskbar and Task
    Manager must be rebuilt with it.
  - Surfaces: 9 desktop + 24 = 33 > 30, so the compositor's surfaces must
    first go to 64, and its layers with them: a scene has at least one
    layer per visible window.
  - Descriptors: 2 × 33 = 66 > 64, so `kMaxFileHandles` must be raised
    first.
- **Descriptors per process, 64 → 128 or 256.**
  - Each `HandleEntry` is 16 bytes and every process carries the whole
    table, so this costs 64 KiB → 128 or 256 KiB of kernel memory for 64
    processes.
  - Every loop over the table walks the larger size, but nothing scans it
    in a hot path that was measured.
  - `FD_SETSIZE` and the POSIX runtime's `SX_FD_SETSIZE` stay 64 unless
    raised too. A POSIX program with more than 64 descriptors could not
    `select()` the upper ones.
- **Pipes, 64 → 128.**
  - The pipe storage is static, 8 KiB per pipe: 512 KiB → 1 MiB of kernel
    memory.
  - Each connected application keeps about one pipe (owned windows add
    none), and the system services hold their own. With 24 windows, 64 pipes
    in the whole system would be tight.
- **Event objects, 128 → 256.** Each window takes one wake event, and the
  programs and services create their own on top.

The kernel's static tables have to fit the kernel image's memory budget. That
budget was not measured for this document; check it before the first raise.

## Process memory

How much a program can allocate is a chain of its own, below the windows:

| Limit | Value | Where | What it counts |
|---|---|---|---|
| Heap arenas | no limit of its own | `subsystems/posix/sdk/v1/runtime/posix.c` | Sections the allocator maps; each is one view and one section object |
| `SX_ARENA_MAX_BYTES` | 64 MiB | same | The largest arena; arenas double what is mapped up to it |
| `kMaxSectionBytes` | 64 MiB | `kernel/object.cpp` | The largest section; one `malloc` larger than this fails |
| `kMaxSectionViews` | 256 per process | `include/kernel/vmm.hpp` | Views: arenas, libraries, mmaps, surfaces |
| `kMaxSectionObjects` | 512 | `include/kernel/object.hpp` | Sections in the **whole system** |

The heap used to stop at eight arenas of at most 32 MiB (`SX_ARENA_CAPACITY`),
96 MiB in all. Xash3D at 1920x1080 ran out there on real hardware during the
campaign: `_Mem_Alloc: out of memory` while saving a game on the sixth level
change. Reproduced in QEMU with the user's game data and a console script that
changes level and saves 26 times, the heap refused a 4 MiB block on the tenth
level with all eight arenas in use, **43 MiB free in 1394 blocks and no free
block larger than 3 MiB**. It is fragmentation, not a leak: with the limit
removed the same run reached the 26th level on 9 arenas (192 MiB mapped,
70 MiB free, the largest free block still about 3 MiB).

What stays open:

- **The kernel tables are still fixed.** With no limit in the heap, one
  process with a heap of gigabytes takes a large share of the 512 section
  objects the windows' surfaces also need. The cure is the kernel's, not the
  heap's: the object tables and the per-process view table growing from the
  kernel heap, and a commit limit per process instead of a count per object
  type.
- **Fragmentation.** The allocator is first fit over one list of free
  blocks. Mixed lifetimes, such as a level's data and the 4–8 MiB buffers a
  save takes, leave a third of the heap free in pieces too small to reuse.
  Size classes, or large blocks served by sections of their own, would keep
  the heap close to what is live.
- **Sections are allocated whole.** An arena's pages are allocated and zeroed
  when it is created, which is also why `kMaxSectionBytes` exists. Pages that
  appear on first touch would let that limit rise.
- **`free` walks the arenas** to find the one holding a block. At tens of
  arenas that is nothing; it is worth a better index before a heap takes
  hundreds.

## Order

Each step is its own commit, measured with `windowd --selftest` (descriptor
count) and the session smokes.

1. **Kernel tables**: `kMaxFileHandles` 128, `kMaxPipeCount` 128,
   `kMaxEventObjects` 256. Measure the kernel's memory before and after.
   Decided on 2026-11-27: `kMaxFileHandles` rises with SxFS v3 (V3-B of
   [SXFS_ROADMAP.md](SXFS_ROADMAP.md)), whose larger trees and files open
   more at once. Win32 regions no longer need it: `winhost` closes each
   region's handle after the map (`SXN_SYS_PROTECT_VIEW`).
2. **Compositor**: `SAVANXP_COMPOSITOR_MAX_SURFACES` 64 and
   `SAVANXP_COMPOSITOR_MAX_LAYERS` 96.
3. **Owned windows**: `SAVANXP_WM_MAX_OWNED_WINDOWS` 8. This is enough for
   the second cascade level, and it fits even before steps 1 and 2, using the
   six windows of headroom.
4. **Application windows**: `WINDOWD_MAX_OVERLAY_CLIENTS` and
   `SAVANXP_WM_MAX_WINDOWS` 24, with shell protocol v6. Raise the selftest's
   expectations with it.
5. **`FD_SETSIZE`**, when a POSIX port needs it.
