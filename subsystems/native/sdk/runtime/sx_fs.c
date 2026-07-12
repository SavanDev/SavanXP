/*
 * SavanXP - listado de directorios para el subsistema nativo (Fase 3).
 *
 * Envuelve las syscalls del baseline open/close/readdir/stat (que el nativo
 * delega en posix) y expone un cargador de directorio de alto nivel: lee todas
 * las entradas de un path a un cache estatico (nombre + es_dir), inserta ".."
 * si no es la raiz y ordena (directorios primero, alfabetico) como filesapp.c.
 * El codigo Haxe consulta count/name/is_dir sin tocar fds ni structs del ABI.
 * Lo usa el port de filesapp.
 */
#include "savanxp_native.h"

#include "savanxp/syscall.h"

#define FS_MAX_ENTRIES 128
#define FS_NAME_CAP 256
#define FS_PATH_CAP 512

static char g_names[FS_MAX_ENTRIES][FS_NAME_CAP];
static unsigned char g_is_dir[FS_MAX_ENTRIES];
static int g_count;

/* --- helpers de string (self-contained, sin depender de libc) --------------- */

static int fs_streq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == *b;
}

static int fs_strcmp(const char *a, const char *b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void fs_strcpy(char *dst, const char *src, unsigned long cap) {
    unsigned long i = 0;
    if (cap == 0) {
        return;
    }
    while (src[i] != '\0' && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/* Une base + "/" + name en buffer (maneja base == "/"). */
static void fs_join(const char *base, const char *name, char *buffer, unsigned long cap) {
    unsigned long pos = 0;
    unsigned long i;
    int base_is_root = base[0] == '/' && base[1] == '\0';
    if (!base_is_root) {
        for (i = 0; base[i] != '\0' && pos + 1 < cap; ++i) {
            buffer[pos++] = base[i];
        }
    }
    if (pos + 1 < cap) {
        buffer[pos++] = '/';
    }
    for (i = 0; name[i] != '\0' && pos + 1 < cap; ++i) {
        buffer[pos++] = name[i];
    }
    buffer[pos] = '\0';
}

/* --- syscalls del baseline (delegadas en posix) ----------------------------- */

static long fs_open(const char *path) {
    return sxn_syscall3(SAVANXP_SYS_OPEN, (long)path, (long)SAVANXP_OPEN_READ, 0);
}
static long fs_close(int fd) {
    return sxn_syscall1(SAVANXP_SYS_CLOSE, fd);
}
static long fs_readdir(int fd, char *buffer, unsigned long cap) {
    return sxn_syscall3(SAVANXP_SYS_READDIR, fd, (long)buffer, (long)cap);
}
static int fs_path_is_dir(const char *path) {
    struct savanxp_stat info;
    info.st_mode = 0;
    info.st_size = 0;
    if (sxn_syscall3(SAVANXP_SYS_STAT, (long)path, (long)&info, 0) != 0) {
        return 0;
    }
    return (info.st_mode & SAVANXP_S_IFMT) == SAVANXP_S_IFDIR ? 1 : 0;
}

/* Orden: directorios antes que archivos, luego alfabetico (selection sort sobre
 * [start, count), dejando ".." en el indice 0). */
static void fs_sort(int start) {
    int left;
    for (left = start; left < g_count; ++left) {
        int right;
        for (right = left + 1; right < g_count; ++right) {
            int swap = 0;
            if (g_is_dir[left] != g_is_dir[right]) {
                swap = g_is_dir[right] > g_is_dir[left];
            } else if (fs_strcmp(g_names[right], g_names[left]) < 0) {
                swap = 1;
            }
            if (swap) {
                char tmp_name[FS_NAME_CAP];
                unsigned char tmp_dir = g_is_dir[left];
                fs_strcpy(tmp_name, g_names[left], FS_NAME_CAP);
                fs_strcpy(g_names[left], g_names[right], FS_NAME_CAP);
                fs_strcpy(g_names[right], tmp_name, FS_NAME_CAP);
                g_is_dir[left] = g_is_dir[right];
                g_is_dir[right] = tmp_dir;
            }
        }
    }
}

/* Carga el directorio `path` al cache. Devuelve la cantidad de entradas o -1. */
int sxn_fs_load(const char *path) {
    long fd;
    char name[FS_NAME_CAP];
    char full[FS_PATH_CAP];
    int start;
    int is_root = path[0] == '/' && path[1] == '\0';

    g_count = 0;
    fd = fs_open(path);
    if (fd < 0) {
        return -1;
    }

    if (!is_root) {
        fs_strcpy(g_names[g_count], "..", FS_NAME_CAP);
        g_is_dir[g_count] = 1;
        g_count += 1;
    }
    start = g_count;

    while (g_count < FS_MAX_ENTRIES) {
        long result = fs_readdir((int)fd, name, sizeof(name));
        if (result <= 0 || name[0] == '\0') {
            break;
        }
        if (fs_streq(name, ".") || fs_streq(name, "..")) {
            continue;
        }
        fs_strcpy(g_names[g_count], name, FS_NAME_CAP);
        fs_join(path, name, full, sizeof(full));
        g_is_dir[g_count] = (unsigned char)fs_path_is_dir(full);
        g_count += 1;
    }
    (void)fs_close((int)fd);

    fs_sort(start);
    return g_count;
}

int sxn_fs_count(void) { return g_count; }

const char *sxn_fs_name(int index) {
    return (index >= 0 && index < g_count) ? g_names[index] : "";
}

int sxn_fs_is_dir(int index) {
    return (index >= 0 && index < g_count) ? (int)g_is_dir[index] : 0;
}

/* --- helpers de path para la navegacion (buffers estaticos; el llamador Haxe
 * copia el resultado a un std::string enseguida) -------------------------------- */

static char g_join_buf[FS_PATH_CAP];
static char g_parent_buf[FS_PATH_CAP];

const char *sxn_fs_join(const char *base, const char *name) {
    fs_join(base, name, g_join_buf, sizeof(g_join_buf));
    return g_join_buf;
}

const char *sxn_fs_parent(const char *path) {
    unsigned long len = 0;
    fs_strcpy(g_parent_buf, path, sizeof(g_parent_buf));
    while (g_parent_buf[len] != '\0') {
        ++len;
    }
    while (len > 1 && g_parent_buf[len - 1] == '/') {
        g_parent_buf[--len] = '\0';
    }
    while (len > 1 && g_parent_buf[len - 1] != '/') {
        g_parent_buf[--len] = '\0';
    }
    while (len > 1 && g_parent_buf[len - 1] == '/') {
        g_parent_buf[--len] = '\0';
    }
    if (g_parent_buf[0] == '\0') {
        g_parent_buf[0] = '/';
        g_parent_buf[1] = '\0';
    }
    return g_parent_buf;
}
