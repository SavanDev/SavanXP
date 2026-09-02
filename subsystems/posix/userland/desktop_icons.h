#pragma once

#include "libc.h"

/*
 * El set horneado quedo reducido al generico. Doom, Shell, Notepad, Gfx Demo,
 * Key Test y Mouse Test tenian su entrada aca; los seis salieron una vez que
 * cada uno consiguio su propio .sxicon (icon_file= o icon=<nombre> en su
 * .sxres, estampado en build) y las tablas que los citaban por id
 * (progman_registry.c, windowd_appinfo.c) pasaron a apuntar al generico como
 * fallback -- docs/SXE_FORMAT.md, "icon= ya no elige de un catalogo".
 *
 * Lo que queda es la red de seguridad universal: el icono para CUALQUIER
 * programa cuyo binario no se pueda leer en absoluto. No hay una entrada por
 * app posible, porque no hay ningun otro id que un binario pueda no llegar a
 * tener -- el propio genero es el ultimo recurso, no una opcion mas.
 */
enum desktop_icon_id
{
    DESKTOP_ICON_DESKTOP = 0,
    DESKTOP_ICON_COUNT
};

struct desktop_embedded_bitmap
{
    uint32_t width;
    uint32_t height;
    const uint32_t *pixels;
};

const struct desktop_embedded_bitmap *desktop_icon_small(enum desktop_icon_id id);
const struct desktop_embedded_bitmap *desktop_icon_large(enum desktop_icon_id id);
