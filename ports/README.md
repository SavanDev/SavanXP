# ports

Banco de pruebas **local**. Todo lo que este en esta carpeta esta gitignorado
(salvo este README): sirve para probar ports y apps que no van a llegar al
repo, sin ensuciar `sdk/` ni el historial.

`sdk/` = ejemplos canonicos y versionados del SDK.
`ports/` = tu copia local, descartable.

## Flujo

Es el mismo tooling que `sdk/`, que ya acepta cualquier ruta como `-Source`:

```powershell
.\build.ps1 build
.\tools\build-user.ps1 -Source .\ports\miport -Name miport
.\build.ps1 run
```

App nueva desde el template del SDK, directo en `ports/`:

```powershell
.\tools\new-user-app.ps1 -Name miport -DestinationRoot ports
```

Compilar sin instalar en `build/disk.img`:

```powershell
.\tools\build-user.ps1 -Source .\ports\miport -Name miport -NoInstall
```

Compilar, instalar y arrancar QEMU en un paso:

```powershell
.\tools\run-user.ps1 -Source .\ports\miport -Name miport
```

## Que acepta el compilador

Identico a `sdk/` (ver `sdk/README.md` para el detalle):

- un `.c` suelto o un directorio entero con fuentes `.c` y `.S`
- headers locales en la raiz del directorio o en `include/`
- `compile-exclude.txt` con una ruta relativa por linea para saltear fuentes
- `<nombre>.sxres` al lado de la fuente para estampar icono/metadata SXE

Se linkean siempre `crt0` + `libc` + `posix` + `gfx` + `gfx2d` + `setjmp` desde
`subsystems/posix/sdk/v1`. Los includes publicos (`savanxp/libc.h`,
`savanxp/gfx2d.h`, ...) y `include/` del repo ya vienen en los `-I`.

## Apps con ventana

El toolkit SXGUI (`savanxp/sxgui.h`) no entra por default: suma ~48 KB al
binario y una app de consola no lo necesita. Se pide con `-Gui`, que linkea
`sxgui.c` + `sxgui_app.c`, el mismo par que build.ps1 le pone a las apps
ventaneadas in-tree:

```powershell
.\tools\build-user.ps1 -Source .\ports\miapp -Name miapp -Gui
.\tools\run-user.ps1 -Source .\ports\miapp -Name miapp -Gui
```

Sin el switch el link falla con simbolos `sxgui_*` sin resolver.

## Ports pesados

Si el port necesita un pipeline propio (mas de un artefacto, WADs, assets),
copiale el patron a `sdk/doomgeneric/build.ps1`: script propio en la carpeta del
port que termina llamando a `tools/build-user.ps1`.
