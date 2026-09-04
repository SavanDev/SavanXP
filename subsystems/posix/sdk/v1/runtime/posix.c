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
/* Arena de bootstrap del malloc. Vive en la BSS y el kernel mapea la BSS entera
 * al exec, asi que cada byte de aca es RAM residente por proceso aunque la app
 * no lo toque: se queda chica a proposito. El resto del heap crece con arenas
 * respaldadas por secciones (section_create + map_view), que solo cuestan RAM
 * cuando hacen falta y se devuelven al kernel cuando quedan vacias.
 * -DSX_HEAP_SIZE sigue siendo el knob para pedir mas arena adelantada. */
#ifndef SX_HEAP_SIZE
#define SX_HEAP_SIZE (256u * 1024u)
#endif
/* Cada arena dinamica consume un section view del address space (el kernel da
 * 32 por proceso, compartidos con las superficies de GPU/WM) y un section
 * object del pool global (64 en todo el sistema), asi que crecen
 * geometricamente para llegar lejos con pocas. */
#define SX_ARENA_PAGE_SIZE ((size_t)4096u)
#define SX_ARENA_FIRST_BYTES ((size_t)(1024u * 1024u))
#define SX_ARENA_MAX_BYTES ((size_t)(32u * 1024u * 1024u))
#define SX_ARENA_CAPACITY ((size_t)8u)
/* 16 y no sizeof(void*): es el max_align_t de x86-64. Con 8, un movaps
 * sobre un buffer de malloc en una app -Sse podia faultear, porque clang
 * asume que malloc devuelve memoria apta para cualquier tipo. */
#define SX_ALLOC_ALIGNMENT ((size_t)16)
#define SX_ALLOC_MIN_SPLIT_SIZE (SX_ALLOC_ALIGNMENT * 2u)
#define SX_ALLOC_MAGIC 0x53584148u
/* Marca de una asignacion alineada. Es un valor de 64 bits que no puede ser
 * un puntero valido a proposito: sx_free lee estos 16 bytes de CUALQUIER
 * bloque, y en uno normal caen sobre los campos next/prev de su header. */
#define SX_ALIGNED_MAGIC 0x5341584d454d414cull
/* El kernel da 64 fds por proceso (process::kMaxFileHandles), asi que un pool
 * mas grande que eso es BSS que no se puede usar ni queriendo. */
#define SX_FILE_POOL_CAPACITY 64
#define SX_DIR_POOL_CAPACITY 64
#define SX_SOCKET_STATE_CAPACITY 128
#define SX_PATH_CAPACITY 256
#define SX_FILE_BUFFER_CAPACITY 512
#define SX_IOFBF 0
#define SX_IOLBF 1
#define SX_IONBF 2

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

/* Espejo de <sys/stat.h>. El kernel solo reporta tipo y tamano; el resto queda
 * en cero porque SxFS no guarda duenio ni marcas de tiempo. */
struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_size;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_nlink;
    unsigned long st_blksize;
    unsigned long st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
};

struct dirent {
    unsigned char d_type;
    char d_name[256];
};

/* Duplicados de <stdlib.h> a proposito: posix.c declara sus tipos localmente en
 * vez de incluir los headers estandar, porque ya define struct stat, struct
 * dirent y FILE por su cuenta y los includes chocarian. */
struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } imaxdiv_t;
typedef struct { long quot; long rem; } ldiv_t;
typedef struct { long long quot; long long rem; } lldiv_t;

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
    /* Lectura: sin buffer, sx_fgets bajaba un read() por caracter. Se llena solo
     * en streams de archivo; stdin sigue yendo directo, igual que la escritura,
     * para no robarle al proceso input que todavia no pidio. */
    size_t read_buffer_used;
    size_t read_buffer_pos;
    int pushback;
    unsigned char write_buffer[SX_FILE_BUFFER_CAPACITY];
    unsigned char read_buffer[SX_FILE_BUFFER_CAPACITY];
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

/* Prefijo de una asignacion alineada: se escribe en los 16 bytes justo antes
 * del puntero que se devuelve, y guarda el puntero real que hay que liberar. */
typedef struct sx_aligned_tag {
    uint64_t magic;
    void* base;
} sx_aligned_tag;

/* El descriptor vive adentro de la propia arena (byte 0), asi no hace falta
 * asignar memoria para poder asignar memoria. Layout:
 *   [sx_arena][sx_alloc_header][payload ...]
 * La lista de bloques es por arena: bloques de arenas distintas nunca son
 * adyacentes, asi que el split/merge no puede cruzar mapeos. */
typedef struct sx_arena {
    struct sx_arena* next;
    sx_alloc_header* first_block;
    size_t size;
    int mapped;
} sx_arena;

struct sx_socket_state {
    int in_use;
    int fd;
    unsigned long recv_timeout_ms;
    unsigned long send_timeout_ms;
};

static union {
    _Alignas(SX_ALLOC_ALIGNMENT) uintptr_t alignment;
    unsigned char bytes[SX_HEAP_SIZE];
} g_heap = {0};
static sx_arena* g_arenas = 0;
static size_t g_mapped_arena_count = 0;
static size_t g_mapped_bytes = 0;
static struct sx_FILE g_file_pool[SX_FILE_POOL_CAPACITY] = {};
static struct sx_FILE g_stdin_file = {
    .fd = 0, .in_use = 1, .is_stdio = 1, .can_read = 1, .pushback = -1};
static struct sx_FILE g_stdout_file = {
    .fd = 1, .in_use = 1, .is_stdio = 1, .can_write = 1, .pushback = -1};
static struct sx_FILE g_stderr_file = {
    .fd = 2, .in_use = 1, .is_stdio = 1, .can_write = 1, .pushback = -1};
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

static size_t sx_arena_header_bytes(void) {
    const size_t mask = SX_ALLOC_ALIGNMENT - 1u;
    return (sizeof(sx_arena) + mask) & ~mask;
}

static unsigned char* sx_arena_payload(sx_arena* arena) {
    return (unsigned char*)arena + sx_arena_header_bytes();
}

static size_t sx_arena_overhead(void) {
    return sx_arena_header_bytes() + sizeof(sx_alloc_header);
}

static size_t sx_arena_initial_block_size(const sx_arena* arena) {
    return arena->size - sx_arena_overhead();
}

/* Convierte un buffer crudo (BSS o vista de seccion) en una arena con un unico
 * bloque libre, y la publica al frente de la lista. */
static sx_arena* sx_arena_stamp(void* base, size_t size, int mapped) {
    sx_arena* arena = 0;
    sx_alloc_header* block = 0;

    if (base == 0 || size <= sx_arena_overhead() + SX_ALLOC_MIN_SPLIT_SIZE) {
        return 0;
    }

    arena = (sx_arena*)base;
    arena->size = size;
    arena->mapped = mapped;

    block = (sx_alloc_header*)sx_arena_payload(arena);
    block->size = sx_arena_initial_block_size(arena);
    block->magic = SX_ALLOC_MAGIC;
    block->free = 1;
    block->next = 0;
    block->prev = 0;

    arena->first_block = block;
    arena->next = g_arenas;
    g_arenas = arena;
    return arena;
}

static void sx_allocator_init(void) {
    if (g_arenas != 0) {
        return;
    }
    (void)sx_arena_stamp(g_heap.bytes, SX_HEAP_SIZE, 0);
}

/* Piso de tamano para la proxima arena: duplicar lo que ya esta mapeado, para
 * que un heap grande se arme con pocas secciones. Se mide sobre lo mapeado hoy
 * (no un contador monotono) asi el piso baja cuando las arenas se devuelven y
 * un ciclo alocar/liberar no termina pidiendo el maximo cada vuelta. */
static size_t sx_arena_growth_floor(void) {
    size_t floor_bytes = g_mapped_bytes;

    if (floor_bytes < SX_ARENA_FIRST_BYTES) {
        floor_bytes = SX_ARENA_FIRST_BYTES;
    }
    if (floor_bytes > SX_ARENA_MAX_BYTES) {
        floor_bytes = SX_ARENA_MAX_BYTES;
    }
    return floor_bytes;
}

/* Pide una arena nueva al kernel. La vista es PRIVATE: en fork el kernel clona
 * la seccion con su contenido, que es la misma semantica que tenia la arena de
 * BSS (cada proceso se lleva su copia). */
static sx_arena* sx_arena_acquire(size_t payload_bytes) {
    const size_t mask = SX_ARENA_PAGE_SIZE - 1u;
    size_t wanted = 0;
    long section = 0;
    void* mapped = 0;
    sx_arena* arena = 0;

    if (g_mapped_arena_count >= SX_ARENA_CAPACITY) {
        return 0;
    }
    if (payload_bytes > ((size_t)-1) - sx_arena_overhead() - mask) {
        return 0;
    }

    wanted = sx_arena_overhead() + payload_bytes;
    if (wanted < sx_arena_growth_floor()) {
        wanted = sx_arena_growth_floor();
    }
    wanted = (wanted + mask) & ~mask;

    section = section_create((unsigned long)wanted, SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE);
    if (section < 0) {
        return 0;
    }

    mapped = map_view((int)section,
        SAVANXP_SECTION_READ | SAVANXP_SECTION_WRITE | SAVANXP_VIEW_PRIVATE);
    (void)savanxp_close((int)section);
    if (mapped == 0 || result_is_error((long)mapped)) {
        return 0;
    }

    arena = sx_arena_stamp(mapped, wanted, 1);
    if (arena == 0) {
        (void)unmap_view(mapped);
        return 0;
    }

    ++g_mapped_arena_count;
    g_mapped_bytes += wanted;
    return arena;
}

static sx_arena* sx_arena_for_address(const void* address) {
    const unsigned char* target = (const unsigned char*)address;
    sx_arena* arena = 0;

    for (arena = g_arenas; arena != 0; arena = arena->next) {
        const unsigned char* start = sx_arena_payload(arena);
        const unsigned char* end = (const unsigned char*)arena + arena->size;
        if (target >= start && target < end) {
            return arena;
        }
    }
    return 0;
}

static int sx_arena_is_empty(const sx_arena* arena) {
    const sx_alloc_header* block = arena != 0 ? arena->first_block : 0;
    return block != 0
        && block->free != 0
        && block->prev == 0
        && block->next == 0
        && (const unsigned char*)block == (const unsigned char*)arena + sx_arena_header_bytes()
        && block->size == sx_arena_initial_block_size(arena);
}

/* Devuelve al kernel una arena que quedo entera libre. La de BSS no se toca. */
static void sx_arena_release(sx_arena* arena) {
    sx_arena** link = &g_arenas;

    if (arena == 0 || arena->mapped == 0 || !sx_arena_is_empty(arena)) {
        return;
    }

    while (*link != 0 && *link != arena) {
        link = &(*link)->next;
    }
    if (*link != arena) {
        return;
    }

    *link = arena->next;
    --g_mapped_arena_count;
    g_mapped_bytes -= arena->size;
    (void)unmap_view(arena);
}

static int sx_block_is_valid(const sx_alloc_header* block) {
    if (block == 0 || sx_arena_for_address(block) == 0) {
        return 0;
    }
    return block->magic == SX_ALLOC_MAGIC;
}

