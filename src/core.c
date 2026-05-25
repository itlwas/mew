/*
 * mew core: globals, errors, allocators, values, gc, strings, lists, maps,
 * environments, strbuf, value formatting and equality.
 */

#include "mew.h"

/* ========================================================================
 * globals
 * ====================================================================== */

Object     *g_objects       = NULL;
size_t      g_alloc_bytes   = 0;
size_t      g_gc_threshold  = 1u << 16;
Env        *g_globals       = NULL;

Value       g_vstack[VSTACK_MAX];
int         g_vsp           = 0;

StrObj    **g_intern        = NULL;
int         g_intern_cap    = 0;
int         g_intern_len    = 0;
StrObj     *g_type_names[8] = {0};
AstChain   *g_ast_chain     = NULL;

const char *g_src_name      = "?";
int         g_err_line      = 0;
Node       *g_parse_current = NULL;

int         g_break         = 0;
int         g_ret           = 0;
Value       g_ret_val;
int         g_call_depth    = 0;

jmp_buf     g_err_jmp;
int         g_err_jmp_set   = 0;
char        g_err_msg[512];
int         g_err_at_eof    = 0;

/* ========================================================================
 * errors and allocators
 * ====================================================================== */

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err_msg, sizeof(g_err_msg), fmt, ap);
    va_end(ap);
    /* flush stdout before reporting: otherwise a buffered print right
     * before the error would vanish, and the user would see the error
     * message without the last output that produced it. */
    fflush(stdout);
    if (g_err_jmp_set) longjmp(g_err_jmp, 1);
    fprintf(stderr, "mew: %s:%d: %s\n", g_src_name, g_err_line, g_err_msg);
    exit(1);
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "mew: out of memory\n"); exit(2); }
    return p;
}
void *xrealloc(void *p, size_t n) {
    void *r = realloc(p, n ? n : 1);
    if (!r) { fprintf(stderr, "mew: out of memory\n"); exit(2); }
    return r;
}

/* ========================================================================
 * safe numeric coercions
 *
 * casting a non-finite or out-of-range double to an integer is undefined
 * behaviour in C99. instead of relying on the compiler silently producing
 * a garbage value (or raising a trap on some cpus), we validate first and
 * raise a language error with a helpful context string. these are used
 * everywhere the interpreter has to turn a user-supplied number into an
 * index, count or byte value.
 * ====================================================================== */

int safe_dtoi(double x, const char *ctx) {
    if (isnan(x))       die("%s: expected integer, got nan", ctx);
    if (isinf(x))       die("%s: expected integer, got %sinf", ctx, x < 0 ? "-" : "");
    if (x <  -2147483648.0 || x > 2147483647.0)
        die("%s: value %g does not fit in int", ctx, x);
    return (int)x;
}
long long safe_dtoll(double x, const char *ctx) {
    if (isnan(x))       die("%s: expected integer, got nan", ctx);
    if (isinf(x))       die("%s: expected integer, got %sinf", ctx, x < 0 ? "-" : "");
    /* doubles cannot represent 2^63 exactly, use conservative bounds */
    if (x <  -9.2233720368547748e18 || x > 9.2233720368547748e18)
        die("%s: value %g does not fit in long long", ctx, x);
    return (long long)x;
}

/* ========================================================================
 * values and stack
 * ====================================================================== */

Value v_nil(void)       { Value v; v.tag = V_NIL;  v.as.n = 0;       return v; }
Value v_bool(int b)     { Value v; v.tag = V_BOOL; v.as.b = b ? 1:0; return v; }
Value v_num(double n)   { Value v; v.tag = V_NUM;  v.as.n = n;       return v; }
Value v_obj(Object *o)  { Value v; v.tag = V_OBJ;  v.as.o = o;       return v; }

int v_truthy(Value v) {
    if (v.tag == V_NIL)  return 0;
    if (v.tag == V_BOOL) return v.as.b;
    return 1;
}

void  vpush(Value v)  { if (g_vsp >= VSTACK_MAX) die("value stack overflow"); g_vstack[g_vsp++] = v; }
Value vpop(void)      { return g_vstack[--g_vsp]; }
int   vsave(void)     { return g_vsp; }
void  vrestore(int s) { g_vsp = s; }

/* ========================================================================
 * gc
 *
 * marker uses an explicit work stack so a deeply nested list or map does
 * not recurse through mark_list / mark_map and overflow the c stack. the
 * stack is heap-allocated and grows on demand; a failure to grow falls
 * back to an allocation error via xrealloc.
 * ====================================================================== */

static Object **g_mark_stack = NULL;
static int       g_mark_cap  = 0;
static int       g_mark_top  = 0;

