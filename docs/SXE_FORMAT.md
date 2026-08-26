# Formato SXE — ejecutables con recursos propios

> **Estado: fases 1 a 4 COMPLETAS, en master.** El formato canónico vive en
> [include/sxe/sxe_format.h](../include/sxe/sxe_format.h), el lector del SDK en
> [savanxp/sxe.h](../subsystems/posix/sdk/v1/include/savanxp/sxe.h) +
> [runtime/sxe.c](../subsystems/posix/sdk/v1/runtime/sxe.c), el estampado en
> [gen_sxe_resources.py](../tools/gen_sxe_resources.py) + `Add-SxeResources`, y
> los consumidores son `progman_registry_apply_sxe()` (launcher) y
> `windowd_presentation_load()` (chrome de ventana y Task List). Validado por
> `sxe-smoke`, `progman-smoke` y `windowd-smoke`. Sigue la **fase 5**: las
> asociaciones mime.
>
> Pendientes arrastrados, anotados en su lugar: **Doom sin estampar** (se
> construye aparte) y **el angostado de `desktop_icons`**, que espera a Doom.
>
> Donde este documento y `sxe_format.h` no coincidan, **gana el header**.
>
> **Decisión-crux:** SXE **no es un contenedor**. Es una convención sobre ELF:
> el archivo sigue empezando en `7F 45 4C 46`, lo carga el mismo
> [kernel/elf.cpp](../kernel/elf.cpp) sin tocar una línea, y `llvm-readelf` lo
> abre. Lo único que agrega son dos secciones **no-alloc** que el kernel jamás
> mapea. "SXE" nombra la convención y la extensión, no un formato binario nuevo.

## Motivación

Hoy un ejecutable no puede hablar de sí mismo, y por eso existen tres tablas
que compensan esa falta:

1. [progman_registry.h:33](../subsystems/posix/userland/progman_registry.h:33) —
   los iconos se referencian **por nombre** contra un set horneado en build. El
   propio comentario dice: *"traer iconos propios por programa necesitaría un
   formato+loader de iconos: trabajo aparte"*. Este documento es ese trabajo.
2. [windowd_appinfo.h:16](../subsystems/posix/userland/windowd_appinfo.h:16) —
   el WM **adivina** título/icono/accent por path, y admite que *"el arreglo de
   fondo es que cada cliente informe su propio título/icono"*.
3. `desktop_icons.h` — iconos de aplicación horneados dentro del binario del WM:
   agregar un programa al menú exige **recompilar el sistema**.

Con recursos en el propio ejecutable, las tres tablas se caen y aparece la
capacidad que hoy no existe: copiar un `.sxe` a `/disk/bin` **es** instalar un
programa.

## Por qué secciones ELF y no un contenedor propio

- **No se duplica el loader del kernel.** `kernel/elf.cpp` es chico y está
  probado. Un formato nuevo significa un segundo parser en el camino más
  crítico del sistema, o un des-envolvedor: bugs y superficie de ataque por
  duplicado a cambio de nada.
- **Los recursos no pagan RAM.** Al no ser `SHF_ALLOC`, no entran en ningún
  `PT_LOAD`: el kernel no los ve, no los mapea, y el proceso no paga por ellos.
  Con el presupuesto de memoria del sistema (~133 MiB usables de 256, arena en
  BSS mapeada eager) meter decenas de KiB de iconos en un segmento cargable
  **por proceso** sería un error caro.
- **Sobrevive el tooling.** `readelf`, `objdump`, `nm`, gdb siguen andando.
- **El estampado ya es posible con lo horneado**: `toolchain/llvm/bin/llvm-objcopy.exe`
  está en el toolchain. Cero herramientas nuevas.

Se consideró y descartó usar `SHT_NOTE` estándar (name/desc/type): el TLV
propio es más simple de parsear sin malloc y no gana nada compartiendo el
mecanismo de notas.

## Anatomía del archivo

```
notepad.sxe
├─ ELF header                        ← intacto; EI_OSABI ya lleva el subsistema
├─ Program headers
├─ .text / .rodata / .data / .bss    → PT_LOAD: esto se mapea
├─ .sxmeta   (NO alloc, ≤ 4 KiB)     → identidad: nombre, versión, mimes…
└─ .sxicon   (NO alloc, ≤ 64 KiB)    → píxeles de los iconos
```

