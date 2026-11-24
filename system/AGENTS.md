# Working in `system/`

The root [`AGENTS.md`](../AGENTS.md) applies; this adds what is particular to
`system/`: the platform code (`gfx/`: SXGFX, SXCHROME and SXGUI-C; `loader/`;
`runtime/`, SUN's runtime), the desktop services (`windowd`, `compositord`,
`sessiond`, `elevationd`) and the system's applications.

## Read first

- [`SUN.md`](../docs/userland/SUN.md) and
  [`NATIVE_APPS.md`](../docs/userland/NATIVE_APPS.md): every new program here
  is a SUN program, and a call SUN lacks is added with the program that needs it.
- [`PLATFORM_GRAPHICS.md`](../docs/desktop/PLATFORM_GRAPHICS.md): code in
  `gfx/` makes no system call and includes nothing from `subsystems/`.
- The desktop: [`DESKTOP_ARCHITECTURE.md`](../docs/desktop/DESKTOP_ARCHITECTURE.md)
  and [`WM_SUBSYSTEM.md`](../docs/desktop/WM_SUBSYSTEM.md).
- The loader and shared libraries: [`SHARED_LIBRARIES.md`](../docs/kernel/SHARED_LIBRARIES.md).
- An application's own document: the "Userland" and "Desktop and graphics"
  sections of [`docs/README.md`](../docs/README.md).

## Verify

- `windowd`, `compositord`, the desktop: `./build.sh smoke windowd-smoke` and
  `./build.sh smoke taskbar-smoke`.
- `libsxgui`, the loader, a program that draws: `./build.sh smoke sxgui-smoke`
  (below). The loader and shared libraries also: `dltest`, `sonametest`,
  `sonametest-newer`, `ldfailtest`, `diamondtest`.
- An application with a scenario of its own (`devmgmt-smoke`, `paint-smoke`,
  `taskmgr-smoke`, ...): `./build.sh smoke --list` names them.
- Anything visible on screen is also looked at with `./tools/shoot.sh`.

## `sxgui-smoke` and the shared library work

- `./build.sh smoke sxgui-smoke` whenever `libsxgui.so.0.4`, the loader, or a
  program that draws is touched. It is the only check that looks at what SxGUI
  puts on screen, and it is what caught a toolkit that stopped painting.
  `smoke` and `calc-smoke` only prove the loader works, not that the toolkit
  draws. It asserts the glyphs of the editor's lines and a taskbar label, not
  just that a pixel moved.
- When checking that something visibly changed, confirm the rebuilt binary is the
  one being tested. `./build.sh build` output matters: a build that fails leaves
  the previous binary in the image and every scenario still passes. When a visual
  check does not fail, extract the binary from `build/disk.img` and look at it
  before believing the check.
