/*
 * services.h -- The host side of the gate's services (sxw_abi.h), shared by
 * the host harness and winhost so both answer the guest the same way.
 *
 * Portable C, no library dependency. The platform supplies only the
 * standard streams; everything the guest passes is checked against the
 * regions the loader mapped before it is read.
 */
#ifndef SAVANXP_WIN32_SERVICES_H
#define SAVANXP_WIN32_SERVICES_H

#include <stdint.h>

#include "loader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sxw_services {
    const sxw_process *process;
    void *context;
    /* Writes to standard input (0), output (1) or error (2). Returns the
     * bytes written, or a negative value on failure. */
    long (*write)(void *context, int stream, const void *buffer, uint32_t length);
    /* Set when the guest asked to end, with its 32-bit status. */
    int exit_requested;
    uint32_t exit_status;
} sxw_services;

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
