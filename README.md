# SavanXP

SavanXP es un sistema operativo experimental para `x86_64 + UEFI`, con
bootloader `Limine`, kernel propio en `C/C++` y un flujo de trabajo pensado
para desarrollarse y probarse desde Windows nativo con `PowerShell`.

El proyecto ya arranca a una sesion grafica funcional, dispone de un shell de
userland, un volumen persistente montado en `/disk`, una base POSIX minima,
apps internas, soporte para apps externas compiladas contra la SDK del repo y
un camino grafico sobre compositor propio.

Version actual: `v0.3.3`  
Historial de cambios: [`CHANGELOG.md`](CHANGELOG.md)  
Licencia: [`MIT`](LICENSE)

## Resumen

Estado actual del sistema:

- Kernel `x86_64` con arranque UEFI via Limine.
- Consola sobre framebuffer y salida serie temprana.
- Espacio de usuario con procesos `ELF64`, syscalls y scheduler preemptivo.
- Shell con `pipes`, redirecciones y builtins basicos.
- Sesion grafica estilo NT 3.5: window manager propio (`windowd`), fondo y
  Program Manager como clientes, y Task List (Ctrl+Esc).
- Volumen persistente `SVFS2` montado en `/disk`.
- Base POSIX y SDK v1 para compilar aplicaciones externas.
- Capa grafica 2D `sxgfx` para superficies, painter y conjuntos de rects.
- Soporte inicial de red, audio, input, GPU y almacenamiento.

## Requisitos

La via recomendada es hornear un toolchain local autocontenido:

```powershell
.\tools\bootstrap.ps1
```

Eso descarga versiones fijadas (LLVM/Clang con `ld.lld`, `llvm-objcopy` y
`llvm-readelf`; QEMU con el firmware OVMF que trae; `xorriso` para generar
ISOs; y `ninja`) a `toolchain/` (ignorado por git) y escribe el manifiesto
`toolchain/toolchain.json` que `build.ps1` consume. Las versiones estan
fijadas en `tools/toolchain.lock.json`; actualizar una herramienta es editar
ese archivo. `xorriso` se puede omitir con `-SkipXorriso`, y `ninja` con
`-SkipNinja`, si ya los tenes resueltos por otra via.

`build.ps1` no contiene rutas de ninguna maquina concreta: resuelve cada
herramienta en este orden y se queda con la primera que exista:

1. override explicito por variable de entorno
   (`SAVANXP_CLANG`, `SAVANXP_CLANGXX`, `SAVANXP_LD`, `SAVANXP_OBJCOPY`,
   `SAVANXP_READELF`, `SAVANXP_QEMU`, `SAVANXP_XORRISO`, `SAVANXP_NINJA`,
   `OVMF_CODE` / `OVMF_VARS`)
2. el toolchain horneado en `toolchain/`
3. el `PATH` del sistema

Por eso `bootstrap.ps1` es opcional: si ya tenes `clang++`, `ld.lld`,
`llvm-objcopy`, `llvm-readelf`, `ninja` y `qemu-system-x86_64` en el `PATH`,
el build funciona igual. Tambien hace falta `git` en el `PATH`. `build.ps1`
descarga automaticamente la rama binaria `v10.x-binary` de Limine si no
existe en `tools/limine`.

Ademas hace falta `python3` (o `python`) en el `PATH` con `Pillow` instalado
(`pip install Pillow`): `build.ps1` lo usa en cada build para generar el arte
del desktop y convertir los PNG de cursor/iconos a headers C
(`tools/gen_desktop_source_art.py`, `tools/gen_cursor_asset.py`,
`tools/gen_desktop_icon_assets.py`). No forma parte del toolchain horneado por
`bootstrap.ps1`.

Fuera de Windows, `.\build.ps1 iso` tambien necesita `make` y un compilador
`cc` en el `PATH`: la rama `v10.x-binary` de Limine solo trae `limine.exe`
prebuildeado para Windows, asi que ahi el deployer `limine` (para el arranque
BIOS de la ISO) se compila una vez desde `limine.c` con el `Makefile` del
propio repo de Limine.

### Linux nativo (sin `bootstrap.ps1`)

