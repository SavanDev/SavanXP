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
- descriptores estándar:
  - `SAVANXP_STDIN_FILENO`
  - `SAVANXP_STDOUT_FILENO`
  - `SAVANXP_STDERR_FILENO`
- `waitpid(SAVANXP_WAIT_ANY, &status)` está soportado

## Convenciones

- éxito: retorno `>= 0`
- error: retorno `< 0`
- para decodificar errores usar:
  - `result_is_error`
  - `result_error_code`
  - `result_error_string`
- para estados de proceso usar:
  - `process_state_string`

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
