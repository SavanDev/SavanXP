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

/*
 * Desde A2 el fondo lo dibuja el cliente shellui, pero quien lo CAMBIA es
 * progman (menu Options): son procesos distintos, y el archivo de config es lo
 * que los sincroniza. Las dos entradas de abajo cubren cada lado.
 */

/* Cicla el modo persistido SIN decodificar la imagen: la disponibilidad del BMP
 * se decide abriendo el archivo. Para procesos que cambian el fondo pero no lo
 * dibujan (progman), que no tienen por que pagar el decode ni la memoria.
 * No toca el estado en memoria del proceso. Devuelve el modo nuevo. */
int desktop_wallpaper_cycle_config(void);

/* Relee el modo persistido y lo aplica si cambio. Devuelve 1 si hubo cambio (y
 * hay que repintar), 0 si no. Lo usa el cliente de fondo para reaccionar cuando
 * otro proceso cambia el wallpaper. */
int desktop_wallpaper_reload(void);
void desktop_wallpaper_draw(struct sx_painter *painter, const struct savanxp_fb_info *info);
