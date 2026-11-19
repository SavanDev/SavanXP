/* SxBASIC core: the arena and the lexer (docs/userland/SXBASIC.md#lexical-rules). */
#include "sxb_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- arena ------------------------------------------------------------------- */

struct sxb_arena_block {
    struct sxb_arena_block *next;
    size_t used;
    size_t capacity;
    unsigned char data[];
};

void *sxb_arena_alloc(struct sxb_arena *arena, size_t size)
{
    struct sxb_arena_block *block = arena->blocks;
    size = (size + 15u) & ~(size_t)15u;
    if (block == NULL || block->capacity - block->used < size) {
        size_t capacity = size > 65536u ? size : 65536u;
        block = malloc(sizeof(*block) + capacity);
        if (block == NULL) {
            return NULL;
        }
        block->next = arena->blocks;
        block->used = 0;
        block->capacity = capacity;
        arena->blocks = block;
    }
    void *result = block->data + block->used;
    block->used += size;
    memset(result, 0, size);
    return result;
}

char *sxb_arena_strndup(struct sxb_arena *arena, const char *text, size_t length)
{
    char *copy = sxb_arena_alloc(arena, length + 1u);
    if (copy != NULL) {
        memcpy(copy, text, length);
        copy[length] = '\0';
    }
    return copy;
}

void sxb_arena_free(struct sxb_arena *arena)
{
    struct sxb_arena_block *block = arena->blocks;
    while (block != NULL) {
        struct sxb_arena_block *next = block->next;
        free(block);
        block = next;
    }
    arena->blocks = NULL;
}

/* ---- names ------------------------------------------------------------------- */

