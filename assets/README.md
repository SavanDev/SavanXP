# Assets del desktop

Estructura:

```
assets/
  desktop/
    cursor.png                  cursor fuente del compositor
    menu_strip_savanxp.png      banda vertical izquierda del menu Start
    icons/
      16x16/                    iconos propios del desktop (16x16)
      32x32/                    iconos propios del desktop (32x32)
```

Todo este arte es propio del repo: se genera localmente con
`tools/GenerateDesktopSourceArt.ps1` y no depende de iconos externos ni de
assets temporales de terceros.

## Cursor

El cursor fuente vive en `assets/desktop/cursor.png`.

Sugerencias:
- `16x16` o `32x32`
- fondo transparente
- hotspot asumido en `0,0` por ahora, o sea la punta superior izquierda

Durante `.\build.ps1 build`, el archivo se convierte automaticamente a
`build/generated/cursor_asset.h` y queda embebido en el binario de
`subsystems/posix/userland/desktop.c`.

## Iconos del desktop

Los iconos PNG del desktop viven en `assets/desktop/icons/16x16` y
`assets/desktop/icons/32x32`:

- `desktop.png`
- `app-terminal.png`
- `app-spider.png`
- `app-libgfx-demo.png`
- `app-keyboard-settings.png`
- `app-mouse.png`

Durante `.\build.ps1 build`, esos PNG se convierten automaticamente a
`build/generated/desktop_icon_assets.h` y quedan embebidos en el binario del
desktop. El arte fuente se regenera con `tools/GenerateDesktopSourceArt.ps1`,
que produce un set propio con los mismos nombres y tamanos base.

## Banda lateral del menu Start

El arte para la banda vertical izquierda vive en
`assets/desktop/menu_strip_savanxp.png`. Tambien se convierte automaticamente
a `build/generated/desktop_icon_assets.h` durante `.\build.ps1 build` y se
regenera junto con los iconos del desktop.
