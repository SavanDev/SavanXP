#include "libc.h"
#include "savanxp/sxgui.h"

#include <time.h>

/*
 * Popup del reloj de la taskbar: calendario del mes en curso estilo W2K con
 * la hora abajo. Mismo molde que volumepopup (cliente sin bordes, anclado
 * por windowd en la esquina inferior derecha, sin .sxres para no salir en el
 * launcher): binario aparte, lanzado on-demand por windowd cuando la taskbar
 * pide SAVANXP_DESKTOP_LAUNCH_FLAG_CLOCK_POPUP. Solo muestra: no recibe foco
 * de teclado y sus clicks se drenan sin accion -- el WM lo abre y lo cierra
 * (toggle del reloj, click afuera). El tamano lo reserva windowd
 * (WINDOWD_CLOCK_POPUP_WIDTH/HEIGHT en windowd.c): si cambia ahi, cambia aca.
 */

#define CLOCKPOPUP_WIDTH 178
#define CLOCKPOPUP_HEIGHT 180
#define CLOCKPOPUP_MARGIN 4
#define CLOCKPOPUP_HEADER_HEIGHT 22
#define CLOCKPOPUP_DOW_HEIGHT 16
#define CLOCKPOPUP_CELL_WIDTH 24
#define CLOCKPOPUP_CELL_HEIGHT 18
#define CLOCKPOPUP_WEEKS 6
#define CLOCKPOPUP_FOOTER_HEIGHT 18
#define CLOCKPOPUP_POLL_FRAMES 30

static const char *k_month_names[12] = {
    "enero", "febrero", "marzo", "abril", "mayo", "junio",
    "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre",
};
static const char *k_dow_labels[7] = {"lu", "ma", "mi", "ju", "vi", "sa", "do"};

/* Mismo bisel que taskbar_bevel (taskbar.c) -- copiado, no compartido: ninguno
 * de los dos linkea el toolkit, y es tres lineas. */
