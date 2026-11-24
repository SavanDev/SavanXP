# Working in `subsystems/win32/`

The root [`AGENTS.md`](../../AGENTS.md) applies; this adds what is particular
to SSW, the Win32 subsystem.

## Read first

- [`WIN32.md`](../../docs/userland/WIN32.md): SSW as built. It is over a
  thousand lines; list its headings and read the sections the change touches.
- The profile being worked on:
  [`WIN32_SOLITAIRE_PROFILE.md`](../../docs/userland/WIN32_SOLITAIRE_PROFILE.md)
  or [`WIN32_WORDPAD_PROFILE.md`](../../docs/userland/WIN32_WORDPAD_PROFILE.md).
- The order of work:
  [`WIN32_SUBSYSTEM_ROADMAP.md`](../../docs/roadmaps/open/WIN32_SUBSYSTEM_ROADMAP.md),
  [`WIN32_WORDPAD_ROADMAP.md`](../../docs/roadmaps/open/WIN32_WORDPAD_ROADMAP.md).

## Rules here

- Microsoft binaries and anything derived from them (`sol.exe`, `cards.dll`,
  `wordpad.exe`, the MFC and RichEdit DLLs, Windows XP captures, card pixels)
  never enter the repository, the build image or the ISO. Tests find them
  through `SAVANXP_WIN32_FIXTURES` and are skipped when it is unset.
- A test skipped for missing fixtures is reported as skipped, never as passing.

## Verify

- Always `./build.sh smoke win32-hello`, `win32-window` and `win32-dialog`.
- With `SAVANXP_WIN32_FIXTURES` set: `win32-cards` and `win32-solitaire`, and
  the `win32-*-test` host tests in `./build.sh test`.
