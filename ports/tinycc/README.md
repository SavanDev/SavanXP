# TinyCC port

> **Status: draft, not run yet.** `build.sh` and the patch were written from a
> host-side experiment (below) and have not been executed as a port, nor
> booted. Treat everything here as the base to finish, not as working.

TinyCC (`tcc`) running on SavanXP: a C compiler that builds POSIX programs on
the system itself, against the same SDK runtime as `tools/build-user.sh`.

## What gets installed

| Path | What |
| --- | --- |
| `/bin/tcc` | the compiler, a static POSIX program built with the SDK's clang |
| `/system/tcc/include` | TCC's own headers (`stdarg.h`, `stddef.h`, ...) and `stdint.h` from `overlay/` |
| `/system/tcc/sdk/include` | the SDK headers |
| `/system/tcc/lib` | `crt0.o` and `libc.a` (the SDK's `sse` runtime), `libtcc1.a` |
| `/system/licenses` | `tinycc.notice` |

`tcc hello.c -o hello` needs no options: the output is static, starts at
`0x400000` with the ELF headers mapped, and carries `EI_OSABI` System V, the
same shape as a program from `build-user.sh`.

## The `SavanXP` target (`patches/0001-savanxp-target.patch`)

- `TARGETOS_SavanXP`: predefines `__savanxp__`, `__SSE__` and `__SSE2__`,
  and not `__linux__` or `__unix__`, matching the SDK's clang target
  (`x86_64-unknown-none-elf`). `__INT64_TYPE__` is `long`, as there.
  `conftest.c` (c2str) learns to map `__savanxp__` when it compiles
  `tccdefs.h` in.
- Static linking by default; `crt0.o` is the only startup object.
- `-run` gets its memory from one section and two views
  (`CONFIG_SAVANXP_RUNMEM`): code executes from a read-execute view and is
  written through a read-write one. W^X holds and no `mprotect` is needed
  ([SECTIONS.md](../../docs/kernel/SECTIONS.md)).

## Known gaps

- **`-run` does not work yet.** A static `tcc` resolves the program's libc
  calls by loading `libc.a` into memory, which brings the TLS `errno`, and TCC
  refuses TLS under `-run`. It has to resolve them against its own runtime
  instead (its own symbol table).
- `-ftest-coverage`: `tcov.o` is left out of `libtcc1.a`, because it needs
  `fcntl` record locks, which the SDK does not have.
- Float literals go through the libc's `strtod`, which is not correctly
  rounded in every case.
- TCC limits seen on the SDK samples: no `xmm` clobbers in inline asm, and
  `__ATOMIC_*` only through `<stdatomic.h>`.

## Still to do

1. Run `build.sh`, fix what it finds, and check persistence (`/bin/doomgeneric`
   and the WADs survive, AGENTS.md).
2. A guest driver (a SUN program) that runs `tcc` on a sample, runs the
   result and checks it, and a `tinycc-smoke` scenario in
   `tools/smoke_catalog.py`.
3. `-run` symbol resolution (above).
4. A changelog entry and a line in `ports/README.md` once it runs.

## The experiment it comes from

On the host, with the pinned revision: `tcc` built for SavanXP with the SDK
flags links into a 717 KB static ELF with no missing symbols (after the libc
gained `realpath`, `freopen`, `strtold` and `ldexpl`), and a host cross `tcc`
with this patch built 12 of the 16 `sdk/` samples with no options. The
other four: `floatsmoke` (`xmm` clobbers), `nativetest` (`__atomic_*`),
`piesmoke` (PIE and `dlopen`) and `sungui` (a header the build generates).