**Invariante que hay que verificar en build:** ni `.sxmeta` ni `.sxicon` deben
tener la flag `A` en `llvm-readelf -S`. Si la tienen, se mapean, y todo el
argumento de memoria se cae en silencio.

**Dos secciones y no una** porque tienen patrones de lectura distintos:
`.sxmeta` es de cientos de bytes y se lee entero; `.sxicon` es de KiB y se lee
solo cuando hay que pintar. Un listado de archivos que solo necesita nombres no
debería arrastrar píxeles.

### Convenciones comunes

- Little-endian, x86-64. Sin padding implícito: todos los campos alineados
  naturalmente.
- Strings en UTF-8. **El `length` manda**: no se garantiza terminador NUL, y el
  lector debe tolerar que venga uno.
- Sin malloc en userland: los lectores copian a buffers fijos y **truncan**.

## Sección `.sxmeta`

### Header (16 bytes)

```c
struct sxe_meta_header {
    uint8_t  magic[4];      /* 'S','X','M','E' */
    uint16_t version;       /* 1 */
    uint16_t header_bytes;  /* sizeof(header); permite crecer sin romper */
    uint32_t blob_bytes;    /* total del blob, header incluido */
    uint32_t record_count;
};
```

`header_bytes` existe para que una v2 pueda agregar campos al header: un lector
v1 saltea `header_bytes` en vez de asumir `sizeof`.

### Registro TLV

```c
struct sxe_record {
    uint16_t tag;       /* ver tabla */
    uint16_t flags;     /* SXE_RECORD_* */
    uint32_t length;    /* bytes de payload, sin contar el padding */
    /* uint8_t payload[length]; */
    /* padding con ceros hasta múltiplo de 4 */
};
```

| flag | valor | significado |
|---|---|---|
| `SXE_RECORD_REQUIRED` | `0x0001` | Si el lector no conoce el `tag`, **debe descartar el blob entero** y caer a los defaults |

La regla general es "tag desconocido se ignora" — así se agregan campos sin
romper binarios viejos. `REQUIRED` es la válvula de escape para el día que se
agregue algo que cambie el significado de lo demás. Ningún tag de v1 lo usa;
está definido ahora porque después ya es tarde.

### Espacio de tags

| rango | uso |
|---|---|
| `0x0001`–`0x00FF` | Identidad |
| `0x0100`–`0x01FF` | Presentación |
| `0x0200`–`0x02FF` | Ejecución |
| `0x0300`–`0x03FF` | Capacidades |
| `0x0400`–`0x7FFF` | Reservado para el sistema |
| `0x8000`–`0xFFFF` | Privado / experimental — el sistema nunca los define |

### Tags de v1

| tag | nombre | payload | notas |
|---|---|---|---|
| `0x0001` | `NAME` | utf8 | Nombre para mostrar. Sin él, el programa no tiene identidad: el lector cae a basename. Recomendado ≤ 31 bytes (`PROGMAN_NAME_CAPACITY`) |
| `0x0002` | `DESCRIPTION` | utf8 | Recomendado ≤ 63 bytes (`PROGMAN_DESC_CAPACITY`) |
| `0x0003` | `VERSION` | `uint16[4]` | major, minor, patch, build. Binario y **comparable** |
| `0x0004` | `VERSION_STRING` | utf8 | Lo que se muestra ("1.2.0-rc3"). Separado de `VERSION` por el mismo motivo que Windows separa `FILEVERSION` de `StringFileInfo`: ordenar y mostrar son cosas distintas |
| `0x0005` | `VENDOR` | utf8 | |
| `0x0006` | `COPYRIGHT` | utf8 | |
| `0x0007` | `BUILD_ID` | utf8 | Commit o id de build; opcional, lo estampa el build |
| `0x0101` | `ACCENT` | `uint32` | `0x00RRGGBB`, el formato que ya devuelve `gfx_rgb`. Reemplaza el campo `accent` de `windowd_appinfo` |
| `0x0102` | `LAUNCH_FLAGS` | `uint32` | `SAVANXP_DESKTOP_LAUNCH_FLAG_*` **por defecto**. Quien lanza puede sobrescribirlos |
| `0x0201` | `INTERPRETER` | utf8 | Path absoluto del programa que ejecuta esta imagen. **Ausente o vacío = lo ejecuta el kernel directamente** |
| `0x0202` | `SUBSYSTEM` | `uint8` | Espejo **informativo** de `EI_OSABI`. La autoridad sigue siendo el byte del ELF ([elf.hpp:14](../include/kernel/elf.hpp:14)); esto existe para que un lector de userland no tenga que parsear el ELF header |
| `0x0301` | `MIME_OPEN` | utf8, entradas separadas por NUL | Tipos que el programa **declara poder** abrir |
| `0x0302` | `EXT_OPEN` | utf8, entradas separadas por NUL | Extensiones, con punto: `.txt` |

