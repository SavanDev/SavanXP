# SDK v1.2

`subsystems/posix/sdk/v1` es la superficie pública actual para compilar apps externas en `C`
contra SavanXP. El nivel de contrato vigente dentro de esta carpeta es `SDK
1.2`.

## Superficie pública

Incluye:

- `include/savanxp/libc.h`
- `include/savanxp/syscall.h`
- headers estandar en `include/` (`unistd.h`, `fcntl.h`, `poll.h`,
  `sys/select.h`, `signal.h`, `stdio.h`, `stdlib.h`, `string.h`, `dirent.h`,
  `sys/stat.h`, `sys/socket.h`, etc.)
- `runtime/crt0.S`
- `runtime/libc.c`
- `runtime/posix.c`
- `linker.ld`
- `REFERENCE.md`
- `include/savanxp/gfx2d.h`
- `runtime/gfx2d.c`

## ABI pública v1.2

Categorías soportadas:

- syscalls: `read`, `write`, `open`, `close`, `readdir`, `ioctl`, `socket`, `bind`, `sendto`, `recvfrom`, `connect`
- syscalls POSIX base: `getpid`, `stat`, `fstat`, `chdir`, `getcwd`
- introspeccion del sistema: `system_info`
- procesos: `spawn`, `spawn_fd`, `spawn_fds`, `exec`, `waitpid`, `fork`, `kill`
- descriptores: `pipe`, `dup`, `dup2`, `seek`, `fcntl`
- filesystem: `unlink`, `mkdir`, `rmdir`, `truncate`, `rename`
- utilidades: `yield`, `sleep_ms`, `uptime_ms`, `clear_screen`, `proc_info`, `poll`, `select`, `raise`
- graficos: `gfx_open`, `gfx_close`, `gfx_acquire`, `gfx_release`, `gfx_present`, `gfx_present_region`, `gfx_poll_event`
- toolkit 2D: `sx_bitmap`, `sx_painter`, `sx_rect_set`, `gfx_present_rects`
- mouse: `mouse_open`, `mouse_poll_event`
- primitivas software: `gfx_rgb`, `gfx_stride_pixels`, `gfx_buffer_pixels`, `gfx_buffer_bytes`, `gfx_clear`, `gfx_pixel`, `gfx_hline`, `gfx_vline`, `gfx_rect`, `gfx_frame`, `gfx_text_width`, `gfx_text_height`, `gfx_blit_text`

Nodos de dispositivo expuestos actualmente:

- `/dev/gpu0`
- `/dev/input0`
- `/dev/mouse0`
- `/dev/net0`
- `/dev/pcspk`
- `/dev/audio0`

Notas graficas:

- `gfx_*` es el camino grafico soportado para apps normales.
- `gfx_open` espera la superficie cliente entregada por `desktop`; si la app
  no fue lanzada como cliente del compositor, falla.
- `/dev/gpu0` queda como interfaz de bajo nivel para compositor, tooling y
  diagnostico directo.

Ioctl groups visibles:

- `NET_IOC_*`
- `PCSPK_IOC_*`
- `GPU_IOC_*`
- `AUDIO_IOC_*`

Errores visibles:

- `SAVANXP_EIO`
- `SAVANXP_EAGAIN`
- `SAVANXP_EINVAL`
- `SAVANXP_EBADF`
- `SAVANXP_ENOENT`
- `SAVANXP_ENOMEM`
- `SAVANXP_EBUSY`
- `SAVANXP_EEXIST`
- `SAVANXP_ENODEV`
- `SAVANXP_ENOTDIR`
- `SAVANXP_EISDIR`
- `SAVANXP_ENOSPC`
- `SAVANXP_EPIPE`
- `SAVANXP_ENOSYS`
- `SAVANXP_ENOTEMPTY`
- `SAVANXP_ECHILD`
- `SAVANXP_ETIMEDOUT`

Reglas del ABI:

- El ABI sigue siendo `int 0x80`.
- Los programas son `ELF64` estáticos para `x86_64`.
- No hay dynamic linker ni shared libraries.
- Las syscalls devuelven `>= 0` en éxito y `< 0` en error.
- `main(int argc, char** argv)` está soportado.
- `waitpid(SAVANXP_WAIT_ANY, &status)` está soportado.
- `fork()` clona el espacio de direcciones y descriptores del proceso actual.
- las señales disponibles en `SDK 1.2` son de accion por defecto (`SIGINT`,
  `SIGTERM`, `SIGKILL`, `SIGPIPE`, `SIGCHLD`); no hay handlers POSIX
  completos todavia.

Límites congelados para v1:

- `SAVANXP_MAX_ARGC = 15`
- `SAVANXP_MAX_ARG_LENGTH = 63`
- `SAVANXP_PROC_NAME_CAPACITY = 32`
- `SAVANXP_STDIN_FILENO`, `SAVANXP_STDOUT_FILENO`, `SAVANXP_STDERR_FILENO`

