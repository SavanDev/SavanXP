# SavanXP

Version actual del proyecto: `v0.2.1`

Historial de versiones: `CHANGELOG.md`

Proyecto de sistema operativo en `C++` sobre `x86_64 + UEFI`, con `Limine`
como bootloader y flujo de trabajo pensado para Windows nativo.

SavanXP ya no se plantea como un experimento aislado. El proyecto analiza
hasta donde se puede diseñar y construir un sistema operativo real con ayuda
de la IA, usando a Codex como colaborador principal dentro del proceso de
desarrollo.

## Estado actual

El kernel ya bootea a una sesion grafica funcional con `desktop` como
compositor/sesion principal:

- Integracion con Limine para recibir `bootloader info`, `firmware type`,
  `framebuffer`, `memory map`, `HHDM` y un modulo `initramfs`.
- Consola de texto sobre framebuffer con scroll, cursor y salida serie
  temprana por `COM1` / `debugcon`.
- GDT/IDT con segmentos de usuario, `TSS`, excepciones basicas y puerta de
  syscall por `int 0x80`.
- Allocador fisico temprano, heap del kernel y VMM minimo para espacios de
  usuario.
- Stack `PS/2` inicial con teclado por `IRQ1`, mouse auxiliar por `IRQ12`,
  decodificacion desacoplada, soporte expandido de teclas especiales y
  degradacion segura a teclado-only si el mouse no inicializa.
- Enumeracion PCI minima por config-space en `q35` y capa de dispositivos
  expuesta como nodos bajo `/dev`.
- Dispositivos iniciales en `/dev`: `gpu0`, `input0`, `mouse0`, `net0`,
  `pcspk` y `audio0`.
- `ioctl` como syscall publica para dispositivos y ABI compartida extendida
  para framebuffer, input, red y PC speaker.
- `VFS` minima montando un `initramfs` `cpio newc`, con archivos dinamicos en
  memoria para redireccion simple y un volumen persistente `SVFS` montado en
  `/disk`.
- Loader `ELF64` estatico para procesos simples en `ring 3`.
- Userland inicial con `init`, `sh`, `busybox`, `uname`, `df`, `ticker`,
  `demo`, `ps`, `fdtest`, `waittest`, `pipestress`, `spawnloop`, `badptr`,
  `rmdir`, `truncate`, `seektest`, `truncatetest`, `errtest`, `sysinfo`,
  `keytest`, `mousetest`, `desktop`, `forktest`, `polltest`, `sigtest`
  y `smoke`; los comandos comodin `echo`, `cat`, `ls`, `mkdir`, `rm`, `mv`,
  `cp`, `true`, `false` y `sleep` ya salen del multicall `busybox`.
- Timer `local APIC/x2APIC` activo y calibrado contra `RTC/CMOS` para dar un
  tiempo base mas estable en QEMU y mantener `uptime_ms` / `sleep_ms`
  alineados con tiempo real.
- Red minima sobre `rtl8139` + `QEMU user-net`, con `ARP`, `IPv4`, `ICMP`
  echo request/reply, sockets UDP IPv4 basicos y cliente TCP minimo, mas
  contadores y estado diagnostico expuestos por `NET_IOC_GET_INFO`.
- Sesion grafica sobre compositor propio `desktop`, taskbar/start menu
  persistentes, apps cliente renderizando sobre un work area estable,
  `shellapp`, primitivas 2D reutilizables en `gfx_*`,
  ejemplo externo `sdk/gfxhello`, demo `gfxdemo` y testers graficos
  migrados al camino cliente del compositor.
- Pipeline grafico del compositor ya optimizado para mayor fluidez, con
  primitivas `gfx_*` mas baratas, present parcial por regiones sucias en
  `gpu0`, apps limitadas al area util y demos/UI que evitan redibujar o
  copiar toda la pantalla cuando no hubo cambios.
- `virtio-gpu` 2D ya expone estadisticas ampliadas, enumeracion de scanouts,
  refresh basico por hotplug, bottom-half interno apoyado por IRQ cuando la
  linea PCI esta disponible y cursor plane opcional con fallback transparente
  al cursor software del desktop.
