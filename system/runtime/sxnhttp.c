#include "sxn/sxnhttp.h"

#include <stdio.h>
#include <string.h>

#define MS_TO_NS 1000000ull

void sxnhttp_client_init(struct sxnhttp_client *client, struct sxndns_resolver *resolver)
{
    if (client == 0)
    {
        return;
    }
    memset(client, 0, sizeof(*client));
    client->resolver = resolver;
}

/* Waits for `interests` on `socket` until `deadline`, the cancel event first.
 * 0, SXNHTTP_ERROR_TIMEOUT or SXNHTTP_ERROR_CANCELLED. */
static long wait_socket(long socket, unsigned interests, long cancel, uint64_t deadline)
{
    struct sxn_io_interest waits[2];
    unsigned long count = 0;
    uint64_t now = sxn_clock_ns();
    long result;

    if (now >= deadline)
    {
        return SXNHTTP_ERROR_TIMEOUT;
    }
    if (cancel >= 0)
    {
        waits[count].handle = (int32_t)cancel;
        waits[count].interests = 0;
        count += 1;
    }
    waits[count].handle = (int32_t)socket;
    waits[count].interests = interests | SXN_IO_ERROR;
    count += 1;
    result = sxn_wait_io(waits, count, (long)((deadline - now + MS_TO_NS - 1) / MS_TO_NS));
    if (result < 0)
    {
        return SXNHTTP_ERROR_TIMEOUT;
    }
    return cancel >= 0 && result == 0 ? SXNHTTP_ERROR_CANCELLED : 0;
}

