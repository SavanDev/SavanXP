#include "libc.h"
#include "savanxp/sxgui.h"

#include "progman_registry.h"
#include "desktop_wallpaper.h"

#include <stdio.h>
#include <unistd.h>

/*
 * Program Manager (A2.3, ver docs/WM_SUBSYSTEM.md).
 *
 * Cliente top-level normal del WM: no tiene z-rol especial ni extension de
 * protocolo -- recibe input por el mismo camino que cualquier app. Muestra los
 * grupos del registro (progman_registry) como pestanias y los programas del
 * grupo activo como grid de iconos; doble click o Enter lanzan via
 * gfx_desktop_launch_ex, pasando los launch flags que declara el registro.
 *
 * Version "proto": un grupo a la vez con pestanias. Los grupos como ventanas
 * hijas MDI con su propia barra de titulo, minimizables a icono -- el Progman
 * real -- necesitan una primitiva de child window que sxgui todavia no tiene:
 * es trabajo de Fase B.
 */

#define PROGMAN_STATUS_HEIGHT 22
#define PROGMAN_CELL_WIDTH 96
#define PROGMAN_ICON_SIZE 32
#define PROGMAN_GRID_MARGIN 8
/* Aire arriba del icono, entre el icono y el rotulo, y debajo del rotulo. */
#define PROGMAN_CELL_PAD_TOP 8
#define PROGMAN_CELL_GAP 6
#define PROGMAN_CELL_PAD_BOTTOM 6
/*
 * El rotulo se parte en hasta DOS lineas, como en el Program Manager real y
 * como en cualquier grilla de iconos de la epoca. Con una sola linea centrada
 * un nombre largo ("Add or Remove Programs") no entraba y el recorte se lo
 * comia por LOS DOS LADOS -- quedaba "or Remove Prog", que no se lee como un
 * texto cortado sino como uno roto.
 *
 * Dos y no mas: tres lineas de 96 px de ancho es una celda mas alta que ancha,
 * y a partir de ahi lo que hace falta no es mas alto sino un nombre mas corto.
 * Lo que no entra en dos lineas se corta con puntos suspensivos, que SI se lee
 * como un texto cortado.
 */
#define PROGMAN_LABEL_LINES 2
/* Margen lateral del rotulo dentro de la celda: sin esto dos nombres largos de
 * celdas vecinas se tocan y parecen uno solo. */
#define PROGMAN_LABEL_MARGIN 4
#define PROGMAN_LABEL_CAPACITY PROGMAN_NAME_CAPACITY
/* La ventana reserva una grilla de este tamano aunque haya menos programas: es
 * el launcher de la sesion, no un dialogo -- ajustarla a los tres iconos que
 * hay hoy la dejaria ridicula y obligaria a redimensionarla a mano en cuanto
 * se agregue algo al registro. Si un grupo pasa de COLUMNS x MIN_ROWS items,
 * crece en filas. */
#define PROGMAN_GRID_COLUMNS 6
#define PROGMAN_MIN_GRID_ROWS 4
#define PROGMAN_DOUBLE_CLICK_MS 450UL

static struct sxgui_app g_app;
/* Unico widget: el control de pestanias. La grilla de iconos NO son widgets --
 * progman la pinta en on_paint y hace su propio hit-testing --, pero las
 * pestanias si lo son: el dibujo y el click de una pestania son iguales en
 * cualquier app, y tenerlos a mano aca era tener una copia peor. */
static struct sxgui_widget g_widgets[1];
#define PROGMAN_TABS (&g_widgets[0])

/* Los rotulos apuntan al nombre que vive en el registro; el widget guarda el
 * puntero, asi que la tabla se rearma cada vez que el registro cambia. */
static const char *g_group_labels[PROGMAN_MAX_GROUPS];

/* --- barra de menu -------------------------------------------------------
 *
 * El Program Manager real tenia las acciones de sesion en su menu File: en
 * NT 3.5 apagar el sistema se hacia desde ahi, no desde una barra de tareas.
 * Con el chrome Win95 retirado (A2.4c) este es el unico camino a esas acciones.
 */

enum progman_command
{
    PROGMAN_CMD_SHUTDOWN = 1,
    PROGMAN_CMD_REBOOT,
    PROGMAN_CMD_EXIT,
    PROGMAN_CMD_REFRESH,
    PROGMAN_CMD_WALLPAPER,
    PROGMAN_CMD_ABOUT,
};

static const struct sxgui_menu_item k_file_items[] = {
    {"Actualizar\tF5", PROGMAN_CMD_REFRESH, 0},
    {0, 0, 0},
    {"Apagar...", PROGMAN_CMD_SHUTDOWN, 0},
    {"Reiniciar...", PROGMAN_CMD_REBOOT, 0},
    {0, 0, 0},
    {"Salir", PROGMAN_CMD_EXIT, 0},
};

/* El Progman real no cambiaba el fondo (eso era el Control Panel), pero no
 * tenemos uno: Options es el hogar razonable mientras tanto. */
static const struct sxgui_menu_item k_options_items[] = {
    {"Cambiar fondo", PROGMAN_CMD_WALLPAPER, 0},
};

static const struct sxgui_menu_item k_help_items[] = {
    {"Acerca de SavanXP...", PROGMAN_CMD_ABOUT, 0},
};

static const struct sxgui_menu k_menus[] = {
    {"File", k_file_items, (int)(sizeof(k_file_items) / sizeof(k_file_items[0]))},
    {"Options", k_options_items, (int)(sizeof(k_options_items) / sizeof(k_options_items[0]))},
    {"Help", k_help_items, (int)(sizeof(k_help_items) / sizeof(k_help_items[0]))},
};

static struct sxgui_menubar g_menubar;

/* Confirmacion modal de las acciones de energia: apagar por accidente desde un
 * menu seria facil, asi que se pregunta igual que hacia el escritorio. */
static struct sxgui_widget g_dialog_widgets[3];
static struct sxgui_dialog g_dialog;
static int g_pending_power = 0;

/* Definidas abajo, con el resto de lo que toca el disco: las usan el
 * lanzamiento y el menu, que aparecen antes en el archivo. */
static int path_is_launchable(const char *path);
static void refresh_catalog(void);