static int fold(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int sxb_names_equal(const char *a, int a_length, const char *b, int b_length)
{
    if (a_length != b_length) {
        return 0;
    }
    for (int i = 0; i < a_length; i++) {
        if (fold((unsigned char)a[i]) != fold((unsigned char)b[i])) {
            return 0;
        }
    }
    return 1;
}

int sxb_name_equal(const char *a, int a_length, const char *b)
{
    return sxb_names_equal(a, a_length, b, (int)strlen(b));
}

static const char *const keyword_names[SXB_KW_COUNT] = {
    "And", "As", "Boolean", "ByRef", "ByVal", "Case", "Catch", "Const", "Dim", "Do",
    "Double", "Each", "Else", "ElseIf", "End", "Exit", "False", "For", "Function", "If",
    "In", "Integer", "Is", "Loop", "Me", "Mod", "Next", "Not", "Or", "Preserve",
    "Print", "Private", "Public", "ReDim", "Rem", "Return", "Select", "Step", "String", "Sub",
    "Then", "To", "True", "Try", "Unload", "Until", "While", "Xor"
};

const char *sxb_keyword_name(int keyword)
{
    return (keyword >= 0 && keyword < SXB_KW_COUNT) ? keyword_names[keyword] : "?";
}

const char *sxb_type_name(int type)
{
    static const char *const names[] = {"nothing", "Integer", "Double", "String", "Boolean"};
    static const char *const arrays[] = {"nothing", "Integer()", "Double()", "String()", "Boolean()"};
    int base = SXB_TY_BASE(type);
    if (base > 4) {
        return "?";
    }
    return SXB_TY_IS_ARRAY(type) ? arrays[base] : names[base];
}

/* ---- lexer ------------------------------------------------------------------- */

struct lexer {
    struct sxb_arena *arena;
    const char *file;
    const char *text;
    size_t length;
    size_t pos;
    int line;
    size_t line_start;
    struct sxb_token *tokens;
    int count;
    int capacity;
    struct sxb_diagnostic *diag;
};

static int fail(struct lexer *lx, size_t at, const char *message)
{
    snprintf(lx->diag->file, sizeof(lx->diag->file), "%s", lx->file);
    lx->diag->line = lx->line;
    lx->diag->column = (int)(at - lx->line_start) + 1;
    snprintf(lx->diag->message, sizeof(lx->diag->message), "%s", message);
    return -1;
}

static struct sxb_token *push(struct lexer *lx, int kind, size_t start)
{
    if (lx->count == lx->capacity) {
        int capacity = lx->capacity ? lx->capacity * 2 : 256;
        struct sxb_token *grown = sxb_arena_alloc(lx->arena, sizeof(*grown) * (size_t)capacity);
        if (grown == NULL) {
            return NULL;
        }
        if (lx->count) {
            memcpy(grown, lx->tokens, sizeof(*grown) * (size_t)lx->count);
        }
        lx->tokens = grown;
        lx->capacity = capacity;
    }
    struct sxb_token *token = &lx->tokens[lx->count++];
    memset(token, 0, sizeof(*token));
    token->kind = kind;
    token->line = lx->line;
    token->column = (int)(start - lx->line_start) + 1;
    token->text = lx->text + start;
    return token;
}

static int is_letter(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int at_statement_start(const struct lexer *lx)
{
    return lx->count == 0 || lx->tokens[lx->count - 1].kind == SXB_TK_NEWLINE;
}

static void skip_to_line_end(struct lexer *lx)
{
    while (lx->pos < lx->length && lx->text[lx->pos] != '\n' && lx->text[lx->pos] != '\r') {
        lx->pos++;
    }
}

static int lex_number(struct lexer *lx)
{
    size_t start = lx->pos;
    const char *t = lx->text;
    if (t[lx->pos] == '&') {
        lx->pos += 2; /* &H */
        uint64_t value = 0;
        int digits = 0;
        int h;
        while (lx->pos < lx->length && (h = hex_value((unsigned char)t[lx->pos])) >= 0) {
            if (value > 0x7FFFFFFFFFFFFFFull) {
                return fail(lx, start, "number too large for an Integer");
            }
            value = value * 16u + (uint64_t)h;
            digits++;
            lx->pos++;
        }
        if (digits == 0) {
            return fail(lx, start, "expected hexadecimal digits after &H");
        }
        struct sxb_token *token = push(lx, SXB_TK_INT, start);
        if (token == NULL) return fail(lx, start, "out of memory");
        token->int_value = (int64_t)value;
        token->length = (int)(lx->pos - start);
        return 0;
    }
    int is_double = 0;
    while (lx->pos < lx->length && is_digit((unsigned char)t[lx->pos])) lx->pos++;
    if (lx->pos < lx->length && t[lx->pos] == '.' && lx->pos + 1 < lx->length &&
        is_digit((unsigned char)t[lx->pos + 1])) {
        is_double = 1;
        lx->pos++;
        while (lx->pos < lx->length && is_digit((unsigned char)t[lx->pos])) lx->pos++;
    }
    if (lx->pos < lx->length && (t[lx->pos] == 'e' || t[lx->pos] == 'E')) {
        size_t e = lx->pos + 1;
        if (e < lx->length && (t[e] == '+' || t[e] == '-')) e++;
        if (e < lx->length && is_digit((unsigned char)t[e])) {
            is_double = 1;
            lx->pos = e;
            while (lx->pos < lx->length && is_digit((unsigned char)t[lx->pos])) lx->pos++;
        }
    }
    if (lx->pos < lx->length && (is_letter((unsigned char)t[lx->pos]) || t[lx->pos] == '_')) {
        return fail(lx, lx->pos, "a number cannot run into a name");
    }
    char buffer[128];
    size_t n = lx->pos - start;
    if (n >= sizeof(buffer)) {
        return fail(lx, start, "number too long");
    }
    memcpy(buffer, t + start, n);
    buffer[n] = '\0';
    struct sxb_token *token = push(lx, is_double ? SXB_TK_DOUBLE : SXB_TK_INT, start);
    if (token == NULL) return fail(lx, start, "out of memory");
    token->length = (int)n;
    if (is_double) {
        token->double_value = strtod(buffer, NULL);
    } else {
        uint64_t value = 0;
        for (size_t i = 0; i < n; i++) {
            uint64_t digit = (uint64_t)(buffer[i] - '0');
            if (value > (0x7FFFFFFFFFFFFFFFull - digit) / 10u) {
                return fail(lx, start, "number too large for an Integer");
            }
            value = value * 10u + digit;
        }
        token->int_value = (int64_t)value;
    }
    return 0;
}

static int lex_string(struct lexer *lx)
{
    size_t start = lx->pos;
    const char *t = lx->text;
    lx->pos++;
    size_t capacity = 64, used = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) return fail(lx, start, "out of memory");
    for (;;) {
        if (lx->pos >= lx->length || t[lx->pos] == '\n' || t[lx->pos] == '\r') {
            free(buffer);
            return fail(lx, start, "the string has no closing quote");
        }
        char c = t[lx->pos++];
        if (c == '"') {
            if (lx->pos < lx->length && t[lx->pos] == '"') {
                lx->pos++;
            } else {
                break;
            }
        }
        if (used + 1 >= capacity) {
            capacity *= 2;
            char *grown = realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return fail(lx, start, "out of memory");
            }
            buffer = grown;
        }
        buffer[used++] = c;
    }
    struct sxb_token *token = push(lx, SXB_TK_STRING, start);
    if (token == NULL) {
        free(buffer);
        return fail(lx, start, "out of memory");
    }
    token->string_value = sxb_arena_strndup(lx->arena, buffer, used);
    token->string_length = used;
    token->length = (int)(lx->pos - start);
    free(buffer);
    return token->string_value != NULL ? 0 : fail(lx, start, "out of memory");
}

