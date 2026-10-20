#pragma once

#include <stdint.h>
#include "savanxp/syscall.h"

/*
 * SavanXP compositor control protocol.
 *
 * Transport (v3): a port the client creates for this one compositord and
 * names on its command line (argv[1]; the client uses
 * SAVANXP_COMPOSITOR_PORT_PREFIX plus its pid). compositord connects to it,
 * so it reads requests from the reply end the port gives it and writes
 * replies on the request end -- the client is the service of the rendezvous,
 * compositord the one that answers. Before the INIT request the client queues
 * the shared display section on its pipe as a handle; compositord takes it
 * when it handles INIT. Nothing is inherited, so it needs neither fork nor
 * fixed descriptors -- what lets a native windowd start it.
 */

#define SAVANXP_COMPOSITOR_PROTOCOL_MAGIC 0x5358434fu /* SXCO */
/* v2 agrego savanxp_compositor_service_timing a la reply. Windowd y
 * compositord se construyen siempre juntos, asi que un desajuste de tamano no
 * puede pasar en la practica; la version igual sube porque el header promete
 * formato fijo y versionado, y asi un desajuste falla limpio en vez de
 * desincronizar el pipe. */
/* v3: a port instead of inherited descriptors 3, 4 and 5. */
#define SAVANXP_COMPOSITOR_PROTOCOL_VERSION 3u

#define SAVANXP_COMPOSITOR_PORT_PREFIX "wd-compositor-"

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
