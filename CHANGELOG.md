# Changelog

Registro de cambios visibles por version para SavanXP.

Notas de corte:

- `v0.1.0` se reconstruyo de forma retroactiva desde el historial hasta `d822857`.
- `v0.1.1` cubre los cambios posteriores a `v0.1.0`, incluyendo el trabajo actual
  ya integrado en el arbol pero todavia no etiquetado en git.

## [Unreleased]

### Agregado

- **Compositor grafico separado (`/bin/compositord`).** El acceso directo a
  `/dev/gpu0`, import de la superficie de display, presents batched, timeline de
  present y cursor hardware se movieron a un daemon userland propio. `desktop`
  arranca el daemon con pipes y una seccion de framebuffer heredada, y habla un
  protocolo binario versionado definido en
  `subsystems/posix/sdk/v1/include/savanxp/compositor_protocol.h`. El shell de
  escritorio conserva politica de ventanas, menu/taskbar y routing de input, pero
  ya no abre `/dev/gpu0` ni ejecuta ioctls GPU directamente.
- **GPU HAL: backend de display intercambiable.** `namespace display` pasa de ser
  un passthrough fijo a `virtio_gpu` a una indireccion real via `display::Backend`
  (vtable). El registro de `/dev/gpu0` y todo `gpu_ioctl` se movieron a un
  dispatcher agnostico del backend (`kernel/gpu_device.cpp`) que se registra
  siempre y delega la operacion de hardware al backend activo. Nuevo backend
  `kernel/fb_gpu.cpp`: compositor por software directo al framebuffer lineal de
  Limine, sin dispositivo PCI, para correr cuando no hay virtio-gpu (VirtualBox /
  QEMU `-vga std`). `kernel_main` autodetecta: virtio si el probe lo encuentra, si
  no el framebuffer plano. El driver `virtio_gpu` no cambia su logica interna. El
  pendiente historico de F11 sobre framebuffer plano queda resuelto por
  fullscreen compositado por software en el shell.
- **Fullscreen compositado para apps (tecla F11).** Una app marcada como
  fullscreen-capable (Doom, Gfx Demo) pasa a pantalla completa sin chrome: el
  shell escala el buffer 640x400 de la app al framebuffer compartido y presenta el
  resultado via `compositord`, sin cambio de modo ni flip directo de scanout de
  cliente. Esto funciona tanto sobre virtio-gpu como sobre el backend framebuffer
  plano. Las apps fullscreen-capable mantienen un buffer 640x400 tight (la misma
  superficie en ventana y fullscreen). Se conserva el layout v3 con
  `pixels_offset` page-aligned por compatibilidad ABI.
- **Subsistema nativo (Haxe) — puntapie de Fase 0.** Cadena probada end-to-end
  al nivel de compile/link: `Main.hx` -> `reflaxe.CPP` -> C++17 -> clang++
  freestanding -> ELF nativo de SavanXP. Nuevo `subsystems/native/` con un SDK
  semilla (`savanxp_native.h`, envoltura de syscalls `sx_native.c`, entrada
  propia `sx_entry.cpp` que reemplaza el `_main_.cpp` de reflaxe.CPP para evitar
  `<memory>` de libstdc++), un programa Haxe de validacion y un `build.ps1`
  aparte (patron `sdk/doomgeneric`) que clona reflaxe/reflaxe.CPP pineados bajo
  `toolchain/haxe-libs/`, genera el C++ y lo linkea contra el `crt0.S`/`linker.ld`
  del SDK posix. `haxe`/`haxelib` se resuelven via `tools/Toolchain.ps1`.
- **Subsistema nativo — Fase 1: procesos nativos reales.** Un binario nativo se
  marca con `e_ident[EI_OSABI]=0x53` (estampado por el build), el loader lo
  reconoce al cargar la imagen (`elf::LoadResult.os_abi`) y le asigna
  `subsystem::Id::native` segun el ABI del binario, no por herencia del padre
  (aplica a spawn y exec). Sus syscalls entran por `dispatch_native_syscall`, que
  pasa de ENOSYS a estar vivo: delega el baseline en posix (el ABI nativo todavia
  comparte convencion) y queda como punto de divergencia. Verificado en QEMU: el
  ELF Haxe corre con identidad nativa (`process: pid=N marcado nativo` +
  `native: dispatcher activo`) e imprime su salida sin romper el smoke test.
- Tipografias reales horneadas offline a tablas C con `tools/font/genfont.py`
  (via `freetype-py`): **GNU UniFont 8x16** para la consola del kernel y el render
  monospace del terminal, y **Noto Sans** proporcional antialiased para el chrome
  del escritorio y los widgets. El SO sigue sin parsear TrueType en runtime.
- Camino de texto monospace en `sxgfx` (`gfx_blit_text_mono`, `gfx_cell_width/height`)
  y alpha-blending por pixel (`gfx_pixel_blend`) para el texto antialiased de Noto.
- Interrupciones reales por **MSI-X** en `virtio-gpu`: el driver recibe las
  completions por interrupcion (vector del local-APIC) con un patron ISR/DPC
  (la ISR solo marca trabajo y el servicio en background lo drena), en lugar del
  polling puro. El kernel no tiene IOAPIC y q35 en modo APIC no entrega el INTx
  legacy, por lo que MSI-X es el unico camino. Incluye `pci::find_capability`,
  `virtio_pci::enable_msix` y un vector/gate de IDT dedicado (49).
