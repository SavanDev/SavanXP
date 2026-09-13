#include "shell_core.h"
#include "shellapp_stats.h"

#include "shared/version.h"

#define SHELLAPP_HISTORY_LINES 256
#define SHELLAPP_LINE_LENGTH 160
#define SHELLAPP_MARGIN_X 16
#define SHELLAPP_MARGIN_Y 14
#define SHELLAPP_HEADER_HEIGHT 30
#define SHELLAPP_LINE_HEIGHT 18
#define SHELLAPP_HISTORY_TOP (SHELLAPP_HEADER_HEIGHT + SHELLAPP_MARGIN_Y)
/* Techo de filas de texto visibles. A 18 px por fila son 4600 px de alto, muy
 * por encima de cualquier pantalla: el tope existe para que la sombra de filas
 * sea un array fijo, no para limitar la ventana. */
#define SHELLAPP_MAX_ROWS 256
/* La terminal se mide en celdas, no en pixeles: 80x24 es el tamano clasico y
 * el que asumen casi todos los programas de consola al formatear su salida. */
#define SHELLAPP_CONTENT_COLUMNS 80
#define SHELLAPP_CONTENT_ROWS 24
#define SHELLAPP_CURSOR_PERIOD_MS 500UL
#define SHELLAPP_PRESENT_INTERVAL_MS 16UL

struct shellapp_line {
    char text[SHELLAPP_LINE_LENGTH];
    int length;
    int stream;
    /* Identidad y version. `serial` es monotono y no se reusa, asi que dice si
     * una fila de la pantalla sigue mostrando LA MISMA linea despues de que el
     * historial se corra; `revision` sube con cada caracter, que es como se
     * detecta que la linea abierta crecio sin haber empujado una nueva. */
    unsigned long serial;
    unsigned long revision;
};

/* Lo que quedo pintado en cada fila de la pantalla. */
struct shellapp_row {
    unsigned long serial;
    unsigned long revision;
};

struct shellapp_state {
    struct savanxp_gfx_context gfx;
    uint32_t* frame;
    char input[256];
    int line_count;
    unsigned long next_serial;
    struct shellapp_row rows[SHELLAPP_MAX_ROWS];
    struct shellapp_stats stats;
    /* Indice de la linea mas vieja viva dentro de g_lines: el historial es un
     * anillo. Antes era un array compacto y descartar la linea mas vieja movia
     * las otras 255 (~42 KB de memmove) por cada linea nueva -- un `ls` de 200
     * entradas costaba 8,4 MB de copias. */
    int line_head;
    int current_line_open;
    int cursor_visible;
    int needs_redraw;
    int force_full_present;
    struct sx_rect_set dirty_rects;
    unsigned long last_present_ms;
    unsigned long next_cursor_toggle_ms;
};

static struct shellapp_line g_lines[SHELLAPP_HISTORY_LINES];
static struct shellapp_state g_shellapp = {0};

/* index 0 = la linea mas vieja viva, index line_count - 1 = la mas reciente. */
static struct shellapp_line* shellapp_line_at(int index) {
    return &g_lines[(g_shellapp.line_head + index) % SHELLAPP_HISTORY_LINES];
}

static struct shellapp_line* shellapp_push_line(int stream) {
    struct shellapp_line* line = 0;

    if (g_shellapp.line_count == SHELLAPP_HISTORY_LINES) {
        g_shellapp.line_head = (g_shellapp.line_head + 1) % SHELLAPP_HISTORY_LINES;
        --g_shellapp.line_count;
    }

    line = shellapp_line_at(g_shellapp.line_count++);
    memset(line, 0, sizeof(*line));
    line->stream = stream;
    line->serial = ++g_shellapp.next_serial;
    line->revision = 1;
    g_shellapp.current_line_open = 1;
    return line;
}

static struct shellapp_line* shellapp_ensure_line(int stream) {
    struct shellapp_line* line = 0;

    if (g_shellapp.line_count == 0 || !g_shellapp.current_line_open) {
        return shellapp_push_line(stream);
    }

