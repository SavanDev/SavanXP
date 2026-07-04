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

## Estado: Fases 0, 1 y 2 — LISTAS (verificadas en QEMU)

Cadena probada end-to-end, ejecutándose dentro de SavanXP real:

```
Main.hx  ──haxe──►  reflaxe.CPP  ──►  C++17  ──clang++ freestanding──►  ELF nativo
```

`build.ps1` produce `build/native/nativehello.elf`, un ELF estático de SavanXP
nacido del [Main.hx](haxe/Main.hx) de validación.

- **Fase 0** — generación + compile/link freestanding del ELF.
- **Fase 1** — el binario corre **como proceso nativo de verdad**: el build
  estampa `e_ident[EI_OSABI]=0x53`, el kernel lo reconoce al cargar la imagen y
  le asigna `subsystem::Id::native`, de modo que sus syscalls entran por
  `dispatch_native_syscall` y no por el de posix.
- **Fase 2** — **ABI nativo v1 + runtime real**: syscalls propias
  (`SXN_SYS_INFO`/`SXN_SYS_LOG` en el rango `0x1000+`), handshake de versión de
  ABI al arrancar el runtime, heap propio (`sxn_alloc`) con `operator new/delete`
  y un mini `<memory>` freestanding — con lo cual **las clases Haxe con
  semántica por defecto (shared) y `@:valueType` ya funcionan** sin libstdc++.
  Verificado en serial:
  `native[4]: runtime: abi verificado, version=1` +
  `main: contador (clase heap)=3` + `main: punto.suma (valueType)=7`.

## El contrato ABI (v1)

El contrato tiene **dos capas**, y es el que heredará HashLink en la etapa VM:

1. **Capa kernel** — [sdk/include/savanxp_native_abi.h](sdk/include/savanxp_native_abi.h):
   números de syscall y structs compartidos kernel↔userland. El espacio de
   números está particionado: `[0, 0x0fff]` es el baseline transitorio delegado
   en la tabla posix; `[0x1000, …)` son las syscalls propias del subsistema
   (un proceso posix que las invoque recibe ENOSYS). Syscalls nativas de rango
   propio no implementadas devuelven ENOSYS **sin** pasar por posix.
2. **Capa runtime** — [sdk/include/savanxp_native.h](sdk/include/savanxp_native.h):
   la API `sxn_*` que consume el C++ generado por reflaxe.CPP (identidad, log,
   I/O, heap). Cuando llegue HashLink, la VM implementará estas mismas
   primitivas sobre el mismo ABI y el código Haxe no cambia.

Al arrancar, el runtime hace el handshake `sxn_info()` y aborta (exit 132) si
`abi_version` no coincide con su `SXN_ABI_VERSION`.

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

- `sdk/include/savanxp_native_abi.h` — **contrato kernel↔userland** (números de
  syscall nativas, structs, versión del ABI). Única fuente; la incluyen el
  kernel y el runtime.
- `sdk/include/savanxp_native.h` — API `sxn_*` del runtime que usa el C++
  generado (identidad/log, I/O baseline, heap, builtins de memoria).
- `sdk/include/cxxstd/memory` — mini `<memory>` freestanding: `shared_ptr` /
  `make_shared` (refcount no-atómico, una asignación por objeto) sobre
  `sxn_alloc`. Sin `weak_ptr` ni `dynamic_pointer_cast` (-fno-rtti); ampliar
  cuando el codegen lo pida.
- `sdk/runtime/sx_native.c` — syscalls (`int $0x80`), heap free-list (arena BSS
  de 4 MiB, first-fit con split y coalescing) y memcpy/memset/memmove/memcmp.
- `sdk/runtime/sx_cxx.cpp` — `operator new/delete` sobre el heap (OOM = exit
  ruidoso) y `__cxa_pure_virtual`.
- `sdk/runtime/sx_entry.cpp` — entrada propia: handshake de ABI + llamada a la
  `main` generada por Haxe. Reemplaza el `_main_.cpp` de reflaxe.CPP.
- `haxe/Main.hx` — programa Haxe de validación (clase heap + `@:valueType` +
  syscalls nativas).
- `kernel/syscall_dispatch.inc` — dispatcher nativo en el kernel: switch propio
  para el rango `0x1000+` y baseline `< 0x1000` delegado en posix.

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
  Las clases por defecto usan `std::shared_ptr`/`std::make_shared` (resuelto por
  nuestro mini `<memory>`); las `@:valueType` son valores planos sin heap.
- **La convención de llamada se comparte con posix** (`int $0x80`, rax/rdi/rsi/
  rdx), pero el espacio de números ya está particionado: las syscalls nativas
  viven en `0x1000+` y solo existen detrás del dispatcher nativo.
- **El analizador de Haxe const-foldea agresivo**: chequeos sobre valores
  estáticamente conocidos desaparecen del C++ generado. Las validaciones de
  runtime reales deben vivir en el runtime (p. ej. el handshake de ABI en
  `sx_entry.cpp`), no en el código Haxe de prueba.
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

1. Resolver el override `std/cxx/_std` (vía `-lib` con repo haxelib local) para
   habilitar `Array`/`String`/`Map` cxx — prerequisito del port del escritorio.
   Implica también proveer los headers/funciones que ese std pida (probablemente
   `<string>`, `<optional>`, `<deque>` mínimos en `cxxstd/`).
2. Crecer el ABI nativo por necesidad real: gfx (surface/present sobre el ABI
   gfx existente), input, tiempo — cada syscall nueva con un consumidor en el
   programa de validación.
3. **Fase 3** — port incremental del escritorio (hoy en C, ~4.000 líneas).
