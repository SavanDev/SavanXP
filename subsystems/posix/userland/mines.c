#include "libc.h"
#include "savanxp/sxgui.h"

#include "mines_board.h"

#include <stdio.h>
#include <string.h>

#include "shared/version.h"

/*
 * Buscaminas, con la forma del de la era Win95: el panel de estado con los dos
 * contadores de LEDs y la cara en el medio, y abajo el tablero hundido.
 *
 * Es el primer juego que viene EN el sistema, no un port externo como Doom
 * (docs/SYSTEM_LAYERING.md, "Games, and the first one"). Las reglas viven en
 * mines_board.c -- este archivo es solo la ventana: layout, dibujo y entrada.
 *
 * La grilla NO son widgets. sxgui no tiene jerarquia y 480 celdas serian 480
 * widgets para dibujar catorce formas distintas: se pinta en on_paint y el
 * hit-testing va en on_pointer, igual que la grilla de iconos de progman. El
 * unico widget de la ventana es la cara, porque un boton que se hunde al
 * apretarlo ya existe y copiarlo aca era tener una version peor.
 */

/* --- metricas --------------------------------------------------------------
 *
 * La celda de 16 px es la del original y no es negociable: el numero se dibuja
 * con la fuente monoespaciada, que mide 8x16, asi que entra centrado y sin
 * escalar. Cambiar la celda obliga a revisar cada glifo de este archivo. */
#define MINES_CELL_SIZE 16
#define MINES_MARGIN 8
#define MINES_PANEL_GAP 6
#define MINES_STATUS_HEIGHT 36
#define MINES_STATUS_PAD 6
#define MINES_DIGIT_WIDTH 13
#define MINES_DIGIT_HEIGHT 23
#define MINES_COUNTER_DIGITS 3
#define MINES_COUNTER_WIDTH (MINES_DIGIT_WIDTH * MINES_COUNTER_DIGITS)
#define MINES_FACE_SIZE 26

/* Colores del juego, que no son los del sistema: los numeros del Buscaminas son
 * una convencion propia y quien los cambie cambia el juego. */
#define MINES_COLOUR_LED SXGUI_RGB(255, 0, 0)
#define MINES_COLOUR_LED_OFF SXGUI_RGB(60, 0, 0)
#define MINES_COLOUR_LED_BACK SXGUI_RGB(0, 0, 0)
#define MINES_COLOUR_FACE_SKIN SXGUI_RGB(255, 255, 0)
#define MINES_COLOUR_MINE SXGUI_RGB(0, 0, 0)
#define MINES_COLOUR_EXPLODED SXGUI_RGB(255, 0, 0)
#define MINES_COLOUR_FLAG SXGUI_RGB(255, 0, 0)

/* Estado de la cara. No es el estado de la partida: la cara tambien reacciona
 * al boton apretado, que no cambia el tablero. */
enum mines_face
{
    MINES_FACE_HAPPY = 0,
    MINES_FACE_SURPRISED,
    MINES_FACE_DEAD,
    MINES_FACE_COOL
};

enum mines_command
{
    MINES_CMD_NEW = 1,
    MINES_CMD_BEGINNER,
    MINES_CMD_INTERMEDIATE,
    MINES_CMD_EXPERT,
    MINES_CMD_MARKS,
    MINES_CMD_BEST_TIMES,
    MINES_CMD_EXIT,
    MINES_CMD_ABOUT
};

static struct sxgui_app g_app;
static struct sxgui_widget g_widgets[1];
#define MINES_FACE_BUTTON (&g_widgets[0])

static struct mines_board g_board;

/* Reloj de la partida. Arranca con el primer descubrimiento y se congela al
 * terminar, como el original.
 *
 * uptime_ms y no monotonic_ns: el reloj de pared del sistema es el que sigue al
 * tiempo de la MAQUINA, y el TSC adentro de un hipervisor mide el del host
 * (docs/TIME.md). Un cronometro que corre al 561% en VirtualBox no es un
 * cronometro. */
static unsigned long g_start_ms = 0;
static int g_elapsed_seconds = 0;

/* Cursor de teclado. Aparece con la primera flecha: sin eso, una ventana que se
 * juega con el mouse mostraria un recuadro que nadie pidio. */
static int g_cursor_column = 0;
static int g_cursor_row = 0;
static int g_cursor_visible = 0;

static uint32_t g_last_buttons = 0;
/* 1 mientras el boton izquierdo esta apretado sobre el tablero: es lo que pone
 * la cara sorprendida. */
static int g_pressing = 0;

/* Donde arranco el apriete del boton izquierdo. Un click es del lugar donde
 * EMPEZO: arrastrar desde el tablero hasta la cara y soltar ahi no empieza una
 * partida nueva -- si no se distinguiera, el gesto de "me arrepenti, muevo el
 * mouse afuera antes de soltar" tiraria la partida en curso. */
#define MINES_PRESS_NONE 0
#define MINES_PRESS_BOARD 1
#define MINES_PRESS_FACE 2
static int g_press_origin = MINES_PRESS_NONE;

static void on_menu_command(int id, void *user);
static void mines_relayout(void);

/* --- barra de menu --------------------------------------------------------
 *
 * Mutable porque los tildes son estado: el nivel activo y las marcas se leen
 * del propio menu, que es donde el usuario los va a mirar.
 *
 * Los indices de los items que llevan tilde son constantes con nombre: contarlos
 * a mano en refresh_menu_checks es como se termina tildando el separador al
 * agregar una entrada. */
#define MINES_MENU_LEVEL_FIRST 2
#define MINES_MENU_MARKS 6

