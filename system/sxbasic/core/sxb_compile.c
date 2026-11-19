/* SxBASIC core: the compiler. Reads a project, parses its modules, resolves
 * names, checks types and emits bytecode for the virtual machine
 * (docs/userland/SXBASIC.md). It stops at the first error.
 *
 * Strings held by constants are reference counted: the program's constant
 * pool owns one reference per entry, a declared Const owns one for its
 * value, and emitting a constant hands the pool a reference of its own. */
#include "sxb_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- builtins ------------------------------------------------------------------
 *
 * Parameters are spelled one letter each: S String, I Integer, D Double (an
 * Integer widens), B Boolean, R String(). A '?' marks where the optional
 * ones start. The result is one of those letters, or V for none. The ones
 * whose types depend on their argument are typed by hand (`special`). */

enum {
    SPECIAL_NONE = 0, SPECIAL_ABS, SPECIAL_SGN, SPECIAL_CINT, SPECIAL_CDBL, SPECIAL_CSTR, SPECIAL_CBOOL,
    SPECIAL_UBOUND, SPECIAL_DIALOG
};

struct builtin {
    const char *name;
    int id;
    const char *params;
    char result;
    int special;
};

static const struct builtin builtins[] = {
    {"Len", SXB_BI_LEN, "S", 'I', 0},
    {"Left", SXB_BI_LEFT, "SI", 'S', 0},
    {"Right", SXB_BI_RIGHT, "SI", 'S', 0},
    {"Mid", SXB_BI_MID, "SI?I", 'S', 0},
    {"InStr", SXB_BI_INSTR, "SS?I", 'I', 0},
    {"UCase", SXB_BI_UCASE, "S", 'S', 0},
    {"LCase", SXB_BI_LCASE, "S", 'S', 0},
    {"Trim", SXB_BI_TRIM, "S", 'S', 0},
    {"LTrim", SXB_BI_LTRIM, "S", 'S', 0},
    {"RTrim", SXB_BI_RTRIM, "S", 'S', 0},
    {"Replace", SXB_BI_REPLACE, "SSS", 'S', 0},
    {"Split", SXB_BI_SPLIT, "SS", 'R', 0},
    {"Join", SXB_BI_JOIN, "RS", 'S', 0},
    {"Space", SXB_BI_SPACE, "I", 'S', 0},
    {"Chr", SXB_BI_CHR, "I", 'S', 0},
    {"Asc", SXB_BI_ASC, "S", 'I', 0},
    {"IsNumeric", SXB_BI_ISNUMERIC, "S", 'B', 0},
    {"Sqr", SXB_BI_SQR, "D", 'D', 0},
    {"Int", SXB_BI_INT, "D", 'D', 0},
    {"Fix", SXB_BI_FIX, "D", 'D', 0},
    {"Round", SXB_BI_ROUND, "D?I", 'D', 0},
    {"Sin", SXB_BI_SIN, "D", 'D', 0},
    {"Cos", SXB_BI_COS, "D", 'D', 0},
    {"Tan", SXB_BI_TAN, "D", 'D', 0},
    {"Atn", SXB_BI_ATN, "D", 'D', 0},
    {"Exp", SXB_BI_EXP, "D", 'D', 0},
    {"Log", SXB_BI_LOG, "D", 'D', 0},
    {"Rnd", SXB_BI_RND, "", 'D', 0},
    {"Randomize", SXB_BI_RANDOMIZE, "?I", 'V', 0},
    {"ReadLine", SXB_BI_READLINE, "", 'S', 0},
    {"ReadAllText", SXB_BI_READALLTEXT, "S", 'S', 0},
    {"WriteAllText", SXB_BI_WRITEALLTEXT, "SS", 'V', 0},
    {"FileExists", SXB_BI_FILEEXISTS, "S", 'B', 0},
    {"AppPath", SXB_BI_APPPATH, "", 'S', 0},
    {"Command", SXB_BI_COMMAND, "", 'S', 0},
    {"Seconds", SXB_BI_SECONDS, "", 'D', 0},
    {"DoEvents", SXB_BI_DOEVENTS, "", 'V', 0},
    {"Abs", -1, "", 0, SPECIAL_ABS},
    {"Sgn", -1, "", 0, SPECIAL_SGN},
    {"CInt", -1, "", 0, SPECIAL_CINT},
    {"CDbl", -1, "", 0, SPECIAL_CDBL},
    {"CStr", -1, "", 0, SPECIAL_CSTR},
    {"CBool", -1, "", 0, SPECIAL_CBOOL},
    {"UBound", -1, "", 0, SPECIAL_UBOUND},
    {"MsgBox", -1, "", 0, SPECIAL_DIALOG},
    {"InputBox", -1, "", 0, SPECIAL_DIALOG},
};

struct builtin_constant {
    const char *name;
    int type;
    int64_t value;
    const char *text;
};

static const struct builtin_constant builtin_constants[] = {
    {"NewLine", SXB_TY_STRING, 0, "\n"},
    {"Tab", SXB_TY_STRING, 0, "\t"},
    {"mbOk", SXB_TY_INT, 0, NULL},
    {"mbOkCancel", SXB_TY_INT, 1, NULL},
    {"mbYesNoCancel", SXB_TY_INT, 3, NULL},
    {"mbYesNo", SXB_TY_INT, 4, NULL},
    {"mbError", SXB_TY_INT, 16, NULL},
    {"mbQuestion", SXB_TY_INT, 32, NULL},
    {"mbWarning", SXB_TY_INT, 48, NULL},
    {"mbInformation", SXB_TY_INT, 64, NULL},
    {"mbResultOk", SXB_TY_INT, 1, NULL},
    {"mbResultCancel", SXB_TY_INT, 2, NULL},
    {"mbResultYes", SXB_TY_INT, 6, NULL},
    {"mbResultNo", SXB_TY_INT, 7, NULL},
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ---- compiler state ------------------------------------------------------------ */

struct global_sym {
    const char *name;
    int name_length;
    int module;
    int is_public;
    int is_const;
    int evaluated;
    int type;
    int slot;
    struct sxb_constant value;
    struct sxb_node *decl;
};

struct local_sym {
    const char *name;
    int name_length;
    int type;
    int slot;
    int is_const;
    struct sxb_constant value;
};

struct loop_ctx {
    int keyword;
    int try_depth;
    int exit_chain;      /* the jumps out, chained through their operands */
};

struct compiler {
    struct sxb_arena arena;
    struct sxb_program *prog;
    struct sxb_diagnostic *diag;
    int failed;
    struct sxb_module_ast *modules;
    int module_count;
    struct global_sym *globals;
    int global_count;
    int global_capacity;
    struct sxb_proc_decl **procs;
    int proc_count;
    /* the procedure being compiled */
    struct sxb_proc_decl *proc;
    int module;
    struct local_sym *locals;
    int local_count;
    int local_capacity;
    int slot_count;
    struct loop_ctx loops[32];
    int loop_depth;
    int try_depth;
    int line;
    int statement_pending; /* the next word emitted starts a statement */
};

static void errorf(struct compiler *c, int line, int column, const char *format, ...)
{
    if (c->failed) {
        return;
    }
    c->failed = 1;
    snprintf(c->diag->file, sizeof(c->diag->file), "%s",
             c->module >= 0 && c->module < c->module_count ? c->modules[c->module].file : "");
    c->diag->line = line;
    c->diag->column = column;
    va_list args;
    va_start(args, format);
    vsnprintf(c->diag->message, sizeof(c->diag->message), format, args);
    va_end(args);
}

#define ERR(c, node, ...) errorf((c), (node)->line, (node)->column, __VA_ARGS__)

static void out_of_memory(struct compiler *c)
{
    errorf(c, c->line, 0, "out of memory");
}

/* ---- emitting ------------------------------------------------------------------ */

static int emit(struct compiler *c, uint32_t word)
{
    struct sxb_program *prog = c->prog;
    if (c->failed) {
        return 0;
    }
    if (prog->code_length == prog->code_capacity) {
        int capacity = prog->code_capacity ? prog->code_capacity * 2 : 1024;
        uint32_t *code = realloc(prog->code, sizeof(*code) * (size_t)capacity);
        if (code) prog->code = code;
        int32_t *lines = realloc(prog->lines, sizeof(*lines) * (size_t)capacity);
        if (lines) prog->lines = lines;
        uint16_t *modules = realloc(prog->modules, sizeof(*modules) * (size_t)capacity);
        if (modules) prog->modules = modules;
        uint8_t *statements = realloc(prog->statements, (size_t)capacity);
        if (statements) prog->statements = statements;
        if (!code || !lines || !modules || !statements) {
            out_of_memory(c);
            return 0;
        }
        prog->code_capacity = capacity;
    }
    int at = prog->code_length++;
    prog->code[at] = word;
    prog->lines[at] = c->line;
    prog->modules[at] = (uint16_t)(c->module < 0 ? 0 : c->module);
    prog->statements[at] = (uint8_t)c->statement_pending;
    c->statement_pending = 0;
    return at;
}

static void emit1(struct compiler *c, int op)
{
    emit(c, (uint32_t)op);
}

static void emit2(struct compiler *c, int op, uint32_t a)
{
    emit(c, (uint32_t)op);
    emit(c, a);
}

static int here(struct compiler *c)
{
    return c->prog->code_length;
}

/* Emits a jump and returns where its address goes. */
static int emit_jump(struct compiler *c, int op)
{
    emit(c, (uint32_t)op);
    return emit(c, 0);
}

static void patch(struct compiler *c, int at, int target)
{
    if (!c->failed) {
        c->prog->code[at] = (uint32_t)target;
    }
}

/* A chain of jumps to one place not known yet: each operand holds the
 * previous one's position, -1 ending the chain. */
static void emit_chained_jump(struct compiler *c, int op, int *chain)
{
    int at = emit_jump(c, op);
    if (!c->failed) {
        c->prog->code[at] = (uint32_t)*chain;
        *chain = at;
    }
}

static void resolve_chain(struct compiler *c, int chain, int target)
{
    while (!c->failed && chain >= 0) {
        int previous = (int)c->prog->code[chain];
        c->prog->code[chain] = (uint32_t)target;
        chain = previous;
    }
}

/* Takes the reference `value` holds. */
static int add_constant(struct compiler *c, struct sxb_constant value)
{
    struct sxb_program *prog = c->prog;
    for (int i = 0; i < prog->constant_count; i++) {
        const struct sxb_constant *k = &prog->constants[i];
        if (k->type != value.type) continue;
        if (value.type == SXB_TY_STRING) {
            size_t la = k->string ? k->string->length : 0;
            size_t lb = value.string ? value.string->length : 0;
            if (la == lb && (la == 0 || memcmp(k->string->data, value.string->data, la) == 0)) {
                sxb_string_release(value.string);
                return i;
            }
        } else if (value.type == SXB_TY_DOUBLE) {
            if (memcmp(&k->double_value, &value.double_value, sizeof(double)) == 0) return i;
        } else if (k->int_value == value.int_value) {
            return i;
        }
    }
    if (prog->constant_count == prog->constant_capacity) {
        int capacity = prog->constant_capacity ? prog->constant_capacity * 2 : 64;
        struct sxb_constant *grown = realloc(prog->constants, sizeof(*grown) * (size_t)capacity);
        if (grown == NULL) {
            if (value.type == SXB_TY_STRING) sxb_string_release(value.string);
            out_of_memory(c);
            return 0;
        }
        prog->constants = grown;
        prog->constant_capacity = capacity;
    }
    prog->constants[prog->constant_count] = value;
    return prog->constant_count++;
}

/* Emits `value`; `value` keeps its own reference. */
static void emit_constant(struct compiler *c, const struct sxb_constant *value)
{
    if (value->type != SXB_TY_STRING && value->type != SXB_TY_DOUBLE && value->int_value >= -2147483647 - 1 &&
        value->int_value <= 2147483647) {
        emit2(c, SXB_OP_PUSH_SMALL, (uint32_t)(int32_t)value->int_value);
        return;
    }
    struct sxb_constant copy = *value;
    if (copy.type == SXB_TY_STRING && copy.string != NULL) {
        copy.string->refs++;
    }
    emit2(c, SXB_OP_PUSH_CONST, (uint32_t)add_constant(c, copy));
}

static struct sxb_string *make_string(struct compiler *c, const char *text, size_t length)
{
    if (length == 0) {
        return NULL;
    }
    struct sxb_string *string = sxb_string_new(text, length);
    if (string == NULL) {
        out_of_memory(c);
    }
    return string;
}

static void constant_release(struct sxb_constant *value)
{
    if (value->type == SXB_TY_STRING) {
        sxb_string_release(value->string);
        value->string = NULL;
    }
}

/* ---- names ------------------------------------------------------------------------ */

enum { SYM_NONE = 0, SYM_LOCAL, SYM_GLOBAL, SYM_CONST, SYM_BUILTIN_CONST, SYM_PROC, SYM_BUILTIN, SYM_MODULE, SYM_ERR };

struct sym {
    int kind;
    int type;
    uint32_t location;
    const struct sxb_constant *value;
    const struct builtin_constant *builtin_constant;
    struct sxb_proc_decl *proc;
    const struct builtin *builtin;
    int module;
};

static int find_module(struct compiler *c, const char *name, int length)
{
    for (int i = 0; i < c->module_count; i++) {
        if (sxb_name_equal(name, length, c->modules[i].name)) return i;
    }
    return -1;
}

static int resolve_in_module(struct compiler *c, const char *name, int length, int module, int public_only,
                             struct sym *out)
{
    for (int i = 0; i < c->global_count; i++) {
        struct global_sym *g = &c->globals[i];
        if (g->module != module || (public_only && !g->is_public)) continue;
        if (!sxb_names_equal(g->name, g->name_length, name, length)) continue;
        if (g->is_const) {
            if (!g->evaluated) continue; /* further down: not known yet */
            out->kind = SYM_CONST;
            out->value = &g->value;
        } else {
            out->kind = SYM_GLOBAL;
            out->location = SXB_LOC_GLOBAL(g->slot);
        }
        out->type = g->type;
        out->module = module;
        return 1;
    }
    for (int i = 0; i < c->proc_count; i++) {
        struct sxb_proc_decl *proc = c->procs[i];
        if (proc->module != module || (public_only && !proc->is_public)) continue;
        if (!sxb_names_equal(proc->name, proc->name_length, name, length)) continue;
        out->kind = SYM_PROC;
        out->proc = proc;
        out->type = proc->is_function ? proc->return_type : SXB_TY_VOID;
        out->module = module;
        return 1;
    }
    return 0;
}

static int resolve(struct compiler *c, const char *name, int length, struct sym *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = c->local_count - 1; i >= 0; i--) {
        struct local_sym *l = &c->locals[i];
        if (!sxb_names_equal(l->name, l->name_length, name, length)) continue;
        if (l->is_const) {
            out->kind = SYM_CONST;
            out->value = &l->value;
        } else {
            out->kind = SYM_LOCAL;
            out->location = SXB_LOC_LOCAL(l->slot);
        }
        out->type = l->type;
        return 1;
    }
    if (c->module >= 0 && resolve_in_module(c, name, length, c->module, 0, out)) {
        return 1;
    }
    for (int m = 0; m < c->module_count; m++) {
        /* A form's Public members are reached through it: FormName.Member. */
        if (m != c->module && c->modules[m].form < 0 && resolve_in_module(c, name, length, m, 1, out)) return 1;
    }
    for (int i = 0; i < COUNT(builtins); i++) {
        if (sxb_name_equal(name, length, builtins[i].name)) {
            out->kind = SYM_BUILTIN;
            out->builtin = &builtins[i];
            return 1;
        }
    }
    for (int i = 0; i < COUNT(builtin_constants); i++) {
        if (sxb_name_equal(name, length, builtin_constants[i].name)) {
            out->kind = SYM_BUILTIN_CONST;
            out->builtin_constant = &builtin_constants[i];
            out->type = builtin_constants[i].type;
            return 1;
        }
    }
    if (sxb_name_equal(name, length, "Err")) {
        out->kind = SYM_ERR;
        return 1;
    }
    int module = find_module(c, name, length);
    if (module >= 0) {
        out->kind = SYM_MODULE;
        out->module = module;
        return 1;
    }
    return 0;
}