static int g_group = 0;
static int g_item = 0;
static int g_last_click_item = -1;
static unsigned long g_last_click_ms = 0;
static uint32_t g_last_buttons = 0;
static char g_status[192];

static int group_item_count(int group_index)
{
    const struct progman_group *group = progman_group_at(group_index);
    return group != 0 ? group->item_count : 0;
}

/* Todo el contenido propio arranca debajo de la barra de menu. */
static int content_top(void)
{
    return sxgui_menubar_height();
}

/*
 * Alto de celda, calculado y no clavado: sale del alto de linea de la fuente
 * ACTIVA (gfx_text_height), que no es una constante que este archivo pueda
 * incluir. Clavar un 76 fue lo que dejo el rotulo sin lugar para la segunda
 * linea el dia que hubo un nombre largo.
 */
static int cell_height(void)
{
    return PROGMAN_CELL_PAD_TOP + PROGMAN_ICON_SIZE + PROGMAN_CELL_GAP +
        (PROGMAN_LABEL_LINES * gfx_text_height()) + PROGMAN_CELL_PAD_BOTTOM;
}

static int grid_columns(void)
{
    int usable = (int)g_app.gfx.info.width - (2 * SXGUI_BORDER_RAISED) - (2 * PROGMAN_GRID_MARGIN);
    int columns = usable / PROGMAN_CELL_WIDTH;

    return columns > 0 ? columns : 1;
}

/* Area util de la pestania activa: es donde entra la grilla. */
static struct sx_rect page_rect(void)
{
    return sxgui_tabs_page(PROGMAN_TABS);
}

static struct sx_rect cell_rect(int item_index)
{
    struct sx_rect page = page_rect();
    int columns = grid_columns();
    int column = item_index % columns;
    int row = item_index / columns;

    return sx_rect_make(
        page.x + PROGMAN_GRID_MARGIN + (column * PROGMAN_CELL_WIDTH),
        page.y + PROGMAN_GRID_MARGIN + (row * cell_height()),
        PROGMAN_CELL_WIDTH,
        cell_height());
}

static void set_status_for_selection(void)
{
    const struct progman_item *item = progman_group_item_at(g_group, g_item);

    if (item == 0)
    {
        snprintf(g_status, sizeof(g_status), "Grupo vacio");
        return;
    }
    if (item->description[0] != '\0')
    {
        snprintf(g_status, sizeof(g_status), "%s  -  %s", item->description, item->path);
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "%s", item->path);
    }
}

static void select_group(int group_index)
{
    if (group_index < 0 || group_index >= progman_group_count())
    {
        return;
    }
    g_group = group_index;
    /* El widget es la fuente de verdad para el DIBUJO, asi que hay que moverlo
     * tambien cuando el cambio viene por teclado y no por click. Ponerlo aca y
     * no en el callback cubre los dos caminos con una sola linea. */
    PROGMAN_TABS->value = group_index;
    g_item = 0;
    g_last_click_item = -1;
    set_status_for_selection();
}

/* Callback del control: el toolkit ya movio `value` cuando llega aca. */
static void on_tab_changed(struct sxgui_widget *widget, void *user)
{
    (void)user;
    select_group(widget->value);
}

static void launch_selected(void)
{
    const struct progman_item *item = progman_group_item_at(g_group, g_item);

    if (item == 0)
    {
        return;
    }

    /*
     * El catalogo se arma al arrancar y no se entera solo de lo que pasa
     * despues: entre medio alguien pudo desinstalar esto con appwiz. Y el WM
     * NO valida el path -- gfx_desktop_launch_ex escribe el pedido en un fd y
     * devuelve 0 sin mirar si el archivo existe --, asi que sin este chequeo
     * el item muerto se queda en la grilla contestando "Lanzando ..." y no
     * pasa nada, que es exactamente el sintoma peor: silencio.
     *
     * Se aprovecha para actualizar: el que hizo clic ya descubrio que el
     * catalogo esta viejo, no tiene sentido hacerlo apretar F5 ademas.
     */
    if (!path_is_launchable(item->path))
    {
        char name[PROGMAN_NAME_CAPACITY];

        /* Copiado antes: refresh_catalog rearma el registro y `item` deja de
         * ser valido. */
        snprintf(name, sizeof(name), "%s", item->name);
        refresh_catalog();
        snprintf(g_status, sizeof(g_status), "%s ya no esta instalado", name);
        return;
    }

    /* Los launch flags salen del registro: el WM no conoce el catalogo (A2.3a). */
    if (gfx_desktop_launch_ex(&g_app.gfx, item->path, item->launch_flags) < 0)
    {
        snprintf(g_status, sizeof(g_status), "No se pudo lanzar %s", item->path);
    }
    else
    {
        snprintf(g_status, sizeof(g_status), "Lanzando %s...", item->name);
    }
}

/* --- rotulo de la celda ---------------------------------------------------- */

/*
 * Largo del prefijo mas largo de `text` que entra en `width`.
 *
 * Se mide de verdad con la fuente y no se estima por cantidad de caracteres:
 * la fuente es proporcional, asi que "WWW" y "iii" ocupan cosas muy distintas
 * con los mismos tres caracteres. Es O(n) llamadas a gfx_text_width con n <= 31
 * (PROGMAN_NAME_CAPACITY), y solo corre al repintar, que es por evento y no por
 * cuadro.
 */
static size_t prefix_that_fits(const char *text, int width, size_t max_bytes)
{
    char probe[PROGMAN_LABEL_CAPACITY];
    size_t length;
    size_t best = 0;
    size_t index;

    if (text == 0 || width <= 0)
    {
        return 0;
    }
    if (max_bytes > sizeof(probe) - 1u)
    {
        max_bytes = sizeof(probe) - 1u;
    }
    length = strlen(text);
    if (length > max_bytes)
    {
        length = max_bytes;
    }
    for (index = 1; index <= length; ++index)
    {
        memcpy(probe, text, index);
        probe[index] = '\0';
        if (gfx_text_width(probe) > width)
        {
            break;
        }
        best = index;
    }
    return best;
}

