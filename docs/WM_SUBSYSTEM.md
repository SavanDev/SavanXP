# Extracción del Window Manager a subsistema (modelo NT 3.5)

> **Estado: Fase A COMPLETA (A1 + A2 + A3), en master.**
>
> Lo que quedó construido:
>
> - El WM es `windowd.c` + módulos `windowd_*` (`_session`, `_render`,
>   `_layout`, `_compositor_client`, `_appinfo`). Binario `/bin/windowd`.
> - El shell son procesos cliente: `shellui` dibuja el fondo, `progman` es el
>   launcher, con su registro en `/disk/progman.ini`.
> - El contrato WM↔cliente es `savanxp/wm_protocol.h` (fds 3..10).
> - El taskbar lo reemplaza el Task List (Ctrl+Esc), UI del WM.
> - Módulos compartidos con clientes (`desktop_icons`, `desktop_wallpaper`)
>   conservan su nombre a propósito: no son del WM.
>
> Siguen **Fase B** (primitiva MDI en sxgui para los grupos de Progman) y
> **Fase C** (la maduración que motivó todo: resize por bordes,
> foco/activación, Alt-Tab, corrección de repintado).
>
> **De acá para abajo, el documento es el registro del plan original** — con
> las decisiones y los hallazgos tal como se tomaron. Está en tiempo futuro y
> nombra archivos con sus nombres viejos (`desktop.c`, `desktop_*`); se
> conserva así porque el valor es el razonamiento, no el estado.
>
> **Decisión-crux resuelta:** el **shell dibuja el fondo** (NT puro). El
> shell-client posee una superficie full-screen de background; `windowd` solo la
> compone al fondo del z-order y no sabe nada de wallpaper.

## Motivación

Hoy `subsystems/posix/userland/desktop.c` es **dos cosas soldadas**:

1. **El window manager**: dueño de la conexión a `compositord`, del
   `struct desktop_session` (z-order en `overlay_order[]`, foco, drag,
   minimize/maximize/fullscreen), del ruteo de input al cliente activo, del
   ciclo de frame (submit/retire/compose) y del pase de composición por
   software de todas las superficies de clientes al display buffer.
2. **El chrome del shell**: taskbar, start menu, iconos del escritorio y
   wallpaper — dibujados *directamente* al display buffer vía
   `desktop_render.c`, con su input manejado inline en el `for(;;)` de `main()`.

Esto es exactamente el layering invertido respecto a NT 3.5, donde el window
manager (USER) vivía en modo usuario dentro de **CSRSS** (subsistema separado)
y Program Manager (`progman.exe`) era **solo un cliente más** — el shell —
nunca el WM. Queremos ese layering porque el objetivo es *madurar el
compositor y la experiencia de uso*: ese trabajo (resize, foco/activación,
Alt-Tab, corrección de repintado) es trabajo de WM, y se hace mucho más limpio
contra un servidor dedicado que contra un archivo que también dibuja el start
menu.

## Layering objetivo

```
  compositord          (owns GPU, un solo surface de display; SIN CAMBIOS)
      ▲
      │ compositor_protocol.h  (fds 3/4/5: request/reply/display section)
      │
  windowd  (WM server)  ← nuevo proceso = desktop.c MENOS el chrome
      ▲
      │ protocolo WM↔cliente (fds 3..10, ver abajo)
      │
   ┌──┴───────────────┬──────────────┐
 shell-client       app (aboutapp,  app (...)
 (progman /         filesapp, ...)
  desktop chrome)
```

- **`compositord`** — sin cambios. Dueño de la GPU, único cliente privilegiado
  = `windowd`.
- **`windowd`** — el proceso `desktop.c` actual, **menos** el rendering de
  chrome. Mantiene: conexión a compositord, `desktop_session`,
  `overlay_clients[]` + `overlay_order[]`, ruteo de input, ciclo de frame,
  el pase genérico de composición, `start_client_process`, reap, launch relay.
