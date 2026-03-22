#include "savanxp/libc.h"

#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <termios.h>

#define EOF (-1)
#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR 0x0002
#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_APPEND 0x0400
#define O_NONBLOCK 0x0800
#define F_DUPFD 0
#define F_GETFL 1
#define F_SETFL 2
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
#define SX_ALLOC_ALIGNMENT ((size_t)sizeof(uintptr_t))
#define SX_ALLOC_MIN_SPLIT_SIZE (SX_ALLOC_ALIGNMENT * 2u)
#define SX_ALLOC_MAGIC 0x53584148u
#define SX_FILE_POOL_CAPACITY 64
#define SX_DIR_POOL_CAPACITY 32
#define SX_SOCKET_STATE_CAPACITY 64
#define SX_PATH_CAPACITY 256
#define SX_FILE_BUFFER_CAPACITY 512

typedef long ssize_t;
typedef long off_t;
typedef int pid_t;
typedef unsigned int mode_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int socklen_t;
typedef long time_t;
typedef long clock_t;
typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

typedef struct sx_FILE FILE;
typedef struct sx_DIR DIR;

struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
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

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020
#define FNM_NOESCAPE 0x01
#define FNM_PATHNAME 0x02

#define SX_FD_SETSIZE 64

typedef struct fd_set {
    unsigned long bits[(SX_FD_SETSIZE + (8 * sizeof(unsigned long)) - 1) / (8 * sizeof(unsigned long))];
} fd_set;

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
    uint32_t magic;
    uint32_t free;
    struct sx_alloc_header* next;
    struct sx_alloc_header* prev;
} sx_alloc_header;

struct sx_socket_state {
    int in_use;
    int fd;
    unsigned long recv_timeout_ms;
    unsigned long send_timeout_ms;
};

static union {
    uintptr_t alignment;
    unsigned char bytes[SX_HEAP_SIZE];
} g_heap = {0};
static sx_alloc_header* g_heap_head = 0;
static struct sx_FILE g_file_pool[SX_FILE_POOL_CAPACITY] = {};
static struct sx_FILE g_stdin_file = {0, 1, 0, 0, 1, 1, 0, 0, {0}};
static struct sx_FILE g_stdout_file = {1, 1, 0, 0, 1, 0, 1, 0, {0}};
static struct sx_FILE g_stderr_file = {2, 1, 0, 0, 1, 0, 1, 0, {0}};
static struct sx_DIR g_dir_pool[SX_DIR_POOL_CAPACITY] = {};
static struct sx_socket_state g_socket_states[SX_SOCKET_STATE_CAPACITY] = {};
static char g_path_env[] = "/disk/bin:/bin";
static char g_path_env_entry[] = "PATH=/disk/bin:/bin";
static char g_term_env_entry[] = "TERM=savanxp";
static char* g_environ_storage[] = { g_path_env_entry, g_term_env_entry, 0 };
static mode_t g_umask_value = 022;
static struct sigaction g_signal_actions[NSIG] = {};
static sigset_t g_signal_mask = 0;
static char g_passwd_root_name[] = "root";
static char g_passwd_root_dir[] = "/";
static struct passwd g_passwd_root = { g_passwd_root_name, g_passwd_root_dir };

FILE* stdin = &g_stdin_file;
FILE* stdout = &g_stdout_file;
FILE* stderr = &g_stderr_file;
char** environ = g_environ_storage;

static int g_errno = 0;

void* sx_malloc(size_t size);
void* sx_calloc(size_t count, size_t size);
void sx_free(void* pointer);
int sx_usleep(unsigned long microseconds);
int sx_vsnprintf(char* buffer, size_t size, const char* format, va_list args);

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

