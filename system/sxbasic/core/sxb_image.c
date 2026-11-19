/* The program image: a compiled program as bytes and back
 * (docs/userland/SXBASIC_IMAGE.md). Make SXE puts it in a made executable's
 * .sxbasic section; the stub loads it instead of compiling.
 *
 * Every number is little-endian and written byte by byte, so the layout does
 * not depend on a struct's padding. The loader checks every length against
 * what is left and every index the forms runtime and the VM look things up
 * by; the bytecode itself is trusted, as an executable's own .text is. */
#include "sxb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_MAGIC "SXBI"
#define IMAGE_VERSION 1u

/* ---- writing ------------------------------------------------------------------------ */

struct writer {
    unsigned char *data;
    size_t length;
    size_t capacity;
    int failed;
};

static void put_bytes(struct writer *w, const void *bytes, size_t count)
{
    if (w->failed) return;
    if (w->length + count > w->capacity) {
        size_t capacity = w->capacity ? w->capacity : 4096;
        while (capacity < w->length + count) capacity *= 2;
        unsigned char *grown = realloc(w->data, capacity);
        if (grown == NULL) {
            w->failed = 1;
            return;
        }
        w->data = grown;
        w->capacity = capacity;
    }
    if (count) memcpy(w->data + w->length, bytes, count);
    w->length += count;
}

static void put_u64(struct writer *w, uint64_t value, int bytes)
{
    unsigned char out[8];
    for (int i = 0; i < bytes; i++) out[i] = (unsigned char)(value >> (8 * i));
    put_bytes(w, out, (size_t)bytes);
}

static void put_u32(struct writer *w, uint32_t value)
{
    put_u64(w, value, 4);
}

static void put_i32(struct writer *w, int value)
{
    put_u64(w, (uint32_t)value, 4);
}

static void put_text(struct writer *w, const char *text)
{
    size_t length = strlen(text);
    put_u32(w, (uint32_t)length);
    put_bytes(w, text, length);
}

static void put_control(struct writer *w, const struct sxb_control_def *control)
{
    put_text(w, control->name);
    put_i32(w, control->kind);
    put_i32(w, control->menu);
    put_text(w, control->text);
    for (int p = 0; p < SXB_P_COUNT; p++) put_u64(w, (uint64_t)control->values[p], 8);
    for (int e = 0; e < SXB_EV_COUNT; e++) put_i32(w, control->handlers[e]);
}

int sxb_program_save(const struct sxb_program *program, unsigned char **data, size_t *size)
{
    struct writer w = {0};
    put_bytes(&w, IMAGE_MAGIC, 4);
    put_u32(&w, IMAGE_VERSION);
    /* The table sizes the controls were written with: a reader built with
     * other sizes refuses the image instead of misreading it. */
    put_u32(&w, SXB_P_COUNT);
    put_u32(&w, SXB_EV_COUNT);

    put_text(&w, program->name);
    put_i32(&w, program->windows);
    put_i32(&w, program->entry_proc);
    put_i32(&w, program->global_count);
    put_i32(&w, program->startup_form);

    put_u32(&w, (uint32_t)program->code_length);
    for (int i = 0; i < program->code_length; i++) put_u32(&w, program->code[i]);
    for (int i = 0; i < program->code_length; i++) put_i32(&w, program->lines[i]);
    for (int i = 0; i < program->code_length; i++) put_u64(&w, program->modules[i], 2);

    put_u32(&w, (uint32_t)program->constant_count);
    for (int i = 0; i < program->constant_count; i++) {
        const struct sxb_constant *k = &program->constants[i];
        uint64_t bits;
        memcpy(&bits, &k->double_value, sizeof(bits));
        put_i32(&w, k->type);
        put_u64(&w, (uint64_t)k->int_value, 8);
        put_u64(&w, bits, 8);
        if (k->type == SXB_TY_STRING) {
            size_t length = k->string ? k->string->length : 0;
            put_u32(&w, (uint32_t)length);
            if (length) put_bytes(&w, k->string->data, length);
        }
    }

    put_u32(&w, (uint32_t)program->proc_count);
    for (int i = 0; i < program->proc_count; i++) {
        const struct sxb_proc_info *proc = &program->procs[i];
        put_text(&w, proc->name);
        put_i32(&w, proc->module);
        put_i32(&w, proc->param_count);
        put_i32(&w, proc->local_count);
        put_i32(&w, proc->is_function);
        put_i32(&w, proc->entry);
    }

    put_u32(&w, (uint32_t)program->module_count);
    for (int i = 0; i < program->module_count; i++) {
        put_text(&w, program->module_files[i]);
        put_i32(&w, program->module_globals ? program->module_globals[i][0] : 0);
        put_i32(&w, program->module_globals ? program->module_globals[i][1] : 0);
    }

    put_u32(&w, (uint32_t)program->form_count);
    for (int f = 0; f < program->form_count; f++) {
        const struct sxb_form_def *form = &program->forms[f];
        put_text(&w, form->name);
        put_i32(&w, program->form_modules[f]);
        put_control(&w, &form->form);
        put_u32(&w, (uint32_t)form->control_count);
        for (int c = 0; c < form->control_count; c++) put_control(&w, &form->controls[c]);
    }

    if (w.failed) {
        free(w.data);
        return -1;
    }
    *data = w.data;
    *size = w.length;
    return 0;
}

