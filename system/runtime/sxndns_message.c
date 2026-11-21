#include "sxndns_message.h"

#include <string.h>

#define SXNDNS_HEADER_SIZE 12
#define SXNDNS_TYPE_A 1
#define SXNDNS_TYPE_CNAME 5
#define SXNDNS_CLASS_IN 1
/* Compression pointers followed while reading one name. */
#define SXNDNS_JUMP_MAX 16

static uint16_t read_u16(const unsigned char *at)
{
    return (uint16_t)(((uint16_t)at[0] << 8) | at[1]);
}

static uint32_t read_u32(const unsigned char *at)
{
    return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16) | ((uint32_t)at[2] << 8) | at[3];
}

static int name_char_allowed(unsigned char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '_';
}

static unsigned char lower(unsigned char value)
{
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value - 'A' + 'a') : value;
}

int sxndns_normalize_name(const char *name, char *out, size_t capacity)
{
    size_t length;
    size_t index;
    size_t label = 0;

    if (name == 0 || out == 0)
    {
        return -1;
    }
    length = strlen(name);
    if (length > 0 && name[length - 1] == '.')
    {
        length -= 1; /* the root, implied */
    }
    if (length == 0 || length > 253 || length + 1 > capacity)
    {
        return -1;
    }
    for (index = 0; index < length; ++index)
    {
        unsigned char value = (unsigned char)name[index];

        if (value == '.')
        {
            if (label == 0)
            {
                return -1;
            }
            label = 0;
        }
        else
        {
            if (!name_char_allowed(value) || ++label > 63)
            {
                return -1;
            }
        }
        out[index] = (char)lower(value);
    }
    out[length] = '\0';
    return 0;
}

int sxndns_encode_query(const char *name, uint16_t txid, unsigned char *out, size_t capacity,
                        size_t *length)
{
    size_t position = SXNDNS_HEADER_SIZE;
    const char *label = name;

    if (name == 0 || out == 0 || length == 0 || name[0] == '\0' || capacity < SXNDNS_HEADER_SIZE)
    {
        return -1;
    }
    for (;;)
    {
        const char *dot = strchr(label, '.');
        size_t size = dot != 0 ? (size_t)(dot - label) : strlen(label);

        if (size == 0 || size > 63 || position + 1 + size + 1 + 4 > capacity)
        {
            return -1;
        }
        out[position++] = (unsigned char)size;
        memcpy(out + position, label, size);
        position += size;
        if (dot == 0)
        {
            break;
        }
        label = dot + 1;
    }
    out[position++] = 0;
    out[position++] = 0;
    out[position++] = SXNDNS_TYPE_A;
    out[position++] = 0;
    out[position++] = SXNDNS_CLASS_IN;
    out[0] = (unsigned char)(txid >> 8);
    out[1] = (unsigned char)(txid & 0xFF);
    out[2] = 0x01; /* recursion desired */
    out[3] = 0x00;
    out[4] = 0x00;
    out[5] = 0x01; /* one question */
    memset(out + 6, 0, 6);
    *length = position;
    return 0;
}

/* Reads a possibly compressed name at `offset`, lowercased and dotted into
 * `out`, and the offset just past it (past the first pointer, when there is
 * one) into *next. -1 on any overrun, a label over 63, a pointer loop or a
 * name that does not fit. */
static int read_name(const unsigned char *message, size_t length, size_t offset, char *out,
                     size_t capacity, size_t *next)
{
    size_t out_length = 0;
    size_t jumps = 0;
    size_t after_jump = 0;
    int jumped = 0;

    for (;;)
    {
        unsigned char size;

        if (offset >= length)
        {
            return -1;
        }
        size = message[offset];
        if ((size & 0xC0) == 0xC0)
        {
            if (offset + 1 >= length || ++jumps > SXNDNS_JUMP_MAX)
            {
                return -1;
            }
            if (!jumped)
            {
                after_jump = offset + 2;
                jumped = 1;
            }
            offset = ((size_t)(size & 0x3F) << 8) | message[offset + 1];
            continue;
        }
        if ((size & 0xC0) != 0)
        {
            return -1; /* the 0x40 and 0x80 label types are not used */
        }
        if (size == 0)
        {
            if (out_length + 1 > capacity)
            {
                return -1;
            }
            out[out_length] = '\0';
            *next = jumped ? after_jump : offset + 1;
            return 0;
        }
        if (offset + 1 + size > length)
        {
            return -1;
        }
        if (out_length != 0)
        {
            if (out_length + 1 >= capacity)
            {
                return -1;
            }
            out[out_length++] = '.';
        }
        if (out_length + size >= capacity)
        {
            return -1;
        }
        for (size_t index = 0; index < size; ++index)
        {
            out[out_length++] = (char)lower(message[offset + 1 + index]);
        }
        offset += 1 + size;
    }
}

/* Everything lives in the caller's frame, about 2 KiB: two resolvers in two
 * threads may parse at the same time. */
