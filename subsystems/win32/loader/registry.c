/*
 * registry.c -- HKEY_CURRENT_USER in memory. See registry.h.
 *
 * Key 0 is the root of HKEY_CURRENT_USER (path ""). A handle the guest sees
 * is 0x100 + 4 * slot; the predefined keys keep Windows' values.
 */
#include "registry.h"

#include "sxw_abi.h"

#define HANDLE_BASE 0x100u
#define ROOT_EMPTY 0x10000u /* a slot holding HKCR or HKLM: no keys under it */
#define HEADER "SavanXP registry 1"

static char fold(char c) {
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

static int same(const char *a, const char *b) {
    while (*a != 0 && fold(*a) == fold(*b)) {
        ++a;
        ++b;
    }
    return fold(*a) == fold(*b);
}

static uint32_t length_of(const char *s) {
    uint32_t n = 0;
    while (s[n] != 0) {
        ++n;
    }
    return n;
}

static void copy_bytes(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) {
        dst[i] = src[i];
    }
}

void sxw_reg_init(sxw_registry *reg) {
    uint8_t *p = (uint8_t *)reg;
    for (size_t i = 0; i < sizeof(*reg); ++i) {
        p[i] = 0;
    }
    reg->keys[0].used = 1; /* the root, path "" */
}

/* ---- Keys ------------------------------------------------------------------ */

static int find_key(const sxw_registry *reg, const char *path) {
    for (uint32_t i = 0; i < SXW_REG_MAX_KEYS; ++i) {
        if (reg->keys[i].used && same(reg->keys[i].path, path)) {
            return (int)i;
        }
    }
    return -1;
}

static int add_key(sxw_registry *reg, const char *path) {
    for (uint32_t i = 1; i < SXW_REG_MAX_KEYS; ++i) {
        if (!reg->keys[i].used) {
            uint32_t n = length_of(path);
            if (n >= SXW_REG_MAX_PATH) {
                return -1;
            }
            copy_bytes((uint8_t *)reg->keys[i].path, (const uint8_t *)path, n + 1u);
            reg->keys[i].used = 1;
            reg->dirty = 1;
            return (int)i;
        }
    }
    return -1;
}

/* parent path + "\\" + subkey, empty components dropped. 0, or -1 when too
 * long. */
static int join_path(const char *parent, const char *subkey, char *out) {
    uint32_t n = 0;
    for (const char *p = parent; *p != 0; ++p) {
        out[n++] = *p;
    }
    const char *s = subkey;
    while (*s != 0) {
        while (*s == '\\') {
            ++s;
        }
        if (*s == 0) {
            break;
        }
        if (n != 0) {
            if (n + 1u >= SXW_REG_MAX_PATH) {
                return -1;
            }
            out[n++] = '\\';
        }
        while (*s != 0 && *s != '\\') {
            if (n + 1u >= SXW_REG_MAX_PATH) {
                return -1;
            }
            out[n++] = *s++;
        }
    }
    out[n] = 0;
    return 0;
}

/* The key a handle names: its index, ROOT_EMPTY for HKCR/HKLM, or -1. */
static int key_of(const sxw_registry *reg, uint32_t hkey) {
    if (hkey == SXW_HKEY_CURRENT_USER) {
        return 0;
    }
    if (hkey == SXW_HKEY_CLASSES_ROOT || hkey == SXW_HKEY_LOCAL_MACHINE) {
        return (int)ROOT_EMPTY;
    }
    if (hkey < HANDLE_BASE || (hkey - HANDLE_BASE) % 4u != 0u ||
        (hkey - HANDLE_BASE) / 4u >= SXW_REG_MAX_HANDLES) {
        return -1;
    }
    uint32_t slot = reg->handles[(hkey - HANDLE_BASE) / 4u];
    if (slot == 0u) {
        return -1;
    }
    return slot == ROOT_EMPTY ? (int)ROOT_EMPTY : (int)(slot - 1u);
}

static uint32_t new_handle(sxw_registry *reg, uint32_t slot_value, uint32_t *result) {
    for (uint32_t i = 0; i < SXW_REG_MAX_HANDLES; ++i) {
        if (reg->handles[i] == 0u) {
            reg->handles[i] = slot_value;
            *result = HANDLE_BASE + 4u * i;
            return SXW_ERROR_SUCCESS;
        }
    }
    return SXW_ERROR_NOT_ENOUGH_MEMORY;
}

