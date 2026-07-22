# Modelo de capas del sistema (C nativo + apps Haxe sobre VM)

> Estado: **decisión fijada (2026-07-21).** Este documento es la **fuente de
> verdad** del layering de lenguajes de SavanXP. Si otro doc contradice esto,
> gana este.

## La regla en una línea

**El núcleo de bajo nivel y los programas del sistema se escriben en C. El resto
de las apps de usuario se escriben en Haxe, en lo posible, corriendo sobre la VM
(HashLink).**

Es el modelo **Android/ART**: un núcleo nativo maduro, y una capa de apps
gestionada encima. No es "reemplazar C con Haxe" — es "C es la plataforma, Haxe
es la capa de aplicaciones".

## Las tres capas

```
  ┌─────────────────────────────────────────────────────────────┐
  │  Apps de usuario (Haxe)                                      │
  │  corren sobre la VM; bindean SXGUI-C por la FFI              │
  └─────────────────────────────────────────────────────────────┘
                              ▲
                              │  FFI / interop (sxn_*), ABI nativo estable
                              ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Runtime intermedio (etapa: hoy AOT reflaxe.CPP → luego VM)  │
  │  el "CLR/ART" de SavanXP: runtime + GC + interop a C         │
  └─────────────────────────────────────────────────────────────┘
                              ▲
                              │  ABI nativo v1 (savanxp_native_abi.h)
                              ▼
  ┌─────────────────────────────────────────────────────────────┐
  │  Plataforma nativa (C)                                       │
  │  kernel · drivers · compositord · windowd · SXGFX · SXGUI-C  │
  │  + system apps: shell · file manager · task manager         │
  └─────────────────────────────────────────────────────────────┘
```

## Qué va en C (plataforma + system apps)

Bajo nivel, siempre C:

- **Kernel, drivers, HAL** (display/audio/GPU/red/almacenamiento).
- **`compositord`** — dueño de la GPU y del único surface de display.
- **`windowd`** — el window manager (extraído de `desktop.c`; ver
  [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md)).
- **SXGFX** — la capa de rasterización 2D del SDK
  (`subsystems/posix/sdk/v1/.../gfx2d.*`): superficies, painter, blit, clipping,
  damage. Análogo a GDI.
- **SXGUI-C** — el toolkit de widgets Win9x sobre SXGFX
  (`subsystems/posix/sdk/v1/.../sxgui.*`). Análogo a USER/comctl32.

Programas del sistema, en C **en esta etapa inicial**:

- **Shell** — el escritorio (`subsystems/posix/userland/desktop*.c`): taskbar,
  start menu, iconos, wallpaper. Tras la extracción del WM queda como
  `shell-client`, un cliente de `windowd`, **pero sigue en C**.
- **File manager** — `subsystems/posix/userland/filesapp.c`.
- **Administrador de tareas** — todavía no existe; se escribirá en C (el CLI
  `ps.c` puede sembrarlo).

> Puede sumarse algún programa del sistema más en C en el futuro si hace falta.
> La lista de arriba es el mínimo de esta etapa, no un techo.

## Qué va en Haxe (apps de usuario, vía VM)

Todo lo demás, **en lo posible**, se escribe en Haxe y corre sobre la VM. La
capa Haxe **no reimplementa** la plataforma: la consume.

- **SXGUI-C es el toolkit canónico y permanente.** Los system apps en C lo usan
  directamente; las apps Haxe lo **bindean** por la FFI de la VM. Es el modelo
  **WinForms/JNI**: una fachada gestionada fina sobre los controles nativos, no
  un segundo toolkit. Mantener dos toolkits en paralelo sería absurdo cuando el C
  no se va nunca.
- La pieza de diseño nueva del binding es el **marshalling de callbacks**
  (closures de Haxe ↔ punteros a función C de `sxgui_widget.on_action`),
  típicamente vía un trampolín C con `void* ctx`.

### Estado de las apps Haxe existentes

`sxguiapp`, `aboutapp-hx` y `filesapp-hx` (en `subsystems/native/haxe-*`) son
**demos de validación** del ABI y la cadena AOT — **no** reemplazos de sus
contrapartes en C, que son las oficiales. Igual, el `haxe-toolkit/`
(reimplementación de Painter/Boton/… en Haxe) es un **bootstrap** que validó la
cadena, no el estado final: el estado final es el binding a SXGUI-C.

## Etapas del runtime (el tier del medio)

El "CLR/ART" de SavanXP se construye por etapas; el ABI nativo se diseña **una
sola vez** y ambas etapas apuntan al mismo contrato:

1. **Hoy — AOT** con [reflaxe.CPP](https://github.com/SomeRanDev/reflaxe.CPP):
   Haxe → C++17 mínimo (sin GC) → ELF freestanding. Sirvió para validar la
   cadena y diseñar el ABI. Los ELF AOT son **artefactos de validación**, no el
   producto final.
2. **Destino — VM (HashLink)** en el SO: se porta el runtime/GC de HL sobre el
   ABI nativo y las apps Haxe corren sobre la VM. `sxn_*` y el ABI no cambian.

Ver [../subsystems/native/README.md](../subsystems/native/README.md) para el
detalle de fases y verificaciones.

## Relación con la extracción del WM

Esta decisión y [WM_SUBSYSTEM.md](WM_SUBSYSTEM.md) son **compatibles y
complementarias**:

- WM_SUBSYSTEM.md saca `windowd` (WM) de `desktop.c`. Es trabajo de plataforma,
  **en C**, e independiente del lenguaje de los clientes.
- Tras la extracción, el `shell-client` (y un eventual `progman`) **se quedan en
  C** — no se reescriben en Haxe. Esto elimina la preocupación de meter el
  runtime Haxe en el camino de boot crítico.

## Analogía (para fijar el modelo mental)

| Rol | SavanXP | Windows | Android |
|---|---|---|---|
| Rasterización 2D | **SXGFX** | GDI32 | Skia / hwui |
| Toolkit de controles | **SXGUI-C** | USER32 / comctl32 | (nativo) |
| Window manager | **windowd** | win32k / USER | WindowManager / SurfaceFlinger |
| Display server | **compositord** | DWM | SurfaceFlinger |
| Shell / launcher | **desktop\*.c** (C) | explorer.exe | SystemUI / Launcher |
| Runtime gestionado | **reflaxe.CPP → HashLink** | CLR | ART |
| Apps de usuario | **Haxe / VM** | .NET (WinForms) | Java/Kotlin |
