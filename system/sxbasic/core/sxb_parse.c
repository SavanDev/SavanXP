/* SxBASIC core: the parser, from tokens to a syntax tree
 * (docs/userland/SXBASIC.md). It stops at the first error; every function
 * still returns a node after one, so callers need no NULL checks, and the
 * tree is thrown away. */
#include "sxb_internal.h"

#include <stdio.h>
#include <string.h>

struct parser {
    struct sxb_arena *arena;
    const char *file;
    struct sxb_token *tokens;
    int count;
    int pos;
    int failed;
    int module;
    struct sxb_diagnostic *diag;
};

static struct sxb_token eof_token = {SXB_TK_EOF, 0, 0, 0, "", 0, 0, 0.0, NULL, 0};

static struct sxb_token *peek(struct parser *p)
{
    return p->failed ? &eof_token : &p->tokens[p->pos];
}

static struct sxb_token *peek_at(struct parser *p, int ahead)
{
    if (p->failed || p->pos + ahead >= p->count) {
        return &eof_token;
    }
    return &p->tokens[p->pos + ahead];
}

static struct sxb_token *next(struct parser *p)
{
    struct sxb_token *token = peek(p);
    if (!p->failed && token->kind != SXB_TK_EOF) {
        p->pos++;
    }
    return token;
}

static void describe(const struct sxb_token *token, char *out, size_t size)
{
    switch (token->kind) {
    case SXB_TK_EOF: snprintf(out, size, "the end of the file"); break;
    case SXB_TK_NEWLINE: snprintf(out, size, "the end of the line"); break;
    case SXB_TK_KEYWORD: snprintf(out, size, "\"%s\"", sxb_keyword_name(token->sub)); break;
    case SXB_TK_STRING: snprintf(out, size, "a string"); break;
    case SXB_TK_INT:
    case SXB_TK_DOUBLE: snprintf(out, size, "a number"); break;
    default: snprintf(out, size, "\"%.*s\"", token->length > 40 ? 40 : token->length, token->text); break;
    }
}

static void error_at(struct parser *p, const struct sxb_token *token, const char *message)
{
    if (p->failed) {
        return;
    }
    p->failed = 1;
    snprintf(p->diag->file, sizeof(p->diag->file), "%s", p->file);
    p->diag->line = token->line;
    p->diag->column = token->column;
    snprintf(p->diag->message, sizeof(p->diag->message), "%s", message);
}

static void expected(struct parser *p, const char *what)
{
    char found[64];
    char message[256];
    struct sxb_token *token = peek(p);
    describe(token, found, sizeof(found));
    snprintf(message, sizeof(message), "expected %s, found %s", what, found);
    error_at(p, token, message);
}

static int is_kw(struct parser *p, int keyword)
{
    struct sxb_token *token = peek(p);
    return token->kind == SXB_TK_KEYWORD && token->sub == keyword;
}

static int is_op(struct parser *p, int op)
{
    struct sxb_token *token = peek(p);
    return token->kind == SXB_TK_OP && token->sub == op;
}

static int is_newline(struct parser *p)
{
    int kind = peek(p)->kind;
    return kind == SXB_TK_NEWLINE || kind == SXB_TK_EOF;
}

static int accept_kw(struct parser *p, int keyword)
{
    if (is_kw(p, keyword)) {
        next(p);
        return 1;
    }
    return 0;
}

static int accept_op(struct parser *p, int op)
{
    if (is_op(p, op)) {
        next(p);
        return 1;
    }
    return 0;
}

static void expect_kw(struct parser *p, int keyword)
{
    if (!accept_kw(p, keyword)) {
        char what[64];
        snprintf(what, sizeof(what), "\"%s\"", sxb_keyword_name(keyword));
        expected(p, what);
    }
}

static void expect_op(struct parser *p, int op, const char *spelling)
{
    if (!accept_op(p, op)) {
        char what[32];
        snprintf(what, sizeof(what), "\"%s\"", spelling);
        expected(p, what);
    }
}

static void expect_newline(struct parser *p)
{
    if (peek(p)->kind == SXB_TK_NEWLINE) {
        next(p);
    } else if (peek(p)->kind != SXB_TK_EOF) {
        expected(p, "the end of the line");
    }
}

static void expect_end_of(struct parser *p, int keyword)
{
    if (is_kw(p, SXB_KW_END) && peek_at(p, 1)->kind == SXB_TK_KEYWORD && peek_at(p, 1)->sub == keyword) {
        next(p);
        next(p);
        expect_newline(p);
        return;
    }
    char what[64];
    snprintf(what, sizeof(what), "\"End %s\"", sxb_keyword_name(keyword));
    expected(p, what);
}