    line = shellapp_line_at(g_shellapp.line_count - 1);
    if (line->stream != stream && line->length != 0) {
        return shellapp_push_line(stream);
    }

    line->stream = stream;
    return line;
}

static void shellapp_finish_line(void) {
    g_shellapp.current_line_open = 0;
}

static void shellapp_append_char(int stream, char value) {
    struct shellapp_line* line = 0;
    int length = 0;

    if (value == '\r') {
        return;
    }
    if (value == '\n') {
        (void)shellapp_ensure_line(stream);
        shellapp_finish_line();
        return;
    }
    if (value == '\t') {
        shellapp_append_char(stream, ' ');
        shellapp_append_char(stream, ' ');
        shellapp_append_char(stream, ' ');
        shellapp_append_char(stream, ' ');
        return;
    }
    if ((unsigned char)value < 32u || (unsigned char)value > 126u) {
        value = '?';
    }

    /* El largo vive en la linea: buscarlo con strlen() en cada caracter hacia
     * O(n^2) el llenado de una linea larga. */
    line = shellapp_ensure_line(stream);
    length = line->length;
    if ((size_t)length + 1 >= sizeof(line->text)) {
        shellapp_finish_line();
        line = shellapp_push_line(stream);
        length = 0;
    }

    line->text[length] = value;
    line->text[length + 1] = '\0';
    line->length = length + 1;
    ++line->revision;
}

static void shellapp_append_text_line(int stream, const char* text) {
    while (text != 0 && *text != '\0') {
        shellapp_append_char(stream, *text++);
    }
    shellapp_finish_line();
}

static void shellapp_clear_history(void) {
    memset(g_lines, 0, sizeof(g_lines));
    g_shellapp.line_count = 0;
    g_shellapp.line_head = 0;
    g_shellapp.current_line_open = 0;
}

static int shellapp_prompt_y(const struct savanxp_fb_info* info) {
    return (int)info->height - SHELLAPP_MARGIN_Y - gfx_cell_height();
}

/* Filas de texto que entran entre el header y el prompt. Una sola definicion:
 * la sombra de filas y el pintado tienen que estar de acuerdo o una fila se
 * marca sucia en una grilla y se dibuja en otra. */
static int shellapp_visible_rows(const struct savanxp_fb_info* info) {
    const int history_bottom = shellapp_prompt_y(info) - SHELLAPP_LINE_HEIGHT;
    int rows = history_bottom > SHELLAPP_HISTORY_TOP
        ? (history_bottom - SHELLAPP_HISTORY_TOP) / SHELLAPP_LINE_HEIGHT
        : 0;

    return rows > SHELLAPP_MAX_ROWS ? SHELLAPP_MAX_ROWS : rows;
}

/* Indice de la linea que va en la fila 0: el historial se muestra pegado al
 * prompt, asi que la ventana visible es siempre la cola. */
static int shellapp_first_visible_line(int visible_rows) {
    return g_shellapp.line_count > visible_rows ? g_shellapp.line_count - visible_rows : 0;
}

static uint32_t shellapp_background_colour(void) {
    return gfx_rgb(15, 19, 26);
}

static uint32_t shellapp_cursor_colour(void) {
    return gfx_rgb(128, 226, 164);
}

static struct sx_rect shellapp_cursor_rect(const struct savanxp_fb_info* info) {
    const int x = SHELLAPP_MARGIN_X + gfx_text_width_mono("> ") + gfx_text_width_mono(g_shellapp.input);
    return sx_rect_make(x, shellapp_prompt_y(info) + gfx_cell_height() + 1, gfx_cell_width(), 2);
}

/* Pinta un rectangulo recortado contra la banda sucia. Dibujar de mas es
 * correcto pero caro: es justamente lo que hacia que cada tecla costara una
 * superficie entera. */
static void shellapp_fill_clipped(const struct savanxp_fb_info* info, int x, int y, int width, int height, struct sx_rect clip, uint32_t colour) {
    struct sx_rect rect = sx_rect_intersect(sx_rect_make(x, y, width, height), clip);

    if (sx_rect_is_empty(rect)) {
        return;
    }
    gfx_rect(g_shellapp.frame, info, rect.x, rect.y, rect.width, rect.height, colour);
}