- `poll()` reporta readiness para objetos waitables del kernel (eventos, timers),
  de modo que el compositor puede esperar los eventos de submit de sus clientes
  en el mismo poll set.
- Overlay en pantalla de FPS y latencia de present (promedio/pico de ms
  bloqueado en `gfx_present_region`) en el backend de Doom, estampado directo en
  el framebuffer; y volcado de las stats por etapa del driver (end-to-end,
  transfer/flush/scanout, esperas, timeouts, notificaciones de IRQ) en
  `gputest --soak`.

### Cambiado

- El compositor del escritorio compone por **regiones con culling por oclusion**:
  en vez de repintar todas las capas (fondo, clientes, taskbar, menu, cursor) por
  cada rectangulo sucio, arma la lista de capas en z-order y pinta cada una una
  sola vez sobre su region visible (`damage` interseccion `bounds` menos los
  oclusores opacos delante), eliminando el overdraw bajo ventanas opacas. Nuevo
  primitivo de resta de rectangulos `sx_rect_set_subtract_rect` en `sxgfx`; la
  capacidad de `sx_rect_set` sube de 32 a 64.
- La consola del kernel pasa de la fuente bitmap 5x7 autorada a UniFont 8x16, con
  cobertura ASCII + Latin-1 + dibujo de cajas/bloques via una tabla dispersa
  (`include/kernel/console_font_unifont.inc`).
- `gfx_blit_text` ahora rasteriza Noto Sans desde un atlas de cobertura 8-bit; el
  terminal `shellapp` usa el camino monospace UniFont. Se reajustaron las metricas
  de chrome del desktop (`desktop_layout.h`, menu Inicio, accesos directos) para la
  altura de linea mayor.
- UniFont se hornea desde `unifont.hex` (bitmaps nitidos en grilla), no desde el
  outline TTF, que rasterizaba fuera de grilla con artefactos.
- Timer del kernel de 200 Hz a **1000 Hz**, con el quantum del scheduler
  reescalado para mantener ~20 ms de reloj de pared. Senalizar un evento ahora
  cede la CPU al thread despertado en el retorno del syscall (wakeup preemptivo)
  en vez de esperar el proximo tick, recortando la latencia del handshake
  compositor<->cliente.
- El compositor despierta ante el submit de frame de un cliente (sus eventos de
  submit entran al `poll` set) en lugar de agotar el timeout de 16 ms, que queda
  como respaldo.
- El driver `virtio-gpu` pasa a ser interrupt-driven: el spin activo de los
  waiters de present se recorta (50000 a 2000 iteraciones) antes de halt-ear
  (despertado por la IRQ de MSI-X), liberando la CPU; los timeouts de respaldo
  del driver se expresan en milisegundos de reloj de pared (convertidos con la
  frecuencia viva del timer) para sobrevivir al cambio de tick rate.

### Eliminado

- Sistema de fuente 8x8 generado a mano (`tools/font/genfont.ps1`, `font8x8.txt`,
  `gfx_font8x8.inc`), reemplazado por el toolchain de `genfont.py`.

### Corregido

- `release_surface_allocation` (`virtio-gpu`) liberaba el backing de la superficie
  primary sin hacer RESOURCE_UNREF del recurso host, por lo que re-ejecutar
  `configure_primary_surface` en runtime (el primer cambio de modo real via
  `GPU_IOC_SET_MODE`, antes nunca ejercitado) fallaba con `RESOURCE_CREATE_2D` ->
  INVALID_RESOURCE_ID. Ahora destruye el recurso host antes de liberar el backing.
- `decode_bar_size` calculaba el complemento de tamano (`~mask + 1`) en 64 bits
  sobre una mascara con solo los 32 bits bajos, devolviendo tamanos basura
  (`0xffffffff00001000` en vez de `0x1000`) para cualquier BAR de memoria de
  64 bits menor a 4 GiB e impidiendo mapearlos (entre ellos la tabla MSI-X).
  Ahora complementa en 32 bits cuando el tamano entra en 4 GiB.