static struct sxb_node *new_node(struct parser *p, int kind, const struct sxb_token *at)
{
    static struct sxb_node fallback;
    struct sxb_node *node = sxb_arena_alloc(p->arena, sizeof(*node));
    if (node == NULL) {
        error_at(p, at, "out of memory");
        memset(&fallback, 0, sizeof(fallback));
        fallback.kind = kind;
        return &fallback;
    }
    node->kind = kind;
    node->line = at->line;
    node->column = at->column;
    return node;
}

static struct sxb_token *expect_ident(struct parser *p, const char *what)
{
    struct sxb_token *token = peek(p);
    if (token->kind != SXB_TK_IDENT) {
        if (token->kind == SXB_TK_KEYWORD) {
            char message[160];
            snprintf(message, sizeof(message), "\"%s\" is a reserved word and cannot name %s",
                     sxb_keyword_name(token->sub), what);
            error_at(p, token, message);
        } else {
            expected(p, what);
        }
        return &eof_token;
    }
    return next(p);
}

/* ---- types ------------------------------------------------------------------ */

static int parse_as_type(struct parser *p, int allow_array)
{
    expect_kw(p, SXB_KW_AS);
    int base = SXB_TY_VOID;
    if (accept_kw(p, SXB_KW_INTEGER)) base = SXB_TY_INT;
    else if (accept_kw(p, SXB_KW_DOUBLE)) base = SXB_TY_DOUBLE;
    else if (accept_kw(p, SXB_KW_STRING)) base = SXB_TY_STRING;
    else if (accept_kw(p, SXB_KW_BOOLEAN)) base = SXB_TY_BOOL;
    else {
        expected(p, "a type (Integer, Double, String or Boolean)");
        return SXB_TY_INT;
    }
    if (allow_array && is_op(p, SXB_OP_LPAREN)) {
        next(p);
        expect_op(p, SXB_OP_RPAREN, ")");
        base |= SXB_TY_ARRAY;
    }
    return base;
}

/* ---- expressions ---------------------------------------------------------- */

static struct sxb_node *parse_expr(struct parser *p);

static struct sxb_node *parse_args(struct parser *p)
{
    struct sxb_node *head = NULL, *tail = NULL;
    expect_op(p, SXB_OP_LPAREN, "(");
    if (!is_op(p, SXB_OP_RPAREN)) {
        do {
            struct sxb_node *arg = parse_expr(p);
            if (tail) tail->next = arg; else head = arg;
            tail = arg;
        } while (!p->failed && accept_op(p, SXB_OP_COMMA));
    }
    expect_op(p, SXB_OP_RPAREN, ")");
    return head;
}

static struct sxb_node *parse_postfix(struct parser *p)
{
    struct sxb_token *token = peek(p);
    struct sxb_node *node;
    if (token->kind == SXB_TK_IDENT) {
        next(p);
        node = new_node(p, SXB_N_NAME, token);
        node->name = token->text;
        node->name_length = token->length;
    } else if (is_kw(p, SXB_KW_ME)) {
        next(p);
        node = new_node(p, SXB_N_ME, token);
    } else {
        expected(p, "a name");
        return new_node(p, SXB_N_INT, token);
    }
    if (is_op(p, SXB_OP_LPAREN)) {
        node->args = parse_args(p);
        node->flags |= SXB_F_HAS_PARENS;
    }
    while (!p->failed && is_op(p, SXB_OP_DOT)) {
        struct sxb_token *dot = next(p);
        struct sxb_token *member = expect_ident(p, "a member");
        struct sxb_node *access = new_node(p, SXB_N_MEMBER, dot);
        access->a = node;
        access->member = member->text;
        access->member_length = member->length;
        access->line = member->line;
        access->column = member->column;
        if (is_op(p, SXB_OP_LPAREN)) {
            access->args = parse_args(p);
            access->flags |= SXB_F_HAS_PARENS;
        }
        node = access;
    }
    return node;
}

