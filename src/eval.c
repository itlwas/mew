/*
 * mew evaluator: tree-walking interpreter.
 * exports: eval_node, exec_block, call_value, run_source, parse_needs_more.
 */

#include "mew.h"

static double to_num_op(Value v, const char *op) {
    if (v.tag != V_NUM) die("%s expects number, got %s", op, type_cname(v));
    return v.as.n;
}

static Value eval_binop(int op, Value a, Value b) {
    switch (op) {
        case T_PLUS:   return v_num(to_num_op(a, "+") + to_num_op(b, "+"));
        case T_MINUS:  return v_num(to_num_op(a, "-") - to_num_op(b, "-"));
        case T_STAR:   return v_num(to_num_op(a, "*") * to_num_op(b, "*"));
        case T_SLASH: {
            double bn = to_num_op(b, "/");
            if (bn == 0) die("division by zero");
            return v_num(to_num_op(a, "/") / bn);
        }
        case T_PERCENT: {
            double an = to_num_op(a, "%"), bn = to_num_op(b, "%");
            if (bn == 0) die("modulo by zero");
            return v_num(fmod(an, bn));
        }
        case T_CONCAT: {
            if (a.tag == V_OBJ && b.tag == V_OBJ && a.as.o->tag == O_STR && b.as.o->tag == O_STR)
                return v_obj((Object *)str_concat((StrObj *)a.as.o, (StrObj *)b.as.o));
            if (a.tag == V_OBJ && b.tag == V_OBJ && a.as.o->tag == O_LIST && b.as.o->tag == O_LIST)
                return v_obj((Object *)list_concat((ListObj *)a.as.o, (ListObj *)b.as.o));
            die("'..' expects two strings or two lists, got %s and %s", type_cname(a), type_cname(b));
        }
        case T_EQ: return v_bool( v_equal(a, b));
        case T_NE: return v_bool(!v_equal(a, b));
        case T_LT: case T_GT: case T_LE: case T_GE: {
            if (a.tag == V_NUM && b.tag == V_NUM) {
                double x = a.as.n, y = b.as.n;
                switch (op) {
                    case T_LT: return v_bool(x <  y);
                    case T_GT: return v_bool(x >  y);
                    case T_LE: return v_bool(x <= y);
                    case T_GE: return v_bool(x >= y);
                }
            }
            if (a.tag == V_OBJ && b.tag == V_OBJ && a.as.o->tag == O_STR && b.as.o->tag == O_STR) {
                StrObj *x = (StrObj *)a.as.o, *y = (StrObj *)b.as.o;
                int mn = x->len < y->len ? x->len : y->len;
                int c  = memcmp(x->data, y->data, (size_t)mn);
                if (c == 0) c = x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
                switch (op) {
                    case T_LT: return v_bool(c <  0);
                    case T_GT: return v_bool(c >  0);
                    case T_LE: return v_bool(c <= 0);
                    case T_GE: return v_bool(c >= 0);
                }
            }
            die("comparison expects two numbers or two strings, got %s and %s", type_cname(a), type_cname(b));
        }
    }
    die("bad binop");
    return v_nil();
}

void exec_block(Node *blk, Env *env) {
    for (int i = 0; i < blk->n && !g_break && !g_ret; i++) eval_node(blk->kids[i], env);
}

