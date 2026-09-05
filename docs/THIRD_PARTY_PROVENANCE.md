# Registro de procedencia de terceros

Este archivo registra todo componente de terceros que termina dentro de un
binario o una imagen que SavanXP distribuye. Esa es la regla de entrada: una
entrada por bit distribuido, no una por proyecto que sirvio de inspiracion. El
razonamiento sobre que se adopta y bajo que categoria vive en
`docs/THIRD_PARTY_ADOPTION.md`.

Cada entrada declara origen, version fijada, licencia verificada, decision y
donde termina el bit distribuido. El campo `Versionado` dice si el repo
contiene la pieza o si se descarga o se provee aparte durante el build: en los
dos casos la imagen construida la redistribuye, y por eso las dos van al
registro.

## Codigo de terceros compilado en el sistema

### uACPI

- Origen: https://github.com/uACPI/uACPI, v6.0.0
- Version fijada: commit `9c9b26d6291a1cdd9014cc5bb6b03e596697cbfd`
  (`vendor/uacpi/UACPI_COMMIT.txt`)
- Versionado: si, `vendor/uacpi/`
- Licencia revisada: MIT (Copyright 2022-2026 Daniil Tatianin),
  `vendor/uacpi/LICENSE`
- Decision: `Adoptar`
- Distribuido en: el binario del kernel; se compila con flags propios
  (`Get-UacpiFlags`) y se integra por `kernel/uacpi_glue.cpp`

### BusyBox

- Origen: https://busybox.net, BusyBox 1.37.0
- Versionado: si, `vendor/busybox/` (arbol upstream completo)
- Licencia revisada: GPLv2, `vendor/busybox/LICENSE`
- Decision: `Adoptar`, con reemplazo previsto
- Distribuido en: `/bin/busybox` y una copia del binario por cada applet
  (`Install-BusyBox` en `build.ps1`)
- Alcance real: solo seis applets se compilan desde upstream (`cat`, `cp`,
  `echo`, `mkdir`, `mv`, `rm`), via los `#include` de una linea en
  `vendor/busybox-port/applet_*.c`. Los otros cinco (`ls`, `ps`, `true`,
  `false`, `sleep`) son codigo propio en `legacy_applets.c`
- Deuda conocida: `vendor/busybox-port/busybox_compat.c` y `libbb.h` son obra
  derivada de BusyBox y todavia no llevan header de licencia, con lo cual
  quedan cubiertos por el MIT del repo de forma incorrecta

### Doom (DoomGeneric)

- Origen: DoomGeneric, sobre la linea Chocolate Doom
- Versionado: si, `sdk/doomgeneric/`
- Licencia revisada: GPLv2. Copyright (C) 1993-1996 Id Software, Inc. y
  Copyright (C) 2005-2014 Simon Howard. Texto completo en
  `sdk/doomgeneric/COPYING`; los avisos por archivo quedan en los headers
- Decision: `Adoptar`
- Distribuido en: `/disk/bin/doomgeneric`. Se compila aparte con
  `sdk/doomgeneric/build.ps1`, no con el build principal

## Bootloader

### Limine

- Origen: https://github.com/limine-bootloader/limine, rama `v10.x-binary`
- Versionado: no. `build.ps1` lo clona a `tools/limine`, que esta en
  `.gitignore`
- Licencia revisada: BSD-2-Clause, `tools/limine/LICENSE`
- Decision: `Adoptar`
- Distribuido en: la ISO, como `BOOTX64.EFI`, `limine-bios-cd.bin`,
  `limine-bios.sys` y `limine-uefi-cd.bin`

### Header del protocolo Limine

- Origen: derivado de https://github.com/limine-bootloader/limine-protocol
- Versionado: si, `vendor/limine.h`
- Licencia revisada: 0BSD, declarada por SPDX en el propio archivo
- Decision: `Port selectivo`
- Distribuido en: el binario del kernel. Es una version minima escrita para el
  repo, no una copia del header upstream

## Contenido y assets en la imagen

### Freedoom

- Origen: https://freedoom.github.io
- Versionado: no. `.gitignore` excluye `sdk/doomgeneric/wad/*.wad`; el archivo
  se descarga aparte y el build imprime la URL si falta
- Licencia revisada: BSD-3-Clause. Proyecto independiente, sin relacion con id
  Software
- Decision: `Adoptar`
- Distribuido en: `/disk/games/doom/freedoom1.wad` de la imagen construida. Es
  el IWAD por defecto para que el LiveCD sea jugable sin contenido
  propietario ni shareware

### GNU UniFont (consola y terminal)

