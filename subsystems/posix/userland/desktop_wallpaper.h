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
 * The desktop process draws the background, but progman (Options menu) is
 * what CHANGES it: they are different processes, and the config file is what
 * keeps them in step. The two entries below cover each side.
 */

/* Cicla el modo persistido SIN decodificar la imagen: la disponibilidad del BMP
 * se decide abriendo el archivo. Para procesos que cambian el fondo pero no lo
 * dibujan (progman), que no tienen por que pagar el decode ni la memoria.
 * No toca el estado en memoria del proceso. Devuelve el modo nuevo. */
int desktop_wallpaper_cycle_config(void);

/* Re-reads the persisted mode and applies it if it changed. Returns 1 if it
 * did (and a repaint is due), 0 if not. The desktop uses it to react when
 * another process changes the wallpaper. */
int desktop_wallpaper_reload(void);
/* Pinta el fondo y, encima, el nombre y la version del sistema abajo a la
 * derecha. */
void desktop_wallpaper_draw(struct sx_painter *painter, const struct savanxp_fb_info *info);

/* Height of the taskbar, to keep the version stamp above it. progman shares
 * this module and does not know the desktop's layout, so it is a copy:
 * desktop/desktop.c asserts it matches DESKTOP_TASKBAR_HEIGHT. */
#define DESKTOP_WALLPAPER_TASKBAR_HEIGHT 28
