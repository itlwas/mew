/*
 * mew parser: lexer, ast node constructors, recursive descent parser.
 * exports: parse_program, free_node, ast_keep.
 */

#include "mew.h"

/* ========================================================================
 * lexer (internal)
 * ====================================================================== */

typedef struct {
    TokKind     kind;
    const char *start;
    int         len;
    int         line;
    double      num;
    StrObj     *str;
} Tok;

typedef struct {
    const char *src;
    const char *p;
    int         line;
    Tok         cur, peek;
    int         has_peek;
} Lexer;

static int is_ident_start(int c) { return isalpha(c) || c == '_'; }
static int is_ident_cont (int c) { return isalnum(c) || c == '_'; }

static const struct { const char *s; TokKind k; } KW[] = {
    {"if", T_IF}, {"then", T_THEN}, {"else", T_ELSE}, {"end", T_END},
    {"while", T_WHILE}, {"do", T_DO}, {"for", T_FOR}, {"to", T_TO}, {"in", T_IN},
    {"fn", T_FN}, {"return", T_RETURN}, {"break", T_BREAK},
    {"and", T_AND}, {"or", T_OR}, {"not", T_NOT},
    {"true", T_TRUE}, {"false", T_FALSE}, {"nil", T_NIL},
    {NULL, 0}
};

static Tok lex_one(Lexer *L) {
    for (;;) {
        while (*L->p && isspace((unsigned char)*L->p)) {
            if (*L->p == '\n') L->line++;
            L->p++;
        }
        if (*L->p == '#') { while (*L->p && *L->p != '\n') L->p++; continue; }
        break;
    }
    Tok t;
    t.kind = T_EOF; t.start = L->p; t.len = 0; t.line = L->line; t.num = 0; t.str = NULL;
    g_err_line = L->line;
    if (!*L->p) return t;

    const char *start = L->p;
    int c = (unsigned char)*L->p;

    if (isdigit(c)) {
        char *end;
        double n = strtod(L->p, &end);
        t.kind = T_NUM; t.num = n; t.start = start;
        t.len = (int)(end - start);
        L->p = end;
        return t;
    }
    if (is_ident_start(c)) {
        const char *s = L->p++;
        while (is_ident_cont((unsigned char)*L->p)) L->p++;
        int len = (int)(L->p - s);
        for (int i = 0; KW[i].s; i++) {
            int kl = (int)strlen(KW[i].s);
            if (kl == len && memcmp(KW[i].s, s, (size_t)len) == 0) {
                t.kind = KW[i].k; t.start = s; t.len = len; return t;
            }
        }
        t.kind = T_IDENT; t.start = s; t.len = len; t.str = intern(s, len);
        return t;
    }
    if (c == '"') {
        L->p++;
        const int MAX_STR = 2147483000;
        int cap = 16, len = 0;
        char *buf = (char *)xmalloc((size_t)cap);
        while (*L->p && *L->p != '"') {
            char ch = *L->p++;
            if (ch == '\\' && *L->p) {
                char e = *L->p++;
                switch (e) {
                    case 'n': ch = '\n'; break;
                    case 't': ch = '\t'; break;
                    case 'r': ch = '\r'; break;
                    case '"': ch = '"';  break;
                    case '\\':ch = '\\'; break;
                    case '0': ch = 0;    break;
                    default:  ch = e;    break;
                }
            } else if (ch == '\n') L->line++;
            if (len + 1 > cap) {
                if (cap > MAX_STR / 2) cap = MAX_STR; else cap *= 2;
                if (len + 1 > cap) { free(buf); die("string literal too long"); }
                buf = (char *)xrealloc(buf, (size_t)cap);
            }
            buf[len++] = ch;
        }
        if (*L->p != '"') { free(buf); g_err_at_eof = 1; die("unterminated string literal"); }
        L->p++;
        t.kind = T_STR;
        /* intern string literals: two identical "abc" share the same StrObj,
         * giving O(1) v_equal by pointer and reducing memory usage */
        t.str = intern(buf, len);
        free(buf);
        return t;
    }

    L->p++;
    switch (c) {
        case '(': t.kind = T_LP; break;
        case ')': t.kind = T_RP; break;
        case '[': t.kind = T_LB; break;
        case ']': t.kind = T_RB; break;
        case '{': t.kind = T_LC; break;
        case '}': t.kind = T_RC; break;
        case ',': t.kind = T_COMMA; break;
        case ':': t.kind = T_COLON; break;
        case '+': t.kind = T_PLUS; break;
        case '-': t.kind = T_MINUS; break;
        case '*': t.kind = T_STAR; break;
        case '/': t.kind = T_SLASH; break;
        case '%': t.kind = T_PERCENT; break;
        case '.': if (*L->p == '.') { L->p++; t.kind = T_CONCAT; } else t.kind = T_DOT;    break;
        case '=': if (*L->p == '=') { L->p++; t.kind = T_EQ;     } else t.kind = T_ASSIGN; break;
        case '!': if (*L->p == '=') { L->p++; t.kind = T_NE;     } else die("unexpected '!'"); break;
        case '<': if (*L->p == '=') { L->p++; t.kind = T_LE;     } else t.kind = T_LT;     break;
        case '>': if (*L->p == '=') { L->p++; t.kind = T_GE;     } else t.kind = T_GT;     break;
        default:  die("unexpected character '%c' (0x%02x)", c, c);
    }
    t.len = (int)(L->p - start);
    return t;
}

