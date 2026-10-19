#include "desktop.h"

#include <time.h>

/*
 * The taskbar clock's popup: a W2K-style calendar of the current month with
 * the time below. A popup of the desktop, anchored in the tray corner. Display
 * only: its clicks are drained with no action; the clock button toggles it and
 * a press outside closes it.
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
    desktop_bevel(&painter, frame, 0);

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

static int g_year = 0;
static int g_month = 0;
static int g_day = 0;
static long g_stamp = -1;
static char g_time_text[9] = "--:--:--";
static int g_have_date = 0;
static unsigned int g_poll_countdown = 0;

static int clock_open(void)
{
    g_stamp = -1;
    g_have_date = 0;
    g_poll_countdown = 0;
    memcpy(g_time_text, "--:--:--", sizeof(g_time_text));
    return 0;
}

static void clock_close(void)
{
}

/* Display only: the pointer does nothing. */
static uint32_t clock_pointer(struct savanxp_gfx_context *gfx, const struct savanxp_gui_pointer_event *pointer)
{
    (void)gfx;
    (void)pointer;
    return DESKTOP_ACTION_NONE;
}

static uint32_t clock_tick(void)
{
    uint32_t actions = DESKTOP_ACTION_NONE;

    if (g_poll_countdown == 0)
    {
        struct savanxp_realtime now;
        long current;

        memset(&now, 0, sizeof(now));
        if (realtime(&now) == 0 && now.valid != 0)
        {
            current = ((long)now.hour * 3600L) + ((long)now.minute * 60L) + (long)now.second;
            g_time_text[0] = (char)('0' + now.hour / 10);
            g_time_text[1] = (char)('0' + now.hour % 10);
            g_time_text[2] = ':';
            g_time_text[3] = (char)('0' + now.minute / 10);
            g_time_text[4] = (char)('0' + now.minute % 10);
            g_time_text[5] = ':';
            g_time_text[6] = (char)('0' + now.second / 10);
            g_time_text[7] = (char)('0' + now.second % 10);
            g_time_text[8] = '\0';
            /* El mes tambien puede cambiar con el popup abierto (medianoche
             * del 31): si el dia no es el mismo, la grilla se recalcula. */
            if (!g_have_date || g_year != (int)now.year || g_month != (int)now.month ||
                g_day != (int)now.day)
            {
                g_year = (int)now.year;
                g_month = (int)now.month;
                g_day = (int)now.day;
                g_have_date = 1;
            }
        }
        else
        {
            unsigned long total_seconds = uptime_ms() / 1000UL;
            unsigned int hours = (unsigned int)((total_seconds / 3600UL) % 24UL);
            unsigned int minutes = (unsigned int)((total_seconds / 60UL) % 60UL);
            unsigned int seconds = (unsigned int)(total_seconds % 60UL);

            current = (long)total_seconds;
            g_time_text[0] = (char)('0' + hours / 10);
            g_time_text[1] = (char)('0' + hours % 10);
            g_time_text[2] = ':';
            g_time_text[3] = (char)('0' + minutes / 10);
            g_time_text[4] = (char)('0' + minutes % 10);
            g_time_text[5] = ':';
            g_time_text[6] = (char)('0' + seconds / 10);
            g_time_text[7] = (char)('0' + seconds % 10);
            g_time_text[8] = '\0';
        }
        if (current != g_stamp)
        {
            g_stamp = current;
            actions |= DESKTOP_ACTION_REPAINT;
        }
        g_poll_countdown = CLOCKPOPUP_POLL_FRAMES;
    }
    else
    {
        g_poll_countdown -= 1;
    }

    return actions;
}

static void clock_paint(struct savanxp_gfx_context *gfx)
{
    popup_paint(gfx, g_year, g_month, g_day, g_time_text, g_have_date);
}

const struct desktop_popup_ops desktop_clock_ops = {
    CLOCKPOPUP_WIDTH,
    CLOCKPOPUP_HEIGHT,
    1,
    clock_open,
    clock_close,
    clock_pointer,
    clock_tick,
    clock_paint,
};
