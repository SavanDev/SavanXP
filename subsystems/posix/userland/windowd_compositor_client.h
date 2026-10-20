#pragma once

#include "libc.h"
#include "savanxp/compositor_protocol.h"

struct windowd_compositor_connection
{
    long pid;
    int request_fd;
    int reply_fd;
    /* The private port of a compositord windowd started itself, in its test
     * modes (compositor_protocol.h); pid is that child, -1 in the session. */
    char port_name[32];
    /* compositord's display section, mapped for the selftests that sample
     * composed pixels; windowd never writes it. */
    void *display_view;
    uint32_t *framebuffer;
    /* The scene section (compositor_protocol.h): written between commits. */
    struct savanxp_compositor_scene *scene;
    /* windowd's own surface, SAVANXP_COMPOSITOR_SURFACE_WM: frames, switcher,
     * fallback background, the size of the native mode. */
    void *wm_view;
    uint32_t wm_pitch;
    struct savanxp_fb_info requested_info;
    struct savanxp_fb_info display_info;
    struct savanxp_gpu_info gpu_info;
    uint32_t next_serial;
    int connected;
};

void windowd_compositor_connection_init(struct windowd_compositor_connection *connection);
/* Connects to the session's compositord, or to one it starts itself when
   `own_daemon` (the test modes, which run without a session). */
int windowd_compositor_open(struct windowd_compositor_connection *connection, int own_daemon);
/* Returns non-zero while the daemon link is live. There is no reconnection:
   without compositord the session is over (sessiond restarts it). */
int windowd_compositor_connected(const struct windowd_compositor_connection *connection);
void windowd_compositor_close(struct windowd_compositor_connection *connection);
/* Attaches a surface (compositor_protocol.h ATTACH): its section, and for a
   client surface its wake event, are handed over; windowd keeps its own. */
int windowd_compositor_attach(
    struct windowd_compositor_connection *connection,
    const struct savanxp_compositor_surface *surface,
    int section_fd,
    int wake_event_fd);
int windowd_compositor_detach(struct windowd_compositor_connection *connection, uint32_t surface_id);
/* The scene section holds a new scene: COMMIT, with `flags`
   (SAVANXP_COMPOSITOR_COMMIT_WAIT). `reply` may be 0. */
int windowd_compositor_commit(
    struct windowd_compositor_connection *connection,
    uint32_t flags,
    struct savanxp_compositor_reply *reply);
int windowd_compositor_get_timeline(
    struct windowd_compositor_connection *connection,
    struct savanxp_gpu_present_timeline *timeline);
/* Programs a video mode; compositord re-imports the display section with it.
   The mapping does not move: the section is sized for the largest mode. On 0,
   connection->display_info holds the geometry that resulted, which may not be
   the one asked for. */
int windowd_compositor_set_mode(
    struct windowd_compositor_connection *connection,
    uint32_t width,
    uint32_t height);
