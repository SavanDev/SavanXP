#pragma once

/* What sxnhttp.c asks of a TLS implementation (docs/net/HTTP.md, "HTTPS").
 *
 * The session never touches the socket: sxnhttp moves the bytes, with its own
 * deadlines, waits and cancel event, and the session turns them into
 * plaintext and back through memory buffers. sxnhttp_tls.c implements it
 * with OpenSSL; a program that does not link it gets the weak
 * sxnhttp_tls_backend() of sxnhttp.c, which has none, and https:// is
 * SXNHTTP_ERROR_UNSUPPORTED_SCHEME there. */

#include <stddef.h>

#include "sxn/sxnhttp.h"

/* Returned by handshake and read: give the session more of the peer's bytes
 * (sxnhttp_tls_backend.put) after sending whatever it has (take). */
#define SXNHTTP_TLS_WANT_INPUT 1

struct sxnhttp_tls_backend
{
    /* Before anything is sent: 0, SXNHTTP_ERROR_TLS_ENTROPY or
     * SXNHTTP_ERROR_TLS_CLOCK. */
    long (*ready)(void);
    /* A client session for `host` (a name, sent as SNI and checked against
     * the certificate, or an IPv4 address, checked as one), trusting
     * `trust_pem` or the system store. 0 and *session, or a negative
     * enum sxnhttp_error. */
    long (*open)(const char *host, const char *trust_pem, size_t trust_pem_length, void **session);
    void (*close)(void *session);
    /* The peer's bytes, all of them taken. 0, or a negative error. */
    long (*put)(void *session, const void *data, size_t length);
    /* Bytes for the peer: how many were copied to `out`, 0 when none wait. */
    size_t (*take)(void *session, void *out, size_t capacity);
    /* 0 once secured, SXNHTTP_TLS_WANT_INPUT, or a negative error, with what
     * the verifier said in `detail`. */
    long (*handshake)(void *session, char *detail, size_t detail_capacity);
    /* Plaintext: `length` bytes accepted whole, or a negative error. */
    long (*write)(void *session, const void *data, size_t length);
    /* Plaintext: >0 bytes, 0 at the peer's close_notify,
     * SXNHTTP_TLS_WANT_INPUT, or a negative error. */
    long (*read)(void *session, void *out, size_t capacity);
    /* The socket ended without a close_notify: 0 if the session agrees the
     * stream is over, or SXNHTTP_ERROR_TRUNCATED. */
    long (*end_of_input)(void *session);
    /* After the handshake: the protocol and cipher names. */
    void (*describe)(void *session, char *protocol, size_t protocol_capacity, char *cipher,
                     size_t cipher_capacity);
};

/* The program's TLS, or 0. Weak in sxnhttp.c, defined by sxnhttp_tls.c. */
const struct sxnhttp_tls_backend *sxnhttp_tls_backend(void);
