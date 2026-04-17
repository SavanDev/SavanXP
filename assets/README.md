Coloca el cursor fuente en `assets/cursor.png`.

Sugerencias:
- `16x16` o `32x32`
- fondo transparente
- hotspot asumido en `0,0` por ahora, o sea la punta superior izquierda

Durante `.\build.ps1 build`, el archivo se convierte automaticamente a `build/generated/cursor_asset.h` y queda embebido en el binario de `subsystems/posix/userland/desktop.c`.

## Iconos propios del desktop

Los iconos PNG del desktop viven en:

- `assets/desktop/original/16x16`
- `assets/desktop/original/32x32`

Durante `.\build.ps1 build`, esos PNG se convierten automaticamente a
`build/generated/desktop_icon_assets.h` y quedan embebidos en el binario del
desktop.

El arte fuente tambien se puede regenerar automaticamente con
`tools/GenerateDesktopSourceArt.ps1`, que produce un set propio del repo con
los mismos nombres y tamanos base.

## Placeholder de la banda lateral

El arte para la banda vertical izquierda del menu Start vive en:

- `assets/desktop/menu_strip_savanxp.png`

Durante `.\build.ps1 build`, tambien se convierte automaticamente a
`build/generated/desktop_icon_assets.h` y queda embebido en el desktop.

Ese bitmap tambien se regenera junto con los iconos del desktop.
