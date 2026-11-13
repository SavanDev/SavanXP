/*
 * services.h -- The host side of the gate's services (sxw_abi.h), shared by
 * the host harness and winhost so both answer the guest the same way.
 *
 * Portable C, no library dependency. The platform supplies the standard
 * streams, the clock, the registry file and, for a program with windows, its
 * windows and input; everything the guest passes is
 * checked against the regions the loader mapped before it is read or
 * written.
 */
#ifndef SAVANXP_WIN32_SERVICES_H
#define SAVANXP_WIN32_SERVICES_H

#include <stddef.h>
#include <stdint.h>

#include "loader.h"
#include "registry.h"
#include "sxw_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXW_REGISTRY_TEXT 32768u

/* What a platform with windows supplies (sxw_abi.h, "Windows and input").
 * NULL in sxw_services for a host without them: then every window service
 * fails and an event never comes. */
typedef struct sxw_window_platform {
    /* Milliseconds since the process started. */
    uint32_t (*ticks)(void *context);
    void (*display)(void *context, uint32_t *width, uint32_t *height);
    /* A new top-level window with a client area of about *width x *height,
     * which is set to the one given. The title is UTF-8; kind, owner and the
     * position are SXW_SVC_WINDOW_CREATE's. An id > 0, or 0. */
    uint32_t (*create)(void *context, const char *title, uint32_t *width, uint32_t *height, uint32_t owner,
                       uint32_t kind, int32_t x, int32_t y);
    void (*destroy)(void *context, uint32_t window);
    int (*set_title)(void *context, uint32_t window, const char *title);
    /* The window's SXW_ICON_EXTENT-square icon, 0xAARRGGBB, top row first.
     * 0 on success. */
    int (*set_icon)(void *context, uint32_t window, const uint32_t *pixels);
    /* Copies the rectangles, already clipped to the buffer, from pixels
     * (stride pixels per row, width x height) and shows them. 0 on success. */
    int (*present)(void *context, uint32_t window, const uint32_t *pixels, uint32_t stride, uint32_t width,
                   uint32_t height, const int32_t *rects, uint32_t count);
    /* Waits up to timeout milliseconds (SXW_INFINITE, or 0 to poll) for an
     * event. 1 with *event filled, 0 on timeout. */
    int (*wait_event)(void *context, uint32_t timeout, sxw_event *event);
} sxw_window_platform;

typedef struct sxw_services {
    sxw_process *process;
    void *context;
    /* Writes to standard input (0), output (1) or error (2). Returns the
     * bytes written, or a negative value on failure. */
    long (*write)(void *context, int stream, const void *buffer, uint32_t length);
    /* Seconds since 1970 UTC. */
    uint64_t (*now)(void *context);
    /* Replaces the registry file with text. 0, or nonzero on failure. NULL
     * keeps the registry in memory only. */
    int (*save_registry)(void *context, const char *text, size_t length);
    /* HKEY_CURRENT_USER, loaded by the platform before the guest starts. */
    sxw_registry *registry;
    /* With fixed_time nonzero, the guest's clock reads that second and stays
     * there (a test run): a program that seeds from it deals the same however
     * long its start takes. */
    uint32_t fixed_time;
    /* Set when the guest asked to end, with its 32-bit status. */
    int exit_requested;
    uint32_t exit_status;
    /* Windows and input, or NULL. */
    const sxw_window_platform *windows;
    char registry_text[SXW_REGISTRY_TEXT];
} sxw_services;

/* Call once before the guest starts. */
void sxw_services_start(sxw_services *services);

/* Performs the service the guest's stack names. guest_esp points at
 * [return address][service][arguments]. Returns the value for eax. */
uint32_t sxw_service(sxw_services *services, uint32_t guest_esp);

/* Whether [address, address + length) lies inside one mapped guest region. */
int sxw_guest_range(const sxw_process *process, uint32_t address, uint32_t length);

/* The console handles Windows XP gives a console program, by stream. */
#define SXW_HANDLE_STDIN 3u
#define SXW_HANDLE_STDOUT 7u
#define SXW_HANDLE_STDERR 11u

/* STATUS_ACCESS_VIOLATION: a service frame or argument block that is not
 * guest memory ends the process with it. */
#define SXW_STATUS_ACCESS_VIOLATION 0xc0000005u
/* STATUS_INVALID_SYSTEM_SERVICE: an unknown service number. */
#define SXW_STATUS_INVALID_SYSTEM_SERVICE 0xc000001cu

#ifdef __cplusplus
}
#endif

#endif