static struct sxb_node *parse_primary(struct parser *p)
{
    struct sxb_token *token = peek(p);
    struct sxb_node *node;
    switch (token->kind) {
    case SXB_TK_INT:
        next(p);
        node = new_node(p, SXB_N_INT, token);
        node->int_value = token->int_value;
        return node;
    case SXB_TK_DOUBLE:
        next(p);
        node = new_node(p, SXB_N_DOUBLE, token);
        node->double_value = token->double_value;
        return node;
    case SXB_TK_STRING:
        next(p);
        node = new_node(p, SXB_N_STRING, token);
        node->string_value = token->string_value;
        node->string_length = token->string_length;
        return node;
    case SXB_TK_KEYWORD:
        if (token->sub == SXB_KW_TRUE || token->sub == SXB_KW_FALSE) {
            next(p);
            node = new_node(p, SXB_N_BOOL, token);
            node->int_value = token->sub == SXB_KW_TRUE;
            return node;
        }
        if (token->sub == SXB_KW_ME) {
            return parse_postfix(p);
        }
        break;
    case SXB_TK_OP:
        if (token->sub == SXB_OP_LPAREN) {
            next(p);
            node = parse_expr(p);
            expect_op(p, SXB_OP_RPAREN, ")");
            return node;
        }
        break;
    case SXB_TK_IDENT:
        return parse_postfix(p);
    default:
        break;
    }
    expected(p, "a value");
    return new_node(p, SXB_N_INT, token);
}

static struct sxb_node *binary(struct parser *p, int op, const struct sxb_token *at, struct sxb_node *a,
                               struct sxb_node *b)
{
    struct sxb_node *node = new_node(p, SXB_N_BINARY, at);
    node->op = op;
    node->a = a;
    node->b = b;
    return node;
}

static struct sxb_node *parse_signed(struct parser *p)
{
    if (is_op(p, SXB_OP_MINUS)) {
        struct sxb_token *token = next(p);
        struct sxb_node *node = new_node(p, SXB_N_UNARY, token);
        node->op = SXB_OP_NEG;
        node->a = parse_signed(p);
        return node;
    }
    return parse_primary(p);
}

static struct sxb_node *parse_power(struct parser *p)
{
    struct sxb_node *left = parse_primary(p);
    while (!p->failed && is_op(p, SXB_OP_CARET)) {
        struct sxb_token *token = next(p);
        left = binary(p, SXB_OP_CARET, token, left, parse_signed(p));
    }
    return left;
}

static struct sxb_node *parse_unary(struct parser *p)
{
    if (is_op(p, SXB_OP_MINUS)) {
        struct sxb_token *token = next(p);
        struct sxb_node *node = new_node(p, SXB_N_UNARY, token);
        node->op = SXB_OP_NEG;
        node->a = parse_unary(p);
        return node;
    }
    return parse_power(p);
}

static struct sxb_node *parse_multiplicative(struct parser *p)
{
    struct sxb_node *left = parse_unary(p);
    while (!p->failed && (is_op(p, SXB_OP_STAR) || is_op(p, SXB_OP_SLASH))) {
        struct sxb_token *token = next(p);
        left = binary(p, token->sub, token, left, parse_unary(p));
    }
    return left;
}

static struct sxb_node *parse_intdiv(struct parser *p)
{
    struct sxb_node *left = parse_multiplicative(p);
    while (!p->failed && (is_op(p, SXB_OP_BACKSLASH) || is_kw(p, SXB_KW_MOD))) {
        struct sxb_token *token = next(p);
        int op = token->kind == SXB_TK_KEYWORD ? SXB_OP_MOD : SXB_OP_BACKSLASH;
        left = binary(p, op, token, left, parse_multiplicative(p));
    }
    return left;
}

static struct sxb_node *parse_additive(struct parser *p)
{
    struct sxb_node *left = parse_intdiv(p);
    while (!p->failed && (is_op(p, SXB_OP_PLUS) || is_op(p, SXB_OP_MINUS))) {
        struct sxb_token *token = next(p);
        left = binary(p, token->sub, token, left, parse_intdiv(p));
    }
    return left;
}

static struct sxb_node *parse_concat(struct parser *p)
{
    struct sxb_node *left = parse_additive(p);
    while (!p->failed && is_op(p, SXB_OP_AMP)) {
        struct sxb_token *token = next(p);
        left = binary(p, SXB_OP_AMP, token, left, parse_additive(p));
    }
    return left;
}

static int is_comparison(struct parser *p)
{
    struct sxb_token *token = peek(p);
    return token->kind == SXB_TK_OP && token->sub >= SXB_OP_EQ && token->sub <= SXB_OP_GE;
}

static struct sxb_node *parse_comparison(struct parser *p)
{
    struct sxb_node *left = parse_concat(p);
    while (!p->failed && is_comparison(p)) {
        struct sxb_token *token = next(p);
        left = binary(p, token->sub, token, left, parse_concat(p));
    }
    return left;
}

