# DoomGeneric para SavanXP

Estado actual del port:

- jugable como app grafica externa sobre el stack `gfx_*` actual
- pensado para el modelo `desktop-first` de `SavanXP v0.2.2`
- render interno restaurado a `320x200`, con escalado entero y centrado
  dentro del `work area` del desktop
- control exclusivamente por teclado
- efectos de sonido por `/dev/audio0`
- musica deshabilitada de forma explicita en esta etapa
- config y saves persistentes bajo `/disk/games/doom`
- validacion del port solo por pruebas manuales

## Estado tecnico en `v0.2.2`

El port sigue compilando desde `sdk/doomgeneric`, pero la SDK canonica del
sistema ahora vive bajo `subsystems/posix/sdk/v1`. El build del port ya usa
esa ruta a traves de la tooling compartida del repo.

El backend no toma el scanout completo ni vuelve al fullscreen exclusivo
legacy. El camino normal sigue siendo:

- `gfx_open()` como cliente del compositor
- superficie compartida cuando el runtime la entrega
- `gfx_present_region()` para presentar solo la banda sucia del frame

La imagen del juego vuelve a salir de un frame interno `320x200`. Eso corrige
el tamaño visual que habia quedado demasiado chico desde `v0.2.2`, porque el
escalado entero vuelve a aprovechar mejor el area cliente disponible.

## Audio

El port ahora habilita solo `SFX`.

- usa `/dev/audio0`
- consulta el formato con `AUDIO_IOC_GET_INFO`
- mezcla a `PCM S16LE stereo 48 kHz`
- degrada limpio a "sin SFX" si el dispositivo no esta disponible

La musica no se reproduce todavia. El backend musical queda en no-op para que
el juego siga arrancando, entrando a partida y usando menu sin ruido ni crash.

## Input

El port queda intencionalmente en modo teclado-only. `DG_GetMouse()` sigue
existiendo como hook por compatibilidad con DoomGeneric, pero no publica input
real y el backend ya no abre `/dev/mouse0`.

Controles esperados:

- flechas: mover / rotar
- `Ctrl`: disparar
- `Espacio`: usar / abrir puertas
- `Alt`: strafe
- `Shift`: correr
- `Esc`: menu
- `F1` a `F10`: teclas especiales del juego

## Build del port

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

Por licencia no se incluye ningun WAD en el repo. Copia tu `doom1.wad` antes
de correr el build.

## Pruebas manuales recomendadas

- arrancar el sistema y ejecutar `doomgeneric`
- confirmar que la imagen se ve grande dentro del `work area` del desktop
- verificar `New Game`
- verificar disparo, puertas, correr y `Esc`
- verificar que se oyen al menos menu, disparo o puertas
- guardar, salir, volver a abrir y cargar una partida
- confirmar retorno limpio a shell o desktop al cerrar

## Fuera de alcance de esta etapa

- musica MIDI / MUS
- mouse ingame
- multiplayer real
- ventanas redimensionables o integracion multi-window
