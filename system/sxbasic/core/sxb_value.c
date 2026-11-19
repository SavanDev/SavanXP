/* SxBASIC core: strings, arrays, and numbers as text. */
#include "sxb_vm_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct sxb_string *sxb_string_new(const char *data, size_t length)
{
    struct sxb_string *string = malloc(sizeof(*string) + length + 1u);
    if (string == NULL) {
        return NULL;
    }
    string->refs = 1;
    string->length = length;
    if (length) memcpy(string->data, data, length);
    string->data[length] = '\0';
    return string;
}

void sxb_string_release(struct sxb_string *string)
{
    if (string != NULL && --string->refs == 0) {
        free(string);
    }
}

struct sxb_string *sxb_vm_string(struct sxb_vm *vm, const char *data, size_t length)
{
    if (length == 0) {
        return NULL;
    }
    struct sxb_string *string = sxb_string_new(data, length);
    if (string == NULL) {
        sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL);
    }
    return string;
}

void sxb_value_retain(const struct sxb_value *value)
{
    if (value->kind == SXB_V_STRING && value->u.s) value->u.s->refs++;
    else if (value->kind == SXB_V_ARRAY && value->u.a) value->u.a->refs++;
}

void sxb_value_release(struct sxb_value *value)
{
    if (value->kind == SXB_V_STRING) sxb_string_release(value->u.s);
    else if (value->kind == SXB_V_ARRAY) sxb_array_release(value->u.a);
    value->kind = SXB_V_SCALAR;
    value->u.i = 0;
}

struct sxb_array *sxb_array_new(int element_type, int64_t count)
{
    struct sxb_array *array = malloc(sizeof(*array));
    if (array == NULL) {
        return NULL;
    }
    array->refs = 1;
    array->element_type = element_type;
    array->count = count;
    array->items = NULL;
    if (count > 0) {
        array->items = calloc((size_t)count, sizeof(*array->items));
        if (array->items == NULL) {
            free(array);
            return NULL;
        }
        if (element_type == SXB_TY_STRING) {
            for (int64_t i = 0; i < count; i++) array->items[i].kind = SXB_V_STRING;
        }
    }
    return array;
}

void sxb_array_release(struct sxb_array *array)
{
    if (array == NULL || --array->refs != 0) {
        return;
    }
    for (int64_t i = 0; i < array->count; i++) {
        sxb_value_release(&array->items[i]);
    }
    free(array->items);
    free(array);
}

struct sxb_array *sxb_array_writable(struct sxb_vm *vm, struct sxb_value *cell)
{
    struct sxb_array *array = cell->kind == SXB_V_ARRAY ? cell->u.a : NULL;
    if (array == NULL || array->refs == 1) {
        return array;
    }
    struct sxb_array *copy = sxb_array_new(array->element_type, array->count);
    if (copy == NULL) {
        sxb_vm_raise(vm, SXB_ERR_OUT_OF_MEMORY, NULL);
        return NULL;
    }
    for (int64_t i = 0; i < array->count; i++) {
        copy->items[i] = array->items[i];
        sxb_value_retain(&copy->items[i]);
    }
    sxb_array_release(array);
    cell->u.a = copy;
    return copy;
}

int64_t sxb_utf8_count(const char *text, size_t length)
{
    int64_t count = 0;
    for (size_t i = 0; i < length; i++) {
        if (((unsigned char)text[i] & 0xC0u) != 0x80u) count++;
    }
    return count;
}

size_t sxb_utf8_offset(const char *text, size_t length, int64_t index)
{
    size_t i = 0;
    while (i < length && index > 0) {
        i++;
        while (i < length && ((unsigned char)text[i] & 0xC0u) == 0x80u) i++;
        index--;
    }
    return i;
}

/* ---- numbers as text ------------------------------------------------------------- */

