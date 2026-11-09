#pragma once
#include <stddef.h>
#include "sxn/sxn.h"

/* Session cursor settings. The compositor owns decoding and presentation;
 * clients choose a pack, never upload pixels or touch the display device. */
#define SXN_CURSOR_PORT "cursor-settings"
#define SXN_CURSOR_MAGIC 0x53584355u
#define SXN_CURSOR_VERSION 1u
#define SXN_CURSOR_ID_SIZE 24u
#define SXN_CURSOR_QUERY 1u
#define SXN_CURSOR_SELECT 2u
#define SXN_CURSOR_SETTINGS_PATH "/users/user/Settings/cursors.cfg"

struct sxn_cursor_request {
    uint32_t magic, version, operation, reserved;
    char theme[SXN_CURSOR_ID_SIZE];
};
struct sxn_cursor_reply {
    uint32_t magic, version;
    int32_t status, save_status; /* Applied successfully but save_status < 0: session only. */
    char theme[SXN_CURSOR_ID_SIZE];
};

/* One bounded request per connection. Never waits indefinitely for a service. */
static inline long sxncursor_request(uint32_t operation, const char *theme, struct sxn_cursor_reply *reply) {
    struct sxn_cursor_request request = {SXN_CURSOR_MAGIC, SXN_CURSOR_VERSION, operation, 0, {0}};
    struct sxn_identity creator;
    int32_t handles[2];
    unsigned i;
    size_t done = 0;
    uint64_t deadline;
    long result;
    if (!reply) return -SAVANXP_EINVAL;
    result = sxn_port_connect_ex(SXN_CURSOR_PORT, handles, &creator);
    if (result < 0) return result;
    if (creator.euid != 0) { result = -SAVANXP_EACCES; goto close; }
    if (theme) {
        for (i = 0; theme[i] && i + 1 < sizeof(request.theme); ++i) request.theme[i] = theme[i];
        if (theme[i]) { result = -SAVANXP_EINVAL; goto close; }
    }
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
    result = reply->magic == SXN_CURSOR_MAGIC && reply->version == SXN_CURSOR_VERSION &&
        reply->theme[SXN_CURSOR_ID_SIZE-1] == 0 ? reply->status : -SAVANXP_EIO;
close:
    (void)sxn_close(handles[0]); (void)sxn_close(handles[1]);
    return result;
}
static inline long sxncursor_query(struct sxn_cursor_reply *reply) { return sxncursor_request(SXN_CURSOR_QUERY, 0, reply); }
static inline long sxncursor_select(const char *theme, struct sxn_cursor_reply *reply) { return sxncursor_request(SXN_CURSOR_SELECT, theme, reply); }
