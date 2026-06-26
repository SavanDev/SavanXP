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

## Estado: Fase 0 (puntapié) — LISTO

Cadena probada end-to-end al nivel de compile/link:

```
Main.hx  ──haxe──►  reflaxe.CPP  ──►  C++17  ──clang++ freestanding──►  ELF nativo
```

`build.ps1` produce `build/native/nativehello.elf`, un ELF estático de SavanXP
nacido del [Main.hx](haxe/Main.hx) de validación, que escribe "hola desde Haxe"
por stdout vía syscall.

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
- `kernel/syscall_dispatch.inc` — dispatcher nativo en el kernel (hoy: ENOSYS).

## Hallazgos de la Fase 0 (importantes para las siguientes)

- **El C++ que genera reflaxe.CPP es mínimo**: las clases no arrastran runtime.
  Un programa que evita `trace()` y usa primitivos nativos genera C++ **sin
  ningún include de libstdc++**, lo que permite compilar freestanding.
- **Único acople a libstdc++**: el `_main_.cpp` autogenerado incluye `<memory>`.
  Lo excluimos del build y proveemos `sx_entry.cpp`.
- **Las syscalls son, por ahora, las de posix** (WRITE=1, EXIT=7). Un binario
  nativo lanzado por un padre posix corre con identidad posix, así que funciona.
  El dispatcher nativo todavía responde ENOSYS.
- **reflaxe.CPP es pre-release (v0.1.0)** y finolis de arrancar. Se maneja 100%
  por `-cp` (sin mutar el estado global de `haxelib`), con **dos** macros de
  init (`reflaxe.ReflectCompiler.Start()` + `cxxcompiler.CompilerInit.Start()`).

### Limitaciones conocidas / deuda para Fase 1+

- **No se usa el override `std/cxx/_std`** de reflaxe.CPP (Array/String/Map/Math
  versión cxx). Vía `-cp` plano choca con el std de Haxe (overload ambiguo en
  `Math`/`JsonPrinter`). Para programas que usen esos tipos (p. ej. el
  escritorio) hay que registrarlo con semántica de reemplazo (`-lib` apuntando a
  un repo haxelib local bajo `toolchain/`), no con `-cp`.
- **`crt0.S`/`linker.ld` son los del SDK posix.** Cuando el ABI nativo diverja,
  el subsistema nativo debería forkear los suyos.

## Próximos pasos

1. Correr el ELF dentro de SavanXP (QEMU) y confirmar la salida "hola desde Haxe".
2. **Fase 1** — marcar procesos como nativos en el loader (hoy todo hereda
   posix) y hacer que el dispatcher nativo atienda al menos una syscall real.
3. **Fase 2** — diseñar el ABI nativo (el contrato que heredará HashLink) y el
   shim de runtime que mapea las necesidades de Haxe sobre esas syscalls.
4. **Fase 3** — port incremental del escritorio (hoy en C, ~4.000 líneas).
