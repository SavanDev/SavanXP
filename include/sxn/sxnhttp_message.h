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
#define SXNHTTP_FILENAME_CAP 256   /* Content-Disposition's file name, decoded, with its terminator */

/* Parser and loader results besides 0. Shared with sxn/sxnhttp.h. */
enum sxnhttp_error
{
    SXNHTTP_ERROR_BAD_URL = -1,            /* not an http:// URL this client can ask */
    SXNHTTP_ERROR_UNSUPPORTED_SCHEME = -2, /* https:// in a program built without TLS */
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
    /* https:// (docs/net/HTTP.md, "HTTPS"). Each says what to fix; none can
     * be overridden. */
    SXNHTTP_ERROR_TLS_ENTROPY = -15,   /* the kernel generator is not seeded */
    SXNHTTP_ERROR_TLS_CLOCK = -16,     /* the date cannot be believed (sxn_clock_status) */
    SXNHTTP_ERROR_TLS_STORE = -17,     /* no trust store to check against */
    SXNHTTP_ERROR_TLS_UNTRUSTED = -18, /* the chain leads to no trusted root, or is broken */
    SXNHTTP_ERROR_TLS_NAME = -19,      /* the certificate is for another name or address */
    SXNHTTP_ERROR_TLS_DATE = -20,      /* the certificate is expired or not yet valid */
    SXNHTTP_ERROR_TLS_HANDSHAKE = -21, /* TLS itself failed: version, alert, bad record */
    SXNHTTP_ERROR_SINK = -22,          /* the caller's body sink refused a piece (a write failed) */
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

/* A name to save a download under, at most `capacity` - 1 bytes: `suggested`
 * (Content-Disposition's file name, or NULL) when anything is left of it
 * once cleaned, else the last path segment of `url`, percent-decoded, else
 * "download". Cleaning keeps only what follows the last '/' or '\\', drops
 * control characters, replaces < > : " | ? * with '_', strips leading dots
 * and spaces and trailing dots and spaces, and shortens a long name inside
 * its stem, keeping its extension and whole UTF-8 sequences. 0, or -1 when
 * `capacity` is below 16. */
int sxnhttp_file_name(const char *suggested, const char *url, char *out, size_t capacity);

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
    /* In, optional (sxnhttp_parser_set_sink): the body handed over piece by
     * piece instead, of any length. The sink returns 0 to go on, 1 to stop
     * there as if the buffer were full (the response completes, `body_cut`
     * set), or a negative enum sxnhttp_error the feed fails with.
     * `body_length` counts the bytes the sink took. */
    long (*sink)(void *user, const unsigned char *bytes, size_t length);
    void *sink_user;

    /* Out, once the head is read. */
    int head_done;
    int version_minor; /* HTTP/1.0 or HTTP/1.1 */
    int status;
    enum sxnhttp_framing framing;
    char content_type[SXNHTTP_TYPE_CAP];
    char location[SXNHTTP_URL_CAP];
    int location_too_long;
    int has_length;          /* Content-Length was given: `content_length` */
    uint64_t content_length;
    /* Content-Disposition: anything but "inline" is an attachment (RFC 6266,
     * unknown types included); `filename` is its filename* (UTF-8) or else
     * its filename, decoded, "" when absent or unusable. Never sanitized:
     * sxnhttp_file_name does that. A malformed header is ignored, not an
     * error: it says nothing about the framing. */
    int attachment;
    char filename[SXNHTTP_FILENAME_CAP];
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

/* Sends the body to `sink` instead of the buffer; call after init. */
void sxnhttp_parser_set_sink(struct sxnhttp_parser *parser,
                             long (*sink)(void *user, const unsigned char *bytes, size_t length), void *user);

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
