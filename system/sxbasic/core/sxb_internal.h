/* SxBASIC core, the parts the compiler and the virtual machine share.
 * Not an interface: sxbasic.h is. */
#ifndef SXB_INTERNAL_H
#define SXB_INTERNAL_H

#include "sxbasic.h"

#include <stddef.h>
#include <stdint.h>

/* ---- types ------------------------------------------------------------------
 * A type is its base, plus SXB_TY_ARRAY for an array of that base. */
enum {
    SXB_TY_VOID = 0,
    SXB_TY_INT = 1,
    SXB_TY_DOUBLE = 2,
    SXB_TY_STRING = 3,
    SXB_TY_BOOL = 4,
    SXB_TY_ARRAY = 8
};
#define SXB_TY_BASE(t) ((t) & 7)
#define SXB_TY_IS_ARRAY(t) (((t) & SXB_TY_ARRAY) != 0)

const char *sxb_type_name(int type);

/* ---- memory -------------------------------------------------------------- */

struct sxb_arena {
    struct sxb_arena_block *blocks;
};
void *sxb_arena_alloc(struct sxb_arena *arena, size_t size);
char *sxb_arena_strndup(struct sxb_arena *arena, const char *text, size_t length);
void sxb_arena_free(struct sxb_arena *arena);

/* ---- tokens ---------------------------------------------------------------- */

enum sxb_token_kind {
    SXB_TK_EOF = 0,
    SXB_TK_NEWLINE,
    SXB_TK_IDENT,
    SXB_TK_KEYWORD,
    SXB_TK_INT,
    SXB_TK_DOUBLE,
    SXB_TK_STRING,
    SXB_TK_OP
};

enum sxb_keyword {
    SXB_KW_AND = 0, SXB_KW_AS, SXB_KW_BOOLEAN, SXB_KW_BYREF, SXB_KW_BYVAL, SXB_KW_CASE,
    SXB_KW_CATCH, SXB_KW_CONST, SXB_KW_DIM, SXB_KW_DO, SXB_KW_DOUBLE, SXB_KW_EACH,
    SXB_KW_ELSE, SXB_KW_ELSEIF, SXB_KW_END, SXB_KW_EXIT, SXB_KW_FALSE, SXB_KW_FOR,
    SXB_KW_FUNCTION, SXB_KW_IF, SXB_KW_IN, SXB_KW_INTEGER, SXB_KW_IS, SXB_KW_LOOP,
    SXB_KW_ME, SXB_KW_MOD, SXB_KW_NEXT, SXB_KW_NOT, SXB_KW_OR, SXB_KW_PRESERVE,
    SXB_KW_PRINT, SXB_KW_PRIVATE, SXB_KW_PUBLIC, SXB_KW_REDIM, SXB_KW_REM, SXB_KW_RETURN,
    SXB_KW_SELECT, SXB_KW_STEP, SXB_KW_STRING, SXB_KW_SUB, SXB_KW_THEN, SXB_KW_TO,
    SXB_KW_TRUE, SXB_KW_TRY, SXB_KW_UNLOAD, SXB_KW_UNTIL, SXB_KW_WHILE, SXB_KW_XOR,
    SXB_KW_COUNT
};

enum sxb_op {
    SXB_OP_PLUS = 0, SXB_OP_MINUS, SXB_OP_STAR, SXB_OP_SLASH, SXB_OP_BACKSLASH, SXB_OP_CARET,
    SXB_OP_AMP, SXB_OP_EQ, SXB_OP_NE, SXB_OP_LT, SXB_OP_GT, SXB_OP_LE, SXB_OP_GE,
    SXB_OP_LPAREN, SXB_OP_RPAREN, SXB_OP_COMMA, SXB_OP_DOT, SXB_OP_SEMICOLON,
    /* not tokens: the binary operators spelled as keywords */
    SXB_OP_MOD, SXB_OP_AND, SXB_OP_OR, SXB_OP_XOR, SXB_OP_NOT, SXB_OP_NEG
};

struct sxb_token {
    int kind;
    int sub;          /* keyword or operator */
    int line;
    int column;
    const char *text; /* identifiers: in the source, not terminated */
    int length;
    int64_t int_value;
    double double_value;
    char *string_value; /* decoded, in the arena */
    size_t string_length;
};

/* Lexes a whole file. 0, or -1 with `diag` filled in. */
int sxb_lex(struct sxb_arena *arena, const char *file, const char *text, size_t length,
            struct sxb_token **tokens, int *count, struct sxb_diagnostic *diag);

const char *sxb_keyword_name(int keyword);
int sxb_name_equal(const char *a, int a_length, const char *b);
int sxb_names_equal(const char *a, int a_length, const char *b, int b_length);

/* ---- syntax tree ------------------------------------------------------------ */

