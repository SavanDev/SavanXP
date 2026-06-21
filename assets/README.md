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

## Tipografias

Las fuentes fuente viven en `assets/desktop/fonts/`:

```
fonts/
  unifont.hex              GNU UniFont 17.0.04 (bitmap, consola/terminal)
  NotoSans-Regular.ttf     Noto Sans Regular v1.06 (proporcional, escritorio)
  LICENSE-OFL-1.1.txt      SIL Open Font License 1.1 (ambas)
```

A diferencia de los PNG, las fuentes **no** se convierten durante `build.ps1`: se
hornean a mano con `tools/font/genfont.py` (requiere `pip install freetype-py`) y
las tablas `.inc` resultantes se commitean. UniFont sale del `.hex` canonico
(bitmaps 8x16 nitidos); Noto Sans se rasteriza del TTF como atlas de cobertura
antialiased. Procedencia y licencias en `docs/THIRD_PARTY_PROVENANCE.md`.

Para regenerar (desde la raiz del repo):

```
python tools/font/genfont.py unifont --hex assets/desktop/fonts/unifont.hex \
    --out include/kernel/console_font_unifont.inc \
    --out subsystems/posix/sdk/v1/runtime/console_font_unifont.inc
python tools/font/genfont.py noto --ttf assets/desktop/fonts/NotoSans-Regular.ttf \
    --out subsystems/posix/sdk/v1/runtime/gfx_font_noto.inc
```