static struct sxgui_menu_item k_game_items[] = {
    {"New\tF2", MINES_CMD_NEW, 0},
    {0, 0, 0},
    {"Beginner", MINES_CMD_BEGINNER, SXGUI_MENU_CHECKED},
    {"Intermediate", MINES_CMD_INTERMEDIATE, 0},
    {"Expert", MINES_CMD_EXPERT, 0},
    {0, 0, 0},
    {"Marks (?)", MINES_CMD_MARKS, SXGUI_MENU_CHECKED},
    {0, 0, 0},
    {"Best Times...", MINES_CMD_BEST_TIMES, 0},
    {0, 0, 0},
    {"Exit", MINES_CMD_EXIT, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"About Minesweeper", MINES_CMD_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"Game", k_game_items, (int)(sizeof(k_game_items) / sizeof(k_game_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menubar g_menubar = {
    k_menus,
    (int)(sizeof(k_menus) / sizeof(k_menus[0])),
    -1,
    -1,
    on_menu_command,
    0
};

/* --- dialogos ------------------------------------------------------------- */

#define MINES_DLG_MARGIN SXGUI_DIALOG_MARGIN
#define MINES_DLG_ROW 22
#define MINES_DLG_BUTTON_ROW(height) ((height) - MINES_DLG_MARGIN - SXGUI_BUTTON_HEIGHT)
#define MINES_DLG_CENTRED(width, count, index) \
    (((width) - (count) * SXGUI_BUTTON_WIDTH - ((count) - 1) * SXGUI_GAP) / 2 + \
     (index) * (SXGUI_BUTTON_WIDTH + SXGUI_GAP))

#define MINES_TIMES_WIDTH 300
#define MINES_TIMES_HEIGHT 172
#define MINES_ABOUT_WIDTH 320
#define MINES_ABOUT_HEIGHT 184

static struct sxgui_dialog g_times_dialog;
/* Titulo + tres nombres + tres tiempos + dos botones. Los nombres y los tiempos
 * son etiquetas SEPARADAS, en dos columnas: rellenar con espacios no alinea nada
 * con una fuente proporcional. */
static struct sxgui_widget g_times_widgets[9];
#define MINES_TIMES_NAME_COLUMN 150
static char g_times_values[MINES_LEVEL_COUNT][32];
static char g_times_headline[64];

static struct sxgui_dialog g_about_dialog;
static struct sxgui_widget g_about_widgets[6];

/* --- geometria de la ventana ---------------------------------------------- */

static int content_top(void)
{
    return sxgui_menubar_height();
}

static int panel_width(void)
{
    return (g_board.columns * MINES_CELL_SIZE) + (SXGUI_BORDER_SUNKEN * 2);
}

static int panel_height(void)
{
    return MINES_STATUS_HEIGHT + MINES_PANEL_GAP +
           (g_board.rows * MINES_CELL_SIZE) + (SXGUI_BORDER_SUNKEN * 2);
}

/*
 * Esquina del panel. Centrado y no clavado al margen porque el pedido de tamano
 * es una SUGERENCIA: el WM la atiende solo mientras la ventana siga en la
 * geometria con la que se lanzo (savanxp/wm_protocol.h). Al cambiar de Experto a
 * Principiante en una ventana ya movida, el tablero chico queda en una ventana
 * grande -- centrado se lee como una eleccion, pegado a la esquina como un bug.
 */
static struct sx_point panel_origin(void)
{
    struct sx_point origin;
    int available_width = (int)g_app.gfx.info.width;
    int available_height = (int)g_app.gfx.info.height - content_top();

    origin.x = (available_width - panel_width()) / 2;
    origin.y = content_top() + ((available_height - panel_height()) / 2);
    if (origin.x < MINES_MARGIN)
    {
        origin.x = MINES_MARGIN;
    }
    if (origin.y < content_top() + MINES_MARGIN)
    {
        origin.y = content_top() + MINES_MARGIN;
    }
    return origin;
}

static struct sx_rect status_rect(void)
{
    struct sx_point origin = panel_origin();

    return sx_rect_make(origin.x, origin.y, panel_width(), MINES_STATUS_HEIGHT);
}

static struct sx_rect grid_rect(void)
{
    struct sx_point origin = panel_origin();

    return sx_rect_make(
        origin.x,
        origin.y + MINES_STATUS_HEIGHT + MINES_PANEL_GAP,
        panel_width(),
        (g_board.rows * MINES_CELL_SIZE) + (SXGUI_BORDER_SUNKEN * 2));
}

static struct sx_rect cell_rect(int column, int row)
{
    struct sx_rect grid = grid_rect();

    return sx_rect_make(
        grid.x + SXGUI_BORDER_SUNKEN + (column * MINES_CELL_SIZE),
        grid.y + SXGUI_BORDER_SUNKEN + (row * MINES_CELL_SIZE),
        MINES_CELL_SIZE,
        MINES_CELL_SIZE);
}

static struct sx_rect face_rect(void)
{
    struct sx_rect status = status_rect();

    return sx_rect_make(
        status.x + ((status.width - MINES_FACE_SIZE) / 2),
        status.y + ((MINES_STATUS_HEIGHT - MINES_FACE_SIZE) / 2),
        MINES_FACE_SIZE,
        MINES_FACE_SIZE);
}

/* Celda bajo un punto, o 0 en column/row si el punto no cae en el tablero. */
static int cell_at_point(int x, int y, int *out_column, int *out_row)
{
    struct sx_rect grid = grid_rect();
    int column;
    int row;

    if (!sx_rect_contains_point(grid, x, y))
    {
        return 0;
    }
    column = (x - grid.x - SXGUI_BORDER_SUNKEN) / MINES_CELL_SIZE;
    row = (y - grid.y - SXGUI_BORDER_SUNKEN) / MINES_CELL_SIZE;
    if (column < 0 || row < 0 || column >= g_board.columns || row >= g_board.rows)
    {
        return 0;
    }
    *out_column = column;
    *out_row = row;
    return 1;
}

/* --- reloj y estado ------------------------------------------------------- */

static int mines_face_state(void)
{
    if (g_board.state == MINES_STATE_LOST)
    {
        return MINES_FACE_DEAD;
    }
    if (g_board.state == MINES_STATE_WON)
    {
        return MINES_FACE_COOL;
    }
    if (g_pressing)
    {
        return MINES_FACE_SURPRISED;
    }
    return MINES_FACE_HAPPY;
}

static int timer_running(void)
{
    return g_board.state == MINES_STATE_PLAYING;
}

static void refresh_menu_checks(void)
{
    int level;

    for (level = 0; level < MINES_LEVEL_COUNT; ++level)
    {
        struct sxgui_menu_item *item = &k_game_items[MINES_MENU_LEVEL_FIRST + level];

        if (level == g_board.level)
        {
            item->flags |= SXGUI_MENU_CHECKED;
        }
        else
        {
            item->flags &= ~(uint32_t)SXGUI_MENU_CHECKED;
        }
    }
    if (g_board.marks_enabled)
    {
        k_game_items[MINES_MENU_MARKS].flags |= SXGUI_MENU_CHECKED;
    }
    else
    {
        k_game_items[MINES_MENU_MARKS].flags &= ~(uint32_t)SXGUI_MENU_CHECKED;
    }
}

static void start_new_game(int level)
{
    mines_board_reset(&g_board, level);
    g_elapsed_seconds = 0;
    g_start_ms = 0;
    g_pressing = 0;
    g_cursor_column = 0;
    g_cursor_row = 0;
    refresh_menu_checks();
}

/* Cierre de partida: congela el reloj y anota el tiempo si es record. Devuelve 1
 * si hubo record, que es cuando vale la pena abrir el cuadro de tiempos. */
static int finish_game(void)
{
    if (g_board.state != MINES_STATE_WON)
    {
        return 0;
    }
    if (g_elapsed_seconds <= 0)
    {
        /* Ganar en menos de un segundo existe (un 4x4 se barre de un click) y el
         * original lo anota como 1: un record de cero segundos no se puede
         * batir. */
        g_elapsed_seconds = 1;
    }
    return mines_scores_record(g_board.level, g_elapsed_seconds);
}

static void refresh_times_lines(void)
{
    int level;

    for (level = 0; level < MINES_LEVEL_COUNT; ++level)
    {
        int best = mines_scores_best(level);

        if (best > 0)
        {
            snprintf(g_times_values[level], sizeof(g_times_values[level]), "%d seconds", best);
        }
        else
        {
            /* "--" y no "0": nunca haber ganado no es haber ganado en cero. */
            snprintf(g_times_values[level], sizeof(g_times_values[level]), "--");
        }
    }
}

static void open_times_dialog(const char *headline)
{
    snprintf(g_times_headline, sizeof(g_times_headline), "%s",
             headline != 0 ? headline : "Fastest times, in seconds:");
    refresh_times_lines();
    sxgui_dialog_begin(&g_app.ui, &g_times_dialog, MINES_TIMES_WIDTH, MINES_TIMES_HEIGHT);
}

/* --- dibujo: LEDs --------------------------------------------------------- */

/*
 * Digito de siete segmentos, en una caja de 13x23 como el del original. Los
 * segmentos apagados se pintan en rojo oscuro y no en negro: es lo que hace que
 * un 1 se lea como un LED con seis segmentos apagados y no como un palito
 * suelto.
 *
 * Bits: A arriba, B arriba-derecha, C abajo-derecha, D abajo, E abajo-izquierda,
 * F arriba-izquierda, G medio.
 */
#define MINES_SEG_A (1u << 0)
#define MINES_SEG_B (1u << 1)
#define MINES_SEG_C (1u << 2)
#define MINES_SEG_D (1u << 3)
#define MINES_SEG_E (1u << 4)
#define MINES_SEG_F (1u << 5)
#define MINES_SEG_G (1u << 6)

static const uint32_t k_digit_segments[10] = {
    MINES_SEG_A | MINES_SEG_B | MINES_SEG_C | MINES_SEG_D | MINES_SEG_E | MINES_SEG_F,
    MINES_SEG_B | MINES_SEG_C,
    MINES_SEG_A | MINES_SEG_B | MINES_SEG_G | MINES_SEG_E | MINES_SEG_D,
    MINES_SEG_A | MINES_SEG_B | MINES_SEG_G | MINES_SEG_C | MINES_SEG_D,
    MINES_SEG_F | MINES_SEG_G | MINES_SEG_B | MINES_SEG_C,
    MINES_SEG_A | MINES_SEG_F | MINES_SEG_G | MINES_SEG_C | MINES_SEG_D,
    MINES_SEG_A | MINES_SEG_F | MINES_SEG_G | MINES_SEG_E | MINES_SEG_C | MINES_SEG_D,
    MINES_SEG_A | MINES_SEG_B | MINES_SEG_C,
    MINES_SEG_A | MINES_SEG_B | MINES_SEG_C | MINES_SEG_D | MINES_SEG_E | MINES_SEG_F | MINES_SEG_G,
    MINES_SEG_A | MINES_SEG_B | MINES_SEG_C | MINES_SEG_D | MINES_SEG_F | MINES_SEG_G,
};

static void paint_segment(struct sx_painter *painter, struct sx_rect rect, int lit)
{
    sx_painter_fill_rect(painter, rect, lit ? MINES_COLOUR_LED : MINES_COLOUR_LED_OFF);
}

/* `glyph` es un digito 0-9, o -1 para el signo menos del contador negativo. */
static void paint_digit(struct sx_painter *painter, int x, int y, int glyph)
{
    uint32_t segments = 0u;

    if (glyph >= 0 && glyph <= 9)
    {
        segments = k_digit_segments[glyph];
    }
    else if (glyph == -1)
    {
        segments = MINES_SEG_G;
    }

    sx_painter_fill_rect(painter, sx_rect_make(x, y, MINES_DIGIT_WIDTH, MINES_DIGIT_HEIGHT),
                         MINES_COLOUR_LED_BACK);
    paint_segment(painter, sx_rect_make(x + 2, y, 9, 3), (segments & MINES_SEG_A) != 0u);
    paint_segment(painter, sx_rect_make(x, y + 2, 3, 9), (segments & MINES_SEG_F) != 0u);
    paint_segment(painter, sx_rect_make(x + 10, y + 2, 3, 9), (segments & MINES_SEG_B) != 0u);
    paint_segment(painter, sx_rect_make(x + 2, y + 10, 9, 3), (segments & MINES_SEG_G) != 0u);
    paint_segment(painter, sx_rect_make(x, y + 12, 3, 9), (segments & MINES_SEG_E) != 0u);
    paint_segment(painter, sx_rect_make(x + 10, y + 12, 3, 9), (segments & MINES_SEG_C) != 0u);
    paint_segment(painter, sx_rect_make(x + 2, y + 20, 9, 3), (segments & MINES_SEG_D) != 0u);
}

/*
 * Contador de tres digitos. Se planta en 999 en vez de dar la vuelta, y un
 * valor negativo -- mas banderas que minas -- sale como "-nn", que es como lo
 * muestra el original.
 */
static void paint_counter(struct sx_painter *painter, int x, int y, int value)
{
    int glyphs[MINES_COUNTER_DIGITS];
    int index;

    if (value < 0)
    {
        int magnitude = -value;

        if (magnitude > 99)
        {
            magnitude = 99;
        }
        glyphs[0] = -1;
        glyphs[1] = magnitude / 10;
        glyphs[2] = magnitude % 10;
    }
    else
    {
        if (value > MINES_COUNTER_MAX)
        {
            value = MINES_COUNTER_MAX;
        }
        glyphs[0] = (value / 100) % 10;
        glyphs[1] = (value / 10) % 10;
        glyphs[2] = value % 10;
    }

    sxgui_draw_sunken_edge(
        painter,
        sx_rect_make(x - SXGUI_BORDER_SUNKEN, y - SXGUI_BORDER_SUNKEN,
                     MINES_COUNTER_WIDTH + (SXGUI_BORDER_SUNKEN * 2),
                     MINES_DIGIT_HEIGHT + (SXGUI_BORDER_SUNKEN * 2)));
    for (index = 0; index < MINES_COUNTER_DIGITS; ++index)
    {
        paint_digit(painter, x + (index * MINES_DIGIT_WIDTH), y, glyphs[index]);
    }
}

/* --- dibujo: la cara ------------------------------------------------------ */

/* La sonrisa es la mitad de abajo de una elipse: clipear y dibujar el contorno
 * da un arco parejo, y hacerlo a mano pixel por pixel daba una curva distinta
 * en cada tamano de cara. */
static void paint_smile(struct sx_painter *painter, struct sx_rect box)
{
    struct sx_rect ellipse = sx_rect_make(box.x + 4, box.y + 7, box.width - 8, box.height - 10);
    struct sx_rect clip = sx_rect_make(box.x, box.y + box.height - 8, box.width, 6);

    if (sx_painter_push_clip(painter, clip))
    {
        sx_painter_draw_ellipse(painter, ellipse, MINES_COLOUR_MINE);
        sx_painter_pop_clip(painter);
    }
}

static void paint_face(struct sx_painter *painter, struct sx_rect rect, int state, int offset)
{
    struct sx_rect box = sx_rect_make(rect.x + 3 + offset, rect.y + 3 + offset,
                                      rect.width - 6, rect.height - 6);
    int left_eye_x = box.x + 4;
    int right_eye_x = box.x + box.width - 7;
    int eye_y = box.y + 5;

    sx_painter_fill_ellipse(painter, box, MINES_COLOUR_FACE_SKIN);
    sx_painter_draw_ellipse(painter, box, MINES_COLOUR_MINE);

    if (state == MINES_FACE_DEAD)
    {
        /* Ojos en X y boca recta: la cara de haber pisado una mina. */
        int index;

        for (index = 0; index < 4; ++index)
        {
            sx_painter_set_pixel(painter, left_eye_x + index, eye_y + index, MINES_COLOUR_MINE);
            sx_painter_set_pixel(painter, left_eye_x + 3 - index, eye_y + index, MINES_COLOUR_MINE);
            sx_painter_set_pixel(painter, right_eye_x + index, eye_y + index, MINES_COLOUR_MINE);
            sx_painter_set_pixel(painter, right_eye_x + 3 - index, eye_y + index, MINES_COLOUR_MINE);
        }
        /* Boca recta y no una sonrisa al reves: es la del original, y una
         * sonrisa invertida en una cara de 20 px se lee como una sonrisa mal
         * dibujada. */
        sx_painter_hline(painter, box.x + 6, box.y + box.height - 6, box.width - 12, MINES_COLOUR_MINE);
        return;
    }

    if (state == MINES_FACE_COOL)
    {
        /* Los anteojos negros del que gano. */
        sx_painter_fill_rect(painter, sx_rect_make(left_eye_x - 1, eye_y, 5, 3), MINES_COLOUR_MINE);
        sx_painter_fill_rect(painter, sx_rect_make(right_eye_x - 1, eye_y, 5, 3), MINES_COLOUR_MINE);
        sx_painter_fill_rect(painter, sx_rect_make(left_eye_x + 4, eye_y + 1, (right_eye_x - left_eye_x) - 5, 1),
                             MINES_COLOUR_MINE);
        paint_smile(painter, box);
        return;
    }

    sx_painter_fill_rect(painter, sx_rect_make(left_eye_x, eye_y, 2, 3), MINES_COLOUR_MINE);
    sx_painter_fill_rect(painter, sx_rect_make(right_eye_x + 1, eye_y, 2, 3), MINES_COLOUR_MINE);
    if (state == MINES_FACE_SURPRISED)
    {
        /* La boca abierta del "ojala no sea una mina". */
        sx_painter_draw_ellipse(painter, sx_rect_make(box.x + (box.width / 2) - 2, box.y + box.height - 8, 5, 5),
                                MINES_COLOUR_MINE);
        return;
    }
    paint_smile(painter, box);
}

/* --- dibujo: el tablero --------------------------------------------------- */

static uint32_t number_colour(int neighbours)
{
    /* Los ocho colores del original, en orden. No son los del sistema: son parte
     * del juego, y alguien que jugo alguna vez lee el 3 en rojo sin contar. */
    static const uint32_t k_colours[9] = {
        0u,
        SXGUI_RGB(0, 0, 255),
        SXGUI_RGB(0, 128, 0),
        SXGUI_RGB(255, 0, 0),
        SXGUI_RGB(0, 0, 128),
        SXGUI_RGB(128, 0, 0),
        SXGUI_RGB(0, 128, 128),
        SXGUI_RGB(0, 0, 0),
        SXGUI_RGB(128, 128, 128),
    };

    if (neighbours < 1 || neighbours > 8)
    {
        return SXGUI_COLOR_TEXT;
    }
    return k_colours[neighbours];
}

static void paint_number(struct sx_painter *painter, struct sx_rect rect, int neighbours)
{
    char text[2];
    int saved_font = sx_painter_set_font(painter, SX_FONT_MONO);

    text[0] = (char)('0' + neighbours);
    text[1] = '\0';
    sx_painter_draw_text(
        painter,
        rect.x + ((rect.width - sx_painter_text_width(painter, text)) / 2),
        rect.y + ((rect.height - sx_painter_text_height(painter)) / 2),
        text,
        number_colour(neighbours));
    (void)sx_painter_set_font(painter, saved_font);
}

static void paint_mine(struct sx_painter *painter, struct sx_rect rect)
{
    int cx = rect.x + (rect.width / 2);
    int cy = rect.y + (rect.height / 2);

    sx_painter_hline(painter, cx - 5, cy, 11, MINES_COLOUR_MINE);
    sx_painter_vline(painter, cx, cy - 5, 11, MINES_COLOUR_MINE);
    sx_painter_draw_line(painter, cx - 3, cy - 3, cx + 3, cy + 3, MINES_COLOUR_MINE);
    sx_painter_draw_line(painter, cx + 3, cy - 3, cx - 3, cy + 3, MINES_COLOUR_MINE);
    sx_painter_fill_ellipse(painter, sx_rect_make(cx - 4, cy - 4, 8, 8), MINES_COLOUR_MINE);
    /* El brillo: dos pixeles blancos arriba a la izquierda. Sin eso la mina es
     * una mancha negra y no una esfera. */
    sx_painter_fill_rect(painter, sx_rect_make(cx - 2, cy - 2, 2, 2), SXGUI_COLOR_LIGHT);
}

static void paint_flag(struct sx_painter *painter, struct sx_rect rect)
{
    struct sx_point banner[3];
    int pole_x = rect.x + 8;

    banner[0].x = pole_x;
    banner[0].y = rect.y + 3;
    banner[1].x = rect.x + 3;
    banner[1].y = rect.y + 6;
    banner[2].x = pole_x;
    banner[2].y = rect.y + 9;
    sx_painter_fill_polygon(painter, banner, 3, MINES_COLOUR_FLAG);

    sx_painter_vline(painter, pole_x, rect.y + 3, 8, MINES_COLOUR_MINE);
    sx_painter_hline(painter, rect.x + 6, rect.y + 11, 5, MINES_COLOUR_MINE);
    sx_painter_hline(painter, rect.x + 4, rect.y + 12, 9, MINES_COLOUR_MINE);
}

static void paint_question(struct sx_painter *painter, struct sx_rect rect)
{
    int saved_font = sx_painter_set_font(painter, SX_FONT_MONO);

    sx_painter_draw_text(
        painter,
        rect.x + ((rect.width - sx_painter_text_width(painter, "?")) / 2),
        rect.y + ((rect.height - sx_painter_text_height(painter)) / 2),
        "?",
        SXGUI_COLOR_TEXT);
    (void)sx_painter_set_font(painter, saved_font);
}

/* La cruz sobre una bandera que estaba mal puesta. Solo aparece al perder: es la
 * unica forma de saber en que se equivoco uno. */
static void paint_wrong_flag(struct sx_painter *painter, struct sx_rect rect)
{
    paint_mine(painter, rect);
    sx_painter_draw_line(painter, rect.x + 2, rect.y + 2, rect.x + 13, rect.y + 13, MINES_COLOUR_EXPLODED);
    sx_painter_draw_line(painter, rect.x + 3, rect.y + 2, rect.x + 14, rect.y + 13, MINES_COLOUR_EXPLODED);
    sx_painter_draw_line(painter, rect.x + 13, rect.y + 2, rect.x + 2, rect.y + 13, MINES_COLOUR_EXPLODED);
    sx_painter_draw_line(painter, rect.x + 14, rect.y + 2, rect.x + 3, rect.y + 13, MINES_COLOUR_EXPLODED);
}

static void paint_cell(struct sx_painter *painter, int column, int row)
{
    const struct mines_cell *cell = mines_board_cell_const(&g_board, column, row);
    struct sx_rect rect = cell_rect(column, row);
    int lost = g_board.state == MINES_STATE_LOST;

    if (cell == 0)
    {
        return;
    }

    if (cell->revealed == 0u)
    {
        sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FACE);
        sxgui_draw_raised_edge(painter, rect);
        if (cell->mark == MINES_MARK_FLAG)
        {
            if (lost && cell->mine == 0u)
            {
                paint_wrong_flag(painter, rect);
            }
            else
            {
                paint_flag(painter, rect);
            }
        }
        else if (cell->mark == MINES_MARK_QUESTION)
        {
            paint_question(painter, rect);
        }
        return;
    }

    /* Celda descubierta: plana, con la linea de grilla arriba y a la izquierda.
     * Es el reverso del bisel de la tapada -- por eso el tablero se lee como una
     * superficie hundida y no como una cuadricula dibujada. */
    sx_painter_fill_rect(painter, rect, cell->exploded != 0u ? MINES_COLOUR_EXPLODED : SXGUI_COLOR_FACE);
    sx_painter_hline(painter, rect.x, rect.y, rect.width, SXGUI_COLOR_SHADOW);
    sx_painter_vline(painter, rect.x, rect.y, rect.height, SXGUI_COLOR_SHADOW);

    if (cell->mine != 0u)
    {
        paint_mine(painter, rect);
        return;
    }
    if (cell->neighbours != 0u)
    {
        paint_number(painter, rect, (int)cell->neighbours);
    }
}

static void paint_cursor(struct sx_painter *painter)
{
    struct sx_brush brush = sx_brush_pattern_transparent(sx_pattern_checker_50, SXGUI_COLOR_TEXT);
    struct sx_rect rect = cell_rect(g_cursor_column, g_cursor_row);

    sx_painter_draw_frame_brush(painter, sx_rect_make(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2),
                                &brush);
}

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;
    struct sx_rect status = status_rect();
    struct sx_rect grid = grid_rect();
    int digits_y = status.y + ((MINES_STATUS_HEIGHT - MINES_DIGIT_HEIGHT) / 2);
    int face_offset = (MINES_FACE_BUTTON->pressed && MINES_FACE_BUTTON->hover) ? 1 : 0;
    int row;

    /* Panel de estado y tablero: los dos hundidos sobre la cara de la ventana,
     * como el original. */
    sxgui_draw_sunken_edge(painter, status);
    paint_counter(painter, status.x + MINES_STATUS_PAD + SXGUI_BORDER_SUNKEN, digits_y,
                  mines_board_remaining_mines(&g_board));
    paint_counter(painter,
                  status.x + status.width - MINES_STATUS_PAD - SXGUI_BORDER_SUNKEN - MINES_COUNTER_WIDTH,
                  digits_y,
                  g_elapsed_seconds);
    /* El bisel del boton lo pinto el toolkit; aca va solo la cara. */
    paint_face(painter, face_rect(), mines_face_state(), face_offset);

    sxgui_draw_sunken_edge(painter, grid);
    for (row = 0; row < g_board.rows; ++row)
    {
        int column;

        for (column = 0; column < g_board.columns; ++column)
        {
            paint_cell(painter, column, row);
        }
    }
    if (g_cursor_visible)
    {
        paint_cursor(painter);
    }
}

/* --- entrada -------------------------------------------------------------- */

static int toolkit_owns_input(void)
{
    return sxgui_dialog_active(&g_app.ui) || g_menubar.open_menu >= 0;
}

static void after_play(void)
{
    if (g_board.state == MINES_STATE_PLAYING && g_start_ms == 0ul)
    {
        g_start_ms = uptime_ms();
        g_elapsed_seconds = 0;
    }
    if (g_board.state == MINES_STATE_WON)
    {
        if (finish_game())
        {
            char headline[64];

            snprintf(headline, sizeof(headline), "New best time for %s!",
                     mines_level_info(g_board.level)->name);
            open_times_dialog(headline);
        }
    }
}

static int on_pointer(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event)
{
    uint32_t buttons = event->buttons;
    uint32_t before = g_last_buttons;
    int left = (buttons & SAVANXP_MOUSE_BUTTON_LEFT) != 0u;
    int right = (buttons & SAVANXP_MOUSE_BUTTON_RIGHT) != 0u;
    int middle = (buttons & SAVANXP_MOUSE_BUTTON_MIDDLE) != 0u;
    int left_before = (before & SAVANXP_MOUSE_BUTTON_LEFT) != 0u;
    int right_before = (before & SAVANXP_MOUSE_BUTTON_RIGHT) != 0u;
    int middle_before = (before & SAVANXP_MOUSE_BUTTON_MIDDLE) != 0u;
    int column = 0;
    int row = 0;
    int on_face;
    int on_grid;
    int changed = 0;
    int pressing;

    (void)app;
    /* Ceder a la barra de menu, al menu abierto y al dialogo modal. */
    if (toolkit_owns_input())
    {
        g_last_buttons = buttons;
        return 0;
    }

    on_face = sx_rect_contains_point(face_rect(), event->x, event->y);
    if (left && !left_before)
    {
        g_press_origin = on_face ? MINES_PRESS_FACE : MINES_PRESS_BOARD;
    }
    else if (!left)
    {
        g_press_origin = MINES_PRESS_NONE;
    }

    if (on_face)
    {
        int dragged_in = left && g_press_origin != MINES_PRESS_FACE;

        g_last_buttons = buttons;
        if (g_pressing)
        {
            g_pressing = 0;
            changed = 1;
        }
        /* La cara es un widget: el evento pasa al toolkit, que le da el hundido
         * y el click. Lo unico que NO pasa es el apriete que venia de afuera --
         * ahi el toolkit veria un flanco de apretado que nunca ocurrio sobre el
         * boton, y soltar encima empezaria una partida. Consumirlo es la forma
         * de que no lo vea. */
        if (dragged_in)
        {
            return 1;
        }
        return changed;
    }

    on_grid = cell_at_point(event->x, event->y, &column, &row);
    pressing = (left && on_grid) ? 1 : 0;
    if (pressing != g_pressing)
    {
        g_pressing = pressing;
        changed = 1;
    }
    if (!on_grid)
    {
        g_last_buttons = buttons;
        return changed;
    }

    /* Los dos botones juntos son el acorde, igual que el boton del medio. Se
     * mira ANTES que el click izquierdo: si no, el primero de los dos ya
     * descubrio la celda y el acorde llega tarde. */
    if (left && right)
    {
        if (!(left_before && right_before))
        {
            if (mines_board_chord(&g_board, column, row))
            {
                after_play();
                changed = 1;
            }
        }
    }
    else if (middle && !middle_before)
    {
        if (mines_board_chord(&g_board, column, row))
        {
            after_play();
            changed = 1;
        }
    }
    else if (left && !left_before && !right_before)
    {
        if (mines_board_reveal(&g_board, column, row))
        {
            after_play();
            changed = 1;
        }
    }
    else if (right && !right_before && !left_before)
    {
        if (mines_board_cycle_mark(&g_board, column, row))
        {
            changed = 1;
        }
    }

    /* El cursor de teclado sigue al mouse: si despues se usan las flechas,
     * arrancan desde donde estaba la mano. */
    g_cursor_column = column;
    g_cursor_row = row;

    g_last_buttons = buttons;
    return changed;
}

static int move_cursor(int dx, int dy)
{
    int column = g_cursor_column + dx;
    int row = g_cursor_row + dy;

    if (!g_cursor_visible)
    {
        /* La primera flecha solo muestra el cursor donde esta: mover y aparecer
         * a la vez hace perder de vista donde arranco. */
        g_cursor_visible = 1;
        return 1;
    }
    if (column < 0 || row < 0 || column >= g_board.columns || row >= g_board.rows)
    {
        return 0;
    }
    g_cursor_column = column;
    g_cursor_row = row;
    return 1;
}

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || toolkit_owns_input())
    {
        return 0;
    }