static void mark_push(Object *o) {
    if (!o || o->marked) return;
    if (g_mark_top >= g_mark_cap) {
        /* saturating grow, mirrors list_push: cap*2 can't overflow int. */
        const int MAX_CAP = 2147483000 / (int)sizeof(Object *);
        int nc;
        if (g_mark_cap == 0)               nc = 256;
        else if (g_mark_cap > MAX_CAP / 2) nc = MAX_CAP;
        else                               nc = g_mark_cap * 2;
        if (nc <= g_mark_cap) {
            fprintf(stderr, "mew: gc mark stack capacity overflow\n");
            exit(2);
        }
        g_mark_stack = (Object **)xrealloc(g_mark_stack, sizeof(Object *) * (size_t)nc);
        g_mark_cap = nc;
    }
    g_mark_stack[g_mark_top++] = o;
}

void *gc_new(size_t sz, ObjTag tag) {
    Object *o = (Object *)xmalloc(sz);
    memset(o, 0, sz);
    o->tag = tag;
    o->next = g_objects;
    g_objects = o;
    g_alloc_bytes += sz;
    return o;
}

void mark_str(StrObj *s) { if (s) s->base.marked = 1; }

static void mark_list(ListObj *l);
static void mark_map(MapObj *m);
static void mark_env(Env *e);
static void mark_fn(FnObj *f);

void mark_value(Value v) { if (v.tag == V_OBJ) mark_obj(v.as.o); }

void mark_obj(Object *o) {
    if (!o || o->marked) return;
    /* defer structural visits to dispatch below so this stays shallow. */
    switch (o->tag) {
        case O_STR:  mark_str((StrObj *)o);   break;
        case O_LIST: mark_list((ListObj *)o); break;
        case O_MAP:  mark_map((MapObj *)o);   break;
        case O_FN:   mark_fn((FnObj *)o);     break;
        case O_ENV:  mark_env((Env *)o);      break;
    }
}

static void mark_list(ListObj *l) {
    if (l->base.marked) return;
    l->base.marked = 1;
    /* iterate into children via mark_push + mark_drain so a deeply nested
     * list does not recurse through mark_list -> mark_obj -> mark_list... */
    for (int i = 0; i < l->len; i++)
        if (l->items[i].tag == V_OBJ) mark_push(l->items[i].as.o);
}
static void mark_map(MapObj *m) {
    if (m->base.marked) return;
    m->base.marked = 1;
    for (int i = 0; i < m->cap; i++) {
        if (!m->buckets[i].used) continue;
        mark_str(m->buckets[i].key);
        if (m->buckets[i].val.tag == V_OBJ) mark_push(m->buckets[i].val.as.o);
    }
}
static void mark_env(Env *e) {
    /* iterative walk up the closure chain so a long chain of nested
     * closures cannot overflow the C call stack during gc marking. */
    while (e && !e->base.marked) {
        e->base.marked = 1;
        for (int i = 0; i < e->len; i++) {
            mark_str(e->names[i]);
            if (e->vals[i].tag == V_OBJ) mark_push(e->vals[i].as.o);
        }
        e = e->parent;
    }
}
static void mark_fn(FnObj *f) {
    if (f->base.marked) return;
    f->base.marked = 1;
    if (f->is_builtin) return;
    for (int i = 0; i < f->nparams; i++) mark_str(f->params[i]);
    if (f->closure) mark_push((Object *)f->closure);
    mark_str(f->name);
}

static void mark_drain(void) {
    while (g_mark_top > 0) {
        Object *o = g_mark_stack[--g_mark_top];
        mark_obj(o);
    }
}

/* mark_ast is used by the gc to keep live string tokens from the AST.
 * a long left-leaning chain of binary operators (a+b+c+... with thousands
 * of plusses) produces a deeply nested N_BINOP tree along the `a` pointer;
 * a recursive walk there overflows the C stack on windows (1 MB default).
 *
 * we use the same work-stack machinery as the gc object marker, but with
 * a separate buffer so the two walks can coexist. enqueue all children,
 * drain one node at a time. */
static Node **g_ast_stack = NULL;
static int    g_ast_cap   = 0;
static int    g_ast_top   = 0;

static void ast_push(Node *n) {
    if (!n) return;
    if (g_ast_top >= g_ast_cap) {
        /* saturating grow, mirrors list_push. */
        const int MAX_CAP = 2147483000 / (int)sizeof(Node *);
        int nc;
        if (g_ast_cap == 0)               nc = 256;
        else if (g_ast_cap > MAX_CAP / 2) nc = MAX_CAP;
        else                              nc = g_ast_cap * 2;
        if (nc <= g_ast_cap) {
            fprintf(stderr, "mew: ast walk stack capacity overflow\n");
            exit(2);
        }
        g_ast_stack = (Node **)xrealloc(g_ast_stack, sizeof(Node *) * (size_t)nc);
        g_ast_cap = nc;
    }
    g_ast_stack[g_ast_top++] = n;
}