**`INTERPRETER` entra desde v1 aunque todavía no haya VM.** Es el campo que
resuelve el caso que se viene: cuando una app Haxe sea bytecode HashLink y no
un ELF x86-64, el lanzador tiene que saber que la imagen no la ejecuta el
kernel sino `/bin/hlvm`. Es el equivalente a shebang / `binfmt_misc`, y no
entra en el byte de `EI_OSABI`.

**`MIME_OPEN` declara capacidad, no asociación.** Que un programa diga que
puede abrir `text/plain` no lo convierte en el que abre los `.txt`: eso es
política del usuario y se resuelve en el registro (ver abajo).

### Tope de tamaño

`.sxmeta` no debe superar **4 KiB**. El lector usa un buffer fijo de ese
tamaño y **rechaza** el blob si `blob_bytes` lo excede — sin malloc no hay otra
opción honesta, y 4 KiB sobran para texto.

## Sección `.sxicon`

```c
struct sxe_icon_header {
    uint8_t  magic[4];      /* 'S','X','I','C' */
    uint16_t version;       /* 1 */
    uint16_t header_bytes;
    uint32_t blob_bytes;
    uint32_t image_count;
    /* struct sxe_icon_entry entries[image_count]; */
    /* píxeles */
};

struct sxe_icon_entry {
    uint16_t width;
    uint16_t height;
    uint32_t format;    /* SXE_ICON_FORMAT_* */
    uint32_t offset;    /* desde el inicio del blob */
    uint32_t length;    /* = width * height * 4 en BGRA8888 */
};
```

| formato | valor | descripción |
|---|---|---|
| `SXE_ICON_FORMAT_BGRA8888` | `2` | `uint32` por píxel, `0xAARRGGBB` (en memoria: B,G,R,A), filas de arriba hacia abajo, sin padding de fila |

El **valor 2 no es arbitrario**: coincide a propósito con `SX_PIXEL_FORMAT_BGRA8888`
de [gfx2d.h:18](../subsystems/posix/sdk/v1/include/savanxp/gfx2d.h:18), para que
los píxeles de un `.sxicon` se le pasen a un `struct sx_bitmap` sin traducir
nada. `sxe.c` tiene un `_Static_assert` que rompe el build si alguien renumera
los formatos de gfx2d — el modo de falla alternativo serían colores dados
vuelta en tiempo de ejecución.

Ese es **exactamente** lo que ya produce
[gen_desktop_icon_assets.py](../tools/gen_desktop_icon_assets.py)
(`(a << 24) | (r << 16) | (g << 8) | b`) y lo que consume
`struct desktop_embedded_bitmap`. Los píxeles del blob se le pasan al blitter
existente **sin conversión**.

**Tamaños:** 16×16 y 32×32 son los que el sistema usa hoy (`desktop_icon_small`
/ `desktop_icon_large`) y todo `.sxe` debería traer los dos. Otros tamaños
(48×48) son válidos y opcionales. Regla de selección del lector: exacto, si no
el menor que sea ≥ al pedido, si no el más grande disponible.

**Tope:** 64 KiB. Con 16+32+48 en BGRA8888 se usan ~16 KiB, así que hay
margen de sobra.

## Retrocompatibilidad

Al estilo del EXE de Windows: **un ejecutable sin recursos es un ejecutable de
primera clase, para siempre.**

- Un ELF sin `.sxmeta` se lanza igual. Nunca va a haber un `/disk/bin`
  "solo SXE". El día que un `.elf` pelado no arranque, se perdió la capacidad
  de debuggear a mano.
- **El kernel no participa de nada de esto.** No lee, no valida, no le importa.
  Todo el mecanismo vive en userland.
- Un `.sxmeta` corrupto, truncado, de versión futura, o con un `REQUIRED`
  desconocido se trata **como ausente**. Jamás impide lanzar.
- Defaults cuando no hay metadata:

  | dato | fallback |
  |---|---|
  | nombre | basename del path |
  | icono | genérico del set del sistema |
  | accent | `gfx_rgb(59, 95, 156)` — el que ya usa [windowd_render.c:356](../subsystems/posix/userland/windowd_render.c:356) |
  | flags | `SAVANXP_DESKTOP_LAUNCH_FLAG_NONE` |