static struct sxb_node *parse_not(struct parser *p)
{
    if (is_kw(p, SXB_KW_NOT)) {
        struct sxb_token *token = next(p);
        struct sxb_node *node = new_node(p, SXB_N_UNARY, token);
        node->op = SXB_OP_NOT;
        node->a = parse_not(p);
        return node;
    }
    return parse_comparison(p);
}

static struct sxb_node *parse_and(struct parser *p)
{
    struct sxb_node *left = parse_not(p);
    while (!p->failed && is_kw(p, SXB_KW_AND)) {
        struct sxb_token *token = next(p);
        left = binary(p, SXB_OP_AND, token, left, parse_not(p));
    }
    return left;
}

static struct sxb_node *parse_expr(struct parser *p)
{
    struct sxb_node *left = parse_and(p);
    while (!p->failed && (is_kw(p, SXB_KW_OR) || is_kw(p, SXB_KW_XOR))) {
        struct sxb_token *token = next(p);
        left = binary(p, token->sub == SXB_KW_OR ? SXB_OP_OR : SXB_OP_XOR, token, left, parse_and(p));
    }
    return left;
}

/* ---- declarations ---------------------------------------------------------- */

static struct sxb_node *parse_var_decl(struct parser *p)
{
    struct sxb_token *name = expect_ident(p, "a variable");
    struct sxb_node *decl = new_node(p, SXB_N_VARDECL, name);
    decl->name = name->text;
    decl->name_length = name->length;
    if (accept_op(p, SXB_OP_LPAREN)) {
        decl->flags |= SXB_F_IS_ARRAY;
        if (!is_op(p, SXB_OP_RPAREN)) {
            decl->a = parse_expr(p);
            decl->flags |= SXB_F_FIXED_SIZE;
        }
        expect_op(p, SXB_OP_RPAREN, ")");
    }
    if (!is_kw(p, SXB_KW_AS)) {
        char message[200];
        snprintf(message, sizeof(message), "\"%.*s\" needs a type: \"%.*s As Integer\", for example",
                 name->length, name->text, name->length, name->text);
        error_at(p, peek(p), message);
        return decl;
    }
    decl->type = parse_as_type(p, 0);
    if (decl->flags & SXB_F_IS_ARRAY) {
        decl->type |= SXB_TY_ARRAY;
    }
    if (is_op(p, SXB_OP_EQ)) {
        struct sxb_token *eq = next(p);
        if (decl->flags & SXB_F_IS_ARRAY) {
            error_at(p, eq, "an array cannot have an initializer");
        }
        decl->b = parse_expr(p);
    }
    return decl;
}

static struct sxb_node *parse_dim(struct parser *p, const struct sxb_token *at)
{
    struct sxb_node *dim = new_node(p, SXB_N_DIM, at);
    struct sxb_node *tail = NULL;
    do {
        struct sxb_node *decl = parse_var_decl(p);
        if (tail) tail->next = decl; else dim->a = decl;
        tail = decl;
    } while (!p->failed && accept_op(p, SXB_OP_COMMA));
    expect_newline(p);
    return dim;
}

static struct sxb_node *parse_const(struct parser *p, const struct sxb_token *at)
{
    struct sxb_token *name = expect_ident(p, "a constant");
    struct sxb_node *node = new_node(p, SXB_N_CONST, at);
    node->name = name->text;
    node->name_length = name->length;
    node->line = name->line;
    node->column = name->column;
    if (is_kw(p, SXB_KW_AS)) {
        node->type = parse_as_type(p, 0);
    }
    expect_op(p, SXB_OP_EQ, "=");
    node->a = parse_expr(p);
    expect_newline(p);
    return node;
}

/* ---- statements -------------------------------------------------------------- */

static struct sxb_node *parse_block(struct parser *p);
static struct sxb_node *parse_statement(struct parser *p, int single_line);

static int block_ends(struct parser *p)
{
    struct sxb_token *token = peek(p);
    if (token->kind == SXB_TK_EOF) {
        return 1;
    }
    if (token->kind != SXB_TK_KEYWORD) {
        return 0;
    }
    switch (token->sub) {
    case SXB_KW_ELSE:
    case SXB_KW_ELSEIF:
    case SXB_KW_CASE:
    case SXB_KW_LOOP:
    case SXB_KW_NEXT:
    case SXB_KW_CATCH:
        return 1;
    case SXB_KW_END: {
        struct sxb_token *after = peek_at(p, 1);
        return after->kind == SXB_TK_KEYWORD &&
               (after->sub == SXB_KW_IF || after->sub == SXB_KW_SUB || after->sub == SXB_KW_FUNCTION ||
                after->sub == SXB_KW_SELECT || after->sub == SXB_KW_TRY);
    }
    default:
        return 0;
    }
}

