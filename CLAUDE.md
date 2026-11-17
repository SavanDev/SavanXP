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

### 3b. Code is written in English too

Identifiers, log lines, user-visible strings, and comments. Spanish comments
from earlier development are translated incrementally: every commit that
touches a file translates the comments on the lines it changes, until only
one language remains. No bulk rewrites. The unit is the whole comment: touching
any part of a Spanish comment block or docstring rewrites all of it in English,
never half and half. **Moving code counts as changing it**: a file moved or
renamed (`git mv`), or code cut from one file into another, has all its
comments and user-visible strings translated in the same commit.

### 4. Never break the persistence of `build/disk.img`

`./build.sh build` must not delete or unconditionally recreate the image if
one already exists and is valid. Changes to the kernel, the build, the SDK,
`SxFS` or the host tooling must not lose external binaries in `/bin` nor
persistent assets under `/games`.

When touching that area, verify it:

```bash
./ports/doomgeneric/build.sh # install an external app into the image
./build.sh build              # then rebuild
```

`/bin/doomgeneric` and `/games/doom/*.wad` must both still be there.
If they are not, treat it as a regression of the persistent image flow.

### 5. No machine-specific paths in the tooling

Build tools resolve through CMake and the host `PATH`, with documented
environment or cache overrides. A new host tool belongs in CMake discovery and
the documented dependency list — never as an absolute path in `build.sh` or
the CMake files.

### 6. New programs are written against SUN

Every new app, tool, service or game uses the native API
([`docs/userland/SUN.md`](docs/userland/SUN.md)), not POSIX. If SUN lacks
something, the program is the consumer of the slice that adds it, with its
`nativetest` assertion. Existing POSIX programs move to SUN one at a time, each
in its own commit with its own changelog entry. POSIX stays for ports and
external programs.

### 7. Feature parity between subsystems

What SUN can do, POSIX should too, and the other way round, as far as the
platform allows. A gap is closed in the subsystem that lacks it, not worked
around in the program, and a planned port to SUN is no reason to leave POSIX
without it.

### 8. Commit to `master`, and never discard work

No feature branch and no pull request unless the change is experimental enough
to destabilize `master`. No `push --force`, `reset --hard`, `rebase`,
`checkout -- <file>`, `restore`, `clean` or `stash drop` unless that operation
was asked for by name, and no push without being asked. Stage files by name,
never `git add -A`. Changes in the tree the task did not make are left alone.

### 9. A failing test is fixed in the code, and results are reported as they are

Do not weaken, skip or delete an assertion, a golden image, a timeout or a
scenario to make it pass. Report what was run and what it printed; a check
that was not run is never reported as passing. **A build that fails leaves the
previous binary in the image and every scenario still passes against it**:
read the `./build.sh build` output before believing a check.

### 10. Verify before calling it done

Every change: `./build.sh build`, then `./build.sh test`; a change that
touches only documentation needs neither. Then the scenario
for the area, from the table in `AGENTS.md` ("Minimum verification for any
change"): `smoke smoke` for the kernel and POSIX, `smoke native-test` for SUN
and syscalls, `smoke windowd-smoke` for the desktop, `smoke sxgui-smoke` for
the toolkit, the loader and anything that draws. `./build.sh smoke --list`
names the rest.

### 11. Stay inside the task, and read the subsystem's document first

No refactors, renames or reformatting in files the task does not need.
`docs/README.md` lists the document for each area; read it before changing the
area. Microsoft binaries, WADs, logs and captures never enter the repository
(`AGENTS.md`, "Files that never enter the repository").

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
[`docs/build/BUILD_CMAKE.md`](docs/build/BUILD_CMAKE.md) covers the native build path.
