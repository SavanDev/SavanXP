#include "savanxp_compat.h"

#include <stdarg.h>
#include <stdint.h>

#include "i_system.h"
#include "savanxp/libc.h"

#define EXIT_FAILURE 1
#define EOF (-1)
#define ENOMEM 12
#define EINVAL 22
#define SX_HEAP_SIZE (48u * 1024u * 1024u)
#define SX_FILE_POOL_CAPACITY 16
#define SX_SHUTDOWN_CAPACITY 8

typedef struct sx_FILE FILE;

typedef struct sx_alloc_header {
    size_t size;
} sx_alloc_header;

struct sx_FILE {
    int fd;
    int in_use;
    int eof;
    int error;
    int is_stdio;
};

static unsigned char g_heap[SX_HEAP_SIZE];
static size_t g_heap_used = 0;

static struct sx_FILE g_file_pool[SX_FILE_POOL_CAPACITY] = {};
static struct sx_FILE g_stdin_file = {0, 1, 0, 0, 1};
static struct sx_FILE g_stdout_file = {1, 1, 0, 0, 1};
static struct sx_FILE g_stderr_file = {2, 1, 0, 0, 1};

FILE *stdin = &g_stdin_file;
FILE *stdout = &g_stdout_file;
FILE *stderr = &g_stderr_file;

static int g_errno = 0;
static void (*g_shutdown_callbacks[SX_SHUTDOWN_CAPACITY])(void) = {};
static size_t g_shutdown_count = 0;
static int g_shutdown_ran = 0;
static int g_exit_started = 0;

size_t sx_strlen(const char *text);
void sx_exit(int code) __attribute__((noreturn));

static size_t sx_min_size(size_t left, size_t right) {
    return left < right ? left : right;
}

static int sx_result_to_errno(long result) {
    return result < 0 ? result_error_code(result) : 0;
}

static void sx_set_errno_from_result(long result) {
    g_errno = sx_result_to_errno(result);
}

static void sx_run_shutdowns(void) {
    size_t index = g_shutdown_count;
    if (g_shutdown_ran) {
        return;
    }

    g_shutdown_ran = 1;
    while (index > 0) {
        --index;
        if (g_shutdown_callbacks[index] != 0) {
            g_shutdown_callbacks[index]();
        }
    }
}

static int sx_emit_file_char(char character, void *context) {
    FILE *stream = (FILE *)context;
    if (write(stream->fd, &character, 1) < 0) {
        stream->error = 1;
        return 0;
    }
    return 1;
}

typedef struct sx_buffer_sink {
    char *buffer;
    size_t size;
    size_t written;
} sx_buffer_sink;

static int sx_emit_buffer_char(char character, void *context) {
    sx_buffer_sink *sink = (sx_buffer_sink *)context;
    if (sink->size != 0 && sink->written + 1 < sink->size) {
        sink->buffer[sink->written] = character;
    }
    sink->written += 1;
    return 1;
}

static int sx_write_padded(
    int (*emit)(char character, void *context),
    void *context,
    const char *text,
    size_t text_length,
    int width,
    char pad
) {
    int count = 0;
    while ((int)text_length < width) {
        if (!emit(pad, context)) {
            return count;
        }
        ++count;
        --width;
    }
    for (size_t index = 0; index < text_length; ++index) {
        if (!emit(text[index], context)) {
            return count;
        }
        ++count;
    }
    return count;
}

static size_t sx_unsigned_to_string(unsigned long long value, unsigned base, int uppercase, char *buffer) {
    static const char kDigitsLower[] = "0123456789abcdef";
    static const char kDigitsUpper[] = "0123456789ABCDEF";
    const char *digits = uppercase ? kDigitsUpper : kDigitsLower;
    size_t count = 0;

    if (value == 0) {
        buffer[count++] = '0';
    } else {
        while (value != 0) {
            buffer[count++] = digits[value % base];
            value /= base;
        }
    }

    for (size_t index = 0; index < count / 2; ++index) {
        char temporary = buffer[index];
        buffer[index] = buffer[count - 1 - index];
        buffer[count - 1 - index] = temporary;
    }
    buffer[count] = '\0';
    return count;
}