static void copy_line(char *destination, const char *source, size_t length)
{
    /* Sin espacios colgando al final: como la linea va centrada, un espacio de
     * mas la corre medio caracter y se nota al lado de la de arriba. */
    while (length > 0u && source[length - 1u] == ' ')
    {
        length -= 1u;
    }
    if (length > PROGMAN_LABEL_CAPACITY - 1u)
    {
        length = PROGMAN_LABEL_CAPACITY - 1u;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

/* Ultima linea con texto de sobra: se corta con puntos suspensivos, que SI se
 * leen como un texto cortado. */
static void ellipsise_line(char *destination, const char *text, int width)
{
    static const char dots[] = "...";
    size_t take = prefix_that_fits(text, width - gfx_text_width(dots),
                                   PROGMAN_LABEL_CAPACITY - sizeof(dots));

    while (take > 0u && text[take - 1u] == ' ')
    {
        take -= 1u;
    }
    memcpy(destination, text, take);
    memcpy(destination + take, dots, sizeof(dots));
}

/*
 * Parte `text` en hasta PROGMAN_LABEL_LINES lineas que entren en `width`.
 * Devuelve cuantas escribio.
 *
 * Corta por espacios; una palabra sola mas ancha que la celda se parte por
 * caracter, porque la alternativa -- dejarla desbordar -- es el problema que
 * esto vino a arreglar. Sin malloc: el array lo trae el llamador.
 */
static int wrap_label(const char *text, int width, char lines[PROGMAN_LABEL_LINES][PROGMAN_LABEL_CAPACITY])
{
    size_t offset = 0;
    size_t length;
    int used = 0;

    if (text == 0 || text[0] == '\0')
    {
        return 0;
    }
    length = strlen(text);

    while (used < PROGMAN_LABEL_LINES && offset < length)
    {
        const char *remaining = text + offset;
        size_t left = length - offset;
        size_t fits = prefix_that_fits(remaining, width, PROGMAN_LABEL_CAPACITY - 1u);
        size_t take;

        if (fits >= left)
        {
            copy_line(lines[used], remaining, left);
            used += 1;
            break;
        }
        if (used == PROGMAN_LABEL_LINES - 1)
        {
            ellipsise_line(lines[used], remaining, width);
            used += 1;
            break;
        }

        take = fits;
        if (remaining[fits] != ' ')
        {
            /* El corte cayo en medio de una palabra: retroceder hasta donde
             * empieza, y dejarla entera para la linea siguiente. */
            size_t scan = fits;

            while (scan > 0u && remaining[scan - 1u] != ' ')
            {
                scan -= 1u;
            }
            /* scan == 0 significa que la palabra sola no entra en la celda: no
             * hay donde cortar sin partirla, asi que se parte. */
            if (scan > 0u)
            {
                take = scan - 1u;
            }
        }
        if (take == 0u)
        {
            take = fits;
        }

        copy_line(lines[used], remaining, take);
        used += 1;
        offset += take;
        while (offset < length && text[offset] == ' ')
        {
            offset += 1u;
        }
    }
    return used;
}

/* --- pintado ------------------------------------------------------------- */

static void fill_embedded_bitmap_info(const struct desktop_embedded_bitmap *source, struct savanxp_fb_info *info)
{
    memset(info, 0, sizeof(*info));
    info->width = source->width;
    info->height = source->height;
    info->pitch = source->width * (uint32_t)sizeof(uint32_t);
    info->bpp = 32u;
    info->buffer_size = info->pitch * info->height;
}

static void draw_icon_scaled(struct sx_painter *painter, const struct desktop_embedded_bitmap *source, int x, int y, int size)
{
    struct sx_bitmap bitmap;
    struct savanxp_fb_info info;

    if (source == 0 || source->pixels == 0 || source->width == 0 || source->height == 0)
    {
        return;
    }
    fill_embedded_bitmap_info(source, &info);
    sx_bitmap_wrap(&bitmap, (uint32_t *)source->pixels, &info, SX_PIXEL_FORMAT_BGRA8888);
    sx_painter_draw_scaled_bitmap_nearest(
        painter,
        &bitmap,
        sx_rect_make(x, y, size, size),
        sx_rect_make(0, 0, (int)source->width, (int)source->height));
}

static void paint_items(struct sx_painter *painter)
{
    int count = group_item_count(g_group);
    int index;

    for (index = 0; index < count; ++index)
    {
        const struct progman_item *item = progman_group_item_at(g_group, index);
        const struct desktop_embedded_bitmap *icon = 0;
        struct sx_rect rect = cell_rect(index);
        int selected = (index == g_item);
        int label_x;

        if (item == 0)
        {
            continue;
        }
        /* No dibujar celdas que caen fuera del area util. */
        if (rect.y + rect.height > (int)g_app.gfx.info.height - PROGMAN_STATUS_HEIGHT)
        {
            break;
        }

        if (selected)
        {
            sx_painter_fill_rect(painter, rect, SXGUI_COLOR_SELECT);
        }
        /* El icono del propio binario manda; el set horneado es el fallback
         * para lo que todavia no trae recursos (docs/SXE_FORMAT.md). */
        icon = progman_item_icon(item);
        if (icon == 0)
        {
            icon = desktop_icon_large((enum desktop_icon_id)item->icon_id);
        }
        draw_icon_scaled(
            painter,
            icon,
            rect.x + ((rect.width - PROGMAN_ICON_SIZE) / 2),
            rect.y + PROGMAN_CELL_PAD_TOP,
            PROGMAN_ICON_SIZE);

        /* El nombre se parte en hasta dos lineas centradas. El clip a la celda
         * se mantiene igual: es la red de seguridad de que nada se derrame a la
         * celda de al lado, no el mecanismo. */
        {
            char lines[PROGMAN_LABEL_LINES][PROGMAN_LABEL_CAPACITY];
            int label_width = PROGMAN_CELL_WIDTH - (2 * PROGMAN_LABEL_MARGIN);
            int count = wrap_label(item->name, label_width, lines);
            int label_y = rect.y + PROGMAN_CELL_PAD_TOP + PROGMAN_ICON_SIZE + PROGMAN_CELL_GAP;
            int line;

            if (sx_painter_push_clip(painter, rect))
            {
                for (line = 0; line < count; ++line)
                {
                    label_x = rect.x + ((rect.width - gfx_text_width(lines[line])) / 2);
                    sx_painter_draw_text(
                        painter,
                        label_x,
                        label_y + (line * gfx_text_height()),
                        lines[line],
                        selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT);
                }
                sx_painter_pop_clip(painter);
            }
        }
    }
}

static void paint_status(struct sx_painter *painter)
{
    int y = (int)g_app.gfx.info.height - PROGMAN_STATUS_HEIGHT;
    struct sx_rect rect = sx_rect_make(0, y, (int)g_app.gfx.info.width, PROGMAN_STATUS_HEIGHT);

    sx_painter_fill_rect(painter, rect, SXGUI_COLOR_FACE);
    sx_painter_fill_rect(painter, sx_rect_make(0, y, rect.width, 1), SXGUI_COLOR_SHADOW);
    if (sx_painter_push_clip(painter, rect))
    {
        sx_painter_draw_text(painter, 6, y + ((PROGMAN_STATUS_HEIGHT - gfx_text_height()) / 2), g_status, SXGUI_COLOR_TEXT);
        sx_painter_pop_clip(painter);
    }
}

/* El control de pestanias -- y con el, el fondo de la pagina -- lo pinto el
 * toolkit antes de llegar aca; on_paint pone encima lo que es propio de esta
 * app: la grilla de iconos adentro de la pagina, y la barra de estado. */
static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;

    paint_items(painter);
    paint_status(painter);
}

/* --- menu ---------------------------------------------------------------- */

static void on_power_confirm(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 1);
    /* Estas llamadas no retornan si tienen exito. */
    if (g_pending_power == PROGMAN_CMD_SHUTDOWN)
    {
        (void)power_shutdown();
    }
    else if (g_pending_power == PROGMAN_CMD_REBOOT)
    {
        (void)power_reboot();
    }
    snprintf(g_status, sizeof(g_status), "La accion de energia fallo");
}

