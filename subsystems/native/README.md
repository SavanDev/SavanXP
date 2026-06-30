# Subsistema nativo (Haxe)

El subsistema nativo de SavanXP se escribe en **Haxe**, con la visión de largo
plazo de una VM en el SO al estilo Java/ART en Android. Se construye en
**etapas (enfoque híbrido)**:

1. **Etapa actual — AOT a C++** vía [reflaxe.CPP](https://github.com/SomeRanDev/reflaxe.CPP):
   Haxe compila a C++17 mínimo (sin GC, sin dependencias), que linkeamos
   freestanding como un ELF nativo normal. Sirve para validar la cadena,
   diseñar el ABI nativo y portar el escritorio sin construir una VM.
2. **Etapa futura — HashLink** en el SO: una vez que el ABI nativo esté estable,
   portar el runtime/GC de HL y correr el mismo código Haxe sobre la VM.

El ABI nativo se diseña **una sola vez**; AOT y HashLink apuntan al mismo
contrato, así que migrar el runtime no debería tocar el código Haxe.

## Estado: Fases 0 y 1 — LISTAS (verificadas en QEMU)

Cadena probada end-to-end, ejecutándose dentro de SavanXP real:

```
Main.hx  ──haxe──►  reflaxe.CPP  ──►  C++17  ──clang++ freestanding──►  ELF nativo
```

`build.ps1` produce `build/native/nativehello.elf`, un ELF estático de SavanXP
nacido del [Main.hx](haxe/Main.hx) de validación, que escribe "hola desde Haxe"
por stdout vía syscall.

- **Fase 0** — generación + compile/link freestanding del ELF.
- **Fase 1** — el binario corre **como proceso nativo de verdad**: el build
  estampa `e_ident[EI_OSABI]=0x53`, el kernel lo reconoce al cargar la imagen y
  le asigna `subsystem::Id::native`, de modo que sus syscalls entran por
  `dispatch_native_syscall` y no por el de posix. Verificado en el serial log:
  `process: pid=N marcado nativo` + `native: dispatcher activo`.

## Cómo construir

```powershell
.\subsystems\native\build.ps1            # genera build/native/nativehello.elf
.\subsystems\native\build.ps1 -Install   # además lo instala en /disk/bin/nativehello
```

El script:

1. Resuelve `haxe`/`clang`/`clang++`/`ld.lld` por `tools/Toolchain.ps1`
   (env > toolchain horneado > PATH).
2. Clona `reflaxe` y `reflaxe.CPP` pineados (ver `tools/toolchain.lock.json`)
   bajo `toolchain/haxe-libs/` (ignorado por git).
3. Genera C++ con reflaxe.CPP en `build/native/gen/`.
4. Compila + linkea freestanding contra el runtime nativo de `sdk/` y reusa el
   `crt0.S` + `linker.ld` del SDK posix.

Es un build **aparte** (patrón `sdk/doomgeneric`): el build principal no lo
invoca y, sin `-Install`, no toca `build/disk.img`.

## Layout

- `sdk/include/savanxp_native.h` — semilla del ABI nativo: envoltura de syscalls
  y primitivos que usa el C++ generado.
- `sdk/runtime/sx_native.c` — implementación de las syscalls (`int $0x80`).
- `sdk/runtime/sx_entry.cpp` — entrada propia que llama a la `main` generada por
  Haxe. Reemplaza el `_main_.cpp` de reflaxe.CPP (que incluye `<memory>` de
  libstdc++, incompatible con freestanding).
- `haxe/Main.hx` — programa Haxe de validación.
- `kernel/syscall_dispatch.inc` — dispatcher nativo en el kernel. Vivo: delega el
  baseline en posix (el ABI nativo comparte convención por ahora) y es el punto
  de divergencia donde aparecerán las syscalls propias.

## Cómo se marca y enruta un binario nativo (Fase 1)

1. El build estampa `e_ident[EI_OSABI] = 0x53` ('S') en el ELF
   (`elf::kOsAbiNative` / `SXN_ELF_OSABI_NATIVE`). Los binarios posix usan 0.
2. `elf::load_user_image` expone ese byte en `LoadResult.os_abi`.
3. Al cargar la imagen (spawn y exec), el kernel fija `subsystem_id` según el
   ABI del binario — **no** por herencia del padre.
4. `handle_syscall` enruta por `subsystem_id`, así que un proceso nativo entra
   por `dispatch_native_syscall`.

## Hallazgos (importantes para las siguientes fases)

- **El C++ que genera reflaxe.CPP es mínimo**: las clases no arrastran runtime.
  Un programa que evita `trace()` y usa primitivos nativos genera C++ **sin
  ningún include de libstdc++**, lo que permite compilar freestanding.
- **Único acople a libstdc++**: el `_main_.cpp` autogenerado incluye `<memory>`.
  Lo excluimos del build y proveemos `sx_entry.cpp`.
- **El ABI nativo todavía comparte la convención de posix** (mismos números de
  syscall, mismo `int $0x80`). El dispatcher nativo delega en posix; la
  divergencia es trabajo de la Fase 2.
- **reflaxe.CPP es pre-release (v0.1.0)** y finolis de arrancar. Se maneja 100%
  por `-cp` (sin mutar el estado global de `haxelib`), con **dos** macros de
  init (`reflaxe.ReflectCompiler.Start()` + `cxxcompiler.CompilerInit.Start()`).

### Limitaciones conocidas / deuda

- **No se usa el override `std/cxx/_std`** de reflaxe.CPP (Array/String/Map/Math
  versión cxx). Vía `-cp` plano choca con el std de Haxe (overload ambiguo en
  `Math`/`JsonPrinter`). Para programas que usen esos tipos (p. ej. el
  escritorio) hay que registrarlo con semántica de reemplazo (`-lib` apuntando a
  un repo haxelib local bajo `toolchain/`), no con `-cp`.
- **`crt0.S`/`linker.ld` son los del SDK posix.** Cuando el ABI nativo diverja,
  el subsistema nativo debería forkear los suyos.

## Próximos pasos

1. **Fase 2** — diseñar el ABI nativo propio (el contrato que heredará
   HashLink): decidir números de syscall propios vs. reusar la tabla posix, y el
   shim de runtime que mapea las necesidades de Haxe (alloc, I/O, gfx) sobre esas
   syscalls. Empezar a divergir en `dispatch_native_syscall`.
2. Resolver el override `std/cxx/_std` (vía `-lib` con repo haxelib local) para
   habilitar `Array`/`String`/`Map` cxx — prerequisito del port del escritorio.
3. **Fase 3** — port incremental del escritorio (hoy en C, ~4.000 líneas).
