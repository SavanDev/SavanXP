/* The pure half of sxn/sxnhttp.h (system/runtime/sxnhttp_message.c): URLs and
 * their resolution, and the response parser fed every way a TCP stream can
 * cut a response -- whole, split at every byte, one byte at a time -- which
 * must all decide the same. Malformed, contradictory, truncated and
 * oversized responses must fail, each with its own error.
 */

#include "sxn/sxnhttp_message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(condition)                                                        \
    do                                                                          \
    {                                                                           \
        if (!(condition))                                                       \
        {                                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                #condition);                                                    \
            g_failures += 1;                                                    \
        }                                                                       \
    } while (0)

/* ---- URLs ------------------------------------------------------------------ */

static void check_resolve(const char *base, const char *reference, const char *expected)
{
    char out[SXNHTTP_URL_CAP];
    int result = sxnhttp_resolve(base, reference, out, sizeof(out));

    if (result != 0 || strcmp(out, expected) != 0)
    {
        fprintf(stderr, "resolve(%s, %s) = %d '%s', expected '%s'\n", base, reference, result,
            result == 0 ? out : "", expected);
        g_failures += 1;
    }
}

static void test_urls(void)
{
    struct sxnhttp_url url;
    char out[SXNHTTP_URL_CAP];

    CHECK(sxnhttp_parse_url("http://Example.COM", &url) == 0);
    CHECK(strcmp(url.host, "example.com") == 0 && url.port == 80 && strcmp(url.target, "/") == 0);
    CHECK(!url.https);
    CHECK(sxnhttp_parse_url("HTTP://10.0.2.2:8080/a/b?x=1#top", &url) == 0);
    CHECK(strcmp(url.host, "10.0.2.2") == 0 && url.port == 8080);
    CHECK(strcmp(url.target, "/a/b?x=1") == 0);
    CHECK(sxnhttp_parse_url("http://host?q", &url) == 0 && strcmp(url.target, "/?q") == 0);
    CHECK(sxnhttp_parse_url("http://host:/x", &url) == 0 && url.port == 80);
    CHECK(sxnhttp_parse_url("https://secure.savanxp/", &url) == 0 && url.https && url.port == 443);
    CHECK(sxnhttp_format_url(&url, out, sizeof(out)) == 0 && strcmp(out, "https://secure.savanxp/") == 0);
    CHECK(sxnhttp_parse_url("http://h:8080/p", &url) == 0);
    CHECK(sxnhttp_format_url(&url, out, sizeof(out)) == 0 && strcmp(out, "http://h:8080/p") == 0);
    CHECK(sxnhttp_format_url(&url, out, 8) == -1);

    CHECK(sxnhttp_parse_url("ftp://host/", &url) == -1);
    CHECK(sxnhttp_parse_url("file:///etc", &url) == -1);
    CHECK(sxnhttp_parse_url("http://", &url) == -1);
    CHECK(sxnhttp_parse_url("http:///path", &url) == -1);
    CHECK(sxnhttp_parse_url("http://user@host/", &url) == -1);
    CHECK(sxnhttp_parse_url("http://[::1]/", &url) == -1);
    CHECK(sxnhttp_parse_url("http://host:0/", &url) == -1);
    CHECK(sxnhttp_parse_url("http://host:65536/", &url) == -1);
    CHECK(sxnhttp_parse_url("http://host:12a/", &url) == -1);
    CHECK(sxnhttp_parse_url("http://ho st/", &url) == -1);
    CHECK(sxnhttp_parse_url("http://host/a b", &url) == -1);
    CHECK(sxnhttp_parse_url("http://host/\x01", &url) == -1);
    {
        char long_url[SXNHTTP_URL_CAP + 32];

        memcpy(long_url, "http://h/", 9);
        memset(long_url + 9, 'a', sizeof(long_url) - 10);
        long_url[sizeof(long_url) - 1] = '\0';
        CHECK(sxnhttp_parse_url(long_url, &url) == -1);
    }

    /* RFC 3986, 5.4.1, the normal examples, on an http base. */
    check_resolve("http://a/b/c/d;p?q", "g", "http://a/b/c/g");
    check_resolve("http://a/b/c/d;p?q", "./g", "http://a/b/c/g");
    check_resolve("http://a/b/c/d;p?q", "g/", "http://a/b/c/g/");
    check_resolve("http://a/b/c/d;p?q", "/g", "http://a/g");
    check_resolve("http://a/b/c/d;p?q", "//g", "http://g/");
    check_resolve("http://a/b/c/d;p?q", "?y", "http://a/b/c/d;p?y");
    check_resolve("http://a/b/c/d;p?q", "g?y", "http://a/b/c/g?y");
    check_resolve("http://a/b/c/d;p?q", "#s", "http://a/b/c/d;p?q");
    check_resolve("http://a/b/c/d;p?q", "g#s", "http://a/b/c/g");
    check_resolve("http://a/b/c/d;p?q", "", "http://a/b/c/d;p?q");
    check_resolve("http://a/b/c/d;p?q", ".", "http://a/b/c/");
    check_resolve("http://a/b/c/d;p?q", "./", "http://a/b/c/");
    check_resolve("http://a/b/c/d;p?q", "..", "http://a/b/");
    check_resolve("http://a/b/c/d;p?q", "../", "http://a/b/");
    check_resolve("http://a/b/c/d;p?q", "../g", "http://a/b/g");
    check_resolve("http://a/b/c/d;p?q", "../..", "http://a/");
    check_resolve("http://a/b/c/d;p?q", "../../g", "http://a/g");
    /* 5.4.2, the abnormal ones that matter: no climbing above the root. */
    check_resolve("http://a/b/c/d;p?q", "../../../g", "http://a/g");
    check_resolve("http://a/b/c/d;p?q", "/./g", "http://a/g");
    check_resolve("http://a/b/c/d;p?q", "/../g", "http://a/g");
    check_resolve("http://a/b/c/d;p?q", "g.", "http://a/b/c/g.");
    check_resolve("http://a/b/c/d;p?q", "./g/.", "http://a/b/c/g/");
    check_resolve("http://a/b/c/d;p?q", "g;x=1/../y", "http://a/b/c/y");
    /* The port and the scheme survive; another scheme is copied as it is. */
    check_resolve("http://h:8080/dir/page", "other.html", "http://h:8080/dir/other.html");
    check_resolve("http://h:8080/dir/page", "/root", "http://h:8080/root");
    check_resolve("https://h/x", "//other/y", "https://other/y");
    check_resolve("http://h/x", "file:///system/samples/welcome.html",
        "file:///system/samples/welcome.html");
    check_resolve("http://h/x", "mailto:someone", "mailto:someone");
    CHECK(sxnhttp_resolve("not a url", "x", out, sizeof(out)) == -1);
    CHECK(sxnhttp_resolve("http://h/x", "//", out, sizeof(out)) == -1);
    CHECK(sxnhttp_resolve("http://h/x", "y", out, 10) == -2);
}