static void lex_init(Lexer *L, const char *src) {
    L->src = src; L->p = src; L->line = 1; L->has_peek = 0;
    L->cur = lex_one(L);
}
static Tok lex_advance(Lexer *L) {
    Tok t = L->cur;
    if (L->has_peek) { L->cur = L->peek; L->has_peek = 0; }
    else L->cur = lex_one(L);
    return t;
}
static Tok *lex_peek_tok(Lexer *L) {
    if (!L->has_peek) { L->peek = lex_one(L); L->has_peek = 1; }
    return &L->peek;
}
static int  lex_check  (Lexer *L, TokKind k) { return L->cur.kind == k; }
static int  lex_match  (Lexer *L, TokKind k) { if (L->cur.kind == k) { lex_advance(L); return 1; } return 0; }
static void lex_expect (Lexer *L, TokKind k, const char *what) {
    if (L->cur.kind != k) { if (L->cur.kind == T_EOF) g_err_at_eof = 1; die("expected %s", what); }
    lex_advance(L);
}

/* ========================================================================
 * AST constructors
 * ====================================================================== */

static Node *node_new(NodeKind k, int line) {
    Node *n = (Node *)xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = k; n->line = line;
    return n;
}
static void node_add_kid(Node *n, Node *k) {
    if (n->n >= 2147483000) die("parse: too many child nodes");
    n->kids = (Node **)xrealloc(n->kids, sizeof(Node *) * (size_t)(n->n + 1));
    n->kids[n->n++] = k;
}
static void node_add_key(Node *n, StrObj *s) {
    n->keys = (StrObj **)xrealloc(n->keys, sizeof(StrObj *) * (size_t)n->n);
    n->keys[n->n - 1] = s;
}

/* iterative free_node: the AST for 'a + b + c + ...' forms a deeply
 * left-leaning chain through ->a, so a naive recursive free overflows
 * the C stack on adversarial input. we walk the tree using the same
 * work-stack trick as mark_ast, freeing children first via a two-pass
 * enqueue + collect-for-free pattern. */