enum sxb_node_kind {
    /* expressions */
    SXB_N_INT = 1, SXB_N_DOUBLE, SXB_N_STRING, SXB_N_BOOL,
    SXB_N_NAME,    /* name, args (with HAS_PARENS when written with them) */
    SXB_N_MEMBER,  /* a = the left side, member, args (HAS_PARENS) */
    SXB_N_UNARY,   /* op, a */
    SXB_N_BINARY,  /* op, a, b */
    SXB_N_ME,
    /* statements */
    SXB_N_DIM,     /* a = list of SXB_N_VARDECL */
    SXB_N_VARDECL, /* name, type, IS_ARRAY, a = upper bound, b = initializer */
    SXB_N_CONST,   /* name, type (0 = from the value), a = value */
    SXB_N_ASSIGN,  /* a = target, b = value */
    SXB_N_CALL,    /* a = the SXB_N_NAME or SXB_N_MEMBER being called */
    SXB_N_IF,      /* a = condition, b = then, c = else (a statement list) */
    SXB_N_SELECT,  /* a = subject, b = SXB_N_CASE list, c = Case Else body or NULL, HAS_ELSE */
    SXB_N_CASE,    /* a = tests, b = body */
    SXB_N_CASE_VALUE, SXB_N_CASE_RANGE, SXB_N_CASE_IS,
    SXB_N_FOR,     /* name, a = from, b = to, c = step, d = body */
    SXB_N_FOREACH, /* name, a = array, d = body */
    SXB_N_DO,      /* a = condition, d = body, op = SXB_DO_* */
    SXB_N_EXIT,    /* op = keyword of what it leaves */
    SXB_N_RETURN,  /* a = value or NULL */
    SXB_N_TRY,     /* a = body, b = catch body */
    SXB_N_REDIM,   /* name, a = upper bound, PRESERVE */
    SXB_N_PRINT,   /* a = items, TRAILING_SEMICOLON */
    SXB_N_END,
    SXB_N_UNLOAD,  /* a = the form */
    SXB_N_PARAM    /* name, type, BYREF */
};

enum {
    SXB_F_HAS_PARENS = 1,
    SXB_F_IS_ARRAY = 2,
    SXB_F_BYREF = 4,
    SXB_F_PRESERVE = 8,
    SXB_F_TRAILING_SEMICOLON = 16,
    SXB_F_HAS_ELSE = 32,
    SXB_F_FIXED_SIZE = 64
};

enum { SXB_DO_FOREVER = 0, SXB_DO_PRE_WHILE, SXB_DO_PRE_UNTIL, SXB_DO_POST_WHILE, SXB_DO_POST_UNTIL };

struct sxb_node {
    int kind;
    int line;
    int column;
    int op;
    int flags;
    int type;            /* declared type, or the type the checker found */
    const char *name;    /* in the source, not terminated */
    int name_length;
    const char *member;  /* SXB_N_MEMBER: the part after the dot */
    int member_length;
    int64_t int_value;
    double double_value;
    const char *string_value;
    size_t string_length;
    struct sxb_node *a, *b, *c, *d;
    struct sxb_node *args;
    struct sxb_node *next;
};

struct sxb_proc_decl {
    const char *name;
    int name_length;
    int line;
    int column;
    int is_function;
    int is_public;
    int return_type;
    struct sxb_node *params;
    struct sxb_node *body;
    int module;
    int index;           /* in the program's procedure table */
    struct sxb_proc_decl *next;
};

struct sxb_global_decl {
    struct sxb_node *decl;  /* SXB_N_VARDECL or SXB_N_CONST */
    int is_public;
    struct sxb_global_decl *next;
};

struct sxb_module_ast {
    char name[64];
    char file[128];
    struct sxb_proc_decl *procs;
    struct sxb_global_decl *globals;
};

/* Parses one module's tokens. 0, or -1 with `diag` filled in. */
int sxb_parse_module(struct sxb_arena *arena, const char *file, struct sxb_token *tokens, int count,
                     int module_index, struct sxb_module_ast *out, struct sxb_diagnostic *diag);

/* ---- the compiled program ---------------------------------------------------- */