static void shellapp_invalidate_rect(int x, int y, int width, int height) {
    struct sx_rect rect = sx_rect_make(x, y, width, height);
    struct sx_rect bounds = sx_rect_make(0, 0, (int)g_shellapp.gfx.info.width, (int)g_shellapp.gfx.info.height);

    if (width <= 0 || height <= 0) {
        return;
    }

    rect = sx_rect_intersect(rect, bounds);
    if (sx_rect_is_empty(rect)) {
        return;
    }

    (void)sx_rect_set_add(&g_shellapp.dirty_rects, rect);
    g_shellapp.needs_redraw = 1;
}

static void shellapp_invalidate_full(void) {
    g_shellapp.force_full_present = 1;
    /* La sombra deja de valer: cambio la geometria o el contenido entero, asi
     * que ninguna fila puede darse por pintada. */
    memset(g_shellapp.rows, 0, sizeof(g_shellapp.rows));
    shellapp_invalidate_rect(0, 0, (int)g_shellapp.gfx.info.width, (int)g_shellapp.gfx.info.height);
}

static void shellapp_invalidate_header(void) {
    shellapp_invalidate_rect(0, 0, (int)g_shellapp.gfx.info.width, SHELLAPP_HEADER_HEIGHT + 2);
}

/* Deriva el danio del contenido comparando lo que cada fila MUESTRA contra lo
 * que quedo PINTADO en ella, y devuelve cuantas filas no coinciden.
 *
 * Antes el danio lo declaraba el que llamaba: invalidate_content() marcaba el
 * area entera, asi que una sola linea nueva costaba repintar y presentar las 24
 * filas visibles. Comparar serial+revision lo vuelve exacto -- una linea que
 * crecio ensucia su fila y nada mas -- y de paso hace que el pintado no pueda
 * desincronizarse de lo que se declaro sucio, porque sale del mismo lugar. */
static int shellapp_sync_rows(const struct savanxp_fb_info* info) {
    const int visible = shellapp_visible_rows(info);
    const int first_line = shellapp_first_visible_line(visible);
    int dirty = 0;
    int row;

    for (row = 0; row < visible; ++row) {
        const int line_index = first_line + row;
        unsigned long serial = 0;
        unsigned long revision = 0;

        if (line_index < g_shellapp.line_count) {
            const struct shellapp_line* line = shellapp_line_at(line_index);
            serial = line->serial;
            revision = line->revision;
        }

        if (g_shellapp.rows[row].serial == serial && g_shellapp.rows[row].revision == revision) {
            continue;
        }

        g_shellapp.rows[row].serial = serial;
        g_shellapp.rows[row].revision = revision;
        shellapp_invalidate_rect(
            0,
            SHELLAPP_HISTORY_TOP + (row * SHELLAPP_LINE_HEIGHT),
            (int)info->width,
            SHELLAPP_LINE_HEIGHT);
        ++dirty;
    }

    return dirty;
}

static void shellapp_invalidate_prompt(void) {
    const int y = shellapp_prompt_y(&g_shellapp.gfx.info) - 8;
    shellapp_invalidate_rect(0, y, (int)g_shellapp.gfx.info.width, (int)g_shellapp.gfx.info.height - y);
}

/* Repinta lo que cae adentro de UN rectangulo sucio y devuelve cuantas filas de
 * texto tuvo que blittear.
 *
 * Limpiar y dibujar adentro del propio rect deja el resultado bien aunque dos
 * rects se pisen: el area compartida se limpia y se vuelve a dibujar entera en
 * el segundo, nunca se compone dos veces sobre si misma. Eso importa porque el
 * texto del header va con alfa y blendearlo dos veces lo oscureceria. */