static void popup_bevel(struct sx_painter *painter, struct sx_rect rect, int sunken)
{
    uint32_t top_left = sunken ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = sunken ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

static int is_leap_year(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month)
{
    switch (month)
    {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
        return 31;
    case 4:
    case 6:
    case 9:
    case 11:
        return 30;
    case 2:
        return is_leap_year(year) ? 29 : 28;
    default:
        return 0;
    }
}

/* Columna del dia 1 (0 = lunes, ES): gmtime ya calcula el dia de semana. */
static int first_weekday_column(int year, int month)
{
    struct tm fields;
    struct tm broken;
    time_t stamp;

    memset(&fields, 0, sizeof(fields));
    fields.tm_year = year - 1900;
    fields.tm_mon = month - 1;
    fields.tm_mday = 1;
    fields.tm_hour = 12;
    stamp = timegm(&fields);
    if (stamp == (time_t)-1 || gmtime_r(&stamp, &broken) == 0)
    {
        return 0;
    }
    return (broken.tm_wday + 6) % 7;
}

static int popup_grid_x(void)
{
    return CLOCKPOPUP_MARGIN + 1;
}

static void popup_draw_text_centered(struct sx_painter *painter, struct sx_rect rect, const char *text, uint32_t colour)
{
    int text_x = rect.x + (rect.width - gfx_text_width(text)) / 2;
    int text_y = rect.y + (rect.height - gfx_text_height()) / 2;

    sx_painter_draw_text(painter, text_x, text_y, text, colour);
}

static void popup_paint(struct savanxp_gfx_context *gfx, int year, int month, int day,
                        const char *time_text, int have_date)
{
    struct sx_bitmap bitmap;
    struct sx_painter painter;
    struct sx_rect frame;
    struct sx_rect header;
    struct sx_rect footer;
    char header_text[32];
    int header_length = 0;
    int column;
    int week;
    int first_column;
    int month_days;

    sx_bitmap_wrap(&bitmap, gfx->pixels, &gfx->info, SX_PIXEL_FORMAT_BGRX8888);
    sx_painter_init(&painter, &bitmap);

    frame = sx_rect_make(0, 0, (int)gfx->info.width, (int)gfx->info.height);
    sx_painter_fill_rect(&painter, frame, SXGUI_COLOR_FACE);
    popup_bevel(&painter, frame, 0);

    header = sx_rect_make(
        CLOCKPOPUP_MARGIN,
        CLOCKPOPUP_MARGIN,
        CLOCKPOPUP_WIDTH - (CLOCKPOPUP_MARGIN * 2),
        CLOCKPOPUP_HEADER_HEIGHT);
    if (have_date && month >= 1 && month <= 12)
    {
        const char *name = k_month_names[month - 1];
        int index = 0;

        while (name[index] != '\0' && header_length < 12)
        {
            header_text[header_length++] = name[index++];
        }
        header_text[header_length++] = ' ';
        header_text[header_length++] = 'd';
        header_text[header_length++] = 'e';
        header_text[header_length++] = ' ';
        {
            /* Anio en decimal sin snprintf: cuatro digitos. */
            int rest = year;
            int divisor = 1000;

            while (divisor > 0 && header_length < (int)sizeof(header_text) - 1)
            {
                header_text[header_length++] = (char)('0' + (rest / divisor) % 10);
                rest = rest % divisor;
                divisor /= 10;
            }
        }
        header_text[header_length] = '\0';
    }
    else
    {
        const char *fallback = "(sin fecha)";
        int index = 0;

        while (fallback[index] != '\0')
        {
            header_text[header_length++] = fallback[index++];
        }
        header_text[header_length] = '\0';
    }
    popup_draw_text_centered(&painter, header, header_text, SXGUI_COLOR_TEXT);

    for (column = 0; column < 7; ++column)
    {
        struct sx_rect cell = sx_rect_make(
            popup_grid_x() + column * CLOCKPOPUP_CELL_WIDTH,
            CLOCKPOPUP_MARGIN + CLOCKPOPUP_HEADER_HEIGHT + 2,
            CLOCKPOPUP_CELL_WIDTH,
            CLOCKPOPUP_DOW_HEIGHT);

        popup_draw_text_centered(&painter, cell, k_dow_labels[column], SXGUI_COLOR_TEXT);
    }

    first_column = have_date ? first_weekday_column(year, month) : 7;
    month_days = have_date ? days_in_month(year, month) : 0;
    for (week = 0; week < CLOCKPOPUP_WEEKS; ++week)
    {
        for (column = 0; column < 7; ++column)
        {
            struct sx_rect cell = sx_rect_make(
                popup_grid_x() + column * CLOCKPOPUP_CELL_WIDTH,
                CLOCKPOPUP_MARGIN + CLOCKPOPUP_HEADER_HEIGHT + 2 + CLOCKPOPUP_DOW_HEIGHT +
                    week * CLOCKPOPUP_CELL_HEIGHT,
                CLOCKPOPUP_CELL_WIDTH,
                CLOCKPOPUP_CELL_HEIGHT);
            int day_number = week * 7 + column - first_column + 1;
            char digits[3];
            int length = 0;

            if (day_number < 1 || day_number > month_days)
            {
                continue;
            }
            if (day_number >= 10)
            {
                digits[length++] = (char)('0' + day_number / 10);
            }
            digits[length++] = (char)('0' + day_number % 10);
            digits[length] = '\0';
            if (day_number == day)
            {
                sx_painter_fill_rect(&painter, cell, SXGUI_COLOR_SELECT);
                popup_draw_text_centered(&painter, cell, digits, SXGUI_COLOR_SELECT_TEXT);
            }
            else
            {
                popup_draw_text_centered(&painter, cell, digits, SXGUI_COLOR_TEXT);
            }
        }
    }

    footer = sx_rect_make(
        CLOCKPOPUP_MARGIN,
        CLOCKPOPUP_HEIGHT - CLOCKPOPUP_MARGIN - CLOCKPOPUP_FOOTER_HEIGHT,
        CLOCKPOPUP_WIDTH - (CLOCKPOPUP_MARGIN * 2),
        CLOCKPOPUP_FOOTER_HEIGHT);
    popup_draw_text_centered(&painter, footer, time_text, SXGUI_COLOR_TEXT);
}

int main(void)
{
    struct savanxp_gfx_context gfx;
    struct savanxp_input_event event;
    struct savanxp_gui_pointer_event pointer;
    int year = 0;
    int month = 0;
    int day = 0;
    long stamp = -1;
    char time_text[9] = "--:--:--";
    int have_date = 0;
    int needs_repaint = 1;
    unsigned int poll_countdown = 0;

    if (gfx_open(&gfx) < 0)
    {
        puts_fd(2, "clockpopup: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&gfx) < 0)
    {
        puts_fd(2, "clockpopup: gfx_acquire failed\n");
        gfx_close(&gfx);
        return 1;
    }

    for (;;)
    {
        while (gfx_poll_event(&gfx, &event) > 0)
        {
            if (event.type == SAVANXP_INPUT_EVENT_RESIZED)
            {
                (void)gfx_apply_resize_event(&gfx, &event);
                needs_repaint = 1;
            }
        }

        /* Sin accion: el popup es de solo muestra y el WM lo cierra. Hay que
         * drenar igual, o la cola del canal se llena y el WM se queda sin
         * poder avisar. */
        while (gfx_poll_pointer(SAVANXP_WM_FD_EVENTS, &pointer) > 0)
        {
        }

        if (poll_countdown == 0)
        {
            struct savanxp_realtime now;
            long current;

            memset(&now, 0, sizeof(now));
            if (realtime(&now) == 0 && now.valid != 0)
            {
                current = ((long)now.hour * 3600L) + ((long)now.minute * 60L) + (long)now.second;
                time_text[0] = (char)('0' + now.hour / 10);
                time_text[1] = (char)('0' + now.hour % 10);
                time_text[2] = ':';
                time_text[3] = (char)('0' + now.minute / 10);
                time_text[4] = (char)('0' + now.minute % 10);
                time_text[5] = ':';
                time_text[6] = (char)('0' + now.second / 10);
                time_text[7] = (char)('0' + now.second % 10);
                time_text[8] = '\0';
                /* El mes tambien puede cambiar con el popup abierto (medianoche
                 * del 31): si el dia no es el mismo, la grilla se recalcula. */
                if (!have_date || year != (int)now.year || month != (int)now.month ||
                    day != (int)now.day)
                {
                    year = (int)now.year;
                    month = (int)now.month;
                    day = (int)now.day;
                    have_date = 1;
                }
            }
            else
            {
                unsigned long total_seconds = uptime_ms() / 1000UL;
                unsigned int hours = (unsigned int)((total_seconds / 3600UL) % 24UL);
                unsigned int minutes = (unsigned int)((total_seconds / 60UL) % 60UL);
                unsigned int seconds = (unsigned int)(total_seconds % 60UL);

                current = (long)total_seconds;
                time_text[0] = (char)('0' + hours / 10);
                time_text[1] = (char)('0' + hours % 10);
                time_text[2] = ':';
                time_text[3] = (char)('0' + minutes / 10);
                time_text[4] = (char)('0' + minutes % 10);
                time_text[5] = ':';
                time_text[6] = (char)('0' + seconds / 10);
                time_text[7] = (char)('0' + seconds % 10);
                time_text[8] = '\0';
            }
            if (current != stamp)
            {
                stamp = current;
                needs_repaint = 1;
            }
            poll_countdown = CLOCKPOPUP_POLL_FRAMES;
        }
        else
        {
            poll_countdown -= 1;
        }

        if (needs_repaint)
        {
            popup_paint(&gfx, year, month, day, time_text, have_date);
            /* present falla cuando el WM cierra la sesion o el link cae. */
            if (gfx_present(&gfx, gfx.pixels) < 0)
            {
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