static struct sxb_node *parse_block(struct parser *p)
{
    struct sxb_node *head = NULL, *tail = NULL;
    while (!p->failed) {
        while (peek(p)->kind == SXB_TK_NEWLINE) next(p);
        if (block_ends(p)) break;
        struct sxb_node *statement = parse_statement(p, 0);
        if (statement == NULL) continue;
        if (tail) tail->next = statement; else head = statement;
        tail = statement;
    }
    return head;
}

static struct sxb_node *parse_if(struct parser *p, const struct sxb_token *at)
{
    struct sxb_node *node = new_node(p, SXB_N_IF, at);
    node->a = parse_expr(p);
    expect_kw(p, SXB_KW_THEN);
    if (!is_newline(p)) {
        node->b = parse_statement(p, 1);
        return node;
    }
    expect_newline(p);
    node->b = parse_block(p);
    if (is_kw(p, SXB_KW_ELSEIF)) {
        struct sxb_token *token = next(p);
        node->c = parse_if(p, token);
        node->flags |= SXB_F_HAS_ELSE;
        return node; /* the nested If consumed End If */
    }
    if (accept_kw(p, SXB_KW_ELSE)) {
        expect_newline(p);
        node->c = parse_block(p);
        node->flags |= SXB_F_HAS_ELSE;
    }
    expect_end_of(p, SXB_KW_IF);
    return node;
}

static struct sxb_node *parse_case_test(struct parser *p)
{
    struct sxb_token *at = peek(p);
    if (accept_kw(p, SXB_KW_IS)) {
        if (!is_comparison(p)) {
            expected(p, "a comparison after \"Is\"");
        }
        struct sxb_token *op = next(p);
        struct sxb_node *node = new_node(p, SXB_N_CASE_IS, at);
        node->op = op->sub;
        node->a = parse_concat(p);
        return node;
    }
    struct sxb_node *value = parse_concat(p);
    if (accept_kw(p, SXB_KW_TO)) {
        struct sxb_node *node = new_node(p, SXB_N_CASE_RANGE, at);
        node->a = value;
        node->b = parse_concat(p);
        return node;
    }
    struct sxb_node *node = new_node(p, SXB_N_CASE_VALUE, at);
    node->a = value;
    return node;
}

static struct sxb_node *parse_select(struct parser *p, const struct sxb_token *at)
{
    struct sxb_node *node = new_node(p, SXB_N_SELECT, at);
    expect_kw(p, SXB_KW_CASE);
    node->a = parse_expr(p);
    expect_newline(p);
    while (peek(p)->kind == SXB_TK_NEWLINE) next(p);
    struct sxb_node *tail = NULL;
    while (!p->failed && is_kw(p, SXB_KW_CASE)) {
        struct sxb_token *case_token = next(p);
        if (node->flags & SXB_F_HAS_ELSE) {
            error_at(p, case_token, "\"Case Else\" has to be the last Case");
            break;
        }
        if (accept_kw(p, SXB_KW_ELSE)) {
            expect_newline(p);
            node->c = parse_block(p);
            node->flags |= SXB_F_HAS_ELSE;
            continue;
        }
        struct sxb_node *clause = new_node(p, SXB_N_CASE, case_token);
        struct sxb_node *test_tail = NULL;
        do {
            struct sxb_node *test = parse_case_test(p);
            if (test_tail) test_tail->next = test; else clause->a = test;
            test_tail = test;
        } while (!p->failed && accept_op(p, SXB_OP_COMMA));
        expect_newline(p);
        clause->b = parse_block(p);
        if (tail) tail->next = clause; else node->b = clause;
        tail = clause;
    }
    expect_end_of(p, SXB_KW_SELECT);
    return node;
}

static void expect_next(struct parser *p, const struct sxb_node *loop)
{
    expect_kw(p, SXB_KW_NEXT);
    if (peek(p)->kind == SXB_TK_IDENT) {
        struct sxb_token *name = next(p);
        if (!sxb_names_equal(name->text, name->length, loop->name, loop->name_length)) {
            char message[200];
            snprintf(message, sizeof(message), "this Next names \"%.*s\", but its For counts \"%.*s\"",
                     name->length, name->text, loop->name_length, loop->name);
            error_at(p, name, message);
        }
    }
    expect_newline(p);
}

