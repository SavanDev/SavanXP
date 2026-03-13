# DoomGeneric para SavanXP

Estado actual del port:

- jugable en modo `fullscreen` exclusivo
- sin audio ni mouse en esta primera etapa
- usa `/dev/fb0` y `/dev/input0`
- guarda config y saves bajo `/disk/games/doom`
- se apoya en el input PS/2 actual para teclas especiales y en SVFS v2 para
  archivos que crecen en runtime
- sirve como primer juego porteado y como prueba fuerte de apps graficas
  externas sobre el ABI actual de SavanXP

Que fixes quedaron en cada capa:

- Kernel / sistema:
  `SVFS2` ahora monta una imagen mas grande y mantiene un layout consistente
  para journal, bitmap de bloques, bitmap de inodos y tabla de inodos; tambien
  soporta crecimiento real de archivos en runtime y mejor propagacion de
  errores de escritura desde `VFS`/process.
- SDK / runtime POSIX:
  la libc minima ahora resuelve mejor `snprintf`/`vsnprintf`, `FILE*`,
  `fflush`, `fclose`, `fseek` y `ftell`; el fix clave para `save/load` fue
  corregir `ftell()` para contar bytes pendientes en el buffer de escritura.
- Port especifico de Doom:
  el backend `doomgeneric_savanxp.c` sigue concentrando framebuffer, input,
  mapeo de teclas y salida limpia; ademas se ajustaron rutas de config/saves y
  algunos detalles de compatibilidad del menu/config del juego.

Build del port:

```powershell
.\sdk\doomgeneric\build.ps1
```

El script:

- compila e instala `/disk/bin/doomgeneric`
- crea `/disk/games/doom`
- copia `doom1.wad` desde `.\sdk\doomgeneric\wad\doom1.wad` si existe

Ubicacion esperada del WAD:

```text
sdk/doomgeneric/wad/doom1.wad
```

Por licencia no se incluye ningun WAD en el repo. Copia tu `doom1.wad` antes de correr el build.

Controles esperados en la build actual:

- flechas: mover/rotar
- `Ctrl`: disparar
- `Espacio`: usar / abrir puertas
- `Alt`: strafe
- `Shift`: correr
- `Esc`: menu / salir al menu
- `F1` a `F10`: teclas especiales del juego

Checklist corta de validacion manual:

- el juego arranca desde `doomgeneric`
- encuentra `doom1.wad` en `/disk/games/doom/doom1.wad`
- `New Game` entra a una partida sin volver a la shell
- `Ctrl` dispara y `Espacio` activa puertas/switches
- al salir, la shell recupera el framebuffer sin corrupcion visual

Smoke test automatizada:

```powershell
.\tools\doom-smoke.ps1
```

La smoke actual:

- bootea la VM
- lanza `doomgeneric` desde la shell
- entra a una partida
- toma capturas del menu, de una partida y despues de disparar para verificar cambio visual real

No valida `save/load`, cambio de nivel ni otros flujos largos. La salida limpia
de Doom a la shell sigue quedando como señal diagnostica (`ReturnedToShell`),
no como criterio duro de fallo.

Fuera de alcance de esta etapa:

- audio PCM / musica
- mouse
- ventanas o compositor
- multiplayer real
