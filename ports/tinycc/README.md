# TinyCC port

TinyCC (`tcc`) running on SavanXP: a C compiler that builds POSIX programs on
the system itself, against the same SDK runtime as `tools/build-user.sh`.

## What gets installed

| Path | What |
| --- | --- |
| `/bin/tcc` | the compiler, a static POSIX program built with the SDK's clang |
| `/system/tcc/include` | TCC's own headers (`stdarg.h`, `stddef.h`, ...) and `stdint.h` from `overlay/` |
| `/system/tcc/sdk/include` | the SDK headers |
| `/system/tcc/lib` | `crt0.o` and `libc.a` (the SDK's `sse` runtime), `libtcc1.a`, `runmain.o` |
| `/bin/tccsmoke.sxe` | the guest check behind `tinycc-smoke` (`tests/tccsmoke.c`, a SUN program) |
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
- `-run` does not load `libc.a`: that would bring its TLS `errno`, which TCC
  refuses in memory. The program calls the libc `tcc` itself is linked with.
  `runsyms.py` lists every global symbol of `libc.a` into a table linked into
  `tcc`, and the static `dlsym` in `tccrun.c` searches it. `runmain.o` gives
  the program its own `exit` and `atexit`, so `exit()` returns to `tcc`.

## Known gaps

- `-ftest-coverage`: `tcov.o` is left out of `libtcc1.a`, because it needs
  `fcntl` record locks, which the SDK does not have.
- Float literals go through the libc's `strtod`, which is not correctly
  rounded in every case.
- TCC limits seen on the SDK samples: no `xmm` clobbers in inline asm, and
  `__ATOMIC_*` only through `<stdatomic.h>`.
- Programs build against POSIX only: the SUN headers are in the tree, but
  nothing has checked a SUN program built by `tcc`.
- A `-run` program shares `tcc`'s libc state (its `stdout` buffer, its heap,
  its `errno`), as on any static `tcc`.

## Testing

```bash
./build.sh smoke tinycc-smoke
```

The scenario runs `build.sh` first (it needs the network for the pinned
clone), boots, and has `tccsmoke` check, with `/bin/tcc`: `tcc -v`; a
one-step build of a program with structs, `long long` and floats; `-c` and a
separate link against `qsort` and `malloc`; `-run` with arguments, `errno`
and a callback from the libc; and a source refused with `file:line: error:`.

## Where it comes from

On the host, with the pinned revision, a cross `tcc` with this patch built 12
of the 16 `sdk/` samples with no options. The other four: `floatsmoke`
(`xmm` clobbers), `nativetest` (`__atomic_*`), `piesmoke` (PIE and `dlopen`)
and `sungui` (a header the build generates).
