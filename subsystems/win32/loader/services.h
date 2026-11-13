/*
 * services.h -- The host side of the gate's services (sxw_abi.h), shared by
 * the host harness and winhost so both answer the guest the same way.
 *
 * Portable C, no library dependency. The platform supplies the standard
 * streams, the clock and the registry file; everything the guest passes is
 * checked against the regions the loader mapped before it is read or
 * written.
 */
#ifndef SAVANXP_WIN32_SERVICES_H
#define SAVANXP_WIN32_SERVICES_H

#include <stddef.h>
#include <stdint.h>

#include "loader.h"
#include "registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXW_REGISTRY_TEXT 32768u

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
    /* With fixed_time nonzero, the guest's clock starts there (a test run)
     * and advances with now(). */
    uint32_t fixed_time;
    uint64_t start_time;
    /* Set when the guest asked to end, with its 32-bit status. */
    int exit_requested;
    uint32_t exit_status;
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