void free_node(Node *root) {
    if (!root) return;
    int stack_cap = 64, stack_top = 0;
    Node **stack = (Node **)xmalloc(sizeof(Node *) * (size_t)stack_cap);
    int all_cap = 64, all_top = 0;
    Node **all = (Node **)xmalloc(sizeof(Node *) * (size_t)all_cap);

    stack[stack_top++] = root;
    while (stack_top > 0) {
        Node *n = stack[--stack_top];
        if (!n) continue;
        if (all_top >= all_cap) {
            all_cap *= 2;
            all = (Node **)xrealloc(all, sizeof(Node *) * (size_t)all_cap);
        }
        all[all_top++] = n;

        /* push child pointers for later traversal */
        Node *kids[3] = { n->a, n->b, n->c };
        for (int i = 0; i < 3; i++) {
            if (!kids[i]) continue;
            if (stack_top >= stack_cap) {
                stack_cap *= 2;
                stack = (Node **)xrealloc(stack, sizeof(Node *) * (size_t)stack_cap);
            }
            stack[stack_top++] = kids[i];
        }
        for (int i = 0; i < n->n; i++) {
            if (!n->kids[i]) continue;
            if (stack_top >= stack_cap) {
                stack_cap *= 2;
                stack = (Node **)xrealloc(stack, sizeof(Node *) * (size_t)stack_cap);
            }
            stack[stack_top++] = n->kids[i];
        }
    }
    /* free the enumerated nodes: each owns its kids/keys arrays, but the
     * nodes those arrays point to are freed separately via this loop. */
    for (int i = 0; i < all_top; i++) {
        Node *n = all[i];
        free(n->kids);
        free(n->keys);
        free(n);
    }
    free(stack);
    free(all);
}

void ast_keep(Node *root) {
    AstChain *c = (AstChain *)xmalloc(sizeof(AstChain));
    c->root = root;
    c->next = g_ast_chain;
    g_ast_chain = c;
}

/* ========================================================================
 * parser (recursive descent)
 * ====================================================================== */

#define PARSE_DEPTH_MAX 256
/* limit on the length of a single left-associative operator chain
 * (a+b+c+..., a.b.c.d..., f(x)(y)(z)...). an otherwise valid but
 * pathological input with tens of thousands of terms would build a
 * linear AST that the tree-walking evaluator then recurses through
 * one node at a time, blowing the C stack. 512 matches CALL_DEPTH_MAX
 * and gives a comfortable margin under a default Windows 1 MB stack
 * (eval frame ~830 bytes -> 425 KB worst case) while still rejecting
 * adversarial input early (audit F-108). */
#define CHAIN_MAX 512
static int g_parse_depth = 0;
/* track nesting depth of enclosing constructs at parse time so we can
 * reject `break` outside any loop and `return` outside any function with
 * a clear syntax error rather than silently swallowing the rest of the
 * containing block at runtime (audit F-205 / F-206). incremented at the
 * start of parse_while/parse_for / parse_fn_body, decremented at exit
 * including via the goto-style die path (which longjmps out, but parse
 * always starts fresh in parse_program where these counters are reset). */
static int g_loop_depth = 0;
static int g_fn_depth   = 0;

static Node *parse_expr(Lexer *L);
static Node *parse_stmt(Lexer *L);

static Node *parse_fn_body(Lexer *L, int line) {
    if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
    /* a function body has its own loop scope: a `break` inside the body
     * cannot escape into the surrounding loop because call_value saves
     * and restores g_break. saving the depth and resetting to 0 makes
     * the parser's break-outside-loop check match this runtime behaviour. */
    int saved_loop_depth = g_loop_depth;
    g_loop_depth = 0;
    g_fn_depth++;
    Node *fn = node_new(N_FN, line);
    lex_expect(L, T_LP, "'(' after fn");
    if (!lex_check(L, T_RP)) {
        for (;;) {
            if (L->cur.kind != T_IDENT) die("expected parameter name");
            StrObj *p = L->cur.str;
            lex_advance(L);
            if (fn->n >= 1024) die("fn: too many parameters (max 1024)");
            /* reject duplicate parameter names: previously fn(a, a) was
             * silently accepted and the env_define on call would just
             * overwrite the first binding, masking a typo. (audit F-106) */
            for (int i = 0; i < fn->n; i++)
                if (fn->keys[i] == p) die("fn: duplicate parameter name '%s'", p->data);
            fn->keys = (StrObj **)xrealloc(fn->keys, sizeof(StrObj *) * (size_t)(fn->n + 1));
            fn->keys[fn->n++] = p;
            if (!lex_match(L, T_COMMA)) break;
        }
    }
    lex_expect(L, T_RP, "')'");
    fn->op = fn->n;
    fn->n  = 0;
    while (!lex_check(L, T_END) && !lex_check(L, T_EOF)) node_add_kid(fn, parse_stmt(L));
    lex_expect(L, T_END, "'end'");
    g_parse_depth--;
    g_fn_depth--;
    g_loop_depth = saved_loop_depth;
    return fn;
}