/* The value of a builtin constant, with a reference of its own. */
static struct sxb_constant builtin_constant_value(struct compiler *c, const struct builtin_constant *k)
{
    struct sxb_constant value;
    memset(&value, 0, sizeof(value));
    value.type = k->type;
    value.int_value = k->value;
    if (k->text != NULL) {
        value.string = make_string(c, k->text, strlen(k->text));
    }
    return value;
}

static int add_local(struct compiler *c, const struct sxb_node *at, const char *name, int length, int type)
{
    for (int i = 0; i < c->local_count; i++) {
        if (sxb_names_equal(c->locals[i].name, c->locals[i].name_length, name, length)) {
            ERR(c, at, "\"%.*s\" is already declared in this procedure", length, name);
            return -1;
        }
    }
    if (c->local_count == c->local_capacity) {
        int capacity = c->local_capacity ? c->local_capacity * 2 : 32;
        struct local_sym *grown = realloc(c->locals, sizeof(*grown) * (size_t)capacity);
        if (grown == NULL) {
            out_of_memory(c);
            return -1;
        }
        c->locals = grown;
        c->local_capacity = capacity;
    }
    struct local_sym *l = &c->locals[c->local_count++];
    memset(l, 0, sizeof(*l));
    l->name = name;
    l->name_length = length;
    l->type = type;
    l->slot = c->slot_count++;
    return l->slot;
}

/* A variable `node` names (local or global, written without parentheses):
 * what ByRef, ReDim, For and assignment need. */
static int variable_of(struct compiler *c, struct sxb_node *node, struct sym *s)
{
    if (node->kind == SXB_N_NAME && !(node->flags & SXB_F_HAS_PARENS)) {
        if (!resolve(c, node->name, node->name_length, s)) return 0;
    } else if (node->kind == SXB_N_MEMBER && !(node->flags & SXB_F_HAS_PARENS) && node->a->kind == SXB_N_NAME &&
               !(node->a->flags & SXB_F_HAS_PARENS)) {
        int module = find_module(c, node->a->name, node->a->name_length);
        memset(s, 0, sizeof(*s));
        if (module < 0 ||
            !resolve_in_module(c, node->member, node->member_length, module, module != c->module, s)) {
            return 0;
        }
    } else {
        return 0;
    }
    return s->kind == SYM_LOCAL || s->kind == SYM_GLOBAL;
}

static const char *node_name(const struct sxb_node *node, int *length)
{
    if (node->kind == SXB_N_MEMBER) {
        *length = node->member_length;
        return node->member;
    }
    *length = node->name_length;
    return node->name ? node->name : "";
}

/* ---- constant expressions -------------------------------------------------------- */

static int is_numeric(int type)
{
    return type == SXB_TY_INT || type == SXB_TY_DOUBLE;
}

static double as_double(const struct sxb_constant *k)
{
    return k->type == SXB_TY_DOUBLE ? k->double_value : (double)k->int_value;
}

static void constant_text(const struct sxb_constant *k, char *out, size_t size)
{
    switch (k->type) {
    case SXB_TY_INT: snprintf(out, size, "%lld", (long long)k->int_value); break;
    case SXB_TY_DOUBLE: sxb_format_double(k->double_value, out); break;
    case SXB_TY_BOOL: snprintf(out, size, "%s", k->int_value ? "True" : "False"); break;
    default: out[0] = '\0'; break;
    }
}

/* Evaluates a Const's value. 0, or -1 after reporting an error. */
static int eval_const(struct compiler *c, struct sxb_node *node, struct sxb_constant *out)
{
    memset(out, 0, sizeof(*out));
    c->line = node->line;
    switch (node->kind) {
    case SXB_N_INT:
        out->type = SXB_TY_INT;
        out->int_value = node->int_value;
        return 0;
    case SXB_N_DOUBLE:
        out->type = SXB_TY_DOUBLE;
        out->double_value = node->double_value;
        return 0;
    case SXB_N_BOOL:
        out->type = SXB_TY_BOOL;
        out->int_value = node->int_value;
        return 0;
    case SXB_N_STRING:
        out->type = SXB_TY_STRING;
        out->string = make_string(c, node->string_value, node->string_length);
        return c->failed ? -1 : 0;
    case SXB_N_NAME: {
        struct sym s;
        if ((node->flags & SXB_F_HAS_PARENS) || !resolve(c, node->name, node->name_length, &s)) {
            break;
        }
        if (s.kind == SYM_CONST) {
            *out = *s.value;
            if (out->type == SXB_TY_STRING && out->string) out->string->refs++;
            return 0;
        }
        if (s.kind == SYM_BUILTIN_CONST) {
            *out = builtin_constant_value(c, s.builtin_constant);
            return c->failed ? -1 : 0;
        }
        break;
    }
    case SXB_N_UNARY:
        if (node->op == SXB_OP_NEG) {
            if (eval_const(c, node->a, out) != 0) return -1;
            if (out->type == SXB_TY_INT) {
                if (out->int_value == INT64_MIN) {
                    ERR(c, node, "the constant does not fit in an Integer");
                    return -1;
                }
                out->int_value = -out->int_value;
                return 0;
            }
            if (out->type == SXB_TY_DOUBLE) {
                out->double_value = -out->double_value;
                return 0;
            }
            constant_release(out);
            ERR(c, node, "\"-\" works on numbers");
            return -1;
        }
        break;
    case SXB_N_BINARY: {
        int op = node->op;
        if (op != SXB_OP_PLUS && op != SXB_OP_MINUS && op != SXB_OP_STAR && op != SXB_OP_SLASH &&
            op != SXB_OP_BACKSLASH && op != SXB_OP_MOD && op != SXB_OP_AMP) {
            break;
        }
        struct sxb_constant a, b;
        if (eval_const(c, node->a, &a) != 0) return -1;
        if (eval_const(c, node->b, &b) != 0) {
            constant_release(&a);
            return -1;
        }
        if (op == SXB_OP_AMP) {
            char ta[40], tb[40];
            const char *pa = ta, *pb = tb;
            size_t la, lb;
            if (a.type == SXB_TY_STRING) { pa = a.string ? a.string->data : ""; la = a.string ? a.string->length : 0; }
            else { constant_text(&a, ta, sizeof(ta)); la = strlen(ta); }
            if (b.type == SXB_TY_STRING) { pb = b.string ? b.string->data : ""; lb = b.string ? b.string->length : 0; }
            else { constant_text(&b, tb, sizeof(tb)); lb = strlen(tb); }
            char *joined = malloc(la + lb + 1);
            if (joined == NULL) {
                constant_release(&a);
                constant_release(&b);
                out_of_memory(c);
                return -1;
            }
            memcpy(joined, pa, la);
            memcpy(joined + la, pb, lb);
            out->type = SXB_TY_STRING;
            out->string = make_string(c, joined, la + lb);
            free(joined);
            constant_release(&a);
            constant_release(&b);
            return c->failed ? -1 : 0;
        }
        if (!is_numeric(a.type) || !is_numeric(b.type)) {
            constant_release(&a);
            constant_release(&b);
            if (op == SXB_OP_PLUS && (a.type == SXB_TY_STRING || b.type == SXB_TY_STRING)) {
                ERR(c, node, "\"+\" does not join strings; use \"&\"");
            } else {
                ERR(c, node, "this operator works on numbers");
            }
            return -1;
        }
        if (op == SXB_OP_BACKSLASH || op == SXB_OP_MOD) {
            if (a.type != SXB_TY_INT || b.type != SXB_TY_INT) {
                ERR(c, node, "\\ and Mod work on Integers");
                return -1;
            }
            if (b.int_value == 0 || (a.int_value == INT64_MIN && b.int_value == -1)) {
                ERR(c, node, "the constant divides by zero or overflows");
                return -1;
            }
            out->type = SXB_TY_INT;
            out->int_value = op == SXB_OP_MOD ? a.int_value % b.int_value : a.int_value / b.int_value;
            return 0;
        }
        if (a.type == SXB_TY_INT && b.type == SXB_TY_INT && op != SXB_OP_SLASH) {
            int64_t result = 0;
            int overflow = op == SXB_OP_PLUS    ? __builtin_add_overflow(a.int_value, b.int_value, &result)
                           : op == SXB_OP_MINUS ? __builtin_sub_overflow(a.int_value, b.int_value, &result)
                                                : __builtin_mul_overflow(a.int_value, b.int_value, &result);
            if (overflow) {
                ERR(c, node, "the constant does not fit in an Integer");
                return -1;
            }
            out->type = SXB_TY_INT;
            out->int_value = result;
            return 0;
        }
        double x = as_double(&a), y = as_double(&b), r;
        if (op == SXB_OP_SLASH && y == 0.0) {
            ERR(c, node, "the constant divides by zero");
            return -1;
        }
        r = op == SXB_OP_PLUS ? x + y : op == SXB_OP_MINUS ? x - y : op == SXB_OP_STAR ? x * y : x / y;
        if (!__builtin_isfinite(r)) {
            ERR(c, node, "the constant overflows a Double");
            return -1;
        }
        out->type = SXB_TY_DOUBLE;
        out->double_value = r;
        return 0;
    }
    default:
        break;
    }
    ERR(c, node, "a Const takes a value known before the program runs: literals, other constants, and + - * / \\ Mod &");
    return -1;
}

/* Converts a Const's value to its declared type. */
static int const_to_type(struct compiler *c, struct sxb_node *node, struct sxb_constant *value, int declared)
{
    if (declared == 0 || declared == value->type) {
        return 0;
    }
    if (declared == SXB_TY_DOUBLE && value->type == SXB_TY_INT) {
        value->double_value = (double)value->int_value;
        value->type = SXB_TY_DOUBLE;
        return 0;
    }
    ERR(c, node, "the constant is declared %s but its value is %s", sxb_type_name(declared), sxb_type_name(value->type));
    constant_release(value);
    return -1;
}

/* ---- expressions ---------------------------------------------------------------- */

static int gen_expr(struct compiler *c, struct sxb_node *node);

/* Emits `node` as a value of `wanted`, widening an Integer to a Double. */
static void gen_expr_as(struct compiler *c, struct sxb_node *node, int wanted, const char *what)
{
    int type = gen_expr(c, node);
    if (c->failed || type == wanted) {
        return;
    }
    if (type == SXB_TY_INT && wanted == SXB_TY_DOUBLE) {
        emit1(c, SXB_OP_I2D);
        return;
    }
    if (type == SXB_TY_DOUBLE && wanted == SXB_TY_INT) {
        ERR(c, node, "%s: a Double does not become an Integer by itself; use CInt, Int or Fix", what);
        return;
    }
    if (type == SXB_TY_VOID) {
        ERR(c, node, "%s: this gives no value", what);
        return;
    }
    ERR(c, node, "%s: expected %s, found %s", what, sxb_type_name(wanted), sxb_type_name(type));
}

static void gen_to_string(struct compiler *c, struct sxb_node *node)
{
    int type = gen_expr(c, node);
    if (c->failed || type == SXB_TY_STRING) return;
    if (type == SXB_TY_VOID || SXB_TY_IS_ARRAY(type)) {
        ERR(c, node, "%s cannot be written as text", type == SXB_TY_VOID ? "this, which gives no value," : "an array");
        return;
    }
    emit2(c, SXB_OP_TOSTR, (uint32_t)type);
}

static void gen_condition(struct compiler *c, struct sxb_node *node, const char *what)
{
    int type = gen_expr(c, node);
    if (c->failed || type == SXB_TY_BOOL) return;
    if (is_numeric(type)) {
        ERR(c, node, "%s has to be a Boolean, not %s: compare it, as in \"x <> 0\"", what, sxb_type_name(type));
    } else {
        ERR(c, node, "%s has to be a Boolean, not %s", what, sxb_type_name(type));
    }
}

/* With the left value (of type `lt`) and the right one (`rt`) on the stack,
 * emits the comparison `op`. */
static void emit_compare(struct compiler *c, struct sxb_node *node, int lt, int rt, int op)
{
    if (is_numeric(lt) && is_numeric(rt)) {
        if (lt == SXB_TY_DOUBLE || rt == SXB_TY_DOUBLE) {
            if (lt == SXB_TY_INT) emit1(c, SXB_OP_I2D2);
            if (rt == SXB_TY_INT) emit1(c, SXB_OP_I2D);
            emit2(c, SXB_OP_CMPD, (uint32_t)op);
        } else {
            emit2(c, SXB_OP_CMPI, (uint32_t)op);
        }
        return;
    }
    if (lt == SXB_TY_STRING && rt == SXB_TY_STRING) {
        emit2(c, SXB_OP_CMPS, (uint32_t)op);
        return;
    }
    if (lt == SXB_TY_BOOL && rt == SXB_TY_BOOL) {
        if (op != SXB_OP_EQ && op != SXB_OP_NE) {
            ERR(c, node, "Booleans compare with = and <> only");
            return;
        }
        emit2(c, SXB_OP_CMPB, (uint32_t)op);
        return;
    }
    ERR(c, node, "%s cannot be compared with %s", sxb_type_name(lt), sxb_type_name(rt));
}

static int gen_call_user(struct compiler *c, struct sxb_node *node, struct sxb_proc_decl *proc, struct sxb_node *args)
{
    int expected = 0;
    for (struct sxb_node *p = proc->params; p; p = p->next) expected++;
    int given = 0;
    for (struct sxb_node *a = args; a; a = a->next) given++;
    if (given != expected) {
        ERR(c, node, "%.*s takes %d argument%s, not %d", proc->name_length, proc->name, expected,
            expected == 1 ? "" : "s", given);
        return SXB_TY_VOID;
    }
    int index = 1;
    struct sxb_node *a = args;
    for (struct sxb_node *p = proc->params; p && !c->failed; p = p->next, a = a->next, index++) {
        char what[160];
        snprintf(what, sizeof(what), "argument %d of %.*s", index, proc->name_length, proc->name);
        if ((p->flags & SXB_F_BYREF) || SXB_TY_IS_ARRAY(p->type)) {
            struct sym s;
            if (!variable_of(c, a, &s)) {
                ERR(c, a, "%s is passed by reference, so it has to be a variable", what);
                break;
            }
            if (s.type != p->type) {
                ERR(c, a, "%s: expected a variable of type %s, found %s", what, sxb_type_name(p->type),
                    sxb_type_name(s.type));
                break;
            }
            c->line = a->line;
            emit2(c, SXB_OP_REF, s.location);
        } else {
            gen_expr_as(c, a, p->type, what);
        }
    }
    c->line = node->line;
    emit2(c, SXB_OP_CALL, (uint32_t)proc->index);
    return proc->is_function ? proc->return_type : SXB_TY_VOID;
}