static void on_power_cancel(struct sxgui_widget *widget, void *user)
{
    (void)widget;
    (void)user;
    sxgui_dialog_end(&g_app.ui, 0);
}

/* Cartelito de pregunta: margen parejo a los cuatro lados y la fila de botones
 * centrada abajo, la misma grilla que usan los dialogos de notepad y files. */
#define PROGMAN_DIALOG_WIDTH 260
#define PROGMAN_DIALOG_HEIGHT 96
#define PROGMAN_DIALOG_BUTTON_ROW (PROGMAN_DIALOG_HEIGHT - SXGUI_DIALOG_MARGIN - SXGUI_BUTTON_HEIGHT)
#define PROGMAN_DIALOG_BUTTON_X(index)     ((PROGMAN_DIALOG_WIDTH - 2 * SXGUI_BUTTON_WIDTH - SXGUI_GAP) / 2 +      (index) * (SXGUI_BUTTON_WIDTH + SXGUI_GAP))

static void ask_power_confirmation(int command)
{
    const char *question = (command == PROGMAN_CMD_REBOOT)
        ? "Reiniciar SavanXP?"
        : "Apagar SavanXP?";

    g_pending_power = command;
    g_dialog_widgets[0] = sxgui_label(
        sx_rect_make(SXGUI_DIALOG_MARGIN, SXGUI_DIALOG_MARGIN,
                     PROGMAN_DIALOG_WIDTH - SXGUI_DIALOG_MARGIN * 2, 18),
        question);
    g_dialog_widgets[1] = sxgui_button(
        sx_rect_make(PROGMAN_DIALOG_BUTTON_X(0), PROGMAN_DIALOG_BUTTON_ROW,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "Si", on_power_confirm, 0);
    g_dialog_widgets[2] = sxgui_button(
        sx_rect_make(PROGMAN_DIALOG_BUTTON_X(1), PROGMAN_DIALOG_BUTTON_ROW,
                     SXGUI_BUTTON_WIDTH, SXGUI_BUTTON_HEIGHT),
        "No", on_power_cancel, 0);

    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.title = (command == PROGMAN_CMD_REBOOT) ? "Reiniciar" : "Apagar";
    /* El default es "No": apagar por Enter de apuro seria justo lo que el
     * dialogo esta tratando de evitar. */
    g_dialog.default_button = 2;
    g_dialog.widgets = g_dialog_widgets;
    g_dialog.widget_count = 3;
    sxgui_dialog_begin(&g_app.ui, &g_dialog, PROGMAN_DIALOG_WIDTH, PROGMAN_DIALOG_HEIGHT);
}

static void on_menu_command(int id, void *user)
{
    (void)user;
    switch (id)
    {
    case PROGMAN_CMD_SHUTDOWN:
    case PROGMAN_CMD_REBOOT:
        ask_power_confirmation(id);
        break;
    case PROGMAN_CMD_EXIT:
        sxgui_app_quit(&g_app, 0);
        break;
    case PROGMAN_CMD_REFRESH:
        refresh_catalog();
        snprintf(g_status, sizeof(g_status), "Catalogo actualizado: %d programa(s)", progman_item_count());
        break;
    case PROGMAN_CMD_WALLPAPER:
    {
        /* Solo persistimos el modo: el fondo lo dibuja shellui, que relee la
         * config y repinta por su cuenta. */
        static const char *k_mode_names[DESKTOP_WALLPAPER_MODE_COUNT] = {
            "teal", "degrade", "patron", "imagen"
        };
        int mode = desktop_wallpaper_cycle_config();

        if (mode >= 0 && mode < DESKTOP_WALLPAPER_MODE_COUNT)
        {
            snprintf(g_status, sizeof(g_status), "Fondo: %s", k_mode_names[mode]);
        }
        break;
    }
    case PROGMAN_CMD_ABOUT:
        if (gfx_desktop_launch_ex(&g_app.gfx, "/bin/aboutapp", SAVANXP_DESKTOP_LAUNCH_FLAG_NONE) < 0)
        {
            snprintf(g_status, sizeof(g_status), "No se pudo lanzar /bin/aboutapp");
        }
        break;
    default:
        break;
    }
}

/* El chrome del toolkit (barra de menu, menu desplegado, dialogo modal) tiene
 * prioridad sobre el hit-testing propio: si es suyo, no consumimos el evento. */
static int toolkit_owns_input(void)
{
    return sxgui_dialog_active(&g_app.ui) || g_menubar.open_menu >= 0;
}

/* --- input --------------------------------------------------------------- */

static int on_key(struct sxgui_app *app, const struct savanxp_input_event *event)
{
    int count = group_item_count(g_group);
    int columns = grid_columns();

    (void)app;
    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN || toolkit_owns_input())
    {
        return 0;
    }

    if (event->key == SAVANXP_KEY_F5)
    {
        refresh_catalog();
        snprintf(g_status, sizeof(g_status), "Catalogo actualizado: %d programa(s)", progman_item_count());
        return 1;
    }
    if (event->key == SAVANXP_KEY_TAB)
    {
        select_group((g_group + 1) % (progman_group_count() > 0 ? progman_group_count() : 1));
        return 1;
    }
    if (event->key == SAVANXP_KEY_ENTER)
    {
        launch_selected();
        return 1;
    }
    if (count == 0)
    {
        return 0;
    }
    if (event->key == SAVANXP_KEY_LEFT)
    {
        g_item = (g_item + count - 1) % count;
        set_status_for_selection();
        return 1;
    }
    if (event->key == SAVANXP_KEY_RIGHT)
    {
        g_item = (g_item + 1) % count;
        set_status_for_selection();
        return 1;
    }
    if (event->key == SAVANXP_KEY_UP)
    {
        if (g_item - columns >= 0)
        {
            g_item -= columns;
            set_status_for_selection();
        }
        return 1;
    }
    if (event->key == SAVANXP_KEY_DOWN)
    {
        if (g_item + columns < count)
        {
            g_item += columns;
            set_status_for_selection();
        }
        return 1;
    }
    return 0;
}