static Node *parse_primary(Lexer *L) {
    if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
    Tok t = L->cur;
    int line = t.line;
    Node *ret = NULL;
    switch (t.kind) {
        case T_NIL:   lex_advance(L); ret = node_new(N_NIL, line); break;
        case T_TRUE:  lex_advance(L); { Node *n = node_new(N_BOOL, line); n->v.b = 1; ret = n; } break;
        case T_FALSE: lex_advance(L); { Node *n = node_new(N_BOOL, line); n->v.b = 0; ret = n; } break;
        case T_NUM:   lex_advance(L); { Node *n = node_new(N_NUM,  line); n->v.n = t.num; ret = n; } break;
        case T_STR:   lex_advance(L); { Node *n = node_new(N_STR,  line); n->v.s = t.str; ret = n; } break;
        case T_IDENT: lex_advance(L); { Node *n = node_new(N_IDENT,line); n->v.s = t.str; ret = n; } break;
        case T_LP: {
            lex_advance(L);
            Node *e = parse_expr(L);
            lex_expect(L, T_RP, "')'");
            ret = e;
            break;
        }
        case T_LB: {
            lex_advance(L);
            Node *n = node_new(N_LIST, line);
            if (!lex_check(L, T_RB)) {
                node_add_kid(n, parse_expr(L));
                while (lex_match(L, T_COMMA)) {
                    if (lex_check(L, T_RB)) break;
                    node_add_kid(n, parse_expr(L));
                }
            }
            lex_expect(L, T_RB, "']'");
            ret = n;
            break;
        }
        case T_LC: {
            lex_advance(L);
            Node *n = node_new(N_MAP, line);
            if (!lex_check(L, T_RC)) {
                for (;;) {
                    if (L->cur.kind != T_STR) die("map key must be a string literal");
                    StrObj *key = L->cur.str;
                    lex_advance(L);
                    lex_expect(L, T_COLON, "':'");
                    Node *val = parse_expr(L);
                    node_add_kid(n, val);
                    node_add_key(n, key);
                    if (!lex_match(L, T_COMMA)) break;
                    if (lex_check(L, T_RC)) break;
                }
            }
            lex_expect(L, T_RC, "'}'");
            ret = n;
            break;
        }
        case T_FN:
            lex_advance(L);
            ret = parse_fn_body(L, line);
            break;
        default:
            if (t.kind == T_EOF) g_err_at_eof = 1;
            die("unexpected token in expression");
    }
    g_parse_depth--;
    return ret;
}

