#pragma once

/* audiod's wire protocol (docs/media/AUDIO_DAEMON.md): what a client sends to
 * the mixing daemon, whichever subsystem it belongs to.
 *
 * It belongs to neither subsystem, like wm/wm_protocol.h. POSIX clients reach
 * the daemon with UDP datagrams on the loopback (savanxp/audio_server.h keeps
 * that side); SUN clients, which have no sockets, reach it through the native
 * port SAVANXP_AUDIOD_PORT_NAME (sxn/sxnaudio.h). Both carry the same
 * datagrams; the port only wraps each one in a fixed-size record.
 */

#include <stdint.h>

/* A datagram: this header, in host order (it never leaves the machine),
 * followed by s16le stereo samples at the declared rate, at most
 * SAVANXP_AUDIOD_MAX_FRAMES of them. The kernel refuses UDP payloads above
 * 1024 bytes, so the limit is not negotiable. */
#define SAVANXP_AUDIOD_MAGIC 0x53415544u /* "SAUD" */
#define SAVANXP_AUDIOD_VERSION 1u
#define SAVANXP_AUDIOD_MAX_DATAGRAM 1024u
#define SAVANXP_AUDIOD_HEADER_BYTES 16u
#define SAVANXP_AUDIOD_MAX_FRAMES \
    ((SAVANXP_AUDIOD_MAX_DATAGRAM - SAVANXP_AUDIOD_HEADER_BYTES) / 4u)

struct savanxp_audiod_header {
    uint32_t magic; /* SAVANXP_AUDIOD_MAGIC */
    uint16_t version; /* SAVANXP_AUDIOD_VERSION */
    uint16_t channels; /* 2, the only layout the daemon mixes */
    uint32_t sample_rate_hz; /* must be the device's */
    uint32_t sequence; /* one step per datagram, to count losses */
};

/* Control: another magic on the same channel. No handshake here either: each
 * message is identified by its sender (audio and DECLARE) or by the stream it
 * names (SET_VOLUME). Versioned like the audio. */
#define SAVANXP_AUDIOD_CONTROL_MAGIC 0x53415543u /* "SAUC" */
#define SAVANXP_AUDIOD_CONTROL_VERSION 1u
#define SAVANXP_AUDIOD_NAME_BYTES 32u /* with the NUL; longer names are cut */

enum savanxp_audiod_control_kind {
    SAVANXP_AUDIOD_DECLARE = 1, /* "this is me": names the sender's stream */
    SAVANXP_AUDIOD_SET_VOLUME = 2, /* sets another stream's volume */
    SAVANXP_AUDIOD_LIST = 3, /* asks for the census; answered with REPLYs */
    SAVANXP_AUDIOD_REPLY = 4, /* one stream of the census */
};

struct savanxp_audiod_declare {
    uint32_t magic; /* CONTROL_MAGIC */
    uint16_t version; /* CONTROL_VERSION */
    uint16_t kind; /* DECLARE */
    char name[SAVANXP_AUDIOD_NAME_BYTES];
};

struct savanxp_audiod_set_volume {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; /* SET_VOLUME */
    uint16_t target_port;
    uint8_t volume; /* 0..100 */
    uint8_t reserved;
};

struct savanxp_audiod_list_query {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; /* LIST */
};

struct savanxp_audiod_list_reply {
    uint32_t magic;
    uint16_t version;
    uint16_t kind; /* REPLY */
    uint8_t count; /* streams in the census */
    uint8_t index; /* which one this is */
    uint16_t port;
    uint8_t volume;
    uint8_t reserved;
    char name[SAVANXP_AUDIOD_NAME_BYTES];
};

/* --- The native port --------------------------------------------------------
 *
 * A SUN client connects to this port (sxn_port_connect) and writes records on
 * the request pipe; the daemon answers a LIST with REPLY records on the event
 * pipe. Every record is exactly SAVANXP_AUDIOD_RECORD_BYTES: the kernel pipe
 * holds 8 KiB, eight records, and a non-blocking write of a whole record is
 * all or nothing, so neither side ever sees half of one. A full pipe means
 * the daemon is behind; the client drops the record and counts it, as a UDP
 * client loses a datagram.
 *
 * The daemon gives each connection a stream number in
 * [SAVANXP_AUDIOD_NATIVE_FIRST_PORT, + SAVANXP_AUDIOD_NATIVE_SLOTS), below
 * every port UDP hands out, so SET_VOLUME and the census name a native stream
 * the same way they name a UDP one. Closing the pipe ends the stream. */
#define SAVANXP_AUDIOD_PORT_NAME "audiod"
#define SAVANXP_AUDIOD_RECORD_MAGIC 0x53415550u /* "SAUP" */
#define SAVANXP_AUDIOD_RECORD_BYTES 1024u
#define SAVANXP_AUDIOD_RECORD_PAYLOAD (SAVANXP_AUDIOD_RECORD_BYTES - 8u)
#define SAVANXP_AUDIOD_RECORD_MAX_FRAMES \
    ((SAVANXP_AUDIOD_RECORD_PAYLOAD - SAVANXP_AUDIOD_HEADER_BYTES) / 4u)
#define SAVANXP_AUDIOD_NATIVE_FIRST_PORT 1u
#define SAVANXP_AUDIOD_NATIVE_SLOTS 8u

struct savanxp_audiod_record {
    uint32_t magic; /* SAVANXP_AUDIOD_RECORD_MAGIC */
    uint32_t length; /* bytes of `payload` in use */
    unsigned char payload[SAVANXP_AUDIOD_RECORD_PAYLOAD]; /* one datagram */
};