static unsigned long shellapp_paint_rect(const struct savanxp_fb_info* info, struct sx_rect clip) {
    const int clip_right = clip.x + clip.width;
    const int clip_bottom = clip.y + clip.height;
    const int prompt_y = shellapp_prompt_y(info);
    const int visible = shellapp_visible_rows(info);
    const int first_line = shellapp_first_visible_line(visible);
    unsigned long rows_painted = 0;
    int row;

    gfx_rect(g_shellapp.frame, info, clip.x, clip.y, clip.width, clip.height, shellapp_background_colour());

    if (clip.y <= SHELLAPP_HEADER_HEIGHT) {
        char cwd[256] = {};

        shell_current_directory(cwd, sizeof(cwd));
        shellapp_fill_clipped(info, 0, 0, (int)info->width, SHELLAPP_HEADER_HEIGHT, clip, gfx_rgb(25, 36, 52));
        shellapp_fill_clipped(info, 0, SHELLAPP_HEADER_HEIGHT, (int)info->width, 1, clip, gfx_rgb(76, 112, 156));
        gfx_blit_text_clip(g_shellapp.frame, info, SHELLAPP_MARGIN_X, 8, SAVANXP_DISPLAY_NAME " shell", gfx_rgb(236, 243, 255),
            clip.x, clip.y, clip_right, clip_bottom);
        gfx_blit_text_clip(g_shellapp.frame, info, (int)info->width - gfx_text_width(cwd) - SHELLAPP_MARGIN_X, 8, cwd, gfx_rgb(173, 201, 232),
            clip.x, clip.y, clip_right, clip_bottom);
    }

    for (row = 0; row < visible; ++row) {
        const int line_index = first_line + row;
        const int y = SHELLAPP_HISTORY_TOP + (row * SHELLAPP_LINE_HEIGHT);
        const struct shellapp_line* line = 0;

        if (line_index >= g_shellapp.line_count) {
            break;
        }
        if (y >= clip_bottom || (y + gfx_cell_height()) <= clip.y) {
            continue;
        }

        line = shellapp_line_at(line_index);
        gfx_blit_text_mono_clip(g_shellapp.frame, info, SHELLAPP_MARGIN_X, y, line->text,
            line->stream == 2 ? gfx_rgb(255, 170, 170) : gfx_rgb(220, 233, 245),
            clip.x, clip.y, clip_right, clip_bottom);
        ++rows_painted;
    }

    shellapp_fill_clipped(info, 0, prompt_y - 8, (int)info->width, 1, clip, gfx_rgb(41, 58, 78));
    if (prompt_y < clip_bottom && (prompt_y + gfx_cell_height()) > clip.y) {
        gfx_blit_text_mono_clip(g_shellapp.frame, info, SHELLAPP_MARGIN_X, prompt_y, "> ", shellapp_cursor_colour(),
            clip.x, clip.y, clip_right, clip_bottom);
        gfx_blit_text_mono_clip(g_shellapp.frame, info, SHELLAPP_MARGIN_X + gfx_text_width_mono("> "), prompt_y, g_shellapp.input,
            gfx_rgb(246, 248, 252), clip.x, clip.y, clip_right, clip_bottom);
    }

    if (g_shellapp.cursor_visible) {
        struct sx_rect cursor = shellapp_cursor_rect(info);
        shellapp_fill_clipped(info, cursor.x, cursor.y, cursor.width, cursor.height, clip, shellapp_cursor_colour());
    }

    return rows_painted;
}

