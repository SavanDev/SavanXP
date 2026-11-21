#pragma once

/* The DNS wire format behind sxn/sxndns.h: building one A/IN question,
 * reading the reply it gets, and the two-byte length framing DNS uses over
 * TCP. No system calls, so the host tests compile it as it is
 * (tests/host/sxndns_message_test.c). Every byte handed to these functions
 * came from the network and is checked before it is believed.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest name this resolver handles, as text with its terminator. */
#define SXNDNS_NAME_CAP 256
/* A query, and a reply over UDP: the classic 512-byte limit. */
#define SXNDNS_UDP_MESSAGE_CAP 512
/* A reply over TCP after a truncated one; larger replies are refused. */
#define SXNDNS_TCP_MESSAGE_CAP 4096
/* Answer records looked at in one reply; the rest are ignored. */
#define SXNDNS_ANSWER_MAX 32
/* Aliases followed inside one reply before giving up on it. */
#define SXNDNS_CHAIN_MAX 8

enum sxndns_outcome
{
    SXNDNS_OUTCOME_ADDRESS = 0,   /* `ipv4` holds the address, `ttl` its lifetime */
    SXNDNS_OUTCOME_ALIAS,         /* the name is an alias of `alias`, with no address here */
    SXNDNS_OUTCOME_NO_SUCH_NAME,  /* NXDOMAIN */
    SXNDNS_OUTCOME_NO_ADDRESS,    /* the name exists but has no A record */
    SXNDNS_OUTCOME_SERVER_FAILURE,/* SERVFAIL, REFUSED and every other error code */
    SXNDNS_OUTCOME_TRUNCATED,     /* TC set: ask again over TCP */
};

struct sxndns_answer
{
    enum sxndns_outcome outcome;
    uint32_t ipv4; /* 0xAABBCCDD */
    uint32_t ttl;  /* seconds: the smallest TTL along the records used */
    char alias[SXNDNS_NAME_CAP];
};

/* Checks `name` and writes it lowercased, without a trailing dot, into
 * `out`. 0, or -1 when it is not a host name this resolver asks about:
 * empty, longer than 253 characters, an empty or 64-character label, or a
 * character other than a letter, digit, '-' or '_'. */
int sxndns_normalize_name(const char *name, char *out, size_t capacity);

/* Builds a query for one A/IN question about `name` (already normalized),
 * recursion desired. 0 with its size in *length, or -1 when it does not fit. */
int sxndns_encode_query(const char *name, uint16_t txid, unsigned char *out, size_t capacity,
                        size_t *length);

/* Reads `reply` against the `query` it answers.
 *  0  it is the reply to this query; `answer` says what it says.
 * -1  it is not a reply to this query (another TXID, another question, not a
 *     response): ignore it and keep waiting for the real one.
 * -2  it claims to be the reply but its bytes overrun or contradict
 *     themselves. */
int sxndns_parse_reply(const unsigned char *reply, size_t reply_length, const unsigned char *query,
                       size_t query_length, struct sxndns_answer *answer);

/* The state of a TCP reply after `have` bytes arrived in `buffer`:
 *  > 0  complete; the message is that many bytes, starting at buffer + 2.
 *    0  more bytes are needed.
 *   -1  the length prefix is 0 or larger than `capacity` - 2. */
long sxndns_tcp_frame(const unsigned char *buffer, size_t have, size_t capacity);

#ifdef __cplusplus
}
#endif
