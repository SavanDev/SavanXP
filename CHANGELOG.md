# Changelog

Registro de cambios visibles por version para SavanXP.

Notas de corte:

- `v0.1.0` se reconstruyo de forma retroactiva desde el historial hasta `d822857`.
- `v0.1.1` cubre los cambios posteriores a `v0.1.0`, incluyendo el trabajo actual
  ya integrado en el arbol pero todavia no etiquetado en git.

## [Unreleased]

### Agregado

- ABI publica nueva en `/dev/gpu0` para seguimiento explicito de presents con
  `GPU_IOC_GET_PRESENT_TIMELINE` y `GPU_IOC_WAIT_PRESENT`.
- Estadisticas ampliadas de `virtio-gpu` con latencia end-to-end por frame,
  incluyendo muestras acumuladas y peor caso observado.

### Cambiado

- El progreso en background de `virtio-gpu` deja de depender del subsistema de
  input y pasa a bombearse desde un servicio de dispositivos del kernel
  invocado en timer y waits bloqueantes.
- El `desktop` pasa a trabajar con presupuesto de un frame visible: si ya hay
  un present en vuelo, coalescea dirty rects y espera el retiro explicito del
  frame anterior antes de reutilizar su surface importada.
- Primer corte interno del `desktop` para desacoplar menu/layout/render del
  loop principal, manteniendo el binario `desktop` unico y el contrato
  `desktop-first` sin cambios visibles.
- `gputest --smoke` ahora valida tambien la timeline de presents y el pacing
  explicito del driver, no solo stats/stages internos.
- El banner lateral del menu Inicio cambia a texto vertical para aprovechar
  mejor la franja del costado y encajar mas limpio en el layout actual.

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
  `.\sdk\doomgeneric\build.ps1`, `.\build.ps1 build` y `.\build.ps1 smoke`
  siguen conservando `doomgeneric` y `doom1.wad` en `build/disk.img` sin
  recreacion normal de la imagen.

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
- `tools/doom-smoke.ps1` sigue disponible como smoke visual del port de
  `doomgeneric`, pero deja de figurar como prueba principal del stack GPU
  ahora que el paradigma normal del sistema es desktop-first.

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
- Primer port externo grande en `sdk/doomgeneric`, con smoke test host-side
  en `tools/doom-smoke.ps1`.
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