enum sxb_opcode {
    SXB_OP_NOP = 0,
    SXB_OP_PUSH_SMALL,   /* int32 */
    SXB_OP_PUSH_CONST,   /* constant index */
    SXB_OP_PUSH_DEFAULT, /* type */
    SXB_OP_LOAD,         /* location */
    SXB_OP_STORE,        /* location */
    SXB_OP_REF,          /* location */
    SXB_OP_ALOAD,        /* (array, index) -> element */
    SXB_OP_ASTORE,       /* location; (index, value) */
    SXB_OP_ANEW,         /* element type; (upper bound) -> array */
    SXB_OP_REDIM,        /* location, element type, preserve; (upper bound) */
    SXB_OP_UBOUND,
    SXB_OP_POP,
    SXB_OP_ADDI, SXB_OP_SUBI, SXB_OP_MULI, SXB_OP_IDIV, SXB_OP_MODI, SXB_OP_NEGI,
    SXB_OP_ADDD, SXB_OP_SUBD, SXB_OP_MULD, SXB_OP_DIVD, SXB_OP_POWD, SXB_OP_NEGD,
    SXB_OP_I2D,          /* the value on top */
    SXB_OP_I2D2,         /* the value under it */
    SXB_OP_CMPI, SXB_OP_CMPD, SXB_OP_CMPS, SXB_OP_CMPB, /* comparison operator */
    SXB_OP_NOTB, SXB_OP_XORB,
    SXB_OP_TOSTR,        /* type */
    SXB_OP_CONCAT,
    SXB_OP_JMP, SXB_OP_JF, SXB_OP_JT, /* address */
    SXB_OP_CALL,         /* procedure */
    SXB_OP_RET, SXB_OP_RETV,
    SXB_OP_BUILTIN,      /* builtin, argument count */
    SXB_OP_TRY,          /* catch address */
    SXB_OP_ENDTRY,
    SXB_OP_RAISE,        /* (number, description) */
    SXB_OP_ERRNUM, SXB_OP_ERRDESC,
    SXB_OP_FORTESTI, SXB_OP_FORTESTD, /* variable, end, step locations -> Boolean */
    SXB_OP_STEPCHECK,    /* location of the step; error 5 when it is 0 (0.0 has all bits 0 too) */
    SXB_OP_PRINT,        /* newline? ; (string) */
    SXB_OP_END
};

/* Builtins the VM implements; the compiler types them (sxb_compile.c). */
enum sxb_builtin {
    SXB_BI_LEN = 0, SXB_BI_LEFT, SXB_BI_RIGHT, SXB_BI_MID, SXB_BI_INSTR, SXB_BI_UCASE,
    SXB_BI_LCASE, SXB_BI_TRIM, SXB_BI_LTRIM, SXB_BI_RTRIM, SXB_BI_REPLACE, SXB_BI_SPLIT,
    SXB_BI_JOIN, SXB_BI_SPACE, SXB_BI_CHR, SXB_BI_ASC,
    SXB_BI_CINT_D, SXB_BI_CINT_S, SXB_BI_CDBL_S, SXB_BI_CBOOL_S, SXB_BI_ISNUMERIC,
    SXB_BI_ABSI, SXB_BI_ABSD, SXB_BI_SGNI, SXB_BI_SGND, SXB_BI_SQR, SXB_BI_INT, SXB_BI_FIX, SXB_BI_ROUND,
    SXB_BI_SIN, SXB_BI_COS, SXB_BI_TAN, SXB_BI_ATN, SXB_BI_EXP, SXB_BI_LOG,
    SXB_BI_RND, SXB_BI_RANDOMIZE,
    SXB_BI_READLINE, SXB_BI_READALLTEXT, SXB_BI_WRITEALLTEXT, SXB_BI_FILEEXISTS, SXB_BI_APPPATH,
    SXB_BI_COMMAND, SXB_BI_SECONDS, SXB_BI_DOEVENTS,
    SXB_BI_COUNT
};

/* A location: a local slot or a global, in one operand. */
#define SXB_LOC_LOCAL(i) ((uint32_t)(i) << 1)
#define SXB_LOC_GLOBAL(i) (((uint32_t)(i) << 1) | 1u)

struct sxb_constant {
    int type;
    int64_t int_value;
    double double_value;
    struct sxb_string *string;
};

struct sxb_proc_info {
    char name[64];
    int module;
    int param_count;
    int local_count;     /* parameters included */
    int is_function;
    int entry;           /* code offset */
};

struct sxb_program {
    char name[64];
    int windows;
    uint32_t *code;
    int32_t *lines;      /* the source line of each code word */
    uint16_t *modules;   /* the module of each code word */
    int code_length;
    int code_capacity;
    struct sxb_constant *constants;
    int constant_count;
    int constant_capacity;
    struct sxb_proc_info *procs;
    int proc_count;
    char (*module_files)[128];
    int module_count;
    int global_count;
    int entry_proc;
};

/* ---- values -------------------------------------------------------------------- */

struct sxb_string {
    uint32_t refs;
    size_t length;      /* bytes */
    char data[];        /* NUL-terminated */
};

enum { SXB_V_SCALAR = 0, SXB_V_STRING, SXB_V_ARRAY, SXB_V_REF };

struct sxb_value {
    int kind;
    union {
        int64_t i;
        double d;
        struct sxb_string *s;      /* NULL is "" */
        struct sxb_array *a;       /* NULL is an empty array */
        struct sxb_value *ref;
    } u;
};

struct sxb_array {
    uint32_t refs;
    int element_type;
    int64_t count;
    struct sxb_value *items;
};

struct sxb_string *sxb_string_new(const char *data, size_t length);
void sxb_string_release(struct sxb_string *string);

#endif
