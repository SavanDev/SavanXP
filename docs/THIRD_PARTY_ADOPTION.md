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

## OpenBSD

Casi todo el arbol de OpenBSD es ISC o BSD-2, compatible con MIT: el unico
requisito es conservar los avisos originales. Es la fuente externa de menor
friccion legal para el repo, asi que el default aca es mas permisivo que en
SerenityOS: piezas chicas y autocontenidas pueden ir directo a `Port
selectivo`.

`Port selectivo`:

- funciones de string seguras de `lib/libc` (strlcpy, strlcat, strtonum,
  reallocarray, recallocarray, freezero, explicit_bzero): quedan usos de
  strcpy, strcat y sprintf en kernel y userland que estas reemplazan sin
  cambiar la forma del codigo
- `sys/kern/subr_prf.c`: printf sin dependencia de FILE, con soporte de width
  y padding. Unifica los dos printf de userland (libc.c y posix.c) y cubre el
  formato que hoy falta
- `arc4random` (ChaCha20, `lib/libcrypto/arc4random`): no hay RNG en el repo;
  lo necesitan los puertos efimeros y los ISN de TCP en kernel/net.cpp, que
  hoy son predecibles
- `sys/dev/pci/ac97.c`: solo la secuencia de warm reset con timeouts de
  codec-ready y la tabla de mixer, que es donde las VMs difieren
- `pcidevs` mas `devlist2h.awk`: tabla de IDs PCI generada desde texto, para
  nombrar devices en pci.cpp y en las vistas de sysinfo y netinfo
- `signify`: firma Ed25519 en ~1000 lineas ISC, para binarios SXE como seccion
  no-alloc adicional

`Referencia`:

- sndio y la API audio(4): misma forma que el HAL de audio propio (dispatcher
  mas backends); lo que aporta es la politica de under-run y latencia
- `amd64/lapic.c`, `ioapic.c`, `acpimadt.c`: xAPIC por MMIO, x2APIC, overrides
  de MADT y virtual wire mode, en codigo corto y legible
- wscons, wsdisplay, wskbd y wsmouse: separacion entre device, emulacion de
  terminal e input; incluye decodificacion PS/2 con reenvios 0xFA y 0xFE
- `malloc.c` de userland: guard pages, canarios, junk fill, unmap al liberar y
  flags de depuracion en runtime
- modelo de pledge y unveil: capacidades declaradas por proceso, con los
  subsistemas y la metadata SXE como punto de corte natural. La
  implementacion no se porta: esta acoplada a su tabla de syscalls
- `src/regress`: organizacion de los tests de regresion
- `tcp_input.c`: solo lectura, para timers de retransmision, ventana
  deslizante y estados de cierre. Acoplado a mbuf, no portable

`No adoptar`:

- UVM, FFS y UFS, pf, xenocara y el DRM importado de Linux: acoplamiento alto
  y sin encaje con SVFS2 ni con el HAL de display propio
- ksh: ya existe shell_core.c
- OpenBSD no tiene driver virtio-gpu, asi que del camino grafico principal no
  hay nada que tomar

Cada pieza que pase a `Port selectivo` entra en el registro de procedencia con
su header ISC original intacto: son avisos de pocas lineas y perderlos es el
error facil de cometer.

## Trazabilidad: el registro es la fuente de verdad

La trazabilidad vive en `docs/THIRD_PARTY_PROVENANCE.md`, no en los mensajes de
commit. La regla anterior pedia que cada commit citara su categoria; se retira
porque apunta al lugar equivocado: cuando hay que auditar que se distribuye, lo
que se mira es el arbol actual, no el historial.

Invariante del registro:

- todo directorio de `vendor/` y `sdk/` con codigo de terceros tiene una
  entrada
- toda pieza de terceros que termine horneada en la ISO, el initramfs o la
  imagen de disco tiene una entrada, incluso cuando el repo no la versiona y
  se descarga o se provee aparte
- cada entrada declara origen, version o commit fijado, licencia verificada,
  decision y donde termina el bit distribuido

Se verifica listando `vendor/` y `sdk/` contra el registro. Es una revision
manual, y el momento de hacerla es el cambio que agrega o actualiza el
componente, no despues.

Reglas a nivel archivo:

- con `Port selectivo`, el archivo conserva su header original o un comentario
  equivalente con origen y licencia
- con `Referencia`, la inspiracion se documenta en el registro y no se repite
  en cada archivo
- el codigo propio que envuelve o adapta una pieza de terceros es obra derivada
  y hereda la licencia de esa pieza, no el MIT del repo: los shims tambien
  llevan header
