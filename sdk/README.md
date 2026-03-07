# SDK v1

Esta carpeta contiene ejemplos de apps externas en `C` para SavanXP. Se compilan en Windows y se instalan directo en `build/disk.img`, sin reconstruir `initramfs`.

Flujo base:

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\sdk\hello\main.c -Name hello
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

Instalacion con ruta explicita:

```powershell
.\tools\build-user.ps1 -Source .\sdk\fsdemo\main.c -Name fsdemo -Destination /disk/bin/fsdemo
```

Compilar, instalar y arrancar QEMU en un paso:

```powershell
.\tools\run-user.ps1 -Source .\sdk\errdemo\main.c -Name errdemo
```