static size_t sx_signed_to_string(long long value, char *buffer) {
    unsigned long long magnitude = 0;
    size_t offset = 0;

    if (value < 0) {
        buffer[offset++] = '-';
        magnitude = (unsigned long long)(-(value + 1)) + 1u;
    } else {
        magnitude = (unsigned long long)value;
    }

    return offset + sx_unsigned_to_string(magnitude, 10u, 0, buffer + offset);
}

static size_t sx_apply_numeric_precision(char *buffer, size_t length, int precision) {
    char temporary[128];
    size_t sign_length = 0;
    size_t digit_length = length;
    size_t zero_count = 0;
    size_t write_index = 0;

    if (precision < 0 || length >= sizeof(temporary)) {
        return length;
    }

    if (length > 0 && buffer[0] == '-') {
        sign_length = 1;
        digit_length -= 1;
    }

    if ((int)digit_length >= precision) {
        return length;
    }

    zero_count = (size_t)(precision - (int)digit_length);

    if (sign_length != 0) {
        temporary[write_index++] = '-';
    }
    while (zero_count-- > 0) {
        temporary[write_index++] = '0';
    }
    for (size_t index = sign_length; index < length; ++index) {
        temporary[write_index++] = buffer[index];
    }
    temporary[write_index] = '\0';

    for (size_t index = 0; index <= write_index; ++index) {
        buffer[index] = temporary[index];
    }
    return write_index;
}

static int sx_vformat(
    int (*emit)(char character, void *context),
    void *context,
    const char *format,
    va_list args
) {
    int written = 0;

    for (const char *cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            if (!emit(*cursor, context)) {
                return written;
            }
            ++written;
            continue;
        }

        ++cursor;
        if (*cursor == '\0') {
            break;
        }

        {
            char pad = ' ';
            int width = 0;
            int precision = -1;
            int long_count = 0;
            int use_size_t = 0;
            char buffer[128];
            size_t length = 0;

            if (*cursor == '0') {
                pad = '0';
                ++cursor;
            }

            while (*cursor >= '0' && *cursor <= '9') {
                width = (width * 10) + (*cursor - '0');
                ++cursor;
            }

            if (*cursor == '.') {
                precision = 0;
                ++cursor;
                while (*cursor >= '0' && *cursor <= '9') {
                    precision = (precision * 10) + (*cursor - '0');
                    ++cursor;
                }
            }

            while (*cursor == 'l') {
                ++long_count;
                ++cursor;
            }
            if (*cursor == 'z') {
                use_size_t = 1;
                ++cursor;
            }

            switch (*cursor) {
                case '%':
                    if (!emit('%', context)) {
                        return written;
                    }
                    ++written;
                    break;
                case 'c':
                    buffer[0] = (char)va_arg(args, int);
                    buffer[1] = '\0';
                    written += sx_write_padded(emit, context, buffer, 1, width, pad);
                    break;
                case 's': {
                    const char *text = va_arg(args, const char *);
                    size_t text_length = 0;
                    if (text == 0) {
                        text = "(null)";
                    }
                    text_length = sx_strlen(text);
                    if (precision >= 0 && (size_t)precision < text_length) {
                        text_length = (size_t)precision;
                    }
                    written += sx_write_padded(emit, context, text, text_length, width, pad);
                    break;
                }
                case 'd':
                case 'i':
                    if (use_size_t || long_count > 0) {
                        length = sx_signed_to_string((long long)va_arg(args, long), buffer);
                    } else {
                        length = sx_signed_to_string((long long)va_arg(args, int), buffer);
                    }
                    length = sx_apply_numeric_precision(buffer, length, precision);
                    written += sx_write_padded(emit, context, buffer, length, width, pad);
                    break;
                case 'u':
                    if (use_size_t || long_count > 0) {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned long), 10u, 0, buffer);
                    } else {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned int), 10u, 0, buffer);
                    }
                    length = sx_apply_numeric_precision(buffer, length, precision);
                    written += sx_write_padded(emit, context, buffer, length, width, pad);
                    break;
                case 'x':
                case 'X':
                    if (use_size_t || long_count > 0) {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned long), 16u, *cursor == 'X', buffer);
                    } else {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned int), 16u, *cursor == 'X', buffer);
                    }
                    length = sx_apply_numeric_precision(buffer, length, precision);
                    written += sx_write_padded(emit, context, buffer, length, width, pad);
                    break;
                case 'p': {
                    uintptr_t value = (uintptr_t)va_arg(args, void *);
                    buffer[0] = '0';
                    buffer[1] = 'x';
                    length = 2 + sx_unsigned_to_string((unsigned long long)value, 16u, 0, buffer + 2);
                    written += sx_write_padded(emit, context, buffer, length, width, pad);
                    break;
                }
                case 'f':
                    (void)va_arg(args, double);
                    buffer[0] = '0';
                    buffer[1] = '\0';
                    length = 1;
                    written += sx_write_padded(emit, context, buffer, length, width, pad);
                    break;
                default:
                    if (!emit('%', context) || !emit(*cursor, context)) {
                        return written;
                    }
                    written += 2;
                    break;
            }
        }
    }

    return written;
}

