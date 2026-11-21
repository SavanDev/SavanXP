/* The DNS wire format of sxn/sxndns.h (system/runtime/sxndns_message.c):
 * replies that are right, replies that belong to someone else, and replies
 * whose bytes lie, cut at every length and fed over TCP one byte at a time.
 */

#include "sxndns_message.h"

#include <stdio.h>
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

struct builder
{
    unsigned char bytes[1024];
    size_t length;
};

static void put(struct builder *b, const void *bytes, size_t length)
{
    memcpy(b->bytes + b->length, bytes, length);
    b->length += length;
}

static void put_u16(struct builder *b, unsigned value)
{
    unsigned char bytes[2] = {(unsigned char)(value >> 8), (unsigned char)value};
    put(b, bytes, 2);
}

static void put_u32(struct builder *b, unsigned long value)
{
    unsigned char bytes[4] = {(unsigned char)(value >> 24), (unsigned char)(value >> 16),
        (unsigned char)(value >> 8), (unsigned char)value};
    put(b, bytes, 4);
}

static void put_name(struct builder *b, const char *name)
{
    while (*name != '\0')
    {
        const char *dot = strchr(name, '.');
        size_t length = dot != 0 ? (size_t)(dot - name) : strlen(name);
        unsigned char size = (unsigned char)length;

        put(b, &size, 1);
        put(b, name, length);
        name += length;
        if (*name == '.')
        {
            name += 1;
        }
    }
    put(b, "", 1);
}

/* A reply header and the echoed question of `query`. */
static void begin_reply(struct builder *b, const unsigned char *query, size_t query_length,
    unsigned flags, unsigned answers)
{
    b->length = 0;
    put(b, query, 2);
    put_u16(b, flags);
    put_u16(b, 1);
    put_u16(b, answers);
    put_u16(b, 0);
    put_u16(b, 0);
    put(b, query + 12, query_length - 12);
}

static void put_a(struct builder *b, const char *owner, unsigned long ttl, unsigned long ipv4)
{
    if (owner == 0)
    {
        put_u16(b, 0xC00C); /* the question's name */
    }
    else
    {
        put_name(b, owner);
    }
    put_u16(b, 1);
    put_u16(b, 1);
    put_u32(b, ttl);
    put_u16(b, 4);
    put_u32(b, ipv4);
}

static void put_cname(struct builder *b, const char *owner, unsigned long ttl, const char *target)
{
    struct builder data = {{0}, 0};

    if (owner == 0)
    {
        put_u16(b, 0xC00C);
    }
    else
    {
        put_name(b, owner);
    }
    put_u16(b, 5);
    put_u16(b, 1);
    put_u32(b, ttl);
    put_name(&data, target);
    put_u16(b, (unsigned)data.length);
    put(b, data.bytes, data.length);
}

static size_t make_query(const char *name, unsigned txid, unsigned char *query)
{
    size_t length = 0;

    CHECK(sxndns_encode_query(name, (uint16_t)txid, query, SXNDNS_UDP_MESSAGE_CAP, &length) == 0);
    return length;
}

