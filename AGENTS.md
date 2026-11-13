# AGENTS

## Toolchain

- The native build uses `build.sh` and CMake, resolving tools from the host
  `PATH` with optional CMake cache and environment overrides.
- Do not put absolute paths from a specific machine back into `build.sh`,
  `CMakeLists.txt` or the tooling. A new host tool belongs in CMake tool
  discovery and the documented dependency list.

## Changelog

- Every behavior change (feature, fix, build/tooling change, removal) is
  documented in `CHANGELOG.md` under `[Unreleased]`, in the matching subsection
  (`Added`/`Changed`/`Removed`/`Fixed`), as part of the same commit that
  introduces it.
- Do not add entries to a version section that is already closed (dated): that
  rewrites published history. If `[Unreleased]` does not exist at the top of
  the file, create it.
- The release commit (`release(vX.Y.Z): ...`) is the only one that renames
  `[Unreleased]` to the new version with a date.
- Right after a release, master bumps `include/shared/version.h` to the next
  version, so master never shows a number that is already published (see
  `docs/build/BUILD_CMAKE.md`, "Version and build number").

### How to write an entry

- An entry records **what changed as seen from the outside**, not how it was
  implemented. The why, the root-cause analysis, the tour of the files touched
  and the verification details belong in the commit message, which is where
  someone will go looking for them. The changelog is read end to end; a commit
  is read one at a time.
- Format: one change per entry, opening with a bold sentence stating the
  change.
- **Hard length limit: a normal change is at most 3 lines. A large structural
  change is at most 6. There is no third tier.** If it does not fit, the entry
  is carrying implementation detail that belongs somewhere else.
- Keep the names someone will need in order to search (commands, targets,
  flags, API functions, boot log lines, new build requirements). Drop the file
  inventory, the step-by-step implementation and the "Verified with ...".
- If several entries are parts of one change, they go together as one.
- Each version carries **one** subsection of each kind at most, in the order
  `Added`/`Changed`/`Removed`/`Fixed`. Do not repeat headings.

### What does not go in the changelog

- Anything that has to stay true for the future of the OS — design decisions,
  architectural rules, layering, formats, roadmaps, gotchas worth remembering —
  goes into `docs/`, not into a changelog entry.
- The changelog says *what changed*. `docs/` says *how the system works and why
  it is that way*. When a change needs more than 6 lines to explain, the
  explanation goes to `docs/` (new document or an existing one) and the entry
  links to it.
- Every document in `docs/` must be listed in `docs/README.md`.

## Documentation language

- All repository documentation (`README.md`, `CHANGELOG.md`, `AGENTS.md`,
  `docs/`, `assets/README.md`) is written in English, so the project is
  readable by anyone who finds it.

## Code language

- Code is written in English: identifiers, log lines, user-visible strings,
  and comments.
- Much of the tree still carries Spanish comments from earlier development.
  Do not rewrite them in bulk: every commit that touches a file translates
  the comments on the lines it changes — and any comment those lines make
  stale — into English, until only one language remains.
- The unit of translation is the whole comment, not the line. A change that
  touches any part of a Spanish comment (a `/* */` or `//` block, a run of
  `#` lines, a docstring) rewrites that entire comment in English. Never leave
  a comment half in Spanish and half in English.
- Moving code counts as changing it. A file moved or renamed (`git mv`), or a
  block of code cut out of one file into another, is touched in full: the
  comments and the user-visible strings it carries are translated in the same
  commit, even when no other line of it changes. A move is the cheapest
  moment to do it, because the diff already shows the whole file. This is
  what keeps the bulk-rewrite rule from becoming a reason to carry Spanish
  into a new place.

## New programs are SUN programs

- Every new program in the tree (app, tool, service or game) is written
  against SUN, the native API (`docs/userland/SUN.md`), not against the POSIX
  subsystem.
- If SUN lacks something the program needs, the program becomes the consumer
  of the slice that adds it (`SUN.md`, rule 1), together with its `nativetest`
  assertion. Missing SUN calls are not a reason to write it on POSIX.
- The existing POSIX programs move to SUN gradually, one at a time, usually
  when they are being changed for another reason. Each move is its own commit
  with its own changelog entry, and the program keeps its name and its
  behavior.
- POSIX stays supported for ports and external programs (`ports/`,
  `tools/build-user.sh`). Nothing here removes it.

## Feature parity between subsystems

- What one subsystem can do, the other should too, as far as the platform
  allows. A program or port must not lose a capability (a window title, a
  cursor shape, a dialog) only because of which API it is written against.
- A gap found in either direction is closed in the subsystem that lacks it,
  each side in its own idiom (`sxnwm_set_title` in SUN, `gfx_set_title` in the
  POSIX runtime), not worked around in the program. A planned port to SUN is
  not a reason to leave POSIX without it.
- When a gap cannot be closed (the platform has no way to do it), say so in
  the subsystem's documentation instead of leaving it to be rediscovered.

## Repository rules

- Do not break the persistence of external apps installed in `build/disk.img`.
- `./build.sh build` must not delete or unconditionally recreate the disk
  image if one already exists and is valid.
- Changes to the kernel, the build, the SDK, `SxFS` or the host tooling must
  not lose external binaries already installed in `/bin`, nor persistent
  assets under `/games`.

## Commit messages

- Do not put a link to an agent's session (for example a
  `Claude-Session: https://claude.ai/...` trailer) in a commit message. A
  co-author trailer is fine; the session link points at a private
  conversation nobody reading the history can open.

## Practical rule for the main build

- The main build may sync the internal userland onto the existing image, but it
  must not reset it except on real corruption or a format incompatibility.
- If `build/disk.img` has to be recreated, that must be a deliberate and
  justified decision, not the normal behavior of `./build.sh build`.

## Minimum verification when touching that area

- Install an external app into the image, for example with:
  `./ports/doomgeneric/build.sh --wad ports/doomgeneric/wad/doom1.wad`
- Then run the normal build:
  `./build.sh build`
- Confirm the executable is still present in `/bin`.
- Confirm its persistent assets, for example
  `/games/doom/doom1.wad`, are still present.
- For a repeatable isolated check, run
  `./tools/verify_doom_persistence.sh` (or pass `--wad` for a real IWAD).

## Minimum verification for the shared library work

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

## Visual checks with `tools/shoot.sh`

- Keep `--out-dir` short, for example `build/shots-<name>`. QEMU opens its
  monitor and QMP sockets inside that directory, and a Unix socket path must
  be under 108 bytes: a deep temporary or scratch directory makes QEMU exit
  with `UNIX socket path ... is too long` before the scenario starts.

## Current reference case

- `ports/doomgeneric` is the canonical Doom port and persistence regression.
- If after a `build` the system cannot find `doomgeneric`, the change must be
  treated as a regression of the persistent image flow.