- Base `virtio_pci` reutilizable para drivers `virtio` modernos y backend
  grafico nuevo `virtio-gpu` 2D para QEMU.
- Nodo `/dev/gpu0` como interfaz grafica de bajo nivel (`GPU_IOC_*`) para
  compositor, mode-setting, import de sections, present por superficie y
  diagnostico directo.
- En QEMU, la salida grafica puede pasar a `virtio-vga` manteniendo una
  resolucion de trabajo consistente durante el handoff desde el framebuffer de
  boot al backend del driver, sin volver a encogerse a `640x480` al terminar
  el arranque.
- La consola y el desktop ya quedan limpios sobre `virtio-gpu`: el redraw de
  `shellapp` y de las apps cliente queda acotado al work area del desktop,
  mientras la taskbar permanece visible y sin residuos visuales.
- `virtio-input` queda alineado con el framebuffer activo, de modo que el
  mouse bajo QEMU vuelve a seguir correctamente al puntero del host incluso
  despues del cambio al backend `virtio-gpu`.
- Primer juego externo porteado: `sdk/doomgeneric`, mantenido como port
  externo de referencia para apps graficas, assets persistentes en `/disk` y
  compatibilidad de input.
- Backend de `sdk/doomgeneric` afinado para integrarse al compositor, con
  escalado por filas cacheadas en lugar de divisiones por pixel en cada frame,
  reduciendo costo de CPU y mejorando la sensacion visual durante el juego.
- Validacion del port de `doomgeneric` ya cerrada para arranque,
  jugabilidad basica y `save/load`; los ajustes quedaron repartidos entre
  `SVFS2` en kernel, la capa `POSIX`/`stdio` de la SDK y el backend propio del
  port, sin mover semantica de libc al kernel. Dentro del flujo desktop-first
  actual, `doomgeneric` queda como regresion de persistencia y smoke visual
  opcional, no como prueba principal del stack grafico.
- Sonido minimo por `PC speaker` con comando `beep`.
- Playback PCM minimo sobre `virtio-sound`, expuesto como `/dev/audio0` con
  `AUDIO_IOC_GET_INFO` y la utilidad `audiotest`.