Value eval_node(Node *n, Env *env) {
    if (!n) return v_nil();
    g_err_line = n->line;
    gc_maybe();
    switch (n->kind) {
        case N_NIL:   return v_nil();
        case N_BOOL:  return v_bool(n->v.b);
        case N_NUM:   return v_num(n->v.n);
        case N_STR:   return v_obj((Object *)n->v.s);
        case N_IDENT: {
            Value out;
            if (!env_lookup(env, n->v.s, &out)) die("undefined variable '%s'", n->v.s->data);
            return out;
        }
        case N_LIST: {
            ListObj *l = list_new();
            vpush(v_obj((Object *)l));
            for (int i = 0; i < n->n; i++) list_push(l, eval_node(n->kids[i], env));
            vpop();
            return v_obj((Object *)l);
        }
        case N_MAP: {
            MapObj *m = map_new();
            vpush(v_obj((Object *)m));
            for (int i = 0; i < n->n; i++) {
                Value v = eval_node(n->kids[i], env);
                vpush(v);
                map_set(m, n->keys[i], v);
                vpop();
            }
            vpop();
            return v_obj((Object *)m);
        }
        case N_FN: {
            FnObj *f = (FnObj *)gc_new(sizeof(FnObj), O_FN);
            f->is_builtin = 0;
            f->nparams    = n->op;
            f->params     = NULL;
            if (f->nparams) {
                f->params = (StrObj **)xmalloc(sizeof(StrObj *) * (size_t)f->nparams);
                memcpy(f->params, n->keys, sizeof(StrObj *) * (size_t)f->nparams);
            }
            f->body    = n;
            f->closure = env;
            f->name    = NULL;
            return v_obj((Object *)f);
        }
        case N_UNOP: {
            Value a = eval_node(n->a, env);
            if (n->op == T_MINUS) {
                if (a.tag != V_NUM) die("unary '-' expects number, got %s", type_cname(a));
                return v_num(-a.as.n);
            }
            if (n->op == T_NOT) return v_bool(!v_truthy(a));
            die("bad unop");
        }
        case N_BINOP: {
            Value a = eval_node(n->a, env);
            vpush(a);
            Value b = eval_node(n->b, env);
            vpush(b);
            Value r = eval_binop(n->op, a, b);
            vpop(); vpop();
            return r;
        }
        case N_LOGIC: {
            Value a = eval_node(n->a, env);
            if (n->op == T_AND) {
                if (!v_truthy(a)) return a;
                return eval_node(n->b, env);
            }
            if (v_truthy(a)) return a;
            return eval_node(n->b, env);
        }
        case N_INDEX: {
            Value coll = eval_node(n->a, env);
            vpush(coll);
            Value idx = eval_node(n->b, env);
            vpush(idx);
            Value result;
            if (coll.tag != V_OBJ) die("cannot index %s", type_cname(coll));
            if (coll.as.o->tag == O_LIST) {
                if (idx.tag != V_NUM) die("list index must be number, got %s", type_cname(idx));
                ListObj *l = (ListObj *)coll.as.o;
                int i = safe_dtoi(idx.as.n, "list index");
                int orig = i;
                if (i < 0) i += l->len;
                if (i < 0 || i >= l->len) die("list index %d out of range (len %d)", orig, l->len);
                result = l->items[i];
            }
            else if (coll.as.o->tag == O_MAP) {
                if (idx.tag != V_OBJ || idx.as.o->tag != O_STR) die("map key must be string, got %s", type_cname(idx));
                result = v_nil();
                map_get((MapObj *)coll.as.o, (StrObj *)idx.as.o, &result);
            }
            else if (coll.as.o->tag == O_STR) {
                if (idx.tag != V_NUM) die("string index must be number, got %s", type_cname(idx));
                StrObj *s = (StrObj *)coll.as.o;
                int i = safe_dtoi(idx.as.n, "string index");
                int orig = i;
                if (i < 0) i += s->len;
                if (i < 0 || i >= s->len) die("string index %d out of range (len %d)", orig, s->len);
                result = v_obj((Object *)str_new(s->data + i, 1));
            }
            else die("cannot index %s", type_cname(coll));
            vpop(); vpop();
            return result;
        }
        case N_DOT: {
            Value coll = eval_node(n->a, env);
            if (coll.tag != V_OBJ || coll.as.o->tag != O_MAP)
                die("'.' requires map, got %s", type_cname(coll));
            Value out;
            if (map_get((MapObj *)coll.as.o, n->v.s, &out)) return out;
            return v_nil();
        }
        case N_CALL: {
            Value fv = eval_node(n->a, env);
            vpush(fv);
            int argc_n = n->n;
            /* evaluate arguments onto the value stack: that keeps them
             * rooted against gc and avoids a separate heap allocation that
             * could leak on an error (longjmp) from call_value. */
            int argv_base = vsave();
            for (int i = 0; i < argc_n; i++) vpush(eval_node(n->kids[i], env));
            Value r = call_value(fv, argc_n, &g_vstack[argv_base]);
            vrestore(argv_base);
            vpop(); /* fv */
            return r;
        }
        case N_ASSIGN: {
            Value v = eval_node(n->a, env);
            env_assign(env, n->v.s, v);
            return v_nil();
        }
        case N_INDEX_SET: {
            Value coll = eval_node(n->a, env);
            vpush(coll);
            Value idx = eval_node(n->b, env);
            vpush(idx);
            Value val = eval_node(n->c, env);
            vpush(val);
            if (coll.tag != V_OBJ) die("cannot index-assign into %s", type_cname(coll));
            if (coll.as.o->tag == O_LIST) {
                if (idx.tag != V_NUM) die("list index must be number");
                ListObj *l = (ListObj *)coll.as.o;
                int i = safe_dtoi(idx.as.n, "list index");
                int orig = i;
                if (i < 0) i += l->len;
                if (i < 0 || i >= l->len) die("list index %d out of range (len %d)", orig, l->len);
                l->items[i] = val;
                vpop(); vpop(); vpop();
                return v_nil();
            }
            if (coll.as.o->tag == O_MAP) {
                if (idx.tag != V_OBJ || idx.as.o->tag != O_STR) die("map key must be string");
                map_set((MapObj *)coll.as.o, (StrObj *)idx.as.o, val);
                vpop(); vpop(); vpop();
                return v_nil();
            }
            die("cannot index-assign into %s", type_cname(coll));
        }
        case N_DOT_SET: {
            Value coll = eval_node(n->a, env);
            vpush(coll);
            Value val = eval_node(n->b, env);
            vpush(val);
            if (coll.tag != V_OBJ || coll.as.o->tag != O_MAP)
                die("'.' assignment requires map, got %s", type_cname(coll));
            map_set((MapObj *)coll.as.o, n->v.s, val);
            vpop(); vpop();
            return v_nil();
        }
        case N_IF: {
            Value c = eval_node(n->a, env);
            if (v_truthy(c)) exec_block(n->b, env);
            else if (n->c)   exec_block(n->c, env);
            return v_nil();
        }
        case N_WHILE: {
            for (;;) {
                Value c = eval_node(n->a, env);
                if (!v_truthy(c)) break;
                exec_block(n->b, env);
                if (g_break) { g_break = 0; break; }
                if (g_ret) break;
            }
            return v_nil();
        }
        case N_FOR_TO: {
            Value fromv = eval_node(n->a, env);
            Value toov  = eval_node(n->b, env);
            if (fromv.tag != V_NUM || toov.tag != V_NUM) die("for-to expects numbers");
            long long from = safe_dtoll(fromv.as.n, "for-to lower bound");
            long long too  = safe_dtoll(toov.as.n,  "for-to upper bound");
            for (long long i = from; i <= too; i++) {
                env_assign(env, n->v.s, v_num((double)i));
                exec_block(n->c, env);
                if (g_break) { g_break = 0; break; }
                if (g_ret) break;
            }
            return v_nil();
        }
        case N_FOR_IN: {
            Value col = eval_node(n->a, env);
            if (col.tag != V_OBJ) die("for-in expects list, map or string, got %s", type_cname(col));
            vpush(col);
            if (col.as.o->tag == O_LIST) {
                ListObj *l = (ListObj *)col.as.o;
                /* snapshot the length before the loop starts, mimicking lua
                 * ipairs semantics: mutating the list in the body does not
                 * change the iteration count. prevents accidental infinite
                 * loops from push inside for-in. */
                int snap_len = l->len;
                for (int i = 0; i < snap_len && i < l->len; i++) {
                    env_assign(env, n->v.s, l->items[i]);
                    exec_block(n->b, env);
                    if (g_break) { g_break = 0; break; }
                    if (g_ret) break;
                }
            } else if (col.as.o->tag == O_MAP) {
                MapObj *m = (MapObj *)col.as.o;
                /* collect keys into a list protected by the value stack so a
                 * die() inside the loop body cannot leak the auxiliary buffer:
                 * the gc will reclaim the list on the next sweep. */
                ListObj *klist = list_new();
                vpush(v_obj((Object *)klist));
                for (int i = 0; i < m->cap; i++)
                    if (m->buckets[i].used) list_push(klist, v_obj((Object *)m->buckets[i].key));
                if (klist->len > 1)
                    qsort(klist->items, (size_t)klist->len, sizeof(Value), cmp_value_str);
                for (int i = 0; i < klist->len; i++) {
                    env_assign(env, n->v.s, klist->items[i]);
                    exec_block(n->b, env);
                    if (g_break) { g_break = 0; break; }
                    if (g_ret) break;
                }
                vpop();
            } else if (col.as.o->tag == O_STR) {
                StrObj *s = (StrObj *)col.as.o;
                for (int i = 0; i < s->len; i++) {
                    env_assign(env, n->v.s, v_obj((Object *)str_new(s->data + i, 1)));
                    exec_block(n->b, env);
                    if (g_break) { g_break = 0; break; }
                    if (g_ret) break;
                }
            } else {
                vpop();
                die("for-in expects list, map or string, got %s", type_cname(col));
            }
            vpop();
            return v_nil();
        }
        case N_FN_DECL: {
            Value fv = eval_node(n->a, env);
            vpush(fv);
            if (fv.tag == V_OBJ && fv.as.o->tag == O_FN) ((FnObj *)fv.as.o)->name = n->v.s;
            env_define(env, n->v.s, fv);
            vpop();
            return v_nil();
        }
        case N_RETURN: {
            g_ret_val = n->a ? eval_node(n->a, env) : v_nil();
            g_ret = 1;
            return v_nil();
        }
        case N_BREAK:     g_break = 1;         return v_nil();
        case N_BLOCK:     exec_block(n, env);  return v_nil();
        case N_EXPR_STMT: eval_node(n->a, env);return v_nil();
    }
    die("bad node kind");
    return v_nil();
}

