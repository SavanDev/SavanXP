#pragma once

#include <stdint.h>
#include "savanxp/syscall.h"

/*
 * SavanXP compositor control protocol (docs/desktop/DESKTOP_ARCHITECTURE.md#
 * composition-in-compositord).
 *
 * Transport (v4): compositord is a session service. sessiond starts it before
 * windowd; it allocates the display memory, picks the mode and creates the
 * port SAVANXP_COMPOSITOR_PORT_NAME -- or the name in its argv[1], when a
 * windowd in a test mode starts its own (SAVANXP_COMPOSITOR_PORT_PREFIX plus
 * that windowd's pid). The window manager connects to it, writes requests on
 * the first pipe sxn_port_connect gives and reads replies on the second.
 *
 * One window manager per compositord. A connection becomes the WM with its
 * INIT, and only from a process whose parent is compositord's parent or that
 * is compositord's parent; a connection that hangs up before INIT (a readiness
 * probe) is dropped. The INIT reply follows the display section, queued as a
 * handle on the reply pipe. compositord exits when its WM hangs up: there is
 * no reconnection, the session is restarted instead.
 */

#define SAVANXP_COMPOSITOR_PROTOCOL_MAGIC 0x5358434fu /* SXCO */
/* v2 added savanxp_compositor_service_timing to the reply. v3: a port instead
 * of inherited descriptors 3, 4 and 5. v4: the port is compositord's, and so
 * is the display memory. */
#define SAVANXP_COMPOSITOR_PROTOCOL_VERSION 4u

#define SAVANXP_COMPOSITOR_PORT_NAME "compositord"
#define SAVANXP_COMPOSITOR_PORT_PREFIX "wd-compositor-"

enum savanxp_compositor_message_type
{
    /* Makes the connection the WM. The reply carries the mode compositord set
     * at startup (the native one) and the GPU's info; the display section is
     * queued on the reply pipe before it. */
    SAVANXP_COMPOSITOR_MSG_INIT = 1,
    SAVANXP_COMPOSITOR_MSG_PRESENT = 2,
    SAVANXP_COMPOSITOR_MSG_SYNC_PRESENT = 3,
    SAVANXP_COMPOSITOR_MSG_GET_TIMELINE = 4,
    SAVANXP_COMPOSITOR_MSG_ENABLE_CURSOR = 5,
    SAVANXP_COMPOSITOR_MSG_MOVE_CURSOR = 6,
    SAVANXP_COMPOSITOR_MSG_SHUTDOWN = 7,
    SAVANXP_COMPOSITOR_MSG_SET_CURSOR_SHAPE = 8,
    /* Programs a video mode and re-imports the display section with it.
     * fb_info in the request is the mode asked for; in the reply, the mode
     * that resulted, which may differ (the device decides the pitch). The
     * section is not reallocated: it is sized for the largest mode, so a
     * smaller one fits and every mapping stays valid. A mode that would not
     * fit is refused and the previous one kept. */
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
