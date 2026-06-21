# Registro de procedencia de terceros

Este archivo registra componentes inspirados o reutilizados desde proyectos
externos.

## SerenityOS

### Compositor 2D y desktop

- Origen: `Userland/Services/WindowServer/Compositor.*`
- Licencia revisada: BSD-2-Clause
- Decision: `Referencia`
- Motivo: el valor principal esta en la arquitectura del compositor, las
  regiones sucias y la oclusion; SavanXP implementa su propia variante sobre su
  runtime y ABI actuales

### Toolkit grafico base

- Origen: `Userland/Libraries/LibGfx/Bitmap.*`, `Painter.*`
- Licencia revisada: BSD-2-Clause
- Decision: `Referencia`
- Motivo: SavanXP incorpora una capa `sxgfx` propia, compatible con su modelo
  de superficies compartidas y con un alcance mucho mas acotado

### Rects y regiones sucias

- Origen: ideas derivadas de `LibGfx` y `DisjointRectSet`
- Licencia revisada: BSD-2-Clause
- Decision: `Referencia`
- Motivo: se prefirio una implementacion C minima sin dependencias de `AK` ni
  de las estructuras de SerenityOS

### Display/GPU abstraction

- Origen: `Kernel/Devices/GPU/DisplayConnector.*`
- Licencia revisada: BSD-2-Clause
- Decision: `Referencia`
- Motivo: SavanXP mantiene `virtio_gpu` como backend actual y agrega una capa
  `display` propia encima, sin portar el stack completo de SerenityOS

### Assets y temas

- Origen: no adoptado
- Licencia revisada: pendiente por componente
- Decision: `No adoptar`
- Motivo: quedan fuera de esta fase hasta revisar procedencia y permisos

### Iconos del desktop

- Origen: arte propio de SavanXP generado por `tools/GenerateDesktopSourceArt.ps1`
- Licencia revisada: propio del repo
- Decision: fuera del registro de terceros desde su reemplazo
- Motivo: el desktop ya no depende de iconos copiados o derivados de SerenityOS

## Tipografias

SavanXP no embebe ni parsea TrueType en runtime: las fuentes se hornean offline a
tablas C (`.inc`) con `tools/font/genfont.py` y solo el bitmap/cobertura derivado
queda compilado en el binario. Los archivos fuente viven en `assets/desktop/fonts/`.

### GNU UniFont (consola y terminal)

- Origen: GNU Unifont 17.0.04, `unifont-17.0.04.hex`
  (https://unifoundry.com/unifont/)
- Fuente local: `assets/desktop/fonts/unifont.hex`
- Licencia revisada: SIL OFL-1.1 y, alternativamente, GNU GPLv2+ con la GNU Font
  Embedding Exception (doble licencia upstream)
- Decision: `Adoptar`
- Motivo: fuente bitmap nativa 8x16 con cobertura Unicode amplia, ideal para la
  consola del kernel y el render monospace del terminal. Se hornea desde el `.hex`
  canonico (rango ASCII + Latin-1 + dibujo de cajas/bloques) porque el outline TTF
  rasteriza fuera de grilla. Texto OFL en `assets/desktop/fonts/LICENSE-OFL-1.1.txt`.

### Noto Sans (escritorio / UI)

- Origen: Noto Sans Regular v1.06 (Copyright 2012 Google Inc.), proyecto Noto
  (https://fonts.google.com/noto)
- Fuente local: `assets/desktop/fonts/NotoSans-Regular.ttf`
- Licencia revisada: SIL OFL-1.1 (`assets/desktop/fonts/LICENSE-OFL-1.1.txt`)
- Decision: `Adoptar`
- Motivo: tipografia proporcional para el chrome del escritorio y los widgets; se
  hornea como atlas de cobertura 8-bit (antialiased) a 13px con `genfont.py`.