- `Get-Svfs2BitmapBit`/`Set-Svfs2BitmapBit` (instalador host-side de SVFS2 en
  `tools/UserAppCommon.ps1`) calculaban el indice de byte con `[int]($Bit / 8)`,
  que en PowerShell devuelve un `double` y `[int]` redondea (banker's rounding)
  en vez de truncar: para cualquier bit con `bit % 8 >= 5` (y algunos `= 4`) el
  bit caia en el byte equivocado, desincronizando el inode/block bitmap con el
  indexado floor del kernel (`kernel/svfs.cpp`). El kernel veia esos inodos como
  libres y los reasignaba durante la operacion (p. ej. archivos temporales del
  smoke), zerando el inodo del archivo original y dejando la entrada de
  directorio colgante (sintoma: "inode esperado N pero se leyo 0" tras un ciclo
  de boot). Ahora usan division entera (`-shr 3`).
- Apagado ordenado de QEMU en `Run-AutomationQemu` (smoke/selftest/soak): en vez
  de `Stop-Process -Force`, se pide `quit` por el monitor HMP para que QEMU
  vacie sus backends de bloque y cierre el archivo de disco limpiamente, con
  fallback al kill forzado si el monitor no responde.

## [0.3.0] - 2026-06-18

### Agregado

- Smoke grafico headless automatizado para el compositor: `desktop --selftest`
  arranca el compositor, importa el scanout, lanza un cliente real y valida de
  forma determinista la composicion multi-window y el avance de la timeline de
  presents del GPU (submit/retire), ejercitando ademas las rutas de
  maximizar/restaurar/minimizar/mover ventana. Expuesto como
  `.\build.ps1 desktop-smoke` (token `DESKTOP SMOKE PASS/FAIL`) y enrutado desde
  `init` via el spec `/SMOKE` `desktop-selftest`, cerrando el hueco de validacion
  del camino grafico que antes solo se probaba a mano.
- Apps cliente nuevas del desktop: `filesapp` (navegacion de `/disk` con preview)
  y `aboutapp` (resumen del sistema), cableadas en el menu Inicio.
- Politica y trazabilidad explicita para adopcion de componentes inspirados en
  SerenityOS, con documentacion nueva en `docs/THIRD_PARTY_ADOPTION.md` y
  `docs/THIRD_PARTY_PROVENANCE.md`.
- Capa grafica 2D reutilizable `sxgfx` en el SDK POSIX v1, con `sx_bitmap`,
  `sx_painter`, alpha blending, clipping y `sx_rect_set` para manejar damage
  multiple desde userland.
- Fachada `display` inspirada en `DisplayConnector` sobre el backend
  `virtio-gpu`, con propiedades de conector, import/release de surfaces,
  batching de presents, timeline y eventos waitables exportables a userland.
- Contrato grafico `SAVANXP_GPU_CLIENT_SURFACE_VERSION_3` para apps cliente,
  basado en `section_create/map_view` mas batches de dirty rects y eventos
  `submit/retire/shutdown` en lugar del pipe legacy de presents.
- ABI publica nueva y ampliada en `/dev/gpu0` para seguimiento explicito de
  presents, batching y capacidades del conector, incluyendo timeline,
  waits/events y consultas de propiedades/scanouts sin ownership del display.
- Compositor de escritorio multi-window real, con overlays simultaneos,
  z-order simple, ventana activa, arrastre desde la titlebar y boton de cierre
  en la esquina superior derecha.
- Pipeline de assets bitmap propio para el desktop, con iconos PNG embebidos y
  arte generado dentro del repo para taskbar, menu Inicio, titlebars y franja
  lateral, eliminando la dependencia de assets de SerenityOS.
- Estadisticas ampliadas de `virtio-gpu` con latencia end-to-end por frame,
  incluyendo muestras acumuladas y peor caso observado.

### Cambiado

- El sistema pasa a reportarse como `v0.3.0` en kernel, shell, `uname`,
  `sysinfo`, `aboutapp` y demas componentes que consumen la version compartida.
- El progreso en background de `virtio-gpu` deja de depender del subsistema de
  input y pasa a bombearse desde un servicio de dispositivos del kernel
  invocado en timer y waits bloqueantes.
- El `desktop` deja atras el modelo de un solo cliente fullscreen y pasa a un
  compositor por surfaces con invalidacion por multiples rectangulos,
  composicion por clipping y presents batched hacia el scanout principal.
- El loop principal del `desktop` se desacopla en layout/render/menu/session,
  manteniendo el binario unico pero separando mejor la responsabilidad del
  compositor, el shell de fondo y las ventanas overlay.
- `shellapp`, `doomgeneric` y las demas apps cliente migran al canal async de
  surfaces version 3; el terminal fuerza redraw completo cuando el scroll mueve
  el historial para evitar artefactos visuales en la ventana.
- La sincronizacion compositor-GPU pasa a usar timeline explicita y retiro del
  frame anterior antes de reciclar el backbuffer visible, reduciendo tearing
  logico y mejorando el pacing del desktop.
- El menu Inicio y la taskbar reciben varias pasadas de pulido visual y de
  comportamiento: layout mas limpio, sidebar bitmap, textos que caben mejor,
  hover estable y mejor feedback del cliente activo.
- Las ventanas overlay ya pueden moverse dentro del area util del desktop y
  cerrarse directamente desde su titlebar con una cruz clasica estilo shell.
- El arte embebido del desktop pasa a generarse desde assets propios del repo,
  sustituyendo las referencias temporales usadas durante el prototipado
  inspirado en SerenityOS.
- `gputest --smoke` ahora valida tambien la timeline de presents y el pacing
  explicito del driver, no solo stats/stages internos.
- El flujo de `build/disk.img` persistente se vuelve a validar despues de cada
  tanda grande de cambios con `doomgeneric` como prueba real de no regresion.
- `virtio-gpu` reorganiza su estado interno alrededor de un `Adapter` con
  subestados separados para transporte, display, cursor, presents y runtime,
  dejando mejor preparada la base para locking fino y recovery mas predecible.
- El trabajo en background de `virtio-gpu` queda separado en fases explicitas
  de drain de colas, avance del pipeline y procesamiento de eventos de config,
  con serializacion atomica corta para submit/drain de virtqueues.
- La timeline de presents pasa a reconocer `present_cookie` como correlacion
  real tambien cuando hay coalescing, retiro por rangos y batching de damage.
- El camino parcial de presentacion ya puede actualizar el front buffer activo
  sin clone completo cuando el recurso esta idle, y los batches importados
  conservan rects reales para `TRANSFER_TO_HOST_2D` y `RESOURCE_FLUSH` antes
  de caer a bounding rect solo si se agota la capacidad interna.
- El `desktop` empieza a usar el evento waitable de present exportado por
  `/dev/gpu0` como hint de readiness para reducir polling innecesario de la
  timeline, manteniendo `WAIT_PRESENT` como sincronizacion fuerte.
- Los eventos de display/scanout de `virtio-gpu` endurecen su handling:
  un refresh fallido ya dispara degradado y recovery deliberado en lugar de
  quedar como fallo silencioso del camino de hotplug/config.
- `gputest --smoke` sube la cobertura del driver validando handles waitables de
  present y scanout, junto con un soak liviano de presents parciales y refreshes
  repetidos para detectar regresiones de pacing y recovery mas temprano.
- Los timeouts de reserva de superficie y de slot pending en `virtio-gpu`
  pasan a tratarse como sintomas de atasco real del pipeline, entrando en modo
  degradado e intentando recovery igual que los otros waits criticos.
- El recovery de `virtio-gpu` evita reentradas simultaneas y `gputest --smoke`
  endurece la cobertura de eventos comprobando tambien que los handles vuelvan
  a quedar sin senal despues de `event_reset`, con un soak algo mas agresivo.
- `gputest` agrega un modo dedicado `--soak` para ejercer durante mas tiempo el
  backend `/dev/gpu0` con mezcla determinista de full presents, partial presents,
  waits por evento y refreshes de scanout, sin volver mas lenta la smoke normal.
- `build.ps1` agrega el comando `gpu-soak`, reutilizando `/SMOKE` como selector
  de runner para que `init` pueda lanzar `gputest --soak` en QEMU y reportar
  tokens `SOAK PASS/FAIL` aptos para validacion automatizada.
- El recovery de `virtio-gpu` deja de ser tan global en dominios no criticos:
  un fallo al rearmar el cursor ahora degrada a software cursor y las imported
  surfaces no criticas pueden descartarse localmente durante recovery en lugar
  de voltear toda la rearmada del dispositivo.
- Los refreshes de scanout de `virtio-gpu` pasan a ser transaccionales sobre el
  cache de conectores: si el host entrega un evento incompleto o falla el rearm
  del primary, el driver restaura el estado anterior y solo escala a recovery
  global cuando ni siquiera puede mantener el scanout primario.
- `gputest --soak` ahora acepta cantidad de iteraciones y cubre tambien imported
  surfaces mediante `GPU_IOC_IMPORT_SECTION` y `GPU_IOC_PRESENT_SURFACE_BATCH`,
  con `build.ps1 gpu-soak -GpuSoakIterations N` para repetir tandas mas largas
  sin tocar la smoke normal.
- El compositor `desktop` corrige el pacing de sus presents importados usando la
  timeline real para generar `present_cookie`, evitando retirar frames antes de
  que el GPU termine y reduciendo el tearing visible en apps pesadas.
- El runtime `gfx` del SDK deja de exponer como backbuffer directo el mismo
  buffer compartido que lee el compositor: cada cliente dibuja en un backbuffer
  privado y el runtime copia al surface compartido solo cuando el frame anterior
  ya fue compuesto, eliminando backlog de frames sin esperar el retiro final del
  GPU.
- El protocolo de surfaces cliente agrega `composed_sequence` para separar
  composicion del desktop y retiro del GPU; el compositor senala progreso apenas
  copia el frame al backbuffer visible y conserva `retired_sequence` para el
  retiro real del present.
- El `desktop` limita el drenaje de eventos de mouse por frame para que arrastrar
  ventanas no acumule un backlog grande antes de volver a componer.
- `poll()` deja de tratar todos los devices como siempre legibles: `/dev/input0`
  y `/dev/mouse0` exponen readiness real de cola, reduciendo el busy-loop del
  compositor cuando no hay eventos pendientes.
- El manejo de mouse del `desktop` pasa a leer bloques por frame y coalescer
  movimientos consecutivos con el mismo estado de botones, siguiendo el patron
  de WindowServer de SerenityOS para no componer un frame por cada paquete crudo.
- `sx_rect_set` corrige el merge de rectangulos adyacentes para no unir areas
  separadas que solo coinciden en una coordenada de borde, evitando dirty rects
  artificialmente enormes al mover ventanas.
- `doomgeneric` y el compositor imprimen ahora el error concreto de present o
  batch invalido cuando una surface cliente falla, facilitando diagnostico de
  regresiones visuales.

## [0.2.2] - 2026-04-01

### Agregado

- Arbol nuevo `subsystems/` con `subsystems/posix` como primer subsistema
  explicito del SO, separando `kernel`, `userland` y `sdk` bajo un mismo
  ownership.

### Cambiado

- La entrada y el dispatcher POSIX de syscalls pasan a vivir bajo
  `subsystems/posix/kernel`, mientras `kernel/` conserva el scheduler,
  procesos base, VM, VFS, drivers y demas mecanismos genericos.
- El SDK canonico v1 pasa a `subsystems/posix/sdk/v1`; el build principal,
  `tools/build-user.ps1` y la extension de VS Code consumen ahora esa ruta
  como referencia publica del subsistema POSIX.
- El userland interno se mueve a `subsystems/posix/userland` y compila contra
  el runtime canonico compartido del SDK, eliminando duplicados internos y
  dejando `sdk/` top-level como raiz de ejemplos y ports.
- La definicion publica del ABI visible queda unificada en
  `subsystems/posix/sdk/v1/include/savanxp/syscall.h`, sin copia paralela en
  `include/shared`.
- El sistema pasa a reportarse como `v0.2.2` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- La migracion vuelve a validar el flujo de imagen persistente:
  `.\sdk\doomgeneric\build.ps1` y `.\build.ps1 build` siguen conservando
  `doomgeneric` y `doom1.wad` en `build/disk.img` sin recreacion normal de la
  imagen.

## [0.2.1] - 2026-03-30

### Agregado

- ABI publica nueva en `/dev/gpu0` para diagnostico y control 2D extendido:
  `GPU_IOC_GET_STATS`, `GPU_IOC_GET_SCANOUTS`,
  `GPU_IOC_REFRESH_SCANOUTS`, `GPU_IOC_SET_CURSOR` y
  `GPU_IOC_MOVE_CURSOR`.
- Estadisticas ampliadas de `virtio-gpu` para presents, stages, waits,
  timeouts, completions, IRQs, recovery y operaciones de cursor.
- Enumeracion de scanouts y refresh basico de display info/hotplug en el
  backend `virtio-gpu`, manteniendo `desktop` single-display por defecto.
- Soporte inicial de cursor plane por hardware en `virtio-gpu`, con fallback
  transparente al cursor software del `desktop` cuando el backend no lo
  expone o falla.
- Coverage automatizado adicional en `gputest --smoke` para validar progreso
  del driver via `GPU_IOC_GET_STATS` y enumeracion de scanouts.

### Cambiado

- El sistema pasa a reportarse como `v0.2.1` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- El modelo grafico normal queda definitivamente desktop-first: la taskbar
  permanece visible, las apps cliente renderizan sobre un work area estable y
  el `desktop` pasa a ser el dueño normal del scanout.
- `shellapp`, `gfxdemo`, `keytest`, `mousetest` y `doomgeneric` quedan
  alineados al camino cliente del compositor en vez del fullscreen directo
  legacy sobre `/dev/gpu0`.
- La taskbar y el menu Inicio reciben una pasada de pulido visual y de
  comportamiento para encajar mejor con el nuevo contrato desktop-first.
- El backend `virtio-gpu` deja de depender de reentradas oportunistas del
  caller para progresar: el scheduler interno ahora coalescea presents por
  recurso, reduce `SET_SCANOUT` redundantes y avanza trabajo en segundo plano
  con apoyo de IRQ cuando la linea PCI esta disponible.
- `virtio-gpu` agrega recovery deliberado y modo degradado predecible frente
  a timeouts del device, intentando restaurar el scanout primario y la
  consola sin exigir reinicio inmediato del SO.
- El build principal y la tooling asociada dejan mas explicito que
  `build/disk.img` es persistente por defecto: se valida consistencia de
  `SVFS2`, se evita recrear la imagen salvo corrupcion real y se conserva
  `doomgeneric` junto con `doom1.wad` como regresion practica de
  persistencia.
- Los perfiles de QEMU usados por `run`, `smoke` y las utilidades graficas se
  alinean mejor con el hardware virtual real del stack actual
  (`virtio-gpu` + `virtio-tablet` + `desktop`).
- `doomgeneric` pasa a vivir definitivamente como cliente del compositor y
  queda orientado a validacion manual dentro de la sesion grafica normal,
  en vez de depender de una smoke host-side propia.

## [0.2.0] - 2026-03-22

### Agregado

- Sesion `desktop-first` con compositor `desktop`, shell fullscreen
  `shellapp`, surfaces de cliente compartidas por `SectionObject` y
  lanzamiento de apps graficas via `fd 3..6`.
- ABI publica extendida en `/dev/gpu0` con `GPU_IOC_SET_MODE`,
  `GPU_IOC_IMPORT_SECTION`, `GPU_IOC_RELEASE_SURFACE`,
  `GPU_IOC_PRESENT_SURFACE_REGION` y `GPU_IOC_WAIT_IDLE`.
- ABI publica nueva para audio con `SAVANXP_IOCTL_GROUP_AUDIO`,
  `AUDIO_IOC_GET_INFO` y `struct savanxp_audio_info`.
- Driver `virtio-sound` playback-only sobre `virtio_pci`, exponiendo
  `/dev/audio0` con formato fijo `S16LE stereo 48 kHz`.
- Utilidad nueva `audiotest` y coverage automatizado en `.\build.ps1 smoke`
  para validar `/dev/audio0`.
- Object manager minimo con handles kernel genéricos para I/O, eventos,
  timers y sections.
- Syscalls nuevas `EVENT_*`, `WAIT_ONE`, `WAIT_MANY`, `TIMER_*`,
  `SECTION_CREATE`, `MAP_VIEW` y `UNMAP_VIEW`, con wrappers actualizados en
  userland y SDK v1.
- Soporte inicial de `Section/View` anónimo en el kernel, incluyendo memoria
  compartida entre procesos, herencia `shared` vs `private` al hacer `fork()`
  y tests nuevos `eventtest`, `timertest`, `sectiontest` y `mmaptest`.
- Capa POSIX nueva para `mmap` / `munmap` anónimo en `subsystems/posix/sdk/v1`, más header
  estándar `sys/mman.h`.

### Cambiado

- El sistema pasa a reportarse como `v0.2.0` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- El arranque normal pasa a supervisar `desktop` desde `init`; `/SMOKE` sigue
  evitando el desktop y mantiene la smoke automatizada headless.
- `gfx_open` y el runtime grafico pasan a ser compositor-first, con fallback
  directo sobre `/dev/gpu0`; el nodo legacy `/dev/fb0` deja de exponerse en
  el sistema actual.
- El build principal ahora instala el multicall BusyBox portado en `/bin` y
  `/disk/bin` para `ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp` y
  `ps`.
- `virtio-gpu` pasa a presentar sobre un set interno de tres superficies y el
  camino legacy `FB_IOC_*` sale de la ABI vigente.
- Los perfiles `run` y `smoke` de QEMU agregan `virtio-sound-pci` con un
  `audiodev` separado del camino de `pcspeaker`.
- `sleep_ms()` ahora corre sobre timers waitables del kernel en vez de un
  camino especial separado, y `fork()` preserva views anónimas compartidas o
  privadas segun el tipo de mapping.
- El menu Inicio deja de ofrecer `Exit Desktop`, y `shellapp` puede cerrarse
  con `exit` para volver al desktop y reabrirse despues desde `Menu -> Shell`.
- El compositor desktop reduce algo de trabajo redundante en el camino de
  presentacion y corrige mejor el enrutado/polling de input para clientes
  fullscreen.

## [0.1.4] - 2026-03-19

### Agregado

- Syscalls y wrappers POSIX nuevos para `fork`, `kill`, `raise`, `poll`,
  `select` y `fcntl(F_GETFL/F_SETFL)` con soporte de `O_NONBLOCK`.
- Runner automatizado `.\build.ps1 smoke`, que recompila, instala en
  `/disk/bin`, arranca QEMU headless y valida `fork`, señales basicas, polling
  y persistencia real sobre `SVFS2`.
- Userland multicall `busybox` para empezar el reemplazo de utilidades
  comodin, incluyendo `echo`, `cat`, `ls`, `mkdir`, `rm`, `mv`, `cp`, `true`,
  `false` y `sleep`.

### Cambiado

- El sistema pasa a reportarse como `v0.1.4` en kernel, shell, `uname`,
  `sysinfo` y componentes que consumen la version compartida.
- El timer base del sistema pasa a calibrarse con objetivo de `200 Hz` en vez
  de `100 Hz`, mejorando un poco la respuesta percibida del mouse y el
  redondeo practico de `sleep_ms()` para loops graficos e input.
- Los techos internos suben para procesos, descriptores, pipes, sockets, VFS
  y `SVFS2`, dejando mas margen para ports y userland real.
- `SVFS2` ya puede montar `/disk` en modo degradado de solo lectura si la
  recuperacion no deja al volumen seguro para `RW`, evitando que quede
  directamente offline frente a fallos recuperables.
- El build principal instala tambien los binarios internos en `/disk/bin`, de
  modo que la shell y la smoke automatizada ejercitan la misma copia
  persistente del userland.

## [0.1.3] - 2026-03-17

### Agregado

- Base compartida `virtio_pci` para drivers `virtio` modernos sobre PCI/MMIO,
  reutilizada por `virtio-input` y preparada para colas sincronas por polling.
- Driver nuevo `virtio-gpu` 2D para QEMU, con soporte MVP de
  `GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, `RESOURCE_ATTACH_BACKING`,
  `SET_SCANOUT`, `TRANSFER_TO_HOST_2D` y `RESOURCE_FLUSH`.
- Nodo nuevo `/dev/gpu0` con ABI publica `GPU_IOC_GET_INFO`,
  `GPU_IOC_ACQUIRE`, `GPU_IOC_RELEASE`, `GPU_IOC_PRESENT` y
  `GPU_IOC_PRESENT_REGION`.
- Utilidad nueva `gputest` para validar el camino directo de presentacion
  sobre `/dev/gpu0`.
- Calibracion del timer `local APIC/x2APIC` contra `RTC/CMOS` durante el boot
  para que `uptime_ms` y `sleep_ms` queden mejor alineados con tiempo real en
  QEMU.

### Cambiado

- `/dev/fb0` conserva compatibilidad con las apps fullscreen existentes, pero
  ahora puede presentar sobre `virtio-gpu` cuando el backend queda disponible.
- El perfil de QEMU en `build.ps1 run` ahora agrega `virtio-vga` con
  `xres=1280,yres=800`, y `limine.conf` pide `1280x800x32` para que el
  framebuffer de boot y el backend grafico queden alineados durante el
  handoff.
- La consola y la UI fullscreen pueden seguir visibles sobre el recurso
  primario de `virtio-gpu`, incluyendo el retorno desde sesiones graficas
  exclusivas y el redraw limpio de toda la shell sin dejar residuos en los
  margenes.
- `virtio-gpu` ahora intenta conservar el modo grande del framebuffer de boot
  antes de caer al scanout nativo reportado por el dispositivo, evitando que
  el sistema vuelva a `640x480` al finalizar el arranque cuando el modo mayor
  es aceptado.
- `virtio-input` paso a usar la geometria efectiva del framebuffer activo para
  normalizar el tablet absoluto, corrigiendo la desincronizacion del mouse con
  el host despues del cambio a `virtio-gpu`.
- El heap del kernel dejo de ser lineal-only y ahora recicla bloques
  liberados, hace `split/coalesce` y puede devolver arenas completas al
  allocador fisico cuando quedan vacias.
- El runtime POSIX de `subsystems/posix/sdk/v1` reemplazo su allocator tipo arena/bump por un
  heap fijo reciclable, de modo que `malloc`, `free`, `calloc` y `realloc`
  ya reutilizan memoria en apps externas.
- `sdk/doomgeneric` ya no corre acelerado por un reloj base incorrecto; el
  tiempo del juego vuelve a apoyarse sobre un backend de tiempo mas cercano al
  real, dejando el tuning de rendimiento restante del lado del port.

### Limites conocidos

- Este MVP de `virtio-gpu` mejora de forma visible la GUI fullscreen en QEMU,
  pero la subida de pixeles sigue siendo sincronica, con copia por CPU y sin
  `mmap`, page flipping ni doble buffer real.
- Portar apps a `/dev/gpu0` reduce capas y deja mejor encaminada la evolucion,
  pero la mejora grande en suavidad queda para una etapa posterior con buffers
  compartidos y presentacion menos bloqueante.
- El rendimiento final de ports externos grandes como `sdk/doomgeneric`
  todavia depende mucho del costo de escalado y del tamano del frame una vez
  que el sistema ya corre a resoluciones mas altas.

## [0.1.2] - 2026-03-14

### Agregado

- Soporte basico de mouse `PS/2` sobre el puerto auxiliar del controlador
  `i8042`, con IRQ12, paquetes estandar de 3 bytes y degradacion segura a
  teclado-only si el mouse no inicializa.
- Nodo nuevo `/dev/mouse0` con eventos dedicados de mouse para apps graficas,
  sin romper la semantica previa de `/dev/input0`.
- ABI compartida extendida con `struct savanxp_mouse_event`, flags publicos de
  botones y helpers nuevos `mouse_open` / `mouse_poll_event` en la libc/runtime.
- Nuevo shell grafico fullscreen `desktop`, inspirado en el lenguaje visual de
  Windows 2000, con taskbar, boton Inicio, reloj y cursor.
- Nueva utilidad `mousetest` para validar `/dev/mouse0`, movimiento relativo y
  botones desde userland.

### Cambiado

- La capa fullscreen del kernel ahora registra `/dev/mouse0` junto con
  `/dev/fb0` y `/dev/input0`, y limpia colas de teclado/mouse al adquirir o
  liberar la sesion grafica exclusiva.
- Bajo QEMU, el escritorio y `mousetest` ahora priorizan un backend absoluto
  `virtio-tablet-pci` cuando esta disponible, mientras `/dev/mouse0` conserva
  la ABI de deltas para no romper apps ya compiladas.
- El kernel ahora reserva una ventana MMIO propia para drivers PCI modernos y
  la usa para mapear BARs de memoria de forma segura durante el boot.
- En entornos QEMU con `virtio-tablet-pci`, el stack `PS/2` deja de inicializar
  el mouse auxiliar y queda teclado-only; el mouse `PS/2` se conserva como
  fallback cuando `virtio-input` no esta disponible.
- Se agrego lectura de `RTC/CMOS` en el kernel y un helper publico aditivo para
  consultar hora real desde userland; el reloj del escritorio ya no depende
  solo de `uptime`.
- La shell builtin ahora publica `desktop` y `mousetest` dentro del help
  interactivo.
- La documentacion principal y la referencia del SDK v1 reflejan el nuevo
  input de mouse, el escritorio inicial y el salto a `v0.1.2`.

### Limites conocidos

- `v0.1.2` expone solo movimiento relativo y botones basicos en la ABI
  publica; internamente puede usar puntero absoluto bajo QEMU, pero no hay
  rueda, ventanas reales, compositor ni input raw para juegos.
- `gfx_poll_event` sigue siendo teclado-only en esta etapa; el mouse entra por
  `/dev/mouse0`.

## [0.1.1] - 2026-03-10

### Agregado

- `SVFS2` como nueva version del filesystem persistente de `/disk`, con
  `superblock` primario/secundario, journal fijo de metadatos, bitmap de
  bloques, bitmap de inodos y tabla de inodos con extents.
- Syscall nueva `sync` y comando userland `sync` para forzar checkpoint
  explicito del estado persistente.
- Red minima sobre `rtl8139` + `QEMU user-net`, con `ARP`, `IPv4`, `ICMP`
  echo request/reply, sockets UDP IPv4 basicos y cliente TCP minimo.
- ABI publica extendida para dispositivos y `ioctl`, con nodos `/dev/fb0`,
  `/dev/input0`, `/dev/net0` y `/dev/pcspk`.
- GUI fullscreen inicial con primitivas `gfx_*`, demo interna `gfxdemo` y
  ejemplo externo `sdk/gfxhello`.
- Sonido minimo por `PC speaker` con comando `beep`.
- Primer port externo grande en `sdk/doomgeneric`, usado como hito practico
  de apps externas graficas sobre el ABI del sistema.
- Capa POSIX/libc inicial para SDK v1 con headers estandar publicos:
  `unistd.h`, `fcntl.h`, `stdio.h`, `stdlib.h`, `string.h`, `dirent.h`,
  `sys/stat.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`, `time.h`
  y asociados.
- Runtime nuevo `subsystems/posix/sdk/v1/runtime/posix.c` para apps externas, con `stdio`
  basico, `DIR*`, heap simple tipo arena, conversiones, helpers de string,
  tiempo y sockets cliente.
- Syscalls/base ABI nuevas para `getpid`, `stat`, `fstat`, `chdir`
  y `getcwd`.
- Smoke test externo `sdk/posixsmoke`, compilado solo contra headers
  estandar.
- Utilidad `keytest` para inspeccionar eventos de teclado sobre `/dev/input0`
  en fullscreen y validar `key down/up`, `keycode` y `ascii`.
- `FB_IOC_PRESENT_REGION` como extension de la ABI grafica para presentar solo
  una region del framebuffer desde userland.

### Cambiado

- La capa VFS ahora centraliza la normalizacion de paths y amplio la capacidad
  de rutas internas a `256` bytes, de modo que `process`, `cwd` y operaciones
  de filesystem comparten una sola resolucion canonica.
- El tooling host sobre `build/disk.img` (`build.ps1`, `tools/build-user.ps1`
  y `tools/UserAppCommon.ps1`) dejo de escribir `SVFS1` y paso a crear e
  instalar directamente sobre imagenes `SVFS2`.
- Las rutas persistentes de `/disk` dejaron de depender de nombres-path como
  identidad on-disk y pasaron a montarse desde inodos estables cacheados en
  memoria del kernel.
- El kernel ahora resuelve paths relativos contra un `cwd` por proceso, de
  modo que `open`, `exec`, `spawn` y operaciones de filesystem comparten
  directorio actual.
- El stack de teclado `PS/2` se reforzo con init del controlador mas robusta,
  decodificacion desacoplada de `TTY`/`UI`, mejor soporte de `AltGr`, locks y
  teclas especiales.
- `sdk/doomgeneric` dejo de depender de su set privado de headers estandar y
  paso a consumir la capa publica del SDK, reduciendo `savanxp_compat.c` a
  glue especifico del port.
- Las primitivas `gfx_*` del runtime compartido se optimizaron para trabajar
  con spans/rectangulos contiguos y reducir el costo de dibujo por frame.
- `gfxdemo`, `sdk/gfxhello` y `keytest` dejaron de refrescar la pantalla
  completa en cada iteracion y ahora usan regiones sucias o redraw bajo
  demanda para mejorar fluidez en fullscreen.
- El backend de `sdk/doomgeneric` reemplazo el escalado basado en divisiones
  por pixel por una expansion por filas cacheadas, bajando el costo de CPU por
  frame durante el render fullscreen.
- La documentacion principal y la referencia del SDK se actualizaron para
  reflejar la superficie POSIX/libc disponible y sus limites practicos.

### Limites conocidos

- La validacion reciente del salto a `SVFS2` cubre compilacion completa y
  verificacion host-side del flujo `build-user`, pero todavia no incluye
  smoke tests de reboot/replay dentro de QEMU.
- Si la recuperacion del journal o del metadata base falla al montar, el
  volumen queda offline; aun no existe un modo degradado de solo lectura.
- `free()` todavia no recicla memoria; el allocator userland sigue siendo de
  tipo arena/bump.
- `DIR->d_type` se completa por `stat()` best-effort en userland.
- `setsockopt`/`getsockopt` cubren solo flags y timeouts basicos del cliente.
- Segun el host y la captura de teclado de QEMU, `ImpPnt` puede no entrar al
  guest como tecla dedicada y requerir `Alt+ImpPnt` para pruebas manuales.
- La validacion reciente de `v0.1.1` es host-side; el smoke POSIX nuevo aun no
  fue corrido dentro de QEMU en esta tanda.

## [0.1.0] - 2026-03-08

Primera version publicada del experimento.

### Agregado

- Bootstrap del kernel sobre `x86_64 + UEFI + Limine`, con recepcion de
  `bootloader info`, `framebuffer`, `memory map`, `HHDM` e `initramfs`.
- Consola de texto sobre framebuffer con scroll, cursor y salida serie
  temprana por `COM1` / `debugcon`.
- GDT/IDT con segmentos de usuario, `TSS`, excepciones basicas y puerta de
  syscall por `int 0x80`.
- Allocador fisico temprano, heap del kernel y VMM minimo para espacios de
  usuario.
- Driver de teclado `PS/2`, `TTY` canonica y shell interactiva inicial.
- `VFS` minima montando `initramfs` `cpio newc`, con archivos dinamicos en
  memoria y volumen persistente `SVFS` montado en `/disk`.
- Loader `ELF64` estatico para procesos simples en `ring 3`.
- Scheduler round-robin preemptivo con bloqueo por `wait`, `read` y `sleep`.
- Shell con `pipes`, redireccion (`|`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`),
  parser de comillas simples/dobles y builtins `exec`, `which` y `mkdir`.