static int lex_operator(struct lexer *lx)
{
    size_t start = lx->pos;
    const char *t = lx->text;
    char c = t[lx->pos];
    char d = lx->pos + 1 < lx->length ? t[lx->pos + 1] : '\0';
    int op;
    int width = 1;
    switch (c) {
    case '+': op = SXB_OP_PLUS; break;
    case '-': op = SXB_OP_MINUS; break;
    case '*': op = SXB_OP_STAR; break;
    case '/': op = SXB_OP_SLASH; break;
    case '\\': op = SXB_OP_BACKSLASH; break;
    case '^': op = SXB_OP_CARET; break;
    case '&': op = SXB_OP_AMP; break;
    case '=': op = SXB_OP_EQ; break;
    case '(': op = SXB_OP_LPAREN; break;
    case ')': op = SXB_OP_RPAREN; break;
    case ',': op = SXB_OP_COMMA; break;
    case '.': op = SXB_OP_DOT; break;
    case ';': op = SXB_OP_SEMICOLON; break;
    case '<':
        if (d == '>') { op = SXB_OP_NE; width = 2; }
        else if (d == '=') { op = SXB_OP_LE; width = 2; }
        else op = SXB_OP_LT;
        break;
    case '>':
        if (d == '=') { op = SXB_OP_GE; width = 2; }
        else op = SXB_OP_GT;
        break;
    case ':':
        return fail(lx, start, "\":\" does not separate statements; put each on its own line");
    default:
        return fail(lx, start, "unexpected character");
    }
    struct sxb_token *token = push(lx, SXB_TK_OP, start);
    if (token == NULL) return fail(lx, start, "out of memory");
    token->sub = op;
    token->length = width;
    lx->pos += (size_t)width;
    return 0;
}