/* ---- reading ------------------------------------------------------------------------ */

struct reader {
    const unsigned char *data;
    size_t length;
    size_t at;
    int failed;
};

static const unsigned char *take(struct reader *r, size_t count)
{
    if (r->failed || count > r->length - r->at) {
        r->failed = 1;
        return NULL;
    }
    const unsigned char *at = r->data + r->at;
    r->at += count;
    return at;
}

static uint64_t get_u64(struct reader *r, int bytes)
{
    const unsigned char *at = take(r, (size_t)bytes);
    uint64_t value = 0;
    if (at == NULL) return 0;
    for (int i = 0; i < bytes; i++) value |= (uint64_t)at[i] << (8 * i);
    return value;
}

static uint32_t get_u32(struct reader *r)
{
    return (uint32_t)get_u64(r, 4);
}

static int get_i32(struct reader *r)
{
    return (int)(int32_t)get_u32(r);
}

/* A count of items of at least `item_bytes` each: one the rest of the image
 * cannot hold fails here, before anything is allocated for it. */
static int get_count(struct reader *r, size_t item_bytes)
{
    uint32_t count = get_u32(r);
    if (count > 0x7FFFFFFF || (item_bytes && count > (r->length - r->at) / item_bytes)) {
        r->failed = 1;
        return 0;
    }
    return (int)count;
}

static void get_text(struct reader *r, char *out, size_t capacity)
{
    uint32_t length = get_u32(r);
    const unsigned char *at = length < capacity ? take(r, length) : NULL;
    if (at == NULL) {
        r->failed = 1;
        out[0] = '\0';
        return;
    }
    memcpy(out, at, length);
    out[length] = '\0';
}

static void get_control(struct reader *r, struct sxb_control_def *control)
{
    get_text(r, control->name, sizeof(control->name));
    control->kind = get_i32(r);
    control->menu = get_i32(r);
    get_text(r, control->text, sizeof(control->text));
    for (int p = 0; p < SXB_P_COUNT; p++) control->values[p] = (int64_t)get_u64(r, 8);
    for (int e = 0; e < SXB_EV_COUNT; e++) control->handlers[e] = get_i32(r);
}

static int control_valid(const struct sxb_control_def *control, int control_count, int proc_count, int form)
{
    if (form ? control->kind != SXB_CTL_FORM : (control->kind <= SXB_CTL_FORM || control->kind > SXB_CTL_MENUITEM)) {
        return 0;
    }
    if (control->menu < -1 || control->menu >= control_count) return 0;
    for (int e = 0; e < SXB_EV_COUNT; e++) {
        if (control->handlers[e] < -1 || control->handlers[e] >= proc_count) return 0;
    }
    return 1;
}

static struct sxb_program *load_failed(struct sxb_program *program, struct sxb_diagnostic *diag, const char *why)
{
    sxb_program_free(program);
    snprintf(diag->message, sizeof(diag->message), "the program image %s", why);
    return NULL;
}

struct sxb_program *sxb_program_load(const void *data, size_t size, struct sxb_diagnostic *diag)
{
    memset(diag, 0, sizeof(*diag));
    snprintf(diag->file, sizeof(diag->file), "%s", SXB_IMAGE_SECTION);
    struct reader r = {data, size, 0, 0};
    const unsigned char *magic = take(&r, 4);
    if (magic == NULL || memcmp(magic, IMAGE_MAGIC, 4) != 0) return load_failed(NULL, diag, "is not one");
    if (get_u32(&r) != IMAGE_VERSION) return load_failed(NULL, diag, "is of a version this SxBASIC does not read");
    if (get_u32(&r) != SXB_P_COUNT || get_u32(&r) != SXB_EV_COUNT) {
        return load_failed(NULL, diag, "was made by another version of SxBASIC");
    }

    struct sxb_program *p = calloc(1, sizeof(*p));
    if (p == NULL) return load_failed(NULL, diag, "does not fit in memory");
    get_text(&r, p->name, sizeof(p->name));
    p->windows = get_i32(&r);
    p->entry_proc = get_i32(&r);
    p->global_count = get_i32(&r);
    p->startup_form = get_i32(&r);

    p->code_length = get_count(&r, 10);
    p->code_capacity = p->code_length;
    p->code = malloc(sizeof(*p->code) * (size_t)(p->code_length + 1));
    p->lines = malloc(sizeof(*p->lines) * (size_t)(p->code_length + 1));
    p->modules = malloc(sizeof(*p->modules) * (size_t)(p->code_length + 1));
    if (p->code == NULL || p->lines == NULL || p->modules == NULL) return load_failed(p, diag, "does not fit in memory");
    for (int i = 0; i < p->code_length; i++) p->code[i] = get_u32(&r);
    for (int i = 0; i < p->code_length; i++) p->lines[i] = get_i32(&r);
    for (int i = 0; i < p->code_length; i++) p->modules[i] = (uint16_t)get_u64(&r, 2);