static int letter_type(char letter)
{
    switch (letter) {
    case 'S': return SXB_TY_STRING;
    case 'I': return SXB_TY_INT;
    case 'D': return SXB_TY_DOUBLE;
    case 'B': return SXB_TY_BOOL;
    case 'R': return SXB_TY_STRING | SXB_TY_ARRAY;
    default: return SXB_TY_VOID;
    }
}

static int gen_one_arg(struct compiler *c, struct sxb_node *node, const struct builtin *b, struct sxb_node *args)
{
    int given = 0;
    for (struct sxb_node *a = args; a; a = a->next) given++;
    if (given != 1) {
        ERR(c, node, "%s takes 1 argument, not %d", b->name, given);
        return SXB_TY_VOID;
    }
    return gen_expr(c, args);
}

static int gen_builtin(struct compiler *c, struct sxb_node *node, const struct builtin *b, struct sxb_node *args)
{
    int type;
    switch (b->special) {
    case SPECIAL_ABS:
    case SPECIAL_SGN:
        type = gen_one_arg(c, node, b, args);
        if (c->failed) return SXB_TY_VOID;
        if (!is_numeric(type)) {
            ERR(c, args, "%s takes a number, not %s", b->name, sxb_type_name(type));
            return SXB_TY_VOID;
        }
        c->line = node->line;
        if (b->special == SPECIAL_ABS) {
            emit(c, SXB_OP_BUILTIN);
            emit(c, type == SXB_TY_INT ? SXB_BI_ABSI : SXB_BI_ABSD);
            emit(c, 1);
            return type;
        }
        emit(c, SXB_OP_BUILTIN);
        emit(c, type == SXB_TY_INT ? SXB_BI_SGNI : SXB_BI_SGND);
        emit(c, 1);
        return SXB_TY_INT;
    case SPECIAL_CINT:
    case SPECIAL_CDBL:
    case SPECIAL_CSTR:
    case SPECIAL_CBOOL:
        type = gen_one_arg(c, node, b, args);
        if (c->failed) return SXB_TY_VOID;
        c->line = node->line;
        if (b->special == SPECIAL_CSTR) {
            if (type == SXB_TY_VOID || SXB_TY_IS_ARRAY(type)) {
                ERR(c, args, "CStr takes one value, not %s", sxb_type_name(type));
                return SXB_TY_VOID;
            }
            if (type != SXB_TY_STRING) emit2(c, SXB_OP_TOSTR, (uint32_t)type);
            return SXB_TY_STRING;
        }
        if (b->special == SPECIAL_CBOOL) {
            if (type == SXB_TY_STRING) {
                emit(c, SXB_OP_BUILTIN);
                emit(c, SXB_BI_CBOOL_S);
                emit(c, 1);
            } else if (type != SXB_TY_BOOL) {
                ERR(c, args, "CBool takes a String, not %s", sxb_type_name(type));
            }
            return SXB_TY_BOOL;
        }
        if (type == SXB_TY_STRING) {
            emit(c, SXB_OP_BUILTIN);
            emit(c, b->special == SPECIAL_CINT ? SXB_BI_CINT_S : SXB_BI_CDBL_S);
            emit(c, 1);
        } else if (type == SXB_TY_DOUBLE && b->special == SPECIAL_CINT) {
            emit(c, SXB_OP_BUILTIN);
            emit(c, SXB_BI_CINT_D);
            emit(c, 1);
        } else if (type == SXB_TY_INT && b->special == SPECIAL_CDBL) {
            emit1(c, SXB_OP_I2D);
        } else if (!is_numeric(type)) {
            ERR(c, args, "%s takes a number or a String, not %s", b->name, sxb_type_name(type));
        }
        return b->special == SPECIAL_CINT ? SXB_TY_INT : SXB_TY_DOUBLE;
    case SPECIAL_UBOUND:
        type = gen_one_arg(c, node, b, args);
        if (c->failed) return SXB_TY_VOID;
        if (!SXB_TY_IS_ARRAY(type)) {
            ERR(c, args, "UBound takes an array, not %s", sxb_type_name(type));
            return SXB_TY_VOID;
        }
        c->line = node->line;
        emit1(c, SXB_OP_UBOUND);
        return SXB_TY_INT;
    case SPECIAL_DIALOG: {
        static const struct builtin message = {"MsgBox", SXB_BI_MSGBOX, "S?IS", 'I', 0};
        static const struct builtin input = {"InputBox", SXB_BI_INPUTBOX, "S?SS", 'S', 0};
        if (!c->prog->windows) {
            ERR(c, node, "%s shows a window, so it needs a Windows program; this is a console program", b->name);
            return SXB_TY_VOID;
        }
        return gen_builtin(c, node, b->name[0] == 'M' ? &message : &input, args);
    }
    default:
        break;
    }
    int required = 0, total = 0, optional = 0;
    for (const char *p = b->params; *p; p++) {
        if (*p == '?') { optional = 1; continue; }
        total++;
        if (!optional) required++;
    }
    int given = 0;
    for (struct sxb_node *a = args; a; a = a->next) given++;
    if (given < required || given > total) {
        if (required == total) {
            ERR(c, node, "%s takes %d argument%s, not %d", b->name, total, total == 1 ? "" : "s", given);
        } else {
            ERR(c, node, "%s takes %d to %d arguments, not %d", b->name, required, total, given);
        }
        return SXB_TY_VOID;
    }
    const char *p = b->params;
    int index = 1;
    for (struct sxb_node *a = args; a && !c->failed; a = a->next, index++) {
        if (*p == '?') p++;
        char what[96];
        snprintf(what, sizeof(what), "argument %d of %s", index, b->name);
        gen_expr_as(c, a, letter_type(*p), what);
        p++;
    }
    c->line = node->line;
    emit(c, SXB_OP_BUILTIN);
    emit(c, (uint32_t)b->id);
    emit(c, (uint32_t)given);
    return letter_type(b->result);
}

/* The value of a name found as `s` (a plain name, or a module's member). */
static int gen_sym_value(struct compiler *c, struct sxb_node *node, struct sym *s)
{
    int length;
    const char *name = node_name(node, &length);
    int parens = (node->flags & SXB_F_HAS_PARENS) != 0;
    switch (s->kind) {
    case SYM_LOCAL:
    case SYM_GLOBAL:
        if (!parens) {
            emit2(c, SXB_OP_LOAD, s->location);
            return s->type;
        }
        if (!SXB_TY_IS_ARRAY(s->type)) {
            ERR(c, node, "\"%.*s\" is a %s, not an array or a function", length, name, sxb_type_name(s->type));
            return SXB_TY_VOID;
        }
        if (node->args == NULL || node->args->next != NULL) {
            ERR(c, node, "an array takes one index: %.*s(i)", length, name);
            return SXB_TY_VOID;
        }
        emit2(c, SXB_OP_LOAD, s->location);
        gen_expr_as(c, node->args, SXB_TY_INT, "the index");
        c->line = node->line;
        emit1(c, SXB_OP_ALOAD);
        return SXB_TY_BASE(s->type);
    case SYM_CONST:
        if (parens) break;
        emit_constant(c, s->value);
        return s->type;
    case SYM_BUILTIN_CONST: {
        if (parens) break;
        struct sxb_constant value = builtin_constant_value(c, s->builtin_constant);
        emit_constant(c, &value);
        constant_release(&value);
        return s->type;
    }
    case SYM_PROC:
        if (!s->proc->is_function) {
            ERR(c, node, "%.*s is a Sub and gives no value", length, name);
            return SXB_TY_VOID;
        }
        return gen_call_user(c, node, s->proc, node->args);
    case SYM_BUILTIN: {
        int type = gen_builtin(c, node, s->builtin, node->args);
        if (!c->failed && type == SXB_TY_VOID) {
            ERR(c, node, "%s gives no value", s->builtin->name);
        }
        return type;
    }
    case SYM_MODULE:
        ERR(c, node, "%.*s is a module; name one of its members, as in %.*s.Something", length, name, length, name);
        return SXB_TY_VOID;
    case SYM_ERR:
        ERR(c, node, "Err has Number, Description and Raise: Err.Number, for example");
        return SXB_TY_VOID;
    default:
        break;
    }
    ERR(c, node, "\"%.*s\" takes no arguments", length, name);
    return SXB_TY_VOID;
}

static void unknown_name(struct compiler *c, struct sxb_node *node)
{
    int length;
    const char *name = node_name(node, &length);
    int form = c->module >= 0 && c->module < c->module_count ? c->modules[c->module].form : -1;
    if (form >= 0 && node->kind == SXB_N_NAME) {
        const struct sxb_form_def *def = &c->prog->forms[form];
        for (int i = 0; i < def->control_count; i++) {
            if (sxb_name_equal(name, length, def->controls[i].name)) {
                ERR(c, node, "%.*s is a control: name one of its properties, as in %.*s.Text", length, name, length, name);
                return;
            }
        }
    }
    ERR(c, node, "nothing named \"%.*s\" is declared", length, name);
}

static void forms_only(struct compiler *c, struct sxb_node *node)
{
    ERR(c, node, "forms belong to Windows programs; this is a console program");
}

/* For a member access `x.y`: the module's member, or Err's. */
static int resolve_member(struct compiler *c, struct sxb_node *node, struct sym *s)
{
    struct sxb_node *left = node->a;
    if (left->kind == SXB_N_ME) {
        forms_only(c, left);
        return 0;
    }
    if (left->kind != SXB_N_NAME || (left->flags & SXB_F_HAS_PARENS)) {
        ERR(c, node, "only a module's members are reached with \".\" in a console program");
        return 0;
    }
    struct sym owner;
    if (!resolve(c, left->name, left->name_length, &owner)) {
        unknown_name(c, left);
        return 0;
    }
    if (owner.kind == SYM_ERR) {
        memset(s, 0, sizeof(*s));
        s->kind = SYM_ERR;
        return 1;
    }
    int module = owner.kind == SYM_MODULE ? owner.module : find_module(c, left->name, left->name_length);
    if (module < 0) {
        ERR(c, left, "\"%.*s\" has no members", left->name_length, left->name);
        return 0;
    }
    memset(s, 0, sizeof(*s));
    if (!resolve_in_module(c, node->member, node->member_length, module, module != c->module, s)) {
        ERR(c, node, "the module %s has no Public \"%.*s\"", c->modules[module].name, node->member_length, node->member);
        return 0;
    }
    return 1;
}

/* ---- forms and controls as objects ----------------------------------------------- */

#define NOT_OBJECT (-100)

struct object {
    int form;
    int control; /* -1: the form itself */
};

static int module_form(struct compiler *c, int module)
{
    return module >= 0 && module < c->module_count ? c->modules[module].form : -1;
}

static int find_control(const struct sxb_form_def *form, const char *name, int length)
{
    for (int i = 0; i < form->control_count; i++) {
        if (sxb_name_equal(name, length, form->controls[i].name)) return i;
    }
    return -1;
}

/* What `node` names when it is a form or one of a form's controls: 1, 0 when
 * it is something else, -1 after reporting an error. */
static int object_of(struct compiler *c, struct sxb_node *node, struct object *out)
{
    int current = module_form(c, c->module);
    if (node->kind == SXB_N_ME) {
        if (current < 0) {
            ERR(c, node, "Me is the form whose code is running, and this code is not in a form");
            return -1;
        }
        out->form = current;
        out->control = -1;
        return 1;
    }
    if (node->flags & SXB_F_HAS_PARENS) return 0;
    if (node->kind == SXB_N_NAME) {
        for (int i = 0; i < c->local_count; i++) {
            if (sxb_names_equal(c->locals[i].name, c->locals[i].name_length, node->name, node->name_length)) return 0;
        }
        if (current >= 0) {
            int k = find_control(&c->prog->forms[current], node->name, node->name_length);
            if (k >= 0) {
                out->form = current;
                out->control = k;
                return 1;
            }
        }
        int module = find_module(c, node->name, node->name_length);
        if (module >= 0 && c->modules[module].form >= 0) {
            out->form = c->modules[module].form;
            out->control = -1;
            return 1;
        }
        return 0;
    }
    if (node->kind == SXB_N_MEMBER) {
        struct object owner;
        int found = object_of(c, node->a, &owner);
        if (found <= 0 || owner.control >= 0) return found < 0 ? -1 : 0;
        int k = find_control(&c->prog->forms[owner.form], node->member, node->member_length);
        if (k < 0) return 0;
        out->form = owner.form;
        out->control = k;
        return 1;
    }
    return 0;
}

static int object_kind(struct compiler *c, const struct object *o)
{
    return o->control < 0 ? SXB_CTL_FORM : c->prog->forms[o->form].controls[o->control].kind;
}

static const char *object_name(struct compiler *c, const struct object *o)
{
    const struct sxb_form_def *form = &c->prog->forms[o->form];
    return o->control < 0 ? form->name : form->controls[o->control].name;
}

/* Another form's members load it first, as touching it does in VB. */
static void emit_form_load(struct compiler *c, int form)
{
    if (form != module_form(c, c->module)) emit2(c, SXB_OP_FORMLOAD, (uint32_t)form);
}

/* `node` is a.member: when `a` is a form or a control, emits the property
 * read or the method call and returns its type; NOT_OBJECT when `a` is
 * neither. As a statement, a result is dropped. */
