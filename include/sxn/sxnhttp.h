#pragma once

/* sxnhttp: loading http:// URLs for SUN programs (docs/net/HTTP.md).
 *
 * sxnhttp_get fetches one URL with GET over HTTP/1.1: the name through the
 * client's sxndns resolver, Host, Connection: close and Accept-Encoding:
 * identity, a body framed by Content-Length, chunked or the close, up to five
 * redirects. Malformed, contradictory and truncated responses fail, each
 * with its own error; nothing is guessed. Every wait is monotonic and checks
 * the caller's cancel event first.
 *
 * A client is a plain struct the caller owns (about 17 KiB: the response head
 * is parsed inside it), used by one thread at a time.
 *
 * A body goes into the response's buffer, cut at its capacity, or, with a
 * sink, to the caller piece by piece, of any length: a download. A head
 * callback sees the final response before its body and may stop there, which
 * is how a browser tells a page from a file.
 *
 * https:// is TLS 1.2 or 1.3 through OpenSSL, validated: the chain against
 * the trust store, the name or address, the dates against a clock that can
 * be believed, SNI sent and only http/1.1 offered. A program gets it by
 * linking system/runtime/sxnhttp_tls.c and the OpenSSL port's libraries
 * (DEPENDS libssl libcrypto); without them https:// is
 * SXNHTTP_ERROR_UNSUPPORTED_SCHEME. There is no way to accept a certificate
 * that failed.
 *
 * Built from system/runtime/sxnhttp.c, linked into every native program; the
 * linker drops it from the ones that do not call it.
 */

#include <stddef.h>
#include <stdint.h>

#include "sxn/sxndns.h"

/* The URL helpers and the error values are shared with the pure module. */
#include "sxn/sxnhttp_message.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SXNHTTP_REDIRECT_MAX 5
/* Budgets, in milliseconds, unless the request names others. */
#define SXNHTTP_CONNECT_MS 5000u
#define SXNHTTP_IDLE_MS 10000u
#define SXNHTTP_TOTAL_MS 30000u
#define SXNHTTP_TLS_MS 10000u /* the TLS handshake */
/* The system's trust store: the OpenSSL port's Mozilla roots. */
#define SXNHTTP_TRUST_STORE "/system/ssl/cert.pem"

/* What the progress callback is told, in order. */
enum sxnhttp_phase
{
    SXNHTTP_PHASE_RESOLVING,  /* `host` is being resolved */
    SXNHTTP_PHASE_CONNECTING,
    SXNHTTP_PHASE_WAITING,    /* the request is sent */
    SXNHTTP_PHASE_SECURING,   /* the TLS handshake, https:// */
    SXNHTTP_PHASE_RECEIVING,  /* `received` body bytes so far */
    SXNHTTP_PHASE_REDIRECTED, /* `host` is the next hop's */
};

struct sxnhttp_response;

struct sxnhttp_request
{
    const char *url;      /* absolute http:// */
    long cancel;          /* a manual-reset event, or -1 */
    uint32_t total_ms;    /* 0: SXNHTTP_TOTAL_MS, for the whole navigation */
    uint32_t idle_ms;     /* 0: SXNHTTP_IDLE_MS, between two reads */
    const char *user_agent; /* 0: none sent */
    /* Optional: called on the loading thread as the load advances.
     * `expected` is the body's Content-Length while RECEIVING, when the
     * response gave one, and 0 otherwise. */
    void (*progress)(void *user, enum sxnhttp_phase phase, const char *host, uint64_t received,
                     uint64_t expected);
    void *user;
    /* Optional: called once with the final response's head (after every
     * redirect), before any of its body: status, url, content_type,
     * content_length with SXNHTTP_RESPONSE_LENGTH, attachment and filename
     * are set. 0 reads the body; 1 stops there, and sxnhttp_get returns 0
     * with SXNHTTP_RESPONSE_HEAD_ONLY and no body. */
    int (*head)(void *user, const struct sxnhttp_response *response);
    /* Optional: the body goes here instead of response->body (which may then
     * be 0), in the pieces it is decoded in, for as long as it runs. 0 goes
     * on; a negative value stops the load, and sxnhttp_get returns
     * SXNHTTP_ERROR_SINK. With a sink, total_ms counts only until the head
     * is read: a download's body takes as long as it takes, and idle_ms
     * still holds between two reads. A body cut short is
     * SXNHTTP_ERROR_TRUNCATED, as without a sink: the sink has seen a
     * prefix, which the caller must not keep. */
    long (*sink)(void *user, const unsigned char *bytes, size_t length);
    /* Optional, https://: the roots to trust instead of SXNHTTP_TRUST_STORE,
     * as PEM in memory. For tests, whose CA must never be installed. */
    const char *trust_pem;
    size_t trust_pem_length;
};

/* The response's flags. */
#define SXNHTTP_RESPONSE_CUT 1u       /* the body went past `body_capacity`; what fits is kept */
#define SXNHTTP_RESPONSE_LENGTH 2u    /* the response gave a Content-Length: `content_length` */
#define SXNHTTP_RESPONSE_HEAD_ONLY 4u /* the head callback stopped the load before the body */

struct sxnhttp_response
{
    /* In: where the decoded body goes. */
    unsigned char *body;
    size_t body_capacity;

    /* Out, on success: the last hop's status and headers. */
    int status;
    char url[SXNHTTP_URL_CAP]; /* the final URL, after redirects */
    char content_type[SXNHTTP_TYPE_CAP];
    size_t body_length; /* with a sink, the bytes it took */
    unsigned flags;
    int redirects;
    uint64_t content_length;
    /* Content-Disposition: not "inline", and its file name as the server
     * wrote it, decoded but not cleaned (sxnhttp_file_name cleans it). */
    int attachment;
    char filename[SXNHTTP_FILENAME_CAP];

    /* Out, on SXNHTTP_ERROR_DNS: the resolver's own result. */
    long dns_error;

    /* Out, when the last hop was https://: "TLSv1.3" and the cipher suite;
     * empty for http://. On a TLS error, the verifier's detail. */
    char tls_protocol[16];
    char tls_cipher[64];
    char tls_detail[96];
};

struct sxnhttp_client
{
    struct sxndns_resolver *resolver;
    /* Owned by sxnhttp_get. */
    struct sxnhttp_parser parser;
};

/* Points the client at `resolver`, which it uses for every name. */
void sxnhttp_client_init(struct sxnhttp_client *client, struct sxndns_resolver *resolver);

/* GETs request->url into `response`. 0, or a negative enum sxnhttp_error
 * (sxnhttp_message.h). A 4xx or 5xx is a success: its body is the page. */
long sxnhttp_get(struct sxnhttp_client *client, const struct sxnhttp_request *request,
                 struct sxnhttp_response *response);

/* A short English sentence for a sxnhttp_get result, for a status line. */
const char *sxnhttp_error_text(long result);

#ifdef __cplusplus
}
#endif