/* ---- the parser -------------------------------------------------------------- */

struct outcome
{
    long result; /* the last feed's, or finish's after EOF */
    int status;
    size_t length;
    int cut;
    unsigned char body[256];
    char type[SXNHTTP_TYPE_CAP];
    char location[SXNHTTP_URL_CAP];
};

static struct sxnhttp_parser g_parser;

/* Feeds `response` in pieces of the sizes `cuts` gives, then the EOF when
 * `close` says the connection ended there. */
static void run(const char *response, size_t length, const size_t *cuts, size_t cut_count,
    int close, size_t capacity, struct outcome *outcome)
{
    size_t offset = 0;
    size_t piece = 0;
    long result = 0;

    memset(outcome, 0, sizeof(*outcome));
    sxnhttp_parser_init(&g_parser, outcome->body, capacity);
    while (offset < length && result == 0)
    {
        size_t take = piece < cut_count ? cuts[piece++] : length - offset;

        if (take > length - offset)
        {
            take = length - offset;
        }
        result = sxnhttp_parser_feed(&g_parser, (const unsigned char *)response + offset, take);
        offset += take;
    }
    if (result == 0 && close)
    {
        result = sxnhttp_parser_finish(&g_parser);
    }
    outcome->result = result;
    outcome->status = g_parser.status;
    outcome->length = g_parser.body_length;
    outcome->cut = g_parser.body_cut;
    memcpy(outcome->type, g_parser.content_type, sizeof(outcome->type));
    memcpy(outcome->location, g_parser.location, sizeof(outcome->location));
}