static Node *parse_postfix(Lexer *L) {
    Node *e = parse_primary(L);
    int chain = 0;
    for (;;) {
        int line = L->cur.line;
        if (lex_match(L, T_LB)) {
            if (++chain > CHAIN_MAX) die("parse: expression chain too long");
            Node *idx = parse_expr(L);
            lex_expect(L, T_RB, "']'");
            Node *n = node_new(N_INDEX, line);
            n->a = e; n->b = idx;
            e = n;
        } else if (lex_match(L, T_DOT)) {
            if (++chain > CHAIN_MAX) die("parse: expression chain too long");
            if (L->cur.kind != T_IDENT) die("expected field name after '.'");
            StrObj *name = L->cur.str;
            lex_advance(L);
            Node *n = node_new(N_DOT, line);
            n->a = e; n->v.s = name;
            e = n;
        } else if (lex_match(L, T_LP)) {
            if (++chain > CHAIN_MAX) die("parse: expression chain too long");
            Node *call = node_new(N_CALL, line);
            call->a = e;
            if (!lex_check(L, T_RP)) {
                node_add_kid(call, parse_expr(L));
                while (lex_match(L, T_COMMA)) node_add_kid(call, parse_expr(L));
            }
            lex_expect(L, T_RP, "')'");
            e = call;
        } else break;
    }
    return e;
}

static Node *parse_unary(Lexer *L) {
    int line = L->cur.line;
    if (lex_match(L, T_MINUS)) {
        if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
        Node *u = parse_unary(L);
        Node *n = node_new(N_UNOP, line);
        n->op = T_MINUS; n->a = u;
        g_parse_depth--;
        return n;
    }
    return parse_postfix(L);
}

/* limit on the length of a single left-associative operator chain
 * (a+b+c+...). an otherwise valid but pathological input like
 * "1+1+1+..." with tens of thousands of terms would build a linear AST
 * that the tree-walking evaluator then recurses through one node at a
 * time, blowing the C stack. 2048 is generous for any real program. */

static Node *parse_mul(Lexer *L) {
    Node *a = parse_unary(L);
    int chain = 0;
    while (lex_check(L, T_STAR) || lex_check(L, T_SLASH) || lex_check(L, T_PERCENT)) {
        if (++chain > CHAIN_MAX) die("parse: expression chain too long");
        int op = L->cur.kind, line = L->cur.line;
        lex_advance(L);
        Node *b = parse_unary(L);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->a = a; n->b = b;
        a = n;
    }
    return a;
}
static Node *parse_add(Lexer *L) {
    Node *a = parse_mul(L);
    int chain = 0;
    while (lex_check(L, T_PLUS) || lex_check(L, T_MINUS) || lex_check(L, T_CONCAT)) {
        if (++chain > CHAIN_MAX) die("parse: expression chain too long");
        int op = L->cur.kind, line = L->cur.line;
        lex_advance(L);
        Node *b = parse_mul(L);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->a = a; n->b = b;
        a = n;
    }
    return a;
}
static Node *parse_cmp(Lexer *L) {
    Node *a = parse_add(L);
    int chain = 0;
    while (lex_check(L, T_EQ) || lex_check(L, T_NE) ||
           lex_check(L, T_LT) || lex_check(L, T_GT) ||
           lex_check(L, T_LE) || lex_check(L, T_GE)) {
        if (++chain > CHAIN_MAX) die("parse: expression chain too long");
        int op = L->cur.kind, line = L->cur.line;
        lex_advance(L);
        Node *b = parse_add(L);
        Node *n = node_new(N_BINOP, line);
        n->op = op; n->a = a; n->b = b;
        a = n;
    }
    return a;
}
static Node *parse_not(Lexer *L) {
    int line = L->cur.line;
    if (lex_match(L, T_NOT)) {
        if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
        Node *u = parse_not(L);
        Node *n = node_new(N_UNOP, line);
        n->op = T_NOT; n->a = u;
        g_parse_depth--;
        return n;
    }
    return parse_cmp(L);
}
static Node *parse_and(Lexer *L) {
    Node *a = parse_not(L);
    int chain = 0;
    while (lex_check(L, T_AND)) {
        if (++chain > CHAIN_MAX) die("parse: expression chain too long");
        int line = L->cur.line;
        lex_advance(L);
        Node *b = parse_not(L);
        Node *n = node_new(N_LOGIC, line);
        n->op = T_AND; n->a = a; n->b = b;
        a = n;
    }
    return a;
}
static Node *parse_or(Lexer *L) {
    Node *a = parse_and(L);
    int chain = 0;
    while (lex_check(L, T_OR)) {
        if (++chain > CHAIN_MAX) die("parse: expression chain too long");
        int line = L->cur.line;
        lex_advance(L);
        Node *b = parse_and(L);
        Node *n = node_new(N_LOGIC, line);
        n->op = T_OR; n->a = a; n->b = b;
        a = n;
    }
    return a;
}
static Node *parse_expr(Lexer *L) { return parse_or(L); }