Value call_value(Value fv, int argc, Value *argv) {
    if (fv.tag != V_OBJ || fv.as.o->tag != O_FN) die("cannot call %s", type_cname(fv));
    FnObj *f = (FnObj *)fv.as.o;
    /* count both builtin and user frames so a mew-visible recursion through
     * a builtin comparator (e.g. sort with user fn) cannot defeat the
     * CALL_DEPTH_MAX guard by alternating native/mew frames. */
    if (g_call_depth >= CALL_DEPTH_MAX) die("call stack overflow (depth %d)", g_call_depth);
    g_call_depth++;
    if (f->is_builtin) {
        Value r = f->cfn(argc, argv);
        g_call_depth--;
        return r;
    }
    if (argc != f->nparams) {
        g_call_depth--;
        die("function '%s' takes %d arg%s, got %d",
            f->name ? f->name->data : "anon",
            f->nparams, f->nparams == 1 ? "" : "s", argc);
    }
    Env *e = env_new(f->closure);
    vpush(v_obj((Object *)e));
    for (int i = 0; i < f->nparams; i++) env_define(e, f->params[i], argv[i]);
    /* save loop/return control state: a callee must not leak a return value
     * or a break flag into the caller's surrounding loop. */
    int   saved_ret   = g_ret;
    Value saved_val   = g_ret_val;
    int   saved_break = g_break;
    /* vpush saved_val so it remains a gc root while the callee may re-set
     * g_ret_val. without this, the previous pending return value would be
     * unreachable during gc that happens inside the callee; after we
     * restore g_ret_val = saved_val, gc could then observe a dangling ptr
     * via mark_value(g_ret_val). */
    vpush(saved_val);
    g_ret   = 0;
    g_break = 0;
    exec_block(f->body, e);
    Value r = g_ret ? g_ret_val : v_nil();
    g_ret     = saved_ret;
    g_ret_val = saved_val;
    g_break   = saved_break;
    vpop(); /* saved_val */
    vpop(); /* env */
    g_call_depth--;
    return r;
}

