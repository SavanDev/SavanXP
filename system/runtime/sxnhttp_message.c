#include "sxn/sxnhttp_message.h"

#include <stdio.h>
#include <string.h>

enum
{
    STATE_HEAD,
    STATE_BODY_LENGTH,
    STATE_BODY_CLOSE,
    STATE_CHUNK_SIZE,
    STATE_CHUNK_DATA,
    STATE_CHUNK_DATA_END,
    STATE_TRAILERS,
    STATE_DONE,
    STATE_FAILED,
};

/* Content-Length values past this (1 TiB) are refused: no download comes
 * near it, and it keeps the arithmetic far from overflow. */
#define SXNHTTP_LENGTH_MAX 0xFFFFFFFFFFull

static char lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value - 'A' + 'a') : value;
}

static int equals_ignoring_case(const char *a, size_t a_length, const char *b)
{
    size_t index;

    for (index = 0; index < a_length; ++index)
    {
        if (b[index] == '\0' || lower(a[index]) != lower(b[index]))
        {
            return 0;
        }
    }
    return b[index] == '\0';
}

static int starts_ignoring_case(const char *text, const char *prefix)
{
    for (; *prefix != '\0'; ++text, ++prefix)
    {
        if (lower(*text) != lower(*prefix))
        {
            return 0;
        }
    }
    return 1;
}

/* ---- URLs ------------------------------------------------------------------ */

static int host_char_allowed(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '-' || value == '.' || value == '_';
}

int sxnhttp_parse_url(const char *text, struct sxnhttp_url *url)
{
    const char *cursor;
    const char *authority_end;
    const char *colon;
    size_t host_length;
    size_t target_length = 0;
    unsigned port;

    if (text == 0 || url == 0)
    {
        return -1;
    }
    for (cursor = text; *cursor != '\0'; ++cursor)
    {
        if ((unsigned char)*cursor <= 0x20 || (unsigned char)*cursor == 0x7F)
        {
            return -1;
        }
    }
    memset(url, 0, sizeof(*url));
    if (starts_ignoring_case(text, "http://"))
    {
        cursor = text + 7;
        port = 80;
    }
    else if (starts_ignoring_case(text, "https://"))
    {
        cursor = text + 8;
        url->https = 1;
        port = 443;
    }
    else
    {
        return -1;
    }
    authority_end = cursor;
    while (*authority_end != '\0' && *authority_end != '/' && *authority_end != '?' &&
           *authority_end != '#')
    {
        if (*authority_end == '@' || *authority_end == '[')
        {
            return -1; /* user information and IPv6 literals are not on this road */
        }
        authority_end += 1;
    }
    colon = memchr(cursor, ':', (size_t)(authority_end - cursor));
    host_length = (size_t)((colon != 0 ? colon : authority_end) - cursor);
    if (host_length == 0 || host_length >= sizeof(url->host))
    {
        return -1;
    }
    for (size_t index = 0; index < host_length; ++index)
    {
        if (!host_char_allowed(cursor[index]))
        {
            return -1;
        }
        url->host[index] = lower(cursor[index]);
    }
    if (colon != 0 && colon + 1 < authority_end)
    {
        unsigned value = 0;

        if (authority_end - colon - 1 > 5)
        {
            return -1;
        }
        for (const char *digit = colon + 1; digit < authority_end; ++digit)
        {
            if (*digit < '0' || *digit > '9')
            {
                return -1;
            }
            value = value * 10u + (unsigned)(*digit - '0');
        }
        if (value == 0 || value > 65535u)
        {
            return -1;
        }
        port = value;
    }
    url->port = (uint16_t)port;

    cursor = authority_end;
    if (*cursor != '/')
    {
        url->target[target_length++] = '/';
    }
    while (*cursor != '\0' && *cursor != '#')
    {
        if (target_length + 1 >= sizeof(url->target))
        {
            return -1;
        }
        url->target[target_length++] = *cursor++;
    }
    url->target[target_length] = '\0';
    return 0;
}

