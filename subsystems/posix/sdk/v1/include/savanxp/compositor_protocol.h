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
#define SAVANXP_COMPOSITOR_PROTOCOL_VERSION 1u

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
};