static int gen_object_member(struct compiler *c, struct sxb_node *node, int as_statement)
{
    struct object o;
    int found = object_of(c, node->a, &o);
    if (found < 0) return SXB_TY_VOID;
    if (found == 0) return NOT_OBJECT;
    int kind = object_kind(c, &o);
    const char *owner = object_name(c, &o);
    int property = sxb_find_property(node->member, node->member_length);
    int method = sxb_find_method(node->member, node->member_length);
    c->line = node->line;
    if (property >= 0 && !(node->flags & SXB_F_HAS_PARENS)) {
        int access = sxb_property_access(property, kind);
        if (access == SXB_ACCESS_NONE) {
            ERR(c, node, "a %s has no property %s", sxb_control_kind_name(kind), sxb_property_name(property));
            return SXB_TY_VOID;
        }
        if (access == SXB_ACCESS_FILE) {
            ERR(c, node, "%s is set in the form file; code can neither read nor change it", sxb_property_name(property));
            return SXB_TY_VOID;
        }
        if (as_statement) {
            ERR(c, node, "%s.%s is a property; to change it, write %s.%s = ...", owner, sxb_property_name(property),
                owner, sxb_property_name(property));
            return SXB_TY_VOID;
        }
        int type = sxb_property_type(property, kind);
        emit(c, SXB_OP_PGET);
        emit(c, (uint32_t)o.form);
        emit(c, (uint32_t)o.control);
        emit(c, (uint32_t)property);
        emit(c, (uint32_t)type);
        return type;
    }
    if (method >= 0) {
        if (!sxb_method_applies(method, kind)) {
            ERR(c, node, "a %s has no method %s", sxb_control_kind_name(kind), sxb_method_name(method));
            return SXB_TY_VOID;
        }
        int result = sxb_method_result(method);
        if (!as_statement && result == SXB_TY_VOID) {
            ERR(c, node, "%s gives no value", sxb_method_name(method));
            return SXB_TY_VOID;
        }
        int argument = sxb_method_arg(method);
        int given = 0;
        for (struct sxb_node *a = node->args; a; a = a->next) given++;
        if (given != (argument ? 1 : 0)) {
            ERR(c, node, "%s takes %d argument%s", sxb_method_name(method), argument ? 1 : 0, argument ? "" : "s");
            return SXB_TY_VOID;
        }
        if (argument) {
            char what[96];
            snprintf(what, sizeof(what), "the argument of %s", sxb_method_name(method));
            gen_expr_as(c, node->args, argument, what);
        }
        c->line = node->line;
        emit(c, SXB_OP_CALLM);
        emit(c, (uint32_t)o.form);
        emit(c, (uint32_t)o.control);
        emit(c, (uint32_t)method);
        emit(c, (uint32_t)given);
        emit(c, (uint32_t)result);
        if (as_statement && result != SXB_TY_VOID) emit1(c, SXB_OP_POP);
        return result;
    }
    if (o.control < 0) {
        int module = c->prog->form_modules[o.form];
        struct sym s;
        memset(&s, 0, sizeof(s));
        if (resolve_in_module(c, node->member, node->member_length, module, module != c->module, &s)) {
            emit_form_load(c, o.form);
            if (!as_statement) return gen_sym_value(c, node, &s);
            if (s.kind != SYM_PROC) {
                ERR(c, node, "%s.%.*s is not a procedure", owner, node->member_length, node->member);
                return SXB_TY_VOID;
            }
            int type = gen_call_user(c, node, s.proc, node->args);
            if (type != SXB_TY_VOID) emit1(c, SXB_OP_POP);
            return type;
        }
    }
    ERR(c, node, "%s has no property, method%s named \"%.*s\"", owner, o.control < 0 ? " or Public member" : "",
        node->member_length, node->member);
    return SXB_TY_VOID;
}

/* An assignment to a property. 1 when the target was a form's or a
 * control's member (handled, or reported), 0 when it is something else. */
static int gen_object_assign(struct compiler *c, struct sxb_node *node)
{
    struct sxb_node *target = node->a;
    struct object o;
    if (target->kind != SXB_N_MEMBER) {
        int found = object_of(c, target, &o);
        if (found == 0) return 0;
        if (found > 0) ERR(c, target, "%s is not a value: set one of its properties", object_name(c, &o));
        return 1;
    }
    int found = object_of(c, target->a, &o);
    if (found < 0) return 1;
    if (found == 0) return 0;
    int kind = object_kind(c, &o);
    int property = sxb_find_property(target->member, target->member_length);
    if (property >= 0 && !(target->flags & SXB_F_HAS_PARENS)) {
        int access = sxb_property_access(property, kind);
        if (access == SXB_ACCESS_NONE) {
            ERR(c, target, "a %s has no property %s", sxb_control_kind_name(kind), sxb_property_name(property));
        } else if (access == SXB_ACCESS_FILE) {
            ERR(c, target, "%s is set in the form file; code cannot change it", sxb_property_name(property));
        } else if (access == SXB_ACCESS_READ) {
            ERR(c, target, "%s of a %s can be read, not changed", sxb_property_name(property), sxb_control_kind_name(kind));
        } else {
            int type = sxb_property_type(property, kind);
            gen_expr_as(c, node->b, type, "the property's new value");
            c->line = node->line;
            emit(c, SXB_OP_PSET);
            emit(c, (uint32_t)o.form);
            emit(c, (uint32_t)o.control);
            emit(c, (uint32_t)property);
            emit(c, (uint32_t)type);
        }
        return 1;
    }
    if (o.control < 0) {
        struct sym s;
        int module = c->prog->form_modules[o.form];
        memset(&s, 0, sizeof(s));
        if (resolve_in_module(c, target->member, target->member_length, module, module != c->module, &s) &&
            (s.kind == SYM_GLOBAL)) {
            emit_form_load(c, o.form);
            return 0; /* a form's Public variable: assigned like any other */
        }
    }
    ERR(c, target, "%s has no property named \"%.*s\" to set", object_name(c, &o), target->member_length,
        target->member);
    return 1;
}

static void gen_unload(struct compiler *c, struct sxb_node *node)
{
    struct object o;
    int found = object_of(c, node->a, &o);
    if (found < 0) return;
    if (found == 0 || o.control >= 0) {
        ERR(c, node->a, "Unload takes a form: Unload Me, or Unload FormName");
        return;
    }
    emit2(c, SXB_OP_UNLOAD, (uint32_t)o.form);
}

static int gen_expr(struct compiler *c, struct sxb_node *node)
{
    if (c->failed) {
        return SXB_TY_VOID;
    }
    c->line = node->line;
    switch (node->kind) {
    case SXB_N_INT:
    case SXB_N_BOOL: {
        struct sxb_constant k;
        memset(&k, 0, sizeof(k));
        k.type = node->kind == SXB_N_INT ? SXB_TY_INT : SXB_TY_BOOL;
        k.int_value = node->int_value;
        emit_constant(c, &k);
        return k.type;
    }
    case SXB_N_DOUBLE: {
        struct sxb_constant k;
        memset(&k, 0, sizeof(k));
        k.type = SXB_TY_DOUBLE;
        k.double_value = node->double_value;
        emit_constant(c, &k);
        return SXB_TY_DOUBLE;
    }
    case SXB_N_STRING: {
        struct sxb_constant k;
        memset(&k, 0, sizeof(k));
        k.type = SXB_TY_STRING;
        k.string = make_string(c, node->string_value, node->string_length);
        emit_constant(c, &k);
        constant_release(&k);
        return SXB_TY_STRING;
    }
    case SXB_N_NAME: {
        struct sym s;
        if (!resolve(c, node->name, node->name_length, &s)) {
            unknown_name(c, node);
            return SXB_TY_VOID;
        }
        return gen_sym_value(c, node, &s);
    }
    case SXB_N_MEMBER: {
        int object_type = gen_object_member(c, node, 0);
        if (object_type != NOT_OBJECT) return object_type;
        struct sym s;
        if (!resolve_member(c, node, &s)) return SXB_TY_VOID;
        if (s.kind == SYM_ERR) {
            if (sxb_name_equal(node->member, node->member_length, "Number") && !(node->flags & SXB_F_HAS_PARENS)) {
                emit1(c, SXB_OP_ERRNUM);
                return SXB_TY_INT;
            }
            if (sxb_name_equal(node->member, node->member_length, "Description") &&
                !(node->flags & SXB_F_HAS_PARENS)) {
                emit1(c, SXB_OP_ERRDESC);
                return SXB_TY_STRING;
            }
            if (sxb_name_equal(node->member, node->member_length, "Raise")) {
                ERR(c, node, "Err.Raise is a statement and gives no value");
                return SXB_TY_VOID;
            }
            ERR(c, node, "Err has Number, Description and Raise, not \"%.*s\"", node->member_length, node->member);
            return SXB_TY_VOID;
        }
        return gen_sym_value(c, node, &s);
    }
    case SXB_N_ME:
        if (module_form(c, c->module) < 0) forms_only(c, node);
        else ERR(c, node, "Me is a form: name one of its properties or methods, as in Me.Hide");
        return SXB_TY_VOID;
    case SXB_N_UNARY: {
        if (node->op == SXB_OP_NOT) {
            gen_condition(c, node->a, "what Not negates");
            c->line = node->line;
            emit1(c, SXB_OP_NOTB);
            return SXB_TY_BOOL;
        }
        int type = gen_expr(c, node->a);
        c->line = node->line;
        if (type == SXB_TY_INT) emit1(c, SXB_OP_NEGI);
        else if (type == SXB_TY_DOUBLE) emit1(c, SXB_OP_NEGD);
        else if (!c->failed) ERR(c, node, "\"-\" works on numbers, not %s", sxb_type_name(type));
        return type;
    }
    case SXB_N_BINARY:
        break;
    default:
        ERR(c, node, "this is not a value");
        return SXB_TY_VOID;
    }

    int op = node->op;
    if (op == SXB_OP_AND || op == SXB_OP_OR) {
        gen_condition(c, node->a, op == SXB_OP_AND ? "each side of And" : "each side of Or");
        c->line = node->line;
        int skip = emit_jump(c, op == SXB_OP_AND ? SXB_OP_JF : SXB_OP_JT);
        gen_condition(c, node->b, op == SXB_OP_AND ? "each side of And" : "each side of Or");
        c->line = node->line;
        int done = emit_jump(c, SXB_OP_JMP);
        patch(c, skip, here(c));
        emit2(c, SXB_OP_PUSH_SMALL, op == SXB_OP_AND ? 0u : 1u);
        patch(c, done, here(c));
        return SXB_TY_BOOL;
    }
    if (op == SXB_OP_XOR) {
        gen_condition(c, node->a, "each side of Xor");
        gen_condition(c, node->b, "each side of Xor");
        c->line = node->line;
        emit1(c, SXB_OP_XORB);
        return SXB_TY_BOOL;
    }
    if (op == SXB_OP_AMP) {
        gen_to_string(c, node->a);
        gen_to_string(c, node->b);
        c->line = node->line;
        emit1(c, SXB_OP_CONCAT);
        return SXB_TY_STRING;
    }
    int lt = gen_expr(c, node->a);
    int rt = gen_expr(c, node->b);
    if (c->failed) return SXB_TY_VOID;
    c->line = node->line;
    if (op >= SXB_OP_EQ && op <= SXB_OP_GE) {
        emit_compare(c, node, lt, rt, op);
        return SXB_TY_BOOL;
    }
    if (!is_numeric(lt) || !is_numeric(rt)) {
        if (op == SXB_OP_PLUS && (lt == SXB_TY_STRING || rt == SXB_TY_STRING)) {
            ERR(c, node, "\"+\" does not join strings; use \"&\"");
        } else {
            ERR(c, node, "this operator works on numbers, not %s and %s", sxb_type_name(lt), sxb_type_name(rt));
        }
        return SXB_TY_VOID;
    }
    if (op == SXB_OP_BACKSLASH || op == SXB_OP_MOD) {
        if (lt != SXB_TY_INT || rt != SXB_TY_INT) {
            ERR(c, node, "%s works on Integers; use CInt, Int or Fix first", op == SXB_OP_MOD ? "Mod" : "\\");
            return SXB_TY_VOID;
        }
        emit1(c, op == SXB_OP_MOD ? SXB_OP_MODI : SXB_OP_IDIV);
        return SXB_TY_INT;
    }
    if (lt == SXB_TY_INT && rt == SXB_TY_INT && op != SXB_OP_SLASH && op != SXB_OP_CARET) {
        emit1(c, op == SXB_OP_PLUS ? SXB_OP_ADDI : op == SXB_OP_MINUS ? SXB_OP_SUBI : SXB_OP_MULI);
        return SXB_TY_INT;
    }
    if (lt == SXB_TY_INT) emit1(c, SXB_OP_I2D2);
    if (rt == SXB_TY_INT) emit1(c, SXB_OP_I2D);
    switch (op) {
    case SXB_OP_PLUS: emit1(c, SXB_OP_ADDD); break;
    case SXB_OP_MINUS: emit1(c, SXB_OP_SUBD); break;
    case SXB_OP_STAR: emit1(c, SXB_OP_MULD); break;
    case SXB_OP_SLASH: emit1(c, SXB_OP_DIVD); break;
    default: emit1(c, SXB_OP_POWD); break;
    }
    return SXB_TY_DOUBLE;
}

/* ---- statements ------------------------------------------------------------------ */

static void gen_block(struct compiler *c, struct sxb_node *list);

static void gen_dim(struct compiler *c, struct sxb_node *dim)
{
    for (struct sxb_node *decl = dim->a; decl && !c->failed; decl = decl->next) {
        c->line = decl->line;
        int has_value = 0;
        if (decl->flags & SXB_F_FIXED_SIZE) {
            gen_expr_as(c, decl->a, SXB_TY_INT, "the array's upper bound");
            c->line = decl->line;
            emit2(c, SXB_OP_ANEW, (uint32_t)SXB_TY_BASE(decl->type));
            has_value = 1;
        } else if (decl->b != NULL) {
            gen_expr_as(c, decl->b, decl->type, "the initial value");
            has_value = 1;
        }
        int slot = add_local(c, decl, decl->name, decl->name_length, decl->type);
        if (slot >= 0 && has_value) {
            c->line = decl->line;
            emit2(c, SXB_OP_STORE, SXB_LOC_LOCAL(slot));
        }
    }
}

static void gen_local_const(struct compiler *c, struct sxb_node *node)
{
    struct sxb_constant value;
    if (eval_const(c, node->a, &value) != 0) return;
    if (const_to_type(c, node, &value, node->type) != 0) return;
    if (add_local(c, node, node->name, node->name_length, value.type) < 0) {
        constant_release(&value);
        return;
    }
    struct local_sym *l = &c->locals[c->local_count - 1];
    l->is_const = 1;
    l->value = value;
}

static void gen_assign(struct compiler *c, struct sxb_node *node)
{
    struct sxb_node *target = node->a;
    struct sym s;
    if (gen_object_assign(c, node) || c->failed) return;
    if (target->kind == SXB_N_NAME && (target->flags & SXB_F_HAS_PARENS)) {
        if (!resolve(c, target->name, target->name_length, &s)) {
            unknown_name(c, target);
            return;
        }
        if ((s.kind != SYM_LOCAL && s.kind != SYM_GLOBAL) || !SXB_TY_IS_ARRAY(s.type)) {
            ERR(c, target, "\"%.*s\" is not an array, so it cannot take an index", target->name_length, target->name);
            return;
        }
        if (target->args == NULL || target->args->next != NULL) {
            ERR(c, target, "an array takes one index: %.*s(i)", target->name_length, target->name);
            return;
        }
        gen_expr_as(c, target->args, SXB_TY_INT, "the index");
        gen_expr_as(c, node->b, SXB_TY_BASE(s.type), "the assigned value");
        c->line = node->line;
        emit2(c, SXB_OP_ASTORE, s.location);
        return;
    }
    if (variable_of(c, target, &s)) {
        gen_expr_as(c, node->b, s.type, "the assigned value");
        c->line = node->line;
        emit2(c, SXB_OP_STORE, s.location);
        return;
    }
    int found;
    if (target->kind == SXB_N_MEMBER) {
        found = resolve_member(c, target, &s);
        if (!found) return;
    } else if (target->kind == SXB_N_NAME) {
        found = resolve(c, target->name, target->name_length, &s);
        if (!found) {
            unknown_name(c, target);
            return;
        }
    } else {
        ERR(c, target, "this cannot be assigned to");
        return;
    }
    int length;
    const char *name = node_name(target, &length);
    if (s.kind == SYM_CONST || s.kind == SYM_BUILTIN_CONST) {
        ERR(c, target, "\"%.*s\" is a constant and cannot change", length, name);
    } else if (s.kind == SYM_PROC && s.proc->is_function) {
        ERR(c, target, "a Function gives its value with Return, not by assigning to its name");
    } else {
        ERR(c, target, "\"%.*s\" is not a variable", length, name);
    }
}

