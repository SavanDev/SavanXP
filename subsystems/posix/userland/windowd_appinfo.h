#pragma once

#include "libc.h"
#include "desktop_icons.h"

/*
 * Tabla de presentacion de ventanas.
 *
 * Lo que queda del viejo catalogo del escritorio: el WM ya NO la usa para
 * decidir como lanzar nada -- eso lo declara quien pide el launch, via los
 * flags del protocolo (A2.3a) --, y el catalogo de programas vive en el
 * registro de progman (/disk/progman.ini). Esta tabla solo traduce un path a
 * un nombre lindo, un icono y un color de barra de titulo, para las ventanas y
 * el Task List.
 *
 * El arreglo de fondo es que cada cliente informe su propio titulo/icono por
 * protocolo; hasta entonces el WM adivina por path y cae a defaults genericos
 * para lo que no conoce.
 */

struct windowd_appinfo
{
    const char *label;
    const char *path;
    enum desktop_icon_id icon_id;
    uint32_t accent;
};

/* Devuelve 0 si el path no esta en la tabla: el llamador usa defaults. */
const struct windowd_appinfo *windowd_appinfo_for_path(const char *path);
