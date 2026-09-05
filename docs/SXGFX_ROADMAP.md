# SxGFX — endurecer la capa 2D como GDI32

> **Estado: propuesta. Nada de esto está implementado.** El documento es el
> plan: qué le falta a SxGFX medido contra el rol que le asigna
> [SYSTEM_LAYERING.md](SYSTEM_LAYERING.md) —**GDI32**, la capa de rasterización
> 2D debajo de SXGUI-C— y en qué orden conviene atacarlo.
>
> El código en cuestión es
> [savanxp/gfx2d.h](../subsystems/posix/sdk/v1/include/savanxp/gfx2d.h) (API
> pública), [runtime/gfx2d.c](../subsystems/posix/sdk/v1/runtime/gfx2d.c) (el
> painter y los conjuntos de rects) y
> [runtime/gfx_impl.inc](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc) (las
> primitivas crudas sobre el buffer y el texto).
>
> **La conclusión corta:** los tres primeros ítems son puramente aditivos, no
> tocan el kernel, y cada uno *borra* código de SXGUI-C en vez de agregarlo. Ese
> es el lote por el que conviene empezar.

## Por qué GDI32 y no DirectX

Vale dejarlo escrito porque la pregunta reaparece: SxGFX **no** es el lugar
donde poner un modelo tipo DirectX (dispositivo, recursos opacos, swapchain,
estado de pipeline). SXGUI-C está construido encima y lo que necesita es
`fill_rect`, no `CreateDevice`. Convertir SxGFX en un D3D le rompe el rol que
tiene asignado.

Curiosamente el kernel ya es más DirectX-like que SxGFX: `GPU_IOC_IMPORT_SECTION`
→ `surface_id` es creación de recursos, `PRESENT_SURFACE_BATCH` es un command
list y `savanxp_gpu_present_timeline` (submitted/retired + `WAIT_PRESENT`) es un
fence. Si algún día se quiere ese modelo, va en una capa nueva al lado —no
adentro— de SxGFX, hablando directo con `/dev/gpu0`. Este documento es sobre la
otra dirección: hacer que SxGFX sea un **buen GDI**.

## Lote 1 — lo que ya duele

Estos tres tienen evidencia directa en el código de SXGUI-C: el toolkit está
emulando a mano cosas que la capa de abajo debería darle.

### 1.1 El painter no expone primitivas que ya existen

`gfx_pixel`, `gfx_hline`, `gfx_vline` y `gfx_frame` están implementadas en
[gfx_impl.inc:755-833](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:755),
pero ningún `sx_painter_*` las envuelve. Como SXGUI-C necesita el clipping del
painter y el painter solo ofrece `fill_rect`, el toolkit termina pintando
**píxeles sueltos a través de la ruta de relleno de rectángulos**:

```c
sx_painter_fill_rect(painter, sx_rect_make(x, y, 1, 1), SXGUI_COLOR_TEXT);
```

Aparece en [sxgui.c:121-136](../subsystems/posix/sdk/v1/runtime/sxgui.c:121)
(el focus rect punteado), [:153](../subsystems/posix/sdk/v1/runtime/sxgui.c:153)
(el dither del checkbox) y
[:1133](../subsystems/posix/sdk/v1/runtime/sxgui.c:1133). Cada píxel paga
intersección de clip, clip contra el bitmap y una llamada. Y el toolkit define
sus propios `sxgui_hline`/`sxgui_vline` sobre `fill_rect` en
[sxgui.c:7-15](../subsystems/posix/sdk/v1/runtime/sxgui.c:7).

**Qué hacer:** `sx_painter_set_pixel`, `sx_painter_hline`, `sx_painter_vline`,
delegando a las primitivas crudas después de aplicar el clip. Es el arreglo más
barato del documento.

**Invariante a respetar:** las primitivas nuevas tienen que ser correctas *por
fragmento*. El comentario en
[gfx2d.c:288](../subsystems/posix/sdk/v1/runtime/gfx2d.c:288) documenta el bug
de residuos del cursor —`draw_frame` trazaba un borde alrededor de cada
sub-rect sucio— y esa lección aplica a todo lo que se agregue acá.

### 1.2 No hay objetos pen ni brush

El color viaja como `uint32_t` suelto en cada llamada. GDI tiene `HPEN` (ancho,
punteado, rayado) y `HBRUSH` (sólido, hatch, patrón). El focus rect punteado y
el dither del checkbox de SXGUI-C son, literalmente, brushes de patrón hechos a
mano píxel por píxel.

**Qué hacer:** un `sx_brush` con color sólido o patrón 8×8 de 1 bit, y un
`sx_pen` con ancho y estilo. El look Win9x sale de ahí en vez de reimplementarse
en cada widget.

### 1.3 No hay origen de coordenadas

GDI tiene `SetViewportOrgEx`. Acá todo es absoluto, así que cada widget calcula
coordenadas absolutas a mano.

**Qué hacer:** `sx_painter_push_origin(dx, dy)` / `pop_origin`, reusando el
mismo patrón de pila que ya tiene el clip (`SX_PAINTER_CLIP_STACK_DEPTH`). Es
prerrequisito de widgets anidados y de contenedores con scroll que no tengan
que hacer la aritmética a mano.

## Lote 2 — lo que cambia estructura

### 2.1 Clip por región, no por rectángulo

`sx_painter` tiene un `clip_rect` único más una pila de 16. Pero `sx_rect_set`
**ya implementa** conjuntos de rects con `sx_rect_set_subtract_rect`: la
maquinaria de regiones está escrita y no está conectada al clip del painter.

GDI tiene `HRGN` con combinación AND/OR/XOR/DIFF. Sin eso no hay ventanas no
rectangulares ni clip directo contra la región de daño.

