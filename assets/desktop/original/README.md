Arte fuente propio del desktop de SavanXP.

Contenido:

- `16x16/desktop.png`
- `16x16/app-terminal.png`
- `16x16/app-spider.png`
- `16x16/app-libgfx-demo.png`
- `16x16/app-keyboard-settings.png`
- `16x16/app-mouse.png`
- `32x32/desktop.png`
- `32x32/app-terminal.png`
- `32x32/app-spider.png`
- `32x32/app-libgfx-demo.png`
- `32x32/app-keyboard-settings.png`
- `32x32/app-mouse.png`

Estos PNG se generan localmente con `tools/GenerateDesktopSourceArt.ps1` y se
embeben durante `.\build.ps1 build` mediante `tools/GenerateDesktopIconAssets.ps1`.

No dependen de iconos externos ni de assets temporales de SerenityOS.
