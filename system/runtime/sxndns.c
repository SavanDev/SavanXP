#include "sxn/sxndns.h"

#include <string.h>

#include "sxndns_message.h"

/* The budgets sxn/sxndns.h promises, in nanoseconds. */
#define SXNDNS_ROUTE_NS 6000000000ull
#define SXNDNS_REPLY_NS 2000000000ull
#define SXNDNS_ATTEMPTS 2
/* Further questions asked to follow an alias the server did not resolve. */
#define SXNDNS_ALIAS_QUERIES 4

_Static_assert(SXNDNS_SCRATCH_CAP == SXNDNS_TCP_MESSAGE_CAP + 2, "scratch holds one TCP reply");
_Static_assert(SXNDNS_CACHE_NAME_CAP == SXNDNS_NAME_CAP, "a cached name is a normalized name");

void sxndns_init(struct sxndns_resolver *resolver)
{
    if (resolver == 0)
    {
        return;
    }
    memset(resolver, 0, sizeof(*resolver));
    resolver->server.ipv4 = SXNDNS_DEFAULT_SERVER;
    resolver->server.port = SXNDNS_DEFAULT_PORT;
}

void sxndns_flush(struct sxndns_resolver *resolver)
{
    if (resolver == 0)
    {
        return;
    }
    memset(resolver->cache, 0, sizeof(resolver->cache));
}

void sxndns_set_server(struct sxndns_resolver *resolver, const struct sxn_ipv4_endpoint *server)
{
    if (resolver == 0 || server == 0)
    {
        return;
    }
    if (resolver->server.ipv4 != server->ipv4 || resolver->server.port != server->port)
    {
        sxndns_flush(resolver);
    }
    resolver->server.ipv4 = server->ipv4;
    resolver->server.port = server->port;
    resolver->server.reserved = 0;
}

static int parse_decimal(const char *text, size_t length, unsigned limit, unsigned *value)
{
    unsigned result = 0;

    if (length == 0 || length > 5)
    {
        return -1;
    }
    for (size_t index = 0; index < length; ++index)
    {
        if (text[index] < '0' || text[index] > '9')
        {
            return -1;
        }
        result = result * 10u + (unsigned)(text[index] - '0');
    }
    if (result > limit)
    {
        return -1;
    }
    *value = result;
    return 0;
}

long sxndns_parse_ipv4(const char *text, uint32_t *ipv4)
{
    uint32_t result = 0;
    const char *cursor = text;

    if (text == 0 || ipv4 == 0)
    {
        return -SAVANXP_EINVAL;
    }
    for (int part = 0; part < 4; ++part)
    {
        const char *start = cursor;
        unsigned value = 0;

        while (*cursor >= '0' && *cursor <= '9')
        {
            cursor += 1;
        }
        if (cursor - start > 3 || parse_decimal(start, (size_t)(cursor - start), 255u, &value) != 0)
        {
            return -SAVANXP_EINVAL;
        }
        result = (result << 8) | value;
        if (part < 3)
        {
            if (*cursor != '.')
            {
                return -SAVANXP_EINVAL;
            }
            cursor += 1;
        }
    }
    if (*cursor != '\0')
    {
        return -SAVANXP_EINVAL;
    }
    *ipv4 = result;
    return 0;
}

long sxndns_parse_server(const char *text, struct sxn_ipv4_endpoint *server)
{
    char host[16];
    const char *colon;
    size_t host_length;
    unsigned port = SXNDNS_DEFAULT_PORT;
    uint32_t ipv4 = 0;

    if (text == 0 || server == 0)
    {
        return -SAVANXP_EINVAL;
    }
    colon = strchr(text, ':');
    host_length = colon != 0 ? (size_t)(colon - text) : strlen(text);
    if (host_length == 0 || host_length >= sizeof(host))
    {
        return -SAVANXP_EINVAL;
    }
    if (colon != 0 && (parse_decimal(colon + 1, strlen(colon + 1), 65535u, &port) != 0 || port == 0))
    {
        return -SAVANXP_EINVAL;
    }
    memcpy(host, text, host_length);
    host[host_length] = '\0';
    if (sxndns_parse_ipv4(host, &ipv4) != 0)
    {
        return -SAVANXP_EINVAL;
    }
    server->ipv4 = ipv4;
    server->port = (uint16_t)port;
    server->reserved = 0;
    return 0;
}

/* ---- waiting ---------------------------------------------------------------- */

/* Waits for `interests` on `socket` until `deadline`, the cancel event first
 * so that it wins over a ready socket. 0, CANCELLED or TIMEOUT. */
