# SavanXP

SavanXP es un sistema operativo experimental para `x86_64 + UEFI`, con
bootloader `Limine`, kernel propio en `C/C++` y un flujo de trabajo pensado
para desarrollarse y probarse desde Windows nativo con `PowerShell`.

El proyecto ya arranca a una sesion grafica funcional, dispone de un shell de
userland, un volumen persistente montado en `/disk`, una base POSIX minima,
apps internas, soporte para apps externas compiladas contra la SDK del repo y
un camino grafico sobre compositor propio.

Version actual: `v0.3.0`  
Historial de cambios: [`CHANGELOG.md`](CHANGELOG.md)  
Licencia: [`MIT`](LICENSE)

## Resumen

Estado actual del sistema:

- Kernel `x86_64` con arranque UEFI via Limine.
- Consola sobre framebuffer y salida serie temprana.
- Espacio de usuario con procesos `ELF64`, syscalls y scheduler preemptivo.
- Shell con `pipes`, redirecciones y builtins basicos.
- Desktop inicial con taskbar, menu Inicio y apps cliente.
- Volumen persistente `SVFS2` montado en `/disk`.
- Base POSIX y SDK v1 para compilar aplicaciones externas.
- Capa grafica 2D `sxgfx` para superficies, painter y conjuntos de rects.
- Soporte inicial de red, audio, input, GPU y almacenamiento.

## Requisitos

La via recomendada es hornear un toolchain local autocontenido:

```powershell
.\tools\bootstrap.ps1
```

Eso descarga versiones fijadas (LLVM/Clang con `ld.lld`, QEMU y el firmware
OVMF que viene con QEMU) a `toolchain/` (ignorado por git) y escribe el
manifiesto `toolchain/toolchain.json` que `build.ps1` consume. Las versiones
estan fijadas en `tools/toolchain.lock.json`; actualizar una herramienta es
editar ese archivo.

`build.ps1` no contiene rutas de ninguna maquina concreta: resuelve cada
herramienta en este orden y se queda con la primera que exista:

1. override explicito por variable de entorno
   (`SAVANXP_CLANG`, `SAVANXP_CLANGXX`, `SAVANXP_LD`, `SAVANXP_QEMU`,
   `OVMF_CODE` / `OVMF_VARS`)
2. el toolchain horneado en `toolchain/`
3. el `PATH` del sistema

Por eso `bootstrap.ps1` es opcional: si ya tenes `clang++`, `ld.lld` y
`qemu-system-x86_64` en el `PATH`, el build funciona igual. Tambien hace falta
`git` en el `PATH`. `build.ps1` descarga automaticamente la rama binaria
`v10.x-binary` de Limine si no existe en `tools/limine`.

## Compilacion

Compilar el sistema:

```powershell
.\build.ps1 build
```

Ese comando:

- compila kernel y userland interno
- genera el `initramfs`
- prepara la imagen EFI de arranque
- crea `build/disk.img` si todavia no existe
- sincroniza el contenido interno sobre el volumen persistente

Importante: el build normal no debe recrear `build/disk.img` de forma
incondicional. La imagen persistente se conserva entre builds salvo corrupcion
real o incompatibilidad de formato.

## Ejecucion

Arrancar el sistema:

```powershell
.\build.ps1 run
```

Otras variantes disponibles:

```powershell
.\build.ps1 debug
.\build.ps1 smoke
.\build.ps1 clean
```

Notas practicas:

- `run` inicia QEMU con sesion grafica y salida serie en la terminal.
- `debug` conserva el flujo de arranque orientado a depuracion.
- `smoke` ejecuta una prueba automatizada headless y deja logs en `build/`.
- `clean` elimina artefactos de compilacion y puede forzar la recreacion del
  entorno en el siguiente build.

## Primer arranque

El sistema entra a `init` y luego a `sh` como shell principal. Para ver el
estado base del sistema desde el guest:

```text
sysinfo
df
ls /disk
```

Comandos utiles incluidos en el userland actual:

- `sh`
- `sysinfo`
- `df`
- `desktop`
- `keytest`
- `mousetest`
- `gputest`
- `ping`
- `netinfo`
- `beep`
- `audiotest`

Ademas, varias utilidades basicas salen del multicall `busybox`, por ejemplo
`ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp`, `true`, `false` y `sleep`.

## Apps externas

El flujo recomendado para probar programas propios no requiere reconstruir el
`initramfs`. Las apps externas se compilan contra la SDK y se instalan directo
en `build/disk.img`, normalmente bajo `/disk/bin`.

Ejemplo:

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\sdk\hello\main.c -Name hello
.\build.ps1 run
```

Dentro de SavanXP:

```text
which hello
hello
```

Tambien existe un wrapper para compilar, instalar y arrancar el sistema en un
paso:

```powershell
.\tools\run-user.ps1 -Source .\sdk\errdemo\main.c -Name errdemo
```

Ejemplos incluidos:

- `sdk/hello`
- `sdk/errdemo`
- `sdk/fsdemo`
- `sdk/gfxhello`
- `sdk/doomgeneric`

## Persistencia

SavanXP usa una imagen de disco persistente en `build/disk.img`, montada como
`/disk` dentro del sistema mediante `SVFS2`.

Esto permite:

- conservar archivos entre reinicios
- instalar binarios externos en `/disk/bin`
- guardar assets y datos persistentes bajo `/disk`

Ejemplo dentro del guest:

```text
echo hola > /disk/notes.txt
sync
cat /disk/notes.txt
```

El flujo del repo protege esta persistencia: un `.\build.ps1 build` no debe
eliminar aplicaciones externas ya instaladas ni assets persistentes como los
de `doomgeneric`.

## Estructura del repositorio

Directorios principales:

- `arch/`: codigo especifico de arquitectura
- `kernel/`: kernel y subsistemas base
- `subsystems/posix/`: capa POSIX, SDK y userland principal
- `rootfs/`: contenido del `initramfs`
- `diskfs/`: contenido inicial del volumen persistente
- `sdk/`: ejemplos, tooling y ports externos
- `tools/`: scripts host-side y utilidades de desarrollo
- `vendor/`: dependencias de terceros

## Estado del proyecto

SavanXP ya supero la etapa de arranque minimo. Hoy ofrece una base coherente
para seguir evolucionando:

- kernel y userland propios
- desktop inicial usable
- camino grafico bajo compositor
- persistencia real sobre `/disk`
- soporte para ports y aplicaciones externas

Todavia sigue siendo un sistema experimental, con APIs y subsistemas en
evolucion, pero ya apunta a ser una base de trabajo consistente y demostrable.
