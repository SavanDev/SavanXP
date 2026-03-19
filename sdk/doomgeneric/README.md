# DoomGeneric para SavanXP

Estado actual del port:

- jugable en modo `fullscreen` exclusivo
- sin audio en esta etapa
- usa `/dev/fb0`, `/dev/input0` y `/dev/mouse0`
- guarda config y saves bajo `/disk/games/doom`
- se apoya en el stack actual de input (`PS/2` o `virtio-input`, segun el
  entorno) para teclado y mouse
- se apoya en `SVFS2` para config, saves y archivos que crecen en runtime
- sirve como primer juego porteado y como prueba fuerte de apps graficas
  externas sobre el ABI actual de SavanXP

Ventajas practicas de `v0.1.4` para el port:

- el timer base a `200 Hz` mejora un poco la respuesta percibida del mouse y
  el redondeo practico de `sleep_ms()` durante el loop del juego
- los techos internos mas altos para procesos, descriptores y `VFS/SVFS2`
  dejan mas margen para ports grandes y assets persistentes
- `SVFS2` ya puede montar `/disk` en modo degradado de solo lectura frente a
  recovery incompleto, lo que evita que el port quede inutilizable por dejar
  el volumen directamente offline
- el build principal instala tambien binarios internos en `/disk/bin`, asi que
  la shell y las pruebas generales del sistema ejercitan mas seguido el mismo
  userland persistente que usa el port

Cosas nuevas de `v0.1.4` que todavia no cambian demasiado a Doom:

- `poll`, `select`, `fcntl` y `O_NONBLOCK` son una mejora real de la SDK, pero
  el backend actual de Doom sigue funcionando bien con `gfx_poll_event()` y
  `mouse_poll_event()` sin necesitar una refactorizacion inmediata
- `busybox` mejora la shell y el entorno general, pero no cambia la logica del
  port en si

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
- mouse: girar / mover vista
- click izquierdo: disparar
- click derecho: strafe
- click medio: avanzar
- `Esc`: menu / salir al menu
- `F1` a `F10`: teclas especiales del juego

Checklist corta de validacion manual:

- el juego arranca desde `doomgeneric`
- encuentra `doom1.wad` en `/disk/games/doom/doom1.wad`
- `New Game` entra a una partida sin volver a la shell
- `Ctrl` dispara y `Espacio` activa puertas/switches
- el mouse responde en menu y durante la partida
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
- ventanas o compositor
- multiplayer real
