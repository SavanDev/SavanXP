/*
 * SavanXP - fileshost: harness headless para la filesapp portada a Haxe.
 *
 * Maneja la app (navegador de directorios): valida que el Listbox renderice con
 * el item 0 seleccionado (navy), mueve la seleccion con las flechas (verifica
 * que el highlight se mueve de fila), ejercita Enter (entrar a un directorio) +
 * Backspace (subir) y por ultimo ESC para cerrar (exit 0). La navegacion real
 * se confirma tambien por el serial (la app loguea cada ruta). Imprime FILES
 * HOST PASS/FAIL.
 *
 * Coordenadas segun el layout de haxe-files/Main.hx (listbox 12,30,W-24,H-70).
 */
#include "savanxp/libc.h"

#define FILES_WIDTH 460u
#define FILES_HEIGHT 360u
#define FILES_PAGE 4096u
#define FILES_CLIENT_PATH "/disk/bin/filesapp-hx"
#define FILES_DEADLINE_MS 8000ul

#define FILES_NAVY 0x00000080u  /* item seleccionado / menu resaltado */
#define FILES_FIELD 0x00FFFFFFu /* fondo de listbox/preview */
#define FILES_SHADOW 0x00808080u
#define FILES_FACE 0x00C0C0C0u
#define FILES_LIGHT 0x00FFFFFFu
/* Layout de haxe-files/Main.hx con 460x360: menubar alto 26 (altoTexto+8),
 * topY=32, panelY=74; listbox (12,74,212,246) y preview (236,74,212,246).
 * Filas del listbox en y+2 + fila*altoTexto(18): fila 0 -> 76..93, fila 1 ->
 * 94..111. Los puntos elegidos caen a la derecha del texto. */
#define FILES_ROW0_X 215u
#define FILES_ROW0_Y 84u
#define FILES_ROW1_X 215u
#define FILES_ROW1_Y 102u
/* Panel de preview: borde hundido superior y un punto interior vacio. */
#define FILES_PREVIEW_BORDER_X 300u
#define FILES_PREVIEW_BORDER_Y 74u
#define FILES_PREVIEW_FIELD_X 440u
#define FILES_PREVIEW_FIELD_Y 300u
/* Menubar: linea SHADOW inferior en y=25. El popup de "Archivo" arranca en
 * y=26 con borde levantado LIGHT; ese pixel vale FACE con el menu cerrado. */
#define FILES_MENUBAR_LINE_X 400u
#define FILES_MENUBAR_LINE_Y 25u
#define FILES_POPUP_EDGE_X 20u
#define FILES_POPUP_EDGE_Y 26u
/* Centros de click: titulo "Archivo", item "Refrescar" (y 27..48) e item
 * "Salir" (y 77..98, tras el separador). */
#define FILES_TITLE_CX 33
#define FILES_TITLE_CY 12
#define FILES_ITEM_REFRESH_CX 30
#define FILES_ITEM_REFRESH_CY 38
#define FILES_ITEM_EXIT_CX 30
#define FILES_ITEM_EXIT_CY 87
/* Menu "Ayuda" (x ~66..119) y su unico item "Acerca de Files" (y 27..48). */
#define FILES_HELP_CX 92
#define FILES_HELP_CY 12
#define FILES_ITEM_ABOUT_CX 100
#define FILES_ITEM_ABOUT_CY 38
/* Dialog "Acerca de": cliente 280x96 + borde 3 + titulo 24 => 286x126, centrado
 * en 460x360 => (87,117). Barra de titulo navy en (90,120,280,24): el punto
 * elegido cae dentro de ella con el dialog abierto, y sobre el fondo FIELD del
 * listbox (fila 3, a la derecha del texto) con el dialog cerrado. */
#define FILES_DIALOG_TITLE_X 215u
#define FILES_DIALOG_TITLE_Y 130u
/* Boton OK del dialog: rel (90,56,100,26) => abs (180,200)-(279,225). */
#define FILES_DIALOG_OK_CX 230
#define FILES_DIALOG_OK_CY 213
/* Ejecutable que se espera lanzar: item 1 de /bin (item 0 es ".."). */
#define FILES_EXPECTED_LAUNCH "/bin/aboutapp"