static int sx_blocks_are_adjacent(const sx_alloc_header* left, const sx_alloc_header* right) {
    return left != 0
        && right != 0
        && ((const unsigned char*)(left + 1) + left->size) == (const unsigned char*)right;
}

/* Se come el bloque libre que sigue si es adyacente. Sirve tanto sobre bloques
 * libres (coalescing) como sobre uno OCUPADO, que es como realloc crece sin
 * mover los datos. Devuelve 1 si absorbio algo: quien itera necesita ese aviso
 * para poder terminar. */
static int sx_absorb_next(sx_alloc_header* block) {
    sx_alloc_header* next = block != 0 ? block->next : 0;

    if (next == 0 || !next->free || !sx_blocks_are_adjacent(block, next)) {
        return 0;
    }

    block->size += sizeof(sx_alloc_header) + next->size;
    block->next = next->next;
    if (block->next != 0) {
        block->next->prev = block;
    }
    return 1;
}

static void sx_merge_with_next(sx_alloc_header* block) {
    if (block == 0 || !block->free) {
        return;
    }

    while (sx_absorb_next(block)) {
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
    sx_arena* arena = 0;
    sx_alloc_header* block = 0;

    sx_allocator_init();

    for (arena = g_arenas; arena != 0; arena = arena->next) {
        for (block = arena->first_block; block != 0; block = block->next) {
            if (block->free && block->size >= size) {
                return block;
            }
        }
    }

    arena = sx_arena_acquire(size);
    if (arena == 0) {
        return 0;
    }

    block = arena->first_block;
    return (block != 0 && block->free && block->size >= size) ? block : 0;
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
        case E2BIG: return "argument list too long";
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

void* sx_memchr(const void* block, int value, size_t count) {
    const unsigned char* bytes = (const unsigned char*)block;
    const unsigned char needle = (unsigned char)value;
    for (size_t index = 0; index < count; ++index) {
        if (bytes[index] == needle) {
            return (void*)(bytes + index);
        }
    }
    return 0;
}

size_t sx_strnlen(const char* text, size_t limit) {
    size_t length = 0;
    while (length < limit && text[length] != 0) {
        ++length;
    }
    return length;
}

char* sx_strcat(char* destination, const char* source) {
    sx_strcpy(destination + sx_strlen(destination), source);
    return destination;
}

char* sx_strncat(char* destination, const char* source, size_t count) {
    size_t end = sx_strlen(destination);
    size_t index = 0;
    while (index < count && source[index] != 0) {
        destination[end + index] = source[index];
        ++index;
    }
    destination[end + index] = 0;
    return destination;
}

/* El estado de strtok es global por definicion del estandar; strtok_r es el que
 * hay que usar donde importe. */
char* sx_strtok(char* text, const char* delimiters) {
    static char* saved = 0;
    return sx_strtok_r(text, delimiters, &saved);
}

char* sx_strsep(char** text, const char* delimiters) {
    char* start = text != 0 ? *text : 0;
    char* cursor = 0;
    if (start == 0) {
        return 0;
    }
    cursor = start + sx_strcspn(start, delimiters);
    if (*cursor != 0) {
        *cursor = 0;
        *text = cursor + 1;
    } else {
        *text = 0;
    }
    return start;
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

char* sx_strcasestr(const char* haystack, const char* needle) {
    const size_t length = sx_strlen(needle);
    if (length == 0) {
        return (char*)haystack;
    }
    for (; *haystack != 0; ++haystack) {
        if (sx_strncasecmp(haystack, needle, (unsigned long)length) == 0) {
            return (char*)haystack;
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

    while (header->size < aligned && sx_absorb_next(header)) {
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

/* alignment tiene que ser potencia de dos. Hasta SX_ALLOC_ALIGNMENT no hace
 * falta nada: malloc ya cumple. Por encima se sobre-asigna y se corre el
 * puntero, dejando el tag con el original para que free lo encuentre. */
static void* sx_allocate_aligned(size_t alignment, size_t size) {
    unsigned char* base = 0;
    unsigned char* aligned = 0;
    sx_aligned_tag* tag = 0;
    size_t total = 0;

    if (alignment <= SX_ALLOC_ALIGNMENT) {
        return sx_malloc(size);
    }
    total = size + alignment + sizeof(sx_aligned_tag);
    if (total < size) {
        return 0;
    }
    base = (unsigned char*)sx_malloc(total);
    if (base == 0) {
        return 0;
    }

    aligned = base + sizeof(sx_aligned_tag);
    aligned += (alignment - ((uintptr_t)aligned & (alignment - 1u))) & (alignment - 1u);
    tag = ((sx_aligned_tag*)aligned) - 1;
    tag->magic = SX_ALIGNED_MAGIC;
    tag->base = base;
    return aligned;
}

/* Devuelve el puntero real detras de uno alineado, o el mismo si no lo es. */
static void* sx_resolve_aligned(void* pointer) {
    const sx_aligned_tag* tag = ((const sx_aligned_tag*)pointer) - 1;
    const sx_alloc_header* header = 0;

    if (tag->magic != SX_ALIGNED_MAGIC || tag->base == 0 || tag->base >= pointer) {
        return pointer;
    }
    /* La marca sola no alcanza: se confirma que el puntero guardado sea de
     * verdad el payload de un bloque vivo. */
    header = ((const sx_alloc_header*)tag->base) - 1;
    if (!sx_block_is_valid(header) || header->free) {
        return pointer;
    }
    return tag->base;
}

int sx_posix_memalign(void** out_pointer, size_t alignment, size_t size) {
    void* block = 0;
    if (out_pointer == 0) {
        return SAVANXP_EINVAL;
    }
    if (alignment < sizeof(void*) || (alignment & (alignment - 1u)) != 0) {
        return SAVANXP_EINVAL;
    }
    block = sx_allocate_aligned(alignment, size);
    if (block == 0) {
        return SAVANXP_ENOMEM;
    }
    *out_pointer = block;
    return 0;
}

void* sx_aligned_alloc(size_t alignment, size_t size) {
    if (alignment == 0 || (alignment & (alignment - 1u)) != 0) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }
    return sx_allocate_aligned(alignment, size);
}

void* sx_memalign(size_t alignment, size_t size) {
    return sx_aligned_alloc(alignment, size);
}

void sx_free(void* pointer) {
    sx_alloc_header* header = 0;

    if (pointer == 0) {
        return;
    }

    pointer = sx_resolve_aligned(pointer);
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
    sx_arena_release(sx_arena_for_address(header));
}

static unsigned long sx_parse_unsigned(const char* text, char** endptr, int base, int* success) {
    const char* cursor = text;
    unsigned long value = 0;
    int digits = 0;
    int overflow = 0;
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
        if (value > (~0ul - (unsigned long)digit) / (unsigned long)base) {
            overflow = 1;
        } else {
            value = (value * (unsigned long)base) + (unsigned long)digit;
        }
        ++digits;
        ++cursor;
    }
    if (endptr != 0) {
        *endptr = (char*)(digits == 0 ? text : cursor);
    }
    *success = digits != 0;
    if (overflow) {
        /* El estandar pide saturar y avisar por ERANGE, no envolver. */
        g_errno = ERANGE;
        return ~0ul;
    }
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

/* Sobre LP64, long y long long son ambos de 64 bits, asi que estas comparten
 * parser con strtol/strtoul; existen igual porque un port las nombra. */
long long sx_strtoll(const char* text, char** endptr, int base) {
    return (long long)sx_strtol(text, endptr, base);
}

unsigned long long sx_strtoull(const char* text, char** endptr, int base) {
    return (unsigned long long)sx_strtoul(text, endptr, base);
}

long sx_atol(const char* text) {
    return sx_strtol(text, 0, 10);
}

/* <inttypes.h>: intmax_t es long, asi que son las mismas de arriba con otro
 * nombre. Existen porque un port las nombra. */
long sx_imaxabs(long value) {
    return value < 0 ? -value : value;
}

imaxdiv_t sx_imaxdiv(long numerator, long denominator) {
    imaxdiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

long sx_strtoimax(const char* text, char** endptr, int base) {
    return sx_strtol(text, endptr, base);
}

unsigned long sx_strtoumax(const char* text, char** endptr, int base) {
    return sx_strtoul(text, endptr, base);
}

long long sx_atoll(const char* text) {
    return (long long)sx_strtol(text, 0, 10);
}

long sx_labs(long value) {
    return value < 0 ? -value : value;
}

long long sx_llabs(long long value) {
    return value < 0 ? -value : value;
}

div_t sx_div(int numerator, int denominator) {
    div_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

ldiv_t sx_ldiv(long numerator, long denominator) {
    ldiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

lldiv_t sx_lldiv(long long numerator, long long denominator) {
    lldiv_t result;
    result.quot = numerator / denominator;
    result.rem = numerator % denominator;
    return result;
}

int sx_atoi(const char* text) {
    return (int)sx_strtol(text, 0, 10);
}

#if defined(__SSE2__)

/* --- Punto flotante -------------------------------------------------------
 *
 * Todo este bloque existe solo con SSE2. Sin -Sse el target no tiene un ABI de
 * punto flotante utilizable (ver runtime/math.c): los double viajan por una
 * convencion propia de clang y cada operacion pide helpers de soft-float que
 * este sistema no tiene. Un va_arg(args, double) en una unidad sin SSE ni
 * siquiera puede leer el valor, porque el llamador lo paso en xmm0..7 y el
 * area de registros que arma el va_list no los guarda.
 *
 * La conversion NO es correctamente redondeada como la de glibc: trabaja en
 * double, que es lo que alcanza para un log, para formatear una opcion y para
 * leer un numero de un archivo de texto. Lo que si esta cubierto son los casos
 * raros: nan, inf, cero con signo y el acarreo del redondeo (0.999 con dos
 * decimales tiene que dar 1.00, no 0.100).
 */

#define SX_DOUBLE_INFINITY __builtin_inf()
#define SX_DOUBLE_NAN __builtin_nan("")

/* Mas alla de 17 decimales un double no tiene informacion que dar; lo que se
 * pida de mas se rellena con ceros. */
#define SX_DOUBLE_EXACT_DIGITS 17
#define SX_DOUBLE_MAX_PRECISION 40
/* Por encima de esto la parte entera no entra en un unsigned long long y se
 * pasa a notacion exponencial. */
#define SX_DOUBLE_PLAIN_LIMIT 1e17
/* sign + 18 enteros + punto + 40 decimales + exponente, con aire. */
#define SX_DOUBLE_BUFFER 128

static const double g_power_of_ten[] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
    1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19,
    1e20, 1e21, 1e22};

#define SX_POWER_OF_TEN_COUNT ((int)(sizeof(g_power_of_ten) / sizeof(g_power_of_ten[0])))

static double sx_scale_by_power_of_ten(double value, int exponent) {
    while (exponent >= SX_POWER_OF_TEN_COUNT) {
        value *= g_power_of_ten[SX_POWER_OF_TEN_COUNT - 1];
        exponent -= SX_POWER_OF_TEN_COUNT - 1;
    }
    while (exponent <= -SX_POWER_OF_TEN_COUNT) {
        value /= g_power_of_ten[SX_POWER_OF_TEN_COUNT - 1];
        exponent += SX_POWER_OF_TEN_COUNT - 1;
    }
    if (exponent >= 0) {
        return value * g_power_of_ten[exponent];
    }
    return value / g_power_of_ten[-exponent];
}

/* Escribe value (positivo, finito) con exactamente `precision` decimales, sin
 * signo. Devuelve los caracteres escritos. */
static size_t sx_emit_fixed(char* out, double value, int precision) {
    const int exact = precision > SX_DOUBLE_EXACT_DIGITS ? SX_DOUBLE_EXACT_DIGITS : precision;
    unsigned long long whole = (unsigned long long)value;
    double fraction = value - (double)whole;
    unsigned long long scaled = 0;
    unsigned long long limit = 1;
    char digits[24];
    size_t digit_count = 0;
    size_t length = 0;
    int index = 0;

    for (index = 0; index < exact; ++index) {
        limit *= 10ull;
    }
    if (exact > 0) {
        scaled = (unsigned long long)(fraction * (double)limit + 0.5);
        if (scaled >= limit) {
            /* El redondeo desbordo hacia la parte entera. */
            scaled = 0;
            whole += 1;
        }
    } else if (fraction >= 0.5) {
        whole += 1;
    }

    do {
        digits[digit_count++] = (char)('0' + (whole % 10ull));
        whole /= 10ull;
    } while (whole != 0);
    while (digit_count > 0) {
        out[length++] = digits[--digit_count];
    }

    if (precision > 0) {
        out[length++] = '.';
        for (index = exact - 1; index >= 0; --index) {
            unsigned long long divisor = 1;
            int step = 0;
            for (step = 0; step < index; ++step) {
                divisor *= 10ull;
            }
            out[length++] = (char)('0' + ((scaled / divisor) % 10ull));
        }
        for (index = exact; index < precision; ++index) {
            out[length++] = '0';
        }
    }
    return length;
}

/* Los ceros que se comen son los DECIMALES. Sin el chequeo del punto, un %g de
 * 100000 (que sale de emit_fixed como "100000", sin parte fraccionaria) se
 * quedaba en "1". */
static size_t sx_strip_trailing_zeros(char* out, size_t length) {
    size_t index = 0;
    int has_point = 0;

    for (index = 0; index < length; ++index) {
        if (out[index] == '.') {
            has_point = 1;
            break;
        }
    }
    if (!has_point) {
        return length;
    }
    while (length > 0 && out[length - 1] == '0') {
        --length;
    }
    if (length > 0 && out[length - 1] == '.') {
        --length;
    }
    return length;
}

/* Formatea value segun conversion ('f', 'e' o 'g' y sus mayusculas) en out,
 * que tiene que tener SX_DOUBLE_BUFFER bytes. Devuelve los escritos. */
static size_t sx_format_double(char* out, double value, int precision, char conversion) {
    const int uppercase = conversion >= 'A' && conversion <= 'Z';
    const char lower = (char)(uppercase ? conversion + ('a' - 'A') : conversion);
    const char* text = 0;
    double normalized = 0.0;
    size_t length = 0;
    size_t start = 0;
    int negative = 0;
    int exponent = 0;
    int strip_zeros = 0;
    int use_exponent = 0;

    if (__builtin_isnan(value)) {
        text = uppercase ? "NAN" : "nan";
        while (*text != 0) {
            out[length++] = *text++;
        }
        return length;
    }
    if (value < 0.0 || (value == 0.0 && __builtin_signbit(value))) {
        negative = 1;
        value = -value;
    }
    if (__builtin_isinf(value)) {
        if (negative) {
            out[length++] = '-';
        }
        text = uppercase ? "INF" : "inf";
        while (*text != 0) {
            out[length++] = *text++;
        }
        return length;
    }

    if (precision < 0) {
        precision = 6;
    }
    if (precision > SX_DOUBLE_MAX_PRECISION) {
        precision = SX_DOUBLE_MAX_PRECISION;
    }

    /* Exponente decimal por escalado sucesivo: no depende de log10, que vive en
     * math.c y no se linkea siempre. */
    normalized = value;
    if (normalized != 0.0) {
        while (normalized >= 10.0) {
            normalized /= 10.0;
            ++exponent;
        }
        while (normalized < 1.0) {
            normalized *= 10.0;
            --exponent;
        }
    }

    if (lower == 'e') {
        use_exponent = 1;
    } else if (lower == 'g') {
        strip_zeros = 1;
        if (precision == 0) {
            precision = 1;
        }
        if (exponent < -4 || exponent >= precision) {
            precision -= 1;
            use_exponent = 1;
        } else {
            precision -= exponent + 1;
        }
    } else if (value >= SX_DOUBLE_PLAIN_LIMIT) {
        /* %f de algo que no entra en un unsigned long long. */
        use_exponent = 1;
    }

    if (negative) {
        out[length++] = '-';
    }
    start = length;

    if (!use_exponent) {
        length += sx_emit_fixed(out + length, value, precision);
        if (strip_zeros) {
            length = sx_strip_trailing_zeros(out, length);
        }
        return length;
    }

    length += sx_emit_fixed(out + length, normalized, precision);
    /* El redondeo puede empujar la mantisa a 10.x: se renormaliza a 1.0 con un
     * exponente mas. Se detecta porque la parte entera quedo con dos digitos. */
    if (length > start + 1 && out[start + 1] != '.') {
        ++exponent;
        length = start + sx_emit_fixed(out + start, 1.0, precision);
    }
    if (strip_zeros) {
        length = sx_strip_trailing_zeros(out, length);
    }
    out[length++] = uppercase ? 'E' : 'e';
    out[length++] = exponent < 0 ? '-' : '+';
    {
        const int magnitude = exponent < 0 ? -exponent : exponent;
        if (magnitude >= 100) {
            out[length++] = (char)('0' + (magnitude / 100));
        }
        out[length++] = (char)('0' + ((magnitude / 10) % 10));
        out[length++] = (char)('0' + (magnitude % 10));
    }
    return length;
}

double sx_strtod(const char* text, char** endptr) {
    const char* cursor = text;
    double mantissa = 0.0;
    int negative = 0;
    int exponent = 0;
    int seen_digit = 0;

    if (endptr != 0) {
        *endptr = (char*)text;
    }
    if (text == 0) {
        return 0.0;
    }
    while (*cursor != 0 && sx_isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    if (*cursor == '-') {
        negative = 1;
        ++cursor;
    } else if (*cursor == '+') {
        ++cursor;
    }

    if (sx_strncasecmp(cursor, "inf", 3) == 0) {
        cursor += 3;
        if (sx_strncasecmp(cursor, "inity", 5) == 0) {
            cursor += 5;
        }
        if (endptr != 0) {
            *endptr = (char*)cursor;
        }
        return negative ? -SX_DOUBLE_INFINITY : SX_DOUBLE_INFINITY;
    }
    if (sx_strncasecmp(cursor, "nan", 3) == 0) {
        cursor += 3;
        if (endptr != 0) {
            *endptr = (char*)cursor;
        }
        return SX_DOUBLE_NAN;
    }

    while (*cursor >= '0' && *cursor <= '9') {
        mantissa = mantissa * 10.0 + (double)(*cursor - '0');
        ++cursor;
        seen_digit = 1;
    }
    if (*cursor == '.') {
        ++cursor;
        while (*cursor >= '0' && *cursor <= '9') {
            mantissa = mantissa * 10.0 + (double)(*cursor - '0');
            --exponent;
            ++cursor;
            seen_digit = 1;
        }
    }
    if (!seen_digit) {
        g_errno = SAVANXP_EINVAL;
        return 0.0;
    }

    if (*cursor == 'e' || *cursor == 'E') {
        const char* mark = cursor;
        int exponent_sign = 1;
        int exponent_value = 0;
        int exponent_digits = 0;
        ++cursor;
        if (*cursor == '-') {
            exponent_sign = -1;
            ++cursor;
        } else if (*cursor == '+') {
            ++cursor;
        }
        while (*cursor >= '0' && *cursor <= '9') {
            if (exponent_value < 100000) {
                exponent_value = exponent_value * 10 + (*cursor - '0');
            }
            ++cursor;
            ++exponent_digits;
        }
        if (exponent_digits == 0) {
            cursor = mark; /* la 'e' no era parte del numero */
        } else {
            exponent += exponent_sign * exponent_value;
        }
    }

    if (endptr != 0) {
        *endptr = (char*)cursor;
    }
    mantissa = sx_scale_by_power_of_ten(mantissa, exponent);
    return negative ? -mantissa : mantissa;
}

float sx_strtof(const char* text, char** endptr) {
    return (float)sx_strtod(text, endptr);
}

double sx_difftime(time_t later, time_t earlier) {
    return (double)(later - earlier);
}

double sx_atof(const char* text) {
    return sx_strtod(text, 0);
}

#else

/* Sin SSE no hay como leer ni devolver un double: se deja el stub historico
 * para que el simbolo exista y el link no se caiga. Una app que necesite
 * punto flotante se construye con -Sse (ver tools/build-user.ps1). */
double sx_atof(const char* text) {
    union {
        uint64_t bits;
        double value;
    } result = {0};
    (void)text;
    return result.value;
}

#endif

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
    long result = savanxp_read(fd, buffer, count);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

ssize_t sx_write(int fd, const void* buffer, size_t count) {
    long result = savanxp_write(fd, buffer, count);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return result;
}

int sx_close(int fd) {
    long result = savanxp_close(fd);
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
    result = savanxp_open_mode(path, raw_flags);
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
    long result = savanxp_unlink(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_mkdir(const char* path, mode_t mode) {
    long result = 0;
    (void)mode; /* SxFS no tiene permisos */
    result = savanxp_mkdir(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_rmdir(const char* path) {
    long result = savanxp_rmdir(path);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return 0;
}

int sx_dup(int fd) {
    long result = savanxp_dup(fd);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int sx_dup2(int oldfd, int newfd) {
    long result = savanxp_dup2(oldfd, newfd);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (int)result;
}

int sx_pipe(int fds[2]) {
    long result = savanxp_pipe(fds);
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

    result = savanxp_fcntl(fd, (unsigned long)command, raw_value);
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

    result = savanxp_poll(raw, count, timeout_ms);
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
        memset(info, 0, sizeof(*info));
        info->st_dev = 1;
        info->st_ino = sx_hash_text(path);
        info->st_mode = raw.st_mode;
        info->st_size = raw.st_size;
        info->st_nlink = 1;
        info->st_blksize = 512;
        info->st_blocks = (raw.st_size + 511u) / 512u;
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
        memset(info, 0, sizeof(*info));
        info->st_dev = 1;
        info->st_ino = (unsigned long)(fd + 1);
        info->st_mode = raw.st_mode;
        info->st_size = raw.st_size;
        info->st_nlink = 1;
        info->st_blksize = 512;
        info->st_blocks = (raw.st_size + 511u) / 512u;
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
    long result = savanxp_sync();
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
    result = savanxp_readdir(directory->fd, name, sizeof(name));
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
            g_file_pool[index].pushback = -1;
            return &g_file_pool[index];
        }
    }
    return 0;
}

static void sx_release_file(FILE* stream) {
    if (stream != 0 && !stream->is_stdio) {
        memset(stream, 0, sizeof(*stream));
        stream->fd = -1;
        stream->pushback = -1;
    }
}

/* Sinks de a tandas para el formateador.
 *
 * sx_vformat emite de a un caracter, y stdout/stderr son streams sin buffer
 * (is_stdio => sx_direct_write), asi que emitir directo costaba un write() por
 * caracter: una linea de 60 columnas eran 60 syscalls. Los sinks juntan la
 * salida y la bajan de una; el flush final corre siempre, incluso si el
 * formato se corto por la mitad. */
typedef struct sx_stream_sink {
    FILE* stream;
    size_t used;
    char bytes[SX_FILE_BUFFER_CAPACITY];
} sx_stream_sink;

static int sx_stream_sink_flush(sx_stream_sink* sink) {
    const size_t pending = sink->used;
    sink->used = 0;
    if (pending == 0) {
        return 1;
    }
    if (sx_fwrite(sink->bytes, 1, pending, sink->stream) != pending) {
        sink->stream->error = 1;
        return 0;
    }
    return 1;
}

static int sx_emit_stream_char(char character, void* context) {
    sx_stream_sink* sink = (sx_stream_sink*)context;
    if (sink->used == sizeof(sink->bytes) && !sx_stream_sink_flush(sink)) {
        return 0;
    }
    sink->bytes[sink->used++] = character;
    return 1;
}

typedef struct sx_fd_sink {
    int fd;
    int failed;
    size_t used;
    char bytes[SX_FILE_BUFFER_CAPACITY];
} sx_fd_sink;

static int sx_fd_sink_flush(sx_fd_sink* sink) {
    const size_t pending = sink->used;
    size_t consumed = 0;
    sink->used = 0;
    while (consumed < pending) {
        const ssize_t written = sx_write(sink->fd, sink->bytes + consumed, pending - consumed);
        if (written <= 0) {
            sink->failed = 1;
            return 0;
        }
        consumed += (size_t)written;
    }
    return 1;
}

static int sx_emit_fd_char(char character, void* context) {
    sx_fd_sink* sink = (sx_fd_sink*)context;
    if (sink->used == sizeof(sink->bytes) && !sx_fd_sink_flush(sink)) {
        return 0;
    }
    sink->bytes[sink->used++] = character;
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

static int sx_write_padded(int (*emit)(char, void*), void* context, const char* text, size_t text_length, int width, char pad, int left_align) {
    int count = 0;
    size_t padding = 0;

    if (width > 0 && text_length < (size_t)width) {
        padding = (size_t)width - text_length;
    }

    // El flag '-' desactiva el relleno con ceros y manda el padding al final.
    if (left_align) {
        pad = ' ';
    } else {
        for (size_t index = 0; index < padding; ++index) {
            if (!emit(pad, context)) {
                return count;
            }
            ++count;
        }
    }

    for (size_t index = 0; index < text_length; ++index) {
        if (!emit(text[index], context)) {
            return count;
        }
        ++count;
    }

    if (left_align) {
        for (size_t index = 0; index < padding; ++index) {
            if (!emit(pad, context)) {
                return count;
            }
            ++count;
        }
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
            int left_align = 0;
            int width = 0;
            int precision = -1;
            int long_count = 0;
            int use_size_t = 0;
            char buffer[128];
            size_t length = 0;

            for (;;) {
                if (*cursor == '-') {
                    left_align = 1;
                } else if (*cursor == '0') {
                    pad = '0';
                } else {
                    break;
                }
                ++cursor;
            }

            if (*cursor == '*') {
                width = va_arg(args, int);
                // printf(3): un ancho negativo equivale al flag '-'.
                if (width < 0) {
                    left_align = 1;
                    width = -width;
                }
                ++cursor;
            } else {
                while (*cursor >= '0' && *cursor <= '9') {
                    width = (width * 10) + (*cursor - '0');
                    ++cursor;
                }
            }

            if (*cursor == '.') {
                precision = 0;
                ++cursor;
                if (*cursor == '*') {
                    precision = va_arg(args, int);
                    ++cursor;
                } else {
                    while (*cursor >= '0' && *cursor <= '9') {
                        precision = (precision * 10) + (*cursor - '0');
                        ++cursor;
                    }
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
                    written += sx_write_padded(emit, context, buffer, 1, width, pad, left_align);
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

                    written += sx_write_padded(emit, context, text, text_length, width, pad, left_align);
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
                    written += sx_write_padded(emit, context, buffer, length, width, pad, left_align);
                    break;
                case 'u':
                    if (use_size_t || long_count > 0) {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned long), 10u, 0, buffer);
                    } else {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned int), 10u, 0, buffer);
                    }
                    length = sx_apply_numeric_precision(buffer, length, precision);
                    written += sx_write_padded(emit, context, buffer, length, width, pad, left_align);
                    break;
                case 'x':
                case 'X':
                    if (use_size_t || long_count > 0) {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned long), 16u, *cursor == 'X', buffer);
                    } else {
                        length = sx_unsigned_to_string((unsigned long long)va_arg(args, unsigned int), 16u, *cursor == 'X', buffer);
                    }
                    length = sx_apply_numeric_precision(buffer, length, precision);
                    written += sx_write_padded(emit, context, buffer, length, width, pad, left_align);
                    break;
                case 'p': {
                    uintptr_t value = (uintptr_t)va_arg(args, void*);
                    buffer[0] = '0';
                    buffer[1] = 'x';
                    length = 2 + sx_unsigned_to_string((unsigned long long)value, 16u, 0, buffer + 2);
                    written += sx_write_padded(emit, context, buffer, length, width, pad, left_align);
                    break;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G': {
#if defined(__SSE2__)
                    char number[SX_DOUBLE_BUFFER];
                    const double value = va_arg(args, double);
                    length = sx_format_double(number, value, precision, *cursor);
                    written += sx_write_padded(emit, context, number, length, width, pad, left_align);
#else
                    /* Sin SSE el llamador no pudo pasar el double por xmm0..7,
                     * asi que aca no hay nada que leer. Queda el stub historico:
                     * una app con punto flotante se construye con -Sse. */
                    (void)va_arg(args, double);
                    written += sx_write_padded(emit, context, "0", 1, width, pad, left_align);
#endif
                    break;
                }
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

/* Rellena el buffer de lectura. Devuelve los bytes disponibles, 0 en fin de
 * archivo o error. Solo para streams de archivo: en un stdio leer de mas le
 * robaria al proceso input que todavia no pidio. */
static size_t sx_fill_read_buffer(FILE* stream) {
    ssize_t result = 0;
    if (stream->read_buffer_pos < stream->read_buffer_used) {
        return stream->read_buffer_used - stream->read_buffer_pos;
    }
    stream->read_buffer_pos = 0;
    stream->read_buffer_used = 0;
    result = sx_read(stream->fd, stream->read_buffer, sizeof(stream->read_buffer));
    if (result < 0) {
        stream->error = 1;
        return 0;
    }
    if (result == 0) {
        stream->eof = 1;
        return 0;
    }
    stream->read_buffer_used = (size_t)result;
    return stream->read_buffer_used;
}

size_t sx_fread(void* buffer, size_t size, size_t count, FILE* stream) {
    unsigned char* output = (unsigned char*)buffer;
    size_t wanted = 0;
    size_t filled = 0;

    if (stream == 0 || buffer == 0 || size == 0 || count == 0) {
        return 0;
    }
    if (stream->can_write && stream->write_buffer_used != 0 && sx_flush_stream(stream) == EOF) {
        return 0;
    }
    wanted = size * count;

    if (stream->pushback >= 0 && filled < wanted) {
        output[filled++] = (unsigned char)stream->pushback;
        stream->pushback = -1;
    }

    while (filled < wanted) {
        size_t available = stream->read_buffer_used - stream->read_buffer_pos;
        size_t chunk = 0;

        if (available == 0) {
            if (stream->is_stdio) {
                /* Sin buffer: una sola bajada por lo que falte. */
                const ssize_t direct = sx_read(stream->fd, output + filled, wanted - filled);
                if (direct < 0) {
                    stream->error = 1;
                    break;
                }
                if (direct == 0) {
                    stream->eof = 1;
                    break;
                }
                filled += (size_t)direct;
                break;
            }
            available = sx_fill_read_buffer(stream);
            if (available == 0) {
                break;
            }
        }

        chunk = (wanted - filled) < available ? (wanted - filled) : available;
        sx_memcpy(output + filled, stream->read_buffer + stream->read_buffer_pos, chunk);
        stream->read_buffer_pos += chunk;
        filled += chunk;
    }

    return filled / size;
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
    if (whence == SEEK_CUR) {
        /* El fd esta adelantado por lo que quedo sin consumir en el buffer. */
        offset -= (long)(stream->read_buffer_used - stream->read_buffer_pos);
        if (stream->pushback >= 0) {
            offset -= 1;
        }
    }
    stream->read_buffer_used = 0;
    stream->read_buffer_pos = 0;
    stream->pushback = -1;
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
    /* Lo que ya se leyo del fd pero el programa todavia no consumio. */
    position -= (long)(stream->read_buffer_used - stream->read_buffer_pos);
    if (stream->pushback >= 0) {
        position -= 1;
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
    sx_stream_sink sink;
    int written = 0;
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    sink.stream = stream;
    sink.used = 0;
    written = sx_vformat(sx_emit_stream_char, &sink, format, args);
    return sx_stream_sink_flush(&sink) ? written : -1;
}

int sx_vdprintf(int fd, const char* format, va_list args) {
    sx_fd_sink sink;
    int written = 0;
    sink.fd = fd;
    sink.failed = 0;
    sink.used = 0;
    written = sx_vformat(sx_emit_fd_char, &sink, format, args);
    if (!sx_fd_sink_flush(&sink) || sink.failed) {
        return -1;
    }
    return written;
}

int sx_dprintf(int fd, const char* format, ...) {
    int written = 0;
    va_list args;
    va_start(args, format);
    written = sx_vdprintf(fd, format, args);
    va_end(args);
    return written;
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

/* Helpers de consola de SavanXP (declarados en savanxp/libc.h).
 *
 * Viven aca y no en runtime/libc.c -- que es la capa cruda de syscalls --
 * porque comparten el formateador con el resto de stdio: tener dos era la
 * razon por la que agregar un especificador nuevo habia que hacerlo dos veces,
 * y por la que %s se comportaba distinto segun que headers incluyera la app.
 *
 * puts_out NO agrega salto de linea. Ese es el motivo de que no se llame
 * `puts`: el `puts` estandar (sx_puts) si lo agrega, y fusionarlos habria
 * cambiado en silencio toda la salida de consola del arbol. */
void putchar_fd(int fd, char character) {
    (void)sx_write(fd, &character, 1);
}

void puts_fd(int fd, const char* text) {
    const size_t length = sx_strlen(text);
    size_t consumed = 0;
    while (consumed < length) {
        const ssize_t written = sx_write(fd, text + consumed, length - consumed);
        if (written <= 0) {
            return;
        }
        consumed += (size_t)written;
    }
}

void puts_err(const char* text) {
    puts_fd(SAVANXP_STDERR_FILENO, text);
}

void puts_out(const char* text) {
    puts_fd(SAVANXP_STDOUT_FILENO, text);
}

void printf_fd(int fd, const char* format, ...) {
    va_list args;
    va_start(args, format);
    (void)sx_vdprintf(fd, format, args);
    va_end(args);
}

void eprintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    (void)sx_vdprintf(SAVANXP_STDERR_FILENO, format, args);
    va_end(args);
}

/* Nombres estandar como alias debiles de la implementacion sx_*: el simbolo
 * `printf` tiene que existir de verdad para que compile codigo de terceros que
 * incluye <stdio.h> sin saber nada de SavanXP. Debiles para que una app que
 * traiga su propia version gane el link en vez de chocar. */
__attribute__((weak, alias("sx_printf"))) int printf(const char* format, ...);
__attribute__((weak, alias("sx_puts"))) int puts(const char* text);
__attribute__((weak, alias("sx_putchar"))) int putchar(int character);

/* --- sscanf ---------------------------------------------------------------
 *
 * Cubre lo que usa codigo real: %d %i %u %o %x %c %s %[...] %n %%, los
 * modificadores de largo (hh h l ll z j t), el ancho maximo y la supresion con
 * '*'. Los de punto flotante (%f %e %g %a) solo con SSE, por el mismo motivo
 * que el resto del punto flotante de este archivo.
 *
 * No hay fscanf: pedirle a un FILE el lookahead arbitrario que necesita la
 * vuelta atras de una conversion fallida requiere mas pushback que el byte que
 * garantiza ungetc.
 */

enum sx_scan_length {
    SX_SCAN_INT = 0,
    SX_SCAN_CHAR,
    SX_SCAN_SHORT,
    SX_SCAN_LONG,
    SX_SCAN_LONG_LONG,
    SX_SCAN_SIZE
};

static void sx_store_signed(void* target, enum sx_scan_length length, long long value) {
    switch (length) {
        case SX_SCAN_CHAR: *(signed char*)target = (signed char)value; break;
        case SX_SCAN_SHORT: *(short*)target = (short)value; break;
        case SX_SCAN_LONG: *(long*)target = (long)value; break;
        case SX_SCAN_LONG_LONG: *(long long*)target = value; break;
        case SX_SCAN_SIZE: *(size_t*)target = (size_t)value; break;
        default: *(int*)target = (int)value; break;
    }
}

static void sx_store_unsigned(void* target, enum sx_scan_length length, unsigned long long value) {
    switch (length) {
        case SX_SCAN_CHAR: *(unsigned char*)target = (unsigned char)value; break;
        case SX_SCAN_SHORT: *(unsigned short*)target = (unsigned short)value; break;
        case SX_SCAN_LONG: *(unsigned long*)target = (unsigned long)value; break;
        case SX_SCAN_LONG_LONG: *(unsigned long long*)target = value; break;
        case SX_SCAN_SIZE: *(size_t*)target = (size_t)value; break;
        default: *(unsigned int*)target = (unsigned int)value; break;
    }
}

/* Arma la tabla de un scanset %[...] y devuelve el cursor pasado el ']'. */
static const char* sx_parse_scanset(const char* format, unsigned char* table, int* negated) {
    int previous = -1;

    for (previous = 0; previous < 256; ++previous) {
        table[previous] = 0;
    }
    *negated = 0;
    if (*format == '^') {
        *negated = 1;
        ++format;
    }
    previous = -1;
    /* Un ']' como primer caracter es literal, no cierra el conjunto. */
    if (*format == ']') {
        table[(unsigned char)']'] = 1;
        previous = ']';
        ++format;
    }
    while (*format != 0 && *format != ']') {
        if (*format == '-' && previous >= 0 && format[1] != 0 && format[1] != ']') {
            int limit = (unsigned char)format[1];
            int step = 0;
            for (step = previous; step <= limit; ++step) {
                table[step] = 1;
            }
            previous = -1;
            format += 2;
            continue;
        }
        table[(unsigned char)*format] = 1;
        previous = (unsigned char)*format;
        ++format;
    }
    if (*format == ']') {
        ++format;
    }
    return format;
}

int sx_vsscanf(const char* input, const char* format, va_list args) {
    const char* cursor = input;
    int assigned = 0;
    int matched_anything = 0;

    if (input == 0 || format == 0) {
        return EOF;
    }

    while (*format != 0) {
        if (sx_isspace((unsigned char)*format)) {
            while (*cursor != 0 && sx_isspace((unsigned char)*cursor)) {
                ++cursor;
            }
            ++format;
            continue;
        }
        if (*format != '%') {
            if (*cursor != *format) {
                return matched_anything || *cursor != 0 ? assigned : EOF;
            }
            ++cursor;
            ++format;
            continue;
        }

        ++format;
        {
            enum sx_scan_length length = SX_SCAN_INT;
            int suppress = 0;
            int width = 0;
            void* target = 0;

            if (*format == '*') {
                suppress = 1;
                ++format;
            }
            while (*format >= '0' && *format <= '9') {
                width = width * 10 + (*format - '0');
                ++format;
            }
            if (*format == 'h') {
                ++format;
                length = SX_SCAN_SHORT;
                if (*format == 'h') {
                    ++format;
                    length = SX_SCAN_CHAR;
                }
            } else if (*format == 'l') {
                ++format;
                length = SX_SCAN_LONG;
                if (*format == 'l') {
                    ++format;
                    length = SX_SCAN_LONG_LONG;
                }
            } else if (*format == 'z' || *format == 'j' || *format == 't') {
                ++format;
                length = SX_SCAN_SIZE;
            } else if (*format == 'L') {
                ++format;
                length = SX_SCAN_LONG_LONG;
            }

            if (*format == '%') {
                if (*cursor != '%') {
                    return assigned;
                }
                ++cursor;
                ++format;
                continue;
            }
            if (*format == 'n') {
                if (!suppress) {
                    sx_store_signed(va_arg(args, void*), length, (long long)(cursor - input));
                }
                ++format;
                continue;
            }

            /* Todas las conversiones menos %c y %[ se saltan el espacio. */
            if (*format != 'c' && *format != '[') {
                while (*cursor != 0 && sx_isspace((unsigned char)*cursor)) {
                    ++cursor;
                }
            }
            if (*cursor == 0) {
                return assigned != 0 ? assigned : EOF;
            }
            if (!suppress) {
                target = va_arg(args, void*);
            }

            switch (*format) {
                case 'd':
                case 'i':
                case 'u':
                case 'o':
                case 'x':
                case 'X':
                case 'p': {
                    const int base = (*format == 'x' || *format == 'X' || *format == 'p')
                                         ? 16
                                         : (*format == 'o' ? 8 : (*format == 'i' ? 0 : 10));
                    const int is_signed = *format == 'd' || *format == 'i';
                    char limited[64];
                    const char* source = cursor;
                    char* end = 0;

                    if (width > 0 && (size_t)width < sizeof(limited)) {
                        size_t index = 0;
                        while (index < (size_t)width && cursor[index] != 0) {
                            limited[index] = cursor[index];
                            ++index;
                        }
                        limited[index] = 0;
                        source = limited;
                    }

                    if (is_signed) {
                        const long value = sx_strtol(source, &end, base);
                        if (end == source) {
                            return assigned;
                        }
                        if (!suppress) {
                            sx_store_signed(target, length, (long long)value);
                        }
                    } else {
                        const unsigned long value = sx_strtoul(source, &end, base);
                        if (end == source) {
                            return assigned;
                        }
                        if (!suppress) {
                            sx_store_unsigned(target, length, (unsigned long long)value);
                        }
                    }
                    cursor += (size_t)(end - source);
                    break;
                }
                case 'f':
                case 'F':
                case 'e':
                case 'E':
                case 'g':
                case 'G':
                case 'a':
                case 'A': {
#if defined(__SSE2__)
                    char* end = 0;
                    const double value = sx_strtod(cursor, &end);
                    if (end == cursor) {
                        return assigned;
                    }
                    if (!suppress) {
                        if (length == SX_SCAN_INT) {
                            *(float*)target = (float)value;
                        } else {
                            *(double*)target = value;
                        }
                    }
                    cursor = end;
                    break;
#else
                    /* Sin SSE no hay donde guardar el resultado. */
                    return assigned;
#endif
                }
                case 'c': {
                    const int count = width > 0 ? width : 1;
                    int index = 0;
                    for (index = 0; index < count; ++index) {
                        if (cursor[index] == 0) {
                            return assigned;
                        }
                        if (!suppress) {
                            ((char*)target)[index] = cursor[index];
                        }
                    }
                    cursor += count;
                    break;
                }
                case 's': {
                    size_t index = 0;
                    while (cursor[index] != 0 && !sx_isspace((unsigned char)cursor[index]) &&
                           (width == 0 || index < (size_t)width)) {
                        if (!suppress) {
                            ((char*)target)[index] = cursor[index];
                        }
                        ++index;
                    }
                    if (index == 0) {
                        return assigned;
                    }
                    if (!suppress) {
                        ((char*)target)[index] = 0;
                    }
                    cursor += index;
                    break;
                }
                case '[': {
                    unsigned char table[256];
                    int negated = 0;
                    size_t index = 0;
                    format = sx_parse_scanset(format + 1, table, &negated) - 1;
                    while (cursor[index] != 0 && (width == 0 || index < (size_t)width)) {
                        const int inside = table[(unsigned char)cursor[index]] != 0;
                        if (inside == negated) {
                            break;
                        }
                        if (!suppress) {
                            ((char*)target)[index] = cursor[index];
                        }
                        ++index;
                    }
                    if (index == 0) {
                        return assigned;
                    }
                    if (!suppress) {
                        ((char*)target)[index] = 0;
                    }
                    cursor += index;
                    break;
                }
                default:
                    /* Conversion desconocida: se corta, como manda el estandar. */
                    return assigned;
            }

            matched_anything = 1;
            if (!suppress) {
                ++assigned;
            }
            ++format;
        }
    }

    return assigned;
}

int sx_sscanf(const char* input, const char* format, ...) {
    int result = 0;
    va_list args;
    va_start(args, format);
    result = sx_vsscanf(input, format, args);
    va_end(args);
    return result;
}

int sx_fgetc(FILE* stream) {
    unsigned char value = 0;
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return EOF;
    }
    return sx_fread(&value, 1, 1, stream) == 1 ? (int)value : EOF;
}

int sx_getc(FILE* stream) {
    return sx_fgetc(stream);
}

int sx_getchar(void) {
    return sx_fgetc(stdin);
}

int sx_fputc(int character, FILE* stream) {
    return sx_putc(character, stream);
}

/* Un solo byte de pushback, que es lo unico que el estandar garantiza. */
int sx_ungetc(int character, FILE* stream) {
    if (stream == 0 || character == EOF || stream->pushback >= 0) {
        return EOF;
    }
    stream->pushback = (int)(unsigned char)character;
    stream->eof = 0;
    return (int)(unsigned char)character;
}

int sx_fileno(FILE* stream) {
    if (stream == 0) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    return stream->fd;
}

FILE* sx_fdopen(int fd, const char* mode) {
    FILE* stream = 0;
    if (fd < 0 || mode == 0) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }
    stream = sx_allocate_file();
    if (stream == 0) {
        g_errno = SAVANXP_ENOMEM;
        return 0;
    }
    stream->fd = fd;
    stream->can_write = mode[0] == 'w' || mode[0] == 'a' || sx_strchr(mode, '+') != 0;
    stream->can_read = mode[0] == 'r' || sx_strchr(mode, '+') != 0;
    return stream;
}

/* El buffer es fijo y vive adentro del FILE, asi que no se puede adoptar el que
 * pasa el programa. Se acepta la llamada -- que es lo que espera un port, que
 * la usa para pedir "mas rapido", no para cambiar la semantica -- y solo se
 * rechaza un modo invalido. */
int sx_setvbuf(FILE* stream, char* buffer, int mode, size_t size) {
    (void)buffer;
    (void)size;
    if (stream == 0 || (mode != SX_IOFBF && mode != SX_IOLBF && mode != SX_IONBF)) {
        g_errno = SAVANXP_EINVAL;
        return -1;
    }
    return 0;
}

void sx_setbuf(FILE* stream, char* buffer) {
    (void)sx_setvbuf(stream, buffer, buffer != 0 ? SX_IOFBF : SX_IONBF, SX_FILE_BUFFER_CAPACITY);
}

void sx_rewind(FILE* stream) {
    (void)sx_fseek(stream, 0, SEEK_SET);
    if (stream != 0) {
        stream->error = 0;
    }
}

off_t sx_ftello(FILE* stream) {
    return (off_t)sx_ftell(stream);
}

int sx_fseeko(FILE* stream, off_t offset, int whence) {
    return sx_fseek(stream, (long)offset, whence);
}

int sx_vsprintf(char* buffer, const char* format, va_list args) {
    return sx_vsnprintf(buffer, (size_t)-1, format, args);
}

/* Respaldo del macro assert() de <assert.h>. No vuelve. */
void sx_assert_failed(const char* expression, const char* file, int line) {
    (void)sx_fprintf(stderr, "assert: %s:%d: %s\n",
                     file != 0 ? file : "?", line, expression != 0 ? expression : "?");
    sx_abort();
}

void sx_perror(const char* prefix) {
    const char* reason = sx_strerror(g_errno);
    if (prefix != 0 && prefix[0] != 0) {
        (void)sx_fprintf(stderr, "%s: %s\n", prefix, reason);
    } else {
        (void)sx_fprintf(stderr, "%s\n", reason);
    }
}

int sx_remove(const char* path) {
    return sx_unlink(path);
}

int sx_rename(const char* old_path, const char* new_path) {
    long result = savanxp_rename(old_path, new_path);
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
    result = savanxp_waitpid(pid, status);
    if (result < 0) {
        sx_set_errno_from_result(result);
        return -1;
    }
    return (pid_t)result;
}

pid_t sx_fork(void) {
    long result = savanxp_fork();
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
    (void)savanxp_close((int)section);
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

/* --- Calendario -----------------------------------------------------------
 *
 * Todo UTC: no hay base de datos de zonas horarias, asi que localtime es
 * gmtime y mktime es timegm. El RTC del kernel (savanxp_realtime) tambien
 * entrega UTC, asi que la cadena entera es coherente.
 *
 * La conversion usa el algoritmo de dias civiles de Howard Hinnant, que es
 * aritmetica entera pura: sin tablas de anios bisiestos y valido para
 * cualquier fecha proleptica gregoriana. Importa que sea entero porque este
 * archivo tambien se compila sin SSE, donde no hay punto flotante utilizable.
 */

#define SX_SECONDS_PER_DAY 86400L

static const char* const g_month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char* const g_day_names[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/* Dias desde 1970-01-01 para una fecha civil. month va 1-12. */
static long sx_days_from_civil(long year, long month, long day) {
    long era = 0;
    unsigned long year_of_era = 0;
    unsigned long day_of_year = 0;
    unsigned long day_of_era = 0;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    year_of_era = (unsigned long)(year - era * 400);
    day_of_year = (unsigned long)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + (long)day_of_era - 719468;
}

static void sx_civil_from_days(long days, int* year, int* month, int* day) {
    long era = 0;
    unsigned long day_of_era = 0;
    unsigned long year_of_era = 0;
    unsigned long day_of_year = 0;
    unsigned long month_prime = 0;
    long civil_year = 0;

    days += 719468;
    era = (days >= 0 ? days : days - 146096) / 146097;
    day_of_era = (unsigned long)(days - era * 146097);
    year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    civil_year = (long)year_of_era + era * 400;
    day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    month_prime = (5 * day_of_year + 2) / 153;

    *day = (int)(day_of_year - (153 * month_prime + 2) / 5 + 1);
    *month = (int)(month_prime + (month_prime < 10 ? 3 : -9));
    *year = (int)(civil_year + (*month <= 2));
}

/* Divide redondeando hacia abajo, que es lo que hace falta para fechas
 * anteriores a 1970: -1 / 86400 tiene que dar -1, no 0. */
static long sx_floor_div(long numerator, long denominator) {
    long quotient = numerator / denominator;
    if ((numerator % denominator) != 0 && ((numerator < 0) != (denominator < 0))) {
        quotient -= 1;
    }
    return quotient;
}

struct tm* sx_gmtime_r(const time_t* value, struct tm* result) {
    long seconds = 0;
    long days = 0;
    long rest = 0;
    int year = 0;
    int month = 0;
    int day = 0;

    if (value == 0 || result == 0) {
        g_errno = SAVANXP_EINVAL;
        return 0;
    }
    seconds = (long)*value;
    days = sx_floor_div(seconds, SX_SECONDS_PER_DAY);
    rest = seconds - days * SX_SECONDS_PER_DAY;

    sx_civil_from_days(days, &year, &month, &day);
    result->tm_sec = (int)(rest % 60);
    result->tm_min = (int)((rest / 60) % 60);
    result->tm_hour = (int)(rest / 3600);
    result->tm_mday = day;
    result->tm_mon = month - 1;
    result->tm_year = year - 1900;
    /* 1970-01-01 fue jueves. */
    result->tm_wday = (int)(((days % 7) + 11) % 7);
    result->tm_yday = (int)(days - sx_days_from_civil(year, 1, 1));
    result->tm_isdst = 0;
    return result;
}

struct tm* sx_gmtime(const time_t* value) {
    static struct tm shared;
    return sx_gmtime_r(value, &shared);
}

struct tm* sx_localtime_r(const time_t* value, struct tm* result) {
    return sx_gmtime_r(value, result);
}

struct tm* sx_localtime(const time_t* value) {
    return sx_gmtime(value);
}

time_t sx_timegm(struct tm* value) {
    long days = 0;
    long seconds = 0;
    if (value == 0) {
        g_errno = SAVANXP_EINVAL;
        return (time_t)-1;
    }
    days = sx_days_from_civil((long)value->tm_year + 1900, (long)value->tm_mon + 1,
                              (long)value->tm_mday);
    seconds = days * SX_SECONDS_PER_DAY +
              (long)value->tm_hour * 3600 + (long)value->tm_min * 60 + (long)value->tm_sec;
    /* mktime/timegm normalizan los campos de salida ademas de convertir. */
    {
        const time_t stamp = (time_t)seconds;
        (void)sx_gmtime_r(&stamp, value);
        return stamp;
    }
}

time_t sx_mktime(struct tm* value) {
    return sx_timegm(value);
}

static size_t sx_append_text(char* buffer, size_t capacity, size_t used, const char* text) {
    while (*text != 0) {
        if (used + 1 < capacity) {
            buffer[used] = *text;
        }
        ++used;
        ++text;
    }
    return used;
}

static size_t sx_append_number(char* buffer, size_t capacity, size_t used, long value,
                               int width, char pad) {
    char digits[24];
    size_t length = 0;
    int negative = value < 0;
    unsigned long magnitude = (unsigned long)(negative ? -value : value);

    do {
        digits[length++] = (char)('0' + (magnitude % 10u));
        magnitude /= 10u;
    } while (magnitude != 0);
    while (length < (size_t)width) {
        digits[length++] = pad;
    }
    if (negative) {
        digits[length++] = '-';
    }
    while (length > 0) {
        --length;
        if (used + 1 < capacity) {
            buffer[used] = digits[length];
        }
        ++used;
    }
    return used;
}

size_t sx_strftime(char* buffer, size_t capacity, const char* format, const struct tm* value) {
    size_t used = 0;

    if (buffer == 0 || capacity == 0 || format == 0 || value == 0) {
        return 0;
    }

    for (; *format != 0; ++format) {
        if (*format != '%') {
            if (used + 1 < capacity) {
                buffer[used] = *format;
            }
            ++used;
            continue;
        }
        ++format;
        switch (*format) {
            case 'Y': used = sx_append_number(buffer, capacity, used, value->tm_year + 1900L, 0, '0'); break;
            case 'y': used = sx_append_number(buffer, capacity, used, (value->tm_year + 1900L) % 100, 2, '0'); break;
            case 'm': used = sx_append_number(buffer, capacity, used, value->tm_mon + 1L, 2, '0'); break;
            case 'd': used = sx_append_number(buffer, capacity, used, value->tm_mday, 2, '0'); break;
            case 'e': used = sx_append_number(buffer, capacity, used, value->tm_mday, 2, ' '); break;
            case 'H': used = sx_append_number(buffer, capacity, used, value->tm_hour, 2, '0'); break;
            case 'M': used = sx_append_number(buffer, capacity, used, value->tm_min, 2, '0'); break;
            case 'S': used = sx_append_number(buffer, capacity, used, value->tm_sec, 2, '0'); break;
            case 'j': used = sx_append_number(buffer, capacity, used, value->tm_yday + 1L, 3, '0'); break;
            case 'a':
                used = sx_append_text(buffer, capacity, used,
                                      g_day_names[(unsigned)value->tm_wday % 7u]);
                break;
            case 'b':
            case 'h':
                used = sx_append_text(buffer, capacity, used,
                                      g_month_names[(unsigned)value->tm_mon % 12u]);
                break;
            case 'p':
                used = sx_append_text(buffer, capacity, used, value->tm_hour < 12 ? "AM" : "PM");
                break;
            case 'Z':
                used = sx_append_text(buffer, capacity, used, "UTC");
                break;
            case 'z':
                used = sx_append_text(buffer, capacity, used, "+0000");
                break;
            case 'n':
                used = sx_append_text(buffer, capacity, used, "\n");
                break;
            case 't':
                used = sx_append_text(buffer, capacity, used, "\t");
                break;
            case 'F':
                used = sx_append_number(buffer, capacity, used, value->tm_year + 1900L, 0, '0');
                used = sx_append_text(buffer, capacity, used, "-");
                used = sx_append_number(buffer, capacity, used, value->tm_mon + 1L, 2, '0');
                used = sx_append_text(buffer, capacity, used, "-");
                used = sx_append_number(buffer, capacity, used, value->tm_mday, 2, '0');
                break;
            case 'T':
                used = sx_append_number(buffer, capacity, used, value->tm_hour, 2, '0');
                used = sx_append_text(buffer, capacity, used, ":");
                used = sx_append_number(buffer, capacity, used, value->tm_min, 2, '0');
                used = sx_append_text(buffer, capacity, used, ":");
                used = sx_append_number(buffer, capacity, used, value->tm_sec, 2, '0');
                break;
            case '%':
                if (used + 1 < capacity) {
                    buffer[used] = '%';
                }
                ++used;
                break;
            case 0:
                --format;
                break;
            default:
                /* Especificador desconocido: se copia crudo, como hace glibc. */
                if (used + 1 < capacity) {
                    buffer[used] = '%';
                }
                ++used;
                if (used + 1 < capacity) {
                    buffer[used] = *format;
                }
                ++used;
                break;
        }
    }

    buffer[used < capacity ? used : capacity - 1] = 0;
    /* strftime devuelve 0 si no entro: el contenido queda indefinido. */
    return used < capacity ? used : 0;
}

clock_t sx_clock(void) {
    return (clock_t)uptime_ms();
}

/* Devolvia el uptime, no la epoca Unix, teniendo el RTC del kernel a mano:
 * cualquier fecha calculada por una app daba 1970. Si el RTC no valida se cae
 * al uptime, que es lo que habia antes. */
time_t sx_time(time_t* out_value) {
    struct savanxp_realtime now = {0};
    time_t value = 0;

    if (realtime(&now) >= 0 && now.valid != 0) {
        struct tm fields = {0};
        fields.tm_year = (int)now.year - 1900;
        fields.tm_mon = (int)now.month - 1;
        fields.tm_mday = (int)now.day;
        fields.tm_hour = (int)now.hour;
        fields.tm_min = (int)now.minute;
        fields.tm_sec = (int)now.second;
        value = sx_timegm(&fields);
    } else {
        value = (time_t)(uptime_ms() / 1000UL);
    }

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
    long result = savanxp_socket((unsigned long)domain, (unsigned long)type, (unsigned long)protocol);
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
    result = savanxp_bind(fd, &raw);
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
    result = savanxp_connect(fd, &raw, state != 0 ? state->send_timeout_ms : 5000u);
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
        result = savanxp_sendto(fd, buffer, count, &raw);
    } else {
        result = savanxp_write(fd, buffer, count);
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
    result = savanxp_recvfrom(fd, buffer, count, address != 0 ? &raw : 0, state != 0 ? state->recv_timeout_ms : 0u);
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
    long result = savanxp_kill((int)pid, signal_number);
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

/* Nombres estandar como alias debiles de la implementacion sx_*.
 *
 * Los headers ya no renombran con #define: declaran el nombre estandar, y
 * el simbolo tiene que existir de verdad para que linkee codigo de terceros
 * que solo conoce <string.h> y compania. Debiles para que una app que traiga
 * su propia version gane el link en vez de chocar. */
__attribute__((weak, alias("sx_isspace"))) int isspace(int character);
__attribute__((weak, alias("sx_isprint"))) int isprint(int character);
__attribute__((weak, alias("sx_isdigit"))) int isdigit(int character);
__attribute__((weak, alias("sx_isalpha"))) int isalpha(int character);
__attribute__((weak, alias("sx_isalnum"))) int isalnum(int character);
__attribute__((weak, alias("sx_islower"))) int islower(int character);
__attribute__((weak, alias("sx_isupper"))) int isupper(int character);
__attribute__((weak, alias("sx_isxdigit"))) int isxdigit(int character);
__attribute__((weak, alias("sx_tolower"))) int tolower(int character);
__attribute__((weak, alias("sx_toupper"))) int toupper(int character);
__attribute__((weak, alias("sx_opendir"))) DIR* opendir(const char* path);
__attribute__((weak, alias("sx_readdir"))) struct dirent* readdir(DIR* directory);
__attribute__((weak, alias("sx_closedir"))) int closedir(DIR* directory);
__attribute__((weak, alias("sx_rewinddir"))) void rewinddir(DIR* directory);
__attribute__((weak, alias("sx_open"))) int open(const char* path, int flags, ...);
__attribute__((weak, alias("sx_fcntl"))) int fcntl(int fd, int command, ...);
__attribute__((weak, alias("sx_fnmatch"))) int fnmatch(const char* pattern, const char* string, int flags);
__attribute__((weak, alias("sx_poll"))) int poll(struct pollfd* fds, unsigned long count, int timeout_ms);
__attribute__((weak, alias("sx_getpwnam"))) struct passwd* getpwnam(const char* name);
__attribute__((weak, alias("sx_kill"))) int kill(pid_t pid, int signal_number);
__attribute__((weak, alias("sx_signal"))) sighandler_t signal(int signal_number, sighandler_t handler);
__attribute__((weak, alias("sx_sigaction"))) int sigaction(int signal_number, const struct sigaction* action, struct sigaction* old_action);
__attribute__((weak, alias("sx_sigemptyset"))) int sigemptyset(sigset_t* set);
__attribute__((weak, alias("sx_sigfillset"))) int sigfillset(sigset_t* set);
__attribute__((weak, alias("sx_sigaddset"))) int sigaddset(sigset_t* set, int signal_number);
__attribute__((weak, alias("sx_sigdelset"))) int sigdelset(sigset_t* set, int signal_number);
__attribute__((weak, alias("sx_sigismember"))) int sigismember(const sigset_t* set, int signal_number);
__attribute__((weak, alias("sx_sigprocmask"))) int sigprocmask(int how, const sigset_t* set, sigset_t* old_set);
__attribute__((weak, alias("sx_sigsuspend"))) int sigsuspend(const sigset_t* mask);
__attribute__((weak, alias("sx_raise"))) int raise(int signal_number);
__attribute__((weak, alias("sx_strsignal"))) char* strsignal(int signal_number);
__attribute__((weak, alias("sx_fopen"))) FILE* fopen(const char* path, const char* mode);
__attribute__((weak, alias("sx_fclose"))) int fclose(FILE* stream);
__attribute__((weak, alias("sx_fread"))) size_t fread(void* buffer, size_t size, size_t count, FILE* stream);
__attribute__((weak, alias("sx_fwrite"))) size_t fwrite(const void* buffer, size_t size, size_t count, FILE* stream);
__attribute__((weak, alias("sx_fseek"))) int fseek(FILE* stream, long offset, int whence);
__attribute__((weak, alias("sx_ftell"))) long ftell(FILE* stream);
__attribute__((weak, alias("sx_fflush"))) int fflush(FILE* stream);
__attribute__((weak, alias("sx_fprintf"))) int fprintf(FILE* stream, const char* format, ...);
__attribute__((weak, alias("sx_vfprintf"))) int vfprintf(FILE* stream, const char* format, va_list args);
__attribute__((weak, alias("sx_sprintf"))) int sprintf(char* buffer, const char* format, ...);
__attribute__((weak, alias("sx_vprintf"))) int vprintf(const char* format, va_list args);
__attribute__((weak, alias("sx_snprintf"))) int snprintf(char* buffer, size_t size, const char* format, ...);
__attribute__((weak, alias("sx_vsnprintf"))) int vsnprintf(char* buffer, size_t size, const char* format, va_list args);
__attribute__((weak, alias("sx_fgets"))) char* fgets(char* buffer, int size, FILE* stream);
__attribute__((weak, alias("sx_feof"))) int feof(FILE* stream);
__attribute__((weak, alias("sx_ferror"))) int ferror(FILE* stream);
__attribute__((weak, alias("sx_clearerr"))) void clearerr(FILE* stream);
__attribute__((weak, alias("sx_fputs"))) int fputs(const char* text, FILE* stream);
__attribute__((weak, alias("sx_putc"))) int putc(int character, FILE* stream);
__attribute__((weak, alias("sx_remove"))) int remove(const char* path);
__attribute__((weak, alias("sx_rename"))) int rename(const char* old_path, const char* new_path);
__attribute__((weak, alias("sx_malloc"))) void* malloc(size_t size);
__attribute__((weak, alias("sx_calloc"))) void* calloc(size_t count, size_t size);
__attribute__((weak, alias("sx_realloc"))) void* realloc(void* pointer, size_t size);
__attribute__((weak, alias("sx_free"))) void free(void* pointer);
__attribute__((weak, alias("sx_atoi"))) int atoi(const char* text);
__attribute__((weak, alias("sx_atof"))) double atof(const char* text);
__attribute__((weak, alias("sx_abs"))) int abs(int value);
__attribute__((weak, alias("sx_getenv"))) char* getenv(const char* name);
__attribute__((weak, alias("sx_system"))) int system(const char* command);
__attribute__((weak, alias("sx_abort"))) void abort(void);
__attribute__((weak, alias("sx_strtol"))) long strtol(const char* text, char** endptr, int base);
__attribute__((weak, alias("sx_strtoul"))) unsigned long strtoul(const char* text, char** endptr, int base);
__attribute__((weak, alias("sx_bsearch"))) void* bsearch(const void* key, const void* base, size_t count, size_t size, int (*compar)(const void*, const void*));
__attribute__((weak, alias("sx_qsort"))) void qsort(void* base, size_t count, size_t size, int (*compar)(const void*, const void*));
__attribute__((weak, alias("sx_memcmp"))) int memcmp(const void* left, const void* right, size_t count);
__attribute__((weak, alias("sx_memmove"))) void* memmove(void* destination, const void* source, size_t count);
__attribute__((weak, alias("sx_strncpy"))) char* strncpy(char* destination, const char* source, size_t count);
__attribute__((weak, alias("sx_strchr"))) char* strchr(const char* text, int character);
__attribute__((weak, alias("sx_strchrnul"))) char* strchrnul(const char* text, int character);
__attribute__((weak, alias("sx_strpbrk"))) char* strpbrk(const char* text, const char* accept);
__attribute__((weak, alias("sx_strrchr"))) char* strrchr(const char* text, int character);
__attribute__((weak, alias("sx_strstr"))) char* strstr(const char* haystack, const char* needle);
__attribute__((weak, alias("sx_strcspn"))) size_t strcspn(const char* text, const char* reject);
__attribute__((weak, alias("sx_strspn"))) size_t strspn(const char* text, const char* accept);
__attribute__((weak, alias("sx_strerror"))) char* strerror(int error_number);
__attribute__((weak, alias("sx_strtok_r"))) char* strtok_r(char* text, const char* delimiters, char** save_ptr);
__attribute__((weak, alias("sx_stpncpy"))) char* stpncpy(char* destination, const char* source, size_t count);
__attribute__((weak, alias("sx_strdup"))) char* strdup(const char* text);
__attribute__((weak, alias("sx_mempcpy"))) void* mempcpy(void* destination, const void* source, size_t count);
__attribute__((weak, alias("sx_strcasecmp"))) int strcasecmp(const char* left, const char* right);
__attribute__((weak, alias("sx_strncasecmp"))) int strncasecmp(const char* left, const char* right, unsigned long count);
__attribute__((weak, alias("sx_tcgetattr"))) int tcgetattr(int fd, struct termios* value);
__attribute__((weak, alias("sx_tcsetattr"))) int tcsetattr(int fd, int optional_actions, const struct termios* value);
__attribute__((weak, alias("sx_tcgetpgrp"))) pid_t tcgetpgrp(int fd);
__attribute__((weak, alias("sx_tcsetpgrp"))) int tcsetpgrp(int fd, pid_t pgrp);
__attribute__((weak, alias("sx_time"))) time_t time(time_t* out_value);
__attribute__((weak, alias("sx_nanosleep"))) int nanosleep(const struct timespec* request, struct timespec* remaining);
__attribute__((weak, alias("sx_clock_gettime"))) int clock_gettime(int clock_id, struct timespec* value);
__attribute__((weak, alias("sx_read"))) ssize_t read(int fd, void* buffer, size_t count);
__attribute__((weak, alias("sx_write"))) ssize_t write(int fd, const void* buffer, size_t count);
__attribute__((weak, alias("sx_close"))) int close(int fd);
__attribute__((weak, alias("sx_lseek"))) off_t lseek(int fd, off_t offset, int whence);
__attribute__((weak, alias("sx_unlink"))) int unlink(const char* path);
__attribute__((weak, alias("sx_rmdir"))) int rmdir(const char* path);
__attribute__((weak, alias("sx_access"))) int access(const char* path, int mode);
__attribute__((weak, alias("sx_isatty"))) int isatty(int fd);
__attribute__((weak, alias("sx_chdir"))) int chdir(const char* path);
__attribute__((weak, alias("sx_getcwd"))) char* getcwd(char* buffer, size_t size);
__attribute__((weak, alias("sx_getpid"))) pid_t getpid(void);
__attribute__((weak, alias("sx_getppid"))) pid_t getppid(void);
__attribute__((weak, alias("sx_getpgrp"))) pid_t getpgrp(void);
__attribute__((weak, alias("sx_setpgid"))) int setpgid(pid_t pid, pid_t pgrp);
__attribute__((weak, alias("sx_setsid"))) pid_t setsid(void);
__attribute__((weak, alias("sx_sync"))) int sync(void);
__attribute__((weak, alias("sx_dup"))) int dup(int fd);
__attribute__((weak, alias("sx_dup2"))) int dup2(int oldfd, int newfd);
__attribute__((weak, alias("sx_pipe"))) int pipe(int fds[2]);
__attribute__((weak, alias("sx_fork"))) pid_t fork(void);
__attribute__((weak, alias("sx_vfork"))) pid_t vfork(void);
__attribute__((weak, alias("sx_execv"))) int execv(const char* path, char* const argv[]);
__attribute__((weak, alias("sx_execve"))) int execve(const char* path, char* const argv[], char* const envp[]);
__attribute__((weak, alias("sx_execvp"))) int execvp(const char* file, char* const argv[]);
__attribute__((weak, alias("sx__exit"))) void _exit(int code) __attribute__((noreturn));
__attribute__((weak, alias("sx_getuid"))) uid_t getuid(void);
__attribute__((weak, alias("sx_geteuid"))) uid_t geteuid(void);
__attribute__((weak, alias("sx_getgid"))) gid_t getgid(void);
__attribute__((weak, alias("sx_getegid"))) gid_t getegid(void);
__attribute__((weak, alias("sx_umask"))) mode_t umask(mode_t mask);
__attribute__((weak, alias("sx_sleep"))) unsigned sleep(unsigned seconds);
__attribute__((weak, alias("sx_usleep"))) int usleep(unsigned long microseconds);
__attribute__((weak, alias("sx_htonl"))) unsigned long htonl(unsigned long value);
__attribute__((weak, alias("sx_htons"))) unsigned short htons(unsigned short value);
__attribute__((weak, alias("sx_ntohl"))) unsigned long ntohl(unsigned long value);
__attribute__((weak, alias("sx_ntohs"))) unsigned short ntohs(unsigned short value);
__attribute__((weak, alias("sx_inet_addr"))) in_addr_t inet_addr(const char* text);
__attribute__((weak, alias("sx_inet_ntoa"))) char* inet_ntoa(struct in_addr address);
__attribute__((weak, alias("sx_inet_pton"))) int inet_pton(int family, const char* source, void* destination);
__attribute__((weak, alias("sx_inet_ntop"))) const char* inet_ntop(int family, const void* source, char* destination, unsigned long size);
__attribute__((weak, alias("sx_mmap"))) void* mmap(void* address, size_t length, int prot, int flags, int fd, off_t offset);
__attribute__((weak, alias("sx_munmap"))) int munmap(void* address, size_t length);
__attribute__((weak, alias("sx_select"))) int select(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, struct timeval* timeout);
__attribute__((weak, alias("sx_socket"))) int socket(int domain, int type, int protocol);
__attribute__((weak, alias("sx_bind"))) int bind(int fd, const struct sockaddr* address, socklen_t address_length);
__attribute__((weak, alias("sx_connect"))) int connect(int fd, const struct sockaddr* address, socklen_t address_length);
__attribute__((weak, alias("sx_sendto"))) ssize_t sendto(int fd, const void* buffer, size_t count, int flags, const struct sockaddr* address, socklen_t address_length);
__attribute__((weak, alias("sx_recvfrom"))) ssize_t recvfrom(int fd, void* buffer, size_t count, int flags, struct sockaddr* address, socklen_t* address_length);
__attribute__((weak, alias("sx_setsockopt"))) int setsockopt(int fd, int level, int option_name, const void* option_value, socklen_t option_length);
__attribute__((weak, alias("sx_getsockopt"))) int getsockopt(int fd, int level, int option_name, void* option_value, socklen_t* option_length);
__attribute__((weak, alias("sx_shutdown"))) int shutdown(int fd, int how);
__attribute__((weak, alias("sx_gettimeofday"))) int gettimeofday(struct timeval* value, void* timezone_ptr);
__attribute__((weak, alias("sx_times"))) clock_t times(struct tms* buffer);
__attribute__((weak, alias("sx_uname"))) int uname(struct utsname* value);
__attribute__((weak, alias("sx_waitpid"))) pid_t waitpid(pid_t pid, int* status, int options);
__attribute__((weak, alias("sx_stat"))) int stat(const char* path, struct stat* info);
__attribute__((weak, alias("sx_fstat"))) int fstat(int fd, struct stat* info);
__attribute__((weak, alias("sx_lstat"))) int lstat(const char* path, struct stat* info);
__attribute__((weak, alias("sx_memchr"))) void* memchr(const void* block, int value, size_t count);
__attribute__((weak, alias("sx_strnlen"))) size_t strnlen(const char* text, size_t limit);
__attribute__((weak, alias("sx_strcat"))) char* strcat(char* destination, const char* source);
__attribute__((weak, alias("sx_strncat"))) char* strncat(char* destination, const char* source, size_t count);
__attribute__((weak, alias("sx_strtok"))) char* strtok(char* text, const char* delimiters);
__attribute__((weak, alias("sx_strsep"))) char* strsep(char** text, const char* delimiters);
__attribute__((weak, alias("sx_strcasestr"))) char* strcasestr(const char* haystack, const char* needle);
__attribute__((weak, alias("sx_strtoll"))) long long strtoll(const char* text, char** endptr, int base);
__attribute__((weak, alias("sx_strtoull"))) unsigned long long strtoull(const char* text, char** endptr, int base);
__attribute__((weak, alias("sx_atol"))) long atol(const char* text);
__attribute__((weak, alias("sx_atoll"))) long long atoll(const char* text);
__attribute__((weak, alias("sx_labs"))) long labs(long value);
__attribute__((weak, alias("sx_llabs"))) long long llabs(long long value);
__attribute__((weak, alias("sx_div"))) div_t div(int numerator, int denominator);
__attribute__((weak, alias("sx_ldiv"))) ldiv_t ldiv(long numerator, long denominator);
__attribute__((weak, alias("sx_lldiv"))) lldiv_t lldiv(long long numerator, long long denominator);
__attribute__((weak, alias("sx_posix_memalign"))) int posix_memalign(void** out_pointer, size_t alignment, size_t size);
__attribute__((weak, alias("sx_aligned_alloc"))) void* aligned_alloc(size_t alignment, size_t size);
__attribute__((weak, alias("sx_memalign"))) void* memalign(size_t alignment, size_t size);
__attribute__((weak, alias("sx_fgetc"))) int fgetc(FILE* stream);
__attribute__((weak, alias("sx_getc"))) int getc(FILE* stream);
__attribute__((weak, alias("sx_getchar"))) int getchar(void);
__attribute__((weak, alias("sx_fputc"))) int fputc(int character, FILE* stream);
__attribute__((weak, alias("sx_ungetc"))) int ungetc(int character, FILE* stream);
__attribute__((weak, alias("sx_fileno"))) int fileno(FILE* stream);
__attribute__((weak, alias("sx_fdopen"))) FILE* fdopen(int fd, const char* mode);
__attribute__((weak, alias("sx_setvbuf"))) int setvbuf(FILE* stream, char* buffer, int mode, size_t size);
__attribute__((weak, alias("sx_setbuf"))) void setbuf(FILE* stream, char* buffer);
__attribute__((weak, alias("sx_rewind"))) void rewind(FILE* stream);
__attribute__((weak, alias("sx_vsprintf"))) int vsprintf(char* buffer, const char* format, va_list args);
__attribute__((weak, alias("sx_perror"))) void perror(const char* prefix);
__attribute__((weak, alias("sx_fseeko"))) int fseeko(FILE* stream, off_t offset, int whence);
__attribute__((weak, alias("sx_ftello"))) off_t ftello(FILE* stream);
__attribute__((weak, alias("sx_gmtime_r"))) struct tm* gmtime_r(const time_t* value, struct tm* result);
__attribute__((weak, alias("sx_gmtime"))) struct tm* gmtime(const time_t* value);
__attribute__((weak, alias("sx_localtime_r"))) struct tm* localtime_r(const time_t* value, struct tm* result);
__attribute__((weak, alias("sx_localtime"))) struct tm* localtime(const time_t* value);
__attribute__((weak, alias("sx_timegm"))) time_t timegm(struct tm* value);
__attribute__((weak, alias("sx_mktime"))) time_t mktime(struct tm* value);
__attribute__((weak, alias("sx_strftime"))) size_t strftime(char* buffer, size_t capacity, const char* format, const struct tm* value);
__attribute__((weak, alias("sx_clock"))) clock_t clock(void);
#if defined(__SSE2__)
__attribute__((weak, alias("sx_strtod"))) double strtod(const char* text, char** endptr);
__attribute__((weak, alias("sx_strtof"))) float strtof(const char* text, char** endptr);
__attribute__((weak, alias("sx_difftime"))) double difftime(time_t later, time_t earlier);
#endif
__attribute__((weak, alias("sx_sscanf"))) int sscanf(const char* input, const char* format, ...);
__attribute__((weak, alias("sx_vsscanf"))) int vsscanf(const char* input, const char* format, va_list args);
__attribute__((weak, alias("sx_imaxabs"))) intmax_t imaxabs(intmax_t value);
__attribute__((weak, alias("sx_imaxdiv"))) imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator);
__attribute__((weak, alias("sx_strtoimax"))) intmax_t strtoimax(const char* text, char** endptr, int base);
__attribute__((weak, alias("sx_strtoumax"))) uintmax_t strtoumax(const char* text, char** endptr, int base);
__attribute__((weak, alias("sx_mkdir"))) int mkdir(const char* path, mode_t mode);
