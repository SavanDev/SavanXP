/*
 * SavanXP - cliente del compositor para el subsistema nativo (runtime v1).
 *
 * Protocolo de app VENTANEADA bajo el escritorio: el shell hace fork + dup2 de
 * los canales de la sesion a los fds 3..9 + exec del binario (nuestro exec ya
 * marca nativo por EI_OSABI, y los fds se heredan). Todo el protocolo corre
 * sobre syscalls del baseline posix (< SXN_SYS_BASE): mapear la seccion
 * compartida, poll/read de input y eventos de submit/retire/shutdown.
 *
 * Los structs de abajo son ESPEJOS del contrato de superficie v3 del
 * compositor (fuente de verdad: savanxp_gpu_client_surface_header y amigos en
 * subsystems/posix/sdk/v1/include/savanxp/syscall.h, y el armado de la seccion
 * en subsystems/posix/userland/desktop.c). Mismos campos, mismo orden: el
 * layout es el contrato de wire con el compositor.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Contrato de superficie v3 (espejo) ------------------------------------- */

#define SXN_GUI_SURFACE_MAGIC 0x53584746u /* "SXGF" */
#define SXN_GUI_SURFACE_VERSION_3 3u
#define SXN_GUI_BATCH_CAPACITY 8u
#define SXN_GUI_BATCH_MAX_RECTS 32u

struct sxn_gui_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bpp;
    uint32_t buffer_size;
};

struct sxn_gui_dirty_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct sxn_gui_batch {
    uint64_t submit_sequence;
    uint32_t rect_count;
    uint32_t flags;
    struct sxn_gui_dirty_rect rects[SXN_GUI_BATCH_MAX_RECTS];
};

struct sxn_gui_surface_header {
    uint32_t magic;
    uint32_t pixels_offset;
    struct sxn_gui_fb_info info;
    uint32_t version;
    uint32_t flags;
    uint32_t pixel_format;
    uint32_t reserved0;
    uint32_t command_offset;
    uint32_t batch_capacity;
    uint32_t rect_capacity;
    uint32_t reserved1;
    uint64_t submit_sequence;
    uint64_t retired_sequence;
    uint64_t composed_sequence;
};

/* Evento de input crudo que el shell rutea por el fd 4. */
struct sxn_gui_input_event {
    uint32_t type; /* 1 = key down, 2 = key up, 3 = resized */
    uint32_t key;
    int32_t ascii;
};

#define SXN_GUI_EVENT_KEY_DOWN 1u
#define SXN_GUI_EVENT_KEY_UP 2u
#define SXN_GUI_EVENT_RESIZED 3u

/* Evento de puntero que el shell rutea por el fd 5, en coordenadas locales a la
 * superficie del cliente (route_pointer resta el origen de la ventana). Espejo
 * de savanxp_gui_pointer_event del SDK posix. */
struct sxn_gui_pointer_event {
    int32_t x;
    int32_t y;
    uint32_t buttons; /* mascara de bits SXN_GUI_MOUSE_BUTTON_* */
};

#define SXN_GUI_MOUSE_BUTTON_LEFT (1u << 0)
#define SXN_GUI_MOUSE_BUTTON_RIGHT (1u << 1)
#define SXN_GUI_MOUSE_BUTTON_MIDDLE (1u << 2)

/* Pedido de lanzamiento que el cliente escribe por el fd 9 para que el shell
 * abra otra app. Espejo de savanxp_desktop_launch_request del SDK posix. */
#define SXN_GUI_LAUNCH_PATH_CAPACITY 192u
#define SXN_GUI_LAUNCH_ARG_CAPACITY 192u

struct sxn_gui_launch_request {
    uint32_t reserved0;
    char path[SXN_GUI_LAUNCH_PATH_CAPACITY];
    char argument[SXN_GUI_LAUNCH_ARG_CAPACITY];
};

/* Tamano de area util que el cliente pide por el fd 11 para su contenido.
 * Espejo de savanxp_desktop_size_hint del SDK posix. */
struct sxn_gui_size_hint {
    uint32_t width;
    uint32_t height;
};