static int fail(const char *reason) {
    printf("fileshost: %s\n", reason);
    puts("FILES HOST FAIL\n");
    return 1;
}

static struct savanxp_gpu_client_surface_header *g_header;
static struct savanxp_gpu_dirty_rect_batch *g_batches;
static uint32_t *g_pixels;
static int g_submit_event;
static int g_retire_event;

static int compose_next_frame(void) {
    unsigned long deadline = uptime_ms() + FILES_DEADLINE_MS;
    while (uptime_ms() < deadline) {
        if (g_header->submit_sequence > g_header->composed_sequence) {
            while (g_header->composed_sequence < g_header->submit_sequence) {
                uint64_t sequence = g_header->composed_sequence + 1u;
                const struct savanxp_gpu_dirty_rect_batch *batch =
                    &g_batches[(sequence - 1u) % g_header->batch_capacity];
                if (batch->submit_sequence != sequence) {
                    return -1;
                }
                g_header->retired_sequence = sequence;
                g_header->composed_sequence = sequence;
                (void)event_set(g_retire_event);
            }
            return 0;
        }
        (void)wait_one(g_submit_event, 100);
        (void)event_reset(g_submit_event);
    }
    return -2;
}

/* Compone todo lo pendiente hasta que la app deja de presentar por un rato: mas
 * robusto que contar frames exactos (una interaccion puede generar 1 o 2). */
static void settle(void) {
    unsigned long quiet_until = uptime_ms() + 400ul;
    while (uptime_ms() < quiet_until) {
        if (g_header->submit_sequence > g_header->composed_sequence) {
            while (g_header->composed_sequence < g_header->submit_sequence) {
                uint64_t sequence = g_header->composed_sequence + 1u;
                g_header->retired_sequence = sequence;
                g_header->composed_sequence = sequence;
                (void)event_set(g_retire_event);
            }
            quiet_until = uptime_ms() + 400ul;
        }
        (void)wait_one(g_submit_event, 50);
        (void)event_reset(g_submit_event);
    }
}

static uint32_t pixel_at(unsigned int x, unsigned int y) {
    return g_pixels[y * FILES_WIDTH + x];
}

static int send_key(int fd, uint32_t key) {
    struct savanxp_input_event event;
    event.type = SAVANXP_INPUT_EVENT_KEY_DOWN;
    event.key = key;
    event.ascii = 0;
    return write(fd, &event, sizeof(event)) == (long)sizeof(event) ? 0 : -1;
}

static int send_pointer(int fd, int x, int y, uint32_t buttons) {
    struct savanxp_gui_pointer_event event;
    event.x = x;
    event.y = y;
    event.buttons = buttons;
    return write(fd, &event, sizeof(event)) == (long)sizeof(event) ? 0 : -1;
}

/* Click completo (press + release) sobre un punto. */
static int click_at(int fd, int x, int y) {
    if (send_pointer(fd, x, y, SAVANXP_MOUSE_BUTTON_LEFT) != 0) {
        return -1;
    }
    return send_pointer(fd, x, y, 0u);
}

/* Lee un pedido de lanzamiento del canal fd 9 (el harness tiene el extremo de
 * lectura) y devuelve el path pedido. */
static int read_launch(int fd, char *out, unsigned long cap) {
    struct savanxp_desktop_launch_request request;
    unsigned long index = 0;
    if (read(fd, &request, sizeof(request)) != (long)sizeof(request)) {
        return -1;
    }
    while (index + 1u < cap && request.path[index] != '\0') {
        out[index] = request.path[index];
        index += 1;
    }
    out[index] = '\0';
    return 0;
}