void mark_ast(Node *root) {
    if (!root) return;
    ast_push(root);
    while (g_ast_top > 0) {
        Node *n = g_ast_stack[--g_ast_top];
        switch (n->kind) {
            case N_STR: case N_IDENT: case N_DOT: case N_ASSIGN:
            case N_DOT_SET: case N_FN_DECL: case N_FOR_TO: case N_FOR_IN:
                mark_str(n->v.s);
                break;
            default: break;
        }
        if (n->keys) {
            int nkeys = 0;
            if      (n->kind == N_MAP) nkeys = n->n;
            else if (n->kind == N_FN)  nkeys = n->op;
            for (int i = 0; i < nkeys; i++) mark_str(n->keys[i]);
        }
        ast_push(n->a); ast_push(n->b); ast_push(n->c);
        for (int i = 0; i < n->n; i++) ast_push(n->kids[i]);
    }
}

/* iterative dfs that returns 1 if any node in the subtree has ast_mark
 * set. used during gc to decide whether an AstChain root is still
 * reachable from a live FnObj.body (which the evaluator pinned by
 * setting ast_mark on the body node before mark-pass). */
static int ast_subtree_has_mark(Node *root) {
    if (!root) return 0;
    g_ast_top = 0;
    ast_push(root);
    while (g_ast_top > 0) {
        Node *n = g_ast_stack[--g_ast_top];
        if (n->ast_mark) { g_ast_top = 0; return 1; }
        ast_push(n->a); ast_push(n->b); ast_push(n->c);
        for (int i = 0; i < n->n; i++) ast_push(n->kids[i]);
    }
    return 0;
}

static void free_obj(Object *o) {
    switch (o->tag) {
        case O_STR:  free(((StrObj *)o)->data);    break;
        case O_LIST: free(((ListObj *)o)->items);  break;
        case O_MAP:  free(((MapObj *)o)->buckets); break;
        case O_ENV:  free(((Env *)o)->names); free(((Env *)o)->vals); break;
        case O_FN: {
            FnObj *f = (FnObj *)o;
            if (!f->is_builtin && f->params) free(f->params);
        } break;
    }
    free(o);
}

static void intern_prune(void) {
    int live = 0;
    for (int i = 0; i < g_intern_cap; i++) if (g_intern[i] && g_intern[i]->base.marked) live++;
    if (live == g_intern_len) return;

    StrObj **nb = (StrObj **)xmalloc(sizeof(StrObj *) * (size_t)g_intern_cap);
    for (int i = 0; i < g_intern_cap; i++) nb[i] = NULL;
    int mask = g_intern_cap - 1;
    for (int i = 0; i < g_intern_cap; i++) {
        StrObj *s = g_intern[i];
        if (!s || !s->base.marked) continue;
        int idx = (int)(s->hash & (uint32_t)mask);
        while (nb[idx]) idx = (idx + 1) & mask;
        nb[idx] = s;
    }
    free(g_intern);
    g_intern = nb;
    g_intern_len = live;
}

void gc_collect(void) {
    /* roots: globals env, value stack, kept ASTs (for function bodies),
     * the pending return value and cached type names. marking into
     * children uses mark_push, so we drain the work stack at the end. */
    mark_env(g_globals);
    for (int i = 0; i < g_vsp; i++) mark_value(g_vstack[i]);
    for (AstChain *c = g_ast_chain; c; c = c->next) mark_ast(c->root);
    mark_value(g_ret_val);
    for (int i = 0; i < (int)(sizeof(g_type_names)/sizeof(g_type_names[0])); i++) mark_str(g_type_names[i]);
    mark_drain();
    intern_prune();

    Object **p = &g_objects;
    while (*p) {
        Object *o = *p;
        if (!o->marked) { *p = o->next; free_obj(o); }
        else            { o->marked = 0; p = &o->next; }
    }
    g_alloc_bytes = 0;
    if (g_gc_threshold < (1u << 24)) g_gc_threshold *= 2;
}

/* AST sweep: drop AstChain entries whose subtree no live FnObj.body
 * points into. only safe to call from a quiescent point (between
 * top-level evaluations), never from inside exec_block, because
 * eval_node may hold transient pointers into the AST that are not
 * visible through any reachable FnObj.
 *
 * we walk all live FnObj in g_objects and ast_mark every reachable
 * node from each f->body subtree. then we sweep g_ast_chain: trees
 * with at least one ast_mark stay; the rest are freed. cleanup
 * clears all marks before returning. without this sweep every
 * successful run_source / bi_load leaks the full Node* tree
 * (audit F-101).
 *
 * roots that are *currently being executed* must not be freed: the
 * caller pins them by pushing onto g_ast_pin_stack via
 * ast_pin_root / ast_unpin_root. the sweeper treats those as live
 * regardless of FnObj reachability. */
static Node **g_ast_pin_stack = NULL;
static int    g_ast_pin_cap   = 0;
static int    g_ast_pin_top   = 0;