static void gen_call_statement(struct compiler *c, struct sxb_node *node)
{
    struct sxb_node *target = node->a;
    struct sym s;
    if (target->kind == SXB_N_ME) {
        object_of(c, target, &(struct object){0, 0});
        if (!c->failed) ERR(c, target, "Me is a form: call one of its methods, as in Me.Hide");
        return;
    }
    if (target->kind == SXB_N_MEMBER) {
        if (gen_object_member(c, target, 1) != NOT_OBJECT || c->failed) return;
        if (!resolve_member(c, target, &s)) return;
        if (s.kind == SYM_ERR) {
            if (!sxb_name_equal(target->member, target->member_length, "Raise")) {
                ERR(c, target, "Err.%.*s is a value; the statement is Err.Raise", target->member_length, target->member);
                return;
            }
            int given = 0;
            for (struct sxb_node *a = target->args; a; a = a->next) given++;
            if (given < 1 || given > 2) {
                ERR(c, target, "Err.Raise takes a number and, optionally, a description");
                return;
            }
            gen_expr_as(c, target->args, SXB_TY_INT, "the error number");
            if (given == 2) {
                gen_expr_as(c, target->args->next, SXB_TY_STRING, "the description");
            } else {
                emit2(c, SXB_OP_PUSH_DEFAULT, SXB_TY_STRING);
            }
            c->line = node->line;
            emit1(c, SXB_OP_RAISE);
            return;
        }
    } else if (target->kind == SXB_N_NAME) {
        if (!resolve(c, target->name, target->name_length, &s)) {
            unknown_name(c, target);
            return;
        }
    } else {
        ERR(c, target, "expected a statement");
        return;
    }
    int length;
    const char *name = node_name(target, &length);
    int type;
    if (s.kind == SYM_PROC) {
        type = gen_call_user(c, target, s.proc, target->args);
    } else if (s.kind == SYM_BUILTIN) {
        type = gen_builtin(c, target, s.builtin, target->args);
    } else if (s.kind == SYM_LOCAL || s.kind == SYM_GLOBAL) {
        ERR(c, target, "\"%.*s\" is a variable; to change it, write %.*s = ...", length, name, length, name);
        return;
    } else {
        ERR(c, target, "\"%.*s\" is not a procedure", length, name);
        return;
    }
    if (!c->failed && type != SXB_TY_VOID) {
        emit1(c, SXB_OP_POP);
    }
}

static void gen_if(struct compiler *c, struct sxb_node *node)
{
    gen_condition(c, node->a, "an If condition");
    c->line = node->line;
    int skip = emit_jump(c, SXB_OP_JF);
    gen_block(c, node->b);
    if (node->flags & SXB_F_HAS_ELSE) {
        int done = emit_jump(c, SXB_OP_JMP);
        patch(c, skip, here(c));
        gen_block(c, node->c);
        patch(c, done, here(c));
    } else {
        patch(c, skip, here(c));
    }
}

static void gen_select(struct compiler *c, struct sxb_node *node)
{
    int subject = gen_expr(c, node->a);
    if (c->failed) return;
    if (subject != SXB_TY_INT && subject != SXB_TY_DOUBLE && subject != SXB_TY_STRING) {
        ERR(c, node->a, "Select Case works on Integer, Double and String, not %s", sxb_type_name(subject));
        return;
    }
    int hidden = c->slot_count++;
    uint32_t location = SXB_LOC_LOCAL(hidden);
    c->line = node->line;
    emit2(c, SXB_OP_STORE, location);
    int end_chain = -1;
    for (struct sxb_node *clause = node->b; clause && !c->failed; clause = clause->next) {
        int body_chain = -1;
        for (struct sxb_node *test = clause->a; test && !c->failed; test = test->next) {
            c->line = test->line;
            emit2(c, SXB_OP_LOAD, location);
            if (test->kind == SXB_N_CASE_RANGE) {
                int low = gen_expr(c, test->a);
                emit_compare(c, test, subject, low, SXB_OP_GE);
                int fail = emit_jump(c, SXB_OP_JF);
                emit2(c, SXB_OP_LOAD, location);
                int high = gen_expr(c, test->b);
                emit_compare(c, test, subject, high, SXB_OP_LE);
                emit_chained_jump(c, SXB_OP_JT, &body_chain);
                patch(c, fail, here(c));
            } else {
                int value = gen_expr(c, test->a);
                emit_compare(c, test, subject, value, test->kind == SXB_N_CASE_IS ? test->op : SXB_OP_EQ);
                emit_chained_jump(c, SXB_OP_JT, &body_chain);
            }
        }
        int next_clause = emit_jump(c, SXB_OP_JMP);
        resolve_chain(c, body_chain, here(c));
        gen_block(c, clause->b);
        emit_chained_jump(c, SXB_OP_JMP, &end_chain);
        patch(c, next_clause, here(c));
    }
    if (node->flags & SXB_F_HAS_ELSE) {
        gen_block(c, node->c);
    }
    resolve_chain(c, end_chain, here(c));
}

static struct loop_ctx *push_loop(struct compiler *c, struct sxb_node *node, int keyword)
{
    if (c->loop_depth == COUNT(c->loops)) {
        ERR(c, node, "loops nest at most %d deep", COUNT(c->loops));
        return NULL;
    }
    struct loop_ctx *loop = &c->loops[c->loop_depth++];
    loop->keyword = keyword;
    loop->try_depth = c->try_depth;
    loop->exit_chain = -1;
    return loop;
}

static void pop_loop(struct compiler *c, int target)
{
    struct loop_ctx *loop = &c->loops[--c->loop_depth];
    resolve_chain(c, loop->exit_chain, target);
}

/* The For variable: a local or global Integer or Double. */
static int loop_variable(struct compiler *c, struct sxb_node *node, struct sym *s, int for_each)
{
    if (!resolve(c, node->name, node->name_length, s)) {
        ERR(c, node, "the loop variable \"%.*s\" is not declared; declare it first with Dim", node->name_length,
            node->name);
        return 0;
    }
    if (s->kind != SYM_LOCAL && s->kind != SYM_GLOBAL) {
        ERR(c, node, "\"%.*s\" is not a variable", node->name_length, node->name);
        return 0;
    }
    if (SXB_TY_IS_ARRAY(s->type) || (!for_each && !is_numeric(s->type))) {
        ERR(c, node, "For counts with an Integer or Double variable, not %s", sxb_type_name(s->type));
        return 0;
    }
    return 1;
}

static void gen_for(struct compiler *c, struct sxb_node *node)
{
    struct sym var;
    if (!loop_variable(c, node, &var, 0)) return;
    int type = var.type;
    gen_expr_as(c, node->a, type, "the start of the For");
    c->line = node->line;
    emit2(c, SXB_OP_STORE, var.location);
    uint32_t end = SXB_LOC_LOCAL(c->slot_count++);
    uint32_t step = SXB_LOC_LOCAL(c->slot_count++);
    gen_expr_as(c, node->b, type, "the end of the For");
    c->line = node->line;
    emit2(c, SXB_OP_STORE, end);
    if (node->c != NULL) {
        gen_expr_as(c, node->c, type, "the Step");
        c->line = node->line;
        emit2(c, SXB_OP_STORE, step);
        emit(c, SXB_OP_STEPCHECK);
        emit(c, step);
        emit(c, (uint32_t)type);
    } else {
        struct sxb_constant one;
        memset(&one, 0, sizeof(one));
        one.type = type;
        one.int_value = 1;
        one.double_value = 1.0;
        emit_constant(c, &one);
        emit2(c, SXB_OP_STORE, step);
    }
    int test = here(c);
    emit(c, type == SXB_TY_INT ? SXB_OP_FORTESTI : SXB_OP_FORTESTD);
    emit(c, var.location);
    emit(c, end);
    emit(c, step);
    int leave = emit_jump(c, SXB_OP_JF);
    if (push_loop(c, node, SXB_KW_FOR) == NULL) return;
    gen_block(c, node->d);
    c->line = node->line;
    emit2(c, SXB_OP_LOAD, var.location);
    emit2(c, SXB_OP_LOAD, step);
    emit1(c, type == SXB_TY_INT ? SXB_OP_ADDI : SXB_OP_ADDD);
    emit2(c, SXB_OP_STORE, var.location);
    emit2(c, SXB_OP_JMP, (uint32_t)test);
    patch(c, leave, here(c));
    pop_loop(c, here(c));
}

static void gen_for_each(struct compiler *c, struct sxb_node *node)
{
    struct sym var;
    if (!loop_variable(c, node, &var, 1)) return;
    int array_type = gen_expr(c, node->a);
    if (c->failed) return;
    if (!SXB_TY_IS_ARRAY(array_type)) {
        ERR(c, node->a, "For Each goes over an array, not %s", sxb_type_name(array_type));
        return;
    }
    if (SXB_TY_BASE(array_type) != var.type) {
        ERR(c, node, "the variable is %s but the array holds %s", sxb_type_name(var.type),
            sxb_type_name(SXB_TY_BASE(array_type)));
        return;
    }
    uint32_t array = SXB_LOC_LOCAL(c->slot_count++);
    uint32_t index = SXB_LOC_LOCAL(c->slot_count++);
    c->line = node->line;
    emit2(c, SXB_OP_STORE, array);
    emit2(c, SXB_OP_PUSH_SMALL, 0);
    emit2(c, SXB_OP_STORE, index);
    int test = here(c);
    emit2(c, SXB_OP_LOAD, index);
    emit2(c, SXB_OP_LOAD, array);
    emit1(c, SXB_OP_UBOUND);
    emit2(c, SXB_OP_CMPI, SXB_OP_LE);
    int leave = emit_jump(c, SXB_OP_JF);
    emit2(c, SXB_OP_LOAD, array);
    emit2(c, SXB_OP_LOAD, index);
    emit1(c, SXB_OP_ALOAD);
    emit2(c, SXB_OP_STORE, var.location);
    if (push_loop(c, node, SXB_KW_FOR) == NULL) return;
    gen_block(c, node->d);
    c->line = node->line;
    emit2(c, SXB_OP_LOAD, index);
    emit2(c, SXB_OP_PUSH_SMALL, 1);
    emit1(c, SXB_OP_ADDI);
    emit2(c, SXB_OP_STORE, index);
    emit2(c, SXB_OP_JMP, (uint32_t)test);
    patch(c, leave, here(c));
    pop_loop(c, here(c));
    /* Let the array go once the loop is over. */
    emit2(c, SXB_OP_PUSH_DEFAULT, (uint32_t)array_type);
    emit2(c, SXB_OP_STORE, array);
}

static void gen_do(struct compiler *c, struct sxb_node *node)
{
    int top = here(c);
    int leave = -1;
    if (node->op == SXB_DO_PRE_WHILE || node->op == SXB_DO_PRE_UNTIL) {
        gen_condition(c, node->a, "a Do condition");
        c->line = node->line;
        leave = emit_jump(c, node->op == SXB_DO_PRE_WHILE ? SXB_OP_JF : SXB_OP_JT);
    }
    if (push_loop(c, node, SXB_KW_DO) == NULL) return;
    gen_block(c, node->d);
    if (node->op == SXB_DO_POST_WHILE || node->op == SXB_DO_POST_UNTIL) {
        gen_condition(c, node->a, "a Loop condition");
        c->line = node->line;
        emit2(c, node->op == SXB_DO_POST_WHILE ? SXB_OP_JT : SXB_OP_JF, (uint32_t)top);
    } else {
        c->line = node->line;
        emit2(c, SXB_OP_JMP, (uint32_t)top);
    }
    if (leave >= 0) patch(c, leave, here(c));
    pop_loop(c, here(c));
}

static void gen_exit(struct compiler *c, struct sxb_node *node)
{
    if (node->op == SXB_KW_FOR || node->op == SXB_KW_DO) {
        for (int i = c->loop_depth - 1; i >= 0; i--) {
            struct loop_ctx *loop = &c->loops[i];
            if (loop->keyword != node->op) continue;
            for (int t = c->try_depth; t > loop->try_depth; t--) emit1(c, SXB_OP_ENDTRY);
            emit_chained_jump(c, SXB_OP_JMP, &loop->exit_chain);
            return;
        }
        ERR(c, node, "Exit %s is not inside a %s loop", sxb_keyword_name(node->op), sxb_keyword_name(node->op));
        return;
    }
    if (node->op == SXB_KW_SUB) {
        if (c->proc == NULL || c->proc->is_function) {
            ERR(c, node, "Exit Sub is for a Sub; in a Function, use Return");
            return;
        }
        emit1(c, SXB_OP_RET);
        return;
    }
    if (c->proc == NULL || !c->proc->is_function) {
        ERR(c, node, "Exit Function is for a Function; in a Sub, use Exit Sub");
        return;
    }
    emit2(c, SXB_OP_PUSH_DEFAULT, (uint32_t)c->proc->return_type);
    emit1(c, SXB_OP_RETV);
}

static void gen_return(struct compiler *c, struct sxb_node *node)
{
    if (c->proc == NULL) {
        ERR(c, node, "Return is for procedures");
        return;
    }
    if (c->proc->is_function) {
        if (node->a == NULL) {
            ERR(c, node, "Return in a Function needs the value it returns");
            return;
        }
        gen_expr_as(c, node->a, c->proc->return_type, "the returned value");
        c->line = node->line;
        emit1(c, SXB_OP_RETV);
        return;
    }
    if (node->a != NULL) {
        ERR(c, node, "a Sub returns nothing; make it a Function to return a value");
        return;
    }
    emit1(c, SXB_OP_RET);
}

static void gen_try(struct compiler *c, struct sxb_node *node)
{
    int handler = emit_jump(c, SXB_OP_TRY);
    c->try_depth++;
    gen_block(c, node->a);
    c->try_depth--;
    c->line = node->line;
    emit1(c, SXB_OP_ENDTRY);
    int done = emit_jump(c, SXB_OP_JMP);
    patch(c, handler, here(c));
    gen_block(c, node->b);
    patch(c, done, here(c));
}

static void gen_redim(struct compiler *c, struct sxb_node *node)
{
    struct sym s;
    if (!resolve(c, node->name, node->name_length, &s)) {
        ERR(c, node, "nothing named \"%.*s\" is declared", node->name_length, node->name);
        return;
    }
    if ((s.kind != SYM_LOCAL && s.kind != SYM_GLOBAL) || !SXB_TY_IS_ARRAY(s.type)) {
        ERR(c, node, "ReDim resizes an array, and \"%.*s\" is not one", node->name_length, node->name);
        return;
    }
    gen_expr_as(c, node->a, SXB_TY_INT, "the new upper bound");
    c->line = node->line;
    emit(c, SXB_OP_REDIM);
    emit(c, s.location);
    emit(c, (uint32_t)SXB_TY_BASE(s.type));
    emit(c, (node->flags & SXB_F_PRESERVE) ? 1u : 0u);
}