- **shell-client** — el chrome (taskbar / start menu / iconos / wallpaper),
  corriendo como **cliente** de `windowd` por el **mismo** protocolo `gfx_*`
  que usa cualquier app, más una extensión privilegiada (ver crux).
- **apps** — clientes, sin cambios.

## El protocolo WM↔cliente ya existe (implícito)

`start_client_process()` (desktop.c:1265) hace `fork` + `dup2` de un contrato
fijo de fds sobre cada cliente:

| fd | contenido                       | dirección        |
|----|---------------------------------|------------------|
| 3  | surface de memoria compartida (header + dirty-rect batches + pixels) | WM→app (map RW) |
| 4  | input de teclado                | WM→app           |
| 5  | input de mouse                  | WM→app           |
| 6  | evento *submit* (frame listo)   | app→WM           |
| 7  | evento *retire* (frame liberado)| WM→app           |
| 8  | evento *shutdown*               | WM→app           |
| 9  | launch pipe (app pide lanzar otra app) | app→WM    |
| 10 | cursor hint pipe (forma del cursor) | app→WM       |

La **mitad-cliente** de este protocolo ya está factorizada como librería en
`subsystems/posix/sdk/v1/runtime/gfx_impl.inc` (detrás de
`gfx_open/acquire/present/poll_event` + `sxgui_app_*`); las apps **no**
hardcodean números de fd. La **mitad-servidor** es lo único embebido en
desktop.c. Extraer el WM = mudar la mitad-servidor a `windowd` intacta, y
convertir al shell en un cliente de ella.

## Decisión-crux: root window + ruteo de input al shell

El `main()` actual fusiona en un solo switch el dispatch del WM y el input del
chrome. Al separar el shell a otro proceso hay **una** cosa genuinamente nueva
que diseñar: cómo le llega input al shell.

- **SUPER** (abrir start menu) debe llegar al shell *aunque una app tenga el
  foco* → el WM necesita **global hotkeys** entregados al shell-client.
- **Clicks fuera de toda ventana de app** (wallpaper, taskbar) deben rutear al
  shell → el WM trata al shell como **root/background window** (el fondo del
  z-order) más una **franja reservada de taskbar**.

### Opción recomendada (para Fase A)

Registrar el shell como un cliente con un **rol privilegiado** (`ROLE_SHELL`):

1. Su superficie es el **background/root window**: full-screen, siempre en el
   fondo del z-order. Dibuja wallpaper + iconos ahí. Reemplaza el dibujo
   directo de wallpaper que hoy hace el proceso desktop.
2. El WM le entrega: (a) todo click que **no** cae sobre una ventana de app
   (background + taskbar), y (b) un conjunto chico de **hotkeys globales**
   (SUPER como mínimo) vía el pipe de input normal (fd 4), marcados para que el
   shell sepa distinguir "global" de "tengo foco".
3. La **taskbar** puede quedar, en Fase A, como una franja que el WM reserva
   del work-area (igual que hoy `desktop_layout.c` recorta el menú lejos de la
   taskbar) y cuyo input rutea al shell.

Esto mantiene el comportamiento observable idéntico (wallpaper, menú, taskbar
se ven y responden igual), solo que ahora el chrome se dibuja desde un proceso
cliente. `F11` (fullscreen composited) y el ruteo al cliente activo **quedan en
el WM** — son concerns de WM, no de shell.

## Plan de ejecución (Fase A: behavior-preserving)

Enfoque de dos saltos, para des-riesgar el corte de un proceso boot-crítico:

