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

## Repository rules

- Do not break the persistence of external apps installed in `build/disk.img`.
- `./build.sh build` must not delete or unconditionally recreate the disk
  image if one already exists and is valid.
- Changes to the kernel, the build, the SDK, `SxFS` or the host tooling must
  not lose external binaries already installed in `/disk/bin`, nor persistent
  assets under `/disk/games`.

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
- Confirm the executable is still present in `/disk/bin`.
- Confirm its persistent assets, for example
  `/disk/games/doom/doom1.wad`, are still present.
- For a repeatable isolated check, run
  `./tools/verify_doom_persistence.sh` (or pass `--wad` for a real IWAD).

## Current reference case

- `ports/doomgeneric` is the canonical Doom port and persistence regression.
- If after a `build` the system cannot find `doomgeneric`, the change must be
  treated as a regression of the persistent image flow.