static void shellapp_redraw(void) {
    const struct savanxp_fb_info* info = &g_shellapp.gfx.info;
    unsigned long rows_painted = 0;
    unsigned long long damage_px = 0;
    struct sx_rect bounds;
    size_t index;

    /* El contenido declara su propio danio antes de mirar si hay algo que hacer:
     * los que lo cambian solo piden un repintado, no saben que filas movieron. */
    (void)shellapp_sync_rows(info);

    if (!sx_rect_set_valid(&g_shellapp.dirty_rects)) {
        g_shellapp.needs_redraw = 0;
        g_shellapp.force_full_present = 0;
        return;
    }

    /* Un rectangulo a la vez, no el bounding box que los contiene: con el header
     * y una fila del medio sucios, la banda que los abarca es casi la ventana
     * entera y repintarla desperdicia todo lo que la sombra de filas acota. */
    shellapp_stats_draw_begin(&g_shellapp.stats);
    for (index = 0; index < g_shellapp.dirty_rects.count; ++index) {
        const struct sx_rect rect = g_shellapp.dirty_rects.rects[index];

        rows_painted += shellapp_paint_rect(info, rect);
        damage_px += (unsigned long long)rect.width * (unsigned long long)rect.height;
    }
    shellapp_stats_draw_done(&g_shellapp.stats, rows_painted);

    bounds = sx_rect_set_bounds(&g_shellapp.dirty_rects);
    shellapp_stats_present_begin(&g_shellapp.stats);
    {
        long present_result = 0;
        if (g_shellapp.force_full_present ||
            (g_shellapp.dirty_rects.count == 1 &&
            bounds.x == 0 &&
            bounds.y == 0 &&
            bounds.width == (int)info->width &&
            bounds.height == (int)info->height)) {
            present_result = gfx_present(&g_shellapp.gfx, g_shellapp.frame);
        } else {
            present_result = gfx_present_rects(
                &g_shellapp.gfx,
                g_shellapp.frame,
                g_shellapp.dirty_rects.rects,
                g_shellapp.dirty_rects.count);
        }
        if (present_result < 0) {
            exit(1);
        }
    }
    shellapp_stats_present_done(
        &g_shellapp.stats,
        damage_px,
        (unsigned long long)bounds.width * (unsigned long long)bounds.height,
        (unsigned long)g_shellapp.dirty_rects.count);

    g_shellapp.needs_redraw = 0;
    g_shellapp.force_full_present = 0;
    sx_rect_set_clear(&g_shellapp.dirty_rects);
    g_shellapp.last_present_ms = uptime_ms();
    shellapp_stats_report(&g_shellapp.stats, 0);
}

static void shellapp_request_redraw(int immediate) {
    g_shellapp.needs_redraw = 1;
    if (immediate || g_shellapp.last_present_ms == 0 ||
        uptime_ms() - g_shellapp.last_present_ms >= SHELLAPP_PRESENT_INTERVAL_MS) {
        shellapp_redraw();
    }
}

/* El parpadeo del cursor toca dos filas de pixeles: pinta la celda (o el fondo,
 * cuando toca apagarla) y presenta ese rectangulo solo. Antes pasaba por
 * shellapp_redraw(), asi que una terminal abierta y quieta rehacia la superficie
 * entera dos veces por segundo, para siempre. El subrayado va debajo de la fila
 * de glifos, no la pisa: borrarlo con el color de fondo es correcto. */
static void shellapp_blink_cursor(void) {
    const struct savanxp_fb_info* info = &g_shellapp.gfx.info;
    struct sx_rect bounds = sx_rect_make(0, 0, (int)info->width, (int)info->height);
    struct sx_rect rect = sx_rect_intersect(shellapp_cursor_rect(info), bounds);

    if (sx_rect_is_empty(rect)) {
        return;
    }

    gfx_rect(g_shellapp.frame, info, rect.x, rect.y, rect.width, rect.height,
        g_shellapp.cursor_visible ? shellapp_cursor_colour() : shellapp_background_colour());
    if (gfx_present_rects(&g_shellapp.gfx, g_shellapp.frame, &rect, 1) < 0) {
        exit(1);
    }
    g_shellapp.last_present_ms = uptime_ms();
    shellapp_stats_blink(&g_shellapp.stats);
}

/* Los que cambian el historial ya no declaran que area se ensucio: piden un
 * repintado y shellapp_sync_rows() averigua que filas movieron. */
static void shellapp_sink_emit(void* context, int fd, const char* bytes, size_t length) {
    size_t index = 0;
    (void)context;

    while (index < length) {
        shellapp_append_char(fd == 2 ? 2 : 1, bytes[index]);
        ++index;
    }

    shellapp_request_redraw(0);
}

static void shellapp_sink_clear(void* context) {
    (void)context;
    shellapp_clear_history();
    shellapp_request_redraw(1);
}

