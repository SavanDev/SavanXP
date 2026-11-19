/* SxBASIC core: the builtin procedures (docs/userland/SXBASIC.md#built-in-procedures).
 * The compiler checked the argument types; each one takes its arguments off
 * the stack and pushes its result, if it has one. */
#include "sxb_vm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct args {
    struct sxb_value *v;
    int count;
};

static const char *s_data(struct args *a, int i) { return sxb_str_data(a->v[i].u.s); }
static size_t s_len(struct args *a, int i) { return sxb_str_length(a->v[i].u.s); }

static void push_int(struct sxb_vm *vm, int64_t value)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) slot->u.i = value;
}

static void push_double(struct sxb_vm *vm, double value)
{
    if (!isfinite(value)) {
        sxb_vm_raise(vm, isnan(value) ? SXB_ERR_INVALID_ARGUMENT : SXB_ERR_OVERFLOW, NULL);
        value = 0;
    }
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) slot->u.d = value;
}

static void push_string(struct sxb_vm *vm, const char *data, size_t length)
{
    struct sxb_string *string = sxb_vm_string(vm, data, length);
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) {
        slot->kind = SXB_V_STRING;
        slot->u.s = string;
    } else {
        sxb_string_release(string);
    }
}

/* Pushes an argument string itself, sharing it. */
static void push_same(struct sxb_vm *vm, struct sxb_value *value)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) {
        *slot = *value;
        sxb_value_retain(slot);
    }
}

static int64_t round_half_away(double x)
{
    return (int64_t)(x < 0 ? -floor(-x + 0.5) : floor(x + 0.5));
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

static void trim(struct sxb_vm *vm, struct args *a, int left, int right)
{
    const char *s = s_data(a, 0);
    size_t start = 0, end = s_len(a, 0);
    if (left) while (start < end && is_blank(s[start])) start++;
    if (right) while (end > start && is_blank(s[end - 1])) end--;
    if (start == 0 && end == s_len(a, 0)) push_same(vm, &a->v[0]);
    else push_string(vm, s + start, end - start);
}

static void change_case(struct sxb_vm *vm, struct args *a, int upper)
{
    size_t n = s_len(a, 0);
    char *copy = malloc(n + 1u);
    if (copy == NULL) { sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL); return; }
    const char *s = s_data(a, 0);
    for (size_t i = 0; i < n; i++) {
        char ch = s[i];
        if (upper && ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if (!upper && ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        copy[i] = ch;
    }
    push_string(vm, copy, n);
    free(copy);
}

/* The byte offset of `find` in `s` from byte `from`, or -1. */
static long find_bytes(const char *s, size_t n, const char *find, size_t m, size_t from)
{
    if (m == 0) return (long)from;
    for (size_t i = from; i + m <= n; i++) {
        if (memcmp(s + i, find, m) == 0) return (long)i;
    }
    return -1;
}

static void replace(struct sxb_vm *vm, struct args *a)
{
    const char *s = s_data(a, 0), *find = s_data(a, 1), *with = s_data(a, 2);
    size_t n = s_len(a, 0), m = s_len(a, 1), w = s_len(a, 2);
    if (m == 0 || n == 0) { push_same(vm, &a->v[0]); return; }
    size_t count = 0;
    for (long at = find_bytes(s, n, find, m, 0); at >= 0; at = find_bytes(s, n, find, m, (size_t)at + m)) count++;
    if (count == 0) { push_same(vm, &a->v[0]); return; }
    size_t total = n - count * m + count * w;
    char *out = malloc(total + 1u);
    if (out == NULL) { sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL); return; }
    size_t from = 0, used = 0;
    for (long at = find_bytes(s, n, find, m, 0); at >= 0; at = find_bytes(s, n, find, m, from)) {
        memcpy(out + used, s + from, (size_t)at - from);
        used += (size_t)at - from;
        memcpy(out + used, with, w);
        used += w;
        from = (size_t)at + m;
    }
    memcpy(out + used, s + from, n - from);
    used += n - from;
    push_string(vm, out, used);
    free(out);
}

static void push_array(struct sxb_vm *vm, struct sxb_array *array)
{
    struct sxb_value *slot = sxb_vm_push_slot(vm);
    if (slot) {
        slot->kind = SXB_V_ARRAY;
        slot->u.a = array;
    } else {
        sxb_array_release(array);
    }
}

static void split(struct sxb_vm *vm, struct args *a)
{
    const char *s = s_data(a, 0), *sep = s_data(a, 1);
    size_t n = s_len(a, 0), m = s_len(a, 1);
    if (n == 0) { push_array(vm, NULL); return; }
    int64_t count = 1;
    if (m > 0) {
        for (long at = find_bytes(s, n, sep, m, 0); at >= 0; at = find_bytes(s, n, sep, m, (size_t)at + m)) count++;
    }
    if (count > SXB_MAX_ARRAY) { sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL); return; }
    struct sxb_array *array = sxb_array_new(SXB_TY_STRING, count);
    if (array == NULL) { sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL); return; }
    size_t from = 0;
    for (int64_t i = 0; i < count; i++) {
        long at = (m > 0 && i + 1 < count) ? find_bytes(s, n, sep, m, from) : (long)n;
        array->items[i].u.s = sxb_vm_string(vm, s + from, (size_t)at - from);
        from = (size_t)at + m;
    }
    push_array(vm, array);
}