    if (event->key == SAVANXP_KEY_F2)
    {
        start_new_game(g_board.level);
        return 1;
    }
    if (event->key == SAVANXP_KEY_LEFT)
    {
        return move_cursor(-1, 0);
    }
    if (event->key == SAVANXP_KEY_RIGHT)
    {
        return move_cursor(1, 0);
    }
    if (event->key == SAVANXP_KEY_UP)
    {
        return move_cursor(0, -1);
    }
    if (event->key == SAVANXP_KEY_DOWN)
    {
        return move_cursor(0, 1);
    }
    if (!g_cursor_visible)
    {
        /* Sin cursor a la vista, el resto de las teclas son del toolkit: Enter
         * sobre la cara tiene que seguir empezando una partida. */
        return 0;
    }
    if (event->key == SAVANXP_KEY_ENTER || event->ascii == ' ')
    {
        if (mines_board_reveal(&g_board, g_cursor_column, g_cursor_row))
        {
            after_play();
        }
        return 1;
    }
    if (event->ascii == 'f' || event->ascii == 'F')
    {
        (void)mines_board_cycle_mark(&g_board, g_cursor_column, g_cursor_row);
        return 1;
    }
    return 0;
}

/* El reloj se mira entre frames, asi que el segundo que se muestra puede llegar
 * hasta un tick tarde. 250 ms es el compromiso: no se nota y no despierta el
 * loop diez veces por segundo para nada. */