1. **A1 — Boundary en-proceso (= refactor de arbitración de input).** El
   dispatch de `main()` es hoy un switch interleaveado donde WM y shell se
   mezclan en el mismo evento, con locals compartidos (`menu_open`,
   `selected_shortcut`, `context_menu`, `confirm_action`, `welcome_visible`) y
   precedencia implícita en el orden del switch. A1 lo reescribe como
   arbitración explícita:

   - **`wm_handle_key` / `wm_handle_pointer`** — primer turno. Consume: F11
     (hotkey global), drag de ventana, hit sobre ventana → `route_*` al cliente
     activo, activate/raise. Devuelve *consumido/no-consumido*.
   - **`shell_handle_key` / `shell_handle_pointer`** — segundo turno, solo si el
     WM no consumió. Consume: start menu, menú contextual, shortcuts, taskbar,
     power, welcome. Todo el estado de chrome (`menu_open`, etc.) se muda a un
     `struct shell_state` privado del módulo shell.

   Esa precedencia WM→shell *es* la frontera de proceso de A2 (el WM decide:
   ¿pega en ventana? ¿hotkey global? si no → al shell). Se mantiene en un solo
   proceso.

   **Hallazgo al implementar (teclado vs. mouse):** el teclado bisecta limpio
   (`shell_notify_key` → `wm_handle_key` → `shell_handle_key`) porque cada
   tecla la consume exactamente un dueño. El **mouse no**: dentro de un mismo
   evento el update de cursor/hover (WM) corre primero porque todo depende de
   él, los modales del shell (confirm dialog, menú contextual) consumen con
   `continue`, y el dispatch de click izquierdo es un if/else-chain único que
   mezcla chrome (start/power/menú/taskbar/shortcut) con hit-de-ventana (WM),
   con precedencia por orden. Forzar un split de dos vías ahí reordena y
   arriesga el comportamiento. Descomposición efectiva del mouse en A1:
   - **Extraíble limpio ahora:** los dos bloques modales →
     `shell_pointer_handle_confirm` / `shell_pointer_handle_context_menu`. Son
     los casos "el shell agarró el input", que A2 reenvía enteros al
     shell-client.
     Devuelven consumido; `main()` hace `last_buttons`+`continue`.
   - **Se bisecta en A2, no en A1:** el chain mixto de click izquierdo (chrome
     vs. hit-de-ventana) queda inline. La frontera de proceso de A2 lo fuerza
     de forma natural (el WM decide reenviar-al-shell vs. manejar-la-ventana) y
     ahí es testeable con el shell ya como cliente separado. Verificar con `desktop --selftest` / `build.ps1 desktop-smoke`
   (compositor headless) + QMP mouse-driving. Cero cambio de comportamiento.

   **Estado de chrome que migra a `struct shell_state`:** `menu_open`,
   `selected_index`, `selected_shortcut`, `context_menu`, `confirm_action`,
   `welcome_visible`/`welcome_until_ms`, `last_shortcut_click(_ms)`. **Queda en
   el WM:** `drag_overlay_slot`/offsets, `cursor_x/y`, `last_buttons`, todo
   `session`.

   **✅ Resultado (A1 HECHO):** módulo `desktop_shell.h/.c` con `struct
   shell_state` + `shell_state_init()`; teclado como arbitración
   `shell_notify_key`→`wm_handle_key`→`shell_handle_key`; modales de puntero en
   `shell_pointer_handle_confirm`/`_context_menu`; cuerpo per-evento de puntero
   en `handle_pointer_event()`; render con `shell_state` +
   `paint_layer`→`wm_paint_layer`(cliente/cursor)/`shell_paint_layer`(chrome).
   `main()` quedó en ~247 líneas (601 antes). Cada corte verificado con
   `desktop-smoke` (`DESKTOP SMOKE PASS`). El chain mixto de click izquierdo y
   `shell_paint_layer` son las piezas que A2 mueve al shell-client.
2. **A2 — Lift del shell a proceso.** Convertir la capa `shell_*` en un
   cliente separado (`shell-client`) que habla con `windowd` por el protocolo
   WM↔cliente + la extensión `ROLE_SHELL`. El wallpaper pasa a ser el
   background surface del shell. `windowd` = lo que queda. Verificar smoke +
   QMP mouse-driving (tray / menú contextual / wallpapers).