static void shellapp_submit_input(void) {
    struct shell_capture_sink sink = {
        .emit = shellapp_sink_emit,
        .clear = shellapp_sink_clear,
        .context = &g_shellapp,
    };
    char line[sizeof(g_shellapp.input)] = {};
    size_t index = 0;

    while (g_shellapp.input[index] != '\0' && index + 1 < sizeof(line)) {
        line[index] = g_shellapp.input[index];
        ++index;
    }
    line[index] = '\0';

    /* Eco del comando en el historial. Se arma la linea y se la agrega una sola
     * vez: antes se agregaba el input crudo y despues se pisaba con strcpy, que
     * ademas dejaba line->length desactualizado. */
    {
        char echo[SHELLAPP_LINE_LENGTH];
        size_t echo_length = 0;

        echo[echo_length++] = '>';
        if (line[0] != '\0') {
            echo[echo_length++] = ' ';
            index = 0;
            while (line[index] != '\0' && echo_length + 1 < sizeof(echo)) {
                echo[echo_length++] = line[index++];
            }
        }
        echo[echo_length] = '\0';
        shellapp_append_text_line(1, echo);
    }

    memset(g_shellapp.input, 0, sizeof(g_shellapp.input));
    g_shellapp.cursor_visible = 1;
    g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
    /* El prompt si hay que declararlo: se vacio el input. El eco del comando en
     * el historial lo encuentra sync_rows. */
    shellapp_invalidate_prompt();
    shellapp_request_redraw(1);

    if (line[0] == '\0') {
        return;
    }

    if (shell_execute_line(line, SHELL_EXEC_CAPTURE, &sink) == SHELL_EXEC_RESULT_EXIT) {
        gfx_close(&g_shellapp.gfx);
        exit(0);
    }

    /* El header muestra el cwd, que un `cd` acaba de cambiar. */
    shellapp_invalidate_header();
    shellapp_request_redraw(1);
}

static void shellapp_handle_key(const struct savanxp_input_event* event) {
    size_t length = strlen(g_shellapp.input);

    if (event->type != SAVANXP_INPUT_EVENT_KEY_DOWN) {
        return;
    }

    if (event->key == SAVANXP_KEY_BACKSPACE) {
        if (length > 0) {
            g_shellapp.input[length - 1] = '\0';
            shellapp_invalidate_prompt();
            g_shellapp.needs_redraw = 1;
        }
        return;
    }

    if (event->key == SAVANXP_KEY_ENTER) {
        shellapp_submit_input();
        return;
    }

    if (event->key == SAVANXP_KEY_TAB) {
        int spaces = 0;
        while (spaces < 4 && length + 1 < sizeof(g_shellapp.input)) {
            g_shellapp.input[length++] = ' ';
            ++spaces;
        }
        g_shellapp.input[length] = '\0';
        shellapp_invalidate_prompt();
        g_shellapp.needs_redraw = 1;
        return;
    }

    if (event->ascii >= 32 && event->ascii <= 126 && length + 1 < sizeof(g_shellapp.input)) {
        g_shellapp.input[length] = (char)event->ascii;
        g_shellapp.input[length + 1] = '\0';
        shellapp_invalidate_prompt();
        g_shellapp.needs_redraw = 1;
    }
}