static void on_tick(struct sxgui_app *app)
{
    int seconds;

    if (!timer_running() || g_start_ms == 0ul)
    {
        return;
    }
    seconds = (int)((uptime_ms() - g_start_ms) / 1000ul);
    if (seconds > MINES_COUNTER_MAX)
    {
        seconds = MINES_COUNTER_MAX;
    }
    if (seconds != g_elapsed_seconds)
    {
        g_elapsed_seconds = seconds;
        sxgui_app_request_repaint(app);
    }
}

/* --- menu y dialogos ------------------------------------------------------ */

static void on_face_clicked(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    start_new_game(g_board.level);
    sxgui_app_request_repaint(&g_app);
}

static void on_times_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void on_times_reset(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    mines_scores_reset();
    refresh_times_lines();
    snprintf(g_times_headline, sizeof(g_times_headline), "Fastest times, in seconds:");
}

static void on_about_ok(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
}

static void change_level(int level)
{
    if (level == g_board.level)
    {
        /* Elegir el nivel en el que ya se esta igual empieza de nuevo: es lo que
         * hace el original y lo que espera quien lo usa como "reiniciar". */
        start_new_game(level);
        return;
    }
    start_new_game(level);
    (void)sxgui_app_set_content_size(
        &g_app,
        (MINES_MARGIN * 2) + panel_width(),
        content_top() + (MINES_MARGIN * 2) + panel_height());
    mines_relayout();
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case MINES_CMD_NEW:
        start_new_game(g_board.level);
        break;
    case MINES_CMD_BEGINNER:
        change_level(MINES_LEVEL_BEGINNER);
        break;
    case MINES_CMD_INTERMEDIATE:
        change_level(MINES_LEVEL_INTERMEDIATE);
        break;
    case MINES_CMD_EXPERT:
        change_level(MINES_LEVEL_EXPERT);
        break;
    case MINES_CMD_MARKS:
        g_board.marks_enabled = g_board.marks_enabled ? 0 : 1;
        refresh_menu_checks();
        break;
    case MINES_CMD_BEST_TIMES:
        open_times_dialog(0);
        break;
    case MINES_CMD_ABOUT:
        sxgui_dialog_begin(&g_app.ui, &g_about_dialog, MINES_ABOUT_WIDTH, MINES_ABOUT_HEIGHT);
        break;
    case MINES_CMD_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    default:
        break;
    }
    sxgui_app_request_repaint(&g_app);
}

