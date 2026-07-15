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

## Estado: Fases 0, 1 y 2 + _std — LISTAS (verificadas en QEMU)

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
- **Override `_std`** — **`String` y `Array` reales de Haxe funcionan**: el
  `_std` de reflaxe.CPP se expone como overrides `*.cross.hx` (aislados del
  contexto macro), y el mini std C++ del SDK crece con `<string>`, `<deque>`,
  `<initializer_list>`, `<algorithm>` y `<cctype>` freestanding. Verificado en
  serial: `HAXE NATIVO EN SAVANXP` (concat + toUpperCase), `suma del array=100`,
  `largo de la frase=23`.
- **ABI gfx + hello GUI** — **un programa Haxe pone píxeles en pantalla**:
  syscalls de gráficos primera-clase (`SXN_SYS_GFX_INFO/ACQUIRE/RELEASE/PRESENT`
  en el bloque `0x1010`, sin `/dev/gpu0` ni ioctls) que comparten los internals
  `display::*` y la sesión exclusiva por pid con el mundo posix. La clase
  `Lienzo` de [Main.hx](haxe/Main.hx) dibuja un degradé con un rectángulo sobre
  un `Array<Int>` (contiguo por garantía de nuestro `<deque>`) y lo presenta.
  Verificado en serial: `gfx: pantalla ancho=1280` + `gfx: present=0` +
  `gfx: release=0`, y `gputest --smoke` adquiere la sesión después (el release
  no filtra).
- **Protocolo cliente del compositor** — **apps ventaneadas nativas**: la capa
  `sxn_gui_*` del runtime ([sx_gui.c](sdk/runtime/sx_gui.c)) habla el contrato
  de superficie v3 del compositor sobre los fds 3..9 que el shell instala antes
  del exec (sección compartida + eventos submit/retire/shutdown + input por
  pipe), todo en syscalls del baseline — sin kernel nuevo. La app
  [haxe-gui/Main.hx](haxe-gui/Main.hx) (`nativegui`) abre la sesión, dibuja y
  anima con dirty rects, procesa teclado (fd 4) y **puntero (fd 5)**. Verificado
  headless con el harness [test/guihost.c](test/guihost.c), que interpreta el
  rol del compositor: `frames compuestos=5`, tecla recibida, evento de mouse
  ruteado y marcador dibujado donde apunta, `NATIVEGUI HOST PASS`. Bonus: primer
  test del cambio de subsistema vía **exec** (fork posix → exec ELF nativo).
  Repetible con `.\build.ps1 native-guihost`.

- **Canal de mouse (fd 5)** — **el puntero llega a las apps nativas**: la capa
  `sxn_gui_*` gana `sxn_gui_poll_pointer` (espejo de `gfx_poll_pointer` del SDK
  posix) sobre el fd 5 que el shell instala junto a la sección; entrega
  `sxn_gui_pointer_event {x, y, buttons}` en coordenadas locales a la superficie
  (el shell resta el origen de la ventana en `route_pointer`). Verificado
  end-to-end: el harness envía `(100,80)` con botón izquierdo, el cliente lo
  recibe y pinta un marcador ahí.
- **`Null<T>` y `Map<K,V>`** — **`std::optional` y `std::map` reales**: el mini
  std C++ del SDK creció con `optional`, `map`, `functional`, `type_traits`,
  `utility`, `limits`, `sstream` y extensiones a `memory`/`string`, más un
  shadow de `Std.hx`. Verificado: `Map<String,Int>` con set/get/exists/remove/
  iteración.
- **Float por hardware (SSE)** — **la aritmética de punto flotante de Haxe
  anda**: el kernel ahora gestiona el estado FPU/SSE **por proceso** (habilita
  SSE en el boot y hace FXSAVE/FXRSTOR en el context switch; el kernel es
  `-mno-sse`, no toca la FPU), y el build nativo compila **con** SSE2. Así el
  `/`, la aritmética y las conversiones `Int↔Float` de Haxe usan instrucciones
  SSE reales, sin soft-float. Bonus: vuelve seguro el x87 que ya usaba posix
  (fabr/sin/...), que antes se corrompía bajo preempción. Verificado:
  `100/4=25`, `(0.1+0.2)*1000=300` (IEEE-754 real), `1.5*1.5*100=225`, sin
  regresión en `smoke`/`desktop-smoke`.