static void gen_print(struct compiler *c, struct sxb_node *node)
{
    if (node->a == NULL) {
        emit2(c, SXB_OP_PUSH_DEFAULT, SXB_TY_STRING);
    } else {
        gen_to_string(c, node->a);
        for (struct sxb_node *item = node->a->next; item && !c->failed; item = item->next) {
            gen_to_string(c, item);
            emit1(c, SXB_OP_CONCAT);
        }
    }
    c->line = node->line;
    emit2(c, SXB_OP_PRINT, (node->flags & SXB_F_TRAILING_SEMICOLON) ? 0u : 1u);
}

static void gen_statement(struct compiler *c, struct sxb_node *node)
{
    c->line = node->line;
    c->statement_pending = 1;
    switch (node->kind) {
    case SXB_N_DIM: gen_dim(c, node); break;
    case SXB_N_CONST: gen_local_const(c, node); break;
    case SXB_N_ASSIGN: gen_assign(c, node); break;
    case SXB_N_CALL: gen_call_statement(c, node); break;
    case SXB_N_IF: gen_if(c, node); break;
    case SXB_N_SELECT: gen_select(c, node); break;
    case SXB_N_FOR: gen_for(c, node); break;
    case SXB_N_FOREACH: gen_for_each(c, node); break;
    case SXB_N_DO: gen_do(c, node); break;
    case SXB_N_EXIT: gen_exit(c, node); break;
    case SXB_N_RETURN: gen_return(c, node); break;
    case SXB_N_TRY: gen_try(c, node); break;
    case SXB_N_REDIM: gen_redim(c, node); break;
    case SXB_N_PRINT: gen_print(c, node); break;
    case SXB_N_END: emit1(c, SXB_OP_END); break;
    case SXB_N_UNLOAD:
        if (!c->prog->windows) forms_only(c, node);
        else gen_unload(c, node);
        break;
    default: ERR(c, node, "expected a statement"); break;
    }
}

static void gen_block(struct compiler *c, struct sxb_node *list)
{
    for (struct sxb_node *statement = list; statement && !c->failed; statement = statement->next) {
        gen_statement(c, statement);
    }
}

/* ---- procedures and the program --------------------------------------------------- */

static void reset_locals(struct compiler *c)
{
    for (int i = 0; i < c->local_count; i++) {
        if (c->locals[i].is_const) constant_release(&c->locals[i].value);
    }
    c->local_count = 0;
    c->slot_count = 0;
    c->loop_depth = 0;
    c->try_depth = 0;
}

/* A variable for the debugger (struct sxb_debug_symbol). */
static void add_symbol(struct compiler *c, const char *name, int length, int type, int slot, int proc, int module,
                       int is_public)
{
    struct sxb_program *prog = c->prog;
    if (prog->symbol_count == prog->symbol_capacity) {
        int capacity = prog->symbol_capacity ? prog->symbol_capacity * 2 : 64;
        struct sxb_debug_symbol *grown = realloc(prog->symbols, sizeof(*grown) * (size_t)capacity);
        if (grown == NULL) {
            out_of_memory(c);
            return;
        }
        prog->symbols = grown;
        prog->symbol_capacity = capacity;
    }
    struct sxb_debug_symbol *symbol = &prog->symbols[prog->symbol_count++];
    snprintf(symbol->name, sizeof(symbol->name), "%.*s", length, name);
    symbol->type = type;
    symbol->slot = slot;
    symbol->proc = proc;
    symbol->module = module;
    symbol->is_public = is_public;
}

static void gen_proc(struct compiler *c, struct sxb_proc_decl *proc)
{
    struct sxb_proc_info *info = &c->prog->procs[proc->index];
    c->proc = proc;
    c->module = proc->module;
    c->line = proc->line;
    reset_locals(c);
    int params = 0;
    for (struct sxb_node *p = proc->params; p && !c->failed; p = p->next) {
        add_local(c, p, p->name, p->name_length, p->type);
        params++;
    }
    info->entry = here(c);
    gen_block(c, proc->body);
    if (proc->is_function) {
        emit2(c, SXB_OP_PUSH_DEFAULT, (uint32_t)proc->return_type);
        emit1(c, SXB_OP_RETV);
    } else {
        emit1(c, SXB_OP_RET);
    }
    info->param_count = params;
    info->local_count = c->slot_count;
    for (int i = 0; i < c->local_count; i++) {
        const struct local_sym *l = &c->locals[i];
        if (!l->is_const) add_symbol(c, l->name, l->name_length, l->type, l->slot, proc->index, proc->module, 0);
    }
    reset_locals(c);
    c->proc = NULL;
}

static int add_global(struct compiler *c, struct sxb_node *decl, int module, int is_public)
{
    if (c->global_count == c->global_capacity) {
        int capacity = c->global_capacity ? c->global_capacity * 2 : 32;
        struct global_sym *grown = realloc(c->globals, sizeof(*grown) * (size_t)capacity);
        if (grown == NULL) {
            out_of_memory(c);
            return -1;
        }
        c->globals = grown;
        c->global_capacity = capacity;
    }
    struct global_sym *g = &c->globals[c->global_count++];
    memset(g, 0, sizeof(*g));
    g->name = decl->name;
    g->name_length = decl->name_length;
    g->module = module;
    g->is_public = is_public;
    g->decl = decl;
    g->is_const = decl->kind == SXB_N_CONST;
    g->type = decl->type;
    if (!g->is_const) {
        g->slot = c->prog->global_count++;
    }
    return 0;
}

/* A name declared twice in a module, or Public in two modules. */
static int check_name(struct compiler *c, const char *name, int length, int module, int is_public, int line,
                      int column)
{
    int shared = is_public && c->modules[module].form < 0; /* a module's Public names are the whole project's */
    for (int i = 0; i < c->global_count; i++) {
        struct global_sym *g = &c->globals[i];
        if (!sxb_names_equal(g->name, g->name_length, name, length)) continue;
        if (g->module == module || (shared && g->is_public && c->modules[g->module].form < 0)) goto clash;
    }
    for (int i = 0; i < c->proc_count; i++) {
        struct sxb_proc_decl *p = c->procs[i];
        if (!sxb_names_equal(p->name, p->name_length, name, length)) continue;
        if (p->module == module || (shared && p->is_public && c->modules[p->module].form < 0)) goto clash;
    }
    return 0;
clash:
    c->module = module;
    errorf(c, line, column, "\"%.*s\" is already declared%s", length, name,
           shared ? " (a module's Public names are shared by the whole project)" : " here");
    return -1;
}

static void bind_handlers(struct compiler *c);

static int compile_modules(struct compiler *c, int startup_main)
{
    struct sxb_program *prog = c->prog;
    /* Declarations first, so the bodies can use what is declared further on. */
    int proc_total = 0;
    for (int m = 0; m < c->module_count; m++) {
        for (struct sxb_proc_decl *p = c->modules[m].procs; p; p = p->next) proc_total++;
    }
    c->procs = sxb_arena_alloc(&c->arena, sizeof(*c->procs) * (size_t)(proc_total + 1));
    prog->procs = calloc((size_t)proc_total + 1u + (size_t)prog->form_count, sizeof(*prog->procs));
    if (c->procs == NULL || prog->procs == NULL) {
        out_of_memory(c);
        return -1;
    }
    for (int m = 0; m < c->module_count && !c->failed; m++) {
        for (struct sxb_global_decl *g = c->modules[m].globals; g && !c->failed; g = g->next) {
            if (check_name(c, g->decl->name, g->decl->name_length, m, g->is_public, g->decl->line, g->decl->column) == 0) {
                add_global(c, g->decl, m, g->is_public);
            }
        }
        for (struct sxb_proc_decl *p = c->modules[m].procs; p && !c->failed; p = p->next) {
            if (check_name(c, p->name, p->name_length, m, p->is_public, p->line, p->column) != 0) break;
            p->index = c->proc_count;
            c->procs[c->proc_count++] = p;
            struct sxb_proc_info *info = &prog->procs[p->index];
            snprintf(info->name, sizeof(info->name), "%.*s", p->name_length, p->name);
            info->module = m;
            info->is_function = p->is_function;
        }
    }
    prog->proc_count = c->proc_count + 1;
    prog->module_globals = calloc((size_t)c->module_count + 1u, sizeof(*prog->module_globals));
    if (prog->module_globals == NULL) {
        out_of_memory(c);
        return -1;
    }
    for (int m = 0; m < c->module_count; m++) prog->module_globals[m][0] = -1;
    for (int i = 0; i < c->global_count; i++) {
        struct global_sym *g = &c->globals[i];
        if (g->is_const) continue;
        if (prog->module_globals[g->module][0] < 0) prog->module_globals[g->module][0] = g->slot;
        prog->module_globals[g->module][1]++;
    }
    bind_handlers(c);
    /* Constants, in the order they are written. */
    for (int i = 0; i < c->global_count && !c->failed; i++) {
        struct global_sym *g = &c->globals[i];
        if (!g->is_const) continue;
        c->module = g->module;
        if (eval_const(c, g->decl->a, &g->value) != 0) break;
        if (const_to_type(c, g->decl, &g->value, g->decl->type) != 0) break;
        g->type = g->value.type;
        g->evaluated = 1;
    }
    for (int i = 0; i < c->proc_count && !c->failed; i++) {
        gen_proc(c, c->procs[i]);
    }
    if (c->failed) return -1;

    /* Where it starts: the globals' initial values, then Sub Main. */
    struct sxb_proc_decl *main_proc = NULL;
    if (startup_main) {
        for (int i = 0; i < c->proc_count; i++) {
            struct sxb_proc_decl *p = c->procs[i];
            if (!sxb_name_equal(p->name, p->name_length, "Main")) continue;
            if (main_proc != NULL) {
                c->module = p->module;
                errorf(c, p->line, p->column, "two modules have a Main; the program starts at one Sub Main");
                return -1;
            }
            main_proc = p;
        }
        c->module = 0;
        if (main_proc == NULL) {
            errorf(c, 0, 0, "the program starts at Sub Main, and no module has one");
            return -1;
        }
        if (main_proc->is_function || main_proc->params != NULL) {
            c->module = main_proc->module;
            errorf(c, main_proc->line, main_proc->column, "Main has to be a Sub with no parameters");
            return -1;
        }
    }
    struct sxb_proc_info *entry = &prog->procs[c->proc_count];
    snprintf(entry->name, sizeof(entry->name), "(start)");
    entry->entry = here(c);
    c->proc = NULL;
    reset_locals(c);
    for (int i = 0; i < c->global_count && !c->failed; i++) {
        struct global_sym *g = &c->globals[i];
        if (g->is_const || c->modules[g->module].form >= 0) continue;
        struct sxb_node *decl = g->decl;
        c->module = g->module;
        c->line = decl->line;
        if (decl->flags & SXB_F_FIXED_SIZE) {
            gen_expr_as(c, decl->a, SXB_TY_INT, "the array's upper bound");
            c->line = decl->line;
            emit2(c, SXB_OP_ANEW, (uint32_t)SXB_TY_BASE(decl->type));
            emit2(c, SXB_OP_STORE, SXB_LOC_GLOBAL(g->slot));
        } else if (decl->b != NULL) {
            gen_expr_as(c, decl->b, decl->type, "the initial value");
            c->line = decl->line;
            emit2(c, SXB_OP_STORE, SXB_LOC_GLOBAL(g->slot));
        }
    }
    if (main_proc != NULL) {
        c->module = main_proc->module;
        c->line = main_proc->line;
        emit2(c, SXB_OP_CALL, (uint32_t)main_proc->index);
    } else if (prog->startup_form >= 0) {
        c->module = prog->form_modules[prog->startup_form];
        c->line = 1;
        emit(c, SXB_OP_CALLM);
        emit(c, (uint32_t)prog->startup_form);
        emit(c, (uint32_t)-1);
        emit(c, SXB_M_SHOW);
        emit(c, 0);
        emit(c, SXB_TY_VOID);
    }
    /* A console program ends here; a Windows one waits for its events. */
    emit1(c, prog->windows ? SXB_OP_RET : SXB_OP_END);
    entry->local_count = c->slot_count;
    entry->module = 0;
    prog->entry_proc = c->proc_count;
    reset_locals(c);
    /* Each form's variables start over every time it loads: their initial
     * values are a procedure of their own, the form's (init). */
    for (int f = 0; f < prog->form_count && !c->failed; f++) {
        struct sxb_proc_info *init = &prog->procs[c->proc_count + 1 + f];
        int module = prog->form_modules[f];
        snprintf(init->name, sizeof(init->name), "(%s)", prog->forms[f].name);
        init->module = module;
        init->entry = here(c);
        c->module = module;
        for (int i = 0; i < c->global_count && !c->failed; i++) {
            struct global_sym *g = &c->globals[i];
            if (g->is_const || g->module != module) continue;
            struct sxb_node *decl = g->decl;
            c->line = decl->line;
            if (decl->flags & SXB_F_FIXED_SIZE) {
                gen_expr_as(c, decl->a, SXB_TY_INT, "the array's upper bound");
                c->line = decl->line;
                emit2(c, SXB_OP_ANEW, (uint32_t)SXB_TY_BASE(decl->type));
                emit2(c, SXB_OP_STORE, SXB_LOC_GLOBAL(g->slot));
            } else if (decl->b != NULL) {
                gen_expr_as(c, decl->b, decl->type, "the initial value");
                c->line = decl->line;
                emit2(c, SXB_OP_STORE, SXB_LOC_GLOBAL(g->slot));
            }
        }
        emit1(c, SXB_OP_RET);
        init->local_count = c->slot_count;
        reset_locals(c);
    }
    prog->proc_count = c->proc_count + 1 + prog->form_count;
    for (int i = 0; i < c->global_count; i++) {
        const struct global_sym *g = &c->globals[i];
        if (!g->is_const) add_symbol(c, g->name, g->name_length, g->type, g->slot, -1, g->module, g->is_public);
    }
    return c->failed ? -1 : 0;
}

static int parse_module_text(struct compiler *c, int index, const char *text, size_t length, int first_line)
{
    struct sxb_token *tokens;
    int count;
    struct sxb_module_ast *module = &c->modules[index];
    /* The tokens and the tree point into the text: keep it as long as they live. */
    text = sxb_arena_strndup(&c->arena, text, length);
    if (text == NULL) {
        out_of_memory(c);
        return -1;
    }
    if (sxb_lex(&c->arena, module->file, text, length, first_line, &tokens, &count, c->diag) != 0) {
        c->failed = 1;
        return -1;
    }
    if (sxb_parse_module(&c->arena, module->file, tokens, count, index, module, c->diag) != 0) {
        c->failed = 1;
        return -1;
    }
    return 0;
}