void ast_pin_root(Node *root) {
    if (g_ast_pin_top >= g_ast_pin_cap) {
        /* saturating grow, mirrors list_push. */
        const int MAX_CAP = 2147483000 / (int)sizeof(Node *);
        int nc;
        if (g_ast_pin_cap == 0)               nc = 16;
        else if (g_ast_pin_cap > MAX_CAP / 2) nc = MAX_CAP;
        else                                  nc = g_ast_pin_cap * 2;
        if (nc <= g_ast_pin_cap) {
            fprintf(stderr, "mew: ast pin stack capacity overflow\n");
            exit(2);
        }
        g_ast_pin_stack = (Node **)xrealloc(g_ast_pin_stack, sizeof(Node *) * (size_t)nc);
        g_ast_pin_cap = nc;
    }
    g_ast_pin_stack[g_ast_pin_top++] = root;
}
void ast_unpin_root(Node *root) {
    /* expected to match the most recent pin; defensive: scan to find */
    for (int i = g_ast_pin_top - 1; i >= 0; i--) {
        if (g_ast_pin_stack[i] == root) {
            for (int j = i; j < g_ast_pin_top - 1; j++)
                g_ast_pin_stack[j] = g_ast_pin_stack[j + 1];
            g_ast_pin_top--;
            return;
        }
    }
}
void ast_pin_reset(void) { g_ast_pin_top = 0; }

static void ast_mark_subtree(Node *root) {
    if (!root) return;
    g_ast_top = 0;
    ast_push(root);
    while (g_ast_top > 0) {
        Node *n = g_ast_stack[--g_ast_top];
        n->ast_mark = 1;
        ast_push(n->a); ast_push(n->b); ast_push(n->c);
        for (int i = 0; i < n->n; i++) ast_push(n->kids[i]);
    }
}
static void ast_clear_subtree(Node *root) {
    if (!root) return;
    g_ast_top = 0;
    ast_push(root);
    while (g_ast_top > 0) {
        Node *n = g_ast_stack[--g_ast_top];
        n->ast_mark = 0;
        ast_push(n->a); ast_push(n->b); ast_push(n->c);
        for (int i = 0; i < n->n; i++) ast_push(n->kids[i]);
    }
}

void ast_sweep(void) {
    /* mark all subtrees reachable from any live (non-builtin) FnObj */
    for (Object *o = g_objects; o; o = o->next) {
        if (o->tag != O_FN) continue;
        FnObj *f = (FnObj *)o;
        if (f->is_builtin) continue;
        if (f->body) ast_mark_subtree(f->body);
    }
    /* mark every pinned root: these are currently being executed and
     * must survive the sweep regardless of FnObj reachability */
    for (int i = 0; i < g_ast_pin_top; i++)
        ast_mark_subtree(g_ast_pin_stack[i]);
    /* drop AstChain entries whose root has no marked descendants */
    AstChain **ap = &g_ast_chain;
    while (*ap) {
        AstChain *c = *ap;
        if (ast_subtree_has_mark(c->root)) {
            ast_clear_subtree(c->root);
            ap = &c->next;
        } else {
            *ap = c->next;
            free_node(c->root);
            free(c);
        }
    }
}

void gc_maybe(void) { if (g_alloc_bytes > g_gc_threshold) gc_collect(); }

/* ========================================================================
 * strings and interning
 * ====================================================================== */

/* per-process hash seed: defends against precomputed FNV-1a collisions
 * being weaponised to flood map buckets with same-modulus keys, turning
 * O(1) lookups into O(n^2) (audit F-204). seeded from time() at startup
 * so identical scripts run-to-run still produce reproducible output for
 * deterministic operations like sorted iteration; only the bucket layout
 * varies between processes. */
static uint32_t g_hash_seed = 0x811c9dc5u;
void mew_hash_seed_init(uint32_t s) { g_hash_seed = s ? s : 0x811c9dc5u; }

uint32_t str_hash(const char *s, int len) {
    uint32_t h = g_hash_seed;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h ? h : 1;
}

StrObj *str_new(const char *s, int len) {
    StrObj *o = (StrObj *)gc_new(sizeof(StrObj), O_STR);
    o->len  = len;
    o->data = (char *)xmalloc((size_t)len + 1);
    if (len > 0) memcpy(o->data, s, (size_t)len);
    o->data[len] = 0;
    o->hash = str_hash(s, len);
    /* account for the string payload in addition to the struct itself,
     * so gc triggers at the correct threshold under heavy string load */
    g_alloc_bytes += (size_t)len + 1;
    return o;
}

static void intern_grow(void) {
    const int MAX_INTERN = 1 << 29;
    int ncap;
    if (g_intern_cap == 0)                ncap = INTERN_INITIAL;
    else if (g_intern_cap > MAX_INTERN/2) ncap = MAX_INTERN;
    else                                  ncap = g_intern_cap * 2;
    if (ncap <= g_intern_cap) die("intern: capacity overflow");
    StrObj **nb = (StrObj **)xmalloc(sizeof(StrObj *) * (size_t)ncap);
    for (int i = 0; i < ncap; i++) nb[i] = NULL;
    int mask = ncap - 1;
    for (int i = 0; i < g_intern_cap; i++) {
        StrObj *s = g_intern[i];
        if (!s) continue;
        int idx = (int)(s->hash & (uint32_t)mask);
        while (nb[idx]) idx = (idx + 1) & mask;
        nb[idx] = s;
    }
    free(g_intern);
    g_intern = nb;
    g_intern_cap = ncap;
}