static uint64_t earlier(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static int cancel_is_set(long cancel)
{
    struct sxn_io_interest wait = {(int32_t)cancel, 0};

    return cancel >= 0 && sxn_wait_io(&wait, 1, 0) == 0;
}

/* One request on one connection, the response parsed into client->parser. */
static long exchange(struct sxnhttp_client *client, const struct sxnhttp_request *request,
                     const struct sxnhttp_url *url, uint32_t ipv4, unsigned char *body,
                     size_t body_capacity, uint64_t total_deadline)
{
    struct sxn_ipv4_endpoint peer;
    char text[SXNHTTP_URL_CAP + SXNHTTP_HOST_CAP + 256];
    const uint64_t idle_ns = (uint64_t)(request->idle_ms != 0 ? request->idle_ms : SXNHTTP_IDLE_MS) *
                             MS_TO_NS;
    uint64_t deadline;
    size_t length;
    size_t sent = 0;
    long socket;
    long result;
    int written;

    socket = sxn_socket_create(SXN_SOCKET_TCP);
    if (socket < 0)
    {
        return SXNHTTP_ERROR_NETWORK;
    }
    memset(&peer, 0, sizeof(peer));
    peer.ipv4 = ipv4;
    peer.port = url->port;
    if (request->progress != 0)
    {
        request->progress(request->user, SXNHTTP_PHASE_CONNECTING, url->host, 0);
    }
    deadline = earlier(sxn_clock_ns() + SXNHTTP_CONNECT_MS * MS_TO_NS, total_deadline);
    result = sxn_socket_connect(socket, &peer);
    while (result == -SAVANXP_EAGAIN)
    {
        long waited = wait_socket(socket, SXN_IO_WRITE, request->cancel, deadline);

        if (waited != 0)
        {
            sxn_close(socket);
            return waited;
        }
        result = sxn_socket_status(socket);
    }
    if (result < 0)
    {
        sxn_close(socket);
        return result == -SAVANXP_ETIMEDOUT ? SXNHTTP_ERROR_TIMEOUT : SXNHTTP_ERROR_CONNECT;
    }

    /* Host carries the port when it is not the default one. */
    if (url->port == 80)
    {
        written = snprintf(text, sizeof(text), "GET %s HTTP/1.1\r\nHost: %s\r\n", url->target, url->host);
    }
    else
    {
        written = snprintf(text, sizeof(text), "GET %s HTTP/1.1\r\nHost: %s:%u\r\n", url->target,
            url->host, (unsigned)url->port);
    }
    if (written > 0 && (size_t)written < sizeof(text) && request->user_agent != 0)
    {
        written += snprintf(text + written, sizeof(text) - (size_t)written, "User-Agent: %s\r\n",
            request->user_agent);
    }
    if (written > 0 && (size_t)written < sizeof(text))
    {
        written += snprintf(text + written, sizeof(text) - (size_t)written,
            "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n");
    }
    if (written <= 0 || (size_t)written >= sizeof(text))
    {
        sxn_close(socket);
        return SXNHTTP_ERROR_BAD_URL;
    }
    length = (size_t)written;
    deadline = earlier(sxn_clock_ns() + SXNHTTP_CONNECT_MS * MS_TO_NS, total_deadline);
    while (sent < length)
    {
        long count = sxn_write(socket, text + sent, length - sent);

        if (count == -SAVANXP_EAGAIN)
        {
            long waited = wait_socket(socket, SXN_IO_WRITE, request->cancel, deadline);

            if (waited != 0)
            {
                sxn_close(socket);
                return waited;
            }
            continue;
        }
        if (count <= 0)
        {
            sxn_close(socket);
            return SXNHTTP_ERROR_NETWORK;
        }
        sent += (size_t)count;
    }

    if (request->progress != 0)
    {
        request->progress(request->user, SXNHTTP_PHASE_WAITING, url->host, 0);
    }
    sxnhttp_parser_init(&client->parser, body, body_capacity);
    result = 0;
    while (result == 0)
    {
        unsigned char buffer[1024];
        long got = sxn_read(socket, buffer, sizeof(buffer));

        if (got == -SAVANXP_EAGAIN)
        {
            long waited = wait_socket(socket, SXN_IO_READ, request->cancel,
                earlier(sxn_clock_ns() + idle_ns, total_deadline));

            if (waited != 0)
            {
                result = waited;
                break;
            }
            continue;
        }
        if (got == 0)
        {
            result = sxnhttp_parser_finish(&client->parser);
            break;
        }
        if (got < 0)
        {
            /* A reset before the response was complete is a truncation: the
             * peer stopped in the middle of it. */
            result = got == -SAVANXP_ECONNRESET ? SXNHTTP_ERROR_TRUNCATED
                   : got == -SAVANXP_ETIMEDOUT ? SXNHTTP_ERROR_TIMEOUT
                                               : SXNHTTP_ERROR_NETWORK;
            break;
        }
        result = sxnhttp_parser_feed(&client->parser, buffer, (size_t)got);
        if (request->progress != 0 && client->parser.head_done)
        {
            request->progress(request->user, SXNHTTP_PHASE_RECEIVING, url->host,
                client->parser.body_length);
        }
    }
    sxn_close(socket);
    return result < 0 ? result : 0;
}

static int is_redirect(int status)
{
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

long sxnhttp_get(struct sxnhttp_client *client, const struct sxnhttp_request *request,
                 struct sxnhttp_response *response)
{
    struct sxnhttp_url url;
    char current[SXNHTTP_URL_CAP];
    uint64_t total_deadline;

    if (client == 0 || client->resolver == 0 || request == 0 || request->url == 0 ||
        response == 0 || response->body == 0)
    {
        return SXNHTTP_ERROR_BAD_URL;
    }
    response->status = 0;
    response->url[0] = '\0';
    response->content_type[0] = '\0';
    response->body_length = 0;
    response->flags = 0;
    response->redirects = 0;
    response->dns_error = 0;
    total_deadline = sxn_clock_ns() +
        (uint64_t)(request->total_ms != 0 ? request->total_ms : SXNHTTP_TOTAL_MS) * MS_TO_NS;

    if (sxnhttp_parse_url(request->url, &url) != 0)
    {
        return SXNHTTP_ERROR_BAD_URL;
    }
    if (url.https)
    {
        return SXNHTTP_ERROR_UNSUPPORTED_SCHEME;
    }
    if (sxnhttp_format_url(&url, current, sizeof(current)) != 0)
    {
        return SXNHTTP_ERROR_BAD_URL;
    }
    for (int hop = 0; hop <= SXNHTTP_REDIRECT_MAX; ++hop)
    {
        uint32_t ipv4 = 0;
        long result;
        struct sxnhttp_url next;
        char resolved[SXNHTTP_URL_CAP];

        if (cancel_is_set(request->cancel))
        {
            return SXNHTTP_ERROR_CANCELLED;
        }
        if (sxn_clock_ns() >= total_deadline)
        {
            return SXNHTTP_ERROR_TIMEOUT;
        }
        if (request->progress != 0)
        {
            request->progress(request->user,
                hop == 0 ? SXNHTTP_PHASE_RESOLVING : SXNHTTP_PHASE_REDIRECTED, url.host, 0);
        }
        result = sxndns_resolve(client->resolver, url.host, request->cancel, &ipv4);
        if (result == SXNDNS_ERROR_CANCELLED)
        {
            return SXNHTTP_ERROR_CANCELLED;
        }
        if (result != 0)
        {
            response->dns_error = result;
            return SXNHTTP_ERROR_DNS;
        }
        if (sxn_clock_ns() >= total_deadline)
        {
            return SXNHTTP_ERROR_TIMEOUT;
        }
        result = exchange(client, request, &url, ipv4, response->body, response->body_capacity,
            total_deadline);
        if (result != 0)
        {
            return result;
        }

        if (!is_redirect(client->parser.status))
        {
            response->status = client->parser.status;
            memcpy(response->url, current, sizeof(response->url));
            memcpy(response->content_type, client->parser.content_type, sizeof(response->content_type));
            response->body_length = client->parser.body_length;
            response->flags = client->parser.body_cut ? SXNHTTP_RESPONSE_CUT : 0;
            response->redirects = hop;
            return 0;
        }

        /* A redirect: only to another http(s) URL, never from https down to
         * http, and never to a local file. */
        if (client->parser.location[0] == '\0' || client->parser.location_too_long ||
            sxnhttp_resolve(current, client->parser.location, resolved, sizeof(resolved)) != 0 ||
            sxnhttp_parse_url(resolved, &next) != 0 || (url.https && !next.https))
        {
            return SXNHTTP_ERROR_BAD_REDIRECT;
        }
        if (next.https)
        {
            return SXNHTTP_ERROR_UNSUPPORTED_SCHEME;
        }
        url = next;
        if (sxnhttp_format_url(&url, current, sizeof(current)) != 0)
        {
            return SXNHTTP_ERROR_BAD_REDIRECT;
        }
        response->redirects = hop + 1;
    }
    return SXNHTTP_ERROR_TOO_MANY_REDIRECTS;
}

const char *sxnhttp_error_text(long result)
{
    switch (result)
    {
    case 0:
        return "Loaded.";
    case SXNHTTP_ERROR_BAD_URL:
        return "That URL is malformed.";
    case SXNHTTP_ERROR_UNSUPPORTED_SCHEME:
        return "HTTPS is not available yet.";
    case SXNHTTP_ERROR_DNS:
        return "The name did not resolve.";
    case SXNHTTP_ERROR_CONNECT:
        return "The server refused the connection.";
    case SXNHTTP_ERROR_TIMEOUT:
        return "The server took too long.";
    case SXNHTTP_ERROR_CANCELLED:
        return "Stopped.";
    case SXNHTTP_ERROR_BAD_RESPONSE:
        return "The server's response is malformed.";
    case SXNHTTP_ERROR_BAD_FRAMING:
        return "The server's response is contradictory.";
    case SXNHTTP_ERROR_TRUNCATED:
        return "The connection dropped before the page ended.";
    case SXNHTTP_ERROR_HEADERS_TOO_LARGE:
        return "The server's headers are too large.";
    case SXNHTTP_ERROR_TOO_MANY_REDIRECTS:
        return "Too many redirects.";
    case SXNHTTP_ERROR_BAD_REDIRECT:
        return "Cannot follow that redirect.";
    case SXNHTTP_ERROR_UNSUPPORTED_ENCODING:
        return "The page is compressed in a way this browser cannot read.";
    case SXNHTTP_ERROR_NETWORK:
        return "The network failed.";
    default:
        return "Loading failed.";
    }
}