static void test_normalize(void)
{
    char out[SXNDNS_NAME_CAP];
    char long_label[80];
    char long_name[300];

    CHECK(sxndns_normalize_name("Test.SavanXP.", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "test.savanxp") == 0);
    CHECK(sxndns_normalize_name("a-b_c.d1", out, sizeof(out)) == 0);
    CHECK(sxndns_normalize_name("", out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name(".", out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name("a..b", out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name(".a", out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name("a b", out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name("a/b", out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name("a:80", out, sizeof(out)) == -1);
    memset(long_label, 'x', 64);
    long_label[64] = '\0';
    CHECK(sxndns_normalize_name(long_label, out, sizeof(out)) == -1);
    long_label[63] = '\0';
    CHECK(sxndns_normalize_name(long_label, out, sizeof(out)) == 0);
    memset(long_name, 'a', sizeof(long_name));
    for (size_t index = 1; index < 254; index += 2)
    {
        long_name[index] = '.';
    }
    long_name[253] = '\0'; /* 253 characters: the longest name */
    CHECK(sxndns_normalize_name(long_name, out, sizeof(out)) == 0);
    long_name[253] = 'a';
    long_name[254] = '\0';
    CHECK(sxndns_normalize_name(long_name, out, sizeof(out)) == -1);
    CHECK(sxndns_normalize_name("abc", out, 3) == -1);
}

static void test_encode(void)
{
    unsigned char query[SXNDNS_UDP_MESSAGE_CAP];
    static const unsigned char expected[] = {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
        4, 't', 'e', 's', 't', 7, 's', 'a', 'v', 'a', 'n', 'x', 'p', 0, 0, 1, 0, 1};
    size_t length = 0;

    CHECK(sxndns_encode_query("test.savanxp", 0x1234, query, sizeof(query), &length) == 0);
    CHECK(length == sizeof(expected));
    CHECK(memcmp(query, expected, sizeof(expected)) == 0);
    CHECK(sxndns_encode_query("test.savanxp", 1, query, sizeof(expected) - 1, &length) == -1);
}

static void test_answers(void)
{
    unsigned char query[SXNDNS_UDP_MESSAGE_CAP];
    size_t query_length = make_query("www.savanxp", 0xBEEF, query);
    struct builder b;
    struct sxndns_answer answer;

    /* A plain address. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_a(&b, 0, 60, 0x0A000202ul);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_ADDRESS);
    CHECK(answer.ipv4 == 0x0A000202u);
    CHECK(answer.ttl == 60);

    /* An owner written uncompressed and in capitals is still ours. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_a(&b, "WWW.SavanXP", 30, 0x01020304ul);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_ADDRESS && answer.ipv4 == 0x01020304u);

    /* An alias and its address in one reply: the smaller TTL wins. */
    begin_reply(&b, query, query_length, 0x8180, 2);
    put_cname(&b, 0, 20, "real.savanxp");
    put_a(&b, "real.savanxp", 90, 0x0A000203ul);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_ADDRESS && answer.ipv4 == 0x0A000203u);
    CHECK(answer.ttl == 20);

    /* The same records in the other order. */
    begin_reply(&b, query, query_length, 0x8180, 2);
    put_a(&b, "real.savanxp", 90, 0x0A000203ul);
    put_cname(&b, 0, 120, "real.savanxp");
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_ADDRESS && answer.ttl == 90);

    /* An alias with no address: the caller asks about the target. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_cname(&b, 0, 40, "Elsewhere.savanxp");
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_ALIAS);
    CHECK(strcmp(answer.alias, "elsewhere.savanxp") == 0);
    CHECK(answer.ttl == 40);

    /* An address for a name nobody asked about is not believed. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_a(&b, "evil.savanxp", 60, 0x0A0A0A0Aul);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_NO_ADDRESS);

    /* No answers at all. */
    begin_reply(&b, query, query_length, 0x8180, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_NO_ADDRESS);

    /* NXDOMAIN, SERVFAIL, REFUSED, truncation. */
    begin_reply(&b, query, query_length, 0x8183, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_NO_SUCH_NAME);
    begin_reply(&b, query, query_length, 0x8182, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_SERVER_FAILURE);
    begin_reply(&b, query, query_length, 0x8185, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_SERVER_FAILURE);
    begin_reply(&b, query, query_length, 0x8380, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == 0);
    CHECK(answer.outcome == SXNDNS_OUTCOME_TRUNCATED);
}

static void test_not_ours(void)
{
    unsigned char query[SXNDNS_UDP_MESSAGE_CAP];
    unsigned char other[SXNDNS_UDP_MESSAGE_CAP];
    size_t query_length = make_query("www.savanxp", 0xBEEF, query);
    size_t other_length = make_query("www.savanxq", 0xBEEF, other);
    struct builder b;
    struct sxndns_answer answer;

    /* Another TXID. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_a(&b, 0, 60, 1);
    b.bytes[1] ^= 1;
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -1);

    /* Another question with the same TXID. */
    begin_reply(&b, other, other_length, 0x8180, 1);
    put_a(&b, 0, 60, 1);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -1);

    /* A query, not a response; another opcode; two questions. */
    begin_reply(&b, query, query_length, 0x0100, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -1);
    begin_reply(&b, query, query_length, 0x8980, 0);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -1);
    begin_reply(&b, query, query_length, 0x8180, 0);
    b.bytes[5] = 2;
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -1);

    /* Another type in the echoed question. */
    begin_reply(&b, query, query_length, 0x8180, 0);
    b.bytes[b.length - 3] = 28;
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -1);

    /* Too short for a header. */
    CHECK(sxndns_parse_reply(b.bytes, 11, query, query_length, &answer) == -1);
}