- Handles refcounted con `dup`, `dup2`, `waitpid(-1)` y procesos zombie/reap.
- Reclaim de paginas en `exit`/`exec`, destruccion de `VmSpace` y liberacion
  de stacks de kernel al reapear procesos.
- `SVFS` con subdirectorios persistentes simples bajo `/disk`, `mkdir`,
  `rmdir` de directorios vacios, `truncate` explicito y `rename`
  persistente.
- SDK v1 minima en `C`, con `crt0`, `libc`, linker script, headers
  `savanxp/*`, tooling host para instalar apps externas en `build/disk.img`
  y ejemplos base (`sdk/hello`, `sdk/errdemo`, `sdk/fsdemo`, `sdk/pathops`,
  `sdk/procpeek`, `sdk/spawnwait`, `sdk/statusdemo`, `sdk/multifile`,
  `sdk/template`).
- Userland inicial con `init`, `sh`, `echo`, `uname`, `ls`, `cat`, `sleep`,
  `ticker`, `demo`, `true`, `false`, `ps`, `fdtest`, `waittest`,
  `pipestress`, `spawnloop`, `badptr`, `rm`, `rmdir`, `truncate`,
  `seektest`, `truncatetest` y `errtest`.

### Cambiado

- Se consolidaron procesos, pipes y persistencia para que `/disk` quede
  operativo como flujo principal de trabajo entre reinicios.
- La superficie de syscalls y la `libc` minima se ampliaron con operaciones de
  filesystem y proceso necesarias para shell, pipes y apps externas.
- La documentacion del repo y del SDK v1 se congelo para dejar una base
  publica util a partir de la primera version numerada.