int main(void) {
    void *view;
    unsigned long command_bytes;
    unsigned long pixels_offset;
    unsigned long buffer_size;
    long section_fd;
    int input_pipe[2] = {-1, -1};
    int mouse_pipe[2] = {-1, -1};
    int launch_pipe[2] = {-1, -1};
    int submit_event;
    int retire_event;
    int shutdown_event;
    long pid;
    int status = -1;
    int guard_fds[7];
    int guard_index;

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        guard_fds[guard_index] = dup(0);
        if (guard_fds[guard_index] < 0) {
            return fail("no se pudieron reservar los fds guardia");
        }
    }

    command_bytes = SAVANXP_GPU_CLIENT_BATCH_CAPACITY * sizeof(struct savanxp_gpu_dirty_rect_batch);
    pixels_offset = (sizeof(*g_header) + command_bytes + (FILES_PAGE - 1u)) & ~(unsigned long)(FILES_PAGE - 1u);
    buffer_size = (unsigned long)FILES_WIDTH * 4u * FILES_HEIGHT;

    section_fd = section_create(pixels_offset + buffer_size, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section_fd < 0) {
        return fail("section_create fallo");
    }
    view = map_view((int)section_fd, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (result_is_error((long)view)) {
        return fail("map_view fallo");
    }

    g_header = (struct savanxp_gpu_client_surface_header *)view;
    memset(view, 0, sizeof(*g_header) + command_bytes);
    g_header->magic = SAVANXP_GPU_CLIENT_SURFACE_MAGIC;
    g_header->command_offset = (uint32_t)sizeof(*g_header);
    g_header->pixels_offset = (uint32_t)pixels_offset;
    g_header->info.width = FILES_WIDTH;
    g_header->info.height = FILES_HEIGHT;
    g_header->info.pitch = FILES_WIDTH * 4u;
    g_header->info.bpp = 32;
    g_header->info.buffer_size = (uint32_t)buffer_size;
    g_header->version = SAVANXP_GPU_CLIENT_SURFACE_VERSION_3;
    g_header->pixel_format = SAVANXP_GPU_SURFACE_FORMAT_BGRX8888;
    g_header->batch_capacity = SAVANXP_GPU_CLIENT_BATCH_CAPACITY;
    g_header->rect_capacity = SAVANXP_GPU_CLIENT_BATCH_MAX_RECTS;
    g_batches = (struct savanxp_gpu_dirty_rect_batch *)((unsigned char *)view + g_header->command_offset);
    g_pixels = (uint32_t *)((unsigned char *)view + pixels_offset);
    memset(g_pixels, 0, buffer_size);

    submit_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    retire_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    shutdown_event = (int)event_create(SAVANXP_EVENT_MANUAL_RESET);
    if (submit_event < 0 || retire_event < 0 || shutdown_event < 0 ||
        pipe(input_pipe) < 0 || pipe(mouse_pipe) < 0 || pipe(launch_pipe) < 0) {
        return fail("no se pudieron crear eventos/pipes");
    }
    g_submit_event = submit_event;
    g_retire_event = retire_event;

    pid = fork();
    if (pid < 0) {
        return fail("fork fallo");
    }
    if (pid == 0) {
        const char *argv[2] = {FILES_CLIENT_PATH, 0};
        if (dup2((int)section_fd, 3) < 0 ||
            dup2(input_pipe[0], 4) < 0 ||
            dup2(mouse_pipe[0], 5) < 0 ||
            dup2(submit_event, 6) < 0 ||
            dup2(retire_event, 7) < 0 ||
            dup2(shutdown_event, 8) < 0 ||
            dup2(launch_pipe[1], 9) < 0) {
            exit(1);
        }
        (void)exec(FILES_CLIENT_PATH, argv, 1);
        printf("fileshost: exec de %s fallo\n", FILES_CLIENT_PATH);
        exit(1);
    }

    for (guard_index = 0; guard_index < 7; ++guard_index) {
        (void)close(guard_fds[guard_index]);
    }

    /* 1) Frame inicial: el listbox rinde con el item 0 seleccionado (navy), la
     *    fila 1 con fondo FIELD, y el panel de preview rinde a la derecha
     *    (borde hundido + interior FIELD). */
    if (compose_next_frame() != 0) {
        return fail("no llego el frame inicial");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_NAVY) {
        return fail("item 0 no resaltado en el frame inicial");
    }
    if (pixel_at(FILES_ROW1_X, FILES_ROW1_Y) != FILES_FIELD) {
        return fail("fila 1 no tiene fondo FIELD (listbox no rinde)");
    }
    if (pixel_at(FILES_PREVIEW_BORDER_X, FILES_PREVIEW_BORDER_Y) != FILES_SHADOW) {
        return fail("el panel de preview no rinde (borde hundido ausente)");
    }
    if (pixel_at(FILES_PREVIEW_FIELD_X, FILES_PREVIEW_FIELD_Y) != FILES_FIELD) {
        return fail("el panel de preview no rinde (interior FIELD ausente)");
    }
    if (pixel_at(FILES_MENUBAR_LINE_X, FILES_MENUBAR_LINE_Y) != FILES_SHADOW) {
        return fail("la barra de menu no rinde (linea inferior ausente)");
    }
    if (pixel_at(FILES_POPUP_EDGE_X, FILES_POPUP_EDGE_Y) != FILES_FACE) {
        return fail("hay un popup abierto al arrancar (deberia estar cerrado)");
    }

    /* 2) Flecha abajo: el resaltado baja a la fila 1. */
    if (send_key(input_pipe[1], SAVANXP_KEY_DOWN) != 0) {
        return fail("no se pudo enviar flecha abajo");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras flecha abajo");
    }
    if (pixel_at(FILES_ROW1_X, FILES_ROW1_Y) != FILES_NAVY) {
        return fail("la seleccion no bajo a la fila 1");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_FIELD) {
        return fail("la fila 0 sigue resaltada tras bajar");
    }

    /* 3) Flecha arriba: vuelve a la fila 0. */
    if (send_key(input_pipe[1], SAVANXP_KEY_UP) != 0) {
        return fail("no se pudo enviar flecha arriba");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras flecha arriba");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_NAVY) {
        return fail("la seleccion no volvio a la fila 0");
    }

    /* 4) Enter: entra al directorio seleccionado (item 0 de la raiz = un dir). */
    if (send_key(input_pipe[1], SAVANXP_KEY_ENTER) != 0) {
        return fail("no se pudo enviar Enter");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras entrar al directorio");
    }

    /* 4b) Dentro del directorio el item 0 es ".."; bajar selecciona una entrada
     *     real, lo que hace que el preview lea el archivo (ver el serial:
     *     "files: lineas de preview"). El item 0 vuelve a quedar sin resaltar. */
    if (send_key(input_pipe[1], SAVANXP_KEY_DOWN) != 0) {
        return fail("no se pudo enviar flecha abajo dentro del directorio");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras seleccionar una entrada del directorio");
    }
    if (pixel_at(FILES_ROW1_X, FILES_ROW1_Y) != FILES_NAVY) {
        return fail("la seleccion no bajo dentro del directorio");
    }

    /* 4c) Enter sobre ese archivo (vive en /bin) => la app le pide al escritorio
     *     que lo lance por el fd 9. El harness tiene el extremo de lectura del
     *     pipe, asi que verifica el pedido de verdad. */
    if (send_key(input_pipe[1], SAVANXP_KEY_ENTER) != 0) {
        return fail("no se pudo enviar Enter para lanzar");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras el lanzamiento");
    }
    settle();
    {
        char launched[256];
        if (read_launch(launch_pipe[0], launched, sizeof(launched)) != 0) {
            return fail("no llego el pedido de lanzamiento por el fd 9");
        }
        if (strcmp(launched, FILES_EXPECTED_LAUNCH) != 0) {
            printf("fileshost: se lanzo '%s', se esperaba '%s'\n", launched, FILES_EXPECTED_LAUNCH);
            puts("FILES HOST FAIL\n");
            return 1;
        }
        printf("fileshost: pedido de lanzamiento OK: %s\n", launched);
    }

    /* 5) Backspace: sube al directorio padre. */
    if (send_key(input_pipe[1], SAVANXP_KEY_BACKSPACE) != 0) {
        return fail("no se pudo enviar Backspace");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras subir");
    }
    settle();

    /* 6) Menubar: click en el titulo "Archivo" despliega el menu (aparece el
     *    borde levantado del popup donde antes habia FACE). */
    if (click_at(mouse_pipe[1], FILES_TITLE_CX, FILES_TITLE_CY) != 0) {
        return fail("no se pudo clickear el titulo Archivo");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras abrir el menu");
    }
    settle();
    if (pixel_at(FILES_POPUP_EDGE_X, FILES_POPUP_EDGE_Y) != FILES_LIGHT) {
        return fail("el menu Archivo no se desplego");
    }

    /* 7) Click en "Refrescar": dispara el comando y cierra el menu. */
    if (click_at(mouse_pipe[1], FILES_ITEM_REFRESH_CX, FILES_ITEM_REFRESH_CY) != 0) {
        return fail("no se pudo clickear Refrescar");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras Refrescar");
    }
    settle();
    if (pixel_at(FILES_POPUP_EDGE_X, FILES_POPUP_EDGE_Y) != FILES_FACE) {
        return fail("el menu no se cerro tras elegir Refrescar");
    }
    if (pixel_at(FILES_ROW0_X, FILES_ROW0_Y) != FILES_NAVY) {
        return fail("el listbox no rinde tras Refrescar");
    }

    /* 7b) Menu "Ayuda" -> "Acerca de": abre el dialog modal (barra de titulo
     *     navy donde antes se veia el fondo FIELD del listbox). */
    if (click_at(mouse_pipe[1], FILES_HELP_CX, FILES_HELP_CY) != 0) {
        return fail("no se pudo abrir el menu Ayuda");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame al abrir Ayuda");
    }
    settle();
    if (click_at(mouse_pipe[1], FILES_ITEM_ABOUT_CX, FILES_ITEM_ABOUT_CY) != 0) {
        return fail("no se pudo clickear Acerca de");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras abrir el dialog");
    }
    settle();
    if (pixel_at(FILES_DIALOG_TITLE_X, FILES_DIALOG_TITLE_Y) != FILES_NAVY) {
        return fail("el dialog Acerca de no se abrio");
    }

    /* 7c) Click en OK: el dialog se cierra y vuelve a verse el listbox. */
    if (click_at(mouse_pipe[1], FILES_DIALOG_OK_CX, FILES_DIALOG_OK_CY) != 0) {
        return fail("no se pudo clickear OK del dialog");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame tras cerrar el dialog");
    }
    settle();
    if (pixel_at(FILES_DIALOG_TITLE_X, FILES_DIALOG_TITLE_Y) != FILES_FIELD) {
        return fail("el dialog no se cerro con OK");
    }

    /* 8) Reabrir "Archivo" y elegir "Salir": la app se cierra sola (exit 0),
     *    verificando el despacho de comandos del menu. */
    if (click_at(mouse_pipe[1], FILES_TITLE_CX, FILES_TITLE_CY) != 0) {
        return fail("no se pudo reabrir el menu Archivo");
    }
    if (compose_next_frame() != 0) {
        return fail("no llego el frame al reabrir el menu");
    }
    settle();
    if (pixel_at(FILES_POPUP_EDGE_X, FILES_POPUP_EDGE_Y) != FILES_LIGHT) {
        return fail("el menu Archivo no se reabrio");
    }
    if (click_at(mouse_pipe[1], FILES_ITEM_EXIT_CX, FILES_ITEM_EXIT_CY) != 0) {
        return fail("no se pudo clickear Salir");
    }

    if (waitpid((int)pid, &status) != pid) {
        return fail("waitpid fallo");
    }
    if (status != 0) {
        printf("fileshost: cliente salio con %d\n", status);
        puts("FILES HOST FAIL\n");
        return 1;
    }

    printf("fileshost: render + seleccion + navegacion OK, frames=%d\n", (int)g_header->composed_sequence);
    puts("FILES HOST PASS\n");
    return 0;
}
