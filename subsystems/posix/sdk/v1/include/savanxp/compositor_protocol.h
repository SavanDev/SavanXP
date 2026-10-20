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
 *
 * What compositord composes is the scene (docs/desktop/DESKTOP_ARCHITECTURE.md#
 * the-scene): surfaces the WM attaches -- client surfaces, and its own -- shown
 * through the layers of its last COMMIT, plus the cursor.
 */

#define SAVANXP_COMPOSITOR_PROTOCOL_MAGIC 0x5358434fu /* SXCO */
/* v2 added savanxp_compositor_service_timing to the reply. v3: a port instead
 * of inherited descriptors 3, 4 and 5. v4: the port is compositord's, and so
 * is the display memory. v5: the scene -- compositord composes, the WM
 * commits layers instead of presenting a composed frame. */
#define SAVANXP_COMPOSITOR_PROTOCOL_VERSION 5u

#define SAVANXP_COMPOSITOR_PORT_NAME "compositord"
#define SAVANXP_COMPOSITOR_PORT_PREFIX "wd-compositor-"

enum savanxp_compositor_message_type
{
    /* Makes the connection the WM. The WM queues the scene section on its
     * request pipe before it; the reply carries the mode compositord set at
     * startup (the native one) and the GPU's info, and the display section is
     * queued on the reply pipe before it. */
    SAVANXP_COMPOSITOR_MSG_INIT = 1,
    SAVANXP_COMPOSITOR_MSG_GET_TIMELINE = 4,
    SAVANXP_COMPOSITOR_MSG_SHUTDOWN = 7,
    /* Programs a video mode and re-imports the display section with it.
     * fb_info in the request is the mode asked for; in the reply, the mode
     * that resulted, which may differ (the device decides the pitch). The
     * section is not reallocated: it is sized for the largest mode, so a
     * smaller one fits and every mapping stays valid. A mode that would not
     * fit is refused and the previous one kept. The scene is composed again
     * whole at the next commit. */
    SAVANXP_COMPOSITOR_MSG_SET_MODE = 9,
    /* Attaches a surface (request.surface): its section, and for a client
     * surface its wake event after it, are queued on the request pipe before
     * the request. compositord takes the geometry from the request, never from
     * the section, which the client can write. */
    SAVANXP_COMPOSITOR_MSG_ATTACH = 10,
    /* Detaches request.surface_id: unmapped, its wake event closed. Its layers
     * must have left the scene in an earlier commit. */
    SAVANXP_COMPOSITOR_MSG_DETACH = 11,
    /* The scene section holds a new scene: compositord copies it, consumes
     * every published client batch, stages the damaged part of the WM's
     * surface, composes and presents. With SAVANXP_COMPOSITOR_COMMIT_WAIT the
     * reply waits until that frame is on the screen. */
    SAVANXP_COMPOSITOR_MSG_COMMIT = 12,
};

/* Surface 1 is the WM's own: frames, switcher, fallback background, the size
 * of the largest mode and drawn in screen coordinates. Client surfaces take
 * ids 2 .. SAVANXP_COMPOSITOR_MAX_SURFACES - 1. */
#define SAVANXP_COMPOSITOR_SURFACE_WM 1u
#define SAVANXP_COMPOSITOR_MAX_SURFACES 32u

/* request.surface.flags */
#define SAVANXP_COMPOSITOR_SURFACE_CLIENT 0x00000001u /* header, batches, wake event */

struct savanxp_compositor_surface
{
    uint32_t surface_id;
    uint32_t flags;
    uint64_t section_size;
    uint32_t pixels_offset;
    uint32_t pitch;
    uint32_t capacity_width;
    uint32_t capacity_height;
};

struct savanxp_compositor_rect
{
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

/* layer.flags */
#define SAVANXP_COMPOSITOR_LAYER_OPAQUE 0x00000001u

/* `src` of surface `surface_id` shown at `dst` on the screen; scaled (nearest)
 * when the sizes differ. */
struct savanxp_compositor_layer
{
    uint32_t surface_id;
    uint32_t flags;
    struct savanxp_compositor_rect src;
    struct savanxp_compositor_rect dst;
};

#define SAVANXP_COMPOSITOR_MAX_LAYERS 48u
#define SAVANXP_COMPOSITOR_MAX_DAMAGE 64u
#define SAVANXP_COMPOSITOR_SCENE_MAGIC 0x53584353u /* SXCS */

/* scene.flags */
#define SAVANXP_COMPOSITOR_SCENE_FULL_DAMAGE 0x00000001u

/* request.flags for COMMIT */
#define SAVANXP_COMPOSITOR_COMMIT_WAIT 0x00000001u

/*
 * The scene section, written by the WM between commits and read by
 * compositord only while it handles one. Layers back to front; damage is what
 * the WM changed since the previous commit, in screen coordinates (geometry
 * and its own surface); more than SAVANXP_COMPOSITOR_MAX_DAMAGE rectangles is
 * said with SAVANXP_COMPOSITOR_SCENE_FULL_DAMAGE. The cursor is compositord's
 * to draw: the hardware plane, or a software layer above everything.
 */
struct savanxp_compositor_scene
{
    uint32_t magic;
    uint32_t flags;
    uint32_t layer_count;
    uint32_t damage_count;
    int32_t cursor_x;
    int32_t cursor_y;
    uint32_t cursor_shape; /* enum savanxp_cursor_shape */
    uint32_t cursor_visible;
    struct savanxp_compositor_layer layers[SAVANXP_COMPOSITOR_MAX_LAYERS];
    struct savanxp_compositor_rect damage[SAVANXP_COMPOSITOR_MAX_DAMAGE];
};

struct savanxp_compositor_request
{
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t serial;
    uint32_t flags;
    uint32_t surface_id;
    struct savanxp_fb_info fb_info;
    struct savanxp_compositor_surface surface;
};

/*
 * How long compositord took, measured by itself (all zero: no clock yet, not
 * instantaneous; drop the sample). service_ns covers the request; compose_ns
 * and gpu_ns (the present syscall: kernel plus backend) and timeline_ns (the
 * timeline query before it) nest inside it.
 */
struct savanxp_compositor_service_timing
{
    uint64_t service_ns;
    uint64_t compose_ns;
    uint64_t gpu_ns;
    uint64_t timeline_ns;
};

struct savanxp_compositor_reply
{
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t serial;
    int32_t status;
    /* COMMIT: client surfaces whose batches were malformed, bit per surface
     * id. compositord stops composing them; the WM drops the client. */
    uint32_t broken_surfaces;
    struct savanxp_fb_info fb_info;
    struct savanxp_gpu_info gpu_info;
    struct savanxp_gpu_present_timeline timeline;
    struct savanxp_compositor_service_timing service_timing;
    /* COMMIT: pixels composed and presented, the bounding box of that damage,
     * and its rectangles: windowd-stats' damage fields. */
    uint64_t damage_px;
    uint64_t bounds_px;
    uint32_t damage_rects;
    uint32_t reserved;
};