static struct sxb_node *parse_for(struct parser *p, const struct sxb_token *at)
{
    if (accept_kw(p, SXB_KW_EACH)) {
        struct sxb_node *node = new_node(p, SXB_N_FOREACH, at);
        struct sxb_token *name = expect_ident(p, "a variable");
        node->name = name->text;
        node->name_length = name->length;
        expect_kw(p, SXB_KW_IN);
        node->a = parse_expr(p);
        expect_newline(p);
        node->d = parse_block(p);
        expect_next(p, node);
        return node;
    }
    struct sxb_node *node = new_node(p, SXB_N_FOR, at);
    struct sxb_token *name = expect_ident(p, "a variable");
    node->name = name->text;
    node->name_length = name->length;
    expect_op(p, SXB_OP_EQ, "=");
    node->a = parse_expr(p);
    expect_kw(p, SXB_KW_TO);
    node->b = parse_expr(p);
    if (accept_kw(p, SXB_KW_STEP)) {
        node->c = parse_expr(p);
    }
    expect_newline(p);
    node->d = parse_block(p);
    expect_next(p, node);
    return node;
}

static struct sxb_node *parse_do(struct parser *p, const struct sxb_token *at)
{
    struct sxb_node *node = new_node(p, SXB_N_DO, at);
    if (accept_kw(p, SXB_KW_WHILE)) {
        node->op = SXB_DO_PRE_WHILE;
        node->a = parse_expr(p);
    } else if (accept_kw(p, SXB_KW_UNTIL)) {
        node->op = SXB_DO_PRE_UNTIL;
        node->a = parse_expr(p);
    }
    expect_newline(p);
    node->d = parse_block(p);
    struct sxb_token *loop = peek(p);
    expect_kw(p, SXB_KW_LOOP);
    if (is_kw(p, SXB_KW_WHILE) || is_kw(p, SXB_KW_UNTIL)) {
        if (node->op != SXB_DO_FOREVER) {
            error_at(p, loop, "a Do loop tests its condition at the top or at the bottom, not both");
        }
        node->op = next(p)->sub == SXB_KW_WHILE ? SXB_DO_POST_WHILE : SXB_DO_POST_UNTIL;
        node->a = parse_expr(p);
    }
    expect_newline(p);
    return node;
}

static struct sxb_node *parse_try(struct parser *p, const struct sxb_token *at)
{
    struct sxb_node *node = new_node(p, SXB_N_TRY, at);
    expect_newline(p);
    node->a = parse_block(p);
    expect_kw(p, SXB_KW_CATCH);
    expect_newline(p);
    node->b = parse_block(p);
    expect_end_of(p, SXB_KW_TRY);
    return node;
}

static struct sxb_node *parse_print(struct parser *p, const struct sxb_token *at)
{
    struct sxb_node *node = new_node(p, SXB_N_PRINT, at);
    struct sxb_node *tail = NULL;
    while (!p->failed && !is_newline(p)) {
        struct sxb_node *item = parse_expr(p);
        if (tail) tail->next = item; else node->a = item;
        tail = item;
        if (accept_op(p, SXB_OP_SEMICOLON)) {
            if (is_newline(p)) {
                node->flags |= SXB_F_TRAILING_SEMICOLON;
            }
            continue;
        }
        break;
    }
    return node;
}

/* An assignment or a call, both starting with a name. */
static struct sxb_node *parse_name_statement(struct parser *p)
{
    struct sxb_token *at = peek(p);
    struct sxb_node *target = parse_postfix(p);
    if (is_op(p, SXB_OP_EQ)) {
        next(p);
        struct sxb_node *node = new_node(p, SXB_N_ASSIGN, at);
        node->a = target;
        node->b = parse_expr(p);
        return node;
    }
    struct sxb_node *node = new_node(p, SXB_N_CALL, at);
    node->a = target;
    if (!is_newline(p)) {
        if (target->flags & SXB_F_HAS_PARENS) {
            expected(p, "the end of the line or \"=\"");
            return node;
        }
        struct sxb_node *tail = NULL;
        do {
            struct sxb_node *arg = parse_expr(p);
            if (tail) tail->next = arg; else target->args = arg;
            tail = arg;
        } while (!p->failed && accept_op(p, SXB_OP_COMMA));
    }
    return node;
}

