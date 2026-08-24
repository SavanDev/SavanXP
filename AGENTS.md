# AGENTS

## Toolchain

- Las herramientas de build (clang, ld.lld, qemu, OVMF) se hornean con
  `tools/bootstrap.ps1` en `toolchain/` (ignorado por git). La resolucion vive
  centralizada en `tools/Toolchain.ps1` (env var > toolchain horneado > PATH).
- No volver a meter rutas absolutas de una maquina concreta en `build.ps1` ni en
  el tooling: si hace falta una herramienta nueva, agregarla a
  `tools/toolchain.lock.json` y al mapa de `tools/Toolchain.ps1`.

## Changelog

- Todo cambio de comportamiento (feature, fix, cambio de build/tooling,
  eliminacion) se documenta en `CHANGELOG.md` bajo `[Unreleased]`, en la
  subseccion que corresponda (`Agregado`/`Cambiado`/`Eliminado`/`Corregido`),
  como parte del mismo commit que lo introduce.
- No agregar la entrada a una seccion de version ya cerrada (con fecha): eso
  reescribe historia ya publicada. Si `[Unreleased]` no existe al tope del
  archivo, crearla.
- El release (`release(vX.Y.Z): ...`) es el unico commit que renombra
  `[Unreleased]` a la version nueva con fecha.

### Como se escribe una entrada

- Una entrada registra **que cambio visto desde afuera**, no como se
  implemento. El porque, el analisis de causa raiz, el recorrido por los
  archivos tocados y el detalle de la verificacion van en el mensaje del
  commit, que es donde alguien los va a buscar. El changelog se lee entero y de
  corrido; un commit se lee de a uno.
- Formato: un cambio por entrada, arrancando con una frase en negrita que diga
  el cambio. Un cambio normal entra en **2 a 6 lineas**; uno estructural grande
  puede llegar a ~12, nunca mas. Conservar los nombres que alguien va a
  necesitar para buscar (comandos, targets, flags, funciones de la API, logs de
  boot, requisitos de build nuevos) y, si el cambio no se entiende solo, una
  linea de contexto de que pasaba antes. Omitir el inventario de archivos, el
  paso a paso de la implementacion y el "Verificado con ...".
- Si varias entradas son partes de un mismo cambio, van juntas en una sola.
- Cada version lleva **una** subseccion de cada tipo como mucho, en el orden
  `Agregado`/`Cambiado`/`Eliminado`/`Corregido`. No repetir encabezados.
- La version `0.3.0` sirve de referencia de largo y tono.

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
