# Working in `subsystems/linux/`

The root [`AGENTS.md`](../../AGENTS.md) applies; this adds what is particular
to SSL, the Linux subsystem.

## Read first

- [`LINUX.md`](../../docs/userland/LINUX.md): SSL as built, the profiles and
  the smoke gates.
- [`LINUX_XEYES_PROFILE.md`](../../docs/userland/LINUX_XEYES_PROFILE.md): the
  measured scope of the Xeyes profile and its fixtures.
- The order of work:
  [`LINUX_SUBSYSTEM_ROADMAP.md`](../../docs/roadmaps/open/LINUX_SUBSYSTEM_ROADMAP.md).

## Rules here

- The guest tree comes from `profile/profile.sh setup` (Alpine). Without it the
  scenarios and host tests that need it are skipped or fail by name; report
  them as such, never as passing.

## Verify

- Always `./build.sh smoke linux-hello` and `linux-restart`.
- The part touched: `linux-dynamic*`, `linux-tools`, `linux-xclient`,
  `linux-xeyes` (`./build.sh smoke --list`).