static unsigned long sx_hash_text(const char* text) {
    unsigned long hash = 2166136261u;
    if (text == 0) {
        return 0;
    }
    while (*text != '\0') {
        hash ^= (unsigned char)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static size_t sx_align_size(size_t size) {
    const size_t mask = SX_ALLOC_ALIGNMENT - 1u;
    if (size == 0 || size > ((size_t)-1) - mask) {
        return 0;
    }
    return (size + mask) & ~mask;
}

static void sx_allocator_init(void) {
    if (g_heap_head != 0 || SX_HEAP_SIZE <= sizeof(sx_alloc_header)) {
        return;
    }

    g_heap_head = (sx_alloc_header*)g_heap.bytes;
    g_heap_head->size = SX_HEAP_SIZE - sizeof(sx_alloc_header);
    g_heap_head->magic = SX_ALLOC_MAGIC;
    g_heap_head->free = 1;
    g_heap_head->next = 0;
    g_heap_head->prev = 0;
}

static int sx_block_is_valid(const sx_alloc_header* block) {
    const unsigned char* heap_start = g_heap.bytes;
    const unsigned char* heap_end = g_heap.bytes + SX_HEAP_SIZE;
    const unsigned char* block_start = 0;
    const unsigned char* payload = 0;

    if (block == 0) {
        return 0;
    }

    block_start = (const unsigned char*)block;
    payload = (const unsigned char*)(block + 1);

    return block_start >= heap_start
        && payload <= heap_end
        && block->magic == SX_ALLOC_MAGIC;
}

static int sx_blocks_are_adjacent(const sx_alloc_header* left, const sx_alloc_header* right) {
    return left != 0
        && right != 0
        && ((const unsigned char*)(left + 1) + left->size) == (const unsigned char*)right;
}

static void sx_merge_with_next(sx_alloc_header* block) {
    sx_alloc_header* next = 0;
    if (block == 0 || !block->free) {
        return;
    }

    next = block->next;
    while (next != 0 && next->free && sx_blocks_are_adjacent(block, next)) {
        block->size += sizeof(sx_alloc_header) + next->size;
        block->next = next->next;
        if (block->next != 0) {
            block->next->prev = block;
        }
        next = block->next;
    }
}

static void sx_split_block(sx_alloc_header* block, size_t size) {
    size_t remaining = 0;
    sx_alloc_header* tail = 0;

    if (block == 0 || block->size <= size) {
        return;
    }

    remaining = block->size - size;
    if (remaining < sizeof(sx_alloc_header) + SX_ALLOC_MIN_SPLIT_SIZE) {
        return;
    }

    tail = (sx_alloc_header*)((unsigned char*)(block + 1) + size);
    tail->size = remaining - sizeof(sx_alloc_header);
    tail->magic = SX_ALLOC_MAGIC;
    tail->free = 1;
    tail->next = block->next;
    tail->prev = block;
    if (tail->next != 0) {
        tail->next->prev = tail;
    }

    block->size = size;
    block->next = tail;
    sx_merge_with_next(tail);
}

static sx_alloc_header* sx_find_free_block(size_t size) {
    sx_alloc_header* block = 0;
    sx_allocator_init();

    for (block = g_heap_head; block != 0; block = block->next) {
        if (block->free && block->size >= size) {
            return block;
        }
    }
    return 0;
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

static unsigned long sx_timeout_from_timeval(const struct timeval* timeout) {
    unsigned long milliseconds = 0;
    if (timeout == 0) {
        return (unsigned long)-1;
    }
    if (timeout->tv_sec < 0 || timeout->tv_usec < 0) {
        return 0;
    }
    milliseconds = (unsigned long)timeout->tv_sec * 1000UL;
    milliseconds += (unsigned long)((timeout->tv_usec + 999L) / 1000L);
    return milliseconds;
}

void sx_fd_zero(fd_set* set) {
    if (set != 0) {
        memset(set, 0, sizeof(*set));
    }
}

void sx_fd_set(int fd, fd_set* set) {
    const unsigned long bit = (unsigned long)fd;
    if (set == 0 || fd < 0 || fd >= SX_FD_SETSIZE) {
        return;
    }
    set->bits[bit / (8 * sizeof(unsigned long))] |= 1UL << (bit % (8 * sizeof(unsigned long)));
}

void sx_fd_clr(int fd, fd_set* set) {
    const unsigned long bit = (unsigned long)fd;
    if (set == 0 || fd < 0 || fd >= SX_FD_SETSIZE) {
        return;
    }
    set->bits[bit / (8 * sizeof(unsigned long))] &= ~(1UL << (bit % (8 * sizeof(unsigned long))));
}

int sx_fd_isset(int fd, const fd_set* set) {
    const unsigned long bit = (unsigned long)fd;
    if (set == 0 || fd < 0 || fd >= SX_FD_SETSIZE) {
        return 0;
    }
    return (set->bits[bit / (8 * sizeof(unsigned long))] & (1UL << (bit % (8 * sizeof(unsigned long))))) != 0;
}

void* sx_memcpy(void* destination, const void* source, size_t count) {
    return memcpy(destination, source, count);
}

void* sx_mempcpy(void* destination, const void* source, size_t count) {
    return (unsigned char*)sx_memcpy(destination, source, count) + count;
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

char* sx_strchrnul(const char* text, int character) {
    while (*text != '\0') {
        if (*text == (char)character) {
            return (char*)text;
        }
        ++text;
    }
    return (char*)text;
}

char* sx_strpbrk(const char* text, const char* accept) {
    while (*text != '\0') {
        if (sx_strchr(accept, *text) != 0) {
            return (char*)text;
        }
        ++text;
    }
    return 0;
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

size_t sx_strcspn(const char* text, const char* reject) {
    size_t length = 0;
    while (text[length] != '\0') {
        if (sx_strchr(reject, text[length]) != 0) {
            break;
        }
        ++length;
    }
    return length;
}

size_t sx_strspn(const char* text, const char* accept) {
    size_t length = 0;
    while (text[length] != '\0') {
        if (sx_strchr(accept, text[length]) == 0) {
            break;
        }
        ++length;
    }
    return length;
}

char* sx_strerror(int error_number) {
    switch (error_number) {
        case 0: return "ok";
        case ESRCH: return "no such process";
        case EINTR: return "interrupted system call";
        case ENOENT: return "no such file or directory";
        case EIO: return "i/o error";
        case EBADF: return "bad file descriptor";
        case ECHILD: return "no child processes";
        case EAGAIN: return "resource temporarily unavailable";
        case ENOMEM: return "out of memory";
        case EACCES: return "permission denied";
        case EBUSY: return "device or resource busy";
        case EEXIST: return "file exists";
        case ENODEV: return "no such device";
        case ENOTDIR: return "not a directory";
        case EISDIR: return "is a directory";
        case EINVAL: return "invalid argument";
        case ENOTTY: return "inappropriate ioctl";
        case ENOSPC: return "no space left on device";
        case EPIPE: return "broken pipe";
        case ERANGE: return "result out of range";
        case ENOSYS: return "function not implemented";
        case ENOTEMPTY: return "directory not empty";
        case ETIMEDOUT: return "timed out";
        default: return "error";
    }
}

char* sx_strtok_r(char* text, const char* delimiters, char** save_ptr) {
    char* current = text != 0 ? text : (save_ptr != 0 ? *save_ptr : 0);
    char* token_start = 0;
    if (current == 0 || delimiters == 0 || save_ptr == 0) {
        return 0;
    }
    while (*current != '\0' && sx_strchr(delimiters, *current) != 0) {
        ++current;
    }
    if (*current == '\0') {
        *save_ptr = current;
        return 0;
    }
    token_start = current;
    while (*current != '\0' && sx_strchr(delimiters, *current) == 0) {
        ++current;
    }
    if (*current != '\0') {
        *current++ = '\0';
    }
    *save_ptr = current;
    return token_start;
}

char* sx_stpncpy(char* destination, const char* source, size_t count) {
    size_t index = 0;
    while (index < count && source[index] != '\0') {
        destination[index] = source[index];
        ++index;
    }
    while (index < count) {
        destination[index++] = '\0';
    }
    while (index != 0 && destination[index - 1] == '\0' && source[index - 1] == '\0') {
        --index;
    }
    return destination + index;
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

int sx_isalpha(int character) {
    return (character >= 'a' && character <= 'z')
        || (character >= 'A' && character <= 'Z');
}

int sx_isalnum(int character) {
    return sx_isalpha(character) || sx_isdigit(character);
}

int sx_islower(int character) {
    return character >= 'a' && character <= 'z';
}

int sx_isupper(int character) {
    return character >= 'A' && character <= 'Z';
}

int sx_isxdigit(int character) {
    return sx_isdigit(character)
        || (character >= 'a' && character <= 'f')
        || (character >= 'A' && character <= 'F');
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
    const size_t aligned = sx_align_size(size);

    if (aligned == 0) {
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }

    header = sx_find_free_block(aligned);
    if (header == 0) {
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }

    sx_split_block(header, aligned);
    header->free = 0;
    return (void*)(header + 1);
}

void* sx_calloc(size_t count, size_t size) {
    const size_t max_size = (size_t)-1;
    const size_t total = count * size;

    if (count != 0 && size > max_size / count) {
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }

    void* pointer = sx_malloc(total);
    if (pointer != 0) {
        sx_memset(pointer, 0, total);
    }
    return pointer;
}

void* sx_realloc(void* pointer, size_t size) {
    sx_alloc_header* header = 0;
    void* replacement = 0;
    size_t aligned = 0;

    if (pointer == 0) {
        return sx_malloc(size);
    }
    if (size == 0) {
        sx_free(pointer);
        return 0;
    }

    header = ((sx_alloc_header*)pointer) - 1;
    if (!sx_block_is_valid(header) || header->free) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }

    aligned = sx_align_size(size);
    if (aligned == 0) {
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }

    if (aligned <= header->size) {
        sx_split_block(header, aligned);
        return pointer;
    }

    while (header->next != 0
        && header->next->free
        && sx_blocks_are_adjacent(header, header->next)
        && header->size < aligned) {
        sx_merge_with_next(header);
    }

    if (header->size >= aligned) {
        sx_split_block(header, aligned);
        return pointer;
    }

    replacement = sx_malloc(size);
    if (replacement == 0) {
        return 0;
    }
    sx_memcpy(replacement, pointer, sx_min_size(header->size, size));
    sx_free(pointer);
    return replacement;
}

void sx_free(void* pointer) {
    sx_alloc_header* header = 0;

    if (pointer == 0) {
        return;
    }

    header = ((sx_alloc_header*)pointer) - 1;
    if (!sx_block_is_valid(header) || header->free) {
        g_errno = SAVANXP_EINVAL;
        return;
    }

    header->free = 1;
    sx_merge_with_next(header);
    if (header->prev != 0 && header->prev->free) {
        sx_merge_with_next(header->prev);
    }
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
    if (name == 0) {
        return 0;
    }
    if (strcmp(name, "PATH") == 0) {
        return g_path_env;
    }
    if (strcmp(name, "TERM") == 0) {
        return (char*)"savanxp";
    }
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
    if ((flags & O_NONBLOCK) != 0) {
        raw_flags |= SAVANXP_OPEN_NONBLOCK;
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

int sx_fcntl(int fd, int command, ...) {
    unsigned long raw_value = 0;
    long result = 0;
    if (command == F_DUPFD || command == F_SETFL) {
        va_list args;
        va_start(args, command);
        raw_value = (unsigned long)va_arg(args, int);
        va_end(args);
    }

    if (command == F_DUPFD) {
        int minimum_fd = (int)raw_value;
        int scratch[64] = {};
        size_t scratch_count = 0;

        if (minimum_fd < 0) {
            g_errno = SAVANXP_EINVAL;
            return -1;
        }

        for (;;) {
            int duplicated = sx_dup(fd);
            if (duplicated < 0) {
                for (size_t index = 0; index < scratch_count; ++index) {
                    sx_close(scratch[index]);
                }
                return -1;
            }
            if (duplicated >= minimum_fd) {
                for (size_t index = 0; index < scratch_count; ++index) {
                    sx_close(scratch[index]);
                }
                return duplicated;
            }
            if (scratch_count >= (sizeof(scratch) / sizeof(scratch[0]))) {
                sx_close(duplicated);
                for (size_t index = 0; index < scratch_count; ++index) {
                    sx_close(scratch[index]);
                }
                g_errno = SAVANXP_EINVAL;
                return -1;
            }
            scratch[scratch_count++] = duplicated;
        }
    }

    if (command == F_SETFL) {
        {
            unsigned long translated = 0;
            if ((raw_value & O_NONBLOCK) != 0) {
                translated |= SAVANXP_OPEN_NONBLOCK;
            }
            raw_value = translated;
        }
    }

    result = fcntl(fd, (unsigned long)command, raw_value);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }

    if (command == F_GETFL) {
        int flags = 0;
        const unsigned long raw = (unsigned long)result;
        if ((raw & SAVANXP_OPEN_WRITE) != 0 && (raw & SAVANXP_OPEN_READ) != 0) {
            flags |= O_RDWR;
        } else if ((raw & SAVANXP_OPEN_WRITE) != 0) {
            flags |= O_WRONLY;
        } else {
            flags |= O_RDONLY;
        }
        if ((raw & SAVANXP_OPEN_NONBLOCK) != 0) {
            flags |= O_NONBLOCK;
        }
        return flags;
    }

    return (int)result;
}

int sx_poll(struct pollfd* fds, unsigned long count, int timeout_ms) {
    struct savanxp_pollfd* raw = 0;
    long result = 0;

    if (count != 0 && fds == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }

    raw = (struct savanxp_pollfd*)sx_calloc(count != 0 ? (size_t)count : 1u, sizeof(*raw));
    if (raw == 0) {
        g_errno = SAVANXP_ENOMEM;
        return -1;
    }

    for (unsigned long index = 0; index < count; ++index) {
        raw[index].fd = fds[index].fd;
        raw[index].events = fds[index].events;
        raw[index].revents = 0;
    }

    result = poll(raw, count, timeout_ms);
    if (result < 0) {
        sx_free(raw);
        sx_set_errno_from_result(result);
        return -1;
    }

    for (unsigned long index = 0; index < count; ++index) {
        fds[index].revents = raw[index].revents;
    }

    sx_free(raw);
    return (int)result;
}

int sx_select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout) {
    struct pollfd pollfds[SX_FD_SETSIZE];
    int fd_map[SX_FD_SETSIZE];
    int poll_count = 0;
    int ready_count = 0;
    int result = 0;

    if (nfds < 0 || nfds > SX_FD_SETSIZE) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }

    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (readfds != 0 && sx_fd_isset(fd, readfds)) {
            events = (short)(events | POLLIN);
        }
        if (writefds != 0 && sx_fd_isset(fd, writefds)) {
            events = (short)(events | POLLOUT);
        }
        if (exceptfds != 0 && sx_fd_isset(fd, exceptfds)) {
            events = (short)(events | POLLERR | POLLHUP);
        }
        if (events == 0) {
            continue;
        }

        pollfds[poll_count].fd = fd;
        pollfds[poll_count].events = events;
        pollfds[poll_count].revents = 0;
        fd_map[poll_count] = fd;
        poll_count += 1;
    }

    if (readfds != 0) {
        sx_fd_zero(readfds);
    }
    if (writefds != 0) {
        sx_fd_zero(writefds);
    }
    if (exceptfds != 0) {
        sx_fd_zero(exceptfds);
    }

    result = sx_poll(pollfds, (unsigned long)poll_count, timeout != 0 ? (int)sx_timeout_from_timeval(timeout) : -1);
    if (result < 0) {
        return -1;
    }

    for (int index = 0; index < poll_count; ++index) {
        const short revents = pollfds[index].revents;
        const int fd = fd_map[index];
        int marked = 0;
        if (readfds != 0 && (revents & (POLLIN | POLLHUP)) != 0) {
            sx_fd_set(fd, readfds);
            marked = 1;
        }
        if (writefds != 0 && (revents & POLLOUT) != 0) {
            sx_fd_set(fd, writefds);
            marked = 1;
        }
        if (exceptfds != 0 && (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            sx_fd_set(fd, exceptfds);
            marked = 1;
        }
        if (marked) {
            ready_count += 1;
        }
    }

    return ready_count;
}

int sx_stat(const char* path, struct stat* info) {
    struct savanxp_stat raw = {};
    long result = savanxp_stat(path, &raw);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    if (info != 0) {
        info->st_dev = 1;
        info->st_ino = sx_hash_text(path);
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
        info->st_dev = 1;
        info->st_ino = (unsigned long)(fd + 1);
        info->st_mode = raw.st_mode;
        info->st_size = raw.st_size;
    }
    return 0;
}

int sx_lstat(const char* path, struct stat* info) {
    return sx_stat(path, info);
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

pid_t sx_getppid(void) {
    return 1;
}

pid_t sx_getpgrp(void) {
    return sx_getpid();
}

int sx_setpgid(pid_t pid, pid_t pgrp) {
    pid_t self = sx_getpid();
    if ((pid != 0 && pid != self) || (pgrp != 0 && pgrp != self)) {
        g_errno = SAVANXP_ENOSYS;
        return -1;
    }
    return 0;
}

pid_t sx_setsid(void) {
    return sx_getpid();
}

int sx_sync(void) {
    long result = sync();
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_tcgetattr(int fd, struct termios* value) {
    (void)fd;
    if (value == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    memset(value, 0, sizeof(*value));
    value->c_lflag = ICANON | ECHO | ECHOK | ECHONL;
    value->c_cc[VMIN] = 1;
    value->c_cc[VTIME] = 0;
    return 0;
}

int sx_tcsetattr(int fd, int optional_actions, const struct termios* value) {
    (void)fd;
    (void)optional_actions;
    (void)value;
    return 0;
}

pid_t sx_tcgetpgrp(int fd) {
    (void)fd;
    return sx_getpid();
}

int sx_tcsetpgrp(int fd, pid_t pgrp) {
    (void)fd;
    (void)pgrp;
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

int sx_sprintf(char* buffer, const char* format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vsnprintf(buffer, (size_t)-1, format, args);
    va_end(args);
    return written;
}

int sx_vprintf(const char* format, va_list args) {
    return sx_vfprintf(stdout, format, args);
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

int sx_ferror(FILE* stream) {
    return stream != 0 ? stream->error : 1;
}

void sx_clearerr(FILE* stream) {
    if (stream == 0) {
        return;
    }
    stream->eof = 0;
    stream->error = 0;
}

int sx_fputs(const char* text, FILE* stream) {
    size_t length = 0;
    if (text == 0 || stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return EOF;
    }
    length = sx_strlen(text);
    return sx_fwrite(text, 1, length, stream) == length ? 0 : EOF;
}

int sx_putc(int character, FILE* stream) {
    unsigned char value = (unsigned char)character;
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return EOF;
    }
    return sx_fwrite(&value, 1, 1, stream) == 1 ? character : EOF;
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
    if ((options & ~(WNOHANG | WUNTRACED | WCONTINUED)) != 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    if ((options & WNOHANG) != 0) {
        g_errno = SAVANXP_ENOSYS;
        return 0;
    }
    result = waitpid(pid, status);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (pid_t)result;
}

pid_t sx_fork(void) {
    long result = fork();
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (pid_t)result;
}

pid_t sx_vfork(void) {
    return sx_fork();
}

void* sx_mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset) {
    unsigned long section_flags = 0;
    unsigned long view_flags = 0;
    long section = 0;
    void* mapped = MAP_FAILED;

    if (address != 0 || length == 0 || fd != -1 || offset != 0) {
        g_errno = SAVANXP_EINVAL;
        return MAP_FAILED;
    }
    if ((flags & MAP_ANONYMOUS) == 0 || ((flags & MAP_SHARED) == 0) == ((flags & MAP_PRIVATE) == 0)) {
        g_errno = SAVANXP_EINVAL;
        return MAP_FAILED;
    }
    if ((prot & ~(PROT_READ | PROT_WRITE)) != 0 || (prot & (PROT_READ | PROT_WRITE)) == 0) {
        g_errno = SAVANXP_ENOSYS;
        return MAP_FAILED;
    }

    if ((prot & PROT_READ) != 0) {
        section_flags |= SAVANXP_SECTION_READ;
    }
    if ((prot & PROT_WRITE) != 0) {
        section_flags |= SAVANXP_SECTION_WRITE;
    }

    section = section_create(length, section_flags);
    if (section < 0) {
        sx_set_errno_from_result(section);
        return MAP_FAILED;
    }

    view_flags = section_flags;
    if ((flags & MAP_PRIVATE) != 0) {
        view_flags |= SAVANXP_VIEW_PRIVATE;
    }

    mapped = map_view((int)section, view_flags);
    (void)close((int)section);
    if (result_is_error((long)mapped)) {
        sx_set_errno_from_result((long)mapped);
        return MAP_FAILED;
    }

    return mapped;
}

int sx_munmap(void* address, size_t length) {
    long result = 0;
    if (address == 0 || address == MAP_FAILED || length == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }

    result = unmap_view(address);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

static int sx_try_exec_path(const char* path, char* const argv[]) {
    int argc = 0;
    const char* const* raw_argv = (const char* const*)argv;
    while (argv != 0 && argv[argc] != 0) {
        ++argc;
    }

    long result = exec(path, raw_argv, argc);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

static int sx_try_exec_search(const char* prefix, const char* file, char* const argv[]) {
    char path[SX_PATH_CAPACITY] = {};
    size_t written = 0;
    if (prefix == 0 || file == 0) {
        return -1;
    }

    while (prefix[written] != '\0' && written + 1 < sizeof(path)) {
        path[written] = prefix[written];
        ++written;
    }
    if (written + 1 >= sizeof(path)) {
        g_errno = SAVANXP_ENOENT;
        return -1;
    }
    if (written == 0 || path[written - 1] != '/') {
        path[written++] = '/';
    }
    for (size_t index = 0; file[index] != '\0' && written + 1 < sizeof(path); ++index) {
        path[written++] = file[index];
    }
    path[written] = '\0';
    return sx_try_exec_path(path, argv);
}

int sx_execvp(const char* file, char* const argv[]) {
    int has_separator = 0;
    if (file == 0 || file[0] == '\0') {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    for (const char* cursor = file; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            has_separator = 1;
            break;
        }
    }
    if (has_separator) {
        return sx_try_exec_path(file, argv);
    }
    if (sx_try_exec_search("/disk/bin", file, argv) == 0) {
        return 0;
    }
    if (g_errno != SAVANXP_ENOENT) {
        return -1;
    }
    return sx_try_exec_search("/bin", file, argv);
}

int sx_execv(const char* path, char* const argv[]) {
    return sx_try_exec_path(path, argv);
}

int sx_execve(const char* path, char* const argv[], char* const envp[]) {
    (void)envp;
    return sx_try_exec_path(path, argv);
}

void sx__exit(int code) {
    exit(code);
}

uid_t sx_getuid(void) {
    return 0;
}

uid_t sx_geteuid(void) {
    return 0;
}

gid_t sx_getgid(void) {
    return 0;
}

gid_t sx_getegid(void) {
    return 0;
}

mode_t sx_umask(mode_t mask) {
    mode_t previous = g_umask_value;
    g_umask_value = mask;
    return previous;
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

int sx_gettimeofday(struct timeval* value, void* timezone_ptr) {
    unsigned long milliseconds = uptime_ms();
    (void)timezone_ptr;
    if (value == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    value->tv_sec = (long)(milliseconds / 1000UL);
    value->tv_usec = (long)((milliseconds % 1000UL) * 1000UL);
    return 0;
}

clock_t sx_times(struct tms* buffer) {
    const unsigned long ticks = uptime_ms();
    if (buffer != 0) {
        buffer->tms_utime = (clock_t)ticks;
        buffer->tms_stime = 0;
        buffer->tms_cutime = 0;
        buffer->tms_cstime = 0;
    }
    return (clock_t)ticks;
}

static void sx_copy_uname_field(char* destination, size_t capacity, const char* source) {
    size_t index = 0;
    if (destination == 0 || capacity == 0) {
        return;
    }
    while (source != 0 && source[index] != '\0' && index + 1 < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

int sx_uname(struct utsname* value) {
    if (value == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }

    sx_copy_uname_field(value->sysname, sizeof(value->sysname), "SavanXP");
    sx_copy_uname_field(value->nodename, sizeof(value->nodename), "savanxp");
    sx_copy_uname_field(value->release, sizeof(value->release), "0.1");
    sx_copy_uname_field(value->version, sizeof(value->version), "sdk-v1");
    sx_copy_uname_field(value->machine, sizeof(value->machine), "x86_64");
    return 0;
}

struct passwd* sx_getpwnam(const char* name) {
    if (name == 0) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }
    if (sx_strcmp(name, "root") == 0) {
        return &g_passwd_root;
    }
    return 0;
}

static int sx_valid_signal_number(int signal_number) {
    return signal_number > 0 && signal_number < NSIG;
}

static int sx_fnmatch_class_matches(const char** pattern_ptr, char character) {
    const char* pattern = *pattern_ptr;
    int negate = 0;
    int matched = 0;

    if (*pattern == '!' || *pattern == '^') {
        negate = 1;
        ++pattern;
    }

    while (*pattern != '\0' && *pattern != ']') {
        if (pattern[1] == '-' && pattern[2] != '\0' && pattern[2] != ']') {
            const char start = pattern[0];
            const char end = pattern[2];
            if (character >= start && character <= end) {
                matched = 1;
            }
            pattern += 3;
            continue;
        }
        if (*pattern == character) {
            matched = 1;
        }
        ++pattern;
    }

    if (*pattern == ']') {
        ++pattern;
    }
    *pattern_ptr = pattern;
    return negate ? !matched : matched;
}

static int sx_fnmatch_internal(const char* pattern, const char* string, int flags) {
    while (*pattern != '\0') {
        if (*pattern == '*') {
            while (*pattern == '*') {
                ++pattern;
            }
            if (*pattern == '\0') {
                if ((flags & FNM_PATHNAME) != 0) {
                    for (; *string != '\0'; ++string) {
                        if (*string == '/') {
                            return 1;
                        }
                    }
                }
                return 0;
            }
            while (*string != '\0') {
                if ((flags & FNM_PATHNAME) != 0 && *string == '/') {
                    break;
                }
                if (sx_fnmatch_internal(pattern, string, flags) == 0) {
                    return 0;
                }
                ++string;
            }
            return sx_fnmatch_internal(pattern, string, flags);
        }

        if (*string == '\0') {
            return 1;
        }

        if (*pattern == '?') {
            if ((flags & FNM_PATHNAME) != 0 && *string == '/') {
                return 1;
            }
            ++pattern;
            ++string;
            continue;
        }

        if (*pattern == '[') {
            ++pattern;
            if ((flags & FNM_PATHNAME) != 0 && *string == '/') {
                return 1;
            }
            if (!sx_fnmatch_class_matches(&pattern, *string)) {
                return 1;
            }
            ++string;
            continue;
        }

        if (*pattern == '\\' && (flags & FNM_NOESCAPE) == 0 && pattern[1] != '\0') {
            ++pattern;
        }
        if (*pattern != *string) {
            return 1;
        }
        ++pattern;
        ++string;
    }

    return *string == '\0' ? 0 : 1;
}

int sx_fnmatch(const char* pattern, const char* string, int flags) {
    if (pattern == 0 || string == 0) {
        g_errno = SAVANXP_EINVAL;
        return 1;
    }
    return sx_fnmatch_internal(pattern, string, flags);
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

int sx_kill(pid_t pid, int signal_number) {
    long result = (kill)((int)pid, signal_number);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_raise(int signal_number) {
    return sx_kill(sx_getpid(), signal_number);
}

sighandler_t sx_signal(int signal_number, sighandler_t handler) {
    sighandler_t previous = SIG_DFL;
    if (!sx_valid_signal_number(signal_number)) {
        g_errno = SAVANXP_EINVAL;
        return SIG_ERR;
    }
    previous = g_signal_actions[signal_number].sa_handler;
    g_signal_actions[signal_number].sa_handler = handler;
    g_signal_actions[signal_number].sa_mask = 0;
    g_signal_actions[signal_number].sa_flags = 0;
    return previous != 0 ? previous : SIG_DFL;
}

int sx_sigaction(int signal_number, const struct sigaction* action, struct sigaction* old_action) {
    if (!sx_valid_signal_number(signal_number)) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    if (old_action != 0) {
        *old_action = g_signal_actions[signal_number];
        if (old_action->sa_handler == 0) {
            old_action->sa_handler = SIG_DFL;
        }
    }
    if (action != 0) {
        g_signal_actions[signal_number] = *action;
    }
    return 0;
}

int sx_sigemptyset(sigset_t* set) {
    if (set == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    *set = 0;
    return 0;
}

int sx_sigfillset(sigset_t* set) {
    if (set == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    *set = ~0ul;
    return 0;
}

int sx_sigaddset(sigset_t* set, int signal_number) {
    if (set == 0 || !sx_valid_signal_number(signal_number)) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    *set |= (1ul << signal_number);
    return 0;
}

int sx_sigdelset(sigset_t* set, int signal_number) {
    if (set == 0 || !sx_valid_signal_number(signal_number)) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    *set &= ~(1ul << signal_number);
    return 0;
}

int sx_sigismember(const sigset_t* set, int signal_number) {
    if (set == 0 || !sx_valid_signal_number(signal_number)) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    return ((*set & (1ul << signal_number)) != 0) ? 1 : 0;
}

int sx_sigprocmask(int how, const sigset_t* set, sigset_t* old_set) {
    if (old_set != 0) {
        *old_set = g_signal_mask;
    }
    if (set == 0) {
        return 0;
    }
    switch (how) {
        case SIG_BLOCK:
            g_signal_mask |= *set;
            return 0;
        case SIG_UNBLOCK:
            g_signal_mask &= ~(*set);
            return 0;
        case SIG_SETMASK:
            g_signal_mask = *set;
            return 0;
        default:
            g_errno = SAVANXP_EINVAL;
            return -1;
    }
}

int sx_sigsuspend(const sigset_t* mask) {
    (void)mask;
    g_errno = EINTR;
    return -1;
}

char* sx_strsignal(int signal_number) {
    switch (signal_number) {
        case SIGHUP: return "Hangup";
        case SIGINT: return "Interrupt";
        case SIGQUIT: return "Quit";
        case SIGKILL: return "Killed";
        case SIGPIPE: return "Broken pipe";
        case SIGALRM: return "Alarm";
        case SIGTERM: return "Terminated";
        case SIGCHLD: return "Child exited";
        case SIGTSTP: return "Stopped";
        case SIGTTIN: return "TTY input";
        case SIGTTOU: return "TTY output";
        default: return "Signal";
    }
}

void* sx_bsearch(const void* key, const void* base, size_t count, size_t size,
    int (*compar)(const void*, const void*)) {
    size_t left = 0;
    size_t right = count;
    const unsigned char* bytes = (const unsigned char*)base;
    if (key == 0 || base == 0 || compar == 0 || size == 0) {
        return 0;
    }
    while (left < right) {
        const size_t mid = left + ((right - left) / 2);
        const void* element = bytes + (mid * size);
        const int result = compar(key, element);
        if (result == 0) {
            return (void*)element;
        }
        if (result < 0) {
            right = mid;
        } else {
            left = mid + 1;
        }
    }
    return 0;
}

void sx_qsort(void* base, size_t count, size_t size, int (*compar)(const void*, const void*)) {
    unsigned char* bytes = (unsigned char*)base;
    unsigned char* tmp = 0;
    if (base == 0 || compar == 0 || size == 0 || count < 2) {
        return;
    }
    tmp = (unsigned char*)sx_malloc(size);
    if (tmp == 0) {
        return;
    }
    for (size_t outer = 0; outer + 1 < count; ++outer) {
        for (size_t inner = outer + 1; inner < count; ++inner) {
            unsigned char* left = bytes + (outer * size);
            unsigned char* right = bytes + (inner * size);
            if (compar(left, right) > 0) {
                sx_memcpy(tmp, left, size);
                sx_memcpy(left, right, size);
                sx_memcpy(right, tmp, size);
            }
        }
    }
    sx_free(tmp);
}

void sx_exit(int code) {
    exit(code);
}
