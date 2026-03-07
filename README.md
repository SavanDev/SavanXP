# SavanXP

Experimento de hobby OS en `C++` sobre `x86_64 + UEFI`, con `Limine` como
bootloader y flujo de trabajo pensado para Windows nativo.

## Estado actual

El kernel ya bootea a una terminal funcional inicial:

- Integracion con Limine para recibir `bootloader info`, `firmware type`,
  `framebuffer`, `memory map`, `HHDM` y un modulo `initramfs`.
- Consola de texto sobre framebuffer con scroll, cursor y salida serie
  temprana por `COM1` / `debugcon`.
- GDT/IDT con segmentos de usuario, `TSS`, excepciones basicas y puerta de
  syscall por `int 0x80`.
- Allocador fisico temprano, heap del kernel y VMM minimo para espacios de
  usuario.
- Driver de teclado `PS/2` por `IRQ1` y `TTY` canonica para la consola
  foreground.
- `VFS` minima montando un `initramfs` `cpio newc`, con archivos dinamicos en
  memoria para redireccion simple.
- Loader `ELF64` estatico para procesos simples en `ring 3`.
- Userland inicial con `init`, `sh`, `echo`, `uname`, `ls`, `cat`, `sleep`,
  `ticker` y `demo`.
- Timer `local APIC/x2APIC` activo para tiempo base del sistema.
- Scheduler round-robin preemptivo con bloqueo por `wait`, `read` y `sleep`.
- Shell con `pipes` y redireccion basica (`|`, `<`, `>`).
- Script `build.ps1` con `build`, `run`, `debug` y `clean`.

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
.\build.ps1 clean
```

Durante `run` y `debug`, QEMU expone la salida serie en la terminal. El kernel
entra a una shell inicial de userland y el `debugcon` queda guardado en
`build/debugcon.log`.
