/*
 * mew - a tiny pocket scripting language
 * shared header for the split build (core.c parse.c eval.c builtins.c main.c)
 * see README.md for usage.
 */

#ifndef MEW_H
#define MEW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <setjmp.h>

#if defined(_WIN32) || defined(_WIN64)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>         /* Sleep */
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#  include <time.h>            /* nanosleep */
#endif

#define MEW_VERSION    "1.0.12"
#define VSTACK_MAX     4096    /* max temporaries protected from gc */
#define CALL_DEPTH_MAX 512     /* max recursion depth */
#define INTERN_INITIAL 256

/* ---------- core types ----------------------------------------------- */

typedef struct Value   Value;
typedef struct Object  Object;
typedef struct StrObj  StrObj;
typedef struct ListObj ListObj;
typedef struct MapObj  MapObj;
typedef struct FnObj   FnObj;
typedef struct Env     Env;
typedef struct Node    Node;

typedef enum { V_NIL, V_BOOL, V_NUM, V_OBJ } ValTag;
typedef enum { O_STR, O_LIST, O_MAP, O_FN, O_ENV } ObjTag;

struct Value {
    ValTag tag;
    union { int b; double n; Object *o; } as;
};

struct Object { ObjTag tag; int marked; Object *next; };

struct StrObj {
    Object   base;
    int      len;
    char    *data;
    uint32_t hash;
};

struct ListObj {
    Object base;
    int    len, cap;
    Value *items;
};

typedef struct { StrObj *key; Value val; int used; } MapEntry;

struct MapObj {
    Object    base;
    int       len, cap;
    MapEntry *buckets;
};

typedef Value (*BuiltinFn)(int argc, Value *argv);

struct FnObj {
    Object       base;
    int          is_builtin;
    BuiltinFn    cfn;
    const char  *cname;
    int          nparams;
    StrObj     **params;
    Node        *body;
    Env         *closure;
    StrObj      *name;
};

struct Env {
    Object    base;
    Env      *parent;
    int       len, cap;
    StrObj  **names;
    Value    *vals;
};

/* ---------- lexer tokens (shared with AST op field) ----------------- */

typedef enum {
    T_EOF, T_NUM, T_STR, T_IDENT,
    T_IF, T_THEN, T_ELSE, T_END,
    T_WHILE, T_DO, T_FOR, T_TO, T_IN,
    T_FN, T_RETURN, T_BREAK,
    T_AND, T_OR, T_NOT,
    T_TRUE, T_FALSE, T_NIL,
    T_LP, T_RP, T_LB, T_RB, T_LC, T_RC,
    T_COMMA, T_DOT, T_ASSIGN, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CONCAT,
    T_EQ, T_NE, T_LT, T_GT, T_LE, T_GE
} TokKind;

/* ---------- AST ------------------------------------------------------ */

typedef enum {
    N_NIL, N_BOOL, N_NUM, N_STR, N_IDENT,
    N_LIST, N_MAP, N_FN,
    N_UNOP, N_BINOP, N_LOGIC, N_INDEX, N_DOT, N_CALL,
    N_ASSIGN, N_INDEX_SET, N_DOT_SET,
    N_IF, N_WHILE, N_FOR_TO, N_FOR_IN,
    N_FN_DECL, N_RETURN, N_BREAK,
    N_BLOCK, N_EXPR_STMT
} NodeKind;

struct Node {
    NodeKind kind;
    int      line;
    union { int b; double n; StrObj *s; } v;
    int      op;
    Node    *a, *b, *c;
    int      n;
    Node   **kids;
    StrObj **keys;   /* map keys, or fn params (count in op for N_FN) */
    int      ast_mark; /* gc transient: 1 if reachable from a live FnObj.body */
};

typedef struct AstChain AstChain;
struct AstChain { Node *root; AstChain *next; };

/* ---------- growable byte buffer ------------------------------------- */

typedef struct { char *data; int len, cap; } StrBuf;

/* ---------- globals (defined in core.c) ------------------------------ */

extern Object     *g_objects;
extern size_t      g_alloc_bytes;
extern size_t      g_gc_threshold;
extern Env        *g_globals;

extern Value       g_vstack[VSTACK_MAX];
extern int         g_vsp;