- Scheduler round-robin preemptivo con bloqueo por `wait`, `read` y `sleep`.
- Shell con `pipes`, redireccion (`|`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`)
  y resolucion de comandos en `/disk/bin` con fallback a `/bin`.
- Shell con parser mejorado para comillas simples/dobles y builtins `exec`,
  `which`, `mkdir`, `cd` y `pwd`.
- Handles refcounted con `dup`, `dup2`, `waitpid(-1)` y procesos zombie/reap.
- Syscalls y capa POSIX ampliadas con `fork`, `kill`, `raise`,
  `fcntl(F_GETFL/F_SETFL)`, `O_NONBLOCK`, `poll` y `select`, dejando una base
  mucho mas realista para ports y utilidades estilo BusyBox.
- Reclaim real de paginas para `exit`/`exec`, destruccion de `VmSpace` y
  liberacion de stacks de kernel al reapear procesos.
- `SVFS2` como filesystem persistente de `/disk`, con `superblock`
  primario/secundario, journal fijo de metadatos, bitmap de bloques, bitmap
  de inodos y tabla de inodos con extents para archivos/directorios.
- SDK v1 minima en `C` (`crt0.S`, `libc.[ch]`, `userland/linker.ld`,
  `include/shared/syscall.h`) y tooling host para instalar apps externas
  directo en `build/disk.img`.
- Heap del kernel y runtime POSIX del SDK ahora reciclan bloques liberados con
  `free()`/`realloc()`, dejando una base comun mejor encaminada para futuras
  extensiones como `mmap` o heaps mas sofisticados.
- `SVFS2` ya puede montar `/disk` en modo degradado de solo lectura si el
  recovery no deja el volumen en condiciones seguras para `RW`, evitando
  dejarlo offline frente a fallos recuperables.
- Validacion basica de punteros de userland en syscalls principales, mas
  syscalls `seek`, `unlink`, `exec`, `mkdir`, `rmdir`, `truncate`, `rename`,
  `sync` e `ioctl`.
- `build.ps1` con `build`, `run`, `debug`, `smoke` y `clean`; la variante
  `smoke` recompila, prepara `/disk/bin`, arranca QEMU headless y espera un
  `SMOKE PASS/FAIL` automatizado desde userland.

## Siguiente orden recomendado

Orden sugerido para las cuatro lineas de trabajo siguientes:

1. Completar el subset real de BusyBox sobre la base nueva y ampliar tests de compatibilidad.
2. Evolucionar el desktop shell y la libreria GUI sobre la base actual de work area + compositor.
3. Ampliar la API de sockets desde el cliente minimo actual hacia servidor TCP y mejor observabilidad.
4. Profundizar señales, TTY y compatibilidad POSIX alrededor de ports mas ambiciosos.

Razon del orden:

- Red ya tiene una base usable, pero todavia necesita mejor observabilidad para
  diagnosticar timeouts, estados intermedios y fallos reales sin depender tanto
  del host.
- La GUI sobre compositor y work area ya probo el camino tecnico; el
  siguiente paso natural es consolidar el desktop shell inicial y convertir
  esas primitivas en una libreria reutilizable y menos demo-driven.
- La capa POSIX/libc ya cubre `fork`, `poll/select`, `kill` y `O_NONBLOCK`,
  asi que el siguiente salto natural es endurecer compatibilidad real con
  utilidades y ports mas ambiciosos.
- El stack actual ya supero `ping` y tiene UDP + cliente TCP minimo; el salto
  pendiente ahora es completar mejor la API de sockets y el soporte de TCP.

## Prerrequisitos

- `git`
- `clang++`
- `ld.lld` o `clang++` con `-fuse-ld=lld`
- `qemu-system-x86_64`
- Firmware OVMF accesible por una de estas vias:
  - variables de entorno `OVMF_CODE` y `OVMF_VARS`
  - o una instalacion de QEMU/MSYS2 en una ruta comun

`build.ps1` descarga automaticamente la rama binaria `v10.x-binary` de Limine
si no existe en `tools/limine`.

## Uso

```powershell
.\build.ps1 build
.\build.ps1 run
.\build.ps1 debug
.\build.ps1 smoke
.\build.ps1 clean
```

Durante `run` y `debug`, QEMU expone la salida serie en la terminal. El kernel
entra a una shell inicial de userland con prompt contextual y el `debugcon`
queda guardado en `build/debugcon.log`. `smoke` usa una sesion headless aparte,
corre el runner `/disk/bin/smoke` y deja logs en `build/smoke-*.log`. Para ver
el snapshot de arranque y
estado base del sistema desde la shell, usa `sysinfo`.

## Apps externas

El flujo principal para probar programas propios ya no requiere reconstruir el
`initramfs`. Compilas desde Windows contra la SDK v1 e instalas el ELF directo
en `build/disk.img`; el build principal tambien replica los binarios internos
a `/disk/bin` para que la shell resuelva primero el userland persistente:

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\sdk\hello\main.c -Name hello
.\build.ps1 run
```

Dentro de SavanXP:

```text
which hello
hello
hello > /disk/out.txt
```

Tambien hay un wrapper para compilar, instalar y arrancar QEMU en un paso:

```powershell
.\tools\run-user.ps1 -Source .\sdk\errdemo\main.c -Name errdemo
```

Los ejemplos base estan en `sdk/hello`, `sdk/errdemo` y `sdk/fsdemo`. Tambien
hay ejemplos/aplicaciones graficas externas en `sdk/gfxhello` y
`sdk/doomgeneric`, mientras `gputest` queda como utilidad principal para
validar el camino directo sobre `/dev/gpu0`.

La smoke automatizada actual valida `forktest`, `polltest`, `sigtest`,
`busybox` y escritura/lectura real sobre `/disk`, de modo que los cambios de
ABI base ya no dependen solo de verificacion host-side.

`tools/doom-smoke.ps1` sigue disponible como smoke visual/manual del port de
`doomgeneric`, pero ya no se considera la prueba principal del paradigma
desktop-first ni del stack GPU del SO.

Dentro de la `sdk/v1`, el runtime POSIX ya no usa un allocator solo tipo
arena/bump: `malloc`, `free`, `calloc` y `realloc` reciclan memoria dentro del
heap fijo actual, lo que vuelve mucho mas utiles ports y apps externas sin
depender todavia de `sbrk`/`mmap`.

Para diagnosticar input grafico dentro del work area existen `keytest` sobre `/dev/input0` y
`mousetest` sobre `/dev/mouse0`. `keytest` muestra `key down/up`, `keycode` y
`ascii` en tiempo real. `mousetest` valida cursor, deltas y botones. Nota
practica: segun el host y la captura de teclado de QEMU, `ImpPnt` puede llegar
al guest solo como `Alt+ImpPnt`.

En QEMU, `build.ps1 run` agrega `virtio-tablet-pci` y el kernel lo prioriza
para el escritorio cuando esta disponible. La ABI publica no cambia: las apps
siguen leyendo `/dev/mouse0` con deltas para mantener compatibilidad.
Debajo de eso, el kernel ahora cuenta con una base MMIO PCI reutilizable para
drivers `virtio` modernos.
El perfil actual tambien agrega `virtio-vga`; cuando `virtio-gpu` queda
operativo, `desktop` mantiene el scanout como duenio estable y `gpu0` queda
disponible para compositor y pruebas directas.
El arranque pide `1280x800x32` al framebuffer de boot y `build.ps1 run`
inicializa `virtio-vga` con `xres=1280,yres=800`, de modo que el handoff
normal ya queda en esa misma geometria cuando el driver grafico toma control.
En ese camino, redimensionar la ventana de QEMU mantiene la relacion de
aspecto del contenido del desktop y de las apps cliente en lugar de
deformarlo.
Cuando `virtio-input` queda activo, el stack `PS/2` conserva el teclado y deja
el mouse auxiliar solo como fallback para entornos sin `virtio-tablet`.
El escritorio tambien puede leer hora real desde `RTC/CMOS`; en QEMU el launcher
usa `-rtc base=localtime` para que el reloj siga la hora local del host.

El stack actual usa `gfx_*` como camino normal para apps graficas: el runtime
ya espera una superficie cliente del compositor y deja `/dev/gpu0` como ABI
de bajo nivel para `desktop` y herramientas diagnosticas como `gputest`.
Debajo de eso, `virtio-gpu` sigue presentando con copia por CPU antes del
`TRANSFER_TO_HOST_2D` / `RESOURCE_FLUSH`, asi que la fluidez final todavia
depende bastante del tamano del frame y de cada port concreto.

Para probar el escritorio inicial:

```text
desktop
```

## Extension de VS Code

El repo incluye una extension experimental en `tools/vscode-extension` para
trabajar con apps externas del SDK desde VS Code. La extension agrega:

- comandos para crear, compilar, instalar y ejecutar apps del SDK
- un panel lateral `SavanXP` con ejemplos bajo `sdk/` y snippets curados
- acceso rapido a `sdk/v1/REFERENCE.md`

Para cargarla en modo desarrollo:

```powershell
code --extensionDevelopmentPath .\tools\vscode-extension
```

## Persistencia experimental

El build genera una imagen `build/disk.img` la primera vez y la conecta como un
disco IDE legacy adicional en QEMU. Esa imagen se monta en `/disk` mediante un
filesystem experimental propio (`SVFS2`), de modo que:

```text
ls /disk
ls /disk/bin
cat /disk/README
echo hola > /disk/notes.txt
mv /disk/notes.txt /disk/tmp/notes.txt
sync
rm /disk/tmp/notes.txt
seektest
renametest
truncatetest
```

deberian sobrevivir a reinicios posteriores mientras no ejecutes `clean`.

Notas practicas del estado actual:

- `build.ps1 build` ya crea imagenes nuevas directamente en formato `SVFS2`.
- `tools/build-user.ps1` instala ELF externos sobre `SVFS2` sin reconstruir
  `initramfs`.
- Si existe una imagen vieja de `SVFS1`, el flujo actual la recrea; no hay
  migracion in-place en esta etapa.