int sxnhttp_format_url(const struct sxnhttp_url *url, char *out, size_t capacity)
{
    int written;
    const unsigned default_port = url->https ? 443u : 80u;

    if (url == 0 || out == 0 || capacity == 0)
    {
        return -1;
    }
    if (url->port == default_port)
    {
        written = snprintf(out, capacity, "%s://%s%s", url->https ? "https" : "http", url->host,
            url->target);
    }
    else
    {
        written = snprintf(out, capacity, "%s://%s:%u%s", url->https ? "https" : "http", url->host,
            (unsigned)url->port, url->target);
    }
    return written < 0 || (size_t)written >= capacity ? -1 : 0;
}

/* RFC 3986, 5.2.4: removes "." and ".." segments from an absolute path. */
static int remove_dot_segments(const char *path, char *out, size_t capacity)
{
    const char *input = path;
    size_t length = 0;

    while (*input != '\0')
    {
        if (strncmp(input, "../", 3) == 0)
        {
            input += 3;
        }
        else if (strncmp(input, "./", 2) == 0)
        {
            input += 2;
        }
        else if (strncmp(input, "/./", 3) == 0)
        {
            input += 2;
        }
        else if (strcmp(input, "/.") == 0)
        {
            input = "/";
        }
        else if (strncmp(input, "/../", 4) == 0 || strcmp(input, "/..") == 0)
        {
            input = input[3] == '\0' ? "/" : input + 3;
            while (length > 0 && out[length - 1] != '/')
            {
                length -= 1;
            }
            if (length > 0)
            {
                length -= 1; /* and the slash before the segment */
            }
        }
        else if (strcmp(input, ".") == 0 || strcmp(input, "..") == 0)
        {
            input += strlen(input);
        }
        else
        {
            do
            {
                if (length + 1 >= capacity)
                {
                    return -1;
                }
                out[length++] = *input++;
            } while (*input != '\0' && *input != '/');
        }
    }
    if (length == 0)
    {
        out[length++] = '/';
    }
    out[length] = '\0';
    return 0;
}

