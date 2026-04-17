# Politica de adopcion de codigo y diseno de terceros

SavanXP puede estudiar, reutilizar o inspirarse en componentes de otros
proyectos cuando eso acelera el desarrollo sin comprometer la claridad legal ni
la mantenibilidad del repo.

## Categorias obligatorias

Cada adopcion debe quedar registrada en una de estas categorias:

- `Referencia`: se estudia el diseno o comportamiento y se implementa desde
  cero dentro de SavanXP.
- `Port selectivo`: se copia codigo pequeno y autocontenido, preservando sus
  avisos de copyright y licencia.
- `No adoptar`: el componente queda descartado por acoplamiento alto, licencia
  dudosa o costo de mantenimiento.

## Regla operativa por defecto

- compositor, toolkit grafico base, APIs de ventana y flujo desktop:
  `Referencia`
- helpers pequenos de geometria, rects, bitmap simple o utilidades similares:
  `Port selectivo` si la licencia es clara y la dependencia resultante sigue
  siendo minima
- assets, iconos, cursores, fuentes o contenido no estrictamente de codigo:
  `No adoptar` hasta revisar licencia y procedencia caso por caso
- excepcion acotada: assets de referencia temporales para prototipos visuales
  pueden incorporarse si quedan aislados bajo `assets/.../reference/...`,
  documentados en el registro de procedencia y con reemplazo previsto por arte
  propio del proyecto

## Requisitos para cualquier adopcion

- registrar el origen exacto del componente en `docs/THIRD_PARTY_PROVENANCE.md`
- anotar la licencia verificada
- justificar la decision tecnica
- conservar los avisos originales cuando haya `Port selectivo`
- no relicenciar como MIT puro una pieza copiada de un tercero con otra
  licencia permisiva

## SerenityOS

Para SerenityOS el enfoque inicial del repo es:

- `WindowServer`, `Compositor` y `LibGUI`: `Referencia`
- `LibGfx` base, primitivas 2D y estructuras de rects: `Referencia` por
  defecto, con `Port selectivo` solo para helpers pequenos y autocontenidos
- backends GPU no VirtIO, assets, iconos y fuentes: `No adoptar` en esta fase
- assets, iconos y arte visual del desktop actual: propios del repo y
  generados localmente, sin dependencia activa de assets de SerenityOS

## Trazabilidad minima en commits y codigo

- los commits que introduzcan una pieza inspirada en terceros deben mencionar la
  categoria adoptada
- cuando exista `Port selectivo`, el archivo debe conservar su header o un
  comentario equivalente con origen y licencia
- cuando exista `Referencia`, se prefiere documentar la inspiracion en el
  registro de procedencia en lugar de repetirla en cada archivo
