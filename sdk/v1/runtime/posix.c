#include "savanxp/libc.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#define EOF (-1)
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_REG 8
#define DT_SOCK 12
#define SX_HEAP_SIZE (48u * 1024u * 1024u)
#define SX_FILE_POOL_CAPACITY 32
#define SX_DIR_POOL_CAPACITY 16
#define SX_SOCKET_STATE_CAPACITY 32
#define SX_PATH_CAPACITY 256
#define SX_FILE_BUFFER_CAPACITY 512

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int socklen_t;
typedef long time_t;
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

typedef struct sx_FILE FILE;
typedef struct sx_DIR DIR;

struct stat {
    unsigned int st_mode;
    unsigned int st_size;
};

struct dirent {
    unsigned char d_type;
    char d_name[256];
};

struct sockaddr {
    unsigned short sa_family;
    char sa_data[14];
};

struct in_addr {
    in_addr_t s_addr;
};

struct sockaddr_in {
    unsigned short sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct sx_FILE {
    int fd;
    int in_use;
    int eof;
    int error;
    int is_stdio;
    int can_read;
    int can_write;
    size_t write_buffer_used;
    unsigned char write_buffer[SX_FILE_BUFFER_CAPACITY];
};

struct sx_DIR {
    int fd;
    int in_use;
    char path[SX_PATH_CAPACITY];
    struct dirent entry;
};

typedef struct sx_alloc_header {
    size_t size;
} sx_alloc_header;

struct sx_socket_state {
    int in_use;
    int fd;
    unsigned long recv_timeout_ms;
    unsigned long send_timeout_ms;
};

static unsigned char g_heap[SX_HEAP_SIZE];
static size_t g_heap_used = 0;
static struct sx_FILE g_file_pool[SX_FILE_POOL_CAPACITY] = {};
static struct sx_FILE g_stdin_file = {0, 1, 0, 0, 1, 1, 0, 0, {0}};
static struct sx_FILE g_stdout_file = {1, 1, 0, 0, 1, 0, 1, 0, {0}};
static struct sx_FILE g_stderr_file = {2, 1, 0, 0, 1, 0, 1, 0, {0}};
static struct sx_DIR g_dir_pool[SX_DIR_POOL_CAPACITY] = {};
static struct sx_socket_state g_socket_states[SX_SOCKET_STATE_CAPACITY] = {};

FILE* stdin = &g_stdin_file;
FILE* stdout = &g_stdout_file;
FILE* stderr = &g_stderr_file;

static int g_errno = 0;

void* sx_malloc(size_t size);
int sx_usleep(unsigned long microseconds);

int* sx_errno_location(void) {
    return &g_errno;
}

size_t sx_fwrite(const void* buffer, size_t size, size_t count, FILE* stream);

static int sx_result_to_errno(long result) {
    return result < 0 ? result_error_code(result) : 0;
}

static void sx_set_errno_from_result(long result) {
    g_errno = sx_result_to_errno(result);
}

static size_t sx_min_size(size_t left, size_t right) {
    return left < right ? left : right;
}

static struct sx_socket_state* sx_find_socket_state(int fd) {
    for (size_t index = 0; index < SX_SOCKET_STATE_CAPACITY; ++index) {
        if (g_socket_states[index].in_use && g_socket_states[index].fd == fd) {
            return &g_socket_states[index];
        }
    }
    return 0;
}

static void sx_track_socket(int fd) {
    for (size_t index = 0; index < SX_SOCKET_STATE_CAPACITY; ++index) {
        if (!g_socket_states[index].in_use) {
            g_socket_states[index].in_use = 1;
            g_socket_states[index].fd = fd;
            g_socket_states[index].recv_timeout_ms = 0;
            g_socket_states[index].send_timeout_ms = 5000;
            return;
        }
    }
}

static void sx_untrack_socket(int fd) {
    struct sx_socket_state* state = sx_find_socket_state(fd);
    if (state != 0) {
        memset(state, 0, sizeof(*state));
    }
}

static unsigned char sx_dtype_from_mode(unsigned int mode) {
    switch (mode & SAVANXP_S_IFMT) {
        case SAVANXP_S_IFDIR:
            return DT_DIR;
        case SAVANXP_S_IFREG:
            return DT_REG;
        case SAVANXP_S_IFCHR:
            return DT_CHR;
        case SAVANXP_S_IFIFO:
            return DT_FIFO;
        case SAVANXP_S_IFSOCK:
            return DT_SOCK;
        default:
            return DT_UNKNOWN;
    }
}

static int sx_join_dir_path(const char* base, const char* leaf, char* output, size_t capacity) {
    const size_t base_length = strlen(base);
    const size_t leaf_length = strlen(leaf);
    size_t written = 0;

    if (base_length + leaf_length + 2 > capacity) {
        return 0;
    }

    memcpy(output, base, base_length);
    written = base_length;
    if (written == 0) {
        output[written++] = '/';
    } else if (!(written == 1 && output[0] == '/')) {
        output[written++] = '/';
    }
    memcpy(output + written, leaf, leaf_length);
    written += leaf_length;
    output[written] = '\0';
    return 1;
}

void* sx_memcpy(void* destination, const void* source, size_t count) {
    return memcpy(destination, source, count);
}

void* sx_memset(void* destination, int value, size_t count) {
    return memset(destination, value, count);
}

int sx_memcmp(const void* left, const void* right, size_t count) {
    const unsigned char* lhs = (const unsigned char*)left;
    const unsigned char* rhs = (const unsigned char*)right;
    for (size_t index = 0; index < count; ++index) {
        if (lhs[index] != rhs[index]) {
            return lhs[index] < rhs[index] ? -1 : 1;
        }
    }
    return 0;
}

void* sx_memmove(void* destination, const void* source, size_t count) {
    unsigned char* dst = (unsigned char*)destination;
    const unsigned char* src = (const unsigned char*)source;
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

size_t sx_strlen(const char* text) {
    return strlen(text);
}

int sx_strcmp(const char* left, const char* right) {
    return strcmp(left, right);
}

int sx_strncmp(const char* left, const char* right, size_t count) {
    return strncmp(left, right, count);
}

char* sx_strcpy(char* destination, const char* source) {
    return strcpy(destination, source);
}

char* sx_strncpy(char* destination, const char* source, size_t count) {
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

char* sx_strchr(const char* text, int character) {
    while (*text != '\0') {
        if (*text == (char)character) {
            return (char*)text;
        }
        ++text;
    }
    return character == 0 ? (char*)text : 0;
}

char* sx_strrchr(const char* text, int character) {
    char* result = 0;
    while (*text != '\0') {
        if (*text == (char)character) {
            result = (char*)text;
        }
        ++text;
    }
    return character == 0 ? (char*)text : result;
}

char* sx_strstr(const char* haystack, const char* needle) {
    const size_t needle_length = sx_strlen(needle);
    if (needle_length == 0) {
        return (char*)haystack;
    }
    for (; *haystack != '\0'; ++haystack) {
        if (sx_strncmp(haystack, needle, needle_length) == 0) {
            return (char*)haystack;
        }
    }
    return 0;
}

char* sx_strdup(const char* text) {
    const size_t length = sx_strlen(text) + 1;
    char* copy = (char*)sx_malloc(length);
    if (copy != 0) {
        sx_memcpy(copy, text, length);
    }
    return copy;
}

int sx_isspace(int character) {
    return character == ' ' || character == '\t' || character == '\n'
        || character == '\r' || character == '\f' || character == '\v';
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

int sx_strcasecmp(const char* left, const char* right) {
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

int sx_strncasecmp(const char* left, const char* right, unsigned long count) {
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

void* sx_malloc(size_t size) {
    sx_alloc_header* header = 0;
    const size_t aligned = (size + sizeof(size_t) - 1u) & ~(sizeof(size_t) - 1u);
    const size_t total = aligned + sizeof(sx_alloc_header);
    if (total == sizeof(sx_alloc_header) || g_heap_used + total > SX_HEAP_SIZE) {
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }
    header = (sx_alloc_header*)(g_heap + g_heap_used);
    header->size = aligned;
    g_heap_used += total;
    return (void*)(header + 1);
}

void* sx_calloc(size_t count, size_t size) {
    const size_t total = count * size;
    void* pointer = sx_malloc(total);
    if (pointer != 0) {
        sx_memset(pointer, 0, total);
    }
    return pointer;
}

void* sx_realloc(void* pointer, size_t size) {
    sx_alloc_header* header = 0;
    void* replacement = 0;
    if (pointer == 0) {
        return sx_malloc(size);
    }
    if (size == 0) {
        return 0;
    }
    header = ((sx_alloc_header*)pointer) - 1;
    replacement = sx_malloc(size);
    if (replacement == 0) {
        return 0;
    }
    sx_memcpy(replacement, pointer, sx_min_size(header->size, size));
    return replacement;
}

void sx_free(void* pointer) {
    (void)pointer;
}

static unsigned long sx_parse_unsigned(const char* text, char** endptr, int base, int* success) {
    const char* cursor = text;
    unsigned long value = 0;
    int digits = 0;
    *success = 0;
    while (*cursor != '\0' && sx_isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (base == 0) {
        if (cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
            base = 16;
            cursor += 2;
        } else if (cursor[0] == '0') {
            base = 8;
            ++cursor;
        } else {
            base = 10;
        }
    } else if (base == 16 && cursor[0] == '0' && (cursor[1] == 'x' || cursor[1] == 'X')) {
        cursor += 2;
    }
    while (*cursor != '\0') {
        int digit = -1;
        if (*cursor >= '0' && *cursor <= '9') {
            digit = *cursor - '0';
        } else if (*cursor >= 'a' && *cursor <= 'f') {
            digit = *cursor - 'a' + 10;
        } else if (*cursor >= 'A' && *cursor <= 'F') {
            digit = *cursor - 'A' + 10;
        } else {
            break;
        }
        if (digit >= base) {
            break;
        }
        value = (value * (unsigned long)base) + (unsigned long)digit;
        ++digits;
        ++cursor;
    }
    if (endptr != 0) {
        *endptr = (char*)(digits == 0 ? text : cursor);
    }
    *success = digits != 0;
    return value;
}

long sx_strtol(const char* text, char** endptr, int base) {
    const char* cursor = text;
    int negative = 0;
    int success = 0;
    unsigned long value = 0;
    while (*cursor != '\0' && sx_isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor == '-') {
        negative = 1;
        ++cursor;
    } else if (*cursor == '+') {
        ++cursor;
    }
    value = sx_parse_unsigned(cursor, endptr, base, &success);
    if (!success) {
        g_errno = SAVANXP_EINVAL;
        if (endptr != 0) {
            *endptr = (char*)text;
        }
        return 0;
    }
    return negative ? -(long)value : (long)value;
}

unsigned long sx_strtoul(const char* text, char** endptr, int base) {
    int success = 0;
    unsigned long value = sx_parse_unsigned(text, endptr, base, &success);
    if (!success) {
        g_errno = SAVANXP_EINVAL;
        if (endptr != 0) {
            *endptr = (char*)text;
        }
        return 0;
    }
    return value;
}

int sx_atoi(const char* text) {
    return (int)sx_strtol(text, 0, 10);
}

double sx_atof(const char* text) {
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

char* sx_getenv(const char* name) {
    (void)name;
    return 0;
}

int sx_system(const char* command) {
    (void)command;
    g_errno = SAVANXP_EINVAL;
    return -1;
}

void sx_abort(void) {
    exit(1);
}

double sx_fabs(double value) {
    double result = 0.0;
    asm volatile("fldl %1\n\tfabs\n\tfstpl %0" : "=m"(result) : "m"(value));
    return result;
}

double sx_sin(double value) {
    double result = 0.0;
    asm volatile("fldl %1\n\tfsin\n\tfstpl %0" : "=m"(result) : "m"(value));
    return result;
}

double sx_tan(double value) {
    double result = 0.0;
    asm volatile("fldl %1\n\tfptan\n\tfstp %%st(0)\n\tfstpl %0" : "=m"(result) : "m"(value));
    return result;
}

double sx_atan(double value) {
    double result = 0.0;
    asm volatile("fldl %1\n\tfld1\n\tfpatan\n\tfstpl %0" : "=m"(result) : "m"(value));
    return result;
}

ssize_t sx_read(int fd, void* buffer, size_t count) {
    long result = read(fd, buffer, count);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

ssize_t sx_write(int fd, const void* buffer, size_t count) {
    long result = write(fd, buffer, count);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

int sx_close(int fd) {
    long result = close(fd);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    sx_untrack_socket(fd);
    return 0;
}

int sx_open(const char* path, int flags, ...) {
    unsigned long raw_flags = 0;
    long result = 0;
    if ((flags & O_RDWR) == O_RDWR) {
        raw_flags |= SAVANXP_OPEN_READ | SAVANXP_OPEN_WRITE;
    } else if ((flags & O_WRONLY) != 0) {
        raw_flags |= SAVANXP_OPEN_WRITE;
    } else {
        raw_flags |= SAVANXP_OPEN_READ;
    }
    if ((flags & O_CREAT) != 0) {
        raw_flags |= SAVANXP_OPEN_CREATE;
    }
    if ((flags & O_TRUNC) != 0) {
        raw_flags |= SAVANXP_OPEN_TRUNCATE;
    }
    if ((flags & O_APPEND) != 0) {
        raw_flags |= SAVANXP_OPEN_APPEND;
    }
    result = open_mode(path, raw_flags);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

off_t sx_lseek(int fd, off_t offset, int whence) {
    long result = seek(fd, offset, whence);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

int sx_unlink(const char* path) {
    long result = unlink(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_rmdir(const char* path) {
    long result = rmdir(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_dup(int fd) {
    long result = dup(fd);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int sx_dup2(int oldfd, int newfd) {
    long result = dup2(oldfd, newfd);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int sx_pipe(int fds[2]) {
    long result = pipe(fds);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_stat(const char* path, struct stat* info) {
    struct savanxp_stat raw = {};
    long result = savanxp_stat(path, &raw);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    if (info != 0) {
        info->st_mode = raw.st_mode;
        info->st_size = raw.st_size;
    }
    return 0;
}

int sx_fstat(int fd, struct stat* info) {
    struct savanxp_stat raw = {};
    long result = savanxp_fstat(fd, &raw);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    if (info != 0) {
        info->st_mode = raw.st_mode;
        info->st_size = raw.st_size;
    }
    return 0;
}

int sx_access(const char* path, int mode) {
    struct stat info = {};
    if (sx_stat(path, &info) < 0) {
        return -1;
    }
    if ((mode & W_OK) != 0 && (info.st_mode & 0222u) == 0) {
        g_errno = SAVANXP_EACCES;
        return -1;
    }
    if ((mode & X_OK) != 0 && (info.st_mode & 0111u) == 0) {
        g_errno = SAVANXP_EACCES;
        return -1;
    }
    return 0;
}

int sx_isatty(int fd) {
    struct stat info = {};
    if (sx_fstat(fd, &info) < 0) {
        return 0;
    }
    if ((info.st_mode & SAVANXP_S_IFMT) == SAVANXP_S_IFCHR) {
        return 1;
    }
    g_errno = SAVANXP_ENOTTY;
    return 0;
}

int sx_chdir(const char* path) {
    long result = savanxp_chdir(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

char* sx_getcwd(char* buffer, size_t size) {
    long result = savanxp_getcwd(buffer, size);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return 0;
    }
    return buffer;
}

pid_t sx_getpid(void) {
    long result = savanxp_getpid();
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (pid_t)result;
}

int sx_sync(void) {
    long result = sync();
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

DIR* sx_opendir(const char* path) {
    struct stat info = {};
    int fd = -1;
    if (sx_stat(path, &info) < 0) {
        return 0;
    }
    if ((info.st_mode & SAVANXP_S_IFMT) != SAVANXP_S_IFDIR) {
        g_errno = SAVANXP_ENOTDIR;
        return 0;
    }
    fd = sx_open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    for (size_t index = 0; index < SX_DIR_POOL_CAPACITY; ++index) {
        if (!g_dir_pool[index].in_use) {
            memset(&g_dir_pool[index], 0, sizeof(g_dir_pool[index]));
            g_dir_pool[index].in_use = 1;
            g_dir_pool[index].fd = fd;
            sx_strncpy(g_dir_pool[index].path, path, sizeof(g_dir_pool[index].path) - 1);
            return &g_dir_pool[index];
        }
    }
    sx_close(fd);
    g_errno = SAVANXP_ENOMEM;
    return 0;
}

struct dirent* sx_readdir(DIR* directory) {
    char name[256] = {};
    char full_path[SX_PATH_CAPACITY] = {};
    struct stat info = {};
    long result = 0;
    if (directory == 0 || !directory->in_use) {
        g_errno = SAVANXP_EBADF;
        return 0;
    }
    result = readdir(directory->fd, name, sizeof(name));
    if (result < 0) {
        sx_set_errno_from_result(result);
        return 0;
    }
    if (result == 0 || name[0] == '\0') {
        return 0;
    }
    memset(&directory->entry, 0, sizeof(directory->entry));
    sx_strncpy(directory->entry.d_name, name, sizeof(directory->entry.d_name) - 1);
    directory->entry.d_type = DT_UNKNOWN;
    if (sx_join_dir_path(directory->path, name, full_path, sizeof(full_path)) && sx_stat(full_path, &info) == 0) {
        directory->entry.d_type = sx_dtype_from_mode(info.st_mode);
    }
    return &directory->entry;
}

int sx_closedir(DIR* directory) {
    if (directory == 0 || !directory->in_use) {
        g_errno = SAVANXP_EBADF;
        return -1;
    }
    sx_close(directory->fd);
    memset(directory, 0, sizeof(*directory));
    return 0;
}

void sx_rewinddir(DIR* directory) {
    int reopened = -1;
    if (directory == 0 || !directory->in_use) {
        return;
    }
    sx_close(directory->fd);
    reopened = sx_open(directory->path, O_RDONLY);
    if (reopened >= 0) {
        directory->fd = reopened;
    } else {
        directory->in_use = 0;
    }
}

static FILE* sx_allocate_file(void) {
    for (size_t index = 0; index < SX_FILE_POOL_CAPACITY; ++index) {
        if (!g_file_pool[index].in_use) {
            memset(&g_file_pool[index], 0, sizeof(g_file_pool[index]));
            g_file_pool[index].fd = -1;
            g_file_pool[index].in_use = 1;
            return &g_file_pool[index];
        }
    }
    return 0;
}

static void sx_release_file(FILE* stream) {
    if (stream != 0 && !stream->is_stdio) {
        memset(stream, 0, sizeof(*stream));
        stream->fd = -1;
    }
}

static int sx_emit_file_char(char character, void* context) {
    FILE* stream = (FILE*)context;
    if (sx_fwrite(&character, 1, 1, stream) != 1) {
        stream->error = 1;
        return 0;
    }
    return 1;
}

typedef struct sx_buffer_sink {
    char* buffer;
    size_t size;
    size_t written;
} sx_buffer_sink;

static int sx_emit_buffer_char(char character, void* context) {
    sx_buffer_sink* sink = (sx_buffer_sink*)context;
    if (sink->size != 0 && sink->written + 1 < sink->size) {
        sink->buffer[sink->written] = character;
    }
    sink->written += 1;
    return 1;
}

static int sx_flush_stream(FILE* stream) {
    size_t written = 0;
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return EOF;
    }
    if (stream->write_buffer_used == 0) {
        return 0;
    }

    while (written < stream->write_buffer_used) {
        const ssize_t result = sx_write(
            stream->fd,
            stream->write_buffer + written,
            stream->write_buffer_used - written);
        if (result <= 0) {
            stream->error = 1;
            if (result == 0) {
                g_errno = SAVANXP_EIO;
            }
            return EOF;
        }
        written += (size_t)result;
    }

    stream->write_buffer_used = 0;
    return 0;
}

static size_t sx_buffered_write(FILE* stream, const unsigned char* buffer, size_t total) {
    size_t consumed = 0;
    if (stream == 0 || buffer == 0) {
        return 0;
    }

    while (consumed < total) {
        if (stream->write_buffer_used == 0 && (total - consumed) >= SX_FILE_BUFFER_CAPACITY) {
            const ssize_t direct = sx_write(stream->fd, buffer + consumed, total - consumed);
            if (direct <= 0) {
                stream->error = 1;
                if (direct == 0) {
                    g_errno = SAVANXP_EIO;
                }
                break;
            }
            consumed += (size_t)direct;
            continue;
        }

        {
            const size_t available = SX_FILE_BUFFER_CAPACITY - stream->write_buffer_used;
            const size_t chunk = (total - consumed) < available ? (total - consumed) : available;
            sx_memcpy(stream->write_buffer + stream->write_buffer_used, buffer + consumed, chunk);
            stream->write_buffer_used += chunk;
            consumed += chunk;
        }

        if (stream->write_buffer_used == SX_FILE_BUFFER_CAPACITY && sx_flush_stream(stream) == EOF) {
            break;
        }
    }

    return consumed;
}

static size_t sx_direct_write(FILE* stream, const unsigned char* buffer, size_t total) {
    size_t consumed = 0;
    if (stream == 0 || buffer == 0) {
        return 0;
    }

    while (consumed < total) {
        const ssize_t result = sx_write(stream->fd, buffer + consumed, total - consumed);
        if (result <= 0) {
            stream->error = 1;
            if (result == 0) {
                g_errno = SAVANXP_EIO;
            }
            break;
        }
        consumed += (size_t)result;
    }

    return consumed;
}

static int sx_write_padded(int (*emit)(char, void*), void* context, const char* text, size_t text_length, int width, char pad) {
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

static size_t sx_unsigned_to_string(unsigned long long value, unsigned base, int uppercase, char* buffer) {
    static const char kDigitsLower[] = "0123456789abcdef";
    static const char kDigitsUpper[] = "0123456789ABCDEF";
    const char* digits = uppercase ? kDigitsUpper : kDigitsLower;
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

static size_t sx_signed_to_string(long long value, char* buffer) {
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

static size_t sx_apply_numeric_precision(char* buffer, size_t length, int precision) {
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

static int sx_vformat(int (*emit)(char, void*), void* context, const char* format, va_list args) {
    int written = 0;

    for (const char* cursor = format; *cursor != '\0'; ++cursor) {
        if (*cursor != '%') {
            if (!emit(*cursor, context)) {
                return written;
            }
            ++written;
            continue;
        }
        ++cursor;

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
                    const char* text = va_arg(args, const char*);
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
                    uintptr_t value = (uintptr_t)va_arg(args, void*);
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

FILE* sx_fopen(const char* path, const char* mode) {
    int flags = O_RDONLY;
    int fd = -1;
    FILE* stream = 0;
    if (path == 0 || mode == 0) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }
    if (mode[0] == 'w') {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT | O_APPEND;
    }
    if (sx_strchr(mode, '+') != 0) {
        flags &= ~O_WRONLY;
        flags |= O_RDWR;
    }
    fd = sx_open(path, flags);
    if (fd < 0) {
        return 0;
    }
    stream = sx_allocate_file();
    if (stream == 0) {
        sx_close(fd);
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }
    stream->fd = fd;
    stream->can_read = (flags & O_RDWR) == O_RDWR || (flags & O_WRONLY) == 0;
    stream->can_write = (flags & O_WRONLY) != 0 || (flags & O_RDWR) == O_RDWR;
    return stream;
}

int sx_fclose(FILE* stream) {
    int flush_result = 0;
    int close_result = 0;
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return EOF;
    }
    if (stream->is_stdio) {
        return 0;
    }
    flush_result = sx_flush_stream(stream);
    close_result = sx_close(stream->fd);
    sx_release_file(stream);
    return (flush_result == EOF || close_result < 0) ? EOF : 0;
}

size_t sx_fread(void* buffer, size_t size, size_t count, FILE* stream) {
    ssize_t result = 0;
    if (stream == 0 || buffer == 0 || size == 0 || count == 0) {
        return 0;
    }
    if (stream->can_write && stream->write_buffer_used != 0 && sx_flush_stream(stream) == EOF) {
        return 0;
    }
    result = sx_read(stream->fd, buffer, size * count);
    if (result < 0) {
        stream->error = 1;
        return 0;
    }
    if ((size_t)result < size * count) {
        stream->eof = 1;
    }
    return (size_t)result / size;
}

size_t sx_fwrite(const void* buffer, size_t size, size_t count, FILE* stream) {
    if (stream == 0 || buffer == 0 || size == 0 || count == 0) {
        return 0;
    }
    if (!stream->can_write) {
        g_errno = SAVANXP_EBADF;
        stream->error = 1;
        return 0;
    }
    {
        const size_t total = size * count;
        const size_t written = stream->is_stdio
            ? sx_direct_write(stream, (const unsigned char*)buffer, total)
            : sx_buffered_write(stream, (const unsigned char*)buffer, total);
        return written / size;
    }
}

int sx_fseek(FILE* stream, long offset, int whence) {
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    if (stream->write_buffer_used != 0 && sx_flush_stream(stream) == EOF) {
        return -1;
    }
    stream->eof = 0;
    return sx_lseek(stream->fd, offset, whence) < 0 ? -1 : 0;
}

long sx_ftell(FILE* stream) {
    long position = 0;
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    position = sx_lseek(stream->fd, 0, SEEK_CUR);
    if (position < 0) {
        stream->error = 1;
        return -1;
    }
    if (stream->can_write && stream->write_buffer_used != 0) {
        position += (long)stream->write_buffer_used;
    }
    return position;
}

int sx_fflush(FILE* stream) {
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return EOF;
    }
    return sx_flush_stream(stream);
}

int sx_vfprintf(FILE* stream, const char* format, va_list args) {
    return sx_vformat(sx_emit_file_char, stream, format, args);
}

int sx_fprintf(FILE* stream, const char* format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vfprintf(stream, format, args);
    va_end(args);
    return written;
}

int sx_printf(const char* format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vfprintf(stdout, format, args);
    va_end(args);
    return written;
}

int sx_vsnprintf(char* buffer, size_t size, const char* format, va_list args) {
    sx_buffer_sink sink = {buffer, size, 0};
    int written = sx_vformat(sx_emit_buffer_char, &sink, format, args);
    if (size != 0) {
        const size_t terminator = sink.written < size ? sink.written : (size - 1);
        buffer[terminator] = '\0';
    }
    return written;
}

int sx_snprintf(char* buffer, size_t size, const char* format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vsnprintf(buffer, size, format, args);
    va_end(args);
    return written;
}

char* sx_fgets(char* buffer, int size, FILE* stream) {
    int index = 0;
    if (buffer == 0 || size <= 1 || stream == 0) {
        return 0;
    }
    while (index + 1 < size) {
        char character = '\0';
        if (sx_fread(&character, 1, 1, stream) != 1) {
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

int sx_feof(FILE* stream) {
    return stream != 0 ? stream->eof : 1;
}

int sx_putchar(int character) {
    char value = (char)character;
    return sx_write(stdout->fd, &value, 1) < 0 ? EOF : character;
}

int sx_puts(const char* text) {
    return sx_printf("%s\n", text);
}

int sx_remove(const char* path) {
    return sx_unlink(path);
}

int sx_rename(const char* old_path, const char* new_path) {
    long result = rename(old_path, new_path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

pid_t sx_waitpid(pid_t pid, int* status, int options) {
    long result = 0;
    if (options != 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    result = waitpid(pid, status);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (pid_t)result;
}

unsigned sx_sleep(unsigned seconds) {
    return sx_usleep((unsigned long)seconds * 1000000UL) < 0 ? seconds : 0;
}

int sx_usleep(unsigned long microseconds) {
    unsigned long milliseconds = microseconds / 1000UL;
    if (microseconds % 1000UL != 0) {
        ++milliseconds;
    }
    if (sleep_ms(milliseconds) < 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    return 0;
}

time_t sx_time(time_t* out_value) {
    time_t value = (time_t)(uptime_ms() / 1000UL);
    if (out_value != 0) {
        *out_value = value;
    }
    return value;
}

int sx_clock_gettime(int clock_id, struct timespec* value) {
    unsigned long milliseconds = uptime_ms();
    (void)clock_id;
    if (value == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    value->tv_sec = (time_t)(milliseconds / 1000UL);
    value->tv_nsec = (long)((milliseconds % 1000UL) * 1000000UL);
    return 0;
}

int sx_nanosleep(const struct timespec* request, struct timespec* remaining) {
    unsigned long milliseconds = 0;
    (void)remaining;
    if (request == 0 || request->tv_sec < 0 || request->tv_nsec < 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    milliseconds = (unsigned long)request->tv_sec * 1000UL;
    milliseconds += (unsigned long)((request->tv_nsec + 999999L) / 1000000L);
    return sx_usleep(milliseconds * 1000UL);
}

unsigned long sx_htonl(unsigned long value) {
    return ((value & 0x000000ffUL) << 24) | ((value & 0x0000ff00UL) << 8)
        | ((value & 0x00ff0000UL) >> 8) | ((value & 0xff000000UL) >> 24);
}

unsigned short sx_htons(unsigned short value) {
    return (unsigned short)(((value & 0x00ffu) << 8) | ((value & 0xff00u) >> 8));
}

unsigned long sx_ntohl(unsigned long value) {
    return sx_htonl(value);
}

unsigned short sx_ntohs(unsigned short value) {
    return sx_htons(value);
}

in_addr_t sx_inet_addr(const char* text) {
    uint32_t parts[4] = {0, 0, 0, 0};
    size_t index = 0;
    const char* cursor = text;
    while (index < 4) {
        char* end = 0;
        unsigned long value = sx_strtoul(cursor, &end, 10);
        if (end == cursor || value > 255) {
            return 0xffffffffu;
        }
        parts[index++] = (uint32_t)value;
        if (*end == '\0') {
            break;
        }
        if (*end != '.') {
            return 0xffffffffu;
        }
        cursor = end + 1;
    }
    if (index != 4) {
        return 0xffffffffu;
    }
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

char* sx_inet_ntoa(struct in_addr address) {
    static char buffer[16];
    sx_snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u",
        (unsigned int)((address.s_addr >> 24) & 0xffu),
        (unsigned int)((address.s_addr >> 16) & 0xffu),
        (unsigned int)((address.s_addr >> 8) & 0xffu),
        (unsigned int)(address.s_addr & 0xffu));
    return buffer;
}

int sx_inet_pton(int family, const char* source, void* destination) {
    in_addr_t value = 0;
    if (family != 1 || source == 0 || destination == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    value = sx_inet_addr(source);
    if (value == 0xffffffffu && sx_strcmp(source, "255.255.255.255") != 0) {
        return 0;
    }
    *(in_addr_t*)destination = sx_htonl(value);
    return 1;
}

const char* sx_inet_ntop(int family, const void* source, char* destination, unsigned long size) {
    unsigned long value = 0;
    if (family != 1 || source == 0 || destination == 0 || size < 16) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }
    value = sx_ntohl(*(const unsigned long*)source);
    sx_snprintf(destination, (size_t)size, "%u.%u.%u.%u",
        (unsigned int)((value >> 24) & 0xffu),
        (unsigned int)((value >> 16) & 0xffu),
        (unsigned int)((value >> 8) & 0xffu),
        (unsigned int)(value & 0xffu));
    return destination;
}

int sx_socket(int domain, int type, int protocol) {
    long result = socket((unsigned long)domain, (unsigned long)type, (unsigned long)protocol);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    sx_track_socket((int)result);
    return (int)result;
}

int sx_bind(int fd, const struct sockaddr* address, socklen_t address_length) {
    const struct sockaddr_in* in_address = (const struct sockaddr_in*)address;
    struct savanxp_sockaddr_in raw = {};
    long result = 0;
    if (address == 0 || address_length < sizeof(*in_address) || in_address->sin_family != 1) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    raw.ipv4 = (uint32_t)sx_ntohl(in_address->sin_addr.s_addr);
    raw.port = sx_ntohs(in_address->sin_port);
    result = bind(fd, &raw);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_connect(int fd, const struct sockaddr* address, socklen_t address_length) {
    const struct sockaddr_in* in_address = (const struct sockaddr_in*)address;
    struct savanxp_sockaddr_in raw = {};
    struct sx_socket_state* state = sx_find_socket_state(fd);
    long result = 0;
    if (address == 0 || address_length < sizeof(*in_address) || in_address->sin_family != 1) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    raw.ipv4 = (uint32_t)sx_ntohl(in_address->sin_addr.s_addr);
    raw.port = sx_ntohs(in_address->sin_port);
    result = connect(fd, &raw, state != 0 ? state->send_timeout_ms : 5000u);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

ssize_t sx_sendto(int fd, const void* buffer, size_t count, int flags, const struct sockaddr* address, socklen_t address_length) {
    const struct sockaddr_in* in_address = (const struct sockaddr_in*)address;
    struct savanxp_sockaddr_in raw = {};
    long result = 0;
    (void)flags;
    if (address != 0) {
        if (address_length < sizeof(*in_address) || in_address->sin_family != 1) {
            g_errno = SAVANXP_EINVAL;
            return -1;
        }
        raw.ipv4 = (uint32_t)sx_ntohl(in_address->sin_addr.s_addr);
        raw.port = sx_ntohs(in_address->sin_port);
        result = sendto(fd, buffer, count, &raw);
    } else {
        result = write(fd, buffer, count);
    }
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

ssize_t sx_recvfrom(int fd, void* buffer, size_t count, int flags, struct sockaddr* address, socklen_t* address_length) {
    struct savanxp_sockaddr_in raw = {};
    struct sx_socket_state* state = sx_find_socket_state(fd);
    long result = 0;
    (void)flags;
    result = recvfrom(fd, buffer, count, address != 0 ? &raw : 0, state != 0 ? state->recv_timeout_ms : 0u);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    if (address != 0 && address_length != 0 && *address_length >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in* out = (struct sockaddr_in*)address;
        memset(out, 0, sizeof(*out));
        out->sin_family = 1;
        out->sin_port = sx_htons(raw.port);
        out->sin_addr.s_addr = sx_htonl(raw.ipv4);
        *address_length = sizeof(*out);
    }
    return result;
}

int sx_setsockopt(int fd, int level, int option_name, const void* option_value, socklen_t option_length) {
    struct sx_socket_state* state = sx_find_socket_state(fd);
    unsigned long value = 0;
    if (level != 1 || state == 0) {
        g_errno = SAVANXP_ENOSYS;
        return -1;
    }
    if (option_value != 0 && option_length >= sizeof(unsigned long)) {
        value = *(const unsigned long*)option_value;
    }
    if (option_name == 20) {
        state->recv_timeout_ms = value;
        return 0;
    }
    if (option_name == 21) {
        state->send_timeout_ms = value;
        return 0;
    }
    if (option_name == 2 || option_name == 6) {
        return 0;
    }
    g_errno = SAVANXP_ENOSYS;
    return -1;
}

int sx_getsockopt(int fd, int level, int option_name, void* option_value, socklen_t* option_length) {
    struct sx_socket_state* state = sx_find_socket_state(fd);
    unsigned long value = 0;
    if (level != 1 || state == 0 || option_value == 0 || option_length == 0 || *option_length < sizeof(unsigned long)) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    if (option_name == 20) {
        value = state->recv_timeout_ms;
    } else if (option_name == 21) {
        value = state->send_timeout_ms;
    } else if (option_name == 2 || option_name == 6) {
        value = 1;
    } else {
        g_errno = SAVANXP_ENOSYS;
        return -1;
    }
    *(unsigned long*)option_value = value;
    *option_length = sizeof(unsigned long);
    return 0;
}

int sx_shutdown(int fd, int how) {
    (void)fd;
    (void)how;
    return 0;
}

void sx_exit(int code) {
    exit(code);
}
