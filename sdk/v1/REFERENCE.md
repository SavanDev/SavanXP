# SDK v1 Reference

## Estabilidad

La carpeta `sdk/v1` es la superficie pública congelada para la primera versión
del SDK externo de SavanXP.

Se consideran parte del contrato:

- `include/savanxp/libc.h`
- `include/savanxp/syscall.h`
- `runtime/crt0.S`
- `runtime/libc.c`
- `linker.ld`
- `tools/build-user.ps1`
- `tools/new-user-app.ps1`

## Límites visibles

- `SAVANXP_MAX_ARGC = 15`
- `SAVANXP_MAX_ARG_LENGTH = 63`
- `SAVANXP_PROC_NAME_CAPACITY = 32`
- descriptores estándar: `SAVANXP_STDIN_FILENO`, `SAVANXP_STDOUT_FILENO`, `SAVANXP_STDERR_FILENO`
- `waitpid(SAVANXP_WAIT_ANY, &status)` está soportado

## Convenciones

- éxito: retorno `>= 0`
- error: retorno `< 0`
- para decodificar errores usar: `result_is_error`, `result_error_code`, `result_error_string`
- para estados de proceso usar: `process_state_string`
- las syscalls siguen usando `int 0x80`
- los binarios externos son `ELF64` estáticos para `x86_64`

## Syscalls visibles

- I/O: `read`, `write`, `open`, `close`, `readdir`, `ioctl`
- sockets: `socket`, `bind`, `sendto`, `recvfrom`, `connect`
- procesos: `spawn`, `spawn_fd`, `spawn_fds`, `exec`, `waitpid`, `exit`
- descriptores: `pipe`, `dup`, `dup2`, `seek`
- filesystem: `unlink`, `mkdir`, `rmdir`, `truncate`, `rename`
- utilidades: `yield`, `sleep_ms`, `uptime_ms`, `clear_screen`, `proc_info`

## Errores visibles

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

## Dispositivos v1

Nodos reservados actualmente:

- `/dev/fb0`
- `/dev/input0`
- `/dev/net0`
- `/dev/pcspk`

Ioctl groups:

- framebuffer: `FB_IOC_GET_INFO`, `FB_IOC_ACQUIRE`, `FB_IOC_RELEASE`, `FB_IOC_PRESENT`
- network: `NET_IOC_GET_INFO`, `NET_IOC_UP`, `NET_IOC_PING`
- pc speaker: `PCSPK_IOC_BEEP`, `PCSPK_IOC_STOP`

Tipos compartidos:

- `struct savanxp_fb_info`
- `struct savanxp_input_event`
- `struct savanxp_net_info`
- `enum savanxp_net_status`
- `struct savanxp_net_ping_request`
- `struct savanxp_net_ping_result`
- `struct savanxp_pcspk_beep`
- `struct savanxp_sockaddr_in`

Sockets v1:

- dominio soportado: `SAVANXP_AF_INET`
- tipo soportado: `SAVANXP_SOCK_DGRAM`
- protocolo soportado: `SAVANXP_IPPROTO_UDP`
- `recvfrom(..., timeout_ms)` usa milisegundos; `0` espera indefinidamente
- `savanxp_sockaddr_in.ipv4` y `.port` usan orden host
- cliente TCP minimo soportado:
- `socket(SAVANXP_AF_INET, SAVANXP_SOCK_STREAM, SAVANXP_IPPROTO_TCP)`
- `connect(fd, &addr, timeout_ms)`
- `write(fd, ...)` para enviar
- `read(fd, ...)` para recibir hasta EOF o timeout
- servidor TCP, `listen` y `accept` quedan fuera de v1

Diagnostico de red:

- `savanxp_net_info.last_status` usa `enum savanxp_net_status`
- `savanxp_net_info` expone `tx_frames`, `rx_frames`, `tx_errors`, `rx_errors`
- `savanxp_net_info` expone `arp_requests`, `arp_timeouts`, `ping_requests`, `ping_timeouts`

## GFX runtime v1

Helpers públicos expuestos por el runtime:

- `gfx_open`
- `gfx_close`
- `gfx_acquire`
- `gfx_release`
- `gfx_present`
- `gfx_poll_event`
- `gfx_rgb`
- `gfx_stride_pixels`
- `gfx_buffer_pixels`
- `gfx_buffer_bytes`
- `gfx_clear`
- `gfx_pixel`
- `gfx_hline`
- `gfx_vline`
- `gfx_rect`
- `gfx_frame`
- `gfx_text_width`
- `gfx_text_height`
- `gfx_blit_text`

Contrato actual:

- una sola app puede adquirir el framebuffer a la vez
- el modo gráfico es fullscreen exclusivo
- `gfx_present` copia un frame completo de 32-bpp desde userland
- los eventos vienen desde `/dev/input0`
- no hay `mmap`, ventanas, mouse ni compositor en v1
- el backbuffer sigue siendo propiedad de la app; `gfx_buffer_pixels` y `gfx_buffer_bytes` ayudan a validarlo

## Flujo recomendado

1. Crear una app desde `sdk/template` o con `tools/new-user-app.ps1`
2. Compilar con `tools/build-user.ps1`
3. Instalar en `/disk/bin`
4. Ejecutar desde la shell por nombre o ruta

## No incluido en v1

- `fork`
- señales
- librerías compartidas
- loader dinámico
- `errno` global
- compatibilidad POSIX completa
- sockets
- TCP/UDP
- DNS
- DHCP
- audio PCM
