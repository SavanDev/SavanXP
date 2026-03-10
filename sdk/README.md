# SDK

Esta carpeta contiene ejemplos de apps externas en `C` para SavanXP. Se compilan en Windows y se instalan directo en `build/disk.img`, sin reconstruir `initramfs`.

La superficie pública actual está en `sdk/v1`.

Flujo base:

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\sdk\hello -Name hello
.\build.ps1 run
```

Una vez en SavanXP:

```text
which hello
hello
```

Ejemplos incluidos:

- `sdk/hello/main.c`: salida simple a `stdout`
- `sdk/errdemo/main.c`: salida simple a `stderr`
- `sdk/fsdemo/main.c`: create/write/read en `/disk/tmp`
- `sdk/pathops/main.c`: `mkdir`/`rename`/`truncate`/`unlink`/`rmdir`
- `sdk/procpeek/main.c`: snapshot simple de procesos
- `sdk/spawnwait/main.c`: `spawn` + `waitpid`
- `sdk/statusdemo/main.c`: errores, estados de proceso y metadata del SDK
- `sdk/gfxhello/main.c`: app fullscreen usando `gfx_*` desde el SDK
- `sdk/doomgeneric/`: port de DoomGeneric con script propio y carpeta `wad/` para copiar `doom1.wad`
- `sdk/udptest/main.c`: self-test local de sockets UDP IPv4
- `sdk/tcpget/main.c`: cliente HTTP minimo sobre sockets TCP IPv4
- `sdk/multifile/*`: app externa con varias fuentes y headers locales
- `sdk/template/main.c`: punto de partida mínimo para apps nuevas

Las apps deberían incluir:

```c
#include "savanxp/libc.h"
```

El tooling acepta:

- un archivo `.c` individual
- un directorio completo con fuentes `.c` y `.S`
- headers locales en el directorio raíz o en `include/`

Instalacion con ruta explicita:

```powershell
.\tools\build-user.ps1 -Source .\sdk\fsdemo -Name fsdemo -Destination /disk/bin/fsdemo
```

Compilar solo el ELF sin instalar:

```powershell
.\tools\build-user.ps1 -Source .\sdk\procpeek -Name procpeek -NoInstall
```

Ejemplo multifile:

```powershell
.\tools\build-user.ps1 -Source .\sdk\multifile -Name multifile
```

Crear una app nueva desde el template:

```powershell
.\tools\new-user-app.ps1 -Name miapp
.\tools\build-user.ps1 -Source .\sdk\miapp -Name miapp
```

Estructura recomendada para una app nueva:

```text
miapp/
  main.c
  include/
    miapp.h
  helper.c
```

Compilar, instalar y arrancar QEMU en un paso:

```powershell
.\tools\run-user.ps1 -Source .\sdk\errdemo -Name errdemo
```

Caso DoomGeneric:

```powershell
.\sdk\doomgeneric\build.ps1
```