int sxb_lex(struct sxb_arena *arena, const char *file, const char *text, size_t length, int first_line,
            struct sxb_token **tokens, int *count, struct sxb_diagnostic *diag)
{
    struct lexer lx;
    memset(&lx, 0, sizeof(lx));
    lx.arena = arena;
    lx.file = file;
    lx.text = text;
    lx.length = length;
    lx.line = first_line;
    lx.diag = diag;
    /* A UTF-8 byte order mark at the start is skipped. */
    if (length >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        lx.pos = 3;
        lx.line_start = 3;
    }
    while (lx.pos < lx.length) {
        char c = text[lx.pos];
        if (c == ' ' || c == '\t') {
            lx.pos++;
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (!at_statement_start(&lx)) {
                if (push(&lx, SXB_TK_NEWLINE, lx.pos) == NULL) return fail(&lx, lx.pos, "out of memory");
            }
            if (c == '\r' && lx.pos + 1 < lx.length && text[lx.pos + 1] == '\n') lx.pos++;
            lx.pos++;
            lx.line++;
            lx.line_start = lx.pos;
            continue;
        }
        if (c == '\'') {
            skip_to_line_end(&lx);
            continue;
        }
        if (c == '_') {
            /* " _" at the end of a line continues the statement on the next. */
            size_t after = lx.pos + 1;
            while (after < lx.length && (text[after] == ' ' || text[after] == '\t')) after++;
            int preceded = lx.pos > lx.line_start && (text[lx.pos - 1] == ' ' || text[lx.pos - 1] == '\t');
            if (preceded && (after >= lx.length || text[after] == '\n' || text[after] == '\r')) {
                lx.pos = after;
                if (lx.pos < lx.length) {
                    if (text[lx.pos] == '\r' && lx.pos + 1 < lx.length && text[lx.pos + 1] == '\n') lx.pos++;
                    lx.pos++;
                }
                lx.line++;
                lx.line_start = lx.pos;
                continue;
            }
            return fail(&lx, lx.pos, "\"_\" continues a line only at its end, after a space");
        }
        if (is_digit((unsigned char)c) ||
            (c == '.' && lx.pos + 1 < lx.length && is_digit((unsigned char)text[lx.pos + 1])) ||
            (c == '&' && lx.pos + 1 < lx.length && (text[lx.pos + 1] == 'H' || text[lx.pos + 1] == 'h'))) {
            if (lex_number(&lx) != 0) return -1;
            continue;
        }
        if (c == '"') {
            if (lex_string(&lx) != 0) return -1;
            continue;
        }
        if (is_letter((unsigned char)c)) {
            size_t start = lx.pos;
            while (lx.pos < lx.length &&
                   (is_letter((unsigned char)text[lx.pos]) || is_digit((unsigned char)text[lx.pos]) ||
                    text[lx.pos] == '_')) {
                lx.pos++;
            }
            int n = (int)(lx.pos - start);
            if (n > 63) {
                return fail(&lx, start, "a name is at most 63 characters");
            }
            int keyword = -1;
            for (int k = 0; k < SXB_KW_COUNT; k++) {
                if (sxb_name_equal(text + start, n, keyword_names[k])) {
                    keyword = k;
                    break;
                }
            }
            if (keyword == SXB_KW_REM) {
                if (!at_statement_start(&lx)) {
                    return fail(&lx, start, "Rem starts a comment only at the start of a statement");
                }
                skip_to_line_end(&lx);
                continue;
            }
            struct sxb_token *token = push(&lx, keyword >= 0 ? SXB_TK_KEYWORD : SXB_TK_IDENT, start);
            if (token == NULL) return fail(&lx, start, "out of memory");
            token->sub = keyword;
            token->length = n;
            continue;
        }
        if ((unsigned char)c >= 0x80) {
            return fail(&lx, lx.pos, "names are ASCII; other characters go inside strings and comments");
        }
        if (lex_operator(&lx) != 0) return -1;
    }
    if (!at_statement_start(&lx)) {
        if (push(&lx, SXB_TK_NEWLINE, lx.pos) == NULL) return fail(&lx, lx.pos, "out of memory");
    }
    if (push(&lx, SXB_TK_EOF, lx.pos) == NULL) return fail(&lx, lx.pos, "out of memory");
    *tokens = lx.tokens;
    *count = lx.count;
    return 0;
}
