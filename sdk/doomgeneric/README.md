# DoomGeneric para SavanXP

Estado actual del port:

- jugable como app grafica externa sobre el stack actual de `gfx_*`
- sin audio en esta etapa
- usa `gfx_open()` como cliente del compositor; el runtime ya no cae a un
  framebuffer exclusivo directo
- usa `/dev/input0` y `/dev/mouse0` para teclado y mouse
- guarda config y saves bajo `/disk/games/doom`
- se integra mejor con la sesion grafica actual; cuando entra como
  cliente del compositor recibe superficie compartida y presentacion via
  `fd 3..6`
- se apoya en el stack actual de input (`PS/2`, `virtio-input` o `virtio-tablet`,
  segun el entorno) para teclado y mouse
- se apoya en `SVFS2` para config, saves y archivos que crecen en runtime
- sirve como primer juego porteado y como prueba fuerte de apps graficas
  externas sobre el ABI actual de SavanXP

Que cambio con `v0.2.1` y como impacta al port:

- la documentacion vieja basada en `/dev/fb0` ya no describe el camino real;
  el port hoy vive sobre `gfx_*` y corre como cliente del compositor
- el backend no se rompio de lleno porque ya usaba `gfx_open()` y
  `gfx_present_region()`; ademas, cuando el runtime entrega una superficie
  compartida de cliente, Doom ahora renderiza directo ahi en lugar de usar un
  buffer extra propio
- el modelo natural de ejecucion ya no es "tomar el framebuffer exclusivo",
  sino integrarse al desktop y a `shellapp`; Doom sigue ocupando todo el
  work area disponible del compositor, con la taskbar del desktop visible
  fuera de esa superficie

Ventajas practicas de `v0.2.1` para el port:

- `gfx_open()` ahora es compositor-first, asi que Doom puede correr como
  cliente grafico real sin rehacerse alrededor de una ABI nueva
- la presentacion por regiones sobre superficies compartidas encaja bien con
  el backend actual, que ya detecta filas sucias y evita redibujar todo el
  frame
- `sleep_ms()` ahora corre sobre timers waitables del kernel, mejorando el
  pacing del loop principal
- `SVFS2` y el runtime POSIX ya quedaron lo bastante firmes como para sostener
  `save/load` sin workarounds del lado del juego
- `/dev/audio0` deja por primera vez una base realista para agregar sonido al
  port sin tener que inventar infraestructura nueva

Cosas nuevas de `v0.2.1` que todavia no cambian demasiado a Doom:

- el port no necesita hablar directo con `GPU_IOC_*`; `gfx_*` ya absorbe casi
  todo el cambio de arquitectura
- Doom tampoco necesita por ahora `poll`, `select`, `fcntl` u otros extras del
  runtime nuevo, porque su backend actual ya funciona bien con
  `gfx_poll_event()` y `mouse_poll_event()`
- el mayor salto pendiente no es de compatibilidad sino de aprovechamiento:
  agregar audio real y, si hiciera falta mas tuning, seguir bajando el costo
  del present en modo cliente

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
  el backend `doomgeneric_savanxp.c` sigue concentrando render, input, mapeo
  de teclas, mouse y salida limpia; ademas se ajustaron rutas de config/saves,
  algunos detalles de compatibilidad del menu/config del juego y el render
  parcial por filas sucias.

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

Smoke visual opcional:

```powershell
.\tools\doom-smoke.ps1
```

La smoke actual:

- bootea la VM
- espera al `desktop` y lanza `doomgeneric` desde la sesion grafica
- entra a una partida
- toma capturas del menu, de una partida y despues de disparar para verificar cambio visual real

No valida `save/load`, cambio de nivel ni otros flujos largos. En el modelo
desktop-first actual sirve como smoke visual del port y del retorno al
desktop, no como prueba principal del stack GPU del SO.

Fuera de alcance de esta etapa:

- audio PCM / musica dentro del juego
- integracion con ventanas propias; el port sigue ocupando el work area
  completa aunque ya se apoye mejor en el compositor
- multiplayer real