static Node *parse_if(Lexer *L) {
    if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
    int line = L->cur.line;
    lex_advance(L);
    Node *cond = parse_expr(L);
    lex_expect(L, T_THEN, "'then'");
    Node *then_blk = node_new(N_BLOCK, line);
    while (!lex_check(L, T_ELSE) && !lex_check(L, T_END) && !lex_check(L, T_EOF))
        node_add_kid(then_blk, parse_stmt(L));
    Node *else_blk = NULL;
    if (lex_match(L, T_ELSE)) {
        else_blk = node_new(N_BLOCK, line);
        while (!lex_check(L, T_END) && !lex_check(L, T_EOF))
            node_add_kid(else_blk, parse_stmt(L));
    }
    lex_expect(L, T_END, "'end'");
    Node *n = node_new(N_IF, line);
    n->a = cond; n->b = then_blk; n->c = else_blk;
    g_parse_depth--;
    return n;
}

static Node *parse_while(Lexer *L) {
    if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
    int line = L->cur.line;
    lex_advance(L);
    Node *cond = parse_expr(L);
    lex_expect(L, T_DO, "'do'");
    Node *body = node_new(N_BLOCK, line);
    g_loop_depth++;
    while (!lex_check(L, T_END) && !lex_check(L, T_EOF))
        node_add_kid(body, parse_stmt(L));
    g_loop_depth--;
    lex_expect(L, T_END, "'end'");
    Node *n = node_new(N_WHILE, line);
    n->a = cond; n->b = body;
    g_parse_depth--;
    return n;
}

static Node *parse_for(Lexer *L) {
    if (++g_parse_depth > PARSE_DEPTH_MAX) die("parse: nesting too deep (limit %d)", PARSE_DEPTH_MAX);
    int line = L->cur.line;
    lex_advance(L);
    if (L->cur.kind != T_IDENT) die("expected identifier after 'for'");
    StrObj *name = L->cur.str;
    lex_advance(L);
    if (lex_match(L, T_ASSIGN)) {
        Node *from = parse_expr(L);
        lex_expect(L, T_TO, "'to'");
        Node *too = parse_expr(L);
        lex_expect(L, T_DO, "'do'");
        Node *body = node_new(N_BLOCK, line);
        g_loop_depth++;
        while (!lex_check(L, T_END) && !lex_check(L, T_EOF))
            node_add_kid(body, parse_stmt(L));
        g_loop_depth--;
        lex_expect(L, T_END, "'end'");
        Node *n = node_new(N_FOR_TO, line);
        n->v.s = name; n->a = from; n->b = too; n->c = body;
        g_parse_depth--;
        return n;
    }
    if (lex_match(L, T_IN)) {
        Node *col = parse_expr(L);
        lex_expect(L, T_DO, "'do'");
        Node *body = node_new(N_BLOCK, line);
        g_loop_depth++;
        while (!lex_check(L, T_END) && !lex_check(L, T_EOF))
            node_add_kid(body, parse_stmt(L));
        g_loop_depth--;
        lex_expect(L, T_END, "'end'");
        Node *n = node_new(N_FOR_IN, line);
        n->v.s = name; n->a = col; n->b = body;
        g_parse_depth--;
        return n;
    }
    die("expected '=' or 'in' after for-variable");
    return NULL;
}

