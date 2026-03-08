# SavanXP

Version actual del experimento: `v0.1.0`

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
  memoria para redireccion simple y un volumen persistente `SVFS` montado en
  `/disk`.
- Loader `ELF64` estatico para procesos simples en `ring 3`.
- Userland inicial con `init`, `sh`, `echo`, `uname`, `ls`, `cat`, `sleep`,
  `ticker`, `demo`, `true`, `false`, `ps`, `fdtest`, `waittest`,
  `pipestress`, `spawnloop`, `badptr`, `rm`, `rmdir`, `truncate`,
  `seektest`, `truncatetest` y `errtest`.
- Timer `local APIC/x2APIC` activo para tiempo base del sistema.
- Scheduler round-robin preemptivo con bloqueo por `wait`, `read` y `sleep`.
- Shell con `pipes`, redireccion (`|`, `<`, `>`, `>>`, `2>`, `2>>`, `2>&1`)
  y resolucion de comandos en `/disk/bin` con fallback a `/bin`.
- Shell con parser mejorado para comillas simples/dobles y builtins `exec`,
  `which` y `mkdir`.
- Handles refcounted con `dup`, `dup2`, `waitpid(-1)` y procesos zombie/reap.
- Reclaim real de paginas para `exit`/`exec`, destruccion de `VmSpace` y
  liberacion de stacks de kernel al reapear procesos.
- `SVFS` con subdirectorios persistentes simples bajo `/disk`, `mkdir`,
  `rmdir` de directorios vacios, `truncate` explicito por path y `rename`
  persistente para archivos/directorios.
- SDK v1 minima en `C` (`crt0.S`, `libc.[ch]`, `userland/linker.ld`,
  `include/shared/syscall.h`) y tooling host para instalar apps externas
  directo en `build/disk.img`.
- Validacion basica de punteros de userland en syscalls principales, mas
  syscalls `seek`, `unlink`, `exec`, `mkdir`, `rmdir`, `truncate` y `rename`.
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

## Apps externas

El flujo principal para probar programas propios ya no requiere reconstruir el
`initramfs`. Compilas desde Windows contra la SDK v1 e instalas el ELF directo
en `build/disk.img`:

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

Los ejemplos base estan en `sdk/hello`, `sdk/errdemo` y `sdk/fsdemo`.

## Persistencia experimental

El build genera una imagen `build/disk.img` la primera vez y la conecta como un
disco IDE legacy adicional en QEMU. Esa imagen se monta en `/disk` mediante un
filesystem experimental propio (`SVFS`), de modo que:

```text
ls /disk
ls /disk/bin
cat /disk/README
echo hola > /disk/notes.txt
mv /disk/notes.txt /disk/tmp/notes.txt
rm /disk/tmp/notes.txt
seektest
renametest
truncatetest
```

deberian sobrevivir a reinicios posteriores mientras no ejecutes `clean`.