StrObj *intern(const char *s, int len) {
    if (g_intern_cap == 0) intern_grow();
    /* load factor: keep below 0.75 with overflow-safe arithmetic */
    if (g_intern_len + 1 > g_intern_cap - g_intern_cap / 4) intern_grow();
    uint32_t h = str_hash(s, len);
    int mask = g_intern_cap - 1;
    int idx  = (int)(h & (uint32_t)mask);
    for (;;) {
        StrObj *e = g_intern[idx];
        if (!e) break;
        if (e->hash == h && e->len == len && memcmp(e->data, s, (size_t)len) == 0) return e;
        idx = (idx + 1) & mask;
    }
    StrObj *n = str_new(s, len);
    g_intern[idx] = n;
    g_intern_len++;
    return n;
}
StrObj *intern_cstr(const char *s) { return intern(s, (int)strlen(s)); }

StrObj *str_concat(StrObj *a, StrObj *b) {
    /* guard against int overflow: 2*INT_MAX would wrap to a small negative
     * length, which would then be cast to an enormous size_t. worse, if it
     * wraps to a small positive int (e.g. two strings of INT_MAX/2 + 1),
     * xmalloc allocates too little and the memcpy writes out of bounds. */
    if (a->len < 0 || b->len < 0 || a->len > 2147483000 - b->len)
        die("string too large: %d + %d bytes", a->len, b->len);
    int len = a->len + b->len;
    StrObj *r = (StrObj *)gc_new(sizeof(StrObj), O_STR);
    r->len  = len;
    r->data = (char *)xmalloc((size_t)len + 1);
    if (a->len) memcpy(r->data,         a->data, (size_t)a->len);
    if (b->len) memcpy(r->data + a->len, b->data, (size_t)b->len);
    r->data[len] = 0;
    r->hash = str_hash(r->data, len);
    g_alloc_bytes += (size_t)len + 1;
    return r;
}

/* ========================================================================
 * lists
 * ====================================================================== */

ListObj *list_new(void) {
    ListObj *l = (ListObj *)gc_new(sizeof(ListObj), O_LIST);
    l->len = l->cap = 0; l->items = NULL;
    return l;
}
void list_push(ListObj *l, Value v) {
    if (l->len >= l->cap) {
        /* grow with saturation so cap*2 can't overflow int and then pass
         * the len>=cap check with a negative cap. caps out at ~2G entries
         * which is already far beyond any realistic script. */
        const int MAX_CAP = 2147483000 / (int)sizeof(Value);
        int nc;
        if (l->cap == 0)              nc = 8;
        else if (l->cap > MAX_CAP / 2) nc = MAX_CAP;
        else                          nc = l->cap * 2;
        if (nc <= l->cap) die("list: capacity overflow");
        l->items = (Value *)xrealloc(l->items, sizeof(Value) * (size_t)nc);
        g_alloc_bytes += sizeof(Value) * (size_t)(nc - l->cap);
        l->cap = nc;
    }
    l->items[l->len++] = v;
}
Value list_pop(ListObj *l) {
    if (l->len == 0) die("pop from empty list");
    return l->items[--l->len];
}
ListObj *list_concat(ListObj *a, ListObj *b) {
    ListObj *r = list_new();
    vpush(v_obj((Object *)r));
    for (int i = 0; i < a->len; i++) list_push(r, a->items[i]);
    for (int i = 0; i < b->len; i++) list_push(r, b->items[i]);
    vpop();
    return r;
}
ListObj *list_slice(ListObj *l, int i, int j) {
    if (i < 0) i += l->len;
    if (j < 0) j += l->len;
    if (i < 0) i = 0;
    if (j > l->len) j = l->len;
    if (j < i) j = i;
    ListObj *r = list_new();
    vpush(v_obj((Object *)r));
    for (int k = i; k < j; k++) list_push(r, l->items[k]);
    vpop();
    return r;
}

/* ========================================================================
 * maps (string keys)
 * ====================================================================== */

