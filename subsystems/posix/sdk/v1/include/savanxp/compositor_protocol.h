#pragma once

#include <stdint.h>
#include "savanxp/syscall.h"

/*
 * SavanXP compositor control protocol.
 *
 * Transport today is a pair of inherited pipes plus one inherited display
 * section:
 *   fd 3: client -> compositord requests
 *   fd 4: compositord -> client replies
 *   fd 5: shared display framebuffer section
 *
 * The format is fixed-size and versioned so the transport can later move to a
 * connectable local socket without changing message semantics.
 */

#define SAVANXP_COMPOSITOR_PROTOCOL_MAGIC 0x5358434fu /* SXCO */
/* v2 agrego savanxp_compositor_service_timing a la reply. Windowd y
 * compositord se construyen siempre juntos, asi que un desajuste de tamano no
 * puede pasar en la practica; la version igual sube porque el header promete
 * formato fijo y versionado, y asi un desajuste falla limpio en vez de
 * desincronizar el pipe. */
#define SAVANXP_COMPOSITOR_PROTOCOL_VERSION 2u

#define SAVANXP_COMPOSITOR_REQUEST_FD 3
#define SAVANXP_COMPOSITOR_REPLY_FD 4
#define SAVANXP_COMPOSITOR_DISPLAY_SECTION_FD 5

enum savanxp_compositor_message_type
{
    SAVANXP_COMPOSITOR_MSG_INIT = 1,
    SAVANXP_COMPOSITOR_MSG_PRESENT = 2,
    SAVANXP_COMPOSITOR_MSG_SYNC_PRESENT = 3,
    SAVANXP_COMPOSITOR_MSG_GET_TIMELINE = 4,
    SAVANXP_COMPOSITOR_MSG_ENABLE_CURSOR = 5,
    SAVANXP_COMPOSITOR_MSG_MOVE_CURSOR = 6,
    SAVANXP_COMPOSITOR_MSG_SHUTDOWN = 7,
    SAVANXP_COMPOSITOR_MSG_SET_CURSOR_SHAPE = 8,
    /* Reprograma el modo de video y reimporta la seccion de display con la
     * geometria nueva. fb_info de la request lleva el modo pedido; el de la
     * reply, el que quedo -- que puede diferir, porque el pitch lo decide el
     * dispositivo. La seccion no se reasigna: se dimensiona para el modo mas
     * grande (el nativo), asi que un modo menor entra sin tocar el mapeo y el
     * puntero al backbuffer del shell sigue siendo valido. */
    SAVANXP_COMPOSITOR_MSG_SET_MODE = 9,
};

enum savanxp_compositor_surface_id
{
    SAVANXP_COMPOSITOR_SURFACE_DISPLAY = 1,
};

struct savanxp_compositor_request
{
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t serial;
    struct savanxp_fb_info fb_info;
    uint32_t surface_id;
    uint32_t rect_count;
    uint32_t flags;
    uint32_t wait_for_target;
    uint64_t target_sequence;
    struct savanxp_gpu_dirty_rect rects[SAVANXP_GPU_SURFACE_PRESENT_BATCH_MAX_RECTS];
    struct savanxp_gpu_cursor_position cursor_position;
};

/*
 * Cuanto tardo compositord en atender la request, medido por el propio
 * compositord. Existe para partir el `present_us` que mide windowd, que es el
 * camino ENTERO -- ida y vuelta por los pipes, compositord, kernel y backend --
 * y por si solo no dice a cual de los cuatro atacar.
 *
 * Los intervalos anidan: gpu_ns y timeline_ns caen adentro de service_ns, y
 * service_ns adentro de lo que mide windowd. La diferencia entre los dos
 * ultimos es el transporte mas el costo de despertar al otro proceso.
 *
 * Todo en cero significa 'sin reloj' (monotonic_ns todavia sin calibrar), no
 * 'instantaneo': el consumidor tiene que descartar la muestra.
 */
struct savanxp_compositor_service_timing
{
    uint64_t service_ns;   /* leer la request -> escribir la reply */
    uint64_t gpu_ns;       /* el syscall de present (kernel + backend) */
    uint64_t timeline_ns;  /* el ioctl de timeline que lo precede */
};

struct savanxp_compositor_reply
{
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t serial;
    int32_t status;
    uint32_t ready;
    uint64_t present_sequence;
    struct savanxp_fb_info fb_info;
    struct savanxp_gpu_info gpu_info;
    struct savanxp_gpu_present_timeline timeline;
    struct savanxp_compositor_service_timing service_timing;
};
