# AGENTS

## Reglas del repo

- No romper la persistencia de apps externas instaladas en `build/disk.img`.
- `.\build.ps1 build` no debe borrar ni recrear incondicionalmente la imagen
  de disco si ya existe y es valida.
- Los cambios en kernel, build, SDK, `SVFS2` o tooling host no deben hacer que
  se pierdan binarios externos ya instalados en `/disk/bin` ni assets
  persistentes bajo `/disk/games`.

## Regla practica para el build principal

- El build principal puede sincronizar userland interno sobre la imagen
  existente, pero no debe resetearla salvo corrupcion real o incompatibilidad
  de formato.
- Si hace falta recrear `build/disk.img`, tiene que ser una decision
  deliberada y justificada, no el comportamiento normal de `.\build.ps1 build`.

## Verificacion minima cuando se toca esa zona

- Instalar una app externa en la imagen, por ejemplo con:
  `.\sdk\doomgeneric\build.ps1`
- Ejecutar despues:
  `.\build.ps1 build`
- Confirmar que el ejecutable sigue presente en `/disk/bin`
- Confirmar que sus assets persistentes, por ejemplo
  `/disk/games/doom/doom1.wad`, siguen presentes

## Caso de referencia actual

- `sdk/doomgeneric` se usa como prueba real de regresion para este punto.
- Si despues de un `build` el sistema no encuentra `doomgeneric`, el cambio
  debe tratarse como regresion del flujo de imagen persistente.
