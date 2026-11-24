# Working in `subsystems/posix/`

The root [`AGENTS.md`](../../AGENTS.md) applies; this adds what is particular
to the POSIX subsystem: its kernel side (`kernel/`), the SDK and runtime
(`sdk/v1/`) and the POSIX programs (`userland/`).

## Read first

- [`SYSTEM_LAYERING.md`](../../docs/userland/SYSTEM_LAYERING.md): where POSIX
  sits next to SUN and the platform code.
- The SDK: [`sdk/v1/REFERENCE.md`](sdk/v1/REFERENCE.md), including the
  versioning policy (`SAVANXP_SDK_VERSION_MAJOR`/`MINOR`).
- The window client in `sdk/v1/runtime/`:
  [`PLATFORM_GRAPHICS.md`](../../docs/desktop/PLATFORM_GRAPHICS.md).

## Rules here

- No new programs here: a new app, tool, service or game is a SUN program in
  `system/`. POSIX stays for ports and external programs.
- Moving a program to SUN is its own commit with its own changelog entry, and
  what it used stays in POSIX: parity only grows.
- What SUN gains, POSIX gains too, in its own idiom (`gfx_set_title` for
  `sxnwm_set_title`). A gap left open is written down as pending in the area's
  document.
- An SDK change checks the SDK version step against the policy.

## Verify

- `./build.sh smoke smoke`. The runtime's window client or anything that draws:
  `./build.sh smoke sxgui-smoke`. Threads: `pthread-test`.
- The SDK or the runtime's install: the persistence check in the root
  `AGENTS.md`.