/* Fds fijos que el shell instala antes del exec del cliente. */
#define SXN_GUI_FD_SECTION 3
#define SXN_GUI_FD_INPUT 4
#define SXN_GUI_FD_MOUSE 5
#define SXN_GUI_FD_SUBMIT_EVENT 6
#define SXN_GUI_FD_RETIRE_EVENT 7
#define SXN_GUI_FD_SHUTDOWN_EVENT 8
#define SXN_GUI_FD_LAUNCH 9
#define SXN_GUI_FD_CURSOR_HINT 10
#define SXN_GUI_FD_SIZE_HINT 11

/* --- API del runtime ---------------------------------------------------------
 * Una sesion de ventana por proceso (estado global en sx_gui.c): suficiente
 * para el modelo de una-superficie-por-cliente del compositor actual. */

/* Conecta con la sesion heredada del shell. Devuelve 0, -ENODEV si no hay
 * seccion en el fd 3 (no nos lanzo el escritorio) o -EINVAL si el header de
 * superficie no valida. */
long sxn_gui_open(void);
void sxn_gui_close(void);

/* Geometria de la ventana (validas tras sxn_gui_open). El frame del cliente
 * debe usar stride_pixels pixeles por fila (pitch de la superficie). */
unsigned int sxn_gui_width(void);
unsigned int sxn_gui_height(void);
unsigned int sxn_gui_stride_pixels(void);

/* Secuencia de frames ya compuestos por el compositor (del header). */
unsigned long sxn_gui_composed_sequence(void);

/* 1 si el compositor pidio cerrar (evento de shutdown senalado). */
int sxn_gui_should_close(void);

/* Presentan copiando del frame del cliente a la superficie compartida y
 * sometiendo el batch (con espera de slot/idle y corte por shutdown).
 * `frame` usa el layout de la superficie (filas de pitch bytes). */
long sxn_gui_present(const void *frame);
long sxn_gui_present_region(const void *frame, unsigned int x, unsigned int y,
                            unsigned int width, unsigned int height);

/* Devuelve 1 con un evento (teclado o resize sintetizado), 0 sin eventos,
 * negativo en error. */
int sxn_gui_poll_event(struct sxn_gui_input_event *event);

/* Devuelve 1 con un evento de puntero del fd 5 (coordenadas locales a la
 * superficie), 0 si no hay ninguno encolado, negativo en error (-EINVAL sin
 * sesion). Igual que el canal de teclado pero sin sintesis: el shell entrega
 * movimiento y botones crudos. */
int sxn_gui_poll_pointer(struct sxn_gui_pointer_event *event);

/* Le pide al escritorio que lance `path` (debe ser absoluto) en otra ventana,
 * escribiendo el pedido por el fd 9. Devuelve 0, o negativo si el path no
 * sirve / falla la escritura. Espejo de gfx_desktop_launch del SDK posix. */
long sxn_gui_launch(const char *path);

/* Pide el area util que necesita el contenido de la ventana. Es una
 * sugerencia: el WM la recorta y solo la atiende mientras la ventana siga en
 * la geometria del launch (ver savanxp/wm_protocol.h en el SDK posix, que es
 * la fuente canonica del protocolo). */
long sxn_gui_request_content_size(unsigned int width, unsigned int height);
/* Espera hasta timeout_ms a que el WM aplique un cambio de tamano. Devuelve 1
 * si llego, 0 si vencio el plazo. Despues, sxn_gui_width/height ya devuelven
 * el tamano nuevo. */
long sxn_gui_wait_content_size(long timeout_ms);

/* --- Texto (fuente Noto horneada, compartida con posix) ----------------------
 * Render de texto para el toolkit del escritorio (Fase 3). Dibujan sobre un
 * buffer XRGB contiguo (`pixels`, `stride` pixeles por fila, recortado a
 * width x height); (x, y) es la esquina superior-izquierda de la caja. */
int sxn_text_width(const char *text);
int sxn_text_height(void);
void sxn_text_draw(unsigned int *pixels, int stride, int width, int height,
                   int x, int y, const char *text, unsigned int color);

#ifdef __cplusplus
}
#endif