static void join(struct sxb_vm *vm, struct args *a)
{
    struct sxb_array *array = a->v[0].u.a;
    size_t m = s_len(a, 1), total = 0;
    int64_t count = array ? array->count : 0;
    for (int64_t i = 0; i < count; i++) total += sxb_str_length(array->items[i].u.s) + (i ? m : 0);
    char *out = malloc(total + 1u);
    if (out == NULL) { sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL); return; }
    size_t used = 0;
    for (int64_t i = 0; i < count; i++) {
        if (i) { memcpy(out + used, s_data(a, 1), m); used += m; }
        size_t l = sxb_str_length(array->items[i].u.s);
        memcpy(out + used, sxb_str_data(array->items[i].u.s), l);
        used += l;
    }
    push_string(vm, out, used);
    free(out);
}

static void encode_utf8(struct sxb_vm *vm, int64_t code)
{
    if (code < 0 || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL);
        return;
    }
    char out[4];
    size_t n;
    if (code < 0x80) { out[0] = (char)code; n = 1; }
    else if (code < 0x800) { out[0] = (char)(0xC0 | (code >> 6)); out[1] = (char)(0x80 | (code & 0x3F)); n = 2; }
    else if (code < 0x10000) {
        out[0] = (char)(0xE0 | (code >> 12)); out[1] = (char)(0x80 | ((code >> 6) & 0x3F));
        out[2] = (char)(0x80 | (code & 0x3F)); n = 3;
    } else {
        out[0] = (char)(0xF0 | (code >> 18)); out[1] = (char)(0x80 | ((code >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((code >> 6) & 0x3F)); out[3] = (char)(0x80 | (code & 0x3F)); n = 4;
    }
    push_string(vm, out, n);
}

static int64_t decode_utf8(const unsigned char *s, size_t n)
{
    if (n == 0) return -1;
    if (s[0] < 0x80) return s[0];
    int extra = s[0] >= 0xF0 ? 3 : s[0] >= 0xE0 ? 2 : s[0] >= 0xC0 ? 1 : 0;
    if (extra == 0 || (size_t)extra > n - 1) return s[0];
    int64_t code = s[0] & (0x3F >> extra);
    for (int i = 1; i <= extra; i++) code = (code << 6) | (s[i] & 0x3F);
    return code;
}

/* A path for the file builtins: absolute, as SUN's are. */
static int check_path(struct sxb_vm *vm, struct args *a)
{
    if (s_len(a, 0) == 0 || s_data(a, 0)[0] != '/') {
        sxb_vm_raise(vm, SXB_ERR_PATH, "a path has to be absolute: AppPath() & \"/name\", for example");
        return 0;
    }
    return 1;
}

static uint64_t next_random(struct sxb_vm *vm)
{
    uint64_t x = vm->random_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    vm->random_state = x;
    return x * 0x2545F4914F6CDD1Dull;
}

void sxb_builtin(struct sxb_vm *vm, int id, int argc)
{
    struct args a;
    a.count = argc;
    vm->sp -= argc;
    /* The arguments stay in place, above sp, until they are released at the
     * end; results are pushed over them only after copying them out. */
    struct sxb_value saved[3];
    for (int i = 0; i < argc && i < 3; i++) saved[i] = vm->stack[vm->sp + i];
    a.v = saved;
    const struct sxb_host *host = vm->host;
    switch (id) {
    case SXB_BI_LEN:
        push_int(vm, sxb_utf8_count(s_data(&a, 0), s_len(&a, 0)));
        break;
    case SXB_BI_LEFT:
    case SXB_BI_RIGHT: {
        int64_t n = a.v[1].u.i;
        if (n < 0) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        const char *s = s_data(&a, 0);
        size_t len = s_len(&a, 0);
        if (id == SXB_BI_LEFT) {
            push_string(vm, s, sxb_utf8_offset(s, len, n));
        } else {
            int64_t total = sxb_utf8_count(s, len);
            size_t from = n >= total ? 0 : sxb_utf8_offset(s, len, total - n);
            push_string(vm, s + from, len - from);
        }
        break;
    }
    case SXB_BI_MID: {
        int64_t start = a.v[1].u.i;
        if (start < 1 || (argc == 3 && a.v[2].u.i < 0)) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        const char *s = s_data(&a, 0);
        size_t len = s_len(&a, 0);
        size_t from = sxb_utf8_offset(s, len, start - 1);
        size_t to = argc == 3 ? from + sxb_utf8_offset(s + from, len - from, a.v[2].u.i) : len;
        push_string(vm, s + from, to - from);
        break;
    }
    case SXB_BI_INSTR: {
        int64_t start = argc == 3 ? a.v[2].u.i : 1;
        if (start < 1) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        const char *s = s_data(&a, 0);
        size_t len = s_len(&a, 0);
        if (start - 1 > sxb_utf8_count(s, len)) { push_int(vm, 0); break; }
        size_t from = sxb_utf8_offset(s, len, start - 1);
        long at = find_bytes(s, len, s_data(&a, 1), s_len(&a, 1), from);
        push_int(vm, at < 0 ? 0 : sxb_utf8_count(s, (size_t)at) + 1);
        break;
    }
    case SXB_BI_UCASE: change_case(vm, &a, 1); break;
    case SXB_BI_LCASE: change_case(vm, &a, 0); break;
    case SXB_BI_TRIM: trim(vm, &a, 1, 1); break;
    case SXB_BI_LTRIM: trim(vm, &a, 1, 0); break;
    case SXB_BI_RTRIM: trim(vm, &a, 0, 1); break;
    case SXB_BI_REPLACE: replace(vm, &a); break;
    case SXB_BI_SPLIT: split(vm, &a); break;
    case SXB_BI_JOIN: join(vm, &a); break;
    case SXB_BI_SPACE: {
        int64_t n = a.v[0].u.i;
        if (n < 0 || n > 16777216) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        char *out = malloc((size_t)n + 1u);
        if (out == NULL) { sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL); break; }
        memset(out, ' ', (size_t)n);
        push_string(vm, out, (size_t)n);
        free(out);
        break;
    }
    case SXB_BI_CHR: encode_utf8(vm, a.v[0].u.i); break;
    case SXB_BI_ASC: {
        int64_t code = decode_utf8((const unsigned char *)s_data(&a, 0), s_len(&a, 0));
        if (code < 0) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        push_int(vm, code);
        break;
    }
    case SXB_BI_CINT_D: {
        double x = a.v[0].u.d;
        if (!(x > -9.2233720368547758e18 && x < 9.2233720368547758e18)) { sxb_vm_raise(vm, SXB_ERR_OVERFLOW, NULL); break; }
        push_int(vm, round_half_away(x));
        break;
    }
    case SXB_BI_CINT_S:
    case SXB_BI_CDBL_S:
    case SXB_BI_ISNUMERIC: {
        int is_integer = 0;
        int64_t integer = 0;
        double real = 0;
        int ok = sxb_parse_number(s_data(&a, 0), s_len(&a, 0), &is_integer, &integer, &real);
        if (id == SXB_BI_ISNUMERIC) { push_int(vm, ok); break; }
        if (!ok) { sxb_vm_raise(vm, SXB_ERR_TYPE_MISMATCH, NULL); break; }
        if (id == SXB_BI_CDBL_S) { push_double(vm, real); break; }
        if (is_integer) { push_int(vm, integer); break; }
        if (!(real > -9.2233720368547758e18 && real < 9.2233720368547758e18)) { sxb_vm_raise(vm, SXB_ERR_OVERFLOW, NULL); break; }
        push_int(vm, round_half_away(real));
        break;
    }
    case SXB_BI_CBOOL_S: {
        const char *s = s_data(&a, 0);
        size_t n = s_len(&a, 0);
        if (n == 4 && sxb_name_equal(s, 4, "True")) push_int(vm, 1);
        else if (n == 5 && sxb_name_equal(s, 5, "False")) push_int(vm, 0);
        else sxb_vm_raise(vm, SXB_ERR_TYPE_MISMATCH, NULL);
        break;
    }
    case SXB_BI_ABSI:
        if (a.v[0].u.i == INT64_MIN) { sxb_vm_raise(vm, SXB_ERR_OVERFLOW, NULL); break; }
        push_int(vm, a.v[0].u.i < 0 ? -a.v[0].u.i : a.v[0].u.i);
        break;
    case SXB_BI_ABSD: push_double(vm, fabs(a.v[0].u.d)); break;
    case SXB_BI_SGNI: push_int(vm, a.v[0].u.i > 0 ? 1 : a.v[0].u.i < 0 ? -1 : 0); break;
    case SXB_BI_SGND: push_int(vm, a.v[0].u.d > 0 ? 1 : a.v[0].u.d < 0 ? -1 : 0); break;
    case SXB_BI_SQR:
        if (a.v[0].u.d < 0) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        push_double(vm, sqrt(a.v[0].u.d));
        break;
    case SXB_BI_INT: push_double(vm, floor(a.v[0].u.d)); break;
    case SXB_BI_FIX: push_double(vm, trunc(a.v[0].u.d)); break;
    case SXB_BI_ROUND: {
        int64_t digits = argc == 2 ? a.v[1].u.i : 0;
        if (digits < 0 || digits > 15) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        double scale = pow(10.0, (double)digits), x = a.v[0].u.d * scale;
        if (fabs(x) >= 9.0e15) { push_double(vm, a.v[0].u.d); break; }
        push_double(vm, (double)round_half_away(x) / scale);
        break;
    }
    case SXB_BI_SIN: push_double(vm, sin(a.v[0].u.d)); break;
    case SXB_BI_COS: push_double(vm, cos(a.v[0].u.d)); break;
    case SXB_BI_TAN: push_double(vm, tan(a.v[0].u.d)); break;
    case SXB_BI_ATN: push_double(vm, atan(a.v[0].u.d)); break;
    case SXB_BI_EXP: push_double(vm, exp(a.v[0].u.d)); break;
    case SXB_BI_LOG:
        if (a.v[0].u.d <= 0) { sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, NULL); break; }
        push_double(vm, log(a.v[0].u.d));
        break;
    case SXB_BI_RND: push_double(vm, (double)(next_random(vm) >> 11) * (1.0 / 9007199254740992.0)); break;
    case SXB_BI_RANDOMIZE: {
        uint64_t seed = argc == 1 ? (uint64_t)a.v[0].u.i : (host->clock_ns ? host->clock_ns(host->user) : 1);
        vm->random_state = (seed ^ 0x9E3779B97F4A7C15ull) | 1u;
        break;
    }
    case SXB_BI_READLINE: {
        char *line = NULL;
        long n = host->read_line ? host->read_line(host->user, &line) : -1;
        push_string(vm, line ? line : "", n > 0 ? (size_t)n : 0);
        free(line);
        break;
    }
    case SXB_BI_READALLTEXT: {
        if (!check_path(vm, &a)) break;
        char *data = NULL;
        size_t size = 0;
        int error = host->read_file ? host->read_file(host->user, s_data(&a, 0), &data, &size) : SXB_ERR_IO;
        if (error) { free(data); sxb_vm_raise(vm, error, NULL); break; }
        push_string(vm, data ? data : "", size);
        free(data);
        break;
    }
    case SXB_BI_WRITEALLTEXT: {
        if (!check_path(vm, &a)) break;
        int error = host->write_file ? host->write_file(host->user, s_data(&a, 0), s_data(&a, 1), s_len(&a, 1)) : SXB_ERR_IO;
        if (error) sxb_vm_raise(vm, error, NULL);
        break;
    }
    case SXB_BI_FILEEXISTS:
        if (!check_path(vm, &a)) break;
        push_int(vm, host->file_exists ? host->file_exists(host->user, s_data(&a, 0)) != 0 : 0);
        break;
    case SXB_BI_APPPATH: push_string(vm, vm->app_path, strlen(vm->app_path)); break;
    case SXB_BI_COMMAND: push_string(vm, vm->arguments, strlen(vm->arguments)); break;
    case SXB_BI_SECONDS: {
        uint64_t now = host->clock_ns ? host->clock_ns(host->user) : vm->start_ns;
        push_double(vm, (double)(now - vm->start_ns) / 1e9);
        break;
    }
    case SXB_BI_DOEVENTS:
        break;
    default:
        sxb_vm_raise(vm, SXB_ERR_INVALID_ARGUMENT, "the program is damaged (unknown builtin)");
        break;
    }
    for (int i = 0; i < argc && i < 3; i++) sxb_value_release(&saved[i]);
}