static int same(const struct outcome *a, const struct outcome *b)
{
    return a->result == b->result && a->status == b->status && a->length == b->length &&
           a->cut == b->cut && memcmp(a->body, b->body, a->length) == 0 &&
           strcmp(a->type, b->type) == 0 && strcmp(a->location, b->location) == 0;
}

/* The same decision however the stream was cut: whole, at every single split
 * point, one byte at a time. Returns the whole-response outcome. */
static struct outcome expect(const char *response, int close, size_t capacity, long want_result,
    int want_status, const char *want_body)
{
    static size_t ones[4096];
    struct outcome whole;
    struct outcome other;
    size_t length = strlen(response);

    for (size_t index = 0; index < sizeof(ones) / sizeof(ones[0]); ++index)
    {
        ones[index] = 1;
    }
    run(response, length, 0, 0, close, capacity, &whole);
    if (whole.result != want_result || (want_status != 0 && whole.status != want_status) ||
        (want_body != 0 &&
            (whole.length != strlen(want_body) || memcmp(whole.body, want_body, whole.length) != 0)))
    {
        fprintf(stderr, "response %.40s...: result %ld status %d length %zu, want %ld %d '%s'\n",
            response, whole.result, whole.status, whole.length, want_result, want_status,
            want_body != 0 ? want_body : "");
        g_failures += 1;
    }
    for (size_t split = 1; split < length; ++split)
    {
        size_t cuts[1] = {split};

        run(response, length, cuts, 1, close, capacity, &other);
        if (!same(&whole, &other))
        {
            fprintf(stderr, "response %.40s...: split at %zu decides otherwise (%ld vs %ld)\n",
                response, split, other.result, whole.result);
            g_failures += 1;
            break;
        }
    }
    run(response, length, ones, length < 4096 ? length : 4096, close, capacity, &other);
    if (!same(&whole, &other))
    {
        fprintf(stderr, "response %.40s...: byte by byte decides otherwise (%ld vs %ld)\n",
            response, other.result, whole.result);
        g_failures += 1;
    }
    return whole;
}

static void test_bodies(void)
{
    struct outcome got;

    got = expect("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: 11\r\n\r\nhello world",
        0, 256, 1, 200, "hello world");
    CHECK(strcmp(got.type, "text/html; charset=utf-8") == 0);
    /* Bytes after a complete response are not body. */
    expect("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhiEXTRA", 0, 256, 1, 200, "hi");
    /* Repeated identical lengths, and a list of them, agree. */
    expect("HTTP/1.1 200 OK\r\nContent-Length: 3\r\ncontent-length: 3\r\n\r\nabc", 0, 256, 1, 200,
        "abc");
    expect("HTTP/1.1 200 OK\r\nContent-Length: 3, 3\r\n\r\nabc", 0, 256, 1, 200, "abc");
    /* Chunked, with extensions, uppercase hex, a zero-padded size and trailers. */
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
           "5;name=value\r\nhello\r\n1\r\n \r\n00A\r\n0123456789\r\n0\r\nX-Trailer: yes\r\n\r\n",
        0, 256, 1, 200, "hello 0123456789");
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: Chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n", 0, 256, 1,
        200, "abc");
    /* Close-delimited: complete only at the close. */
    expect("HTTP/1.0 200 OK\r\n\r\nto the end", 1, 256, 1, 200, "to the end");
    expect("HTTP/1.0 200 OK\r\n\r\nto the end", 0, 256, 0, 200, 0);
    /* Bare LF line ends, as old servers write them. */
    expect("HTTP/1.0 200 OK\nContent-Length: 4\n\nbody", 0, 256, 1, 200, "body");
    /* No body: 204, 304, and a zero length, whatever else they say. */
    expect("HTTP/1.1 204 No Content\r\nContent-Length: 9\r\n\r\n", 0, 256, 1, 204, "");
    expect("HTTP/1.1 304 Not Modified\r\n\r\n", 0, 256, 1, 304, "");
    expect("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 256, 1, 200, "");
    /* Interim responses are skipped; the real one follows. */
    expect("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 102 Processing\r\n\r\n"
           "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok",
        0, 256, 1, 200, "ok");
    /* An error page is a page: its body is kept. */
    expect("HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n\r\ngone", 0, 256, 1, 404, "gone");
    /* A redirect keeps its Location. */
    got = expect("HTTP/1.1 302 Found\r\nLocation:  /next?x=1 \r\nContent-Length: 0\r\n\r\n", 0, 256,
        1, 302, "");
    CHECK(strcmp(got.location, "/next?x=1") == 0);
    /* A status line with no reason phrase. */
    expect("HTTP/1.1 200\r\nContent-Length: 1\r\n\r\nx", 0, 256, 1, 200, "x");
}