### La extensión es una pista, no la autoridad

`.sxe` señala "acá probablemente hay recursos"; `.elf` señala "no busques". Eso
le ahorra al lanzador abrir N archivos por escaneo, que es I/O real sobre
SVFS2.

Pero **el blob es la fuente de verdad**. Un `.sxe` puede no tener `.sxmeta`
válido (build viejo, archivo truncado, alguien renombró) y un `.elf` puede
tenerlo. Si el lector trata la extensión como garantía, ese caso lo rompe. Como
pista, el fallback es el mismo de arriba: silencioso y ya escrito.

## Sin caché, por ahora

progman abre y parsea los `.sxe` en cada arranque. No hay índice persistente.

Con el atajo de la extensión y ~20 binarios el costo probablemente ni se note,
y una caché invalidada por mtime es exactamente la clase de cosa que agrega
bugs de invalidación difíciles de ver. **Primero medir.** Si el escaneo aparece
en el tiempo de arranque de progman, ahí se diseña el índice.

## Quién lee qué

```
  .sxe en disco
      │
      │ lector compartido del SDK (savanxp/sxe.h) — sin malloc, buffers fijos
      ▼
  progman ──► pinta el launcher (nombre, icono, descripción)
      │
      │ fd 9 (SAVANXP_WM_FD_LAUNCH): path + flags, tal cual hoy
      ▼
  windowd ──► relee el .sxe del path al crear la ventana
              título, icono de ventana, accent, Task List
```

El WM deja de adivinar por path y pasa a leer; `windowd_appinfo.c` queda como
fallback para huérfanos, o desaparece.

