#pragma once

/* sxnelevate: asking the system to run a program with its rights
 * (docs/kernel/USERS_AND_PERMISSIONS.md#elevation).
 *
 * The request goes to the elevation broker over its port; windowd shows the
 * consent prompt, and on a yes the broker starts `path` with the caller's uid
 * and euid 0. The caller never gains the rights itself. A caller that already
 * acts as the system is not asked.
 *
 * Every call this makes is open to all subsystems, so POSIX's elevate() is a
 * wrapper over this one: one implementation of the wire protocol.
 *
 * Header-only like sxn.h. Returns the elevated program's pid, -EACCES when
 * the user declined (or the broker is not the system's), or another -errno.
 * With `exit_status` non-null it also waits for the program and stores its
 * exit status.
 */

#include <stdint.h>
#include <string.h>

#include "abi/savanxp_abi.h"
#include "elevation/elevation_protocol.h"
#include "sxn/sxn.h"

static inline long sxn_elevate(const char* path, const char* const* argv, int argc, int* exit_status) {
    struct savanxp_elevation_request request;
    struct savanxp_elevation_reply reply;
    struct sxn_identity creator;
    int32_t channel[2] = {-1, -1};
    size_t used = 0;
    long result = 0;
    int index;

    if (path == 0 || path[0] != '/' || argv == 0 || argc <= 0 || argc > (int)SAVANXP_ELEVATION_MAX_ARGS ||
        strlen(path) >= sizeof(request.path)) {
        return -SAVANXP_EINVAL;
    }
    memset(&request, 0, sizeof(request));
    request.magic = SAVANXP_ELEVATION_MAGIC;
    request.version = SAVANXP_ELEVATION_VERSION;
    request.flags = exit_status != 0 ? SAVANXP_ELEVATION_WAIT : 0u;
    request.argc = (uint32_t)argc;
    memcpy(request.path, path, strlen(path) + 1u);
    for (index = 0; index < argc; ++index) {
        size_t length = argv[index] != 0 ? strlen(argv[index]) : 0u;

        if (argv[index] == 0 || used + length + 1u > sizeof(request.args)) {
            return -SAVANXP_E2BIG;
        }
        memcpy(request.args + used, argv[index], length + 1u);
        used += length + 1u;
    }

    memset(&creator, 0, sizeof(creator));
    result = sxn_port_connect_ex(SAVANXP_ELEVATION_PORT_NAME, channel, &creator);
    if (result < 0) {
        return result;
    }
    /* A broker the system did not start could only lie about the answer. */
    if (creator.euid != 0u) {
        result = -SAVANXP_EACCES;
        goto done;
    }
    if (sxn_write(channel[0], &request, sizeof(request)) != (long)sizeof(request)) {
        result = -SAVANXP_EIO;
        goto done;
    }
    if (sxn_read(channel[1], &reply, sizeof(reply)) != (long)sizeof(reply) ||
        reply.magic != SAVANXP_ELEVATION_MAGIC || reply.record != SAVANXP_ELEVATION_RECORD_STARTED) {
        result = -SAVANXP_EIO;
        goto done;
    }
    result = reply.status;
    if (result > 0 && exit_status != 0) {
        if (sxn_read(channel[1], &reply, sizeof(reply)) != (long)sizeof(reply) ||
            reply.magic != SAVANXP_ELEVATION_MAGIC || reply.record != SAVANXP_ELEVATION_RECORD_EXITED) {
            result = -SAVANXP_EIO;
            goto done;
        }
        *exit_status = reply.status;
    }

done:
    (void)sxn_close(channel[0]);
    (void)sxn_close(channel[1]);
    return result;
}