static void test_limits(void)
{
    static char response[SXNHTTP_HEAD_CAP + 256];
    struct outcome got;
    size_t length;

    /* A body past the capacity is cut there, and says so. */
    got = expect("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", 0, 4, 1, 200, "0123");
    CHECK(got.cut);
    got = expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n6\r\nabcdef\r\n0\r\n\r\n", 0,
        4, 1, 200, "abcd");
    CHECK(got.cut);
    got = expect("HTTP/1.0 200 OK\r\n\r\nabcdef", 1, 4, 1, 200, "abcd");
    CHECK(got.cut);
    /* Exactly the capacity is not a cut. */
    got = expect("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabcd", 0, 4, 1, 200, "abcd");
    CHECK(!got.cut);
    got = expect("HTTP/1.0 200 OK\r\n\r\nabcd", 1, 4, 1, 200, "abcd");
    CHECK(!got.cut);

    /* The head may take 16 KiB and no more. */
    length = (size_t)snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nX-Pad: ");
    memset(response + length, 'p', SXNHTTP_HEAD_CAP);
    length += SXNHTTP_HEAD_CAP;
    memcpy(response + length, "\r\n\r\n", 5);
    expect(response, 0, 256, SXNHTTP_ERROR_HEADERS_TOO_LARGE, 0, 0);
    length = (size_t)snprintf(response, sizeof(response), "HTTP/1.1 200 OK\r\nX-Pad: ");
    memset(response + length, 'p', SXNHTTP_HEAD_CAP - 64);
    length += SXNHTTP_HEAD_CAP - 64;
    memcpy(response + length, "\r\nContent-Length: 1\r\n\r\nz", 25);
    expect(response, 0, 256, 1, 200, "z");

    /* Trailers have their own cap. */
    length = (size_t)snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n");
    for (int index = 0; index < 20; ++index)
    {
        length += (size_t)snprintf(response + length, sizeof(response) - length, "X-%02d: %0300d\r\n",
            index, 0);
    }
    snprintf(response + length, sizeof(response) - length, "\r\n");
    expect(response, 0, 256, SXNHTTP_ERROR_HEADERS_TOO_LARGE, 200, 0);

    /* Too many interim responses. */
    expect("HTTP/1.1 100 A\r\n\r\nHTTP/1.1 100 B\r\n\r\nHTTP/1.1 100 C\r\n\r\n"
           "HTTP/1.1 100 D\r\n\r\nHTTP/1.1 100 E\r\n\r\nHTTP/1.1 200 OK\r\n\r\n",
        0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
}

