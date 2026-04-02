Coloca el cursor fuente en `assets/cursor.png`.

Sugerencias:
- `16x16` o `32x32`
- fondo transparente
- hotspot asumido en `0,0` por ahora, o sea la punta superior izquierda

Durante `.\build.ps1 build`, el archivo se convierte automaticamente a `build/generated/cursor_asset.h` y queda embebido en el binario de `subsystems/posix/userland/desktop.c`.
