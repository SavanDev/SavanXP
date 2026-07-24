#include "libc.h"
#include "savanxp/sxgui.h"

#include "desktop_wallpaper.h"

/*
 * shell-client de background (A2, modelo "Fondo + pivot NT 3.5"; ver
 * docs/WM_SUBSYSTEM.md).
 *
 * Paso A2.1: un proceso aparte que se conecta como cliente del WM y dibuja el
 * wallpaper en su superficie. En A2.2 windowd lo lanza como ROLE_SHELL y compone
 * su superficie como capa de fondo (full-screen, debajo de las ventanas), y deja
 * de dibujar el wallpaper el mismo. Los iconos del escritorio y su input (doble
 * click -> launch por el fd de launch) llegan en A2.3.
 */

static void shellui_paint(struct savanxp_gfx_context *gfx)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);
    desktop_wallpaper_draw(&painter, &gfx->info);
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    int needs_repaint = 1;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "shellui: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "shellui: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    /* Carga la config persistida e intenta decodificar /disk/wallpaper.bmp. */
    desktop_wallpaper_init();

    for (;;)
    {
        while (gfx_poll_event(&gfx, &event) > 0)
        {
            /* El WM reasigna el tamano de la superficie de fondo cuando cambia
             * el modo de video; volvemos a pintar el wallpaper al nuevo tamano. */
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &event);
                needs_repaint = 1;
            }
        }

        if (needs_repaint)
        {
            shellui_paint(&gfx);
            if (gfx_present(&gfx, gfx.pixels) < 0)
            {
                /* present falla cuando el WM cierra la sesion (shutdown event)
                 * o el link cae: salimos limpio. */
                break;
            }
            needs_repaint = 0;
        }
        else
        {
            sleep_ms(16);
        }
    }

    gfx_close(&gfx);
    return 0;
}