static long wait_socket(long socket, unsigned interests, long cancel, uint64_t deadline)
{
    struct sxn_io_interest waits[2];
    unsigned long count = 0;
    uint64_t now = sxn_clock_ns();
    long result;

    if (now >= deadline)
    {
        return SXNDNS_ERROR_TIMEOUT;
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
    result = sxn_wait_io(waits, count, (long)((deadline - now + 999999u) / 1000000u));
    if (result == -SAVANXP_ETIMEDOUT)
    {
        return SXNDNS_ERROR_TIMEOUT;
    }
    if (result < 0)
    {
        return SXNDNS_ERROR_NETWORK;
    }
    return cancel >= 0 && result == 0 ? SXNDNS_ERROR_CANCELLED : 0;
}

static int cancel_is_set(long cancel)
{
    struct sxn_io_interest wait = {(int32_t)cancel, 0};

    return cancel >= 0 && sxn_wait_io(&wait, 1, 0) == 0;
}

static uint16_t next_txid(struct sxndns_resolver *resolver)
{
    /* No kernel RNG yet: the clock, a counter and the resolver's address
     * mixed. Enough against accidents and stale replies, not an adversary;
     * the source check and the echoed question carry the rest. */
    uint64_t mix = sxn_clock_ns() ^ ((uint64_t)(unsigned long)resolver >> 4);

    resolver->sequence += 1;
    mix ^= (uint64_t)resolver->sequence * 0x9E3779B97F4A7C15ull;
    mix ^= mix >> 29;
    return (uint16_t)(mix ^ (mix >> 16) ^ (mix >> 32));
}

/* ---- one question -------------------------------------------------------------- */

/* Asks the truncated question again over TCP before `deadline`. */
static long ask_over_tcp(struct sxndns_resolver *resolver, const unsigned char *query,
                         size_t query_length, long cancel, uint64_t deadline,
                         struct sxndns_answer *answer)
{
    unsigned char *buffer = resolver->scratch;
    size_t have = 0;
    size_t sent = 0;
    long socket = sxn_socket_create(SXN_SOCKET_TCP);
    long result;
    long frame = 0;

    if (socket < 0)
    {
        return SXNDNS_ERROR_NETWORK;
    }
    result = sxn_socket_connect(socket, &resolver->server);
    while (result == -SAVANXP_EAGAIN)
    {
        long waited = wait_socket(socket, SXN_IO_WRITE, cancel, deadline);

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
        return SXNDNS_ERROR_NETWORK;
    }

    buffer[0] = (unsigned char)(query_length >> 8);
    buffer[1] = (unsigned char)(query_length & 0xFF);
    memcpy(buffer + 2, query, query_length);
    while (sent < query_length + 2)
    {
        long written = sxn_write(socket, buffer + sent, query_length + 2 - sent);

        if (written == -SAVANXP_EAGAIN)
        {
            long waited = wait_socket(socket, SXN_IO_WRITE, cancel, deadline);

            if (waited != 0)
            {
                sxn_close(socket);
                return waited;
            }
            continue;
        }
        if (written <= 0)
        {
            sxn_close(socket);
            return SXNDNS_ERROR_NETWORK;
        }
        sent += (size_t)written;
    }

    /* The reply may arrive in any number of pieces, its length prefix split
     * too; only a whole frame is parsed. */
    while ((frame = sxndns_tcp_frame(buffer, have, SXNDNS_SCRATCH_CAP)) == 0)
    {
        long got;

        if (have == SXNDNS_SCRATCH_CAP)
        {
            frame = -1;
            break;
        }
        got = sxn_read(socket, buffer + have, SXNDNS_SCRATCH_CAP - have);
        if (got == -SAVANXP_EAGAIN)
        {
            long waited = wait_socket(socket, SXN_IO_READ, cancel, deadline);

            if (waited != 0)
            {
                sxn_close(socket);
                return waited;
            }
            continue;
        }
        if (got == 0)
        {
            frame = -1; /* the connection ended inside the reply */
            break;
        }
        if (got < 0)
        {
            sxn_close(socket);
            return SXNDNS_ERROR_NETWORK;
        }
        have += (size_t)got;
    }
    sxn_close(socket);
    if (frame < 0 ||
        sxndns_parse_reply(buffer + 2, (size_t)frame, query, query_length, answer) != 0 ||
        answer->outcome == SXNDNS_OUTCOME_TRUNCATED)
    {
        return SXNDNS_ERROR_BAD_REPLY;
    }
    return 0;
}

/* Asks one A/IN question about `name` (normalized). 0 with `answer` filled,
 * or a negative enum sxndns_error. */
static long ask(struct sxndns_resolver *resolver, const char *name, long cancel,
                struct sxndns_answer *answer)
{
    unsigned char query[SXNDNS_UDP_MESSAGE_CAP];
    size_t query_length = 0;
    uint64_t route_deadline = sxn_clock_ns() + SXNDNS_ROUTE_NS;
    struct sxn_ipv4_endpoint local;
    long socket;
    long result = SXNDNS_ERROR_TIMEOUT;
    int saw_bad = 0;

    socket = sxn_socket_create(SXN_SOCKET_UDP);
    if (socket < 0)
    {
        return SXNDNS_ERROR_NETWORK;
    }
    memset(&local, 0, sizeof(local));
    if (sxn_socket_bind(socket, &local) < 0)
    {
        sxn_close(socket);
        return SXNDNS_ERROR_NETWORK;
    }
    for (int attempt = 0; attempt < SXNDNS_ATTEMPTS; ++attempt)
    {
        uint64_t reply_deadline;
        long sent;

        if (sxndns_encode_query(name, next_txid(resolver), query, sizeof(query), &query_length) != 0)
        {
            result = SXNDNS_ERROR_BAD_NAME;
            break;
        }
        /* The first datagram may wait for the route (NIC setup, ARP). */
        while ((sent = sxn_socket_sendto(socket, query, query_length, &resolver->server)) ==
               -SAVANXP_EAGAIN)
        {
            long waited = wait_socket(socket, SXN_IO_WRITE, cancel, route_deadline);

            if (waited != 0)
            {
                sent = waited == SXNDNS_ERROR_TIMEOUT ? SXNDNS_ERROR_NETWORK : waited;
                break;
            }
        }
        if (sent == SXNDNS_ERROR_CANCELLED)
        {
            result = SXNDNS_ERROR_CANCELLED;
            break;
        }
        if (sent < 0)
        {
            result = SXNDNS_ERROR_NETWORK;
            break;
        }

        reply_deadline = sxn_clock_ns() + SXNDNS_REPLY_NS;
        for (;;)
        {
            struct sxn_ipv4_endpoint from;
            long got = sxn_socket_recvfrom(socket, resolver->scratch, SXNDNS_UDP_MESSAGE_CAP, &from);
            int parsed;

            if (got == -SAVANXP_EAGAIN)
            {
                long waited = wait_socket(socket, SXN_IO_READ, cancel, reply_deadline);

                if (waited == SXNDNS_ERROR_TIMEOUT)
                {
                    break; /* the next attempt, with a new TXID */
                }
                if (waited != 0)
                {
                    sxn_close(socket);
                    return waited;
                }
                continue;
            }
            if (got < 0)
            {
                sxn_close(socket);
                return SXNDNS_ERROR_NETWORK;
            }
            /* Only the server's own address and port is believed. */
            if (from.ipv4 != resolver->server.ipv4 || from.port != resolver->server.port)
            {
                continue;
            }
            parsed = sxndns_parse_reply(resolver->scratch, (size_t)got, query, query_length, answer);
            if (parsed == -1)
            {
                continue;
            }
            if (parsed != 0)
            {
                saw_bad = 1; /* keep waiting: the real reply may still come */
                continue;
            }
            sxn_close(socket);
            if (answer->outcome == SXNDNS_OUTCOME_TRUNCATED)
            {
                return ask_over_tcp(resolver, query, query_length, cancel, reply_deadline, answer);
            }
            return 0;
        }
    }
    sxn_close(socket);
    if (result == SXNDNS_ERROR_TIMEOUT && saw_bad)
    {
        result = SXNDNS_ERROR_BAD_REPLY;
    }
    return result;
}

/* ---- the cache -------------------------------------------------------------- */

static struct sxndns_cache_entry *cache_find(struct sxndns_resolver *resolver, const char *name,
                                             uint64_t now)
{
    for (int index = 0; index < SXNDNS_CACHE_ENTRIES; ++index)
    {
        struct sxndns_cache_entry *entry = &resolver->cache[index];

        if (entry->name[0] != '\0' && strcmp(entry->name, name) == 0)
        {
            if (now < entry->expires_ns)
            {
                return entry;
            }
            entry->name[0] = '\0'; /* expired */
        }
    }
    return 0;
}

static void cache_store(struct sxndns_resolver *resolver, const char *name, uint32_t ipv4,
                        uint32_t ttl, uint64_t now)
{
    struct sxndns_cache_entry *slot = 0;

    if (ttl == 0)
    {
        return;
    }
    if (ttl > SXNDNS_TTL_MAX_SECONDS)
    {
        ttl = SXNDNS_TTL_MAX_SECONDS;
    }
    /* The same name's slot, else a free or expired one, else the one used
     * longest ago. */
    for (int index = 0; index < SXNDNS_CACHE_ENTRIES && slot == 0; ++index)
    {
        if (resolver->cache[index].name[0] != '\0' && strcmp(resolver->cache[index].name, name) == 0)
        {
            slot = &resolver->cache[index];
        }
    }
    for (int index = 0; index < SXNDNS_CACHE_ENTRIES && slot == 0; ++index)
    {
        if (resolver->cache[index].name[0] == '\0' || now >= resolver->cache[index].expires_ns)
        {
            slot = &resolver->cache[index];
        }
    }
    if (slot == 0)
    {
        slot = &resolver->cache[0];
        for (int index = 1; index < SXNDNS_CACHE_ENTRIES; ++index)
        {
            if (resolver->cache[index].used_ns < slot->used_ns)
            {
                slot = &resolver->cache[index];
            }
        }
    }
    memcpy(slot->name, name, SXNDNS_CACHE_NAME_CAP);
    slot->ipv4 = ipv4;
    slot->expires_ns = now + (uint64_t)ttl * 1000000000ull;
    slot->used_ns = now;
}

/* ---- resolving ---------------------------------------------------------------- */

long sxndns_resolve(struct sxndns_resolver *resolver, const char *name, long cancel, uint32_t *ipv4)
{
    char asked[SXNDNS_NAME_CAP];
    char current[SXNDNS_NAME_CAP];
    struct sxndns_answer answer;
    struct sxndns_cache_entry *cached;
    uint32_t ttl = 0xFFFFFFFFu;
    uint64_t now;

    if (resolver == 0 || ipv4 == 0 || name == 0)
    {
        return SXNDNS_ERROR_BAD_NAME;
    }
    if (sxndns_parse_ipv4(name, ipv4) == 0)
    {
        return 0;
    }
    if (sxndns_normalize_name(name, asked, sizeof(asked)) != 0)
    {
        return SXNDNS_ERROR_BAD_NAME;
    }
    now = sxn_clock_ns();
    cached = cache_find(resolver, asked, now);
    if (cached != 0)
    {
        cached->used_ns = now;
        *ipv4 = cached->ipv4;
        return 0;
    }
    if (cancel_is_set(cancel))
    {
        return SXNDNS_ERROR_CANCELLED;
    }

    memcpy(current, asked, sizeof(current));
    for (int question = 0; question <= SXNDNS_ALIAS_QUERIES; ++question)
    {
        long result = ask(resolver, current, cancel, &answer);

        if (result != 0)
        {
            return result;
        }
        switch (answer.outcome)
        {
        case SXNDNS_OUTCOME_ADDRESS:
            if (answer.ttl < ttl)
            {
                ttl = answer.ttl;
            }
            *ipv4 = answer.ipv4;
            cache_store(resolver, asked, answer.ipv4, ttl, sxn_clock_ns());
            return 0;
        case SXNDNS_OUTCOME_ALIAS:
            if (answer.ttl < ttl)
            {
                ttl = answer.ttl;
            }
            /* The alias came off the wire: it must be a name we would ask. */
            if (sxndns_normalize_name(answer.alias, current, sizeof(current)) != 0)
            {
                return SXNDNS_ERROR_BAD_REPLY;
            }
            continue;
        case SXNDNS_OUTCOME_NO_SUCH_NAME:
            return SXNDNS_ERROR_NO_SUCH_NAME;
        case SXNDNS_OUTCOME_NO_ADDRESS:
            return SXNDNS_ERROR_NO_ADDRESS;
        case SXNDNS_OUTCOME_SERVER_FAILURE:
            return SXNDNS_ERROR_SERVER_FAILURE;
        default:
            return SXNDNS_ERROR_BAD_REPLY;
        }
    }
    return SXNDNS_ERROR_BAD_REPLY; /* an alias chain longer than we follow */
}

const char *sxndns_error_text(long result)
{
    switch (result)
    {
    case 0:
        return "Resolved.";
    case SXNDNS_ERROR_NO_SUCH_NAME:
        return "No such host.";
    case SXNDNS_ERROR_NO_ADDRESS:
        return "The host has no IPv4 address.";
    case SXNDNS_ERROR_BAD_NAME:
        return "That name is malformed.";
    case SXNDNS_ERROR_TIMEOUT:
        return "The name server did not answer.";
    case SXNDNS_ERROR_CANCELLED:
        return "Stopped.";
    case SXNDNS_ERROR_SERVER_FAILURE:
        return "The name server failed.";
    case SXNDNS_ERROR_BAD_REPLY:
        return "The name server's reply was unusable.";
    case SXNDNS_ERROR_NETWORK:
        return "Cannot reach the name server.";
    default:
        return "Name resolution failed.";
    }
}