static FILE *sx_allocate_file(void) {
    for (size_t index = 0; index < SX_FILE_POOL_CAPACITY; ++index) {
        if (!g_file_pool[index].in_use) {
            g_file_pool[index].fd = -1;
            g_file_pool[index].in_use = 1;
            g_file_pool[index].eof = 0;
            g_file_pool[index].error = 0;
            g_file_pool[index].is_stdio = 0;
            return &g_file_pool[index];
        }
    }
    return 0;
}

static void sx_release_file(FILE *stream) {
    if (stream != 0 && !stream->is_stdio) {
        stream->fd = -1;
        stream->in_use = 0;
        stream->eof = 0;
        stream->error = 0;
    }
}

void *sx_memcpy(void *destination, const void *source, size_t count) {
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;
    for (size_t index = 0; index < count; ++index) {
        dst[index] = src[index];
    }
    return destination;
}

void *sx_memset(void *destination, int value, size_t count) {
    unsigned char *dst = (unsigned char *)destination;
    for (size_t index = 0; index < count; ++index) {
        dst[index] = (unsigned char)value;
    }
    return destination;
}

int sx_memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *lhs = (const unsigned char *)left;
    const unsigned char *rhs = (const unsigned char *)right;
    for (size_t index = 0; index < count; ++index) {
        if (lhs[index] != rhs[index]) {
            return lhs[index] < rhs[index] ? -1 : 1;
        }
    }
    return 0;
}

void *sx_memmove(void *destination, const void *source, size_t count) {
    unsigned char *dst = (unsigned char *)destination;
    const unsigned char *src = (const unsigned char *)source;

    if (dst == src || count == 0) {
        return destination;
    }

    if (dst < src) {
        for (size_t index = 0; index < count; ++index) {
            dst[index] = src[index];
        }
    } else {
        for (size_t index = count; index > 0; --index) {
            dst[index - 1] = src[index - 1];
        }
    }
    return destination;
}

size_t sx_strlen(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

int sx_strcmp(const char *left, const char *right) {
    size_t index = 0;
    while (left[index] != '\0' || right[index] != '\0') {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
        ++index;
    }
    return 0;
}

int sx_strncmp(const char *left, const char *right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
        if (left[index] == '\0') {
            return 0;
        }
    }
    return 0;
}