uint32_t sxw_reg_open(sxw_registry *reg, uint32_t hkey, const char *subkey, int create,
                      uint32_t *result, uint32_t *disposition) {
    int parent = key_of(reg, hkey);
    if (parent < 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    const int empty_subkey = subkey == 0 || subkey[0] == 0;
    if (parent == (int)ROOT_EMPTY) {
        if (empty_subkey) {
            if (disposition != 0) {
                *disposition = SXW_REG_OPENED_EXISTING_KEY;
            }
            return new_handle(reg, ROOT_EMPTY, result);
        }
        return create ? SXW_ERROR_ACCESS_DENIED : SXW_ERROR_FILE_NOT_FOUND;
    }
    char path[SXW_REG_MAX_PATH];
    if (join_path(reg->keys[parent].path, empty_subkey ? "" : subkey, path) != 0) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    int key = find_key(reg, path);
    uint32_t outcome = SXW_REG_OPENED_EXISTING_KEY;
    if (key < 0) {
        if (!create) {
            return SXW_ERROR_FILE_NOT_FOUND;
        }
        /* Every missing key on the way is created, as Windows does. */
        char prefix[SXW_REG_MAX_PATH];
        uint32_t n = 0;
        for (uint32_t i = 0;; ++i) {
            if (path[i] == '\\' || path[i] == 0) {
                prefix[n] = 0;
                if (find_key(reg, prefix) < 0 && add_key(reg, prefix) < 0) {
                    return SXW_ERROR_NOT_ENOUGH_MEMORY;
                }
                if (path[i] == 0) {
                    break;
                }
            }
            prefix[n++] = path[i];
        }
        key = find_key(reg, path);
        outcome = SXW_REG_CREATED_NEW_KEY;
    }
    uint32_t error = new_handle(reg, (uint32_t)key + 1u, result);
    if (error == SXW_ERROR_SUCCESS && disposition != 0) {
        *disposition = outcome;
    }
    return error;
}

uint32_t sxw_reg_close(sxw_registry *reg, uint32_t hkey) {
    if (hkey == SXW_HKEY_CURRENT_USER || hkey == SXW_HKEY_CLASSES_ROOT ||
        hkey == SXW_HKEY_LOCAL_MACHINE) {
        return SXW_ERROR_SUCCESS;
    }
    if (key_of(reg, hkey) < 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    reg->handles[(hkey - HANDLE_BASE) / 4u] = 0u;
    return SXW_ERROR_SUCCESS;
}

/* ---- Values ----------------------------------------------------------------- */

static int find_value(const sxw_registry *reg, int key, const char *name) {
    for (uint32_t i = 0; i < SXW_REG_MAX_VALUES; ++i) {
        const sxw_reg_value *v = &reg->values[i];
        if (v->used && v->key == (uint8_t)key && same(v->name, name)) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t sxw_reg_query(const sxw_registry *reg, uint32_t hkey, const char *name, uint32_t *type,
                       uint8_t *data, uint32_t capacity, uint32_t *size_out) {
    int key = key_of(reg, hkey);
    if (key < 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    if (key == (int)ROOT_EMPTY) {
        return SXW_ERROR_FILE_NOT_FOUND;
    }
    int index = find_value(reg, key, name != 0 ? name : "");
    if (index < 0) {
        return SXW_ERROR_FILE_NOT_FOUND;
    }
    const sxw_reg_value *v = &reg->values[index];
    if (type != 0) {
        *type = v->type;
    }
    if (size_out != 0) {
        *size_out = v->size;
    }
    if (data == 0) {
        return SXW_ERROR_SUCCESS;
    }
    if (capacity < v->size) {
        return SXW_ERROR_MORE_DATA;
    }
    copy_bytes(data, reg->data + v->offset, v->size);
    return SXW_ERROR_SUCCESS;
}

/* Moves every value's data down over the gaps replaced values left. */
static void compact(sxw_registry *reg) {
    uint32_t cursor = 0;
    for (;;) {
        int next = -1;
        for (uint32_t i = 0; i < SXW_REG_MAX_VALUES; ++i) {
            const sxw_reg_value *v = &reg->values[i];
            if (v->used && v->offset >= cursor && v->size != 0u &&
                (next < 0 || v->offset < reg->values[next].offset)) {
                next = (int)i;
            }
        }
        if (next < 0) {
            break;
        }
        sxw_reg_value *v = &reg->values[next];
        for (uint32_t b = 0; b < v->size; ++b) {
            reg->data[cursor + b] = reg->data[v->offset + b];
        }
        v->offset = cursor;
        cursor += v->size;
    }
    reg->data_used = cursor;
}

static int store_value(sxw_registry *reg, int key, const char *name, uint32_t type, const uint8_t *data,
                       uint32_t size) {
    uint32_t n = length_of(name);
    if (n >= SXW_REG_MAX_NAME) {
        return -1;
    }
    int index = find_value(reg, key, name);
    /* Room first, so a refusal changes nothing: every other value's bytes
     * plus the new ones must fit. */
    uint32_t live = 0;
    for (uint32_t i = 0; i < SXW_REG_MAX_VALUES; ++i) {
        if (reg->values[i].used && (int)i != index) {
            live += reg->values[i].size;
        }
    }
    if (live + size > SXW_REG_DATA_BYTES) {
        return -1;
    }
    if (index < 0) {
        for (uint32_t i = 0; i < SXW_REG_MAX_VALUES && index < 0; ++i) {
            if (!reg->values[i].used) {
                index = (int)i;
            }
        }
        if (index < 0) {
            return -1;
        }
        sxw_reg_value *v = &reg->values[index];
        v->used = 1;
        v->key = (uint8_t)key;
        v->size = 0;
        v->offset = 0;
        copy_bytes((uint8_t *)v->name, (const uint8_t *)name, n + 1u);
    }
    sxw_reg_value *v = &reg->values[index];
    if (size > v->size) {
        v->size = 0; /* its old bytes become a gap */
        if (reg->data_used + size > SXW_REG_DATA_BYTES) {
            compact(reg);
        }
        v->offset = reg->data_used;
        reg->data_used += size;
    }
    copy_bytes(reg->data + v->offset, data, size);
    v->size = size;
    v->type = type;
    reg->dirty = 1;
    return 0;
}

uint32_t sxw_reg_set(sxw_registry *reg, uint32_t hkey, const char *name, uint32_t type,
                     const uint8_t *data, uint32_t size) {
    int key = key_of(reg, hkey);
    if (key < 0) {
        return SXW_ERROR_INVALID_HANDLE;
    }
    if (key == (int)ROOT_EMPTY) {
        return SXW_ERROR_ACCESS_DENIED;
    }
    if (size > SXW_REG_DATA_BYTES || (size != 0u && data == 0)) {
        return SXW_ERROR_INVALID_PARAMETER;
    }
    return store_value(reg, key, name != 0 ? name : "", type, data, size) == 0
               ? SXW_ERROR_SUCCESS
               : SXW_ERROR_NOT_ENOUGH_MEMORY;
}

/* ---- The file ---------------------------------------------------------------- */

typedef struct writer {
    char *out;
    size_t capacity;
    size_t length;
    int overflow;
} writer;

static void w_char(writer *w, char c) {
    if (w->length + 1u >= w->capacity) {
        w->overflow = 1;
        return;
    }
    w->out[w->length++] = c;
}

static void w_text(writer *w, const char *s) {
    while (*s != 0) {
        w_char(w, *s++);
    }
}

static void w_hex(writer *w, uint32_t value, int digits) {
    for (int i = digits - 1; i >= 0; --i) {
        uint32_t nibble = (value >> (4 * i)) & 0xfu;
        w_char(w, (char)(nibble < 10u ? '0' + nibble : 'a' + nibble - 10u));
    }
}

int sxw_reg_serialize(const sxw_registry *reg, char *out, size_t capacity, size_t *length) {
    writer w = {out, capacity, 0, 0};
    w_text(&w, HEADER "\n");
    for (uint32_t k = 0; k < SXW_REG_MAX_KEYS; ++k) {
        if (!reg->keys[k].used) {
            continue;
        }
        int has_values = 0;
        for (uint32_t i = 0; i < SXW_REG_MAX_VALUES; ++i) {
            has_values |= reg->values[i].used && reg->values[i].key == k;
        }
        if (k == 0u && !has_values) {
            continue;
        }
        w_char(&w, '[');
        w_text(&w, reg->keys[k].path);
        w_text(&w, "]\n");
        for (uint32_t i = 0; i < SXW_REG_MAX_VALUES; ++i) {
            const sxw_reg_value *v = &reg->values[i];
            if (!v->used || v->key != k) {
                continue;
            }
            if (v->name[0] == 0) {
                w_char(&w, '@');
            } else {
                w_char(&w, '"');
                for (const char *c = v->name; *c != 0; ++c) {
                    if (*c == '"' || *c == '\\') {
                        w_char(&w, '\\');
                    }
                    w_char(&w, *c);
                }
                w_char(&w, '"');
            }
            const uint8_t *d = reg->data + v->offset;
            if (v->type == SXW_REG_DWORD && v->size == 4u) {
                w_text(&w, "=dword:");
                w_hex(&w, (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24), 8);
            } else {
                w_text(&w, "=hex(");
                w_hex(&w, v->type, 1);
                w_text(&w, "):");
                for (uint32_t b = 0; b < v->size; ++b) {
                    if (b != 0u) {
                        w_char(&w, ',');
                    }
                    w_hex(&w, d[b], 2);
                }
            }
            w_char(&w, '\n');
        }
    }
    if (w.overflow) {
        return -1;
    }
    out[w.length] = 0;
    *length = w.length;
    return 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* One value line, from name to the end of the line. 0 or -1. */
static int parse_value(sxw_registry *reg, int key, const char *s, const char *end) {
    char name[SXW_REG_MAX_NAME];
    uint32_t n = 0;
    if (s < end && *s == '@') {
        ++s;
    } else if (s < end && *s == '"') {
        ++s;
        while (s < end && *s != '"') {
            if (*s == '\\' && s + 1 < end) {
                ++s;
            }
            if (n + 1u >= SXW_REG_MAX_NAME) {
                return -1;
            }
            name[n++] = *s++;
        }
        if (s >= end) {
            return -1;
        }
        ++s;
    } else {
        return -1;
    }
    name[n] = 0;
    if (s >= end || *s++ != '=') {
        return -1;
    }
    static const char dword[] = "dword:";
    static const char hex[] = "hex(";
    uint8_t bytes[SXW_REG_DATA_BYTES < 4096u ? SXW_REG_DATA_BYTES : 4096u];
    uint32_t size = 0;
    uint32_t type;
    uint32_t i = 0;
    while (dword[i] != 0 && s + i < end && s[i] == dword[i]) {
        ++i;
    }
    if (dword[i] == 0) {
        s += i;
        uint32_t value = 0;
        for (int d = 0; d < 8; ++d) {
            int digit = s < end ? hex_digit(*s++) : -1;
            if (digit < 0) {
                return -1;
            }
            value = (value << 4) | (uint32_t)digit;
        }
        type = SXW_REG_DWORD;
        bytes[0] = (uint8_t)value;
        bytes[1] = (uint8_t)(value >> 8);
        bytes[2] = (uint8_t)(value >> 16);
        bytes[3] = (uint8_t)(value >> 24);
        size = 4;
    } else {
        i = 0;
        while (hex[i] != 0 && s + i < end && s[i] == hex[i]) {
            ++i;
        }
        if (hex[i] != 0) {
            return -1;
        }
        s += i;
        type = 0;
        int digits = 0;
        while (s < end && hex_digit(*s) >= 0 && digits < 8) {
            type = (type << 4) | (uint32_t)hex_digit(*s++);
            ++digits;
        }
        if (digits == 0 || s + 1 >= end || s[0] != ')' || s[1] != ':') {
            return -1;
        }
        s += 2;
        while (s < end) {
            int hi = hex_digit(*s);
            int lo = s + 1 < end ? hex_digit(s[1]) : -1;
            if (hi < 0 || lo < 0 || size == sizeof(bytes)) {
                return -1;
            }
            bytes[size++] = (uint8_t)((hi << 4) | lo);
            s += 2;
            if (s < end) {
                if (*s != ',') {
                    return -1;
                }
                ++s;
            }
        }
    }
    if (s != end) {
        return -1;
    }
    return store_value(reg, key, name, type, bytes, size);
}

int sxw_reg_parse(sxw_registry *reg, const char *text, size_t length) {
    sxw_reg_init(reg);
    const char *s = text;
    const char *end = text + length;
    int key = -1;
    int line = 0;
    while (s < end) {
        const char *eol = s;
        while (eol < end && *eol != '\n') {
            ++eol;
        }
        const char *last = eol;
        if (last > s && last[-1] == '\r') {
            --last;
        }
        if (line == 0) {
            const char *h = HEADER;
            const char *c = s;
            while (*h != 0 && c < last && *c == *h) {
                ++c;
                ++h;
            }
            if (*h != 0 || c != last) {
                sxw_reg_init(reg);
                return -1;
            }
        } else if (last > s && *s == '[') {
            if (last[-1] != ']' || last - s - 2 >= (long)SXW_REG_MAX_PATH) {
                sxw_reg_init(reg);
                return -1;
            }
            char path[SXW_REG_MAX_PATH];
            uint32_t n = 0;
            for (const char *c = s + 1; c < last - 1; ++c) {
                path[n++] = *c;
            }
            path[n] = 0;
            key = find_key(reg, path);
            if (key < 0) {
                key = add_key(reg, path);
            }
            if (key < 0) {
                sxw_reg_init(reg);
                return -1;
            }
        } else if (last > s) {
            if (key < 0 || parse_value(reg, key, s, last) != 0) {
                sxw_reg_init(reg);
                return -1;
            }
        }
        s = eol < end ? eol + 1 : end;
        ++line;
    }
    reg->dirty = 0;
    return 0;
}