static void compiler_free(struct compiler *c)
{
    for (int i = 0; i < c->global_count; i++) {
        if (c->globals[i].is_const && c->globals[i].evaluated) constant_release(&c->globals[i].value);
    }
    reset_locals(c);
    free(c->globals);
    free(c->locals);
    sxb_arena_free(&c->arena);
}

static struct sxb_program *finish(struct compiler *c, struct sxb_program *prog)
{
    if (!c->failed) {
        prog->module_count = c->module_count;
        prog->module_files = calloc((size_t)c->module_count + 1u, sizeof(*prog->module_files));
        if (prog->module_files == NULL) {
            c->failed = 1;
            snprintf(c->diag->message, sizeof(c->diag->message), "out of memory");
        } else {
            for (int i = 0; i < c->module_count; i++) {
                snprintf(prog->module_files[i], sizeof(prog->module_files[i]), "%s", c->modules[i].file);
            }
        }
    }
    compiler_free(c);
    if (c->failed) {
        sxb_program_free(prog);
        return NULL;
    }
    return prog;
}

static int valid_identifier(const char *name, size_t length)
{
    if (length == 0 || length > 63) return 0;
    if (!((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z'))) return 0;
    for (size_t i = 1; i < length; i++) {
        char ch = name[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return 0;
    }
    return 1;
}

static int is_reserved(const char *name, size_t length)
{
    for (int k = 0; k < SXB_KW_COUNT; k++) {
        if (sxb_name_equal(name, (int)length, sxb_keyword_name(k))) return 1;
    }
    return 0;
}

static struct sxb_program *new_program(struct sxb_diagnostic *diag)
{
    struct sxb_program *prog = calloc(1, sizeof(*prog));
    if (prog == NULL) {
        snprintf(diag->message, sizeof(diag->message), "out of memory");
    }
    return prog;
}

struct sxb_program *sxb_compile_source(const char *name, const char *text, size_t length,
                                       struct sxb_diagnostic *diag)
{
    memset(diag, 0, sizeof(*diag));
    struct sxb_program *prog = new_program(diag);
    if (prog == NULL) return NULL;
    struct compiler c;
    memset(&c, 0, sizeof(c));
    c.prog = prog;
    c.diag = diag;
    c.module = -1;
    c.modules = sxb_arena_alloc(&c.arena, sizeof(*c.modules));
    if (c.modules == NULL) {
        compiler_free(&c);
        free(prog);
        snprintf(diag->message, sizeof(diag->message), "out of memory");
        return NULL;
    }
    c.module_count = 1;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    snprintf(c.modules[0].file, sizeof(c.modules[0].file), "%s", base);
    size_t stem = strlen(base);
    const char *dot = strrchr(base, '.');
    if (dot) stem = (size_t)(dot - base);
    snprintf(c.modules[0].name, sizeof(c.modules[0].name), "%.*s", (int)(stem < 63 ? stem : 63), base);
    snprintf(prog->name, sizeof(prog->name), "%s", c.modules[0].name);
    c.modules[0].form = -1;
    prog->startup_form = -1;
    if (parse_module_text(&c, 0, text, length, 1) == 0) {
        compile_modules(&c, 1);
    }
    return finish(&c, prog);
}

/* ---- the project file ------------------------------------------------------------------ */

#define MAX_MODULES 64

struct project {
    char name[64];
    int windows;
    int startup_main;
    int has_kind;
    int has_startup;
    char modules[MAX_MODULES][128]; /* forms and modules, in the project's order */
    int module_lines[MAX_MODULES];
    int module_is_form[MAX_MODULES];
    int module_count;
    int form_count;
    char startup[64];
    struct sxb_project_info info;
};

struct project_value {
    int kind; /* 'S' string, 'I' integer, 'W' word */
    char text[256];
    int64_t number;
};

static int project_error(struct sxb_diagnostic *diag, const char *file, int line, const char *format, ...)
{
    snprintf(diag->file, sizeof(diag->file), "%s", file);
    diag->line = line;
    diag->column = 0;
    va_list args;
    va_start(args, format);
    vsnprintf(diag->message, sizeof(diag->message), format, args);
    va_end(args);
    return -1;
}

static int parse_project_value(const char *p, const char *end, struct project_value *out)
{
    memset(out, 0, sizeof(*out));
    if (p < end && *p == '"') {
        size_t used = 0;
        p++;
        for (;;) {
            if (p >= end) return -1;
            if (*p == '"') {
                if (p + 1 < end && p[1] == '"') {
                    p++;
                } else {
                    p++;
                    break;
                }
            }
            if (used + 1 >= sizeof(out->text)) return -1;
            out->text[used++] = *p++;
        }
        out->kind = 'S';
    } else if (p + 1 < end && p[0] == '&' && (p[1] == 'H' || p[1] == 'h')) {
        p += 2;
        int digits = 0;
        uint64_t value = 0;
        while (p < end) {
            char ch = *p;
            int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                    : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
            if (v < 0) break;
            if (value > 0x7FFFFFFFFFFFFFFull) return -1;
            value = value * 16u + (uint64_t)v;
            digits++;
            p++;
        }
        if (!digits) return -1;
        out->kind = 'I';
        out->number = (int64_t)value;
    } else if (p < end && *p >= '0' && *p <= '9') {
        uint64_t value = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            if (value > 900000000000000000ull) return -1;
            value = value * 10u + (uint64_t)(*p++ - '0');
        }
        out->kind = 'I';
        out->number = (int64_t)value;
    } else {
        size_t used = 0;
        while (p < end && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) {
            if (used + 1 >= sizeof(out->text)) return -1;
            out->text[used++] = *p++;
        }
        if (!used) return -1;
        out->kind = 'W';
    }
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p < end && *p != '\'') return -1;
    return 0;
}

static int valid_version(const char *text)
{
    int parts = 0, digits = 0;
    long part = 0;
    for (const char *p = text;; p++) {
        if (*p >= '0' && *p <= '9') {
            digits++;
            part = part * 10 + (*p - '0');
            if (digits > 5 || part > 65535) return 0;
            continue;
        }
        if (digits == 0) return 0;
        parts++;
        digits = 0;
        part = 0;
        if (*p == '\0') break;
        if (*p != '.') return 0;
    }
    return parts >= 1 && parts <= 4;
}

static int parse_project(const char *file, const char *text, size_t length, struct project *out,
                         struct sxb_diagnostic *diag)
{
    memset(out, 0, sizeof(*out));
    const char *p = text, *end = text + length;
    int line = 0;
    int has_name = 0;
    if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        p += 3;
    }
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        const char *next_line = eol ? eol + 1 : end;
        line++;
        if (line_end > p && line_end[-1] == '\r') line_end--;
        while (p < line_end && (*p == ' ' || *p == '\t')) p++;
        const char *trimmed_end = line_end;
        while (trimmed_end > p && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) trimmed_end--;
        if (line == 1) {
            const char *header = "SxBASIC Project ";
            size_t header_length = strlen(header);
            if ((size_t)(trimmed_end - p) <= header_length || memcmp(p, header, header_length) != 0) {
                return project_error(diag, file, line, "a project file starts with \"SxBASIC Project 1\"");
            }
            if (!((size_t)(trimmed_end - p) == header_length + 1 && p[header_length] == '1')) {
                return project_error(diag, file, line,
                                     "this project is in format \"%.*s\"; this SxBASIC reads format 1",
                                     (int)(trimmed_end - p - (long)header_length), p + header_length);
            }
            p = next_line;
            continue;
        }
        if (p == trimmed_end || *p == '\'') {
            p = next_line;
            continue;
        }
        const char *key = p;
        while (p < trimmed_end && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))) p++;
        int key_length = (int)(p - key);
        while (p < trimmed_end && (*p == ' ' || *p == '\t')) p++;
        if (key_length == 0 || p >= trimmed_end || *p != '=') {
            return project_error(diag, file, line, "expected \"Key = value\"");
        }
        p++;
        while (p < trimmed_end && (*p == ' ' || *p == '\t')) p++;
        struct project_value value;
        if (parse_project_value(p, trimmed_end, &value) != 0) {
            return project_error(diag, file, line, "the value of %.*s is not a string, a number or a word", key_length,
                                 key);
        }
#define KEY(k) sxb_name_equal(key, key_length, k)
#define WANT(t, what)                                                                              \
    do {                                                                                           \
        if (value.kind != (t)) return project_error(diag, file, line, "%.*s takes %s", key_length, key, what); \
    } while (0)
        if (KEY("Name")) {
            WANT('S', "a string");
            size_t n = strlen(value.text);
            if (n == 0 || n > 63) return project_error(diag, file, line, "Name is 1 to 63 characters");
            for (size_t i = 0; i < n; i++) {
                char ch = value.text[i];
                if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == ' ' ||
                      ch == '-' || ch == '_')) {
                    return project_error(diag, file, line, "Name takes letters, digits, spaces, \"-\" and \"_\"");
                }
            }
            snprintf(out->name, sizeof(out->name), "%s", value.text);
            has_name = 1;
        } else if (KEY("Kind")) {
            WANT('W', "Console or Windows");
            if (sxb_name_equal(value.text, (int)strlen(value.text), "Console")) out->windows = 0;
            else if (sxb_name_equal(value.text, (int)strlen(value.text), "Windows")) out->windows = 1;
            else return project_error(diag, file, line, "Kind is Console or Windows");
            out->has_kind = 1;
        } else if (KEY("Startup")) {
            WANT('W', "Main or a form's name");
            out->startup_main = sxb_name_equal(value.text, (int)strlen(value.text), "Main");
            snprintf(out->startup, sizeof(out->startup), "%s", value.text);
            out->has_startup = 1;
        } else if (KEY("Module")) {
            WANT('S', "a file name in quotes");
            size_t n = strlen(value.text);
            if (n < 5 || !sxb_name_equal(value.text + n - 4, 4, ".bas")) {
                return project_error(diag, file, line, "a module is a .bas file");
            }
            if (out->module_count == MAX_MODULES) {
                return project_error(diag, file, line, "a project has at most %d modules", MAX_MODULES);
            }
            snprintf(out->modules[out->module_count], sizeof(out->modules[0]), "%s", value.text);
            out->module_lines[out->module_count++] = line;
        } else if (KEY("Form")) {
            WANT('S', "a file name in quotes");
            size_t n = strlen(value.text);
            if (n < 5 || !sxb_name_equal(value.text + n - 4, 4, ".sbf")) {
                return project_error(diag, file, line, "a form is a .sbf file");
            }
            if (out->module_count == MAX_MODULES) {
                return project_error(diag, file, line, "a project has at most %d forms and modules", MAX_MODULES);
            }
            snprintf(out->modules[out->module_count], sizeof(out->modules[0]), "%s", value.text);
            out->module_is_form[out->module_count] = 1;
            out->module_lines[out->module_count++] = line;
            out->form_count++;
        } else if (KEY("Description") || KEY("Vendor") || KEY("Copyright")) {
            WANT('S', "a string");
            char *field = KEY("Description") ? out->info.description
                          : KEY("Vendor")    ? out->info.vendor
                                             : out->info.copyright;
            snprintf(field, sizeof(out->info.description), "%s", value.text);
        } else if (KEY("Category")) {
            WANT('S', "a string");
            if (strlen(value.text) >= sizeof(out->info.category)) {
                return project_error(diag, file, line, "a Category is at most %d bytes",
                                     (int)sizeof(out->info.category) - 1);
            }
            snprintf(out->info.category, sizeof(out->info.category), "%s", value.text);
        } else if (KEY("Icon")) {
            WANT('S', "a PNG file name in quotes");
            size_t n = strlen(value.text);
            if (n < 5 || !sxb_name_equal(value.text + n - 4, 4, ".png") || n >= sizeof(out->info.icons[0])) {
                return project_error(diag, file, line, "an Icon is a .png file");
            }
            if (out->info.icon_count == SXB_MAX_ICONS) {
                return project_error(diag, file, line, "a project has at most %d icons", SXB_MAX_ICONS);
            }
            snprintf(out->info.icons[out->info.icon_count++], sizeof(out->info.icons[0]), "%s", value.text);
        } else if (KEY("Version")) {
            WANT('S', "a string such as \"1.2.3\"");
            if (!valid_version(value.text) || strlen(value.text) >= sizeof(out->info.version)) {
                return project_error(diag, file, line, "Version is up to four numbers below 65536: \"1.2.3\"");
            }
            snprintf(out->info.version, sizeof(out->info.version), "%s", value.text);
        } else if (KEY("Accent")) {
            WANT('I', "a colour as &HRRGGBB");
            if (value.number < 0 || value.number > 0xFFFFFF) {
                return project_error(diag, file, line, "Accent is a colour from &H000000 to &HFFFFFF");
            }
            out->info.has_accent = 1;
            out->info.accent = (uint32_t)value.number;
        } else {
            return project_error(diag, file, line, "unknown key \"%.*s\"", key_length, key);
        }
#undef KEY
#undef WANT
        p = next_line;
    }
    if (line == 0) return project_error(diag, file, 1, "the project file is empty");
    if (!has_name) return project_error(diag, file, 0, "the project has no Name");
    if (!out->has_kind) return project_error(diag, file, 0, "the project has no Kind (Console or Windows)");
    if (!out->has_startup) return project_error(diag, file, 0, "the project has no Startup");
    if (!out->windows && !out->startup_main) {
        return project_error(diag, file, 0, "a console program starts at Sub Main: Startup = Main");
    }
    if (!out->windows && out->form_count > 0) {
        return project_error(diag, file, 0, "a console program has no forms; make it Kind = Windows");
    }
    if (out->module_count == 0) return project_error(diag, file, 0, "the project has no forms and no modules");
    return 0;
}

/* ---- the form file ----------------------------------------------------------------
 *
 * "SxBASIC Form 1", a Begin Form block of controls and their properties, then
 * the form's code, read as a module whose lines keep their numbers. */

static int form_error(struct compiler *c, const char *file, int line, const char *format, ...)
{
    if (c->failed) return -1;
    c->failed = 1;
    snprintf(c->diag->file, sizeof(c->diag->file), "%s", file);
    c->diag->line = line;
    c->diag->column = 0;
    va_list args;
    va_start(args, format);
    vsnprintf(c->diag->message, sizeof(c->diag->message), format, args);
    va_end(args);
    return -1;
}

static int set_form_property(struct compiler *c, const char *file, int line, struct sxb_control_def *control,
                             const char *key, int key_length, const char *p, const char *end)
{
    int property = sxb_find_property(key, key_length);
    int access = property >= 0 ? sxb_property_access(property, control->kind) : SXB_ACCESS_NONE;
    if (property < 0 || access == SXB_ACCESS_NONE || property == SXB_P_LISTCOUNT) {
        return form_error(c, file, line, "a %s has no property %.*s", sxb_control_kind_name(control->kind), key_length, key);
    }
    int negative = 0;
    if (p < end && *p == '-') {
        negative = 1;
        p++;
    }
    struct project_value value;
    if (parse_project_value(p, end, &value) != 0) {
        return form_error(c, file, line, "the value of %.*s is not a string, a number, True or False", key_length, key);
    }
    int type = sxb_property_type(property, control->kind);
    if (type == SXB_TY_STRING) {
        if (value.kind != 'S' || negative) return form_error(c, file, line, "%.*s takes a string in quotes", key_length, key);
        snprintf(control->text, sizeof(control->text), "%s", value.text);
        return 0;
    }
    if (type == SXB_TY_BOOL) {
        int is_true = value.kind == 'W' && sxb_name_equal(value.text, (int)strlen(value.text), "True");
        int is_false = value.kind == 'W' && sxb_name_equal(value.text, (int)strlen(value.text), "False");
        if ((!is_true && !is_false) || negative) return form_error(c, file, line, "%.*s is True or False", key_length, key);
        control->values[property] = is_true;
        return 0;
    }
    if (value.kind != 'I') return form_error(c, file, line, "%.*s takes a whole number", key_length, key);
    control->values[property] = negative ? -value.number : value.number;
    return 0;
}