static int on_pointer(struct sxgui_app *app, const struct savanxp_gui_pointer_event *event)
{
    uint32_t left = event->buttons & SAVANXP_MOUSE_BUTTON_LEFT;
    uint32_t left_was = g_last_buttons & SAVANXP_MOUSE_BUTTON_LEFT;
    int changed = 0;

    (void)app;
    /* Ceder a la barra de menu / menu abierto / dialogo modal. */
    if (toolkit_owns_input() || event->y < content_top())
    {
        g_last_buttons = event->buttons;
        return 0;
    }
    /* La fila de pestanias es del widget: dejar pasar el evento para que lo
     * despache el toolkit y no duplicar el hit-testing aca. */
    if (event->y < content_top() + sxgui_tabs_height())
    {
        g_last_buttons = event->buttons;
        return 0;
    }
    /* Solo la transicion suelto->apretado cuenta como click. */
    if (left != 0 && left_was == 0)
    {
        int index;

        for (index = 0; index < group_item_count(g_group); ++index)
        {
            if (!sx_rect_contains_point(cell_rect(index), event->x, event->y))
            {
                continue;
            }
            {
                unsigned long now = uptime_ms();
                int double_click = (g_last_click_item == index) &&
                    (now - g_last_click_ms <= PROGMAN_DOUBLE_CLICK_MS);

                g_item = index;
                set_status_for_selection();
                if (double_click)
                {
                    launch_selected();
                    /* Evita que un tercer click encadene otro lanzamiento. */
                    g_last_click_item = -1;
                }
                else
                {
                    g_last_click_item = index;
                    g_last_click_ms = now;
                }
                changed = 1;
            }
            break;
        }
    }

    g_last_buttons = event->buttons;
    return changed;
}

/* El control cubre todo lo que hay entre la barra de menu y la de estado: la
 * fila de pestanias Y la pagina. La grilla se recalcula sola a partir de
 * page_rect(), asi que esto es lo unico que hay que reubicar al cambiar de
 * tamano. */
static void progman_layout(struct sxgui_app *app)
{
    int height = (int)app->gfx.info.height - content_top() - PROGMAN_STATUS_HEIGHT;

    if (height < 0)
    {
        height = 0;
    }
    PROGMAN_TABS->rect = sx_rect_make(0, content_top(), (int)app->gfx.info.width, height);
}

static void on_resize(struct sxgui_app *app)
{
    progman_layout(app);
}

/* Tamano de la ventana: la grilla base (PROGMAN_GRID_COLUMNS x
 * PROGMAN_MIN_GRID_ROWS), estirada en filas si el grupo mas cargado no entra,
 * mas la barra de pestanias -- que tiene que entrar entera o las ultimas
 * quedan fuera de la ventana -- y la de estado. Se calcula aca y no en el WM
 * porque depende del registro, que solo conoce progman. */
static void preferred_content_size(int *width, int *height)
{
    int max_items = 0;
    int tabs_width = sxgui_tabs_preferred_width(g_group_labels, progman_group_count());
    int rows;
    int index;

    for (index = 0; index < progman_group_count(); ++index)
    {
        int count = group_item_count(index);

        if (count > max_items)
        {
            max_items = count;
        }
    }

    rows = (max_items + PROGMAN_GRID_COLUMNS - 1) / PROGMAN_GRID_COLUMNS;
    if (rows < PROGMAN_MIN_GRID_ROWS)
    {
        rows = PROGMAN_MIN_GRID_ROWS;
    }

    *width = (2 * SXGUI_BORDER_RAISED) + (2 * PROGMAN_GRID_MARGIN) +
        (PROGMAN_GRID_COLUMNS * PROGMAN_CELL_WIDTH);
    if (tabs_width > *width)
    {
        *width = tabs_width;
    }
    *height = content_top() + sxgui_tabs_height() + (2 * SXGUI_BORDER_RAISED) +
        (2 * PROGMAN_GRID_MARGIN) + (rows * cell_height()) + PROGMAN_STATUS_HEIGHT;
}

/* Existencia real: si el path no se puede abrir, no se puede lanzar. */
static int path_is_launchable(const char *path)
{
    long fd;

    if (path == 0 || path[0] == '\0')
    {
        return 0;
    }

    fd = savanxp_open(path);
    if (fd < 0)
    {
        return 0;
    }
    (void)savanxp_close((int)fd);
    return 1;
}

/* --- catalogo ------------------------------------------------------------- */

/*
 * Arma el catalogo entero contra el disco. Las cuatro etapas y su orden estan
 * documentadas en progman_registry.h; lo que importa aca es que esto es una
 * FUNCION y no un tramo de main, porque hay que poder repetirla: el catalogo
 * describe el disco, y el disco cambia mientras el launcher esta abierto.
 */