char *sx_strcpy(char *destination, const char *source) {
    size_t index = 0;
    while (source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
    return destination;
}

char *sx_strncpy(char *destination, const char *source, size_t count) {
    size_t index = 0;
    while (index < count && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    while (index < count) {
        destination[index++] = '\0';
    }
    return destination;
}

char *sx_strchr(const char *text, int character) {
    while (*text != '\0') {
        if (*text == (char)character) {
            return (char *)text;
        }
        ++text;
    }
    return character == 0 ? (char *)text : 0;
}

char *sx_strrchr(const char *text, int character) {
    char *result = 0;
    while (*text != '\0') {
        if (*text == (char)character) {
            result = (char *)text;
        }
        ++text;
    }
    return character == 0 ? (char *)text : result;
}

char *sx_strstr(const char *haystack, const char *needle) {
    size_t needle_length = sx_strlen(needle);
    if (needle_length == 0) {
        return (char *)haystack;
    }

    for (; *haystack != '\0'; ++haystack) {
        if (sx_strncmp(haystack, needle, needle_length) == 0) {
            return (char *)haystack;
        }
    }
    return 0;
}

int sx_isspace(int character) {
    return character == ' '
        || character == '\t'
        || character == '\n'
        || character == '\r'
        || character == '\f'
        || character == '\v';
}

int sx_isprint(int character) {
    return character >= 32 && character <= 126;
}

int sx_isdigit(int character) {
    return character >= '0' && character <= '9';
}

int sx_tolower(int character) {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A' + 'a';
    }
    return character;
}

int sx_toupper(int character) {
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 'A';
    }
    return character;
}

int sx_strcasecmp(const char *left, const char *right) {
    while (*left != '\0' || *right != '\0') {
        const int lhs = sx_tolower((unsigned char)*left);
        const int rhs = sx_tolower((unsigned char)*right);
        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
        if (*left != '\0') {
            ++left;
        }
        if (*right != '\0') {
            ++right;
        }
    }
    return 0;
}

int sx_strncasecmp(const char *left, const char *right, unsigned long count) {
    for (unsigned long index = 0; index < count; ++index) {
        const int lhs = sx_tolower((unsigned char)left[index]);
        const int rhs = sx_tolower((unsigned char)right[index]);
        if (lhs != rhs) {
            return lhs < rhs ? -1 : 1;
        }
        if (left[index] == '\0') {
            return 0;
        }
    }
    return 0;
}

void *sx_malloc(size_t size) {
    sx_alloc_header *header;
    const size_t aligned = (size + sizeof(size_t) - 1u) & ~(sizeof(size_t) - 1u);
    const size_t total = aligned + sizeof(sx_alloc_header);

    if (total == sizeof(sx_alloc_header) || g_heap_used + total > SX_HEAP_SIZE) {
        g_errno = ENOMEM;
        return 0;
    }

    header = (sx_alloc_header *)(g_heap + g_heap_used);
    header->size = aligned;
    g_heap_used += total;

    return (void *)(header + 1);
}

void *sx_calloc(size_t count, size_t size) {
    const size_t total = count * size;
    void *pointer = sx_malloc(total);
    if (pointer != 0) {
        sx_memset(pointer, 0, total);
    }
    return pointer;
}

void *sx_realloc(void *pointer, size_t size) {
    sx_alloc_header *header;
    void *replacement;

    if (pointer == 0) {
        return sx_malloc(size);
    }
    if (size == 0) {
        return 0;
    }

    header = ((sx_alloc_header *)pointer) - 1;
    replacement = sx_malloc(size);
    if (replacement == 0) {
        return 0;
    }

    sx_memcpy(replacement, pointer, sx_min_size(header->size, size));
    return replacement;
}

void sx_free(void *pointer) {
    (void)pointer;
}

char *sx_strdup(const char *text) {
    const size_t length = sx_strlen(text) + 1;
    char *copy = (char *)sx_malloc(length);
    if (copy != 0) {
        sx_memcpy(copy, text, length);
    }
    return copy;
}

int sx_parse_int(const char *text, int *result) {
    int sign = 1;
    int base = 10;
    int value = 0;
    int digits = 0;

    if (text == 0 || result == 0) {
        return 0;
    }

    while (*text != '\0' && sx_isspace((unsigned char)*text)) {
        ++text;
    }

    if (*text == '-') {
        sign = -1;
        ++text;
    } else if (*text == '+') {
        ++text;
    }

    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    } else if (text[0] == '0' && sx_isdigit((unsigned char)text[1])) {
        base = 8;
        ++text;
    }

    while (*text != '\0') {
        int digit = -1;
        if (*text >= '0' && *text <= '9') {
            digit = *text - '0';
        } else if (*text >= 'a' && *text <= 'f') {
            digit = *text - 'a' + 10;
        } else if (*text >= 'A' && *text <= 'F') {
            digit = *text - 'A' + 10;
        } else {
            break;
        }

        if (digit >= base) {
            break;
        }

        value = (value * base) + digit;
        ++digits;
        ++text;
    }

    if (digits == 0) {
        return 0;
    }

    *result = value * sign;
    return 1;
}