extern StrObj    **g_intern;
extern int         g_intern_cap, g_intern_len;
extern StrObj     *g_type_names[8];
extern AstChain   *g_ast_chain;

extern const char *g_src_name;
extern int         g_err_line;

/* partially built ast root during parse_program: the repl error handler
 * uses this to release the tree if a syntax error triggers longjmp. owned
 * by parse.c, set to NULL on success. */
extern Node       *g_parse_current;

extern int         g_break;
extern int         g_ret;
extern Value       g_ret_val;
extern int         g_call_depth;

extern jmp_buf     g_err_jmp;
extern int         g_err_jmp_set;
extern char        g_err_msg[512];
extern int         g_err_at_eof;   /* set by parser when the error was caused by hitting EOF early */

/* ---------- core.c: errors, allocators, values, collections ---------- */

#if defined(__GNUC__) || defined(__clang__)
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
#else
void die(const char *fmt, ...);
#endif

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

Value v_nil(void);
Value v_bool(int b);
Value v_num(double n);
Value v_obj(Object *o);
int   v_truthy(Value v);
int   v_equal(Value a, Value b);

void  vpush(Value v);
Value vpop(void);
int   vsave(void);
void  vrestore(int s);

void *gc_new(size_t sz, ObjTag tag);
void  gc_collect(void);
void  gc_maybe(void);
void  ast_sweep(void);
void  ast_pin_root(Node *root);
void  ast_unpin_root(Node *root);
void  ast_pin_reset(void);

void  mark_value(Value v);
void  mark_obj(Object *o);
void  mark_str(StrObj *s);
void  mark_ast(Node *n);

uint32_t str_hash(const char *s, int len);
void     mew_hash_seed_init(uint32_t s);
StrObj  *str_new(const char *s, int len);
StrObj  *intern(const char *s, int len);
StrObj  *intern_cstr(const char *s);
StrObj  *str_concat(StrObj *a, StrObj *b);

/* safe numeric truncation: double to int / long long.
 * rejects nan, +-inf and out-of-range values with die(), so callers never
 * execute the undefined C behaviour of converting such a double to an
 * integer. 'ctx' is the caller name used in the error message. */
int        safe_dtoi  (double x, const char *ctx);
long long  safe_dtoll (double x, const char *ctx);

ListObj *list_new(void);
void     list_push(ListObj *l, Value v);
Value    list_pop(ListObj *l);
ListObj *list_concat(ListObj *a, ListObj *b);
ListObj *list_slice(ListObj *l, int i, int j);

MapObj *map_new(void);
void    map_set(MapObj *m, StrObj *k, Value val);
int     map_get(MapObj *m, StrObj *k, Value *out);
int     map_del(MapObj *m, StrObj *k);

Env *env_new(Env *parent);
void env_define(Env *e, StrObj *name, Value v);
void env_assign(Env *e, StrObj *name, Value v);
int  env_lookup(Env *e, StrObj *name, Value *out);

void sb_init(StrBuf *b);
void sb_putc(StrBuf *b, char c);
void sb_puts(StrBuf *b, const char *s);
void sb_putn(StrBuf *b, const char *s, int n);
void sb_free(StrBuf *b);

int         cmp_value_str(const void *a, const void *b);
void        value_format(StrBuf *b, Value v, int repr);
void        value_format_depth(StrBuf *b, Value v, int repr, int depth);
void        print_value_to(FILE *fp, Value v, int repr);
const char *type_cname(Value v);
StrObj     *value_to_str(Value v);

/* ---------- parse.c: lexer and parser -------------------------------- */

Node *parse_program(const char *src);
void  free_node(Node *n);
void  ast_keep(Node *root);

/* ---------- eval.c: tree-walking interpreter ------------------------- */

Value eval_node(Node *n, Env *env);
void  exec_block(Node *blk, Env *env);
Value call_value(Value fv, int argc, Value *argv);
void  run_source(const char *src);
int   parse_needs_more(const char *src);

/* ---------- builtins.c: standard library registration ---------------- */

void install_builtins(Env *e);
void seed_rng_from_time(void);

/* args list: defined in builtins.c, populated by main.c at startup */
extern ListObj *g_args_list;

/* ---------- main.c: slurp helper, shared with builtins load() -------- */

char *slurp(const char *path);

#endif /* MEW_H */
