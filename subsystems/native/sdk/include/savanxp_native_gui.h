/*
 * SavanXP - cliente del compositor para el subsistema nativo (runtime v1).
 *
 * Protocolo de app VENTANEADA bajo el escritorio: el WM hace fork + dup2 de
 * los canales de la sesion a los fds 3..6 + exec del binario (nuestro exec ya
 * marca nativo por EI_OSABI, y los fds se heredan). Todo el protocolo corre
 * sobre syscalls del baseline posix (< SXN_SYS_BASE): mapear la seccion
 * compartida, poll/read de eventos y los eventos de wake/submit.
 *
 * Los structs de abajo son ESPEJOS del contrato de superficie v4 del
 * compositor (fuente de verdad: savanxp_gpu_client_surface_header y amigos en
 * subsystems/posix/sdk/v1/include/savanxp/syscall.h, y el contrato de fds en
 * savanxp/wm_protocol.h). Mismos campos, mismo orden: el layout es el contrato
 * de wire con el compositor.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Contrato de superficie v4 (espejo) ------------------------------------- */

#define SXN_GUI_SURFACE_MAGIC 0x53584746u /* "SXGF" */
#define SXN_GUI_SURFACE_VERSION_4 4u
#define SXN_GUI_BATCH_CAPACITY 8u
#define SXN_GUI_BATCH_MAX_RECTS 32u
/* Bit de flags: el WM pidio cerrar la ventana. */
#define SXN_GUI_SURFACE_FLAG_SHUTDOWN 0x00000001u

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

/* Pedido de lanzamiento para que el WM abra otra app. Espejo de
 * savanxp_desktop_launch_request del SDK posix. */
#define SXN_GUI_LAUNCH_PATH_CAPACITY 192u
#define SXN_GUI_LAUNCH_ARG_CAPACITY 192u

struct sxn_gui_launch_request {
    uint32_t flags;
    char path[SXN_GUI_LAUNCH_PATH_CAPACITY];
    char argument[SXN_GUI_LAUNCH_ARG_CAPACITY];
};

/* Pedidos del cliente al WM dentro del header. Espejo de
 * savanxp_wm_client_requests: cursor como estado, size hint con seqlock
 * (impar = a medio escribir) y launches en una cola circular donde el cliente
 * escribe la entrada y despues incrementa launch_head. */
#define SXN_GUI_LAUNCH_QUEUE_CAPACITY 4u

struct sxn_gui_requests {
    uint32_t cursor_shape;
    uint32_t size_hint_width;
    uint32_t size_hint_height;
    uint32_t size_hint_sequence;
    uint32_t launch_head;
    uint32_t launch_tail;
    struct sxn_gui_launch_request launch[SXN_GUI_LAUNCH_QUEUE_CAPACITY];
    /* Pedido de ventana con dueno (docs/OWNED_WINDOWS.md). Este runtime no abre
     * ventanas con dueno; los campos estan para que el layout coincida. */
    uint32_t window_sequence;
    uint32_t window_reply_sequence;
    int32_t window_reply_status;
    uint32_t window_reserved;
    uint32_t window_action;
    uint32_t window_id;
    uint32_t window_width;
    uint32_t window_height;
    char window_title[64];
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
    struct sxn_gui_requests requests;
};

/* Evento de teclado (o resize sintetizado del header). */
struct sxn_gui_input_event {
    uint32_t type; /* 1 = key down, 2 = key up, 3 = resized */
    uint32_t key;
    int32_t ascii;
    uint32_t modifiers; /* SXN_GUI_KEY_MOD_*, espejo de savanxp_key_modifier */
};

#define SXN_GUI_KEY_MOD_SHIFT 0x01u
#define SXN_GUI_KEY_MOD_CTRL 0x02u
#define SXN_GUI_KEY_MOD_ALT 0x04u
#define SXN_GUI_KEY_MOD_ALT_GR 0x08u
#define SXN_GUI_KEY_MOD_CAPS_LOCK 0x10u
#define SXN_GUI_KEY_MOD_NUM_LOCK 0x20u
#define SXN_GUI_KEY_MOD_SCROLL_LOCK 0x40u

#define SXN_GUI_EVENT_KEY_DOWN 1u
#define SXN_GUI_EVENT_KEY_UP 2u
#define SXN_GUI_EVENT_RESIZED 3u

/* Evento de puntero en coordenadas locales a la superficie del cliente
 * (route_pointer resta el origen de la ventana). Espejo de
 * savanxp_gui_pointer_event del SDK posix. */
struct sxn_gui_pointer_event {
    int32_t x;
    int32_t y;
    int32_t wheel; /* ticks de rueda; positivo = lejos del usuario */
    uint32_t buttons; /* mascara de bits SXN_GUI_MOUSE_BUTTON_* */
};

#define SXN_GUI_MOUSE_BUTTON_LEFT (1u << 0)
#define SXN_GUI_MOUSE_BUTTON_RIGHT (1u << 1)
#define SXN_GUI_MOUSE_BUTTON_MIDDLE (1u << 2)

/* Registro de 32 bytes del canal de eventos (fd 4): teclado y puntero por el
 * mismo pipe. Espejo de savanxp_wm_event. */
#define SXN_GUI_WM_EVENT_KEY 1u
#define SXN_GUI_WM_EVENT_POINTER 2u

struct sxn_gui_wm_event {
    uint32_t kind;
    uint32_t window_id; /* 0 = la ventana principal, la unica que abre este runtime */
    union {
        struct sxn_gui_input_event key;
        struct sxn_gui_pointer_event pointer;
        uint8_t bytes[24];
    } payload;
};

/* Fds fijos que el WM instala antes del exec del cliente. */
#define SXN_GUI_FD_SECTION 3
#define SXN_GUI_FD_EVENTS 4
#define SXN_GUI_FD_WAKE_EVENT 5
#define SXN_GUI_FD_SUBMIT_EVENT 6

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

/* 1 si el compositor pidio cerrar (flag de shutdown en el header). */
int sxn_gui_should_close(void);

/* Presentan copiando del frame del cliente a la superficie compartida y
 * sometiendo el batch (con espera de slot/idle y corte por shutdown).
 * `frame` usa el layout de la superficie (filas de pitch bytes). */
long sxn_gui_present(const void *frame);
long sxn_gui_present_region(const void *frame, unsigned int x, unsigned int y,
                            unsigned int width, unsigned int height);

/* Devuelve 1 con un evento (teclado o resize sintetizado), 0 sin eventos,
 * negativo en error. Los eventos de puntero que lea en el camino quedan
 * guardados para sxn_gui_poll_pointer. */
int sxn_gui_poll_event(struct sxn_gui_input_event *event);

/* Devuelve 1 con un evento de puntero (coordenadas locales a la superficie),
 * 0 si no hay ninguno encolado, negativo en error (-EINVAL sin sesion). Sin
 * sintesis: el WM entrega movimiento y botones crudos. Las teclas que lea en el
 * camino quedan guardadas para sxn_gui_poll_event. */
int sxn_gui_poll_pointer(struct sxn_gui_pointer_event *event);

/* Le pide al escritorio que lance `path` (debe ser absoluto) en otra ventana,
 * encolando el pedido en el header. Devuelve 0, o negativo si el path no sirve
 * o la cola sigue llena. Espejo de gfx_desktop_launch del SDK posix. */
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