static struct sxb_node *parse_statement(struct parser *p, int single_line)
{
    struct sxb_token *token = peek(p);
    struct sxb_node *node = NULL;
    if (token->kind == SXB_TK_IDENT) {
        node = parse_name_statement(p);
    } else if (token->kind == SXB_TK_KEYWORD) {
        int block = 0;
        switch (token->sub) {
        case SXB_KW_ME:
            node = parse_name_statement(p);
            break;
        case SXB_KW_DIM:
            next(p);
            if (single_line) break;
            return parse_dim(p, token);
        case SXB_KW_CONST:
            next(p);
            if (single_line) break;
            return parse_const(p, token);
        case SXB_KW_IF:
            next(p);
            block = 1;
            if (single_line) break;
            return parse_if(p, token);
        case SXB_KW_SELECT:
            next(p);
            block = 1;
            if (single_line) break;
            return parse_select(p, token);
        case SXB_KW_FOR:
            next(p);
            block = 1;
            if (single_line) break;
            return parse_for(p, token);
        case SXB_KW_DO:
            next(p);
            block = 1;
            if (single_line) break;
            return parse_do(p, token);
        case SXB_KW_TRY:
            next(p);
            block = 1;
            if (single_line) break;
            return parse_try(p, token);
        case SXB_KW_EXIT: {
            next(p);
            node = new_node(p, SXB_N_EXIT, token);
            struct sxb_token *what = peek(p);
            if (what->kind == SXB_TK_KEYWORD &&
                (what->sub == SXB_KW_FOR || what->sub == SXB_KW_DO || what->sub == SXB_KW_SUB ||
                 what->sub == SXB_KW_FUNCTION)) {
                node->op = next(p)->sub;
            } else {
                expected(p, "For, Do, Sub or Function after \"Exit\"");
            }
            break;
        }
        case SXB_KW_RETURN:
            next(p);
            node = new_node(p, SXB_N_RETURN, token);
            if (!is_newline(p)) node->a = parse_expr(p);
            break;
        case SXB_KW_REDIM: {
            next(p);
            node = new_node(p, SXB_N_REDIM, token);
            if (accept_kw(p, SXB_KW_PRESERVE)) node->flags |= SXB_F_PRESERVE;
            struct sxb_token *name = expect_ident(p, "an array");
            node->name = name->text;
            node->name_length = name->length;
            expect_op(p, SXB_OP_LPAREN, "(");
            node->a = parse_expr(p);
            expect_op(p, SXB_OP_RPAREN, ")");
            break;
        }
        case SXB_KW_PRINT:
            next(p);
            node = parse_print(p, token);
            break;
        case SXB_KW_UNLOAD:
            next(p);
            node = new_node(p, SXB_N_UNLOAD, token);
            node->a = parse_expr(p);
            break;
        case SXB_KW_END:
            next(p);
            node = new_node(p, SXB_N_END, token);
            break;
        default:
            break;
        }
        if (block && single_line) {
            error_at(p, token, "a one-line If takes one simple statement; use a block If for this");
            return NULL;
        }
    }
    if (node == NULL) {
        if (!p->failed) {
            if (token->kind == SXB_TK_KEYWORD && (token->sub == SXB_KW_DIM || token->sub == SXB_KW_CONST)) {
                error_at(p, token, "a one-line If takes one simple statement");
            } else {
                expected(p, "a statement");
            }
        }
        return NULL;
    }
    expect_newline(p);
    return node;
}

/* ---- procedures and modules ------------------------------------------------ */

static struct sxb_node *parse_params(struct parser *p)
{
    struct sxb_node *head = NULL, *tail = NULL;
    expect_op(p, SXB_OP_LPAREN, "(");
    if (!is_op(p, SXB_OP_RPAREN)) {
        do {
            struct sxb_token *at = peek(p);
            int byref = 0;
            if (accept_kw(p, SXB_KW_BYREF)) byref = 1;
            else accept_kw(p, SXB_KW_BYVAL);
            struct sxb_token *name = expect_ident(p, "a parameter");
            struct sxb_node *param = new_node(p, SXB_N_PARAM, at);
            param->name = name->text;
            param->name_length = name->length;
            int is_array = 0;
            if (accept_op(p, SXB_OP_LPAREN)) {
                expect_op(p, SXB_OP_RPAREN, ")");
                is_array = 1;
            }
            if (!is_kw(p, SXB_KW_AS)) {
                char message[200];
                snprintf(message, sizeof(message), "the parameter \"%.*s\" needs a type", name->length, name->text);
                error_at(p, peek(p), message);
            }
            param->type = parse_as_type(p, 0) | (is_array ? SXB_TY_ARRAY : 0);
            if (byref) param->flags |= SXB_F_BYREF;
            if (is_array && (at->kind == SXB_TK_KEYWORD && at->sub == SXB_KW_BYVAL)) {
                error_at(p, at, "an array is always passed by reference; drop ByVal");
            }
            if (tail) tail->next = param; else head = param;
            tail = param;
        } while (!p->failed && accept_op(p, SXB_OP_COMMA));
    }
    expect_op(p, SXB_OP_RPAREN, ")");
    return head;
}