/* --- layout --------------------------------------------------------------- */

static void mines_relayout(void)
{
    MINES_FACE_BUTTON->rect = face_rect();
}

static void on_resize(struct sxgui_app *app)
{
    (void)app;
    mines_relayout();
}

/* --- arranque ------------------------------------------------------------- */

static void build_dialogs(void)
{
    int index;
    int text_width = MINES_TIMES_WIDTH - (MINES_DLG_MARGIN * 2);

    g_times_widgets[0] = sxgui_label(
        sx_rect_make(MINES_DLG_MARGIN, MINES_DLG_MARGIN, text_width, MINES_DLG_ROW),
        g_times_headline);
    for (index = 0; index < MINES_LEVEL_COUNT; ++index)
    {
        int row_y = MINES_DLG_MARGIN + (MINES_DLG_ROW * (index + 1));

        g_times_widgets[1 + index] = sxgui_label(
            sx_rect_make(MINES_DLG_MARGIN, row_y, MINES_TIMES_NAME_COLUMN, MINES_DLG_ROW),
            mines_level_info(index)->name);
        g_times_widgets[1 + MINES_LEVEL_COUNT + index] = sxgui_label(
            sx_rect_make(MINES_DLG_MARGIN + MINES_TIMES_NAME_COLUMN, row_y,
                         text_width - MINES_TIMES_NAME_COLUMN, MINES_DLG_ROW),
            g_times_values[index]);
    }
    g_times_widgets[7] = sxgui_button(
        sx_rect_make(MINES_DLG_CENTRED(MINES_TIMES_WIDTH, 2, 0), MINES_DLG_BUTTON_ROW(MINES_TIMES_HEIGHT),
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Reset", on_times_reset, 0);
    g_times_widgets[8] = sxgui_button(
        sx_rect_make(MINES_DLG_CENTRED(MINES_TIMES_WIDTH, 2, 1), MINES_DLG_BUTTON_ROW(MINES_TIMES_HEIGHT),
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_times_ok, 0);
    g_times_dialog.title = "Best Times";
    g_times_dialog.widgets = g_times_widgets;
    g_times_dialog.widget_count = (int)(sizeof(g_times_widgets) / sizeof(g_times_widgets[0]));
    g_times_dialog.default_button = 8;

    text_width = MINES_ABOUT_WIDTH - (MINES_DLG_MARGIN * 2);
    g_about_widgets[0] = sxgui_label(
        sx_rect_make(MINES_DLG_MARGIN, MINES_DLG_MARGIN, text_width, MINES_DLG_ROW),
        "SavanXP Minesweeper");
    g_about_widgets[1] = sxgui_label(
        sx_rect_make(MINES_DLG_MARGIN, MINES_DLG_MARGIN + MINES_DLG_ROW, text_width, MINES_DLG_ROW),
        "Version: " SAVANXP_VERSION_STRING);
    g_about_widgets[2] = sxgui_label(
        sx_rect_make(MINES_DLG_MARGIN, MINES_DLG_MARGIN + (MINES_DLG_ROW * 2), text_width, MINES_DLG_ROW),
        "Left click clears   Right click flags");
    g_about_widgets[3] = sxgui_label(
        sx_rect_make(MINES_DLG_MARGIN, MINES_DLG_MARGIN + (MINES_DLG_ROW * 3), text_width, MINES_DLG_ROW),
        "Both buttons chord   F2 starts a new game");
    g_about_widgets[4] = sxgui_label(
        sx_rect_make(MINES_DLG_MARGIN, MINES_DLG_MARGIN + (MINES_DLG_ROW * 4), text_width, MINES_DLG_ROW),
        "Arrows move   Enter clears   F flags");
    g_about_widgets[5] = sxgui_button(
        sx_rect_make(MINES_DLG_CENTRED(MINES_ABOUT_WIDTH, 1, 0), MINES_DLG_BUTTON_ROW(MINES_ABOUT_HEIGHT),
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "OK", on_about_ok, 0);
    g_about_dialog.title = "About Minesweeper";
    g_about_dialog.widgets = g_about_widgets;
    g_about_dialog.widget_count = (int)(sizeof(g_about_widgets) / sizeof(g_about_widgets[0]));
    g_about_dialog.default_button = 5;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return mines_board_selftest();
    }

    mines_random_seed_from_clock();
    mines_scores_load();
    start_new_game(MINES_LEVEL_BEGINNER);
    refresh_times_lines();
    snprintf(g_times_headline, sizeof(g_times_headline), "Fastest times, in seconds:");

    /* La cara es un boton sin rotulo: el bisel, el hundido al apretarlo y el
     * click los da el toolkit, y el dibujo lo pone on_paint encima. */
    *MINES_FACE_BUTTON = sxgui_button(sx_rect_make(0, 0, MINES_FACE_SIZE, MINES_FACE_SIZE), "",
                                      on_face_clicked, 0);
    build_dialogs();

    if (sxgui_app_init(&g_app, "mines", g_widgets, 1) < 0)
    {
        return 1;
    }
    sxgui_set_menubar(&g_app.ui, &g_menubar);
    (void)sxgui_app_set_content_size(
        &g_app,
        (MINES_MARGIN * 2) + panel_width(),
        content_top() + (MINES_MARGIN * 2) + panel_height());
    mines_relayout();

    g_app.on_paint = on_paint;
    g_app.on_pointer = on_pointer;
    g_app.on_key = on_key;
    g_app.on_resize = on_resize;
    g_app.on_tick = on_tick;
    g_app.tick_interval_ms = 250ul;
    return sxgui_app_run(&g_app);
}