int sx_atoi(const char *text) {
    int value = 0;
    (void)sx_parse_int(text, &value);
    return value;
}

double sx_atof(const char *text) {
    union {
        uint64_t bits;
        double value;
    } result = {0};

    (void)text;
    return result.value;
}

int sx_abs(int value) {
    return value < 0 ? -value : value;
}

char *sx_getenv(const char *name) {
    (void)name;
    return 0;
}

int sx_system(const char *command) {
    (void)command;
    g_errno = EINVAL;
    return -1;
}

void sx_abort(void) {
    sx_exit(EXIT_FAILURE);
}

double sx_fabs(double value) {
    double result = 0.0;
    asm volatile(
        "fldl %1\n\t"
        "fabs\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(value)
    );
    return result;
}

double sx_sin(double value) {
    double result = 0.0;
    asm volatile(
        "fldl %1\n\t"
        "fsin\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(value)
    );
    return result;
}

double sx_tan(double value) {
    double result = 0.0;
    asm volatile(
        "fldl %1\n\t"
        "fptan\n\t"
        "fstp %%st(0)\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(value)
    );
    return result;
}

double sx_atan(double value) {
    double result = 0.0;
    asm volatile(
        "fldl %1\n\t"
        "fld1\n\t"
        "fpatan\n\t"
        "fstpl %0"
        : "=m"(result)
        : "m"(value)
    );
    return result;
}

FILE *sx_fopen(const char *path, const char *mode) {
    FILE *stream = 0;
    unsigned long flags = 0;
    long fd = -1;

    if (path == 0 || mode == 0) {
        g_errno = EINVAL;
        return 0;
    }

    if (mode[0] == 'r') {
        flags = SAVANXP_OPEN_READ;
    } else if (mode[0] == 'w') {
        flags = SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_TRUNCATE;
    } else if (mode[0] == 'a') {
        flags = SAVANXP_OPEN_WRITE | SAVANXP_OPEN_CREATE | SAVANXP_OPEN_APPEND;
    } else {
        g_errno = EINVAL;
        return 0;
    }

    if (sx_strchr(mode, '+') != 0) {
        flags |= SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE;
    }

    fd = open_mode(path, flags);
    if (fd < 0) {
        sx_set_errno_from_result(fd);
        return 0;
    }

    stream = sx_allocate_file();
    if (stream == 0) {
        close((int)fd);
        g_errno = ENOMEM;
        return 0;
    }

    stream->fd = (int)fd;
    if (mode[0] == 'a') {
        (void)seek(stream->fd, 0, SAVANXP_SEEK_END);
    }
    return stream;
}

int sx_fclose(FILE *stream) {
    long result = 0;

    if (stream == 0) {
        g_errno = EINVAL;
        return EOF;
    }
    if (stream->is_stdio) {
        stream->eof = 0;
        stream->error = 0;
        return 0;
    }

    result = close(stream->fd);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return EOF;
    }

    sx_release_file(stream);
    return 0;
}

size_t sx_fread(void *buffer, size_t size, size_t count, FILE *stream) {
    long result = 0;
    size_t bytes = size * count;

    if (stream == 0 || buffer == 0 || size == 0 || count == 0) {
        return 0;
    }

    result = read(stream->fd, buffer, bytes);
    if (result < 0) {
        stream->error = 1;
        sx_set_errno_from_result(result);
        return 0;
    }
    if ((size_t)result < bytes) {
        stream->eof = 1;
    }

    return (size_t)result / size;
}

size_t sx_fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    long result = 0;
    size_t bytes = size * count;

    if (stream == 0 || buffer == 0 || size == 0 || count == 0) {
        return 0;
    }

    result = write(stream->fd, buffer, bytes);
    if (result < 0) {
        stream->error = 1;
        sx_set_errno_from_result(result);
        return 0;
    }

    return (size_t)result / size;
}