Helpers públicos del runtime:

- errores: `result_is_error`, `result_error_code`, `result_error_string`
- logging: `puts_err`, `printf_fd`, `eprintf`
- introspección: `process_state_string`
- rendering: `struct savanxp_gfx_context`

Capas públicas:

- `savanxp/*` sigue expuesto como capa baja del ABI y wrappers crudos.
- los headers estándar en `subsystems/posix/sdk/v1/include` montan una capa POSIX/libc arriba sin romper apps viejas.
- la libc pública incluye allocator sobre heap fijo del runtime, con `malloc`,
  `free`, `calloc` y `realloc` reciclables dentro de ese espacio.

Tipos compartidos para dispositivos:

- `struct savanxp_fb_info`
- `struct savanxp_input_event`
- `struct savanxp_mouse_event`
- `struct savanxp_net_info`
- `struct savanxp_system_info`
- `enum savanxp_net_status`
- `enum savanxp_timer_backend`
- `struct savanxp_net_ping_request`
- `struct savanxp_net_ping_result`
- `struct savanxp_pcspk_beep`
- `struct savanxp_audio_info`
- `struct savanxp_sockaddr_in`

Sockets v1:

- `socket(SAVANXP_AF_INET, SAVANXP_SOCK_DGRAM, SAVANXP_IPPROTO_UDP)`
- `bind(fd, &addr)`
- `sendto(fd, buffer, len, &addr)`
- `recvfrom(fd, buffer, len, &addr, timeout_ms)`
- `socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP)`
- `connect(fd, &addr, timeout_ms)`
- sockets TCP conectados usan `write(fd, ...)` y `read(fd, ...)`
- `poll`/`select` cubren sockets, pipes, TTY/input y archivos regulares como
  `ready` inmediato
- `fcntl(fd, F_SETFL, O_NONBLOCK)` expone modo no bloqueante para pipes,
  sockets y entradas compatibles
- servidor TCP todavia no existe en v1; el alcance actual es cliente minimo

Diagnostico de red v1:

- `savanxp_net_info.last_status` expone la ultima etapa observada por el driver
- `savanxp_net_info` incluye contadores basicos de TX/RX, ARP y ping timeout

## Flujo recomendado

Compilar e instalar una app externa:

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\sdk\hello -Name hello
```

Compilar solo el ELF:

```powershell
.\tools\build-user.ps1 -Source .\sdk\multifile -Name multifile -NoInstall
```

Compilar e instalar desde un directorio con varias fuentes:

```powershell
.\tools\build-user.ps1 -Source .\sdk\multifile -Name multifile
```

Crear una app nueva desde el template público:

```powershell
.\tools\new-user-app.ps1 -Name miapp
.\tools\build-user.ps1 -Source .\sdk\miapp -Name miapp
```

El tooling:

- acepta un archivo `.c` individual o un directorio
- compila todas las fuentes `.c` y `.S` del directorio en forma recursiva
- agrega automáticamente `-I <source>` y `-I <source>/include` si existe
- usa `subsystems/posix/sdk/v1` como runtime público en vez del árbol interno de `subsystems/posix/userland`

Smoke tests útiles en el estado actual:

- `netinfo`
- `ping 10.0.2.2`
- `udptest`
- `tcpget 104.18.26.120 80 example.com /`
- `beep 440 200`
- `audiotest`
- `gfxdemo`
- `forktest`
- `polltest`
- `sigtest`
- `busybox ls /disk/bin`
- `smoke` via `.\build.ps1 smoke`

Ejemplo externo recomendado para GUI:

```powershell
.\tools\build-user.ps1 -Source .\sdk\gfxhello -Name gfxhello
.\tools\build-user.ps1 -Source .\sdk\udptest -Name udptest
.\tools\build-user.ps1 -Source .\sdk\tcpget -Name tcpget
```

Nota de red:

- con `QEMU user-net` la prueba confiable del stack ICMP v1 es `ping 10.0.2.2`
- un `ping` directo a Internet puede fallar aunque el driver RTL8139 esté bien
- la IP usada por `tcpget` puede cambiar; el 9 de marzo de 2026 el valor
  validado para `example.com:80` fue `104.18.26.120`

## Estructura mínima recomendada

Las apps nuevas deberían incluir:

```c
#include "savanxp/libc.h"
```

La estructura mínima más cómoda es:

```text
miapp/
  main.c
  include/
    miapp.h
  helper.c
```

El ejemplo `sdk/template` sirve como punto de partida y `sdk/multifile`
demuestra compilación con varias unidades de traducción.

Para el detalle de límites y contratos estables, ver `subsystems/posix/sdk/v1/REFERENCE.md`.