static int has_scheme(const char *reference)
{
    const char *cursor = reference;

    if (!((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z')))
    {
        return 0;
    }
    while ((*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
           (*cursor >= '0' && *cursor <= '9') || *cursor == '+' || *cursor == '-' || *cursor == '.')
    {
        cursor += 1;
    }
    return *cursor == ':';
}

int sxnhttp_resolve(const char *base, const char *reference, char *out, size_t capacity)
{
    struct sxnhttp_url url;
    char clean[SXNHTTP_URL_CAP];
    char merged[SXNHTTP_URL_CAP * 2];
    const char *query;
    size_t clean_length = 0;

    if (base == 0 || reference == 0 || out == 0 || sxnhttp_parse_url(base, &url) != 0)
    {
        return -1;
    }
    while (reference[clean_length] != '\0' && reference[clean_length] != '#')
    {
        if (clean_length + 1 >= sizeof(clean))
        {
            return -2;
        }
        clean[clean_length] = reference[clean_length];
        clean_length += 1;
    }
    clean[clean_length] = '\0';

    if (has_scheme(clean))
    {
        if (clean_length + 1 > capacity)
        {
            return -2;
        }
        memcpy(out, clean, clean_length + 1);
        return 0;
    }
    if (clean[0] == '/' && clean[1] == '/')
    {
        struct sxnhttp_url other;

        if (snprintf(merged, sizeof(merged), "%s:%s", url.https ? "https" : "http", clean) >=
                (int)sizeof(merged) ||
            sxnhttp_parse_url(merged, &other) != 0)
        {
            return -1;
        }
        url = other;
    }
    else if (clean[0] == '\0')
    {
        /* The same document. */
    }
    else if (clean[0] == '?')
    {
        char *base_query = strchr(url.target, '?');
        size_t path_length = base_query != 0 ? (size_t)(base_query - url.target) : strlen(url.target);

        if (path_length + clean_length + 1 > sizeof(url.target))
        {
            return -2;
        }
        memcpy(url.target + path_length, clean, clean_length + 1);
    }
    else
    {
        char *last_slash;
        char *base_query = strchr(url.target, '?');

        if (base_query != 0)
        {
            *base_query = '\0';
        }
        if (clean[0] == '/')
        {
            snprintf(merged, sizeof(merged), "%s", clean);
        }
        else
        {
            last_slash = strrchr(url.target, '/');
            if (last_slash != 0)
            {
                last_slash[1] = '\0';
            }
            if (snprintf(merged, sizeof(merged), "%s%s", url.target, clean) >= (int)sizeof(merged))
            {
                return -2;
            }
        }
        query = strchr(merged, '?');
        {
            char path[SXNHTTP_URL_CAP * 2];
            size_t path_length = query != 0 ? (size_t)(query - merged) : strlen(merged);

            memcpy(path, merged, path_length);
            path[path_length] = '\0';
            if (remove_dot_segments(path, url.target, sizeof(url.target)) != 0)
            {
                return -2;
            }
            if (query != 0 && strlen(url.target) + strlen(query) + 1 > sizeof(url.target))
            {
                return -2;
            }
            if (query != 0)
            {
                strcat(url.target, query);
            }
        }
    }
    return sxnhttp_format_url(&url, out, capacity) == 0 ? 0 : -2;
}

/* ---- file names for downloads ------------------------------------------------ */

static int hex_value(char digit)
{
    digit = lower(digit);
    if (digit >= '0' && digit <= '9')
    {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f')
    {
        return digit - 'a' + 10;
    }
    return -1;
}

/* `length` bytes of `name` cleaned into `out` (see sxnhttp_file_name): the
 * length kept, 0 when nothing is left. */
static size_t clean_file_name(const char *name, size_t length, char *out, size_t capacity)
{
    char kept[SXNHTTP_URL_CAP];
    size_t kept_length = 0;
    size_t start = 0;
    size_t stem;
    size_t extension = 0;

    for (size_t index = 0; index < length; ++index)
    {
        if (name[index] == '/' || name[index] == '\\')
        {
            start = index + 1;
        }
    }
    for (size_t index = start; index < length && kept_length + 1 < sizeof(kept); ++index)
    {
        unsigned char value = (unsigned char)name[index];

        if (value < 0x20 || value == 0x7F)
        {
            continue;
        }
        kept[kept_length++] = strchr("<>:\"|?*", (char)value) != 0 ? '_' : (char)value;
    }
    start = 0;
    while (start < kept_length && (kept[start] == '.' || kept[start] == ' '))
    {
        start += 1;
    }
    while (kept_length > start && (kept[kept_length - 1] == '.' || kept[kept_length - 1] == ' '))
    {
        kept_length -= 1;
    }
    length = kept_length - start;
    if (length == 0)
    {
        return 0;
    }
    if (length < capacity)
    {
        memcpy(out, kept + start, length);
        out[length] = '\0';
        return length;
    }
    /* Too long: the stem gives way, the extension (a short last ".x") stays. */
    for (size_t index = kept_length; index > start + 1; --index)
    {
        if (kept[index - 1] == '.')
        {
            if (kept_length - (index - 1) <= 16 && kept_length - (index - 1) < capacity / 2)
            {
                extension = kept_length - (index - 1);
            }
            break;
        }
    }
    stem = capacity - 1 - extension;
    while (stem > 0 && ((unsigned char)kept[start + stem] & 0xC0) == 0x80)
    {
        stem -= 1; /* never half a UTF-8 sequence */
    }
    while (stem > 0 && (kept[start + stem - 1] == '.' || kept[start + stem - 1] == ' '))
    {
        stem -= 1;
    }
    if (stem == 0)
    {
        return 0;
    }
    memcpy(out, kept + start, stem);
    memcpy(out + stem, kept + kept_length - extension, extension);
    out[stem + extension] = '\0';
    return stem + extension;
}

int sxnhttp_file_name(const char *suggested, const char *url, char *out, size_t capacity)
{
    struct sxnhttp_url parsed;

    if (out == 0 || capacity < 16)
    {
        return -1;
    }
    if (suggested != 0 && clean_file_name(suggested, strlen(suggested), out, capacity) > 0)
    {
        return 0;
    }
    if (url != 0 && sxnhttp_parse_url(url, &parsed) == 0)
    {
        char segment[SXNHTTP_URL_CAP];
        size_t segment_length = 0;
        const char *end = strchr(parsed.target, '?');
        const char *begin;

        if (end == 0)
        {
            end = parsed.target + strlen(parsed.target);
        }
        begin = end;
        while (begin > parsed.target && begin[-1] != '/')
        {
            begin -= 1;
        }
        for (const char *cursor = begin; cursor < end; ++cursor)
        {
            if (*cursor == '%' && end - cursor >= 3 && hex_value(cursor[1]) >= 0 && hex_value(cursor[2]) >= 0)
            {
                segment[segment_length++] = (char)(hex_value(cursor[1]) * 16 + hex_value(cursor[2]));
                cursor += 2;
            }
            else
            {
                segment[segment_length++] = *cursor;
            }
        }
        if (clean_file_name(segment, segment_length, out, capacity) > 0)
        {
            return 0;
        }
    }
    memcpy(out, "download", 9);
    return 0;
}

/* ---- the response parser ---------------------------------------------------- */

void sxnhttp_parser_init(struct sxnhttp_parser *parser, unsigned char *body, size_t body_capacity)
{
    memset(parser, 0, sizeof(*parser));
    parser->body = body;
    parser->body_capacity = body_capacity;
    parser->state = STATE_HEAD;
}

void sxnhttp_parser_set_sink(struct sxnhttp_parser *parser,
                             long (*sink)(void *user, const unsigned char *bytes, size_t length), void *user)
{
    parser->sink = sink;
    parser->sink_user = user;
}

static long fail(struct sxnhttp_parser *parser, long error)
{
    parser->state = STATE_FAILED;
    parser->remaining = (uint64_t)(-error);
    return error;
}

static int token_char(char value)
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || strchr("!#$%&'*+-.^_`|~", value) != 0;
}

static int ows(char value)
{
    return value == ' ' || value == '\t';
}

/* A comma-separated Content-Length whose values all agree. 0, or an error. */
static long parse_length(const char *value, size_t length, uint64_t *out)
{
    size_t index = 0;
    int have = 0;
    uint64_t first = 0;

    while (index < length)
    {
        uint64_t number = 0;
        size_t digits = 0;

        while (index < length && ows(value[index]))
        {
            index += 1;
        }
        while (index < length && value[index] >= '0' && value[index] <= '9')
        {
            number = number * 10u + (uint64_t)(value[index] - '0');
            if (number > SXNHTTP_LENGTH_MAX)
            {
                return SXNHTTP_ERROR_BAD_FRAMING;
            }
            index += 1;
            digits += 1;
        }
        while (index < length && ows(value[index]))
        {
            index += 1;
        }
        if (digits == 0 || (index < length && value[index] != ','))
        {
            return SXNHTTP_ERROR_BAD_FRAMING;
        }
        if (have && number != first)
        {
            return SXNHTTP_ERROR_BAD_FRAMING;
        }
        first = number;
        have = 1;
        index += 1; /* the comma */
    }
    if (!have)
    {
        return SXNHTTP_ERROR_BAD_FRAMING;
    }
    *out = first;
    return 0;
}

/* RFC 8187's charset'language'value, UTF-8 only, percent-decoded into `out`.
 * 0, or -1 for another charset, a bad escape or a NUL. */
static int decode_extended(const char *value, size_t length, char *out, size_t capacity)
{
    const char *first = memchr(value, '\'', length);
    const char *second;
    size_t written = 0;

    if (first == 0 || !equals_ignoring_case(value, (size_t)(first - value), "utf-8"))
    {
        return -1;
    }
    second = memchr(first + 1, '\'', length - (size_t)(first + 1 - value));
    if (second == 0)
    {
        return -1;
    }
    for (const char *cursor = second + 1; cursor < value + length; ++cursor)
    {
        char decoded = *cursor;

        if (*cursor == '%')
        {
            if (value + length - cursor < 3 || hex_value(cursor[1]) < 0 || hex_value(cursor[2]) < 0)
            {
                return -1;
            }
            decoded = (char)(hex_value(cursor[1]) * 16 + hex_value(cursor[2]));
            cursor += 2;
        }
        if (decoded == '\0' || written + 1 >= capacity)
        {
            return -1;
        }
        out[written++] = decoded;
    }
    out[written] = '\0';
    return 0;
}

/* Content-Disposition (RFC 6266): a type, then ";"-separated parameters,
 * each a token or a quoted string. A malformed parameter ends the reading;
 * what was read before it stands. */
static void parse_disposition(struct sxnhttp_parser *parser, const char *value, size_t length)
{
    char plain[SXNHTTP_FILENAME_CAP];
    char extended[SXNHTTP_FILENAME_CAP];
    size_t index = 0;
    size_t stop;

    plain[0] = '\0';
    extended[0] = '\0';
    while (index < length && value[index] != ';')
    {
        index += 1;
    }
    stop = index;
    while (stop > 0 && ows(value[stop - 1]))
    {
        stop -= 1;
    }
    if (stop == 0)
    {
        return;
    }
    parser->attachment = !equals_ignoring_case(value, stop, "inline");
    while (index < length)
    {
        char text[SXNHTTP_FILENAME_CAP];
        size_t text_length = 0;
        size_t name;
        size_t name_length;
        int usable = 1;

        index += 1; /* the ';' */
        while (index < length && ows(value[index]))
        {
            index += 1;
        }
        name = index;
        while (index < length && token_char(value[index]))
        {
            index += 1;
        }
        name_length = index - name;
        while (index < length && ows(value[index]))
        {
            index += 1;
        }
        if (name_length == 0 || index >= length || value[index] != '=')
        {
            break;
        }
        index += 1;
        while (index < length && ows(value[index]))
        {
            index += 1;
        }
        if (index < length && value[index] == '"')
        {
            index += 1;
            while (index < length && value[index] != '"')
            {
                if (value[index] == '\\' && index + 1 < length)
                {
                    index += 1;
                }
                if (text_length + 1 >= sizeof(text))
                {
                    usable = 0;
                }
                else
                {
                    text[text_length++] = value[index];
                }
                index += 1;
            }
            if (index >= length)
            {
                break; /* no closing quote */
            }
            index += 1;
        }
        else
        {
            while (index < length && value[index] != ';' && !ows(value[index]))
            {
                if (text_length + 1 >= sizeof(text))
                {
                    usable = 0;
                }
                else
                {
                    text[text_length++] = value[index];
                }
                index += 1;
            }
        }
        while (index < length && ows(value[index]))
        {
            index += 1;
        }
        if (index < length && value[index] != ';')
        {
            break;
        }
        text[text_length] = '\0';
        if (!usable)
        {
            continue;
        }
        if (equals_ignoring_case(value + name, name_length, "filename"))
        {
            memcpy(plain, text, text_length + 1);
        }
        else if (equals_ignoring_case(value + name, name_length, "filename*") &&
                 decode_extended(text, text_length, extended, sizeof(extended)) != 0)
        {
            extended[0] = '\0';
        }
    }
    memcpy(parser->filename, extended[0] != '\0' ? extended : plain, SXNHTTP_FILENAME_CAP);
}

/* Reads the status line and headers in parser->head. 0, 2 for an interim
 * response to skip, or an error. */
static long parse_head(struct sxnhttp_parser *parser)
{
    char *cursor = parser->head;
    char *end = parser->head + parser->head_length;
    int has_length = 0;
    int has_chunked = 0;
    uint64_t length = 0;
    char *line_end;
    size_t line_length;

    parser->head[parser->head_length] = '\0';
    line_end = memchr(cursor, '\n', (size_t)(end - cursor));
    line_length = (size_t)(line_end - cursor);
    if (line_length > 0 && cursor[line_length - 1] == '\r')
    {
        line_length -= 1;
    }
    /* HTTP/1.0 or HTTP/1.1, a space, three digits, then a space and a reason
     * or nothing. */
    if (line_length < 12 || strncmp(cursor, "HTTP/1.", 7) != 0 ||
        (cursor[7] != '0' && cursor[7] != '1') || cursor[8] != ' ' || cursor[9] < '1' ||
        cursor[9] > '9' || cursor[10] < '0' || cursor[10] > '9' || cursor[11] < '0' ||
        cursor[11] > '9' || (line_length > 12 && cursor[12] != ' '))
    {
        return SXNHTTP_ERROR_BAD_RESPONSE;
    }
    parser->version_minor = cursor[7] - '0';
    parser->status = (cursor[9] - '0') * 100 + (cursor[10] - '0') * 10 + (cursor[11] - '0');
    parser->content_type[0] = '\0';
    parser->location[0] = '\0';
    parser->location_too_long = 0;
    parser->has_length = 0;
    parser->content_length = 0;
    parser->attachment = 0;
    parser->filename[0] = '\0';

    for (cursor = line_end + 1; cursor < end; cursor = line_end + 1)
    {
        char *colon;
        char *value;
        size_t name_length;
        size_t value_length;

        line_end = memchr(cursor, '\n', (size_t)(end - cursor));
        line_length = (size_t)(line_end - cursor);
        if (line_length > 0 && cursor[line_length - 1] == '\r')
        {
            line_length -= 1;
        }
        if (line_length == 0)
        {
            break; /* the empty line that ends the head */
        }
        if (ows(cursor[0]))
        {
            return SXNHTTP_ERROR_BAD_RESPONSE; /* obsolete line folding */
        }
        colon = memchr(cursor, ':', line_length);
        if (colon == 0 || colon == cursor)
        {
            return SXNHTTP_ERROR_BAD_RESPONSE;
        }
        name_length = (size_t)(colon - cursor);
        for (size_t index = 0; index < name_length; ++index)
        {
            if (!token_char(cursor[index]))
            {
                return SXNHTTP_ERROR_BAD_RESPONSE;
            }
        }
        value = colon + 1;
        value_length = line_length - name_length - 1;
        while (value_length > 0 && ows(*value))
        {
            value += 1;
            value_length -= 1;
        }
        while (value_length > 0 && ows(value[value_length - 1]))
        {
            value_length -= 1;
        }
        for (size_t index = 0; index < value_length; ++index)
        {
            if ((unsigned char)value[index] < 0x20 && value[index] != '\t')
            {
                return SXNHTTP_ERROR_BAD_RESPONSE;
            }
        }

        if (equals_ignoring_case(cursor, name_length, "content-length"))
        {
            uint64_t number = 0;
            long parsed = parse_length(value, value_length, &number);

            if (parsed != 0 || (has_length && number != length))
            {
                return SXNHTTP_ERROR_BAD_FRAMING;
            }
            has_length = 1;
            length = number;
        }
        else if (equals_ignoring_case(cursor, name_length, "transfer-encoding"))
        {
            /* Only "chunked", once: any other coding, or chunked applied twice,
             * is a body this client cannot read. */
            size_t index = 0;

            while (index < value_length)
            {
                size_t start;
                size_t stop;

                while (index < value_length && (ows(value[index]) || value[index] == ','))
                {
                    index += 1;
                }
                start = index;
                while (index < value_length && value[index] != ',')
                {
                    index += 1;
                }
                stop = index;
                while (stop > start && ows(value[stop - 1]))
                {
                    stop -= 1;
                }
                if (stop == start)
                {
                    continue;
                }
                if (has_chunked || !equals_ignoring_case(value + start, stop - start, "chunked"))
                {
                    return SXNHTTP_ERROR_BAD_FRAMING;
                }
                has_chunked = 1;
            }
            if (!has_chunked)
            {
                return SXNHTTP_ERROR_BAD_FRAMING;
            }
        }
        else if (equals_ignoring_case(cursor, name_length, "content-encoding"))
        {
            if (value_length != 0 && !equals_ignoring_case(value, value_length, "identity"))
            {
                return SXNHTTP_ERROR_UNSUPPORTED_ENCODING;
            }
        }
        else if (equals_ignoring_case(cursor, name_length, "content-type"))
        {
            size_t take = value_length < sizeof(parser->content_type) - 1
                ? value_length : sizeof(parser->content_type) - 1;

            memcpy(parser->content_type, value, take);
            parser->content_type[take] = '\0';
        }
        else if (equals_ignoring_case(cursor, name_length, "content-disposition"))
        {
            parse_disposition(parser, value, value_length);
        }
        else if (equals_ignoring_case(cursor, name_length, "location"))
        {
            if (value_length >= sizeof(parser->location))
            {
                parser->location_too_long = 1;
                parser->location[0] = '\0';
            }
            else
            {
                memcpy(parser->location, value, value_length);
                parser->location[value_length] = '\0';
            }
        }
    }

    if (parser->status < 200)
    {
        /* 100 Continue and friends come before the real response; 101 would
         * switch protocols, which nothing here asked for. */
        if (parser->status == 101 || ++parser->interim > SXNHTTP_INTERIM_MAX)
        {
            return SXNHTTP_ERROR_BAD_RESPONSE;
        }
        return 2;
    }
    if (parser->status == 204 || parser->status == 304)
    {
        parser->framing = SXNHTTP_FRAMING_NONE;
    }
    else if (has_chunked && has_length)
    {
        return SXNHTTP_ERROR_BAD_FRAMING; /* two lengths that disagree by construction */
    }
    else if (has_chunked)
    {
        if (parser->version_minor == 0)
        {
            return SXNHTTP_ERROR_BAD_FRAMING; /* HTTP/1.0 has no chunked bodies */
        }
        parser->framing = SXNHTTP_FRAMING_CHUNKED;
    }
    else if (has_length)
    {
        parser->has_length = 1;
        parser->content_length = length;
        parser->framing = length == 0 ? SXNHTTP_FRAMING_NONE : SXNHTTP_FRAMING_LENGTH;
        parser->remaining = length;
    }
    else
    {
        parser->framing = SXNHTTP_FRAMING_CLOSE;
    }
    parser->head_done = 1;
    return 0;
}

/* Keeps what fits of the body, or hands it to the sink. 0, 1 when the body
 * stops here (bytes had to be dropped, or the sink said so), or an error. */
static long store(struct sxnhttp_parser *parser, const unsigned char *bytes, size_t length)
{
    size_t room = parser->body_capacity - parser->body_length;

    if (parser->sink != 0)
    {
        long result = length > 0 ? parser->sink(parser->sink_user, bytes, length) : 0;

        if (result < 0)
        {
            return fail(parser, result);
        }
        if (result > 0)
        {
            parser->body_cut = 1;
            return 1;
        }
        parser->body_length += length;
        return 0;
    }
    size_t take = length < room ? length : room;

    if (take > 0)
    {
        memcpy(parser->body + parser->body_length, bytes, take);
        parser->body_length += take;
    }
    if (take < length)
    {
        parser->body_cut = 1;
        return 1;
    }
    return 0;
}

/* A chunk-size line: hex digits, then optional whitespace and extensions. */
static long parse_chunk_size(struct sxnhttp_parser *parser)
{
    size_t length = parser->line_length;
    size_t index = 0;
    uint64_t size = 0;

    if (length > 0 && parser->line[length - 1] == '\r')
    {
        length -= 1;
    }
    while (index < length)
    {
        char digit = lower(parser->line[index]);
        unsigned value;

        if (digit >= '0' && digit <= '9')
        {
            value = (unsigned)(digit - '0');
        }
        else if (digit >= 'a' && digit <= 'f')
        {
            value = (unsigned)(digit - 'a' + 10);
        }
        else
        {
            break;
        }
        if (index >= 8)
        {
            return SXNHTTP_ERROR_BAD_FRAMING;
        }
        size = size * 16u + value;
        index += 1;
    }
    if (index == 0)
    {
        return SXNHTTP_ERROR_BAD_RESPONSE;
    }
    while (index < length && ows(parser->line[index]))
    {
        index += 1;
    }
    if (index < length && parser->line[index] != ';')
    {
        return SXNHTTP_ERROR_BAD_RESPONSE;
    }
    parser->remaining = size;
    return 0;
}

long sxnhttp_parser_feed(struct sxnhttp_parser *parser, const unsigned char *bytes, size_t length)
{
    size_t index = 0;

    if (parser->state == STATE_DONE)
    {
        return 1;
    }
    if (parser->state == STATE_FAILED)
    {
        return -(long)parser->remaining;
    }
    while (index < length)
    {
        switch (parser->state)
        {
        case STATE_HEAD:
        {
            char value = (char)bytes[index++];
            size_t at;

            if (parser->head_length + 1 >= sizeof(parser->head))
            {
                return fail(parser, SXNHTTP_ERROR_HEADERS_TOO_LARGE);
            }
            parser->head[parser->head_length++] = value;
            at = parser->head_length;
            if (value != '\n' ||
                !((at >= 2 && parser->head[at - 2] == '\n') ||
                    (at >= 3 && parser->head[at - 2] == '\r' && parser->head[at - 3] == '\n')))
            {
                continue;
            }
            {
                long parsed = parse_head(parser);

                if (parsed < 0)
                {
                    return fail(parser, parsed);
                }
                if (parsed == 2)
                {
                    parser->head_length = 0;
                    continue;
                }
            }
            switch (parser->framing)
            {
            case SXNHTTP_FRAMING_NONE:
                parser->state = STATE_DONE;
                return 1;
            case SXNHTTP_FRAMING_LENGTH:
                parser->state = STATE_BODY_LENGTH;
                break;
            case SXNHTTP_FRAMING_CHUNKED:
                parser->state = STATE_CHUNK_SIZE;
                parser->line_length = 0;
                break;
            case SXNHTTP_FRAMING_CLOSE:
                parser->state = STATE_BODY_CLOSE;
                break;
            }
            break;
        }
        case STATE_BODY_LENGTH:
        case STATE_CHUNK_DATA:
        {
            size_t take = length - index;

            if (take > parser->remaining)
            {
                take = (size_t)parser->remaining;
            }
            {
                long stored = store(parser, bytes + index, take);

                if (stored < 0)
                {
                    return stored;
                }
                if (stored > 0)
                {
                    parser->state = STATE_DONE;
                    return 1;
                }
            }
            index += take;
            parser->remaining -= take;
            if (parser->remaining == 0)
            {
                if (parser->state == STATE_BODY_LENGTH)
                {
                    parser->state = STATE_DONE;
                    return 1;
                }
                parser->state = STATE_CHUNK_DATA_END;
                parser->line_length = 0;
            }
            else if (parser->sink == 0 && parser->body_length == parser->body_capacity)
            {
                parser->body_cut = 1; /* more is coming and none of it fits */
                parser->state = STATE_DONE;
                return 1;
            }
            break;
        }
        case STATE_BODY_CLOSE:
        {
            long stored = store(parser, bytes + index, length - index);

            if (stored < 0)
            {
                return stored;
            }
            if (stored > 0)
            {
                parser->state = STATE_DONE;
                return 1;
            }
            index = length;
            break;
        }
        case STATE_CHUNK_DATA_END:
        {
            char value = (char)bytes[index++];

            if (value == '\r' && parser->line_length == 0)
            {
                parser->line_length = 1;
            }
            else if (value == '\n')
            {
                parser->state = STATE_CHUNK_SIZE;
                parser->line_length = 0;
            }
            else
            {
                return fail(parser, SXNHTTP_ERROR_BAD_FRAMING);
            }
            break;
        }
        case STATE_CHUNK_SIZE:
        case STATE_TRAILERS:
        {
            char value = (char)bytes[index++];

            if (parser->state == STATE_TRAILERS && ++parser->trailer_bytes > SXNHTTP_TRAILER_CAP)
            {
                return fail(parser, SXNHTTP_ERROR_HEADERS_TOO_LARGE);
            }
            if (value != '\n')
            {
                if (parser->line_length + 1 >= sizeof(parser->line))
                {
                    return fail(parser, SXNHTTP_ERROR_BAD_RESPONSE);
                }
                parser->line[parser->line_length++] = value;
                continue;
            }
            if (parser->state == STATE_TRAILERS)
            {
                int empty = parser->line_length == 0 ||
                            (parser->line_length == 1 && parser->line[0] == '\r');

                parser->line_length = 0;
                if (empty)
                {
                    parser->state = STATE_DONE;
                    return 1;
                }
                continue;
            }
            {
                long parsed = parse_chunk_size(parser);

                parser->line_length = 0;
                if (parsed < 0)
                {
                    return fail(parser, parsed);
                }
            }
            parser->state = parser->remaining == 0 ? STATE_TRAILERS : STATE_CHUNK_DATA;
            break;
        }
        default:
            return 1;
        }
    }
    return parser->state == STATE_DONE ? 1 : 0;
}

long sxnhttp_parser_finish(struct sxnhttp_parser *parser)
{
    if (parser->state == STATE_DONE)
    {
        return 1;
    }
    if (parser->state == STATE_FAILED)
    {
        return -(long)parser->remaining;
    }
    if (parser->state == STATE_BODY_CLOSE)
    {
        parser->state = STATE_DONE;
        return 1;
    }
    return fail(parser, SXNHTTP_ERROR_TRUNCATED);
}
