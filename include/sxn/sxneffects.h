#pragma once
#include <stddef.h>
#include "sxn/sxn.h"

#define SXN_EFFECTS_PORT "effects-settings"
#define SXN_EFFECTS_MAGIC 0x53584546u
#define SXN_EFFECTS_VERSION 1u
#define SXN_EFFECTS_QUERY 1u
#define SXN_EFFECTS_SET 2u
#define SXN_EFFECTS_WINDOWS 1u
#define SXN_EFFECTS_CURSOR 2u
#define SXN_EFFECTS_ALL 3u
/* The session currently runs the uid 1000 desktop, like cursor preferences. */
#define SXN_EFFECTS_PATH "/users/user/Settings/effects.cfg"
struct sxn_effects_request { uint32_t magic, version, operation, effects; };
struct sxn_effects_reply { uint32_t magic, version; int32_t status, save_status; uint32_t effects; };

static inline long sxneffects_request(uint32_t operation, uint32_t effects, struct sxn_effects_reply *reply) {
    struct sxn_effects_request request = {SXN_EFFECTS_MAGIC, SXN_EFFECTS_VERSION, operation, effects};
    struct sxn_identity creator;
    int32_t handles[2];
    size_t done = 0;
    uint64_t deadline;
    long result;
    if (!reply) return -SAVANXP_EINVAL;
    result = sxn_port_connect_ex(SXN_EFFECTS_PORT, handles, &creator);
    if (result < 0) return result;
    if (creator.euid != 0) { result = -SAVANXP_EACCES; goto close; }
    (void)sxn_set_nonblocking(handles[0], 1);
    (void)sxn_set_nonblocking(handles[1], 1);
    result = sxn_write(handles[0], &request, sizeof(request));
    if (result != sizeof(request)) { if (result >= 0) result = -SAVANXP_EIO; goto close; }
    deadline = sxn_clock_ns() + 2000000000ull;
    while (done < sizeof(*reply)) {
        if (sxn_clock_ns() >= deadline) { result = -SAVANXP_ETIMEDOUT; goto close; }
        result = sxn_wait_one(handles[1], 50);
        if (result == -SAVANXP_ETIMEDOUT) continue;
        if (result < 0) goto close;
        result = sxn_read(handles[1], (unsigned char *)reply + done, sizeof(*reply) - done);
        if (result == -SAVANXP_EAGAIN) continue;
        if (result <= 0) { if (!result) result = -SAVANXP_EPIPE; goto close; }
        done += (size_t)result;
    }
    result = reply->magic == SXN_EFFECTS_MAGIC && reply->version == SXN_EFFECTS_VERSION &&
        !(reply->effects & ~SXN_EFFECTS_ALL) && reply->status <= 0 ? reply->status : -SAVANXP_EIO;
close:
    (void)sxn_close(handles[0]); (void)sxn_close(handles[1]);
    return result;
}