static void test_malformed(void)
{
    unsigned char query[SXNDNS_UDP_MESSAGE_CAP];
    size_t query_length = make_query("www.savanxp", 0xBEEF, query);
    struct builder b;
    struct builder good;
    struct sxndns_answer answer;
    size_t answer_start;

    /* An answer whose data runs past the end. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_a(&b, 0, 60, 1);
    b.bytes[b.length - 5] = 40;
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -2);

    /* An A record that is not four bytes. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_a(&b, 0, 60, 1);
    b.bytes[b.length - 5] = 3;
    CHECK(sxndns_parse_reply(b.bytes, b.length - 1, query, query_length, &answer) == -2);

    /* A compression pointer that points at itself. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    answer_start = b.length;
    put_u16(&b, (unsigned)(0xC000 | answer_start));
    put_u16(&b, 1);
    put_u16(&b, 1);
    put_u32(&b, 60);
    put_u16(&b, 4);
    put_u32(&b, 1);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -2);

    /* A pointer past the end. */
    begin_reply(&b, query, query_length, 0x8180, 1);
    put_u16(&b, 0xC3FF);
    put_u16(&b, 1);
    put_u16(&b, 1);
    put_u32(&b, 60);
    put_u16(&b, 4);
    put_u32(&b, 1);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -2);

    /* Aliases that loop. */
    begin_reply(&b, query, query_length, 0x8180, 2);
    put_cname(&b, 0, 60, "loop.savanxp");
    put_cname(&b, "loop.savanxp", 60, "www.savanxp");
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -2);

    /* An answer count larger than the answers present. */
    begin_reply(&b, query, query_length, 0x8180, 3);
    put_a(&b, "other.savanxp", 60, 1);
    CHECK(sxndns_parse_reply(b.bytes, b.length, query, query_length, &answer) == -2);

    /* Every cut of a good reply is refused, never read past. */
    begin_reply(&good, query, query_length, 0x8180, 2);
    put_cname(&good, 0, 20, "real.savanxp");
    put_a(&good, "real.savanxp", 90, 0x0A000203ul);
    for (size_t cut = 0; cut < good.length; ++cut)
    {
        unsigned char copy[1024];
        int result;

        memcpy(copy, good.bytes, cut);
        result = sxndns_parse_reply(copy, cut, query, query_length, &answer);
        CHECK(result == -1 || result == -2);
    }
    CHECK(sxndns_parse_reply(good.bytes, good.length, query, query_length, &answer) == 0);

    /* Every single flipped byte past the question either parses cleanly or is
     * refused; none crashes. */
    for (size_t index = 12; index < good.length; ++index)
    {
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            unsigned char copy[1024];

            memcpy(copy, good.bytes, good.length);
            copy[index] ^= (unsigned char)(1u << bit);
            (void)sxndns_parse_reply(copy, good.length, query, query_length, &answer);
        }
    }
}

static void test_tcp_frame(void)
{
    unsigned char stream[SXNDNS_TCP_MESSAGE_CAP + 2];
    size_t message = 300;

    stream[0] = (unsigned char)(message >> 8);
    stream[1] = (unsigned char)message;
    memset(stream + 2, 0xAB, message);
    /* Fed one byte at a time, the length prefix split too. */
    for (size_t have = 0; have < message + 2; ++have)
    {
        CHECK(sxndns_tcp_frame(stream, have, sizeof(stream)) == 0);
    }
    CHECK(sxndns_tcp_frame(stream, message + 2, sizeof(stream)) == (long)message);
    CHECK(sxndns_tcp_frame(stream, message + 40, sizeof(stream)) == (long)message);

    stream[0] = 0;
    stream[1] = 0;
    CHECK(sxndns_tcp_frame(stream, 2, sizeof(stream)) == -1);
    stream[0] = (unsigned char)((SXNDNS_TCP_MESSAGE_CAP + 1) >> 8);
    stream[1] = (unsigned char)(SXNDNS_TCP_MESSAGE_CAP + 1);
    CHECK(sxndns_tcp_frame(stream, 2, sizeof(stream)) == -1);
    stream[0] = (unsigned char)(SXNDNS_TCP_MESSAGE_CAP >> 8);
    stream[1] = (unsigned char)SXNDNS_TCP_MESSAGE_CAP;
    CHECK(sxndns_tcp_frame(stream, 2, sizeof(stream)) == 0);
}

int main(void)
{
    test_normalize();
    test_encode();
    test_answers();
    test_not_ours();
    test_malformed();
    test_tcp_frame();
    if (g_failures != 0)
    {
        fprintf(stderr, "sxndns-message-test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("sxndns-message-test: OK\n");
    return 0;
}