Esto **no contradice** la decisión que el WM ya tomó en
[wm_protocol.h:64](../subsystems/posix/sdk/v1/include/savanxp/wm_protocol.h:64)
(*"el que pide declara los flags de lanzamiento: el WM no conoce ningún catálogo
de apps"*). Lo que se prohibió ahí fue que el WM tenga un **catálogo**: una
tabla de apps conocidas que hay que mantener a mano. Leer la autodescripción
del binario que le acaban de pedir lanzar es exactamente lo contrario de un
catálogo — no hay tabla, no hay conocimiento previo, y un programa que el WM
nunca vio se presenta solo.

`desktop_icons.h` **no muere: se angosta.** Carpetas, archivo genérico, iconos
de diálogos y chrome del WM no pertenecen a ninguna app y siguen horneados. Lo
que se va del set son los iconos *de aplicación*, que es lo que nunca debió
estar ahí.

> **Angostarlo todavía no, y a propósito.** Al cerrar la fase 4 los iconos de
> aplicación del set siguen siendo la red de seguridad de tres cosas que aún
> dependen de ellos: Doom (que se construye aparte y no está estampado), el
> `icon=` de `progman.ini` —que referencia el set por nombre— y la tabla
> fallback de `windowd_appinfo`. Sacarlos ahora cambiaría un fallback que
> funciona por unos pocos KiB de imagen. El momento correcto es después de
> estampar Doom y de rediseñar el `icon=` del `.ini`.

### Decisión: el WM lee los recursos, no viajan por el protocolo

**El WM abre el `.sxe` del path que lanzó y lee `.sxmeta` + `.sxicon` una vez,
al crear la ventana.** No se agranda `savanxp_desktop_launch_request`.

La alternativa era mandar la presentación inline en el request. Se descartó:

- `savanxp_desktop_launch_request` hoy son 388 bytes (flags + path + argument).
  Un icono de 16×16 en BGRA8888 son 1 KiB más — el mensaje se agranda ~4× y
  entra en territorio de **lecturas parciales de pipe**, una clase de bug que ya
  mordió antes en este sistema y que no vale la pena reabrir por un icono.
- Habría **dos fuentes** para el mismo dato (lo que progman parseó y lo que dice
  el binario), y por lo tanto una forma de que queden desincronizadas.

Leyendo el WM, el costo está **acotado por la cantidad de ventanas abiertas**
—un puñado— y no por el tamaño de un directorio, que era la única razón de
peso para no hacer I/O acá. Y como el WM ya tiene el path y ya va a abrir el
archivo, leer los ~4 KiB de `.sxmeta` en la misma pasada sale prácticamente
gratis: mismo `open`, mismo inodo. Por eso lee **las dos** secciones y no solo
el icono.

Consecuencias:

- **Cero cambios de protocolo.** `savanxp_desktop_launch_request` queda tal
  cual está.
- Se lee **una vez** por ventana y se guarda en la estructura de sesión del
  cliente; el resto de la vida de la ventana no toca disco.
- Si el archivo no abre, no tiene `.sxmeta`, o el blob es inválido: fallback a
  los defaults de la tabla de arriba. **Nunca bloquea la creación de la
  ventana.**

La contra honesta que se está aceptando es I/O de disco en el loop del WM, que
es sensible a latencia. Se paga en el momento de crear una ventana, que ya es
el más caro del ciclo, así que es el lugar correcto. Si alguna vez se nota, la
salida es pintar el icono genérico en el primer frame y completar después —
**no** mover los píxeles al protocolo.

## División de responsabilidades con `progman.ini`

El registro no desaparece: **cambia de rol**, y queda el modelo de Windows.

| | `.sxmeta` | `progman.ini` |
|---|---|---|
| qué es | identidad del programa | arreglo del usuario |
| qué guarda | nombre, versión, icono, accent, flags default, mimes que declara | qué grupos hay, qué entra, en qué orden, overrides puntuales |
| quién lo pone | el que compila | el que usa |

Deja de ser un catálogo con iconos hardcodeados y pasa a ser lo que un menú
inicio realmente es. Y resuelve el mime: el binario **declara capacidad**, el
registro **resuelve la asociación**.

### Precedencia (implementada en la fase 3)

De mayor a menor, campo por campo:

1. **La clave escrita en el `.ini`** — lo que el usuario decidió
2. **El `.sxmeta`/`.sxicon` del binario** — lo que el programa declara de sí
3. **El default horneado** — red de seguridad para lo que no trae recursos
4. **Genérico** — basename e icono de escritorio

El escalón 1 necesita distinguir *"el usuario eligió este nombre"* de *"quedó
el valor por defecto"*, y eso no se puede deducir del valor: por eso
`struct progman_item` lleva una máscara `overrides` que el parser marca por
cada clave presente. Sin ella, el `.sxe` pisaría decisiones del usuario o al
revés, según cómo se ordenaran los pasos.

`progman_registry_apply_sxe()` corre **después** del pruning: no tiene sentido
abrir el binario de un item que se va a descartar, y el pruning reordena los
items —lo que invalidaría los slots de icono ya asignados—.

Los iconos leídos se copian a un pool fijo de `PROGMAN_MAX_ITEMS` slots de
32×32 (**192 KiB de BSS**). Es una elección consciente: sin malloc hay que
reservar el peor caso, y la alternativa —releer el `.sxicon` al pintar— pondría
I/O de disco dentro del ciclo de repintado.

## Integración con el build

### El manifiesto `.sxres`

Cada programa declara sus recursos en un `<nombre>.sxres` **al lado de su
fuente**. La convención es todo lo que hay que saber: si el archivo existe se
estampa, si no existe el binario sale exactamente como antes. Nada que
registrar en `build.ps1`.

```ini
# subsystems/posix/userland/notepad.sxres
name=Notepad
description=Edit text files
version=system
vendor=SavanXP
accent=78643c
icon=app-notepad
mime_open=text/plain
ext_open=.txt,.ini,.cfg,.md
```

| clave | valor |
|---|---|
| `name`, `description`, `vendor`, `copyright`, `build_id`, `version_string`, `interpreter` | texto, tal cual |
| `version` | `1.2.3`, hasta 4 componentes — o `system`, que lo resuelve contra `include/shared/version.h` |
| `accent` | `RRGGBB` en hexa (acepta `#` o `0x` adelante) |
| `launch_flags` | lista por comas; los nombres salen de `SAVANXP_DESKTOP_LAUNCH_FLAG_*` |
| `subsystem` | `posix` o `native` |
| `icon` | nombre de asset bajo `assets/desktop/icons/{16x16,32x32}/<icon>.png` — igual que `progman.ini` lo referencia hoy |
| `mime_open`, `ext_open` | listas por comas |

**`version=system` existe para que los programas del sistema no queden stale.**
Hardcodear `0.3.3` en nueve manifiestos sería la misma duplicación que este
diseño evita en todo lo demás.

### El generador

[tools/gen_sxe_resources.py](../tools/gen_sxe_resources.py) convierte los
manifiestos en blobs. **No duplica ni un número**: magics, tags, versiones,
tamaños y topes salen de `include/sxe/sxe_format.h`; los flags de lanzamiento
de `savanxp/syscall.h`; el OSABI nativo de `savanxp_native.h`. Es el criterio
de `Assert-Svfs2FormatMatchesHeader` — un formato copiado a mano entre lector y
generador se desincroniza, y la falla es silenciosa.

Al revés que el parser de runtime, que ignora lo que no entiende para poder
leer binarios más nuevos, **el generador es estricto**: una clave desconocida,
un icono faltante o un flag inexistente rompen el build. Un typo en un
manifiesto tiene que fallar, no dejar la app sin icono en silencio.

### El estampado

```bash
llvm-objcopy --add-section .sxmeta=app.sxmeta --add-section .sxicon=app.sxicon app app
```

`--add-section` crea secciones sin `SHF_ALLOC`, que es lo que se quiere — pero
se **verifica** igual, leyendo el valor crudo de `sh_flags` con
`llvm-readelf --section-details` y chequeando el bit `0x2`. Si alguna vez
aparece, el costo se paga en RAM por proceso y **sin ningún síntoma visible**:
esa es exactamente la clase de regresión que necesita un guard automático.

`Add-SxeResources` e `Invoke-SxeResourceGenerator` viven en
`tools/UserAppCommon.ps1`, no en `build.ps1`, para que los dos caminos de build
—el in-tree y el de apps externas (`build-user.ps1`)— estampen con la misma
implementación. Duplicar el paso significaría que la verificación de no-alloc
se aplica en uno y no en el otro.

> **Gotcha de PowerShell:** todo lo que un comando escribe sin capturar se suma
> al valor de retorno de la función que lo contiene. Un `sxe: N manifiestos`
> suelto convirtió la ruta que devuelve `Build-ExternalUserProgram` en un array
> de dos elementos, y el build se rompió lejos de ahí (busybox copiando a un
> "drive" llamado `sxe`). Por eso las invocaciones de python y objcopy capturan
> su salida y la reemiten con `Write-Host`.

## Fases sugeridas

1. ~~**Formato + lector.**~~ **HECHA.** El formato canónico en
   `include/sxe/sxe_format.h` (freestanding, C/C++, con static asserts de
   layout), el lector en `savanxp/sxe.h` + `runtime/sxe.c`, y el harness
   `build.ps1 sxe-smoke` (`/disk/bin/sxetest`). El parseo puro se ejercita
   contra blobs fabricados en el stack — bien formados y todos los degradados
   que ningún generador correcto produciría — y el camino de disco contra los
   binarios reales de la imagen.
2. ~~**Estampado en build.**~~ **HECHA.** Manifiestos `.sxres` por app,
   generador host-side header-driven, estampado con `llvm-objcopy` y guard de
   no-alloc por bit de `sh_flags` — compartido entre el build in-tree y el de
   apps externas. Nueve programas del sistema estampados; todavía nadie los
   lee, así que lo único que cambia es que los binarios engordan ~5 KiB.
3. ~~**progman consume.**~~ **HECHA.** `progman_registry_apply_sxe()` resuelve
   nombre, descripción, launch flags e icono desde el binario de cada item,
   respetando lo que el `.ini` haya declarado explícitamente. En la imagen
   actual toma recursos de 8 de 9 items — el noveno es Doom, que se construye
   aparte y no está estampado, así que cae al icono horneado. **Nada cambia
   visualmente**, y eso es lo correcto: los manifiestos reproducen la
   presentación que antes vivía en las tablas. Lo que cambió es de dónde
   salen los datos.
4. ~~**windowd consume.**~~ **HECHA.** `windowd_presentation_load()` resuelve
   título, icono de 16×16 y accent leyendo el `.sxe` del binario que acaba de
   lanzar, una sola vez, en `start_client_process()`. **Cero cambios de
   protocolo**, como estaba decidido. `windowd_appinfo` quedó como escalón de
   fallback y `desktop_icons` sigue entero — ver arriba por qué angostarlo
   todavía no.
5. **Asociaciones.** `MIME_OPEN`/`EXT_OPEN` + resolución en el registro +
   filesapp abriendo archivos con el programa asociado, vía el campo `argument`
   del launch request que ya existe.

`INTERPRETER` no tiene fase: el campo está desde v1 y se empieza a honrar el
día que exista la VM.
