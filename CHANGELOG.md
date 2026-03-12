# Changelog

Registro de cambios visibles por version para SavanXP.

Notas de corte:
- `v0.1.0` se reconstruyo de forma retroactiva desde el historial hasta `d822857`.
- `v0.1.1` cubre los cambios posteriores a `v0.1.0`, incluyendo el trabajo actual
  ya integrado en el arbol pero todavia no etiquetado en git.

## [0.1.1] - 2026-03-10

### Agregado

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

### Cambiado

- El kernel ahora resuelve paths relativos contra un `cwd` por proceso, de
  modo que `open`, `exec`, `spawn` y operaciones de filesystem comparten
  directorio actual.
- El stack de teclado `PS/2` se reforzo con init del controlador mas robusta,
  decodificacion desacoplada de `TTY`/`UI`, mejor soporte de `AltGr`, locks y
  teclas especiales.
- `sdk/doomgeneric` dejo de depender de su set privado de headers estandar y
  paso a consumir la capa publica del SDK, reduciendo `savanxp_compat.c` a
  glue especifico del port.
- La documentacion principal y la referencia del SDK se actualizaron para
  reflejar la superficie POSIX/libc disponible y sus limites practicos.

### Limites conocidos

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