int main(void) {
    struct savanxp_input_event event = {0};
    struct savanxp_pollfd pollfd = {0};

    memset(&g_shellapp, 0, sizeof(g_shellapp));
    if (gfx_open(&g_shellapp.gfx) < 0) {
        puts_fd(2, "shellapp: gfx_open failed\n");
        return 1;
    }
    if (gfx_acquire(&g_shellapp.gfx) < 0) {
        puts_fd(2, "shellapp: gfx_acquire failed\n");
        gfx_close(&g_shellapp.gfx);
        return 1;
    }

    /* Pedir la ventana que necesita la grilla de texto. El WM puede recortar
     * el pedido, asi que el tamano real se lee despues de que lo aplique -- el
     * loop principal ya sincroniza gfx.info con el header en cada vuelta. */
    if (gfx_request_content_size(
            &g_shellapp.gfx,
            (uint32_t)((2 * SHELLAPP_MARGIN_X) + (SHELLAPP_CONTENT_COLUMNS * gfx_cell_width())),
            (uint32_t)(SHELLAPP_HEADER_HEIGHT + (2 * SHELLAPP_MARGIN_Y) +
                ((SHELLAPP_CONTENT_ROWS + 1) * SHELLAPP_LINE_HEIGHT))) == 0)
    {
        (void)gfx_wait_content_size(&g_shellapp.gfx, 250UL);
    }

    /* Se dibuja siempre sobre la superficie compartida del WM. El respaldo era
     * un g_backbuffer[1920*1080] estatico -- 8,3 MiB de .bss que el loader mapea
     * eager (pagina a pagina, con memset) antes de entrar a main, y que este
     * camino no usaba nunca. */
    if (g_shellapp.gfx.pixels == 0) {
        puts_fd(2, "shellapp: no shared surface\n");
        gfx_close(&g_shellapp.gfx);
        return 1;
    }
    g_shellapp.frame = g_shellapp.gfx.pixels;
    shellapp_stats_open(&g_shellapp.stats);
    g_shellapp.cursor_visible = 1;
    g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
    shellapp_append_text_line(1, SAVANXP_DISPLAY_NAME " shell app");
    shellapp_append_text_line(1, "Desktop compositor session. Press Super for the launcher.");
    shellapp_append_text_line(1, "Builtins keep state here; external commands run through /bin/sh -c.");
    shellapp_invalidate_full();

    for (;;) {
        const unsigned long now_ms = uptime_ms();
        long timeout_ms = (long)(g_shellapp.next_cursor_toggle_ms > now_ms
            ? (g_shellapp.next_cursor_toggle_ms - now_ms)
            : 0UL);
        long ready = 0;

        if (g_shellapp.gfx.header != 0 &&
            (g_shellapp.gfx.header->info.width != g_shellapp.gfx.info.width ||
             g_shellapp.gfx.header->info.height != g_shellapp.gfx.info.height)) {
            g_shellapp.gfx.info.width = g_shellapp.gfx.header->info.width;
            g_shellapp.gfx.info.height = g_shellapp.gfx.header->info.height;
            g_shellapp.gfx.notified_width = g_shellapp.gfx.header->info.width;
            g_shellapp.gfx.notified_height = g_shellapp.gfx.header->info.height;
            shellapp_invalidate_full();
        }

        if (g_shellapp.needs_redraw) {
            shellapp_redraw();
        }

        pollfd.fd = g_shellapp.gfx.input_fd;
        pollfd.events = SAVANXP_POLLIN | SAVANXP_POLLHUP;
        pollfd.revents = 0;
        ready = savanxp_poll(&pollfd, 1, timeout_ms);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            g_shellapp.cursor_visible = !g_shellapp.cursor_visible;
            g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
            shellapp_blink_cursor();
            continue;
        }
        if ((pollfd.revents & SAVANXP_POLLHUP) != 0) {
            break;
        }
        if ((pollfd.revents & SAVANXP_POLLIN) == 0) {
            continue;
        }

        /* 0 no es un error: el canal trae tambien el puntero, y un registro de
         * puntero despierta el poll sin dejar ninguna tecla. Cerrar la terminal
         * en ese caso la hacia desaparecer apenas el mouse pasaba por encima. */
        {
            const int polled = gfx_poll_event(&g_shellapp.gfx, &event);
            if (polled < 0) {
                break;
            }
            if (polled == 0) {
                continue;
            }
        }

        if (event.type == SAVANXP_INPUT_EVENT_RESIZED) {
            (void)gfx_apply_resize_event(&g_shellapp.gfx, &event);
            g_shellapp.cursor_visible = 1;
            g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
            shellapp_invalidate_full();
            continue;
        }

        g_shellapp.cursor_visible = 1;
        g_shellapp.next_cursor_toggle_ms = uptime_ms() + SHELLAPP_CURSOR_PERIOD_MS;
        shellapp_invalidate_prompt();
        shellapp_handle_key(&event);
    }

    shellapp_stats_report(&g_shellapp.stats, 1);
    shellapp_stats_close(&g_shellapp.stats);

    gfx_close(&g_shellapp.gfx);
    return 0;
}