static void build_catalog(void)
{
    /* El .ini es el ARREGLO del usuario, y ya no es lo que hace que un programa
     * exista en el menu: si falta, el registro arranca vacio y lo llena el
     * escaneo. */
    (void)progman_registry_load_file();
    /* Sacar del catalogo lo que no esta instalado: los ports en Haxe y Doom se
     * construyen con builds APARTE, asi que en un arbol limpio sus entradas
     * existen pero no lanzan nada. Aplica igual a los defaults y al .ini. */
    (void)progman_registry_prune_missing(path_is_launchable);
    /* "Todos los programas": todo binario instalado que declare categoria entra
     * aca, sin que nadie lo haya anotado en ningun lado. Va despues del pruning
     * y antes de apply_sxe (ver progman_registry.h). */
    (void)progman_registry_scan_programs(path_is_launchable);
    if (progman_item_count() == 0)
    {
        /* Ni archivo ni escaneo: recien ahora los defaults horneados. Son la
         * red de seguridad para una imagen donde no se puede leer un solo
         * binario, no el catalogo de nadie. */
        progman_registry_load_defaults();
        (void)progman_registry_prune_missing(path_is_launchable);
    }
    /* Recien aca se leen los recursos de cada binario: despues del pruning,
     * para no abrir ejecutables que se van a descartar y porque el pruning
     * reordena los items (docs/SXE_FORMAT.md, fase 3). */
    (void)progman_registry_apply_sxe();
}

/* Rearma la tabla de rotulos contra el registro actual. Los rotulos apuntan
 * DENTRO del registro, asi que hay que rehacerla cada vez que el catalogo
 * cambia -- si no, el widget de solapas dibuja nombres de grupos que ya no
 * existen. */
static void rebuild_group_labels(void)
{
    int index;
    int count = progman_group_count();

    if (count > PROGMAN_MAX_GROUPS)
    {
        count = PROGMAN_MAX_GROUPS;
    }
    for (index = 0; index < count; ++index)
    {
        const struct progman_group *group = progman_group_at(index);
        g_group_labels[index] = (group != 0) ? group->name : "";
    }
    PROGMAN_TABS->items = g_group_labels;
    PROGMAN_TABS->item_count = count;
}

static void refresh_catalog(void)
{
    char previous[PROGMAN_NAME_CAPACITY];
    const struct progman_group *group = progman_group_at(g_group);
    int index;
    int count;

    snprintf(previous, sizeof(previous), "%s", group != 0 ? group->name : "");

    build_catalog();
    rebuild_group_labels();

    /*
     * Volver al grupo por NOMBRE y no por indice. Si el que desaparecio esta
     * antes del actual -- desinstalar el ultimo juego se lleva el grupo Games
     * entero --, el indice viejo apunta a otro grupo y el usuario se encuentra
     * en una solapa que no eligio.
     */
    count = progman_group_count();
    g_group = 0;
    for (index = 0; index < count; ++index)
    {
        const struct progman_group *candidate = progman_group_at(index);

        if (candidate != 0 && strcmp(candidate->name, previous) == 0)
        {
            g_group = index;
            break;
        }
    }
    select_group(g_group);

    /* La ventana NO se redimensiona: al arrancar el tamano sale del catalogo,
     * pero despues es del usuario, y achicarsela abajo de la mano porque
     * desinstalo algo seria peor que dejar un hueco. Solo se reacomoda lo de
     * adentro. */
    progman_layout(&g_app);
    sxgui_app_request_repaint(&g_app);
}

/* --- selftest ------------------------------------------------------------ */

/* Un nombre que no existe en /bin: si existiera, el escaneo lo descartaria por
 * ser un programa del sistema y la fixture no probaria nada. */
#define PROGMAN_FIXTURE_APP "/disk/bin/progman-selftest-app"

static int copy_file(const char *source_path, const char *destination_path)
{
    static char buffer[8192];
    long source = savanxp_open_mode(source_path, SAVANXP_OPEN_READ);
    long destination;
    int result = 0;

    if (source < 0)
    {
        return -1;
    }
    destination = savanxp_open_mode(destination_path,
        SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE);
    if (destination < 0)
    {
        (void)savanxp_close((int)source);
        return -1;
    }
    for (;;)
    {
        long read_bytes = savanxp_read((int)source, buffer, sizeof(buffer));

        if (read_bytes < 0)
        {
            result = -1;
            break;
        }
        if (read_bytes == 0)
        {
            break;
        }
        if (savanxp_write((int)destination, buffer, (size_t)read_bytes) != read_bytes)
        {
            result = -1;
            break;
        }
    }
    (void)savanxp_close((int)source);
    (void)savanxp_close((int)destination);
    return result;
}

