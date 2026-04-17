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