int sxndns_parse_reply(const unsigned char *reply, size_t reply_length, const unsigned char *query,
                       size_t query_length, struct sxndns_answer *answer)
{
    char asked[SXNDNS_NAME_CAP];
    char echoed[SXNDNS_NAME_CAP];
    char current[SXNDNS_NAME_CAP];
    size_t query_end = 0;
    size_t offset = 0;
    uint16_t answers;
    uint32_t ttl = 0xFFFFFFFFu;
    unsigned rcode;
    unsigned count = 0;
    unsigned hops = 0;
    struct
    {
        size_t owner; /* offset of the owner name in the reply */
        uint16_t type;
        uint32_t ttl;
        size_t data;
        uint16_t data_length;
    } records[SXNDNS_ANSWER_MAX];

    if (reply == 0 || query == 0 || answer == 0 || query_length < SXNDNS_HEADER_SIZE + 5)
    {
        return -1;
    }
    memset(answer, 0, sizeof(*answer));
    if (read_name(query, query_length, SXNDNS_HEADER_SIZE, asked, sizeof(asked), &query_end) != 0 ||
        query_end + 4 != query_length)
    {
        return -1;
    }

    /* Whose reply is it? Anything that is not a response to this TXID and
     * this exact question is someone else's datagram. */
    if (reply_length < SXNDNS_HEADER_SIZE || read_u16(reply) != read_u16(query))
    {
        return -1;
    }
    if ((reply[2] & 0x80) == 0 || ((reply[2] >> 3) & 0x0F) != 0 || read_u16(reply + 4) != 1)
    {
        return -1;
    }
    if (read_name(reply, reply_length, SXNDNS_HEADER_SIZE, echoed, sizeof(echoed), &offset) != 0 ||
        offset + 4 > reply_length)
    {
        return -1;
    }
    if (strcmp(echoed, asked) != 0 || memcmp(reply + offset, query + query_end, 4) != 0)
    {
        return -1;
    }
    offset += 4;

    rcode = reply[3] & 0x0F;
    if (rcode == 3)
    {
        answer->outcome = SXNDNS_OUTCOME_NO_SUCH_NAME;
        return 0;
    }
    if (rcode != 0)
    {
        answer->outcome = SXNDNS_OUTCOME_SERVER_FAILURE;
        return 0;
    }
    if ((reply[2] & 0x02) != 0)
    {
        answer->outcome = SXNDNS_OUTCOME_TRUNCATED;
        return 0;
    }

    /* Collect the answer records, every one bounds-checked. */
    answers = read_u16(reply + 6);
    for (unsigned index = 0; index < answers && count < SXNDNS_ANSWER_MAX; ++index)
    {
        size_t after = 0;
        uint16_t data_length;

        if (read_name(reply, reply_length, offset, current, sizeof(current), &after) != 0 ||
            after + 10 > reply_length)
        {
            return -2;
        }
        data_length = read_u16(reply + after + 8);
        if (after + 10 + data_length > reply_length)
        {
            return -2;
        }
        records[count].owner = offset;
        records[count].type = read_u16(reply + after);
        records[count].ttl = read_u32(reply + after + 4) & 0x7FFFFFFFu;
        records[count].data = after + 10;
        records[count].data_length = data_length;
        if (read_u16(reply + after + 2) == SXNDNS_CLASS_IN)
        {
            count += 1;
        }
        offset = after + 10 + data_length;
    }

    /* Follow the chain from the name asked: an A record for the current name
     * ends it, a CNAME moves it. Records about other names are not believed. */
    memcpy(current, asked, sizeof(current));
    for (;;)
    {
        int moved = 0;

        for (unsigned index = 0; index < count; ++index)
        {
            char owner[SXNDNS_NAME_CAP];
            size_t ignored = 0;

            if (read_name(reply, reply_length, records[index].owner, owner, sizeof(owner),
                    &ignored) != 0)
            {
                return -2;
            }
            if (strcmp(owner, current) != 0)
            {
                continue;
            }
            if (records[index].type == SXNDNS_TYPE_A)
            {
                if (records[index].data_length != 4)
                {
                    return -2;
                }
                answer->outcome = SXNDNS_OUTCOME_ADDRESS;
                answer->ipv4 = read_u32(reply + records[index].data);
                answer->ttl = records[index].ttl < ttl ? records[index].ttl : ttl;
                return 0;
            }
            if (records[index].type == SXNDNS_TYPE_CNAME)
            {
                char target[SXNDNS_NAME_CAP];
                size_t end = 0;

                if (read_name(reply, reply_length, records[index].data, target, sizeof(target),
                        &end) != 0 ||
                    end > records[index].data + records[index].data_length || target[0] == '\0')
                {
                    return -2;
                }
                if (++hops > SXNDNS_CHAIN_MAX)
                {
                    return -2;
                }
                if (records[index].ttl < ttl)
                {
                    ttl = records[index].ttl;
                }
                memcpy(current, target, sizeof(current));
                moved = 1;
                break;
            }
        }
        if (!moved)
        {
            break;
        }
    }
    if (strcmp(current, asked) != 0)
    {
        answer->outcome = SXNDNS_OUTCOME_ALIAS;
        answer->ttl = ttl;
        memcpy(answer->alias, current, sizeof(answer->alias));
        return 0;
    }
    answer->outcome = SXNDNS_OUTCOME_NO_ADDRESS;
    return 0;
}

long sxndns_tcp_frame(const unsigned char *buffer, size_t have, size_t capacity)
{
    size_t size;

    if (buffer == 0 || capacity < 3 || have < 2)
    {
        return 0;
    }
    size = read_u16(buffer);
    if (size == 0 || size > capacity - 2)
    {
        return -1;
    }
    return have >= size + 2 ? (long)size : 0;
}