int sx_fseek(FILE *stream, long offset, int whence) {
    long result = 0;
    if (stream == 0) {
        g_errno = EINVAL;
        return -1;
    }

    result = seek(stream->fd, offset, whence);
    if (result < 0) {
        stream->error = 1;
        sx_set_errno_from_result(result);
        return -1;
    }

    stream->eof = 0;
    return 0;
}

long sx_ftell(FILE *stream) {
    long result = 0;
    if (stream == 0) {
        g_errno = EINVAL;
        return -1;
    }

    result = seek(stream->fd, 0, SAVANXP_SEEK_CUR);
    if (result < 0) {
        stream->error = 1;
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

int sx_fflush(FILE *stream) {
    (void)stream;
    return 0;
}

int sx_vfprintf(FILE *stream, const char *format, va_list args) {
    return sx_vformat(sx_emit_file_char, stream, format, args);
}

int sx_fprintf(FILE *stream, const char *format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vfprintf(stream, format, args);
    va_end(args);
    return written;
}

int sx_printf(const char *format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vfprintf(stdout, format, args);
    va_end(args);
    return written;
}

int sx_vsnprintf(char *buffer, size_t size, const char *format, va_list args) {
    sx_buffer_sink sink = {buffer, size, 0};
    int written = sx_vformat(sx_emit_buffer_char, &sink, format, args);

    if (size != 0) {
        const size_t terminator = sink.written < size ? sink.written : (size - 1);
        buffer[terminator] = '\0';
    }

    return written;
}

int sx_snprintf(char *buffer, size_t size, const char *format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vsnprintf(buffer, size, format, args);
    va_end(args);
    return written;
}

char *sx_fgets(char *buffer, int size, FILE *stream) {
    int index = 0;

    if (buffer == 0 || size <= 1 || stream == 0) {
        return 0;
    }

    while (index + 1 < size) {
        char character = '\0';
        size_t read_count = sx_fread(&character, 1, 1, stream);
        if (read_count != 1) {
            break;
        }
        buffer[index++] = character;
        if (character == '\n') {
            break;
        }
    }

    if (index == 0) {
        return 0;
    }

    buffer[index] = '\0';
    return buffer;
}

int sx_feof(FILE *stream) {
    return stream != 0 ? stream->eof : 1;
}

int sx_putchar(int character) {
    char value = (char)character;
    if (write(stdout->fd, &value, 1) < 0) {
        return EOF;
    }
    return character;
}

int sx_puts(const char *text) {
    return sx_printf("%s\n", text);
}

int sx_remove(const char *path) {
    long result = unlink(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_rename(const char *old_path, const char *new_path) {
    long result = rename(old_path, new_path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_make_dirs(const char *path) {
    char partial[128];
    size_t length = sx_strlen(path);
    size_t index = 0;
    long result = 0;

    if (length == 0 || length >= sizeof(partial)) {
        g_errno = EINVAL;
        return -1;
    }

    for (index = 0; index < length; ++index) {
        partial[index] = path[index];
        partial[index + 1] = '\0';

        if (partial[index] != '/' || index == 0) {
            continue;
        }

        result = mkdir(partial);
        if (result < 0 && result_error_code(result) != SAVANXP_EEXIST) {
            sx_set_errno_from_result(result);
            return -1;
        }
    }

    result = mkdir(partial);
    if (result < 0 && result_error_code(result) != SAVANXP_EEXIST) {
        sx_set_errno_from_result(result);
        return -1;
    }

    return 0;
}

void sx_register_shutdown(void (*callback)(void)) {
    if (callback == 0 || g_shutdown_count >= SX_SHUTDOWN_CAPACITY) {
        return;
    }
    g_shutdown_callbacks[g_shutdown_count++] = callback;
}

int *sx_errno_location(void) {
    return &g_errno;
}

void sx_exit(int code) {
    if (!g_exit_started) {
        g_exit_started = 1;
        I_Quit();
    }
    sx_run_shutdowns();
    exit(code);
}