/* ========================================================================
 * driver: run a source buffer, probe incomplete input for the REPL
 * ====================================================================== */

void run_source(const char *src) {
    Node *prog = parse_program(src);
    ast_keep(prog);
    /* pin while executing: bi_load called from inside this exec_block
     * may sweep the AST chain; without the pin it would consider this
     * top-level program unreachable (no FnObj points into a top-level
     * block that contains no function literals) and free the tree we
     * are still walking. */
    ast_pin_root(prog);
    exec_block(prog, g_globals);
    ast_unpin_root(prog);
    g_break = 0; g_ret = 0; g_call_depth = 0;
    g_vsp = 0; /* normal completion leaves an empty vstack; be explicit */
    /* between top-level evaluations we can safely free AST trees that
     * no live function body references. only safe here, never inside
     * exec_block. closes audit F-101. */
    ast_sweep();
}

int parse_needs_more(const char *src) {
    /* probe: try to parse; if parsing fails because EOF was hit too early
     * (inside a string, after an '=', or before an expected keyword), ask
     * for more input. any other error is a real syntax error.
     *
     * 'more' is volatile because C99 7.13.2.1 requires any automatic that
     * is modified between setjmp and longjmp to be volatile to preserve
     * its value after longjmp. gcc's -Wclobbered catches this. */
    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    g_err_at_eof  = 0;
    volatile int more = 0;
    if (setjmp(g_err_jmp) == 0) {
        Node *n = parse_program(src);
        free_node(n);
    } else {
        more = g_err_at_eof;
        /* clean up partial tree built before the error */
        if (g_parse_current) { free_node(g_parse_current); g_parse_current = NULL; }
    }
    g_err_at_eof  = 0;
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    return more;
}