static int catalog_has_path(const char *path)
{
    int index;

    for (index = 0; index < progman_item_count(); ++index)
    {
        const struct progman_item *item = progman_item_at(index);

        if (item != 0 && strcmp(item->path, path) == 0)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * El rotulo de dos lineas. Es logica pura -- gfx_text_width no necesita
 * contexto grafico --, asi que se puede ejercitar en el selftest headless, que
 * es donde tiene que estar: el bug original ("or Remove Prog") pasaba todos los
 * smokes porque nadie miraba ese texto.
 */
static int selftest_wrap_label(void)
{
    const int width = PROGMAN_CELL_WIDTH - (2 * PROGMAN_LABEL_MARGIN);
    char lines[PROGMAN_LABEL_LINES][PROGMAN_LABEL_CAPACITY];
    int failures = 0;
    int count;
    int line;

    count = wrap_label("", width, lines);
    if (count != 0)
    {
        printf("PROGMAN SMOKE FAIL wrap: texto vacio dio %d linea(s)\n", count);
        failures += 1;
    }

    /* Un nombre corto no cambia: la mayoria de los programas cae aca y tiene
     * que seguir viendose exactamente igual que antes. */
    count = wrap_label("Shell", width, lines);
    if (count != 1 || strcmp(lines[0], "Shell") != 0)
    {
        printf("PROGMAN SMOKE FAIL wrap: 'Shell' dio %d linea(s) ('%s')\n", count, count > 0 ? lines[0] : "");
        failures += 1;
    }

    /* El caso que motivo todo esto. Tiene que partirse en dos, sin perder una
     * sola letra y sin puntos suspensivos: entra completo en dos lineas. */
    count = wrap_label("Add/Remove Programs", width, lines);
    if (count != 2)
    {
        printf("PROGMAN SMOKE FAIL wrap: el nombre largo dio %d linea(s)\n", count);
        failures += 1;
    }
    else
    {
        char joined[PROGMAN_LABEL_CAPACITY * PROGMAN_LABEL_LINES];

        snprintf(joined, sizeof(joined), "%s %s", lines[0], lines[1]);
        if (strcmp(joined, "Add/Remove Programs") != 0)
        {
            printf("PROGMAN SMOKE FAIL wrap: se perdio texto ('%s' + '%s')\n", lines[0], lines[1]);
            failures += 1;
        }
    }

    /*
     * Una palabra sola, sin ningun espacio donde cortar, y mas larga que las
     * dos lineas juntas. Tiene que partirse igual -- desbordar es el problema
     * que esto vino a arreglar -- y avisar con puntos suspensivos que quedo
     * texto afuera, en vez de recortar en silencio.
     */
    count = wrap_label("Supercalifragilisticoespialidosoyalgomas", width, lines);
    if (count != PROGMAN_LABEL_LINES)
    {
        printf("PROGMAN SMOKE FAIL wrap: la palabra larga dio %d linea(s)\n", count);
        failures += 1;
    }
    else
    {
        size_t length = strlen(lines[PROGMAN_LABEL_LINES - 1]);

        if (length < 3u || strcmp(lines[PROGMAN_LABEL_LINES - 1] + length - 3u, "...") != 0)
        {
            printf("PROGMAN SMOKE FAIL wrap: la ultima linea no avisa que corto ('%s')\n",
                lines[PROGMAN_LABEL_LINES - 1]);
            failures += 1;
        }
    }

    /*
     * Y la garantia que de verdad le importa al que dibuja, sobre TODOS los
     * casos de arriba: ninguna linea se pasa del ancho de la celda. Sin esto el
     * clip sigue tapando el desborde y el rotulo vuelve a verse roto.
     */
    {
        static const char *const k_names[] = {
            "Shell",
            "Add/Remove Programs",
            "Supercalifragilisticoespialidosoyalgomas",
            "Mouse Test",
            "a b c d e f g h i j k l m n o p",
        };
        size_t index;

        for (index = 0; index < sizeof(k_names) / sizeof(k_names[0]); ++index)
        {
            count = wrap_label(k_names[index], width, lines);
            for (line = 0; line < count; ++line)
            {
                if (gfx_text_width(lines[line]) > width)
                {
                    printf("PROGMAN SMOKE FAIL wrap: '%s' -> la linea '%s' no entra (%d > %d)\n",
                        k_names[index], lines[line], gfx_text_width(lines[line]), width);
                    failures += 1;
                }
            }
        }
    }
    return failures;
}

static int progman_selftest(void)
{
    int failures = progman_registry_selftest() + selftest_wrap_label();
    int dropped;
    int applied;
    int index;

    if (failures != 0)
    {
        printf("PROGMAN SMOKE FAIL %d checks\n", failures);
        return 1;
    }

    /* Lo unico que el predicado falso del registro no puede cubrir: que open()
     * distinga de verdad un binario instalado de uno ausente. Sin esto el
     * pruning podria estar bien y aun asi no descartar nada nunca. */
    if (!path_is_launchable("/bin/progman"))
    {
        printf("PROGMAN SMOKE FAIL path_is_launchable con binario instalado\n");
        return 1;
    }
    if (path_is_launchable("/disk/bin/__no_instalado__"))
    {
        printf("PROGMAN SMOKE FAIL path_is_launchable con binario ausente\n");
        return 1;
    }

    progman_registry_load_defaults();
    printf("PROGMAN SMOKE defaults groups=%d items=%d\n",
        progman_group_count(),
        progman_item_count());

    /* Lo unico que el selftest del registro no puede fingir: que hace el
     * pruning contra el disco REAL de esta imagen. Es lo que decide si Doom se
     * muestra o no, porque se construye con un build aparte y en un arbol
     * limpio no esta instalado. */
    dropped = progman_registry_prune_missing(path_is_launchable);
    printf("PROGMAN SMOKE prune dropped=%d\n", dropped);
    for (index = 0; index < progman_group_count(); ++index)
    {
        const struct progman_group *group = progman_group_at(index);
        if (group != 0)
        {
            printf("PROGMAN SMOKE group %s items=%d\n", group->name, group->item_count);
        }
    }

    /* Igual que el pruning: el selftest del registro ejercita la precedencia
     * con fixtures, pero solo aca se corre contra el catalogo REAL de esta
     * imagen. Si ningun item toma recursos, el estampado del build se rompio
     * y el sistema seguiria andando con los defaults sin decir nada. */
    applied = progman_registry_apply_sxe();
    printf("PROGMAN SMOKE sxe applied=%d of %d\n", applied, progman_item_count());
    if (applied <= 0)
    {
        printf("PROGMAN SMOKE FAIL ningun item tomo recursos de su binario\n");
        return 1;
    }
    for (index = 0; index < progman_item_count(); ++index)
    {
        const struct progman_item *item = progman_item_at(index);
        if (item != 0)
        {
            printf("PROGMAN SMOKE item %s icono=%s\n",
                item->name,
                progman_item_icon(item) != 0 ? "propio" : "horneado");
        }
    }

    /*
     * El escaneo, contra los binarios REALES de esta imagen. Se corre sobre un
     * registro vaciado a proposito: lo que interesa medir es lo que el escaneo
     * aporta POR SI SOLO, que es exactamente el caso de una instalacion sin
     * /disk/progman.ini -- el unico que existe hoy en una imagen recien hecha.
     */
    (void)progman_registry_parse("", 0);
    {
        int scanned = progman_registry_scan_programs(path_is_launchable);
        int rescanned;

        printf("PROGMAN SMOKE scan added=%d examined=%d groups=%d\n",
            scanned,
            progman_registry_scan_examined(),
            progman_group_count());
        if (scanned <= 0)
        {
            printf("PROGMAN SMOKE FAIL el escaneo no encontro un solo programa con categoria\n");
            return 1;
        }
        if (progman_registry_source() != PROGMAN_REGISTRY_SOURCE_SCAN)
        {
            printf("PROGMAN SMOKE FAIL el escaneo no se adjudico la fuente del catalogo\n");
            return 1;
        }

        /*
         * Correrlo de nuevo no puede agregar NADA. Es el chequeo del desempate
         * por basename, y no es teorico: /disk/bin es una copia de /bin, asi
         * que la segunda pasada del propio escaneo ya se topa con cada programa
         * del sistema por segunda vez.
         */
        rescanned = progman_registry_scan_programs(path_is_launchable);
        if (rescanned != 0)
        {
            printf("PROGMAN SMOKE FAIL el reescaneo duplico %d item(s)\n", rescanned);
            return 1;
        }

        /*
         * El opt-in, por los dos lados. progman se lanza a si mismo desde el
         * menu si alguien lo anota, pero no declara categoria y por lo tanto no
         * se lista solo; busybox tampoco, y ahi importa de verdad, porque son
         * 30 copias del mismo binario bajo nombres distintos.
         */
        for (index = 0; index < progman_item_count(); ++index)
        {
            const struct progman_item *item = progman_item_at(index);
            if (item == 0)
            {
                continue;
            }
            if (strcmp(item->path, "/bin/progman") == 0 ||
                strcmp(item->path, "/bin/busybox") == 0 ||
                strcmp(item->path, "/bin/ls") == 0)
            {
                printf("PROGMAN SMOKE FAIL el escaneo listo '%s' sin categoria declarada\n", item->path);
                return 1;
            }
        }

        for (index = 0; index < progman_group_count(); ++index)
        {
            const struct progman_group *group = progman_group_at(index);
            if (group != 0)
            {
                printf("PROGMAN SMOKE scan group %s items=%d\n", group->name, group->item_count);
            }
        }
        applied = progman_registry_apply_sxe();
        printf("PROGMAN SMOKE scan sxe applied=%d of %d\n", applied, progman_item_count());
        for (index = 0; index < progman_item_count(); ++index)
        {
            const struct progman_item *item = progman_item_at(index);
            if (item != 0)
            {
                printf("PROGMAN SMOKE scan item %s -> %s icono=%s\n",
                    item->name,
                    item->path,
                    progman_item_icon(item) != 0 ? "propio" : "horneado");
            }
        }
    }

    /*
     * Que el catalogo se pueda REHACER, que es lo que arregla el bug de que un
     * programa desinstalado siguiera en la grilla hasta reiniciar.
     *
     * La fixture es una copia de un binario ESTAMPADO (/bin/notepad trae su
     * .sxmeta con categoria) bajo un nombre que no existe en /bin: asi el
     * escaneo la ve como un programa instalado aparte, igual que a Doom. Un
     * archivo cualquiera no serviria -- sin categoria no se lista, y el test no
     * probaria nada.
     */
    if (copy_file("/bin/notepad", PROGMAN_FIXTURE_APP) != 0)
    {
        printf("PROGMAN SMOKE FAIL no se pudo instalar la fixture\n");
        return 1;
    }
    build_catalog();
    if (!catalog_has_path(PROGMAN_FIXTURE_APP))
    {
        printf("PROGMAN SMOKE FAIL la fixture instalada no aparece en el catalogo\n");
        (void)unlink(PROGMAN_FIXTURE_APP);
        return 1;
    }
    {
        /* Rehacerlo sin tocar el disco no puede cambiar nada: si build_catalog
         * acumulara en vez de rearmar, cada F5 duplicaria la grilla entera. */
        int items = progman_item_count();
        int groups = progman_group_count();

        build_catalog();
        if (progman_item_count() != items || progman_group_count() != groups)
        {
            printf("PROGMAN SMOKE FAIL rehacer el catalogo cambio %d/%d a %d/%d\n",
                groups, items, progman_group_count(), progman_item_count());
            (void)unlink(PROGMAN_FIXTURE_APP);
            return 1;
        }
    }
    if (unlink(PROGMAN_FIXTURE_APP) != 0)
    {
        printf("PROGMAN SMOKE FAIL no se pudo desinstalar la fixture\n");
        return 1;
    }
    build_catalog();
    if (catalog_has_path(PROGMAN_FIXTURE_APP))
    {
        printf("PROGMAN SMOKE FAIL la fixture desinstalada sigue en el catalogo\n");
        return 1;
    }
    printf("PROGMAN SMOKE refresh ok items=%d\n", progman_item_count());

    printf("PROGMAN SMOKE PASS groups=%d items=%d\n",
        progman_group_count(),
        progman_item_count());
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && argv != 0 && argv[1] != 0 && strcmp(argv[1], "--selftest") == 0)
    {
        return progman_selftest();
    }

    build_catalog();

    /* El widget de solapas se crea vacio y lo llena rebuild_group_labels(), que
     * es el mismo camino que toma cada actualizacion posterior: si el arranque
     * armara los rotulos por su cuenta, el codigo que corre una vez y el que
     * corre siempre serian distintos, y solo uno estaria ejercitado. */
    g_widgets[0] = sxgui_tabs(sx_rect_make(0, 0, 0, 0), g_group_labels, 0, 0);
    g_widgets[0].on_action = on_tab_changed;
    rebuild_group_labels();
    select_group(0);

    if (sxgui_app_init(&g_app, "progman", g_widgets, 1) < 0)
    {
        return 1;
    }
    g_menubar.menus = k_menus;
    g_menubar.menu_count = (int)(sizeof(k_menus) / sizeof(k_menus[0]));
    g_menubar.on_command = on_menu_command;
    g_menubar.user = 0;
    sxgui_set_menubar(&g_app.ui, &g_menubar);

    g_app.on_key = on_key;
    g_app.on_pointer = on_pointer;
    g_app.on_paint = on_paint;
    g_app.on_resize = on_resize;

    {
        int content_width = 0;
        int content_height = 0;

        /* Ya con el registro cargado: el tamano sale de los grupos y sus items. */
        preferred_content_size(&content_width, &content_height);
        (void)sxgui_app_set_content_size(&g_app, content_width, content_height);
    }
    progman_layout(&g_app);
    return sxgui_app_run(&g_app);
}