- Origen: GNU Unifont 17.0.04, `unifont-17.0.04.hex`
  (https://unifoundry.com/unifont/)
- Versionado: si, `assets/desktop/fonts/unifont.hex`
- Licencia revisada: SIL OFL-1.1 y, alternativamente, GNU GPLv2+ con la GNU
  Font Embedding Exception (doble licencia upstream). Texto en
  `assets/desktop/fonts/LICENSE-OFL-1.1.txt`
- Decision: `Adoptar`
- Distribuido en: el binario, como tabla C horneada offline por
  `tools/font/genfont.py`. SavanXP no embebe ni parsea TrueType en runtime: lo
  que queda compilado es el bitmap derivado
- Motivo: fuente bitmap nativa 8x16 con cobertura Unicode amplia. Se hornea
  desde el `.hex` canonico (ASCII, Latin-1, cajas y bloques) porque el outline
  TTF rasteriza fuera de grilla

### Noto Sans (escritorio y UI)

- Origen: Noto Sans Regular v1.06 (Copyright 2012 Google Inc.), proyecto Noto
  (https://fonts.google.com/noto)
- Versionado: si, `assets/desktop/fonts/NotoSans-Regular.ttf`
- Licencia revisada: SIL OFL-1.1,
  `assets/desktop/fonts/LICENSE-OFL-1.1.txt`
- Decision: `Adoptar`
- Distribuido en: el binario, como atlas de cobertura 8-bit antialiased a 13px
  horneado por `tools/font/genfont.py`
- Motivo: tipografia proporcional para el chrome del escritorio y los widgets

### Iconos y arte del desktop

- Origen: arte propio del repo, generado por
  `tools/gen_desktop_source_art.py`
- Decision: fuera del registro de terceros desde su reemplazo
- Motivo: el desktop ya no depende de iconos copiados ni derivados de otros
  proyectos
- Alcance: `assets/desktop/icons/16x16` y `32x32`. **No** cubre
  `assets/desktop/icons/mime/`, que es el catalogo de tipos y tiene la
  procedencia de abajo

### Tango Icon Theme (catalogo de tipos de archivo)

- Origen: Tango Icon Theme 0.8.90, `tango-icon-theme_0.8.90.orig.tar.gz`
  (proyecto Tango Desktop, http://tango.freedesktop.org/), `sha256`
  `6e98d8032d57d818acc907ec47e6a718851ff251ae7c29aafb868743eb65c88e`
- Versionado: si, 22 PNG (11 iconos x 16x16 y 32x32) en
  `assets/desktop/icons/mime/`
- Licencia revisada: **dominio publico**. El `COPYING` del tarball dice
  textual *"The icons in this repository are herefore released into the Public
  Domain"*. Las versiones anteriores a 0.8.90 eran CC BY-SA 2.5; el cambio a
  dominio publico se hizo justamente para sacar la incompatibilidad con la GPL,
  asi que la version importa: **0.8.90 o posterior**
- Decision: `Adoptar`
- Distribuido en: la imagen, como blobs `.sxicon` sueltos en `/disk/icons`
  emitidos por `tools/gen_mime_icons.py`. **No** se hornea en ningun binario,
  por la misma razon por la que `.sxicon` es una seccion no-alloc (ver
  `docs/SXE_FORMAT.md`)
- Motivo: los iconos por tipo de archivo se dibujan sobre el campo BLANCO de
  una lista, no sobre el face gris del escritorio. Medido sobre `C0C0C0` el set
  de Tango pierde contra el arte propio (`input-keyboard` 27.1 contra 52.2 de
  contraste medio de luminancia), pero sobre blanco casi lo duplica, que es
  exactamente el fondo de este caso de uso. Ademas trae la cobertura de tipos
  que el arte propio no tiene y sigue la Icon Naming Specification de
  freedesktop, que es lo que permite cambiar de origen sin reescribir
  `diskfs/mimeicon.ini`
- Nota: los iconos de `devices/` y `places/` de Tango **no** se adoptaron. Sobre
  el face gris se pierden, y son justo los que el escritorio necesitaria

## Solo build: no se distribuye

Las herramientas del toolchain horneado (LLVM, QEMU, xorriso, Ninja) no entran
en ninguna imagen: producen los binarios y quedan afuera. Su procedencia,
version y `sha256` estan fijados en `tools/toolchain.lock.json`, que es el
registro autoritativo para ellas. No se duplican aca.

## Referencia: sin bits de terceros en el repo

Estas adopciones son de diseno, no de codigo: no hay lineas copiadas y por lo
tanto no hay nada que licenciar ni redistribuir. La decision y el motivo de
cada una viven en `docs/THIRD_PARTY_ADOPTION.md`.

- SerenityOS (BSD-2-Clause): `WindowServer` y `Compositor` para la
  arquitectura del compositor, las regiones sucias y la oclusion; `LibGfx`
  (`Bitmap`, `Painter`, `DisjointRectSet`) para el toolkit y el manejo de
  rects; `Kernel/Devices/GPU/DisplayConnector` para la capa `display`. SavanXP
  implementa su propia variante en C sobre su ABI, sin dependencias de `AK`
- OpenBSD (ISC y BSD-2): candidatos evaluados y categorizados en el doc de
  adopcion. Todavia no hay ninguna pieza incorporada, asi que no hay entrada
  distribuida que registrar