3. **A3 — Rename/estructura.** `desktop.c` → `windowd.c` (o subsistema propio),
   documentar el protocolo WM↔cliente como header público explícito.

## A2 — diseño detallado (el nudo del z-order)

**Restricción del protocolo actual:** cada cliente recibe UNA superficie (fd 3,
tamaño fijo al launch, `gfx_open_client` en `gfx_impl.inc`), colocada por el WM
en `window_x/y/width/height`. Input por fds 4/5, launch por fd 9, cursor hint
por fd 10.

**El nudo:** el chrome del shell vive en dos niveles de z-order incompatibles
con una sola superficie:
- **wallpaper + iconos del escritorio** → *debajo* de todas las ventanas.
- **taskbar + start menu + menú contextual + confirm** → *encima* de todas las
  ventanas (hoy son capas `TASKBAR`/`MENU`/… por sobre `CLIENT` en
  `build_layers`).

Un cliente de una sola superficie no puede ser fondo y frente a la vez.

**Acoplamiento extra:** `draw_taskbar` lee la lista de ventanas del WM
(`desktop_taskbar_button_client` → `overlay_clients`, active/minimized). En A2
el WM tendría que enviarle esa lista al shell (protocolo WM→shell: window-list
updates) para que dibuje los botones.

**Ángulo estratégico:** el chrome Win95 (taskbar + start menu) es temporal —
Fase B lo reemplaza por Program Manager (NT 3.5, sin taskbar). Levantarlo
fielmente con protocolo multi-superficie es trabajo que Fase B tira.

### Opciones de modelo de superficie

- **A) Multi-superficie fiel al Win95.** El shell crea 2 superficies con z-rol:
  background (full-screen, fondo) + overlay (full-screen, tope, alpha). Extensión
  de protocolo acotada (un cliente `ROLE_SHELL` con dos surfaces a z-roles
  fijos) + window-list updates para el taskbar. Fiel al chrome actual; parte del
  trabajo lo tira Fase B.
**Decidido: opción B.** Progreso: **A2.1 HECHO** (shellui dibuja el wallpaper
como cliente) y **A2.2 HECHO** (windowd lanza shellui al boot como
`background_client` y compone su superficie como capa de fondo; fallback a
wallpaper dibujado por windowd si el cliente no está listo; verificado
end-to-end en el soak de `desktop-smoke`). Siguen A2.3 (iconos + input al
shell), A2.4 (retirar chrome Win95, launcher proto-Progman), A2.5 (limpieza).

- **B) Fondo + arrancar el pivot NT 3.5 (recomendada).** El shell posee solo la
  superficie de **background** (wallpaper + iconos, fondo). El launcher deja de
  ser taskbar/start-menu y pasa a ser una **ventana normal** (proto-Progman),
  que es un top-level como cualquier app — sin z-rol especial, sin
  multi-superficie. Adelanta una pieza mínima de Fase B, evita el protocolo
  multi-superficie, y mueve hacia el objetivo. Pendiente menor: el menú
  contextual del escritorio (flotante sobre ventanas) se difiere o se maneja como
  top-level transitorio.
- **C) Solo diseño de protocolo.** Escribir el spec completo del protocolo
  WM↔shell (ROLE_SHELL, surfaces/z-roles, window-list, ruteo de input) y decidir
  el modelo antes de tocar código.

## Fases siguientes (fuera de Fase A)

- **Fase B** — `progman`: shell-client alternativo estilo Program Manager
  (grupos MDI, menubar File/Options/Window/Help). Trivial una vez que
  `windowd` no sabe quién es el shell. Requiere una primitiva MDI nueva en
  sxgui (child window con title bar, drag acotado al client area, minimize a
  icono) — hoy no existe.
- **Fase C** — maduración del WM contra el servidor limpio: resize por bordes,
  foco/activación entre ventanas, Alt-Tab, corrección de repintado.
