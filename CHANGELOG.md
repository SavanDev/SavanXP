# Changelog

Registro de cambios visibles por version para SavanXP.

Notas de corte:
- `v0.1.0` se reconstruyo de forma retroactiva desde el historial hasta `d822857`.
- `v0.1.1` cubre los cambios posteriores a `v0.1.0`, incluyendo el trabajo actual
  ya integrado en el arbol pero todavia no etiquetado en git.

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
- Runtime nuevo `sdk/v1/runtime/posix.c` para apps externas, con `stdio`
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