static int parse_form_text(struct compiler *c, int module_index, int form_index, const char *file, const char *text,
                           size_t length)
{
    struct sxb_form_def *form = &c->prog->forms[form_index];
    struct sxb_module_ast *module = &c->modules[module_index];
    const char *p = text, *end = text + length;
    int line = 0;
    int depth = 0;           /* 0 before Begin Form, 1 inside it, 2 inside a control, 3 inside a menu's item */
    int current = -1;        /* the control being filled */
    int current_menu = -1;
    int capacity = 0;
    const char *code = NULL;
    int code_line = 0;
    if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        p += 3;
    }
    snprintf(module->file, sizeof(module->file), "%s", file);
    module->form = form_index;
    while (p < end && code == NULL) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = eol ? eol : end;
        const char *next_line = eol ? eol + 1 : end;
        line++;
        if (line_end > p && line_end[-1] == '\r') line_end--;
        while (p < line_end && (*p == ' ' || *p == '\t')) p++;
        const char *trimmed_end = line_end;
        while (trimmed_end > p && (trimmed_end[-1] == ' ' || trimmed_end[-1] == '\t')) trimmed_end--;
        int n = (int)(trimmed_end - p);
        if (line == 1) {
            if (!(n == 14 && memcmp(p, "SxBASIC Form 1", 14) == 0)) {
                return form_error(c, file, line, "a form file starts with \"SxBASIC Form 1\"");
            }
            p = next_line;
            continue;
        }
        if (n == 0 || *p == '\'') {
            p = next_line;
            continue;
        }
        const char *word = p;
        while (p < trimmed_end && *p != ' ' && *p != '\t' && *p != '=') p++;
        int word_length = (int)(p - word);
        while (p < trimmed_end && (*p == ' ' || *p == '\t')) p++;
        if (sxb_name_equal(word, word_length, "Begin")) {
            const char *kind_name = p;
            while (p < trimmed_end && *p != ' ' && *p != '\t') p++;
            int kind_length = (int)(p - kind_name);
            while (p < trimmed_end && (*p == ' ' || *p == '\t')) p++;
            const char *name = p;
            int name_length = (int)(trimmed_end - p);
            int kind = sxb_find_control_kind(kind_name, kind_length);
            if (sxb_name_equal(kind_name, kind_length, "PictureBox")) {
                return form_error(c, file, line, "PictureBox is not supported yet: SXGUI has no picture widget");
            }
            if (kind < 0) return form_error(c, file, line, "there is no control called %.*s", kind_length, kind_name);
            if (!valid_identifier(name, (size_t)name_length) || is_reserved(name, (size_t)name_length) ||
                sxb_name_equal(name, name_length, "Form")) {
                return form_error(c, file, line, "\"%.*s\" cannot name a control: use a letter, then letters, digits and \"_\"",
                                  name_length, name);
            }
            if (depth == 0) {
                if (kind != SXB_CTL_FORM) return form_error(c, file, line, "the file starts with Begin Form <name>");
                sxb_control_defaults(SXB_CTL_FORM, &form->form);
                snprintf(form->name, sizeof(form->name), "%.*s", name_length, name);
                snprintf(form->form.name, sizeof(form->form.name), "%.*s", name_length, name);
                snprintf(module->name, sizeof(module->name), "%.*s", name_length, name);
                depth = 1;
                p = next_line;
                continue;
            }
            int allowed = (depth == 1 && kind != SXB_CTL_FORM && kind != SXB_CTL_MENUITEM) ||
                          (depth == 2 && current >= 0 && form->controls[current].kind == SXB_CTL_MENU &&
                           kind == SXB_CTL_MENUITEM);
            if (!allowed) {
                return form_error(c, file, line,
                                  kind == SXB_CTL_MENUITEM ? "a MenuItem goes inside a Menu"
                                                           : "controls do not nest: end the one above first");
            }
            if (find_control(form, name, name_length) >= 0) {
                return form_error(c, file, line, "two controls are named %.*s", name_length, name);
            }
            if (form->control_count == capacity) {
                capacity = capacity ? capacity * 2 : 16;
                struct sxb_control_def *grown = realloc(form->controls, sizeof(*grown) * (size_t)capacity);
                if (grown == NULL) return form_error(c, file, line, "out of memory");
                form->controls = grown;
            }
            struct sxb_control_def *control = &form->controls[form->control_count];
            sxb_control_defaults(kind, control);
            snprintf(control->name, sizeof(control->name), "%.*s", name_length, name);
            if (kind == SXB_CTL_MENU) current_menu = form->control_count;
            if (kind == SXB_CTL_MENUITEM) control->menu = current_menu;
            current = form->control_count++;
            depth++;
            p = next_line;
            continue;
        }
        if (sxb_name_equal(word, word_length, "End") && p == trimmed_end) {
            if (depth == 0) return form_error(c, file, line, "End without a Begin");
            depth--;
            if (depth == 1) current = -1;
            if (depth == 2) current = current_menu;
            if (depth == 0) {
                code = next_line;
                code_line = line + 1;
            }
            p = next_line;
            continue;
        }
        if (depth == 0) return form_error(c, file, line, "expected Begin Form <name>");
        if (p >= trimmed_end || *p != '=') return form_error(c, file, line, "expected \"Property = value\"");
        p++;
        while (p < trimmed_end && (*p == ' ' || *p == '\t')) p++;
        struct sxb_control_def *target = current >= 0 ? &form->controls[current] : &form->form;
        if (set_form_property(c, file, line, target, word, word_length, p, trimmed_end) != 0) return -1;
        p = next_line;
    }
    if (code == NULL) {
        return form_error(c, file, line, depth == 0 ? "the file has no Begin Form block" : "the Begin Form block has no End");
    }
    if (form->form.values[SXB_P_WIDTH] < 64 || form->form.values[SXB_P_HEIGHT] < 32 ||
        form->form.values[SXB_P_WIDTH] > 4096 || form->form.values[SXB_P_HEIGHT] > 4096) {
        return form_error(c, file, 0, "the form's Width and Height are 64 by 32 to 4096 by 4096");
    }
    return parse_module_text(c, module_index, code, (size_t)(end - code), code_line);
}

/* Binds Control_Event procedures to their controls, and refuses a misspelt one. */
static void bind_handlers(struct compiler *c)
{
    struct sxb_program *prog = c->prog;
    for (int f = 0; f < prog->form_count && !c->failed; f++) {
        struct sxb_form_def *form = &prog->forms[f];
        int module = prog->form_modules[f];
        c->module = module;
        for (int k = 0; k < form->control_count && !c->failed; k++) {
            struct sym s;
            memset(&s, 0, sizeof(s));
            const char *name = form->controls[k].name;
            if (resolve_in_module(c, name, (int)strlen(name), module, 0, &s)) {
                errorf(c, 0, 0, "the control %s has the name of a variable or procedure of its form", name);
            }
        }
        for (int i = 0; i < c->proc_count && !c->failed; i++) {
            struct sxb_proc_decl *proc = c->procs[i];
            if (proc->module != module) continue;
            int split = -1;
            for (int j = proc->name_length - 1; j > 0; j--) {
                if (proc->name[j] == '_') {
                    split = j;
                    break;
                }
            }
            if (split <= 0) continue;
            struct sxb_control_def *target = NULL;
            if (sxb_name_equal(proc->name, split, "Form")) {
                target = &form->form;
            } else {
                int k = find_control(form, proc->name, split);
                if (k >= 0) target = &form->controls[k];
            }
            if (target == NULL) continue;
            const char *event_name = proc->name + split + 1;
            int event_length = proc->name_length - split - 1;
            int event = sxb_find_event(event_name, event_length);
            if (event < 0 || !sxb_event_applies(event, target->kind)) {
                errorf(c, proc->line, proc->column, "a %s has no event %.*s, so %.*s would never run",
                       sxb_control_kind_name(target->kind), event_length, event_name, proc->name_length, proc->name);
                break;
            }
            int params = 0;
            struct sxb_node *param = proc->params;
            for (struct sxb_node *q = proc->params; q; q = q->next) params++;
            if (proc->is_function) {
                errorf(c, proc->line, proc->column, "an event's procedure is a Sub");
            } else if (event == SXB_EV_UNLOAD &&
                       (params != 1 || !(param->flags & SXB_F_BYREF) || param->type != SXB_TY_BOOL)) {
                errorf(c, proc->line, proc->column, "Form_Unload takes (ByRef Cancel As Boolean)");
            } else if (event != SXB_EV_UNLOAD && params != 0) {
                errorf(c, proc->line, proc->column, "%.*s takes no parameters", proc->name_length, proc->name);
            }
            target->handlers[event] = proc->index;
        }
    }
}

struct sxb_program *sxb_compile_project(const char *project_path, const struct sxb_host *host,
                                        struct sxb_diagnostic *diag)
{
    memset(diag, 0, sizeof(*diag));
    const char *slash = strrchr(project_path, '/');
    const char *file = slash ? slash + 1 : project_path;
    size_t directory_length = slash ? (size_t)(slash - project_path) : 0;
    char *text = NULL;
    size_t size = 0;
    int error = host->read_file(host->user, project_path, &text, &size);
    if (error != 0) {
        project_error(diag, file, 0, "cannot read the project file (error %d)", error);
        return NULL;
    }
    struct project *project = malloc(sizeof(*project));
    if (project == NULL) {
        free(text);
        project_error(diag, file, 0, "out of memory");
        return NULL;
    }
    if (parse_project(file, text, size, project, diag) != 0) {
        free(text);
        free(project);
        return NULL;
    }
    free(text);
    struct sxb_program *prog = new_program(diag);
    if (prog == NULL) {
        free(project);
        return NULL;
    }
    snprintf(prog->name, sizeof(prog->name), "%s", project->name);
    prog->windows = project->windows;
    prog->info = project->info;
    struct compiler c;
    memset(&c, 0, sizeof(c));
    c.prog = prog;
    c.diag = diag;
    c.module = -1;
    c.modules = sxb_arena_alloc(&c.arena, sizeof(*c.modules) * (size_t)project->module_count);
    if (c.modules == NULL) {
        project_error(diag, file, 0, "out of memory");
        c.failed = 1;
    }
    prog->startup_form = -1;
    if (!c.failed && project->form_count > 0) {
        prog->forms = calloc((size_t)project->form_count, sizeof(*prog->forms));
        prog->form_modules = calloc((size_t)project->form_count, sizeof(*prog->form_modules));
        if (prog->forms == NULL || prog->form_modules == NULL) {
            project_error(diag, file, 0, "out of memory");
            c.failed = 1;
        }
    }
    for (int i = 0; i < project->module_count && !c.failed; i++) {
        const char *relative = project->modules[i];
        const char *base = strrchr(relative, '/');
        base = base ? base + 1 : relative;
        size_t stem = strlen(base) - 4;
        int is_form = project->module_is_form[i];
        if (!is_form && (!valid_identifier(base, stem) || is_reserved(base, stem))) {
            project_error(diag, file, project->module_lines[i],
                          "a module's file name is its name, so \"%s\" has to be a name: a letter, then letters, "
                          "digits and \"_\"",
                          base);
            c.failed = 1;
            break;
        }
        c.modules[i].form = -1;
        snprintf(c.modules[i].name, sizeof(c.modules[i].name), "%.*s", (int)stem, base);
        snprintf(c.modules[i].file, sizeof(c.modules[i].file), "%s", relative);
        c.module_count = i + 1;
        char path[512];
        if (relative[0] == '/') {
            snprintf(path, sizeof(path), "%s", relative);
        } else {
            snprintf(path, sizeof(path), "%.*s/%s", (int)directory_length, project_path, relative);
        }
        error = host->read_file(host->user, path, &text, &size);
        if (error != 0) {
            project_error(diag, file, project->module_lines[i], "cannot read %s (error %d)", relative, error);
            c.failed = 1;
            break;
        }
        if (is_form) {
            int form = prog->form_count++;
            prog->form_modules[form] = i;
            parse_form_text(&c, i, form, relative, text, size);
        } else {
            parse_module_text(&c, i, text, size, 1);
        }
        free(text);
        for (int j = 0; j < i && !c.failed; j++) {
            if (sxb_name_equal(c.modules[i].name, (int)strlen(c.modules[i].name), c.modules[j].name)) {
                project_error(diag, file, project->module_lines[i], "two forms or modules are named %s",
                              c.modules[i].name);
                c.failed = 1;
            }
        }
    }
    if (!c.failed && project->windows && !project->startup_main) {
        for (int f = 0; f < prog->form_count; f++) {
            if (sxb_name_equal(project->startup, (int)strlen(project->startup), prog->forms[f].name)) {
                prog->startup_form = f;
            }
        }
        if (prog->startup_form < 0) {
            project_error(diag, file, 0, "Startup names %s, and no form is called that", project->startup);
            c.failed = 1;
        }
    }
    if (!c.failed) {
        compile_modules(&c, project->startup_main);
    }
    free(project);
    return finish(&c, prog);
}

void sxb_program_free(struct sxb_program *program)
{
    if (program == NULL) return;
    for (int i = 0; i < program->constant_count; i++) {
        if (program->constants[i].type == SXB_TY_STRING) sxb_string_release(program->constants[i].string);
    }
    free(program->constants);
    free(program->code);
    free(program->lines);
    free(program->modules);
    free(program->statements);
    free(program->symbols);
    free(program->procs);
    free(program->module_files);
    free(program->module_globals);
    for (int f = 0; f < program->form_count; f++) free(program->forms[f].controls);
    free(program->forms);
    free(program->form_modules);
    free(program);
}

const char *sxb_program_name(const struct sxb_program *program)
{
    return program->name;
}

const struct sxb_project_info *sxb_program_info(const struct sxb_program *program)
{
    return &program->info;
}

int sxb_program_is_windows(const struct sxb_program *program)
{
    return program->windows;
}

int sxb_program_form_count(const struct sxb_program *program)
{
    return program->form_count;
}

const struct sxb_form_def *sxb_program_form(const struct sxb_program *program, int form)
{
    return form >= 0 && form < program->form_count ? &program->forms[form] : NULL;
}

int sxb_program_startup_form(const struct sxb_program *program)
{
    return program->startup_form;
}

int sxb_program_form_init(const struct sxb_program *program, int form)
{
    return program->entry_proc + 1 + form;
}