Ojo con una simplificación existente: `sx_rect_set_add`
([gfx2d.c:511](../subsystems/posix/sdk/v1/runtime/gfx2d.c:511)) fusiona por
bounding box ante cualquier solape o adyacencia, así que dos rects en L se
vuelven el rectángulo que los contiene. Sobre-cubre. Una región por bandas
—como la de GDI— es exactamente el upgrade que resuelve esto y el clip a la vez.

### 2.2 Objeto fuente

Hay **dos** fuentes horneadas y la elección está clavada en el nombre de la
función que se llama: `gfx_blit_text` (Noto, proporcional, antialiased) contra
`gfx_blit_text_mono` + `gfx_cell_width`
([gfx_impl.inc:912](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:912),
UniFont, la consola). El painter solo expone la primera, vía
`sx_painter_draw_text`.

Peor: `gfx_noto_glyph(unsigned char c)`
([gfx_impl.inc:9](../subsystems/posix/sdk/v1/runtime/gfx_impl.inc:9)) indexa por
byte. Tope duro de 256 glifos, **sin Unicode**, sin tamaños, sin bold ni italic.
No hay equivalente de `SelectObject(hFont)`.

**Qué hacer, en orden:** (a) decodificar UTF-8 → codepoint en el camino de
texto; (b) un `sx_font` opaco que el painter seleccione, con las dos fuentes
actuales como las dos primeras instancias; (c) recién después, variantes y
tamaños.

## Lote 3 — huecos de GDI que faltan enteros

- **Raster ops.** No hay SRCCOPY/SRCINVERT/PATINVERT: el blend está clavado en
  SRC_OVER (`sx_blend_bgra8888_over_rgb`,
  [gfx2d.c:3](../subsystems/posix/sdk/v1/runtime/gfx2d.c:3)). XOR es lo que hace
  baratos los rubber-bands de arrastre y los focus rects, que hoy se emulan
  píxel a píxel.
- **Geometría.** No hay línea diagonal, círculo, elipse, polígono ni rectángulo
  redondeado. Bresenham más elipse por punto medio son unas 80 líneas.
- **Escalado con calidad.** `sx_painter_draw_scaled_bitmap_nearest` es la única
  opción y el filtro está en el nombre. GDI tiene
  `SetStretchBltMode(HALFTONE)`; bilineal al achicar iconos y wallpapers se nota
  a simple vista.
- **Memory DC.** Solo existe `sx_bitmap_wrap`. GDI tiene
  `CreateCompatibleDC` + `CreateCompatibleBitmap`, que es la receta canónica
  para pintar sin parpadeo; hoy cada app hace su propio malloc y arma el
  `savanxp_fb_info` a mano.
- **Paths.** `BeginPath`/`EndPath` y regiones derivadas de paths. Última
  prioridad: no es lo que está frenando nada.

## Endurecimiento de lo que ya hay

**`draw_scaled_bitmap_nearest` no valida `source_rect` contra el bitmap
origen.** El destino sí se clipea (`target_rect`), pero `source_x` y `source_y`
se derivan de un `source_rect` que provee el llamador y del que solo se comprueba
que no esté vacío
([gfx2d.c:378](../subsystems/posix/sdk/v1/runtime/gfx2d.c:378)). Un `source_rect`
que exceda las dimensiones del origen, o con `x`/`y` negativos, lee fuera del
buffer.

Hoy es **latente, no un bug activo**: los tres llamadores in-tree
(`desktop_wallpaper.c:356`, `progman.c:228`, `windowd_render.c:385`) pasan el
rect completo del origen. Pero es API pública del SDK y el clamp son cuatro
líneas.

## Lo que está bien y no hay que romper

- **Texto antialiased** por cobertura por píxel (`kNotoCoverage`). GDI32 tardó
  años en tener eso; ninguna refactorización debería perderlo.
- **Clipping correcto por fragmento**, con el razonamiento documentado en
  [gfx2d.c:288](../subsystems/posix/sdk/v1/runtime/gfx2d.c:288). Es una
  invariante ganada a pulso contra un bug real de repintado.
- **El desborde de `sx_rect_set` colapsa a un superset deliberado**
  ([gfx2d.c:558](../subsystems/posix/sdk/v1/runtime/gfx2d.c:558)): sobre-pinta,
  nunca sub-pinta, y está documentado. La capacidad fija de 64 y la pila de clip
  de 16 degradan de forma segura.
- **El camino rápido de `memcpy`** en `sx_painter_blit_bitmap` cuando origen y
  destino comparten ancho completo
  ([gfx2d.c:308](../subsystems/posix/sdk/v1/runtime/gfx2d.c:308)).

## Orden sugerido

1. **Lote 1 completo** (1.1 + 1.3 + 1.2, en ese orden). Aditivo, sin tocar el
   kernel, y borra código de SXGUI-C.
2. **El clamp de `source_rect`.** Cuatro líneas, cerrar mientras se está mirando
   el archivo.
3. **2.1 (regiones).** Absorbe el problema de sobre-cobertura de
   `sx_rect_set_add` y destraba ventanas no rectangulares.
4. **2.2 (fuentes).** El único con una dependencia externa real: el pipeline de
   horneado en `tools/font/genfont.py` tiene que emitir más que 256 glifos.
5. **Lote 3, por demanda.** Cada ítem cuando aparezca el consumidor que lo pide.

Verificación: todo esto cae bajo el preview headless del toolkit en el host
—`clang` del toolchain con stubs renderizando a PNG— y bajo `windowd-smoke`. Las
primitivas nuevas deberían llegar con una comparación de imagen antes de tocar
SXGUI-C.