static void test_framing_errors(void)
{
    /* Both a length and chunking: contradictory, never guessed. */
    expect("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n"
           "5\r\nhello\r\n0\r\n\r\n",
        0, 256, SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 5, 6\r\n\r\nhello!", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 5x\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: \r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 99999999999999\r\n\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    /* Codings this client does not decode, and chunked twice. */
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n\r\n", 0,
        256, SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.0 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 0, 0);
    expect("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: 1\r\n\r\nx", 0, 256,
        SXNHTTP_ERROR_UNSUPPORTED_ENCODING, 0, 0);
    /* Chunk framing that lies. */
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_RESPONSE, 200, 0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_RESPONSE, 200, 0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcX\r\n0\r\n\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 200, 0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n123456789\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_FRAMING, 200, 0);
    expect("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3 x\r\nabc\r\n", 0, 256,
        SXNHTTP_ERROR_BAD_RESPONSE, 200, 0);
}

static void test_malformed(void)
{
    expect("HTTP/2 200 OK\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 2000 OK\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 20 OK\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 099 Low\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("<html>no status line</html>\n\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 101 Switching Protocols\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    /* Obsolete line folding, a header without a colon, a name with a space. */
    expect("HTTP/1.1 200 OK\r\nX-A: one\r\n two\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 200 OK\r\nNo colon here\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 200 OK\r\nBad Name: x\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 200 OK\r\n: empty name\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
    expect("HTTP/1.1 200 OK\r\nX-Ctl: a\x01b\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_RESPONSE, 0, 0);
}

static void test_truncated(void)
{
    /* The connection ending anywhere inside a framed response is a failure,
     * never a short page. */
    static const char *const responses[] = {
        "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n01234",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabcde\r\n",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n",
        "HTTP/1.1 200 OK\r\nContent-Le",
        "",
    };

    for (size_t index = 0; index < sizeof(responses) / sizeof(responses[0]); ++index)
    {
        struct outcome got;

        run(responses[index], strlen(responses[index]), 0, 0, 1, 256, &got);
        if (got.result != SXNHTTP_ERROR_TRUNCATED)
        {
            fprintf(stderr, "truncated response %zu: result %ld\n", index, got.result);
            g_failures += 1;
        }
    }
    /* And every cut of a good framed response, closed there, fails too. */
    {
        const char *good = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
        size_t length = strlen(good);

        for (size_t cut = 0; cut < length; ++cut)
        {
            struct outcome got;

            run(good, cut, 0, 0, 1, 256, &got);
            CHECK(got.result == SXNHTTP_ERROR_TRUNCATED);
        }
    }
}

/* Random bytes after a valid head: whatever they are, the parser stays in
 * bounds and decides something. */
static void test_noise(void)
{
    static const char *const heads[] = {
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\n",
        "HTTP/1.1 200 OK\r\n",
    };
    unsigned seed = 12345u;

    for (int round = 0; round < 3000; ++round)
    {
        unsigned char noise[200];
        unsigned char body[16];
        const char *head = heads[round % 3];
        size_t noise_length = (size_t)(round % 200);
        long result;

        for (size_t index = 0; index < noise_length; ++index)
        {
            seed = seed * 1103515245u + 12345u;
            noise[index] = (unsigned char)(seed >> 16);
            if (round % 2 == 0 && (seed >> 8) % 7 == 0)
            {
                noise[index] = (unsigned char)"\r\n0123456789abcdef;:"[(seed >> 4) % 20];
            }
        }
        sxnhttp_parser_init(&g_parser, body, sizeof(body));
        result = sxnhttp_parser_feed(&g_parser, (const unsigned char *)head, strlen(head));
        if (result == 0)
        {
            result = sxnhttp_parser_feed(&g_parser, noise, noise_length);
        }
        if (result == 0)
        {
            result = sxnhttp_parser_finish(&g_parser);
        }
        CHECK(result == 1 || (result < 0 && result >= SXNHTTP_ERROR_NETWORK));
        CHECK(g_parser.body_length <= sizeof(body));
    }
}

/* ---- the sink: a body of any length, handed over in pieces ------------------- */

struct collected
{
    unsigned char bytes[1024];
    size_t length;
    size_t calls;
    size_t stop_after; /* 0: never; else return 1 once this many bytes came */
    long fail_with;    /* returned on the first call when non-zero */
};

static long collect(void *user, const unsigned char *bytes, size_t length)
{
    struct collected *collected = user;

    collected->calls += 1;
    if (collected->fail_with != 0)
    {
        return collected->fail_with;
    }
    if (collected->length + length > sizeof(collected->bytes))
    {
        return SXNHTTP_ERROR_SINK;
    }
    memcpy(collected->bytes + collected->length, bytes, length);
    collected->length += length;
    return collected->stop_after != 0 && collected->length >= collected->stop_after ? 1 : 0;
}

/* Feeds `response` to a parser with a sink, `piece` bytes at a time (0: all). */
static long run_sink(const char *response, size_t piece, int close, struct collected *collected)
{
    size_t length = strlen(response);
    size_t offset = 0;
    long result = 0;

    sxnhttp_parser_init(&g_parser, 0, 0);
    sxnhttp_parser_set_sink(&g_parser, collect, collected);
    while (offset < length && result == 0)
    {
        size_t take = piece == 0 || piece > length - offset ? length - offset : piece;

        result = sxnhttp_parser_feed(&g_parser, (const unsigned char *)response + offset, take);
        offset += take;
    }
    if (result == 0 && close)
    {
        result = sxnhttp_parser_finish(&g_parser);
    }
    return result;
}

static void expect_sink(const char *response, int close, long want_result, const char *want_body)
{
    static const size_t pieces[] = {0, 1, 2, 3, 7, 64};

    for (size_t index = 0; index < sizeof(pieces) / sizeof(pieces[0]); ++index)
    {
        struct collected collected;
        long result;

        memset(&collected, 0, sizeof(collected));
        result = run_sink(response, pieces[index], close, &collected);
        if (result != want_result ||
            (want_body != 0 && (collected.length != strlen(want_body) ||
                                   memcmp(collected.bytes, want_body, collected.length) != 0 ||
                                   g_parser.body_length != collected.length)))
        {
            fprintf(stderr, "sink %.40s... in pieces of %zu: result %ld, %zu bytes\n", response, pieces[index],
                result, collected.length);
            g_failures += 1;
        }
    }
}

static void test_sink(void)
{
    struct collected collected;

    /* Every framing reaches the sink whole, however it was cut, and past
     * any buffer: there is none. */
    expect_sink("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n0123456789", 0, 1, "0123456789");
    expect_sink("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n6\r\nefghij\r\n0\r\n\r\n", 0, 1,
        "abcdefghij");
    expect_sink("HTTP/1.0 200 OK\r\n\r\nto the end", 1, 1, "to the end");
    expect_sink("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 0, 1, "");
    /* Truncation is still truncation: the sink saw a prefix, the feed says so. */
    expect_sink("HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\n0123456789", 1, SXNHTTP_ERROR_TRUNCATED, 0);
    expect_sink("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nabc", 1, SXNHTTP_ERROR_TRUNCATED, 0);

    /* The sink's error is the feed's, and stays: the parser has failed. */
    memset(&collected, 0, sizeof(collected));
    collected.fail_with = SXNHTTP_ERROR_SINK;
    CHECK(run_sink("HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabcd", 0, 0, &collected) == SXNHTTP_ERROR_SINK);
    CHECK(collected.calls == 1);
    CHECK(sxnhttp_parser_feed(&g_parser, (const unsigned char *)"x", 1) == SXNHTTP_ERROR_SINK);
    CHECK(sxnhttp_parser_finish(&g_parser) == SXNHTTP_ERROR_SINK);

    /* A sink that says stop completes the response there, as a cut. */
    memset(&collected, 0, sizeof(collected));
    collected.stop_after = 2;
    CHECK(run_sink("HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nabcdefgh", 1, 0, &collected) == 1);
    CHECK(g_parser.body_cut && collected.length == 2);

    /* Nothing of the head reaches the sink, interim responses included. */
    expect_sink("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 0, 1, "ok");
}

/* ---- Content-Length and Content-Disposition, as the head says them ---------- */

static void head_of(const char *response)
{
    struct collected collected;

    memset(&collected, 0, sizeof(collected));
    (void)run_sink(response, 0, 1, &collected);
}

static void test_head_fields(void)
{
    head_of("HTTP/1.1 200 OK\r\nContent-Length: 1234\r\n\r\n");
    CHECK(g_parser.head_done && g_parser.has_length && g_parser.content_length == 1234);
    head_of("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    CHECK(g_parser.head_done && !g_parser.has_length && g_parser.content_length == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    CHECK(g_parser.has_length && g_parser.content_length == 0);
    /* Up to a tebibyte, minus one. */
    head_of("HTTP/1.1 200 OK\r\nContent-Length: 1099511627775\r\n\r\n");
    CHECK(g_parser.head_done && g_parser.content_length == 1099511627775ull);
    expect("HTTP/1.1 200 OK\r\nContent-Length: 1099511627776\r\n\r\n", 0, 256, SXNHTTP_ERROR_BAD_FRAMING, 0, 0);

    head_of("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    CHECK(!g_parser.attachment && g_parser.filename[0] == '\0');
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: inline\r\nContent-Length: 0\r\n\r\n");
    CHECK(!g_parser.attachment);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: INLINE; filename=page.html\r\nContent-Length: 0\r\n\r\n");
    CHECK(!g_parser.attachment && strcmp(g_parser.filename, "page.html") == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment\r\nContent-Length: 0\r\n\r\n");
    CHECK(g_parser.attachment && g_parser.filename[0] == '\0');
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: Attachment; filename=setup.exe\r\nContent-Length: 0\r\n\r\n");
    CHECK(g_parser.attachment && strcmp(g_parser.filename, "setup.exe") == 0);
    /* Quoted, with escapes, spaces and ';' inside. */
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=\"my \\\"big\\\" file;1.tar.gz\""
            "\r\nContent-Length: 0\r\n\r\n");
    CHECK(strcmp(g_parser.filename, "my \"big\" file;1.tar.gz") == 0);
    /* filename* wins over filename, whichever comes first, UTF-8 only. */
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename*=UTF-8''na%C3%AFve%20file.txt; "
            "filename=\"naive file.txt\"\r\nContent-Length: 0\r\n\r\n");
    CHECK(strcmp(g_parser.filename, "na\xc3\xafve file.txt") == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=plain.txt; "
            "filename*=utf-8'en'%E2%82%AC.txt\r\nContent-Length: 0\r\n\r\n");
    CHECK(strcmp(g_parser.filename, "\xe2\x82\xac.txt") == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=plain.txt; "
            "filename*=iso-8859-1''%E9.txt\r\nContent-Length: 0\r\n\r\n");
    CHECK(strcmp(g_parser.filename, "plain.txt") == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=plain.txt; "
            "filename*=UTF-8''bad%0.txt\r\nContent-Length: 0\r\n\r\n");
    CHECK(strcmp(g_parser.filename, "plain.txt") == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename*=UTF-8''nul%00.txt\r\n"
            "Content-Length: 0\r\n\r\n");
    CHECK(g_parser.filename[0] == '\0');
    /* An unknown type is an attachment (RFC 6266, 4.2). */
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: x-foo; filename=a.bin\r\nContent-Length: 0\r\n\r\n");
    CHECK(g_parser.attachment && strcmp(g_parser.filename, "a.bin") == 0);
    /* Malformed: what came before the fault stands, the response does not fail. */
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=\"unterminated\r\n"
            "Content-Length: 0\r\n\r\n");
    CHECK(g_parser.head_done && g_parser.attachment && g_parser.filename[0] == '\0');
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=a.txt; junk\r\n"
            "Content-Length: 0\r\n\r\n");
    CHECK(g_parser.head_done && strcmp(g_parser.filename, "a.txt") == 0);
    head_of("HTTP/1.1 200 OK\r\nContent-Disposition: ; filename=a.txt\r\nContent-Length: 0\r\n\r\n");
    CHECK(g_parser.head_done && !g_parser.attachment && g_parser.filename[0] == '\0');
    {
        static char response[1024];
        char name[400];

        memset(name, 'n', sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\nContent-Disposition: attachment; filename=%s\r\nContent-Length: 0\r\n\r\n", name);
        head_of(response);
        CHECK(g_parser.head_done && g_parser.attachment && g_parser.filename[0] == '\0');
    }
    /* Each response says its own: a redirect's disposition does not carry over. */
    head_of("HTTP/1.1 100 Continue\r\nContent-Disposition: attachment; filename=a\r\n\r\n"
            "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
    CHECK(!g_parser.attachment && g_parser.filename[0] == '\0');
}

static void check_name(const char *suggested, const char *url, size_t capacity, const char *expected)
{
    char out[SXNHTTP_FILENAME_CAP];
    int result = sxnhttp_file_name(suggested, url, out, capacity);

    if (result != 0 || strcmp(out, expected) != 0)
    {
        fprintf(stderr, "file_name(%s, %s, %zu) = %d '%s', expected '%s'\n", suggested ? suggested : "(null)",
            url ? url : "(null)", capacity, result, result == 0 ? out : "", expected);
        g_failures += 1;
    }
}

static void test_file_names(void)
{
    char out[64];

    check_name("setup.exe", "http://h/x", 256, "setup.exe");
    check_name(0, "http://h/dist/savanxp-1.0.iso?mirror=3#top", 256, "savanxp-1.0.iso");
    check_name("", "http://h/a%20b%2Fc.txt", 256, "c.txt");
    check_name(0, "http://h/files/na%C3%AFve.txt", 256, "na\xc3\xafve.txt");
    check_name(0, "http://h/", 256, "download");
    check_name(0, "http://h/dir/", 256, "download");
    check_name(0, "http://h", 256, "download");
    check_name(0, 0, 256, "download");
    check_name(0, "not a url", 256, "download");
    /* No directories, no control characters, nothing reserved. */
    check_name("../../etc/passwd", "http://h/x", 256, "passwd");
    check_name("C:\\Windows\\evil.dll", "http://h/x", 256, "evil.dll");
    check_name("a\x01" "b\x7f" "c.txt", "http://h/x", 256, "abc.txt");
    check_name("what?<is>:\"this\"|*.txt", "http://h/x", 256, "what__is___this___.txt");
    /* No hidden files, no names that end in a dot or a space. */
    check_name(".bashrc", "http://h/x", 256, "bashrc");
    check_name(" . .hidden. . ", "http://h/x", 256, "hidden");
    check_name("..", "http://h/fallback.bin", 256, "fallback.bin");
    check_name("/", "http://h/fallback.bin", 256, "fallback.bin");
    check_name(0, "http://h/%2e%2e", 256, "download");
    check_name(0, "http://h/%00%01", 256, "download");
    /* Long names lose their stem, keep their extension, never split UTF-8. */
    check_name("abcdefghijklmnopqrstuvwxyz.tar", "http://h/x", 16, "abcdefghijk.tar");
    check_name("abcdefghijklmnopqrstuvwxyz", "http://h/x", 16, "abcdefghijklmno");
    check_name("aaaaaaaaaaaa\xc3\xa9\xc3\xa9\xc3\xa9.txt", "http://h/x", 16, "aaaaaaaaaaa.txt");
    check_name("aaaaaaaaaa\xc3\xa9\xc3\xa9\xc3\xa9.txt", "http://h/x", 16, "aaaaaaaaaa.txt");
    check_name("abcdefghijklmnopqrstuvwxyz.averyveryverylongextension", "http://h/x", 16, "abcdefghijklmno");
    CHECK(sxnhttp_file_name("a", "http://h/", out, 15) == -1);
    CHECK(sxnhttp_file_name("a", "http://h/", 0, 64) == -1);
}

int main(void)
{
    test_urls();
    test_bodies();
    test_limits();
    test_framing_errors();
    test_malformed();
    test_truncated();
    test_noise();
    test_sink();
    test_head_fields();
    test_file_names();
    if (g_failures != 0)
    {
        fprintf(stderr, "sxnhttp-message-test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("sxnhttp-message-test: OK\n");
    return 0;
}