static struct sxb_proc_decl *parse_proc(struct parser *p, int is_public)
{
    struct sxb_token *kind = next(p);
    struct sxb_proc_decl *proc = sxb_arena_alloc(p->arena, sizeof(*proc));
    if (proc == NULL) {
        error_at(p, kind, "out of memory");
        return NULL;
    }
    proc->is_function = kind->sub == SXB_KW_FUNCTION;
    proc->is_public = is_public;
    proc->module = p->module;
    struct sxb_token *name = expect_ident(p, proc->is_function ? "a function" : "a sub");
    proc->name = name->text;
    proc->name_length = name->length;
    proc->line = name->line;
    proc->column = name->column;
    proc->params = parse_params(p);
    if (proc->is_function) {
        proc->return_type = parse_as_type(p, 1);
    } else if (is_kw(p, SXB_KW_AS)) {
        error_at(p, peek(p), "a Sub returns nothing; make it a Function to return a value");
    }
    expect_newline(p);
    proc->body = parse_block(p);
    expect_end_of(p, kind->sub);
    return proc;
}

int sxb_parse_module(struct sxb_arena *arena, const char *file, struct sxb_token *tokens, int count,
                     int module_index, struct sxb_module_ast *out, struct sxb_diagnostic *diag)
{
    struct parser p;
    memset(&p, 0, sizeof(p));
    p.arena = arena;
    p.file = file;
    p.tokens = tokens;
    p.count = count;
    p.diag = diag;
    p.module = module_index;
    struct sxb_proc_decl *proc_tail = NULL;
    struct sxb_global_decl *global_tail = NULL;
    while (!p.failed && peek(&p)->kind != SXB_TK_EOF) {
        if (peek(&p)->kind == SXB_TK_NEWLINE) {
            next(&p);
            continue;
        }
        struct sxb_token *at = peek(&p);
        int visibility = -1; /* -1 none, 0 Private, 1 Public */
        if (accept_kw(&p, SXB_KW_PUBLIC)) visibility = 1;
        else if (accept_kw(&p, SXB_KW_PRIVATE)) visibility = 0;
        if (is_kw(&p, SXB_KW_SUB) || is_kw(&p, SXB_KW_FUNCTION)) {
            /* A module's procedures are Public unless they say Private. */
            struct sxb_proc_decl *proc = parse_proc(&p, visibility != 0);
            if (proc == NULL) break;
            if (proc_tail) proc_tail->next = proc; else out->procs = proc;
            proc_tail = proc;
            continue;
        }
        struct sxb_node *decls = NULL;
        if (accept_kw(&p, SXB_KW_CONST)) {
            decls = parse_const(&p, at);
        } else if (accept_kw(&p, SXB_KW_DIM)) {
            if (visibility >= 0) {
                error_at(&p, at, "write \"Public x As ...\" or \"Dim x As ...\", not both");
                break;
            }
            decls = parse_dim(&p, at)->a;
        } else if (visibility >= 0 && peek(&p)->kind == SXB_TK_IDENT) {
            decls = parse_dim(&p, at)->a;
        } else {
            if (peek(&p)->kind == SXB_TK_KEYWORD || peek(&p)->kind == SXB_TK_IDENT) {
                error_at(&p, peek(&p),
                         "only declarations go outside a procedure: Sub, Function, Dim, Const, Public or Private");
            } else {
                expected(&p, "a declaration");
            }
            break;
        }
        for (struct sxb_node *decl = decls; decl != NULL;) {
            struct sxb_node *following = decl->kind == SXB_N_VARDECL ? decl->next : NULL;
            struct sxb_global_decl *global = sxb_arena_alloc(arena, sizeof(*global));
            if (global == NULL) {
                error_at(&p, at, "out of memory");
                break;
            }
            global->decl = decl;
            global->is_public = visibility == 1;
            decl->next = NULL;
            if (global_tail) global_tail->next = global; else out->globals = global;
            global_tail = global;
            decl = following;
        }
    }
    return p.failed ? -1 : 0;
}