static Node *parse_fn_decl(Lexer *L) {
    int line = L->cur.line;
    lex_advance(L);
    if (L->cur.kind == T_IDENT && lex_peek_tok(L)->kind == T_LP) {
        StrObj *name = L->cur.str;
        lex_advance(L);
        Node *fn = parse_fn_body(L, line);
        Node *d  = node_new(N_FN_DECL, line);
        d->v.s = name;
        d->a   = fn;
        return d;
    }
    /* anonymous fn used as expression statement */
    Node *fn = parse_fn_body(L, line);
    Node *es = node_new(N_EXPR_STMT, line);
    es->a = fn;
    return es;
}

static Node *parse_assign_or_expr(Lexer *L) {
    int line = L->cur.line;
    Node *lhs = parse_expr(L);
    if (lex_match(L, T_ASSIGN)) {
        Node *rhs = parse_expr(L);
        if (lhs->kind == N_IDENT) {
            Node *n = node_new(N_ASSIGN, line);
            n->v.s = lhs->v.s; n->a = rhs;
            free(lhs);
            return n;
        }
        if (lhs->kind == N_INDEX) {
            Node *n = node_new(N_INDEX_SET, line);
            n->a = lhs->a; n->b = lhs->b; n->c = rhs;
            free(lhs);
            return n;
        }
        if (lhs->kind == N_DOT) {
            Node *n = node_new(N_DOT_SET, line);
            n->a = lhs->a; n->v.s = lhs->v.s; n->b = rhs;
            free(lhs);
            return n;
        }
        /* invalid target: free both half-built trees before raising so a
         * REPL session that produces many syntax errors does not leak
         * Node memory linearly. (audit F-213). */
        free_node(lhs);
        free_node(rhs);
        die("invalid assignment target");
    }
    Node *es = node_new(N_EXPR_STMT, line);
    es->a = lhs;
    return es;
}

static Node *parse_stmt(Lexer *L) {
    switch (L->cur.kind) {
        case T_IF:     return parse_if(L);
        case T_WHILE:  return parse_while(L);
        case T_FOR:    return parse_for(L);
        case T_FN:     return parse_fn_decl(L);
        case T_RETURN: {
            int line = L->cur.line;
            lex_advance(L);
            /* a top-level `return` (g_fn_depth == 0) used to set g_ret = 1
             * and silently swallow the rest of the program. now it is a
             * parse-time error like in lua and python (audit F-206). */
            if (g_fn_depth == 0) die("'return' outside of a function");
            Node *n = node_new(N_RETURN, line);
            switch (L->cur.kind) {
                case T_END: case T_ELSE: case T_EOF: n->a = NULL; break;
                default: n->a = parse_expr(L); break;
            }
            return n;
        }
        case T_BREAK: {
            int line = L->cur.line;
            lex_advance(L);
            /* `break` outside any while/for used to set g_break = 1 and
             * abandon the rest of the enclosing block silently. now it is
             * a parse-time error so typos are caught up front (audit F-205).
             * the check uses g_loop_depth which parse_fn_body resets to 0
             * inside a function body, so a break inside a closure declared
             * inside a loop still requires its own loop. */
            if (g_loop_depth == 0) die("'break' outside of a loop");
            return node_new(N_BREAK, line);
        }
        default: return parse_assign_or_expr(L);
    }
}

Node *parse_program(const char *src) {
    Lexer L;
    lex_init(&L, src);
    g_parse_depth = 0;
    g_loop_depth  = 0;
    g_fn_depth    = 0;
    /* publish the in-progress root so that a die() anywhere inside the
     * parser can still be cleaned up by the outermost error handler. on
     * success we clear it back to NULL. this avoids using setjmp here,
     * which would otherwise require volatile on several locals and has
     * shown subtle interactions with the optimiser on gcc -O2. */
    g_parse_current = node_new(N_BLOCK, 1);
    while (!lex_check(&L, T_EOF))
        node_add_kid(g_parse_current, parse_stmt(&L));
    Node *block = g_parse_current;
    g_parse_current = NULL;
    return block;
}
