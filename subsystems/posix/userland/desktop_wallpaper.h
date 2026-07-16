#pragma once

#include "libc.h"

/* Fondo de pantalla del escritorio: modos integrados (procedurales) mas una
 * imagen BMP opcional cargada de /disk. El modo elegido persiste en
 * /disk/desktop.cfg y se restaura en el proximo arranque. */
enum desktop_wallpaper_mode
{
    DESKTOP_WALLPAPER_TEAL = 0,
    DESKTOP_WALLPAPER_GRADIENT,
    DESKTOP_WALLPAPER_PATTERN,
    DESKTOP_WALLPAPER_IMAGE,
    DESKTOP_WALLPAPER_MODE_COUNT
};

/* Carga la config persistida e intenta decodificar /disk/wallpaper.bmp. Solo
 * la sesion interactiva la llama: los selftests headless se quedan con el
 * modo default (teal plano) para mantener sus frames deterministas. */
void desktop_wallpaper_init(void);
int desktop_wallpaper_mode(void);
/* Pasa al siguiente modo disponible (saltea IMAGE si no hay BMP cargado) y
 * persiste la eleccion. Devuelve el modo nuevo. */
int desktop_wallpaper_cycle(void);
void desktop_wallpaper_draw(struct sx_painter *painter, const struct savanxp_fb_info *info);
