#pragma once

/* The pure half of sxn/sxnhttp.h: URLs and the HTTP/1.1 response parser,
 * built from system/runtime/sxnhttp_message.c. No system calls, so the host
 * tests compile it as it is (tests/host/sxnhttp_message_test.c). Every byte
 * handed to the parser came from the network; it is fed in whatever pieces
 * they arrived in and decides the same way however they were cut. See
 * docs/net/HTTP.md.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SXNHTTP_URL_CAP 512        /* a whole URL, as text with its terminator */
#define SXNHTTP_HOST_CAP 256
#define SXNHTTP_HEAD_CAP (16 * 1024) /* status line and headers together */
#define SXNHTTP_TYPE_CAP 128
#define SXNHTTP_LINE_CAP 512       /* a chunk-size line or a trailer line */
#define SXNHTTP_TRAILER_CAP 4096   /* every trailer line together */
#define SXNHTTP_INTERIM_MAX 4      /* 1xx responses skipped before the real one */

/* Parser and loader results besides 0. Shared with sxn/sxnhttp.h. */
enum sxnhttp_error
{
    SXNHTTP_ERROR_BAD_URL = -1,            /* not an http:// URL this client can ask */
    SXNHTTP_ERROR_UNSUPPORTED_SCHEME = -2, /* https:// until validated TLS (N5) */
    SXNHTTP_ERROR_DNS = -3,                /* the name did not resolve: see dns_error */
    SXNHTTP_ERROR_CONNECT = -4,            /* refused, reset or unreachable */
    SXNHTTP_ERROR_TIMEOUT = -5,            /* a step or the whole navigation ran out of time */
    SXNHTTP_ERROR_CANCELLED = -6,
    SXNHTTP_ERROR_BAD_RESPONSE = -7,       /* malformed status line, header or chunk */
    SXNHTTP_ERROR_BAD_FRAMING = -8,        /* contradictory or unusable body framing */
    SXNHTTP_ERROR_TRUNCATED = -9,          /* the connection ended inside the response */
    SXNHTTP_ERROR_HEADERS_TOO_LARGE = -10, /* over SXNHTTP_HEAD_CAP */
    SXNHTTP_ERROR_TOO_MANY_REDIRECTS = -11,
    SXNHTTP_ERROR_BAD_REDIRECT = -12,      /* no Location, or one to file://, https->http, ... */
    SXNHTTP_ERROR_UNSUPPORTED_ENCODING = -13, /* a Content-Encoding other than identity */
    SXNHTTP_ERROR_NETWORK = -14,           /* no socket, a write or read failure */
};

/* ---- URLs ------------------------------------------------------------------ */

struct sxnhttp_url
{
    int https;                    /* the scheme: 0 http, 1 https */
    char host[SXNHTTP_HOST_CAP];  /* lowercased, no brackets, no port */
    uint16_t port;                /* explicit or the scheme's default */
    char target[SXNHTTP_URL_CAP]; /* path and query, "/" at least; no fragment */
};

/* Parses an absolute http:// or https:// URL. 0, or -1 for anything else:
 * another scheme, no host, user information ("user@host"), a bad port, IPv6
 * literals, spaces or control characters. A fragment is dropped. */
int sxnhttp_parse_url(const char *text, struct sxnhttp_url *url);

/* The URL written back: scheme://host[:port]target, the port only when it is
 * not the scheme's default. 0, or -1 when it does not fit. */
int sxnhttp_format_url(const struct sxnhttp_url *url, char *out, size_t capacity);

/* Resolves `reference` against the absolute http(s) URL `base` (RFC 3986,
 * section 5.2): another scheme is copied as it is, "//host/x" keeps the
 * scheme, "/x" the authority, "?q" the path, and a relative path is merged
 * and its "." and ".." segments removed. A fragment is dropped. 0, -1 for a
 * malformed base or reference, -2 when the result does not fit. */
int sxnhttp_resolve(const char *base, const char *reference, char *out, size_t capacity);

/* ---- the response parser ---------------------------------------------------- */

enum sxnhttp_framing
{
    SXNHTTP_FRAMING_NONE,   /* 1xx, 204, 304: no body */
    SXNHTTP_FRAMING_LENGTH, /* Content-Length */
    SXNHTTP_FRAMING_CHUNKED,
    SXNHTTP_FRAMING_CLOSE,  /* the body ends when the connection does */
};

struct sxnhttp_parser
{
    /* In: where the decoded body goes. Past `body_capacity` the parser stops
     * and says the body was cut (`body_cut`): the caller keeps what fits. */
    unsigned char *body;
    size_t body_capacity;

    /* Out, once the head is read. */
    int head_done;
    int version_minor; /* HTTP/1.0 or HTTP/1.1 */
    int status;
    enum sxnhttp_framing framing;
    char content_type[SXNHTTP_TYPE_CAP];
    char location[SXNHTTP_URL_CAP];
    int location_too_long;
    size_t body_length;
    int body_cut;

    /* Owned by the parser. */
    int state;
    int interim;
    char head[SXNHTTP_HEAD_CAP];
    size_t head_length;
    uint64_t remaining;
    char line[SXNHTTP_LINE_CAP];
    size_t line_length;
    size_t trailer_bytes;
};

void sxnhttp_parser_init(struct sxnhttp_parser *parser, unsigned char *body, size_t body_capacity);

/* Feeds the next `length` bytes of the response. 0 when more are needed,
 * 1 when the response is complete (or its body reached the capacity), or a
 * negative enum sxnhttp_error. Bytes after a complete response are ignored. */
long sxnhttp_parser_feed(struct sxnhttp_parser *parser, const unsigned char *bytes, size_t length);

/* The connection ended. 1 when that completes the response (a body that runs
 * to the close, or one already complete), or SXNHTTP_ERROR_TRUNCATED. */
long sxnhttp_parser_finish(struct sxnhttp_parser *parser);

#ifdef __cplusplus
}
#endif
