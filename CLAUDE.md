# CLAUDE.md

**[`AGENTS.md`](AGENTS.md) is the source of truth for how to work in this
repository. Read it before making changes.** This file exists because the
harness loads `CLAUDE.md` automatically and `AGENTS.md` only when asked, so the
rules that get skipped most often are repeated here in full. Where the two
disagree, `AGENTS.md` wins.

## Non-negotiables

### 1. Every behavior change gets a changelog entry, in the same commit

Under `[Unreleased]` in `CHANGELOG.md`, in the matching subsection
(`Added`/`Changed`/`Removed`/`Fixed`). Never in a version section that already
has a date — that rewrites published history.

**Hard length limit: a normal change is at most 3 lines. A large structural
change is at most 6. There is no third tier.**

An entry says *what changed as seen from the outside*. The why, the root-cause
analysis, the tour of the files touched and the verification details go in the
commit message. Keep the names someone will search for (commands, targets,
flags, API functions, boot log lines, new build requirements); drop the file
inventory, the step-by-step and the "Verified with ...".

If it does not fit in 6 lines, the entry is carrying something that belongs in
`docs/`. Put it there and link to it.

### 2. Anything that must stay true for the future of the OS goes in `docs/`

Design decisions, architectural rules, layering, formats, roadmaps, gotchas
worth remembering. The changelog says *what changed*; `docs/` says *how the
system works and why*. Every document in `docs/` must be listed in
[`docs/README.md`](docs/README.md).

### 3. All repository documentation is written in English

`README.md`, `CHANGELOG.md`, `AGENTS.md`, `docs/`, `assets/README.md`. The
project should be readable by anyone who finds it.

### 4. Never break the persistence of `build/disk.img`

`./build.sh build` must not delete or unconditionally recreate the image if
one already exists and is valid. Changes to the kernel, the build, the SDK,
`SxFS` or the host tooling must not lose external binaries in `/disk/bin` nor
persistent assets under `/disk/games`.

When touching that area, verify it:

```bash
./ports/doomgeneric/build.sh # install an external app into the image
./build.sh build              # then rebuild
```

`/disk/bin/doomgeneric` and `/disk/games/doom/*.wad` must both still be there.
If they are not, treat it as a regression of the persistent image flow.

### 5. No machine-specific paths in the tooling

Build tools resolve through CMake and the host `PATH`, with documented
environment or cache overrides. A new host tool belongs in CMake discovery and
the documented dependency list — never as an absolute path in `build.sh` or
the CMake files.

## Build commands

Native Linux path:

```bash
./build.sh build             # kernel, userland and disk image
./build.sh run               # boot in QEMU
./build.sh iso               # bootable ISO
./build.sh test              # CTest/host tests
./build.sh smoke smoke       # core QEMU smoke
./tools/shoot.sh             # visual desktop/QMP check
./tools/build-user.sh --source path/to/app --name app
```

`README.md` has the full list and the requirements;
[`docs/BUILD_CMAKE.md`](docs/BUILD_CMAKE.md) covers the native build path.
