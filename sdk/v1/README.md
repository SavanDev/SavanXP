# SDK v1

`sdk/v1` es la superficie pública actual para compilar apps externas en `C`
contra SavanXP.

## Superficie pública

Incluye:

- `include/savanxp/libc.h`
- `include/savanxp/syscall.h`
- `runtime/crt0.S`
- `runtime/libc.c`
- `linker.ld`
- `REFERENCE.md`

## ABI pública v1

Categorías soportadas:

- syscalls: `read`, `write`, `open`, `close`, `readdir`
- procesos: `spawn`, `spawn_fd`, `spawn_fds`, `exec`, `waitpid`
- descriptores: `pipe`, `dup`, `dup2`, `seek`
- filesystem: `unlink`, `mkdir`, `rmdir`, `truncate`, `rename`
- utilidades: `yield`, `sleep_ms`, `uptime_ms`, `clear_screen`, `proc_info`

Errores visibles:

- `SAVANXP_EINVAL`
- `SAVANXP_EBADF`
- `SAVANXP_ENOENT`
- `SAVANXP_ENOMEM`
- `SAVANXP_EBUSY`
- `SAVANXP_EEXIST`
- `SAVANXP_ENOTDIR`
- `SAVANXP_EISDIR`
- `SAVANXP_ENOSPC`
- `SAVANXP_EPIPE`
- `SAVANXP_ENOSYS`
- `SAVANXP_ENOTEMPTY`
- `SAVANXP_ECHILD`

Reglas del ABI:

- El ABI sigue siendo `int 0x80`.
- Los programas son `ELF64` estáticos para `x86_64`.
- No hay dynamic linker ni shared libraries.
- Las syscalls devuelven `>= 0` en éxito y `< 0` en error.
- `main(int argc, char** argv)` está soportado.
- `waitpid(SAVANXP_WAIT_ANY, &status)` está soportado.

Límites congelados para v1:

- `SAVANXP_MAX_ARGC = 15`
- `SAVANXP_MAX_ARG_LENGTH = 63`
- `SAVANXP_PROC_NAME_CAPACITY = 32`
- `SAVANXP_STDIN_FILENO`, `SAVANXP_STDOUT_FILENO`, `SAVANXP_STDERR_FILENO`

Helpers públicos del runtime:

- errores: `result_is_error`, `result_error_code`, `result_error_string`
- logging: `puts_err`, `printf_fd`, `eprintf`
- introspección: `process_state_string`

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
- usa `sdk/v1` como runtime público en vez del árbol interno de `userland`

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

Para el detalle de límites y contratos estables, ver `sdk/v1/REFERENCE.md`.
