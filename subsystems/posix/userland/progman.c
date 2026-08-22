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

#define PROGMAN_TAB_HEIGHT 26
#define PROGMAN_STATUS_HEIGHT 22
#define PROGMAN_CELL_WIDTH 96
#define PROGMAN_CELL_HEIGHT 76
#define PROGMAN_ICON_SIZE 32
#define PROGMAN_GRID_MARGIN 8
#define PROGMAN_DOUBLE_CLICK_MS 450UL

static struct sxgui_app g_app;
/* El toolkit exige un array no nulo aunque no usemos widgets: progman pinta
 * todo su contenido en on_paint y hace su propio hit-testing. */
static struct sxgui_widget g_widgets[1];

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
    int usable = (int)g_app.gfx.info.width - (2 * PROGMAN_GRID_MARGIN);
    int columns = usable / PROGMAN_CELL_WIDTH;

    return columns > 0 ? columns : 1;
}

static struct sx_rect cell_rect(int item_index)
{
    int columns = grid_columns();
    int column = item_index % columns;
    int row = item_index / columns;

    return sx_rect_make(
        PROGMAN_GRID_MARGIN + (column * PROGMAN_CELL_WIDTH),
        content_top() + PROGMAN_TAB_HEIGHT + PROGMAN_GRID_MARGIN + (row * PROGMAN_CELL_HEIGHT),
        PROGMAN_CELL_WIDTH,
        PROGMAN_CELL_HEIGHT);
}

static struct sx_rect group_tab_rect(int group_index)
{
    int x = 4;
    int index;

    for (index = 0; index < progman_group_count(); ++index)
    {
        const struct progman_group *group = progman_group_at(index);
        int width = gfx_text_width(group != 0 ? group->name : "") + 20;

        if (index == group_index)
        {
            return sx_rect_make(x, content_top() + 3, width, PROGMAN_TAB_HEIGHT - 6);
        }
        x += width + 2;
    }
    return sx_rect_make(0, 0, 0, 0);
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
    g_item = 0;
    g_last_click_item = -1;
    set_status_for_selection();
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

/* Bisel 3D estilo sistema: claro arriba/izquierda, oscuro abajo/derecha. */
static void draw_bevel(struct sx_painter *painter, struct sx_rect rect, int pressed)
{
    uint32_t top_left = pressed ? SXGUI_COLOR_SHADOW : SXGUI_COLOR_LIGHT;
    uint32_t bottom_right = pressed ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_SHADOW;

    if (rect.width <= 0 || rect.height <= 0)
    {
        return;
    }
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, rect.width, 1), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y, 1, rect.height), top_left);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x, rect.y + rect.height - 1, rect.width, 1), bottom_right);
    sx_painter_fill_rect(painter, sx_rect_make(rect.x + rect.width - 1, rect.y, 1, rect.height), bottom_right);
}

static void paint_tabs(struct sx_painter *painter)
{
    int index;

    sx_painter_fill_rect(painter, sx_rect_make(0, content_top(), (int)g_app.gfx.info.width, PROGMAN_TAB_HEIGHT), SXGUI_COLOR_FACE);
    for (index = 0; index < progman_group_count(); ++index)
    {
        const struct progman_group *group = progman_group_at(index);
        struct sx_rect rect = group_tab_rect(index);
        int active = (index == g_group);

        if (group == 0 || rect.width <= 0)
        {
            continue;
        }
        sx_painter_fill_rect(painter, rect, active ? SXGUI_COLOR_LIGHT : SXGUI_COLOR_FACE);
        draw_bevel(painter, rect, active);
        sx_painter_draw_text(painter, rect.x + 10, rect.y + ((rect.height - gfx_text_height()) / 2), group->name, SXGUI_COLOR_TEXT);
    }
    /* Linea de separacion bajo las pestanias. */
    sx_painter_fill_rect(painter, sx_rect_make(0, content_top() + PROGMAN_TAB_HEIGHT - 1, (int)g_app.gfx.info.width, 1), SXGUI_COLOR_SHADOW);
}

static void paint_items(struct sx_painter *painter)
{
    int count = group_item_count(g_group);
    int index;

    for (index = 0; index < count; ++index)
    {
        const struct progman_item *item = progman_group_item_at(g_group, index);
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
        draw_icon_scaled(
            painter,
            desktop_icon_large((enum desktop_icon_id)item->icon_id),
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

static void on_paint(struct sxgui_app *app)
{
    struct sx_painter *painter = &app->ui.painter;

    paint_tabs(painter);
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

static void ask_power_confirmation(int command)
{
    const char *question = (command == PROGMAN_CMD_REBOOT)
        ? "Reiniciar SavanXP?"
        : "Apagar SavanXP?";

    g_pending_power = command;
    g_dialog_widgets[0] = sxgui_label(sx_rect_make(16, 14, 220, 16), question);
    g_dialog_widgets[1] = sxgui_button(sx_rect_make(30, 48, 80, 26), "Si", on_power_confirm, 0);
    g_dialog_widgets[2] = sxgui_button(sx_rect_make(126, 48, 80, 26), "No", on_power_cancel, 0);

    memset(&g_dialog, 0, sizeof(g_dialog));
    g_dialog.title = (command == PROGMAN_CMD_REBOOT) ? "Reiniciar" : "Apagar";
    g_dialog.widgets = g_dialog_widgets;
    g_dialog.widget_count = 3;
    sxgui_dialog_begin(&g_app.ui, &g_dialog, 240, 92);
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
    /* Solo la transicion suelto->apretado cuenta como click. */
    if (left != 0 && left_was == 0)
    {
        int index;

        for (index = 0; index < progman_group_count(); ++index)
        {
            if (sx_rect_contains_point(group_tab_rect(index), event->x, event->y))
            {
                if (index != g_group)
                {
                    select_group(index);
                    changed = 1;
                }
                g_last_buttons = event->buttons;
                return changed;
            }
        }

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

static void on_resize(struct sxgui_app *app)
{
    (void)app;
    /* El grid se recalcula solo desde gfx.info; nada que reubicar. */
}

/* Existencia real: si el path no se puede abrir, no se puede lanzar. */
static int path_is_launchable(const char *path)
{
    long fd;

    if (path == 0 || path[0] == '\0')
    {
        return 0;
    }

    fd = open(path);
    if (fd < 0)
    {
        return 0;
    }
    (void)close((int)fd);
    return 1;
}

/* --- selftest ------------------------------------------------------------ */

static int progman_selftest(void)
{
    int failures = progman_registry_selftest();
    int dropped;
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
     * pruning contra el disco REAL de esta imagen. Es lo que decide si el grupo
     * Native (ports en Haxe) y Doom se muestran o no, porque se construyen con
     * builds aparte y en un arbol limpio no estan instalados. */
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
    select_group(0);

    if (sxgui_app_init(&g_app, "progman", g_widgets, 0) < 0)
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
    return sxgui_app_run(&g_app);
}