MapObj *map_new(void) {
    MapObj *m = (MapObj *)gc_new(sizeof(MapObj), O_MAP);
    m->len = m->cap = 0; m->buckets = NULL;
    return m;
}
static void map_resize(MapObj *m, int ncap) {
    MapEntry *nb = (MapEntry *)xmalloc(sizeof(MapEntry) * (size_t)ncap);
    for (int i = 0; i < ncap; i++) { nb[i].used = 0; nb[i].key = NULL; }
    int mask = ncap - 1;
    for (int i = 0; i < m->cap; i++) {
        if (!m->buckets[i].used) continue;
        StrObj *k = m->buckets[i].key;
        int idx = (int)(k->hash & (uint32_t)mask);
        while (nb[idx].used) idx = (idx + 1) & mask;
        nb[idx] = m->buckets[i];
    }
    free(m->buckets);
    m->buckets = nb;
    /* delta accounting (audit F-110): only count the new bytes, not the
     * full new table, otherwise resize triggers gc trigger too early. */
    g_alloc_bytes += sizeof(MapEntry) * (size_t)(ncap - m->cap);
    m->cap = ncap;
}
void map_set(MapObj *m, StrObj *k, Value val) {
    if (m->cap == 0) map_resize(m, 8);
    /* load factor check written overflow-safe: expands when len+1 exceeds
     * 3/4 of cap. the old "(len+1)*4 > cap*3" could wrap int at huge sizes. */
    if (m->len + 1 > m->cap - m->cap / 4) {
        const int MAX_CAP = 1 << 29;
        int nc;
        if (m->cap > MAX_CAP / 2) nc = MAX_CAP;
        else                      nc = m->cap * 2;
        if (nc <= m->cap) die("map: capacity overflow");
        map_resize(m, nc);
    }
    int mask = m->cap - 1;
    int idx  = (int)(k->hash & (uint32_t)mask);
    for (;;) {
        if (!m->buckets[idx].used) {
            m->buckets[idx].used = 1;
            m->buckets[idx].key  = k;
            m->buckets[idx].val  = val;
            m->len++;
            return;
        }
        StrObj *ek = m->buckets[idx].key;
        if (ek == k || (ek->len == k->len && ek->hash == k->hash &&
                        memcmp(ek->data, k->data, (size_t)k->len) == 0)) {
            m->buckets[idx].val = val;
            return;
        }
        idx = (idx + 1) & mask;
    }
}
int map_get(MapObj *m, StrObj *k, Value *out) {
    if (m->cap == 0) return 0;
    int mask = m->cap - 1;
    int idx  = (int)(k->hash & (uint32_t)mask);
    for (;;) {
        if (!m->buckets[idx].used) return 0;
        StrObj *ek = m->buckets[idx].key;
        if (ek == k || (ek->len == k->len && ek->hash == k->hash &&
                        memcmp(ek->data, k->data, (size_t)k->len) == 0)) {
            *out = m->buckets[idx].val;
            return 1;
        }
        idx = (idx + 1) & mask;
    }
}
int map_del(MapObj *m, StrObj *k) {
    if (m->cap == 0) return 0;
    int mask = m->cap - 1;
    int idx  = (int)(k->hash & (uint32_t)mask);
    for (;;) {
        if (!m->buckets[idx].used) return 0;
        StrObj *ek = m->buckets[idx].key;
        if (ek == k || (ek->len == k->len && ek->hash == k->hash &&
                        memcmp(ek->data, k->data, (size_t)k->len) == 0)) {
            m->buckets[idx].used = 0;
            m->len--;
            int j = (idx + 1) & mask;
            while (m->buckets[j].used) {
                MapEntry e = m->buckets[j];
                m->buckets[j].used = 0;
                m->len--;
                map_set(m, e.key, e.val);
                j = (j + 1) & mask;
            }
            return 1;
        }
        idx = (idx + 1) & mask;
    }
}

/* ========================================================================
 * environments
 * ====================================================================== */

Env *env_new(Env *parent) {
    Env *e = (Env *)gc_new(sizeof(Env), O_ENV);
    e->parent = parent;
    e->len = e->cap = 0; e->names = NULL; e->vals = NULL;
    return e;
}
static int env_find_here(Env *e, StrObj *name) {
    for (int i = 0; i < e->len; i++) if (e->names[i] == name) return i;
    return -1;
}
void env_define(Env *e, StrObj *name, Value v) {
    int idx = env_find_here(e, name);
    if (idx >= 0) { e->vals[idx] = v; return; }
    if (e->len >= e->cap) {
        /* saturated doubling so cap*2 cannot wrap negative */
        const int MAX_CAP = 2147483000 / (int)sizeof(Value);
        int nc;
        if (e->cap == 0)              nc = 4;
        else if (e->cap > MAX_CAP / 2) nc = MAX_CAP;
        else                          nc = e->cap * 2;
        if (nc <= e->cap) die("env: capacity overflow");
        e->names = (StrObj **)xrealloc(e->names, sizeof(StrObj *) * (size_t)nc);
        e->vals  = (Value *)  xrealloc(e->vals,  sizeof(Value)    * (size_t)nc);
        /* delta accounting so gc fires on real growth (audit F-111) */
        g_alloc_bytes += (sizeof(StrObj *) + sizeof(Value)) * (size_t)(nc - e->cap);
        e->cap = nc;
    }
    e->names[e->len] = name;
    e->vals[e->len]  = v;
    e->len++;
}
void env_assign(Env *e, StrObj *name, Value v) {
    for (Env *cur = e; cur; cur = cur->parent) {
        int idx = env_find_here(cur, name);
        if (idx >= 0) { cur->vals[idx] = v; return; }
    }
    env_define(e, name, v);
}
int env_lookup(Env *e, StrObj *name, Value *out) {
    for (Env *cur = e; cur; cur = cur->parent) {
        int idx = env_find_here(cur, name);
        if (idx >= 0) { *out = cur->vals[idx]; return 1; }
    }
    return 0;
}