- **Fase 3 arrancada — app sxgui-style INTERACTIVA en Haxe**: `sxguiapp`
  ([haxe-sxgui/Main.hx](haxe-sxgui/Main.hx)) dibuja el área cliente de una
  ventana Win9x con un mini-toolkit `Painter` (paleta sxgui + biseles 3D
  `raised`/`sunken`) y **texto real** por la fuente Noto horneada, compartida
  con posix vía [sx_text.c](sdk/runtime/sx_text.c) (`sxn_text_*`). Tiene modelo
  de widgets (`Boton` con estado), **hit-test del puntero** (canal fd 5) y un
  **event loop** que maneja press/release/click con feedback visual (el botón
  se hunde al presionarlo; al soltarlo dispara la acción y enciende una
  "lámpara"). Cliente del compositor como `nativegui`. Verificado headless con
  [test/sxguihost.c](test/sxguihost.c) (`.\build.ps1 native-sxgui`), que
  **maneja** la app: valida el render inicial, envía un press (comprueba que el
  botón se hunde), un release (comprueba lámpara verde + botón levantado) y
  cierre limpio.
- **`aboutapp` portada a Haxe** — **primer reemplazo real de una app en C**:
  [haxe-about/Main.hx](haxe-about/Main.hx) reproduce
  [aboutapp.c](../posix/userland/aboutapp.c): labels, **group boxes** con marco
  etched, **info del sistema** (versión/uptime/procesos/memoria/disco/reloj vía
  [sx_sysinfo.c](sdk/runtime/sx_sysinfo.c), que envuelve las syscalls del
  baseline), y botones **Refrescar**/**Cerrar** (+ F5/ESC). El toolkit
  (`Painter`, `Boton`) se movió a **[haxe-toolkit/](haxe-toolkit/)** (`-cp`
  compartido) y ahora lo usan sxguiapp y aboutapp. Verificado con
  [test/abouthost.c](test/abouthost.c) (`.\build.ps1 native-about`): render +
  info real leída (procesos=4, mem=133 MiB) + Refrescar (hunde/levanta) +
  Cerrar (la app se cierra sola).
- **`filesapp` portada a Haxe (navegador + preview)** —
  [haxe-files/Main.hx](haxe-files/Main.hx): layout de dos paneles como
  [filesapp.c](../posix/userland/filesapp.c). A la izquierda un widget
  **`Listbox`** (scrollable/seleccionable) lista el directorio (con `..`) y se
  navega de verdad — click/Enter entra a un subdirectorio, `..`/Backspace sube,
  ↑↓ mueven la selección. A la derecha un **`Textview`** muestra el **preview**
  de lo seleccionado: ruta, tipo/tamaño y las primeras líneas del archivo
  (saneadas: corta en `\n`, no imprimibles → `.`). Todo el filesystem va por
  [sx_fs.c](sdk/runtime/sx_fs.c) (open/readdir/stat/read del baseline, orden
  dirs-primero). Verificado con [test/fileshost.c](test/fileshost.c)
  (`.\build.ps1 native-files`): render de ambos paneles + selección por teclado
  + **navegación real** (`/` 5 entradas → `/bin` 60 → `/`) + el preview
  siguiendo la selección y **leyendo un archivo real** (`/bin/aboutapp`).
  Tiene además una **barra de menú** (`Menubar` en el toolkit) con Archivo
  (Refrescar/Subir/separador/Salir) y Ayuda, con dropdowns, separadores etched
  y despacho de comandos **por id** (como las tablas de menú de sxgui en C, para
  no depender de closures). El dialog modal de "Acerca de" queda como follow-up
  (por ahora escribe en la barra de estado).

## El contrato ABI (v1)

El contrato tiene **dos capas**, y es el que heredará HashLink en la etapa VM:

1. **Capa kernel** — [sdk/include/savanxp_native_abi.h](sdk/include/savanxp_native_abi.h):
   números de syscall y structs compartidos kernel↔userland. El espacio de
   números está particionado: `[0, 0x0fff]` es el baseline transitorio delegado
   en la tabla posix; `[0x1000, …)` son las syscalls propias del subsistema
   (un proceso posix que las invoque recibe ENOSYS). Syscalls nativas de rango
   propio no implementadas devuelven ENOSYS **sin** pasar por posix. Bloques:
   `0x1000` identidad/log, `0x1010` gráficos (info/acquire/release/present, el
   display como parte del ABI — sin `/dev/gpu0` ni ioctls; misma sesión
   exclusiva por pid que los ioctls GPU de posix, liberada sola al morir el
   proceso).
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
# la app ventaneada:
.\subsystems\native\build.ps1 -Name nativegui -Source haxe-gui -Install
```

`nativegui` instalada aparece en `/disk/bin` y puede lanzarse desde el
escritorio (app de archivos): corre como ventana normal bajo el compositor.

Verificación headless repetible desde la raíz del repo (construyen e instalan
las apps nativas post-rebuild y las corren en QEMU):

```powershell
.\build.ps1 native-hello      # valida el runtime: clases, String/Array, Null<T>, Map, Float, gfx
.\build.ps1 native-guihost    # valida el cliente del compositor: teclado + puntero (fd 5)
.\build.ps1 native-sxgui      # valida la app sxgui-style (Fase 3): toolkit + texto Noto
.\build.ps1 native-about      # valida aboutapp portada: group boxes + info del sistema
.\build.ps1 native-files      # valida filesapp portada: Listbox + navegación de directorios
```

El script:

1. Resuelve `haxe`/`clang`/`clang++`/`ld.lld` por `tools/Toolchain.ps1`
   (env > toolchain horneado > PATH).
2. Clona `reflaxe` y `reflaxe.CPP` pineados (ver `tools/toolchain.lock.json`)
   bajo `toolchain/haxe-libs/` (ignorado por git).
3. Expone el `_std` de reflaxe.CPP como overrides `*.cross.hx` en
   `build/native/std-cross/` (más el `Math.hx` arreglado de `haxe-std-fixes/`).
4. Genera C++ con reflaxe.CPP en `build/native/gen/`.
5. Compila + linkea freestanding contra el runtime nativo de `sdk/` y reusa el
   `crt0.S` + `linker.ld` del SDK posix.

Es un build **aparte** (patrón `sdk/doomgeneric`): el build principal no lo
invoca y, sin `-Install`, no toca `build/disk.img`.

## Layout

- `sdk/include/savanxp_native_abi.h` — **contrato kernel↔userland** (números de
  syscall nativas, structs, versión del ABI). Única fuente; la incluyen el
  kernel y el runtime.
- `sdk/include/savanxp_native.h` — API `sxn_*` del runtime que usa el C++
  generado (identidad/log, I/O baseline, heap, builtins de memoria).
- `sdk/include/cxxstd/` — **mini std C++ freestanding** sobre `sxn_alloc`:
  `__sxn_core` (move/forward, placement new, compartido), `memory`
  (`shared_ptr`/`make_shared`/`unique_ptr`/`make_unique`/`weak_ptr`/
  `enable_shared_from_this`/`static_pointer_cast`/`addressof`, refcount
  no-atómico, una asignación por objeto), `optional` (`std::optional`/
  `std::nullopt` para `Null<T>`; valor plano con flag engaged, `value()` sobre
  vacío aborta ruidoso porque no hay excepciones), `map` (`std::map` para
  `haxe.ds.StringMap`; respaldado por `deque`, búsqueda lineal, NO es un árbol
  ordenado — basta para mapas chicos), `functional` (`std::function` con
  type-erasure sobre el heap), `utility` (`std::pair`), `type_traits` (los
  rasgos que pide `DynamicToString`; los triviales delegan en builtins `__is_`
  de clang), `limits` (`std::numeric_limits`), `sstream` (`std::stringstream`
  mínimo, solo el fallback de formateo de punteros), `string` (`std::string` +
  literales `"..."s` + `to_string` + `stoi`; sin `stof` — `Std.parseFloat`
  quedó stubbeado aunque el Float ya ande, es parsing aparte),
  `deque` (respaldo del `Array<T>`; buffer contiguo, iteradores planos),
  `initializer_list`, `algorithm` (`transform`/`min`/`max`), `cctype` (ASCII) y
  `new`. Sin `dynamic_pointer_cast` (-fno-rtti); crecer bajo demanda — el error
  de compilación es la señal.
- `sdk/runtime/sx_native.c` — syscalls (`int $0x80`), heap free-list (arena BSS
  de 4 MiB, first-fit con split y coalescing) y memcpy/memset/memmove/memcmp.
- `sdk/runtime/sx_cxx.cpp` — `operator new/delete` sobre el heap (OOM = exit
  ruidoso) y `__cxa_pure_virtual`.
- `sdk/runtime/sx_entry.cpp` — entrada propia: handshake de ABI + llamada a la
  `main` generada por Haxe. Reemplaza el `_main_.cpp` de reflaxe.CPP.
- `sdk/include/savanxp_native_gui.h` + `sdk/runtime/sx_gui.c` — cliente del
  compositor: espejos del contrato de superficie v3 (fuente de verdad en el SDK
  posix y desktop.c) y la API `sxn_gui_*` (open/present/present_region/
  poll_event/poll_pointer/should_close) sobre los fds 3..9 heredados del shell.
- `sdk/runtime/sx_text.c` — render de texto (`sxn_text_width`/`sxn_text_height`/
  `sxn_text_draw`) reusando la fuente Noto horneada del SDK posix (include
  relativo de `gfx_font_noto.inc`, fuente de verdad `tools/font/genfont.py`).
  Blit de glifos antialiased a un buffer XRGB. Base del texto del escritorio.
- `sdk/runtime/sx_sysinfo.c` — info del sistema (`sxn_sys_refresh` + getters de
  versión/uptime/procesos/memoria/disco/reloj) envolviendo las syscalls del
  baseline (system_info/proc_info/realtime) que el nativo delega en posix. Lo
  usa el port de aboutapp.
- `sdk/runtime/sx_fs.c` — filesystem: listado de directorios (`sxn_fs_load` +
  `count`/`name`/`is_dir`, con `..` y ordenado), helpers de path (`join`/
  `parent`) y **preview de archivos** (`sxn_fs_size`, `sxn_fs_preview_load` +
  `preview_count`/`preview_line`, que lee el comienzo y lo parte en líneas
  saneadas). Envuelve open/close/readdir/stat/read del baseline. Lo usa
  filesapp.
- `haxe-toolkit/` — **toolkit sxgui compartido** entre las apps GUI nativas
  (`Painter` con paleta/biseles/label/groupbox/texto, `Boton` con estado +
  hit-test, `Listbox` scrollable/seleccionable, `Textview` de solo lectura, y
  `Menubar`/`Menu`/`MenuItem` con dropdowns y comandos por id), agregado al
  `-cp` del build.
- `haxe/Main.hx` — programa Haxe de validación (clase heap + `@:valueType` +
  String/Array + Null<T> + Map + Float + syscalls nativas). `haxe-gui/Main.hx` —
  `nativegui`, la app ventaneada de ejemplo (cliente del compositor).
  `haxe-sxgui/Main.hx` — `sxguiapp`, la app sxgui-style interactiva de la Fase 3.
  `haxe-about/Main.hx` — `aboutapp` portada a Haxe (labels + group boxes + info
  del sistema). `haxe-files/Main.hx` — `filesapp` portada (Listbox + navegación
  de directorios). Todas usan el toolkit compartido de `haxe-toolkit/`.
- `test/guihost.c` — harness POSIX headless que interpreta el rol del
  compositor (sección + eventos + input) y valida al cliente nativo de punta a
  punta. Build: `tools\build-user.ps1 -Source subsystems\native\test\guihost.c
  -Name nativeguihost`. `test/sxguihost.c` — maneja `sxguiapp` (press/release/
  click). `test/abouthost.c` — maneja `aboutapp` (render + Refrescar + Cerrar).
  `test/fileshost.c` — maneja `filesapp` (listbox + selección + navegación real).
- `haxe-support/` — tooling macro del build: `SxnCompilerInit.hx` (envuelve el
  init de reflaxe.CPP y registra nuestro preprocesador) y `UniqueLocalNames.hx`
  (hace únicos los locals por función; ver Hallazgos).
- `haxe-std-fixes/` — shadows del `_std` (se copian como `*.cross.hx` encima de
  los de la lib): `Math.hx` (fix del overload `isFinite` ambiguo en Haxe 4) y
  `Std.hx` (`parseInt` con `std::stoi` propio sin `try/catch`, `parseFloat`
  stubbeado a NaN sin `std::stof` — el original rompe con `-fno-exceptions` y
  reintroduce Float).
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
  init (`reflaxe.ReflectCompiler.Start()` + nuestro `SxnCompilerInit.Start()`,
  que envuelve al de cxxcompiler).
- **El `_std` va como `*.cross.hx`, nunca como `.hx` plano.** Los shadows de
  `_std` (String/Sys/Math…) usan `untyped __cpp__` y guards de plataforma; como
  `.hx` planos en `-cp` también los tipa el **contexto macro/eval** (que compila
  el propio framework reflaxe, el cual usa `haxe.Json`, `Sys`, etc.) y explota
  con errores crípticos (`Ambiguous overload` en Math, `getEnv` inexistente,
  `__cpp__` desconocido). La extensión `.cross.hx` es el mecanismo oficial de
  Haxe para overrides por plataforma: aplica solo al target de generación
  ("cross") y deja el eval limpio. Es lo que hace `haxelib run reflaxe build`
  al empaquetar; el build lo replica copiando/renombrando a
  `build/native/std-cross/`.
- **Los locals de scopes hermanos colisionan al aplanar bloques.** Haxe permite
  reusar nombres en scopes hermanos (cada `for-in` declara su contador `_g`) y
  el codegen de reflaxe.CPP aplana los bloques al emitir → dos `int _g` en el
  mismo scope de C++. El `PreventRepeatVariables` del framework solo mira la
  cadena de scopes anidados. Nuestro preprocesador `UniqueLocalNames` (via
  `ExpressionPreprocessor.Custom`) renombra function-wide. Ojo: **hay que
  reconstruir el árbol con copias del TVar** (mapa por id, como hace el propio
  framework) — mutar `tvar.name` por reflection renombra la declaración pero NO
  los usos (cada acceso macro materializa un objeto distinto) y produce **código
  incorrecto que compila** (el bug se manifestó como `suma del array=0`).
- **`RemovePureExpressions` (pase default de reflaxe) elimina los `if` cuyos
  cuerpos son solo inyecciones `untyped __cpp__`** — los considera puros y
  descarta la rama: otra vez código incorrecto que compila (se manifestó como
  branches de error desaparecidos en la demo gfx, y explica también el chequeo
  devorado de la Fase 2). `SxnCompilerInit` arma la lista de preprocesadores
  SIN ese pase (el DCE fino ya lo hace clang). Aislado por bisección de pases.
- **Float de Haxe anda por hardware (SSE)**: en vez de soft-float (el toolchain
  no trae compiler-rt) se optó por FPU real. El kernel habilita SSE en el boot y
  gestiona el estado FPU/SSE **por proceso** (`arch::x86_64::enable_fpu` +
  `fpu_save`/`fpu_restore` con FXSAVE64/FXRSTOR64 en `switch_to_process`, área
  de 512 B alineada a 16 en `process::Process`, sembrada con un estado limpio en
  `allocate_process_slot`). Como el kernel es `-mno-sse` nunca toca la FPU entre
  medio, así que el save/restore solo hace falta en el cambio real de proceso.
  El build nativo pasó a compilar **con** SSE2 (sacó `-mno-sse`/`-mgeneral-regs-
  only`) y el `crt0` alinea el stack a 16. Nota: `fork` sin `exec` arranca al
  hijo con FPU limpia (no hereda la del padre) — desviación menor de POSIX,
  inocua porque fork va seguido de exec.
- **Un método estático que devuelve su propia clase no compila**: reflaxe.CPP
  emite `static std::shared_ptr<Foo> bar();` en `Foo.h` pero **no agrega el
  `#include <memory>`** en ese caso (sí lo hace cuando `shared_ptr` aparece en
  un campo). Se manifestó al hacer `MenuItem.separador()`; la salida es
  construir con `new` desde el llamador (ver `haxe-toolkit/MenuItem.hx`).
- **`--no-opt` en el hxml**: el analizador de Haxe también const-foldea
  condiciones que dependen de `untyped __cpp__`; se compila sin analizador
  (igual que el CI de reflaxe.CPP) y la optimización queda del lado de clang.

### Limitaciones conocidas / deuda

- **`Map<String,Int>` y `Null<T>` verificados** (mini `<map>`/`<optional>` +
  toda la maquinaria que arrastra `StringMap`). Los otros `*Map` (`IntMap`,
  `ObjectMap`) comparten backing pero no se ejercitaron aún; probablemente
  anden, crecer `cxxstd/` si el codegen pide algo nuevo. `Dynamic` sigue mejor
  evitarlo en código del sistema. `Std.parseFloat` es un stub (devuelve NaN)
  hasta que llegue Float.
- **`trace()` sin cablear**: `haxe.Log` de `_std` probablemente arrastre I/O que
  no tenemos; hoy se loguea con `sxn_log`/`sxn_log_num`. Cablear `trace` →
  `SXN_SYS_LOG` cuando se necesite.
- **`crt0.S`/`linker.ld` son los del SDK posix.** Cuando el ABI nativo diverja,
  el subsistema nativo debería forkear los suyos.

## Próximos pasos

1. **Fase 3** — port incremental del escritorio (hoy en C, ~4.000 líneas). Los
   prerequisitos técnicos ya están: clases + String/Array + `Null<T>` + `Map` +
   cliente del compositor + teclado + puntero. Empezar por una app sxgui-style
   en Haxe.