    int constants = get_count(&r, 20);
    p->constants = calloc((size_t)constants + 1, sizeof(*p->constants));
    if (p->constants == NULL) return load_failed(p, diag, "does not fit in memory");
    for (int i = 0; i < constants && !r.failed; i++) {
        struct sxb_constant *k = &p->constants[i];
        uint64_t bits;
        k->type = get_i32(&r);
        k->int_value = (int64_t)get_u64(&r, 8);
        bits = get_u64(&r, 8);
        memcpy(&k->double_value, &bits, sizeof(bits));
        p->constant_count = i + 1;
        if (k->type == SXB_TY_STRING) {
            uint32_t length = get_u32(&r);
            const unsigned char *text = take(&r, length);
            if (text == NULL) break;
            k->string = sxb_string_new((const char *)text, length);
            if (k->string == NULL) return load_failed(p, diag, "does not fit in memory");
        }
    }

    p->proc_count = get_count(&r, 28);
    p->procs = calloc((size_t)p->proc_count + 1, sizeof(*p->procs));
    if (p->procs == NULL) return load_failed(p, diag, "does not fit in memory");
    for (int i = 0; i < p->proc_count; i++) {
        struct sxb_proc_info *proc = &p->procs[i];
        get_text(&r, proc->name, sizeof(proc->name));
        proc->module = get_i32(&r);
        proc->param_count = get_i32(&r);
        proc->local_count = get_i32(&r);
        proc->is_function = get_i32(&r);
        proc->entry = get_i32(&r);
    }

    p->module_count = get_count(&r, 12);
    p->module_files = calloc((size_t)p->module_count + 1, sizeof(*p->module_files));
    p->module_globals = calloc((size_t)p->module_count + 1, sizeof(*p->module_globals));
    if (p->module_files == NULL || p->module_globals == NULL) return load_failed(p, diag, "does not fit in memory");
    for (int i = 0; i < p->module_count; i++) {
        get_text(&r, p->module_files[i], sizeof(p->module_files[i]));
        p->module_globals[i][0] = get_i32(&r);
        p->module_globals[i][1] = get_i32(&r);
    }

    int forms = get_count(&r, 8);
    p->forms = calloc((size_t)forms + 1, sizeof(*p->forms));
    p->form_modules = calloc((size_t)forms + 1, sizeof(*p->form_modules));
    if (p->forms == NULL || p->form_modules == NULL) return load_failed(p, diag, "does not fit in memory");
    p->form_count = forms;
    for (int f = 0; f < p->form_count && !r.failed; f++) {
        struct sxb_form_def *form = &p->forms[f];
        get_text(&r, form->name, sizeof(form->name));
        p->form_modules[f] = get_i32(&r);
        get_control(&r, &form->form);
        int count = get_count(&r, 16);
        form->controls = calloc((size_t)count + 1, sizeof(*form->controls));
        if (form->controls == NULL) return load_failed(p, diag, "does not fit in memory");
        form->control_count = count;
        for (int c = 0; c < count; c++) get_control(&r, &form->controls[c]);
    }
    if (r.failed || r.at != r.length) return load_failed(p, diag, "is damaged");

    /* What the VM and the forms runtime index by. */
    if (p->proc_count == 0 || p->entry_proc < 0 || p->entry_proc >= p->proc_count || p->global_count < 0 ||
        p->windows < 0 || p->windows > 1 || (p->windows && (p->startup_form < 0 || p->startup_form >= p->form_count)) ||
        p->form_count > p->proc_count - 1 - p->entry_proc) {
        return load_failed(p, diag, "is damaged");
    }
    for (int i = 0; i < p->code_length; i++) {
        if (p->modules[i] >= p->module_count) return load_failed(p, diag, "is damaged");
    }
    for (int i = 0; i < p->proc_count; i++) {
        const struct sxb_proc_info *proc = &p->procs[i];
        if (proc->module < 0 || proc->module >= p->module_count || proc->entry < 0 ||
            proc->entry >= p->code_length || proc->param_count < 0 || proc->local_count < proc->param_count) {
            return load_failed(p, diag, "is damaged");
        }
    }
    for (int i = 0; i < p->module_count; i++) {
        int first = p->module_globals[i][0], count = p->module_globals[i][1];
        /* A module with no globals has none to start at: -1. */
        if (count < 0 || (count == 0 ? first < -1 : first < 0) || first > p->global_count ||
            count > p->global_count - (first < 0 ? 0 : first)) {
            return load_failed(p, diag, "is damaged");
        }
    }
    for (int f = 0; f < p->form_count; f++) {
        const struct sxb_form_def *form = &p->forms[f];
        if (p->form_modules[f] < 0 || p->form_modules[f] >= p->module_count ||
            !control_valid(&form->form, form->control_count, p->proc_count, 1)) {
            return load_failed(p, diag, "is damaged");
        }
        for (int c = 0; c < form->control_count; c++) {
            if (!control_valid(&form->controls[c], form->control_count, p->proc_count, 0)) {
                return load_failed(p, diag, "is damaged");
            }
        }
    }
    return p;
}
