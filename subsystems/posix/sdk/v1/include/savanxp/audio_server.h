#pragma once

#include <stddef.h>
#include <stdint.h>
#include "audio/audiod_protocol.h"

/* POSIX playback uses audiod's native object port when available, including
 * systems without a NIC. UDP remains available for older daemons and explicit
 * census/control callers; otherwise playback falls back to /dev/audio0.
 * Native connections provide kernel-authenticated application identities for
 * preferences. Anonymous UDP streams have only session controls. */
#define SAVANXP_AUDIOD_PORT 47830u
/* The kernel short-circuits its configured IPv4 address; it does not implement
 * 127.0.0.1. Native playback does not depend on this network configuration. */
#define SAVANXP_AUDIOD_HOST_IPV4 ((10u << 24) | (0u << 16) | (2u << 8) | 15u)

/* Layout is unchanged for existing binaries. udp_fd also stores a native pipe
 * handle in mode 3; callers must use link_close rather than closing it directly. */
struct sx_audio_server_link {
    int mode; /* 0 undecided, 1 direct, 2 UDP, 3 native */
    int udp_fd; /* transport descriptor/handle, -1 when absent */
    uint32_t sequence;
    char client_name[SAVANXP_AUDIOD_NAME_BYTES]; /* display name and stable role */
    uint64_t last_declare_ms;
};

struct sx_audio_server_entry {
    uint16_t port;
    uint8_t volume;
    char name[SAVANXP_AUDIOD_NAME_BYTES];
};

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize without a transport. memset alone would leave a valid fd 0. */
void sx_audio_server_link_init(struct sx_audio_server_link* link);
/* Close the appropriate transport, retaining the configured application role. */
void sx_audio_server_link_close(struct sx_audio_server_link* link);

/* Explicit legacy UDP sender: stereo S16, at most 1 KiB per datagram.
 * Advances sequence per datagram; 0 on success, negative errno on failure. */
long sx_audio_server_send(
    int socket_fd,
    uint32_t* sequence,
    const int16_t* frames,
    size_t frame_count,
    uint32_t rate_hz);

/* Prefer direct output when idle; EBUSY tries the native daemon and then UDP.
 * Reconnect after daemon EOF/error. bytes must contain whole stereo S16 frames.
 * Returns 1 when submitted, 0 on transient transport pressure, negative errno
 * on a fatal device error. Calls never wait for space in a daemon pipe. */
long sx_audio_server_output(
    struct sx_audio_server_link* link,
    int audio_fd,
    const int16_t* frames,
    size_t bytes,
    uint32_t rate_hz);

/* Set the display name and role. Native output declares it before its next
 * samples; UDP announces it on connection and periodically thereafter. */
void sx_audio_server_set_client_name(
    struct sx_audio_server_link* link,
    const char* name);

/* Legacy UDP controls are available to this single-user system. Set another
 * stream's volume (0..100) by source port; return 0 or negative errno. */
long sx_audio_server_set_volume(
    int socket_fd,
    uint16_t target_port,
    int volume);

/* Collect the UDP census until complete or 200 ms without a response. Returns
 * total entries even if capacity is smaller, or negative errno. */
long sx_audio_server_list(
    int socket_fd,
    struct sx_audio_server_entry* out,
    size_t capacity);

#ifdef __cplusplus
}
#endif