`build.ps1` en si mismo es un script de PowerShell: hace falta `pwsh` en el
`PATH`. Las distros no siempre lo empaquetan (Arch no lo trae en los repos
oficiales); el tarball `powershell-<version>-linux-x64.tar.gz` de
[PowerShell/PowerShell](https://github.com/PowerShell/PowerShell/releases) se
extrae directo, sin instalador.

Resolviendo el resto del toolchain por `PATH` (punto 3 del orden de arriba,
sin `bootstrap.ps1`), en Arch Linux los paquetes son:

```bash
pacman -S clang lld llvm qemu-system-x86 edk2-ovmf libisoburn python-pillow ninja
```

- `clang`/`lld`/`llvm`: compilador, linker (`ld.lld`) y las herramientas
  (`llvm-objcopy`, `llvm-readelf`) que usa `Add-SxeResources` para estampar
  recursos `.sxmeta`/`.sxicon`.
- `libisoburn` es el paquete que trae el binario `xorriso` (el nombre no
  coincide con el de la herramienta).
- `ninja` hace falta siempre, este o no horneado el toolchain: no es parte de
  ningun paquete base.
- Sin `bootstrap.ps1` no existe `toolchain/toolchain.json` con las rutas de
  OVMF: hay que definir `OVMF_CODE`/`OVMF_VARS` a mano, por ejemplo
  `/usr/share/edk2/x64/OVMF_CODE.4m.fd` y `.../OVMF_VARS.4m.fd` (rutas de
  `edk2-ovmf` en Arch).

Para `.\build.ps1 run`/`debug` (QEMU con ventana grafica), Arch separa los
backends de QEMU en paquetes aparte de `qemu-system-x86`:

```bash
pacman -S qemu-ui-gtk qemu-ui-opengl qemu-audio-sdl libpulse
```

Sin `qemu-ui-gtk` no existe el backend `-display gtk` que usa `Run-Qemu` (con
`qemu-system-x86` a secas, `-display help` solo lista `none`). Sin
`qemu-audio-sdl`, pedir `-audiodev sdl,...` (lo que usan `run`/`debug`) hace
**segfaultear** a QEMU en vez de fallar con un error legible. `libpulse` es lo
que le da a SDL2 un backend real de audio para hablar con el servidor de audio
del host (en WSL2, el socket Pulse que expone WSLg).

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

Para compilar sin las apps de testeo y diagnostico (keytest, gfxdemo, smoke,
etc.), usa `-NoTestApps`: esos binarios no entran al rootfs y el menu del
escritorio se compila sin sus entradas. Los comandos de automatizacion
(`smoke`, `windowd-smoke`, ...) las incluyen siempre porque sus harnesses
dependen de ellas.

```powershell
.\build.ps1 build -NoTestApps
```

Generar una ISO booteable:

```powershell
.\build.ps1 iso
```

La ISO queda en `build/SavanXP.iso`. Ese comando requiere `xorriso`, resuelto
por `SAVANXP_XORRISO`, por `toolchain/toolchain.json` o por el `PATH`, y usa el
arbol EFI ya preparado por el build. Si queres conservar datos de `/disk` al
arrancar en VirtualBox u otro hipervisor, adjunta tambien `build/disk.img` como
disco adicional.

## Ejecucion

Arrancar el sistema:

```powershell
.\build.ps1 run
```

Por defecto usa TCG (emulacion por software). Si tenes Hyper-V activo en
Windows, `-Accel whpx` acelera el boot usando el Windows Hypervisor Platform:

```powershell
.\build.ps1 run -Accel whpx
```

Nota: con whpx, `-cpu max`/`-cpu host` hacen crashear a OVMF con un #GP en
PlatformPei apenas arranca (WHPX no puede respaldar features de CPU muy
nuevas que esos modelos exponen al guest). Por eso `-Accel whpx` fuerza
`-cpu qemu64`, que arranca sin problemas.

La maquina QEMU se arma por defecto con hardware "base": VGA estandar, mouse
PS/2 y audio AC'97, el mismo que emula VirtualBox. Asi el kernel ejercita los
backends de fallback (`fb_gpu`, `ps2`, `ac97`) sin salir de QEMU. Para levantar
la maquina con dispositivos paravirtualizados (virtio-vga, virtio-tablet,
virtio-sound) hay que pedirlo explicitamente:

```powershell
.\build.ps1 run -Virtio
```

El switch vale para todos los comandos que lanzan QEMU (`run`, `debug`, los
smokes y `gpu-soak`). Los harnesses de audio que miden un driver concreto
(`ac97-count`, `virtio-count`, ...) fuerzan su dispositivo y no dependen de el.

Otras variantes disponibles:

```powershell
.\build.ps1 debug
.\build.ps1 smoke
.\build.ps1 windowd-smoke
.\build.ps1 gpu-soak
.\build.ps1 clean
```

Notas practicas:

- `run` inicia QEMU con sesion grafica y salida serie en la terminal.
- `debug` conserva el flujo de arranque orientado a depuracion.
- `smoke` ejecuta una prueba automatizada headless y deja logs en `build/`.
- `windowd-smoke` ejercita el compositor grafico.
- `gpu-soak` estresa el camino de presentacion de GPU.
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
- `windowd`
- `keytest`
- `mousetest`
- `gputest`
- `ping`
- `netinfo`
- `beep`
- `audiotest`

Ademas, varias utilidades basicas salen del multicall `busybox`, por ejemplo
`ls`, `cat`, `echo`, `mkdir`, `rm`, `mv`, `cp`, `ps`, `true`, `false` y
`sleep`.

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
- `libsvfs/`: nucleo portable del filesystem `SVFS2` y `svfs-cli`, el tool de
  host que hace toda la escritura de imagenes de disco
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
