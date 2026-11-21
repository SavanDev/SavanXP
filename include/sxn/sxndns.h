#pragma once

/* sxndns: name resolution for SUN programs (docs/net/NETWORKING.md,
 * "Name resolution").
 *
 * A resolver turns a host name into one IPv4 address: numeric IPv4 never
 * touches the network; anything else is an A/IN question over UDP to the
 * resolver's server, retried once, moved to TCP when the reply comes back
 * truncated, with CNAME chains followed. Replies are believed only from the
 * server's own address and port, for this TXID and this question. Answers stay
 * in a small cache for their TTL, at most five minutes.
 *
 * A resolver is a plain struct the caller owns, cache included. It is not
 * locked: one thread uses it at a time (a browser window's network worker,
 * a tool's main thread). Changing its server empties its cache.
 *
 * Built from system/runtime/sxndns.c, linked into every native program; the
 * linker drops it from the ones that do not call it.
 */

#include <stdint.h>

#include "sxn/sxn.h"

#ifdef __cplusplus
extern "C" {
#endif

/* QEMU user-mode networking's DNS; what a resolver asks until told otherwise. */
#define SXNDNS_DEFAULT_SERVER 0x0A000203u /* 10.0.2.3 */
#define SXNDNS_DEFAULT_PORT 53u

#define SXNDNS_CACHE_ENTRIES 16
#define SXNDNS_CACHE_NAME_CAP 256
/* The longest an answer is kept, whatever its TTL says. */
#define SXNDNS_TTL_MAX_SECONDS 300u

/* What sxndns_resolve returns besides 0. Not errno values: a resolver can
 * fail in ways a system call does not (sxndns_error_text names each one). */
enum sxndns_error
{
    SXNDNS_ERROR_NO_SUCH_NAME = -1,   /* the server says the name does not exist */
    SXNDNS_ERROR_NO_ADDRESS = -2,     /* it exists, but has no IPv4 address */
    SXNDNS_ERROR_BAD_NAME = -3,       /* not a host name: empty, too long, bad characters */
    SXNDNS_ERROR_TIMEOUT = -4,        /* no usable reply in time */
    SXNDNS_ERROR_CANCELLED = -5,      /* the cancel event was set */
    SXNDNS_ERROR_SERVER_FAILURE = -6, /* the server answered with an error */
    SXNDNS_ERROR_BAD_REPLY = -7,      /* replies arrived, but malformed or a looping chain */
    SXNDNS_ERROR_NETWORK = -8,        /* no socket, no route, the server unreachable */
};

struct sxndns_cache_entry
{
    char name[SXNDNS_CACHE_NAME_CAP]; /* normalized; empty when the slot is free */
    uint32_t ipv4;
    uint64_t expires_ns; /* sxn_clock_ns */
    uint64_t used_ns;
};

/* Room for one reply over TCP with its length prefix. */
#define SXNDNS_SCRATCH_CAP (4096 + 2)

struct sxndns_resolver
{
    struct sxn_ipv4_endpoint server;
    uint32_t sequence;
    struct sxndns_cache_entry cache[SXNDNS_CACHE_ENTRIES];
    /* Owned by sxndns_resolve: replies land here rather than on the stack of
     * a worker thread. */
    unsigned char scratch[SXNDNS_SCRATCH_CAP];
};

/* Points `resolver` at 10.0.2.3:53 with an empty cache. */
void sxndns_init(struct sxndns_resolver *resolver);

/* Asks `server` from now on. A server other than the current one empties the
 * cache: its answers were the old server's. */
void sxndns_set_server(struct sxndns_resolver *resolver, const struct sxn_ipv4_endpoint *server);

/* Forgets every cached answer. */
void sxndns_flush(struct sxndns_resolver *resolver);

/* Dotted-quad IPv4 ("10.0.2.15") into 0xAABBCCDD. 0, or -EINVAL. */
long sxndns_parse_ipv4(const char *text, uint32_t *ipv4);

/* A server written "a.b.c.d" or "a.b.c.d:port" (the port defaults to 53).
 * 0, or -EINVAL. */
long sxndns_parse_server(const char *text, struct sxn_ipv4_endpoint *server);

/* Resolves `name` into *ipv4. 0 or a negative enum sxndns_error.
 *
 * `cancel` is a manual-reset event (SXN_EVENT_MANUAL_RESET) or -1 for none.
 * Once it is set the call returns SXNDNS_ERROR_CANCELLED at its next wait,
 * closing what it opened; a numeric address or a cached answer is still
 * returned, since neither waits. Budgets are monotonic: up to six seconds for
 * the route to the server, then two attempts of two seconds each for a reply;
 * a truncated reply is asked again over TCP within the same two seconds.
 * Up to four aliases that need another question are followed. */
long sxndns_resolve(struct sxndns_resolver *resolver, const char *name, long cancel, uint32_t *ipv4);

/* A short English sentence for a sxndns_resolve result, for a status line. */
const char *sxndns_error_text(long result);

#ifdef __cplusplus
}
#endif