/* ========================================================================
 * strbuf
 * ====================================================================== */

void sb_init(StrBuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void sb_need(StrBuf *b, int more) {
    /* guard against pathological int overflow: if b->len + more + 1 wraps
     * past INT_MAX the comparison loop would spin forever or allocate a
     * gigantic buffer. cap grow also has to saturate at INT_MAX. */
    if (more < 0 || b->len < 0 || more > 2147483000 - b->len - 1)
        die("string buffer too large");
    int need = b->len + more + 1;
    if (need > b->cap) {
        int nc = b->cap ? b->cap : 64;
        while (nc < need) {
            if (nc > 2147483000 / 2) { nc = 2147483000; break; }
            nc *= 2;
        }
        if (nc < need) die("string buffer too large");
        b->data = (char *)xrealloc(b->data, (size_t)nc);
        b->cap = nc;
    }
}
void sb_putc(StrBuf *b, char c)               { sb_need(b, 1); b->data[b->len++] = c; }
void sb_puts(StrBuf *b, const char *s)        { int n = (int)strlen(s); sb_need(b, n); memcpy(b->data + b->len, s, (size_t)n); b->len += n; }
void sb_putn(StrBuf *b, const char *s, int n) { sb_need(b, n); if (n) memcpy(b->data + b->len, s, (size_t)n); b->len += n; }
void sb_free(StrBuf *b)                       { free(b->data); b->data = NULL; b->len = b->cap = 0; }

/* ========================================================================
 * value printing, equality, typing
 * ====================================================================== */

int cmp_value_str(const void *a, const void *b) {
    /* comparator for an array of Value that all hold StrObj. used by for-in
     * over a map and by bi_keys / bi_values to sort keys in-place. */
    const Value *x = (const Value *)a;
    const Value *y = (const Value *)b;
    const StrObj *xs = (const StrObj *)x->as.o;
    const StrObj *ys = (const StrObj *)y->as.o;
    int n = xs->len < ys->len ? xs->len : ys->len;
    int c = memcmp(xs->data, ys->data, (size_t)n);
    if (c) return c;
    return xs->len < ys->len ? -1 : (xs->len > ys->len ? 1 : 0);
}

static void format_num(StrBuf *b, double n) {
    char buf[64];
    int len;
    if (isnan(n))      { sb_puts(b, "nan");  return; }
    if (isinf(n))      { sb_puts(b, n > 0 ? "inf" : "-inf"); return; }
    /* check the int-representable range FIRST so the (long long) cast
     * inside the guarded branch is well defined. the previous code
     * evaluated n == (double)(long long)n unconditionally and would
     * invoke UB when n was, say, 1e30. */
    if (n > -9.2233720368547748e18 && n < 9.2233720368547748e18) {
        long long ll = (long long)n;
        if (n == (double)ll && n > -1e18 && n < 1e18) {
            len = snprintf(buf, sizeof(buf), "%lld", ll);
            /* defensive clamp: snprintf returning a negative value (encoding
             * error) or >= sizeof(buf) (truncated) would have caused
             * sb_putn to read out of bounds. neither is reachable with the
             * format strings we use here, but cheaper to be sure. (audit
             * F-201). */
            if (len < 0 || len >= (int)sizeof(buf)) len = 0;
            sb_putn(b, buf, len);
            return;
        }
    }
    len = snprintf(buf, sizeof(buf), "%.14g", n);
    if (len < 0 || len >= (int)sizeof(buf)) len = 0;
    sb_putn(b, buf, len);
}

static void format_str_repr(StrBuf *b, StrObj *s) {
    sb_putc(b, '"');
    for (int i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->data[i];
        switch (c) {
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            case '\n': sb_puts(b, "\\n");  break;
            case '\t': sb_puts(b, "\\t");  break;
            case '\r': sb_puts(b, "\\r");  break;
            default:   sb_putc(b, (char)c); break;
        }
    }
    sb_putc(b, '"');
}

static void format_list(StrBuf *b, ListObj *l, int depth) {
    if (depth <= 0) { sb_puts(b, "[...]"); return; }
    sb_putc(b, '[');
    for (int i = 0; i < l->len; i++) {
        if (i) sb_puts(b, ", ");
        value_format_depth(b, l->items[i], 1, depth - 1);
    }
    sb_putc(b, ']');
}

static void format_map(StrBuf *b, MapObj *m, int depth) {
    if (depth <= 0) { sb_puts(b, "{...}"); return; }
    sb_putc(b, '{');
    /* collect keys in a transient vstack slot so that a die() deep inside
     * value_format_depth cannot leak the raw key array. gc reclaims the
     * list on the next sweep; we vpop on the success path. */
    ListObj *klist = list_new();
    vpush(v_obj((Object *)klist));
    for (int i = 0; i < m->cap; i++)
        if (m->buckets[i].used) list_push(klist, v_obj((Object *)m->buckets[i].key));
    if (klist->len > 1)
        qsort(klist->items, (size_t)klist->len, sizeof(Value), cmp_value_str);
    for (int i = 0; i < klist->len; i++) {
        if (i) sb_puts(b, ", ");
        StrObj *key = (StrObj *)klist->items[i].as.o;
        format_str_repr(b, key);
        sb_puts(b, ": ");
        Value v;
        map_get(m, key, &v);
        value_format_depth(b, v, 1, depth - 1);
    }
    vpop();
    sb_putc(b, '}');
}

void value_format_depth(StrBuf *b, Value v, int repr, int depth) {
    switch (v.tag) {
        case V_NIL:  sb_puts(b, "nil"); break;
        case V_BOOL: sb_puts(b, v.as.b ? "true" : "false"); break;
        case V_NUM:  format_num(b, v.as.n); break;
        case V_OBJ: {
            Object *o = v.as.o;
            switch (o->tag) {
                case O_STR:
                    if (repr) format_str_repr(b, (StrObj *)o);
                    else      sb_putn(b, ((StrObj *)o)->data, ((StrObj *)o)->len);
                    break;
                case O_LIST: format_list(b, (ListObj *)o, depth); break;
                case O_MAP:  format_map(b, (MapObj *)o,  depth); break;
                case O_FN: {
                    FnObj *f = (FnObj *)o;
                    if (f->is_builtin) { sb_puts(b, "<builtin "); sb_puts(b, f->cname); sb_putc(b, '>'); }
                    else { sb_puts(b, "<fn "); sb_puts(b, f->name ? f->name->data : "anon"); sb_putc(b, '>'); }
                } break;
                case O_ENV: sb_puts(b, "<env>"); break;
            }
        } break;
    }
}

void value_format(StrBuf *b, Value v, int repr) {
    /* cap nesting depth so cyclic containers print as "[...]" / "{...}"
     * instead of blowing the c stack. 64 levels is deep enough to handle
     * every realistic script while terminating self-referential shapes. */
    value_format_depth(b, v, repr, 64);
}

void print_value_to(FILE *fp, Value v, int repr) {
    StrBuf b; sb_init(&b);
    /* sandbox value_format so a die() from sb_need's 2GB cap or an OOM
     * inside xrealloc unwinds through us. without this, b.data (up to
     * ~2GB) leaks on every failed print/write of a huge value (audit
     * F-500). pattern mirrors bi_format. */
    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    volatile int v_failed = 0;
    if (setjmp(g_err_jmp) == 0) {
        value_format(&b, v, repr);
    } else {
        v_failed = 1;
    }
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    if (v_failed) {
        sb_free(&b);
        char saved_msg[sizeof(g_err_msg)];
        memcpy(saved_msg, g_err_msg, sizeof(saved_msg));
        die("%s", saved_msg);
    }
    if (b.len) fwrite(b.data, 1, (size_t)b.len, fp);
    sb_free(&b);
}

int v_equal(Value a, Value b) {
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
        case V_NIL:  return 1;
        case V_BOOL: return a.as.b == b.as.b;
        case V_NUM:  return a.as.n == b.as.n;
        case V_OBJ:
            if (a.as.o == b.as.o) return 1;
            if (a.as.o->tag == O_STR && b.as.o->tag == O_STR) {
                StrObj *x = (StrObj *)a.as.o, *y = (StrObj *)b.as.o;
                return x->len == y->len && memcmp(x->data, y->data, (size_t)x->len) == 0;
            }
            return 0;
    }
    return 0;
}