static double power10(int exponent)
{
    static const double exact[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                   1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    if (exponent >= 0 && exponent <= 22) return exact[exponent];
    if (exponent < 0 && exponent >= -22) return 1.0 / exact[-exponent];
    return pow(10.0, exponent);
}

/* digits * 10^(exponent - 14) is |value|, with 15 digits in `digits`. */
static void fifteen_digits(double value, int64_t *digits, int *exponent)
{
    int e = (int)floor(log10(value));
    for (int attempt = 0; attempt < 3; attempt++) {
        int shift = 14 - e;
        double scaled;
        if (shift > 22) scaled = value * 1e22 * power10(shift - 22);
        else if (shift >= 0) scaled = value * power10(shift);
        else if (shift >= -22) scaled = value / power10(-shift);
        else scaled = value / 1e22 / power10(-shift - 22);
        int64_t d = (int64_t)(scaled + 0.5);
        if (d >= 1000000000000000ll) { e++; continue; }
        if (d < 100000000000000ll) { e--; continue; }
        *digits = d;
        *exponent = e;
        return;
    }
    *digits = 100000000000000ll;
    *exponent = e;
}

void sxb_format_double(double value, char out[32])
{
    if (value == 0.0 || !isfinite(value)) {
        strcpy(out, value == 0.0 ? "0" : "0");
        return;
    }
    char *p = out;
    if (value < 0) {
        *p++ = '-';
        value = -value;
    }
    int64_t digits;
    int e;
    fifteen_digits(value, &digits, &e);
    char d[16];
    for (int i = 14; i >= 0; i--) {
        d[i] = (char)('0' + digits % 10);
        digits /= 10;
    }
    int n = 15;
    while (n > 1 && d[n - 1] == '0') n--;
    if (e < -4 || e >= 15) {
        *p++ = d[0];
        if (n > 1) {
            *p++ = '.';
            for (int i = 1; i < n; i++) *p++ = d[i];
        }
        *p++ = 'E';
        *p++ = e < 0 ? '-' : '+';
        int a = e < 0 ? -e : e;
        if (a >= 100) *p++ = (char)('0' + a / 100);
        *p++ = (char)('0' + (a / 10) % 10);
        *p++ = (char)('0' + a % 10);
    } else if (e < 0) {
        *p++ = '0';
        *p++ = '.';
        for (int i = -1; i > e; i--) *p++ = '0';
        for (int i = 0; i < n; i++) *p++ = d[i];
    } else {
        for (int i = 0; i <= e; i++) *p++ = i < n ? d[i] : '0';
        if (n > e + 1) {
            *p++ = '.';
            for (int i = e + 1; i < n; i++) *p++ = d[i];
        }
    }
    *p = '\0';
}

int sxb_parse_number(const char *text, size_t length, int *is_integer, int64_t *integer, double *real)
{
    size_t i = 0;
    while (i < length && (text[i] == ' ' || text[i] == '\t')) i++;
    while (length > i && (text[length - 1] == ' ' || text[length - 1] == '\t')) length--;
    size_t start = i;
    if (i < length && (text[i] == '+' || text[i] == '-')) i++;
    size_t digits = 0, fraction = 0;
    int integral = 1;
    while (i < length && text[i] >= '0' && text[i] <= '9') { i++; digits++; }
    if (i < length && text[i] == '.') {
        integral = 0;
        i++;
        while (i < length && text[i] >= '0' && text[i] <= '9') { i++; fraction++; }
    }
    if (digits + fraction == 0) return 0;
    if (i < length && (text[i] == 'e' || text[i] == 'E')) {
        integral = 0;
        i++;
        if (i < length && (text[i] == '+' || text[i] == '-')) i++;
        size_t exponent_digits = 0;
        while (i < length && text[i] >= '0' && text[i] <= '9') { i++; exponent_digits++; }
        if (!exponent_digits) return 0;
    }
    if (i != length || length - start >= 64) return 0;
    char buffer[64];
    memcpy(buffer, text + start, length - start);
    buffer[length - start] = '\0';
    *is_integer = 0;
    if (integral) {
        int negative = buffer[0] == '-';
        uint64_t value = 0;
        int overflow = 0;
        for (const char *p = buffer + (buffer[0] == '-' || buffer[0] == '+'); *p; p++) {
            uint64_t digit = (uint64_t)(*p - '0');
            if (value > (UINT64_MAX - digit) / 10u) { overflow = 1; break; }
            value = value * 10u + digit;
        }
        if (!overflow && value <= (negative ? (uint64_t)INT64_MAX + 1u : (uint64_t)INT64_MAX)) {
            *is_integer = 1;
            *integer = negative ? (int64_t)(0u - value) : (int64_t)value;
        }
    }
    *real = strtod(buffer, NULL);
    return isfinite(*real) ? 1 : 0;
}
