#include "libc.h"
#include "savanxp/sxgui.h"

#include "progman_registry.h"
#include "desktop_wallpaper.h"

#include <stdio.h>

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
#define PROGMAN_CELL_HEIGHT 76
#define PROGMAN_ICON_SIZE 32
#define PROGMAN_GRID_MARGIN 8
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
    PROGMAN_CMD_WALLPAPER,
    PROGMAN_CMD_ABOUT,
};

static const struct sxgui_menu_item k_file_items[] = {
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
        page.y + PROGMAN_GRID_MARGIN + (row * PROGMAN_CELL_HEIGHT),
        PROGMAN_CELL_WIDTH,
        PROGMAN_CELL_HEIGHT);
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
        int label_width;
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
            rect.y + 8,
            PROGMAN_ICON_SIZE);

        /* El nombre se recorta a la celda para que un label largo no invada la
         * de al lado. */
        label_width = gfx_text_width(item->name);
        label_x = rect.x + ((rect.width - label_width) / 2);
        if (sx_painter_push_clip(painter, rect))
        {
            sx_painter_draw_text(
                painter,
                label_x,
                rect.y + 8 + PROGMAN_ICON_SIZE + 6,
                item->name,
                selected ? SXGUI_COLOR_SELECT_TEXT : SXGUI_COLOR_TEXT);
            sx_painter_pop_clip(painter);
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
        (2 * PROGMAN_GRID_MARGIN) + (rows * PROGMAN_CELL_HEIGHT) + PROGMAN_STATUS_HEIGHT;
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

/* --- selftest ------------------------------------------------------------ */

static int progman_selftest(void)
{
    int failures = progman_registry_selftest();
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

    progman_registry_load();
    /* Sacar del catalogo lo que no esta instalado: los ports en Haxe y Doom se
     * construyen con builds APARTE, asi que en un arbol limpio sus entradas
     * existen pero no lanzan nada. Aplica igual a los defaults y al .ini. */
    (void)progman_registry_prune_missing(path_is_launchable);
    /* Recien aca se leen los recursos de cada binario: despues del pruning,
     * para no abrir ejecutables que se van a descartar y porque el pruning
     * reordena los items (docs/SXE_FORMAT.md, fase 3). */
    (void)progman_registry_apply_sxe();

    /* La tabla de rotulos se arma DESPUES del pruning: es el momento en que el
     * registro deja de cambiar, y el widget se queda con estos punteros. */
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
        g_widgets[0] = sxgui_tabs(sx_rect_make(0, 0, 0, 0), g_group_labels, count, 0);
        g_widgets[0].on_action = on_tab_changed;
    }
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