const char *type_cname(Value v) {
    switch (v.tag) {
        case V_NIL:  return "nil";
        case V_BOOL: return "bool";
        case V_NUM:  return "number";
        case V_OBJ:
            switch (v.as.o->tag) {
                case O_STR:  return "string";
                case O_LIST: return "list";
                case O_MAP:  return "map";
                case O_FN:   return "fn";
                case O_ENV:  return "env";
            }
    }
    return "?";
}

StrObj *value_to_str(Value v) {
    if (v.tag == V_OBJ && v.as.o->tag == O_STR) return (StrObj *)v.as.o;
    StrBuf b; sb_init(&b);
    /* sandbox value_format so a die() from sb_need's 2GB cap or an OOM
     * inside xrealloc unwinds through us. without this, b.data (up to
     * ~2GB) leaks every time str() fails on a huge value (audit F-200).
     * pattern mirrors bi_format. */
    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    volatile int v_failed = 0;
    if (setjmp(g_err_jmp) == 0) {
        value_format(&b, v, 0);
    } else {
        v_failed = 1;
    }
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    if (v_failed) {
        sb_free(&b);
        char saved_msg[sizeof(g_err_msg)];
        memcpy(saved_msg, g_err_msg, sizeof(saved_msg));
        die("%s", saved_msg);
    }
    StrObj *s = str_new(b.data ? b.data : "", b.len);
    sb_free(&b);
    return s;
}
