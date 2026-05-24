/*
 * mew standard library.
 * exports: install_builtins and the shared g_args_list used by bi_args.
 */

#include "mew.h"

#define NEED(n)      do { if (argc != (n)) die("expected %d args, got %d", (n), argc); } while (0)
#define NEED_GE(n)   do { if (argc <  (n)) die("expected at least %d args, got %d", (n), argc); } while (0)
#define NEED_LE(a,b) do { if (argc < (a) || argc > (b)) die("expected %d..%d args, got %d", (a), (b), argc); } while (0)

ListObj *g_args_list = NULL;

/* ---------- tiny type check helpers --------------------------------- */

static StrObj  *as_str (Value v, const char *ctx) { if (v.tag != V_OBJ || v.as.o->tag != O_STR ) die("%s: expected string, got %s", ctx, type_cname(v)); return (StrObj  *)v.as.o; }
static ListObj *as_list(Value v, const char *ctx) { if (v.tag != V_OBJ || v.as.o->tag != O_LIST) die("%s: expected list, got %s",   ctx, type_cname(v)); return (ListObj *)v.as.o; }
static MapObj  *as_map (Value v, const char *ctx) { if (v.tag != V_OBJ || v.as.o->tag != O_MAP ) die("%s: expected map, got %s",    ctx, type_cname(v)); return (MapObj  *)v.as.o; }
static double   as_num (Value v, const char *ctx) { if (v.tag != V_NUM)                         die("%s: expected number, got %s", ctx, type_cname(v)); return v.as.n; }

/* ---------- io ------------------------------------------------------- */

static Value bi_print(int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        print_value_to(stdout, argv[i], 0);
    }
    fputc('\n', stdout);
    return v_nil();
}
static Value bi_write(int argc, Value *argv) {
    for (int i = 0; i < argc; i++) print_value_to(stdout, argv[i], 0);
    return v_nil();
}
static Value bi_repr(int argc, Value *argv) {
    NEED(1);
    StrBuf b; sb_init(&b);
    /* sandbox value_format so a die() from sb_need's 2GB cap or an OOM
     * inside xrealloc unwinds through us. without this, b.data (up to
     * ~2GB) leaks every time repr() fails (audit F-181). pattern mirrors
     * bi_format. */
    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    volatile int v_failed = 0;
    if (setjmp(g_err_jmp) == 0) {
        value_format(&b, argv[0], 1);
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
    return v_obj((Object *)s);
}
static Value bi_read(int argc, Value *argv) {
    (void)argc; (void)argv;
    const int MAX_LEN = 2147483000;
    int cap = 64, len = 0;
    char *b = (char *)xmalloc((size_t)cap);
    int c;
    while ((c = getchar()) != EOF && c != '\n') {
        if (c == '\r') continue; /* strip CR so Windows CRLF stdin behaves like LF */
        if (len + 1 > cap) {
            if (cap > MAX_LEN / 2) cap = MAX_LEN; else cap *= 2;
            if (len + 1 > cap) { free(b); die("read: line too long"); }
            b = (char *)xrealloc(b, (size_t)cap);
        }
        b[len++] = (char)c;
    }
    if (c == EOF && len == 0) { free(b); return v_nil(); }
    StrObj *s = str_new(b, len);
    free(b);
    return v_obj((Object *)s);
}

/* ---------- collections and introspection --------------------------- */

static Value bi_len(int argc, Value *argv) {
    NEED(1);
    Value v = argv[0];
    if (v.tag == V_OBJ) {
        if (v.as.o->tag == O_STR)  return v_num(((StrObj  *)v.as.o)->len);
        if (v.as.o->tag == O_LIST) return v_num(((ListObj *)v.as.o)->len);
        if (v.as.o->tag == O_MAP)  return v_num(((MapObj  *)v.as.o)->len);
    }
    die("len: expected string, list or map, got %s", type_cname(v));
    return v_nil();
}

static StrObj *type_name_str(Value v) {
    int ix = -1;
    switch (v.tag) {
        case V_NIL:  ix = 0; break;
        case V_BOOL: ix = 1; break;
        case V_NUM:  ix = 2; break;
        case V_OBJ:
            switch (v.as.o->tag) {
                case O_STR:  ix = 3; break;
                case O_LIST: ix = 4; break;
                case O_MAP:  ix = 5; break;
                case O_FN:   ix = 6; break;
                case O_ENV:  ix = 7; break;
            }
            break;
    }
    if (ix < 0) return intern_cstr("?");
    if (!g_type_names[ix]) g_type_names[ix] = intern_cstr(type_cname(v));
    return g_type_names[ix];
}
static Value bi_type(int argc, Value *argv) { NEED(1); return v_obj((Object *)type_name_str(argv[0])); }

/* ---------- conversions --------------------------------------------- */

static Value bi_str(int argc, Value *argv) { NEED(1); return v_obj((Object *)value_to_str(argv[0])); }
static Value bi_num(int argc, Value *argv) {
    NEED(1);
    if (argv[0].tag == V_NUM)  return argv[0];
    if (argv[0].tag == V_BOOL) return v_num(argv[0].as.b ? 1 : 0);
    if (argv[0].tag == V_NIL)  return v_nil();
    if (argv[0].tag == V_OBJ && argv[0].as.o->tag == O_STR) {
        StrObj *s = (StrObj *)argv[0].as.o;
        if (s->len == 0) return v_nil();
        const char *p = s->data;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) return v_nil();
        char *end;
        double d = strtod(p, &end);
        if (end == p) return v_nil();
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end) return v_nil();
        return v_num(d);
    }
    return v_nil();
}

/* ---------- strings ------------------------------------------------- */

static Value bi_upper(int argc, Value *argv) {
    NEED(1);
    StrObj *s = as_str(argv[0], "upper");
    StrObj *r = str_new(s->data, s->len);
    for (int i = 0; i < r->len; i++) r->data[i] = (char)toupper((unsigned char)r->data[i]);
    r->hash = str_hash(r->data, r->len);
    return v_obj((Object *)r);
}
static Value bi_lower(int argc, Value *argv) {
    NEED(1);
    StrObj *s = as_str(argv[0], "lower");
    StrObj *r = str_new(s->data, s->len);
    for (int i = 0; i < r->len; i++) r->data[i] = (char)tolower((unsigned char)r->data[i]);
    r->hash = str_hash(r->data, r->len);
    return v_obj((Object *)r);
}
static Value bi_trim(int argc, Value *argv) {
    NEED(1);
    StrObj *s = as_str(argv[0], "trim");
    int i = 0, j = s->len;
    while (i < j && isspace((unsigned char)s->data[i])) i++;
    while (j > i && isspace((unsigned char)s->data[j - 1])) j--;
    return v_obj((Object *)str_new(s->data + i, j - i));
}
static Value bi_repeat(int argc, Value *argv) {
    NEED(2);
    StrObj *s = as_str(argv[0], "repeat");
    double dn = as_num(argv[1], "repeat");
    if (isnan(dn) || isinf(dn)) die("repeat: count must be a finite number");
    if (dn < 0) dn = 0;
    if (dn > 1e9) die("repeat: count too large");
    int n = (int)dn;
    /* empty string repeat is trivially empty regardless of count */
    if (s->len == 0 || n == 0) return v_obj((Object *)str_new("", 0));
    /* overflow guard: n * s->len must fit in int and size_t */
    if (n > (int)(2147483000 / s->len)) die("repeat: result would exceed string size limit");
    size_t total = (size_t)s->len * (size_t)n;
    char *buf = (char *)xmalloc(total + 1);
    for (int i = 0; i < n; i++) memcpy(buf + (size_t)i * (size_t)s->len, s->data, (size_t)s->len);
    StrObj *r = str_new(buf, (int)total);
    free(buf);
    return v_obj((Object *)r);
}
static Value bi_starts_with(int argc, Value *argv) {
    NEED(2);
    StrObj *s = as_str(argv[0], "starts_with");
    StrObj *p = as_str(argv[1], "starts_with");
    if (p->len > s->len) return v_bool(0);
    return v_bool(memcmp(s->data, p->data, (size_t)p->len) == 0);
}
static Value bi_ends_with(int argc, Value *argv) {
    NEED(2);
    StrObj *s = as_str(argv[0], "ends_with");
    StrObj *p = as_str(argv[1], "ends_with");
    if (p->len > s->len) return v_bool(0);
    return v_bool(memcmp(s->data + s->len - p->len, p->data, (size_t)p->len) == 0);
}
static Value bi_contains(int argc, Value *argv) {
    NEED(2);
    Value a = argv[0], b = argv[1];
    if (a.tag == V_OBJ && a.as.o->tag == O_STR && b.tag == V_OBJ && b.as.o->tag == O_STR) {
        StrObj *s = (StrObj *)a.as.o, *p = (StrObj *)b.as.o;
        if (p->len == 0) return v_bool(1);
        if (p->len > s->len) return v_bool(0);
        for (int i = 0; i <= s->len - p->len; i++)
            if (memcmp(s->data + i, p->data, (size_t)p->len) == 0) return v_bool(1);
        return v_bool(0);
    }
    if (a.tag == V_OBJ && a.as.o->tag == O_LIST) {
        ListObj *l = (ListObj *)a.as.o;
        for (int i = 0; i < l->len; i++) if (v_equal(l->items[i], b)) return v_bool(1);
        return v_bool(0);
    }
    if (a.tag == V_OBJ && a.as.o->tag == O_MAP) {
        MapObj *m = (MapObj *)a.as.o;
        StrObj *k = as_str(b, "contains");
        Value tmp;
        return v_bool(map_get(m, k, &tmp));
    }
    die("contains: expected string, list or map as first arg, got %s", type_cname(a));
    return v_nil();
}
static Value bi_split(int argc, Value *argv) {
    NEED(2);
    StrObj *s   = as_str(argv[0], "split");
    StrObj *sep = as_str(argv[1], "split");
    ListObj *l  = list_new();
    vpush(v_obj((Object *)l));
    if (sep->len == 0) {
        for (int i = 0; i < s->len; i++) list_push(l, v_obj((Object *)str_new(s->data + i, 1)));
    } else {
        int i = 0;
        while (i <= s->len) {
            int j = i;
            while (j + sep->len <= s->len && memcmp(s->data + j, sep->data, (size_t)sep->len) != 0) j++;
            if (j + sep->len > s->len) {
                list_push(l, v_obj((Object *)str_new(s->data + i, s->len - i)));
                break;
            }
            list_push(l, v_obj((Object *)str_new(s->data + i, j - i)));
            i = j + sep->len;
            if (i == s->len) { list_push(l, v_obj((Object *)str_new("", 0))); break; }
        }
    }
    vpop();
    return v_obj((Object *)l);
}
static Value bi_join(int argc, Value *argv) {
    NEED(2);
    ListObj *l   = as_list(argv[0], "join");
    StrObj  *sep = as_str(argv[1], "join");
    /* compute the total in size_t with explicit overflow checks so that
     * a maliciously crafted list cannot wrap an int and trick xmalloc
     * into allocating too little, which would turn the memcpy loop into
     * a heap buffer overflow. */
    size_t total = 0;
    const size_t LIMIT = (size_t)2147483000;
    for (int i = 0; i < l->len; i++) {
        StrObj *it = as_str(l->items[i], "join element");
        if ((size_t)it->len > LIMIT - total) die("join: result too large");
        total += (size_t)it->len;
        if (i) {
            if ((size_t)sep->len > LIMIT - total) die("join: result too large");
            total += (size_t)sep->len;
        }
    }
    char *b = (char *)xmalloc(total + 1);
    size_t off = 0;
    for (int i = 0; i < l->len; i++) {
        if (i) { memcpy(b + off, sep->data, (size_t)sep->len); off += (size_t)sep->len; }
        StrObj *it = (StrObj *)l->items[i].as.o;
        memcpy(b + off, it->data, (size_t)it->len);
        off += (size_t)it->len;
    }
    StrObj *r = str_new(b, (int)total);
    free(b);
    return v_obj((Object *)r);
}
static Value bi_find(int argc, Value *argv) {
    NEED(2);
    StrObj *s   = as_str(argv[0], "find");
    StrObj *sub = as_str(argv[1], "find");
    if (sub->len == 0) return v_num(0);
    if (sub->len > s->len) return v_num(-1);
    for (int i = 0; i <= s->len - sub->len; i++)
        if (memcmp(s->data + i, sub->data, (size_t)sub->len) == 0) return v_num(i);
    return v_num(-1);
}
static Value bi_slice(int argc, Value *argv) {
    NEED(3);
    double di = as_num(argv[1], "slice"), dj = as_num(argv[2], "slice");
    int i = safe_dtoi(di, "slice start");
    int j = safe_dtoi(dj, "slice end");
    Value c = argv[0];
    if (c.tag != V_OBJ) die("slice: expected string or list");
    if (c.as.o->tag == O_STR) {
        StrObj *s = (StrObj *)c.as.o;
        if (i < 0) i += s->len;
        if (j < 0) j += s->len;
        if (i < 0) i = 0;
        if (j > s->len) j = s->len;
        if (j < i) j = i;
        return v_obj((Object *)str_new(s->data + i, j - i));
    }
    if (c.as.o->tag == O_LIST) return v_obj((Object *)list_slice((ListObj *)c.as.o, i, j));
    die("slice: expected string or list, got %s", type_cname(c));
    return v_nil();
}
static Value bi_replace(int argc, Value *argv) {
    NEED(3);
    StrObj *s = as_str(argv[0], "replace");
    StrObj *a = as_str(argv[1], "replace");
    StrObj *b = as_str(argv[2], "replace");
    if (a->len == 0) return v_obj((Object *)s);
    /* grow cap by doubling with saturation so overflow in 'cap *= 2' cannot
     * stall the loop or produce a short buffer that memcpy then overflows. */
    const int MAX_LEN = 2147483000;
    int cap = s->len + 16;
    if (cap < 16) cap = 16;
    if (cap > MAX_LEN) cap = MAX_LEN;
    int len = 0;
    char *buf = (char *)xmalloc((size_t)cap);
    int i = 0;
    while (i < s->len) {
        if (i + a->len <= s->len && memcmp(s->data + i, a->data, (size_t)a->len) == 0) {
            if (b->len > MAX_LEN - len) { free(buf); die("replace: result too large"); }
            while (len + b->len > cap) {
                if (cap > MAX_LEN / 2) cap = MAX_LEN; else cap *= 2;
                if (len + b->len > cap) { free(buf); die("replace: result too large"); }
                buf = (char *)xrealloc(buf, (size_t)cap);
            }
            memcpy(buf + len, b->data, (size_t)b->len);
            len += b->len;
            i   += a->len;
        } else {
            if (len + 1 > cap) {
                if (cap > MAX_LEN / 2) cap = MAX_LEN; else cap *= 2;
                if (len + 1 > cap) { free(buf); die("replace: result too large"); }
                buf = (char *)xrealloc(buf, (size_t)cap);
            }
            buf[len++] = s->data[i++];
        }
    }
    StrObj *r = str_new(buf, len);
    free(buf);
    return v_obj((Object *)r);
}

/* ---------- lists --------------------------------------------------- */

static Value bi_push(int argc, Value *argv) { NEED(2); list_push(as_list(argv[0], "push"), argv[1]); return v_nil(); }
static Value bi_pop (int argc, Value *argv) { NEED(1); return list_pop(as_list(argv[0], "pop")); }
static Value bi_append(int argc, Value *argv) {
    NEED(2);
    Value a = argv[0], b = argv[1];
    if (a.tag == V_OBJ && b.tag == V_OBJ) {
        if (a.as.o->tag == O_STR  && b.as.o->tag == O_STR ) return v_obj((Object *)str_concat((StrObj  *)a.as.o, (StrObj  *)b.as.o));
        if (a.as.o->tag == O_LIST && b.as.o->tag == O_LIST) return v_obj((Object *)list_concat((ListObj *)a.as.o, (ListObj *)b.as.o));
    }
    die("append expects two strings or two lists");
    return v_nil();
}
static Value bi_reverse(int argc, Value *argv) {
    NEED(1);
    Value v = argv[0];
    if (v.tag == V_OBJ && v.as.o->tag == O_LIST) {
        ListObj *l = (ListObj *)v.as.o;
        ListObj *r = list_new();
        vpush(v_obj((Object *)r));
        for (int i = l->len - 1; i >= 0; i--) list_push(r, l->items[i]);
        vpop();
        return v_obj((Object *)r);
    }
    if (v.tag == V_OBJ && v.as.o->tag == O_STR) {
        StrObj *s = (StrObj *)v.as.o;
        StrObj *r = str_new(s->data, s->len);
        for (int i = 0, j = r->len - 1; i < j; i++, j--) {
            char t = r->data[i]; r->data[i] = r->data[j]; r->data[j] = t;
        }
        r->hash = str_hash(r->data, r->len);
        return v_obj((Object *)r);
    }
    die("reverse: expected string or list, got %s", type_cname(v));
    return v_nil();
}

/* stable mergesort using a user fn(a,b)->number or default comparison.
 * the auxiliary buffer lives on the value stack as a transient ListObj so
 * that a die() inside the user comparator does not leak it: the gc will
 * reclaim the list on the next sweep. */
static ListObj *g_sort_tmp = NULL;
static FnObj   *g_sort_fn  = NULL;

/* clear any sort comparator state. called from the REPL error handler
 * after a longjmp out of bi_sort so a stale FnObj pointer cannot linger
 * in g_sort_fn between sessions (audit F-430). bi_sort itself overrides
 * these on every entry, so the state never produces a UAF, but a clean
 * reset keeps the architecture defensive. */
void mew_sort_state_reset(void) { g_sort_fn = NULL; g_sort_tmp = NULL; }

static int sort_compare(Value a, Value b) {
    if (g_sort_fn) {
        Value args[2] = {a, b};
        Value r = call_value(v_obj((Object *)g_sort_fn), 2, args);
        if (r.tag != V_NUM) die("sort comparator must return number");
        double d = r.as.n;
        return d < 0 ? -1 : (d > 0 ? 1 : 0);
    }
    if (a.tag == V_NUM && b.tag == V_NUM) return a.as.n < b.as.n ? -1 : (a.as.n > b.as.n ? 1 : 0);
    if (a.tag == V_OBJ && b.tag == V_OBJ && a.as.o->tag == O_STR && b.as.o->tag == O_STR) {
        StrObj *x = (StrObj *)a.as.o, *y = (StrObj *)b.as.o;
        int mn = x->len < y->len ? x->len : y->len;
        int c  = memcmp(x->data, y->data, (size_t)mn);
        if (c) return c < 0 ? -1 : 1;
        return x->len < y->len ? -1 : (x->len > y->len ? 1 : 0);
    }
    die("sort: cannot compare %s with %s", type_cname(a), type_cname(b));
    return 0;
}
static void msort(Value *a, int lo, int hi) {
    if (hi - lo <= 1) return;
    int mid = (lo + hi) / 2;
    msort(a, lo, mid);
    msort(a, mid, hi);
    int i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        if (sort_compare(a[i], a[j]) <= 0) g_sort_tmp->items[k++] = a[i++];
        else                                g_sort_tmp->items[k++] = a[j++];
    }
    while (i < mid) g_sort_tmp->items[k++] = a[i++];
    while (j < hi ) g_sort_tmp->items[k++] = a[j++];
    for (int t = lo; t < hi; t++) a[t] = g_sort_tmp->items[t];
}
static Value bi_sort(int argc, Value *argv) {
    NEED_LE(1, 2);
    ListObj *l = as_list(argv[0], "sort");
    FnObj   *prev_fn  = g_sort_fn;
    ListObj *prev_tmp = g_sort_tmp;
    g_sort_fn = NULL;
    if (argc == 2) {
        if (argv[1].tag != V_OBJ || argv[1].as.o->tag != O_FN) die("sort: expected fn as comparator");
        g_sort_fn = (FnObj *)argv[1].as.o;
    }
    ListObj *r = list_new();
    vpush(v_obj((Object *)r));
    for (int i = 0; i < l->len; i++) list_push(r, l->items[i]);
    if (r->len > 1) {
        /* preallocate buffer of the same length; it is gc-protected because
         * we push it on the value stack. if the user comparator raises via
         * die(), the buffer is abandoned and the next gc reclaims it. */
        g_sort_tmp = list_new();
        vpush(v_obj((Object *)g_sort_tmp));
        for (int i = 0; i < r->len; i++) list_push(g_sort_tmp, v_nil());
        msort(r->items, 0, r->len);
        vpop();
    }
    g_sort_tmp = prev_tmp;
    g_sort_fn  = prev_fn;
    vpop();
    return v_obj((Object *)r);
}

/* ---------- maps ---------------------------------------------------- */

static Value bi_keys(int argc, Value *argv) {
    NEED(1);
    MapObj *m = as_map(argv[0], "keys");
    ListObj *l = list_new();
    vpush(v_obj((Object *)l));
    /* populate then sort in-place; no bare malloc to leak on die() */
    for (int i = 0; i < m->cap; i++)
        if (m->buckets[i].used) list_push(l, v_obj((Object *)m->buckets[i].key));
    if (l->len > 1)
        qsort(l->items, (size_t)l->len, sizeof(Value), cmp_value_str);
    vpop();
    return v_obj((Object *)l);
}
static Value bi_values(int argc, Value *argv) {
    NEED(1);
    MapObj *m = as_map(argv[0], "values");
    /* first build a sorted key list into a transient vstack slot, then
     * copy values in that key order. avoids the raw malloc that the old
     * version could leak on die(). */
    ListObj *klist = list_new();
    vpush(v_obj((Object *)klist));
    for (int i = 0; i < m->cap; i++)
        if (m->buckets[i].used) list_push(klist, v_obj((Object *)m->buckets[i].key));
    if (klist->len > 1)
        qsort(klist->items, (size_t)klist->len, sizeof(Value), cmp_value_str);
    ListObj *out = list_new();
    vpush(v_obj((Object *)out));
    for (int i = 0; i < klist->len; i++) {
        Value v;
        map_get(m, (StrObj *)klist->items[i].as.o, &v);
        list_push(out, v);
    }
    vpop();
    vpop();
    return v_obj((Object *)out);
}
static Value bi_has(int argc, Value *argv) {
    NEED(2);
    MapObj *m = as_map(argv[0], "has");
    StrObj *k = as_str(argv[1], "has");
    Value tmp;
    return v_bool(map_get(m, k, &tmp));
}
static Value bi_del(int argc, Value *argv) {
    NEED(2);
    MapObj *m = as_map(argv[0], "del");
    StrObj *k = as_str(argv[1], "del");
    map_del(m, k);
    return v_nil();
}

/* ---------- files --------------------------------------------------- */

static Value bi_read_file(int argc, Value *argv) {
    NEED(1);
    StrObj *p = as_str(argv[0], "read_file");
    FILE *f = fopen(p->data, "rb");
    if (!f) return v_nil();
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return v_nil(); }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return v_nil(); }
    if ((unsigned long)sz > 2147483000UL) { fclose(f); return v_nil(); }
    rewind(f);
    char *b = (char *)xmalloc((size_t)sz + 1);
    size_t rd = fread(b, 1, (size_t)sz, f);
    int err = ferror(f);
    fclose(f);
    if (err) { free(b); return v_nil(); }
    StrObj *s = str_new(b, (int)rd);
    free(b);
    return v_obj((Object *)s);
}
static Value bi_write_file(int argc, Value *argv) {
    NEED(2);
    StrObj *p = as_str(argv[0], "write_file");
    StrObj *d = as_str(argv[1], "write_file");
    FILE *f = fopen(p->data, "wb");
    if (!f) return v_bool(0);
    size_t wr = fwrite(d->data, 1, (size_t)d->len, f);
    /* check fclose return: it can fail with EIO or ENOSPC when the libc
     * flushes a buffered chunk on close. reporting success in that case
     * would be a silent data-loss bug. */
    int cr = fclose(f);
    return v_bool(wr == (size_t)d->len && cr == 0);
}
static Value bi_lines(int argc, Value *argv) {
    NEED(1);
    StrObj *p = as_str(argv[0], "lines");
    FILE *f = fopen(p->data, "rb");
    if (!f) return v_nil();
    ListObj *l = list_new();
    vpush(v_obj((Object *)l));
    const int MAX_LEN = 2147483000;
    int cap = 64, len = 0;
    /* volatile per C99 7.13.2.1: b is reassigned by xrealloc inside the
     * sandbox and read by free(b) after the longjmp branch, so it must
     * survive the unwind. without this, a future compiler that places b
     * in a callee-saved register would restore the pre-xrealloc pointer
     * after longjmp and free a stale buffer (audit F-301). */
    char * volatile b = (char *)xmalloc((size_t)cap);
    /* local error sandbox so a die() from xrealloc/str_new/list_push (OOM
     * or capacity-overflow paths) unwinds through us: we can then fclose
     * the FILE* and free the line buffer before re-raising. without this,
     * die() longjmps straight to the outer handler and leaks FD+buffer. */
    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    volatile int failed = 0;
    if (setjmp(g_err_jmp) == 0) {
        int c;
        while ((c = fgetc(f)) != EOF) {
            if (c == '\n') { list_push(l, v_obj((Object *)str_new(b, len))); len = 0; }
            else if (c == '\r') {}
            else {
                if (len + 1 > cap) {
                    if (cap > MAX_LEN / 2) cap = MAX_LEN; else cap *= 2;
                    if (len + 1 > cap) die("lines: line too long");
                    b = (char *)xrealloc(b, (size_t)cap);
                }
                b[len++] = (char)c;
            }
        }
        if (len > 0) list_push(l, v_obj((Object *)str_new(b, len)));
    } else {
        failed = 1;
    }
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    free(b);
    fclose(f);
    vpop();
    if (failed) {
        char saved_msg[sizeof(g_err_msg)];
        memcpy(saved_msg, g_err_msg, sizeof(saved_msg));
        die("%s", saved_msg);
    }
    return v_obj((Object *)l);
}

/* ---------- env and os ---------------------------------------------- */

static Value bi_args(int argc, Value *argv) { (void)argc; (void)argv; return v_obj((Object *)g_args_list); }
static Value bi_exit(int argc, Value *argv) {
    int code = 0;
    if (argc >= 1 && argv[0].tag == V_NUM) code = safe_dtoi(argv[0].as.n, "exit");
    exit(code);
    return v_nil();
}
static Value bi_getenv(int argc, Value *argv) {
    NEED(1);
    StrObj *p = as_str(argv[0], "getenv");
    const char *e = getenv(p->data);
    if (!e) return v_nil();
    size_t n = strlen(e);
    if (n > 2147483000) die("getenv: value too long");
    return v_obj((Object *)str_new(e, (int)n));
}
static Value bi_clock(int argc, Value *argv) {
    (void)argc; (void)argv;
    clock_t c = clock();
    return v_num((double)c / (double)CLOCKS_PER_SEC);
}
static Value bi_time (int argc, Value *argv) {
    (void)argc; (void)argv;
    time_t t = time(NULL);
    return v_num((double)t);
}
static Value bi_sleep(int argc, Value *argv) {
    NEED(1);
    double s = as_num(argv[0], "sleep");
    if (isnan(s) || isinf(s)) die("sleep: duration must be finite");
    if (s <= 0) return v_nil();
    if (s > 86400.0) die("sleep: duration exceeds 24h (%.1f sec)", s);
#if defined(_WIN32) || defined(_WIN64)
    Sleep((DWORD)(s * 1000.0));
#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)
    struct timespec ts;
    ts.tv_sec  = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
#else
    clock_t target = clock() + (clock_t)(s * (double)CLOCKS_PER_SEC);
    while (clock() < target) {}
#endif
    return v_nil();
}

/* ---------- math ---------------------------------------------------- */

static Value bi_abs  (int argc, Value *argv) { NEED(1); double x = as_num(argv[0],"abs");   return v_num(x < 0 ? -x : x); }
static Value bi_min  (int argc, Value *argv) { NEED(2); double a = as_num(argv[0],"min"), b = as_num(argv[1],"min"); return v_num(a<b?a:b); }
static Value bi_max  (int argc, Value *argv) { NEED(2); double a = as_num(argv[0],"max"), b = as_num(argv[1],"max"); return v_num(a>b?a:b); }
static Value bi_floor(int argc, Value *argv) { NEED(1); return v_num(floor(as_num(argv[0],"floor"))); }
static Value bi_ceil (int argc, Value *argv) { NEED(1); return v_num(ceil (as_num(argv[0],"ceil"))); }
static Value bi_round(int argc, Value *argv) { NEED(1); double x = as_num(argv[0],"round"); return v_num(x >= 0 ? floor(x + 0.5) : -floor(-x + 0.5)); }
static Value bi_sqrt (int argc, Value *argv) { NEED(1); return v_num(sqrt(as_num(argv[0],"sqrt"))); }
static Value bi_pow  (int argc, Value *argv) { NEED(2); return v_num(pow(as_num(argv[0],"pow"), as_num(argv[1],"pow"))); }

static uint32_t g_rng_state = 0x12345678u;

void seed_rng_from_time(void) {
    uint32_t s = (uint32_t)time(NULL) ^ 0xBADC0FFEu;
    g_rng_state = s ? s : 1u;
}

static Value bi_rand(int argc, Value *argv) {
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 17;
    g_rng_state ^= g_rng_state <<  5;
    if (argc == 0) return v_num((double)g_rng_state / 4294967296.0);
    NEED(1);
    double n = as_num(argv[0], "rand");
    if (isnan(n) || isinf(n)) die("rand: bound must be a finite number");
    if (n <= 0) die("rand: bound must be > 0");
    if (n > 4294967295.0) die("rand: bound exceeds 2^32");
    /* guard fractional positive bounds (0 < n < 1): casting to uint32_t would
     * truncate to 0, then `state % 0` is undefined behaviour (crashes on x86
     * with INT_DIVIDE_BY_ZERO). fail fast with a clear error instead. */
    uint32_t bound = (uint32_t)n;
    if (bound == 0) die("rand: bound must be >= 1, got %g", n);
    return v_num((double)(g_rng_state % bound));
}
static Value bi_seed(int argc, Value *argv) {
    NEED(1);
    double s = as_num(argv[0], "seed");
    if (isnan(s) || isinf(s)) die("seed: value must be finite");
    /* fold 53-bit mantissa into 32-bit state; tolerates negative and
     * fractional inputs without invoking undefined conversion behaviour.
     * for very large |s| the multiply can overflow to inf and fmod would
     * return nan, so we check after the fold. */
    double fs = s < 0 ? -s : s;
    double scaled = fmod(fs * 2654435761.0, 4294967296.0);
    uint32_t u;
    if (isnan(scaled) || isinf(scaled)) {
        /* fall back to xoring the raw bit pattern of s into the state,
         * which is always well defined */
        uint64_t bits;
        memcpy(&bits, &s, sizeof(bits) < sizeof(s) ? sizeof(bits) : sizeof(s));
        u = (uint32_t)(bits ^ (bits >> 32));
    } else {
        u = (uint32_t)scaled;
    }
    g_rng_state = u ? u : 1u;
    return v_nil();
}

/* ---------- bytes and formatting ------------------------------------ */

static Value bi_chr(int argc, Value *argv) {
    NEED(1);
    double d = as_num(argv[0], "chr");
    int c = safe_dtoi(d, "chr");
    if (c < 0 || c > 255) die("chr: byte value out of range 0..255, got %d", c);
    char buf = (char)c;
    return v_obj((Object *)str_new(&buf, 1));
}
static Value bi_ord(int argc, Value *argv) {
    NEED(1);
    StrObj *s = as_str(argv[0], "ord");
    if (s->len == 0) die("ord: empty string");
    return v_num((double)(unsigned char)s->data[0]);
}

/* format: %s, %d, %x, %f with optional width, '-' left align, '0' pad,
 * and %.N precision for %f.
 *
 * the body runs inside a local error sandbox so that any die() raised
 * for a malformed specifier or oversized output unwinds through us. we
 * then sb_free(&out) before re-raising. without this, every format()
 * error leaked the StrBuf payload (audit F-103). */
static Value bi_format(int argc, Value *argv) {
    NEED_GE(1);
    StrObj *fmt = as_str(argv[0], "format");
    StrBuf out; sb_init(&out);
    int ai = 1;
    const char *p = fmt->data;
    const char *e = p + fmt->len;
    /* baseline g_vsp so unwind can release any owned StrObj pushed by
     * the spec='s' branch (audit F-212). */
    int v_base = vsave();

    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    /* volatile required by C99 7.13.2.1: these locals are written between
     * setjmp and longjmp and must survive the unwind. */
    volatile int      v_failed = 0;
    char  * volatile  v_heap   = NULL;
    if (setjmp(g_err_jmp) != 0) {
        v_failed = 1;
        goto unwind;
    }

    while (p < e) {
        if (*p != '%') { sb_putc(&out, *p++); continue; }
        p++;
        if (p < e && *p == '%') { sb_putc(&out, '%'); p++; continue; }
        int left = 0, zero = 0, width = 0, precision = -1;
        while (p < e && (*p == '-' || *p == '0')) { if (*p == '-') left = 1; else zero = 1; p++; }
        while (p < e && *p >= '0' && *p <= '9') {
            if (width >= 100000) die("format: width too large");
            width = width * 10 + (*p - '0'); p++;
        }
        if (p < e && *p == '.') {
            p++; precision = 0;
            while (p < e && *p >= '0' && *p <= '9') {
                if (precision >= 100000) die("format: precision too large");
                precision = precision * 10 + (*p - '0'); p++;
            }
        }
        if (p >= e) die("format: truncated specifier");
        char spec = *p++;
        if (ai >= argc) die("format: not enough arguments for specifier %%%c", spec);
        Value v = argv[ai++];
        /* build the raw text of this specifier into a local buffer.
         * %d and %x always fit in 24 bytes (long long).
         * %f with a user-chosen precision may need more, so we start with
         * a 64-byte stack buffer and fall back to a heap buffer sized
         * precisely from snprintf's return value. */
        char   stack_buf[64];
        char  *heap_buf = NULL;
        const char *src;
        int    slen;
        StrObj *owned   = NULL;
        if (spec == 's') {
            owned = value_to_str(v);
            /* root owned on the value stack so a hypothetical future gc
             * trigger inside sb_putn (e.g. if sb_need ever started calling
             * gc_maybe) cannot reclaim its data buffer mid-copy.
             * vsave/vrestore at function exit rebalances the stack on the
             * normal path; the unwind handler restores g_vsp through the
             * outer error frame's vsave. (audit F-212). */
            vpush(v_obj((Object *)owned));
            src = owned->data;
            slen = owned->len;
        } else if (spec == 'd' || spec == 'x') {
            if (v.tag != V_NUM) die("format: %%%c expects number, got %s", spec, type_cname(v));
            long long n = safe_dtoll(v.as.n, spec == 'd' ? "format %d" : "format %x");
            slen = snprintf(stack_buf, sizeof(stack_buf), spec == 'd' ? "%lld" : "%llx", n);
            if (slen < 0 || slen >= (int)sizeof(stack_buf)) die("format: %%%c output too large", spec);
            src = stack_buf;
        } else if (spec == 'f') {
            if (v.tag != V_NUM) die("format: %%f expects number, got %s", type_cname(v));
            if (precision > 200) die("format: precision too large (max 200)");
            int need;
            if (precision >= 0) need = snprintf(stack_buf, sizeof(stack_buf), "%.*f", precision, v.as.n);
            else                need = snprintf(stack_buf, sizeof(stack_buf), "%g", v.as.n);
            if (need < 0) die("format: %%f encoding failed");
            if (need < (int)sizeof(stack_buf)) { src = stack_buf; slen = need; }
            else {
                /* spill onto the heap for edge cases like %.100f of 1e300 */
                size_t hsz = (size_t)need + 1;
                heap_buf = (char *)xmalloc(hsz);
                v_heap   = heap_buf;
                if (precision >= 0) slen = snprintf(heap_buf, hsz, "%.*f", precision, v.as.n);
                else                slen = snprintf(heap_buf, hsz, "%g", v.as.n);
                if (slen < 0 || slen >= (int)hsz) die("format: %%f encoding failed");
                src = heap_buf;
            }
        } else die("format: unknown specifier '%%%c'", spec);
        int pad = width > slen ? width - slen : 0;
        if (!left) {
            char fill = zero ? '0' : ' ';
            /* bulk-pad: single byte stream via sb_putn for speed */
            while (pad > 0) {
                char chunk[64];
                int n = pad < (int)sizeof(chunk) ? pad : (int)sizeof(chunk);
                memset(chunk, fill, (size_t)n);
                sb_putn(&out, chunk, n);
                pad -= n;
            }
        }
        sb_putn(&out, src, slen);
        if (left) {
            while (pad > 0) {
                char chunk[64];
                int n = pad < (int)sizeof(chunk) ? pad : (int)sizeof(chunk);
                memset(chunk, ' ', (size_t)n);
                sb_putn(&out, chunk, n);
                pad -= n;
            }
        }
        if (heap_buf) free(heap_buf);
        v_heap = NULL;
        if (owned) {
            /* paired with vpush after spec=='s'; release the gc root now
             * that the bytes have been copied into out. */
            vpop();
            owned = NULL;
        }
        /* overall output cap: refuse to produce a >64 MB result no matter
         * how the specifiers combine. prevents memory exhaustion DoS from a
         * malicious script that multiplies many wide specifiers together. */
        if (out.len > (1 << 26)) die("format: output too large (>64 MB)");
    }
    StrObj *r;
    r = str_new(out.data ? out.data : "", out.len);
    sb_free(&out);
    /* defensive: restore the value stack to the level we captured at entry
     * so any future change that adds an unbalanced vpush in the loop body
     * cannot leak gc roots into the caller. on the unwind path we already
     * call vrestore; mirroring it here makes the contract symmetric
     * (audit F-301). */
    vrestore(v_base);
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    return v_obj((Object *)r);

unwind:
    /* error path: the StrBuf and a possibly-live heap scratch buffer
     * would otherwise leak through the longjmp. release both, restore the
     * outer error handler, then re-raise the same message. saved_msg is
     * required because vsnprintf into g_err_msg with g_err_msg as source
     * would alias under C99. */
    if (v_heap) free(v_heap);
    sb_free(&out);
    vrestore(v_base);
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    if (v_failed) {
        char saved_msg[sizeof(g_err_msg)];
        memcpy(saved_msg, g_err_msg, sizeof(saved_msg));
        die("%s", saved_msg);
    }
    return v_nil(); /* unreachable */
}

/* ---------- errors and modules -------------------------------------- */

static Value bi_error(int argc, Value *argv) {
    NEED(1);
    StrObj *s = as_str(argv[0], "error");
    die("%s", s->data);
    return v_nil();
}
static Value bi_assert(int argc, Value *argv) {
    NEED_LE(1, 2);
    if (!v_truthy(argv[0])) {
        if (argc == 2 && argv[1].tag == V_OBJ && argv[1].as.o->tag == O_STR)
            die("assertion failed: %s", ((StrObj *)argv[1].as.o)->data);
        die("assertion failed");
    }
    return v_nil();
}

static Value bi_load(int argc, Value *argv) {
    NEED(1);
    StrObj *p = as_str(argv[0], "load");
    /* volatile on 'src' is mandated by C99 7.13.2.1: we assign NULL to it
     * after setjmp in the success path, so the error path must observe
     * the updated value to avoid double-free. -Wclobbered catches this. */
    char * volatile src = slurp(p->data);
    if (!src) return v_bool(0);
    const char *prev_name = g_src_name;
    int         prev_line = g_err_line;
    g_src_name = p->data;

    /* parse and execute in a local error-handling sandbox so that any
     * partially built AST, the slurp buffer and the caller's source
     * context are cleaned up before a die() re-raises to the outer
     * handler. */
    jmp_buf prev; int prev_set = g_err_jmp_set;
    memcpy(&prev, &g_err_jmp, sizeof(prev));
    g_err_jmp_set = 1;
    /* track the pinned prog across setjmp so the error branch can
     * unpin it. volatile per C99 7.13.2.1 to survive longjmp. */
    Node * volatile pinned_prog = NULL;
    if (setjmp(g_err_jmp) == 0) {
        Node *prog = parse_program((const char *)src);
        ast_keep(prog);
        free((char *)src);
        src = NULL;
        /* pin the loaded program while it executes; ast_sweep below
         * runs while we are still inside the caller's exec_block, so
         * the caller's pin (set by run_source or an outer load) keeps
         * its tree, and our pin keeps ours. */
        ast_pin_root(prog);
        pinned_prog = prog;
        exec_block(prog, g_globals);
        ast_unpin_root(prog);
        pinned_prog = NULL;
        ast_sweep();
        g_err_jmp_set = prev_set;
        memcpy(&g_err_jmp, &prev, sizeof(prev));
        g_src_name = prev_name;
        g_err_line = prev_line;
        return v_bool(1);
    }
    /* error path: release resources that would otherwise leak and re-raise.
     * src is freed only if it still owns the buffer. */
    if (pinned_prog) ast_unpin_root(pinned_prog);
    if (src) free((char *)src);
    if (g_parse_current) { free_node(g_parse_current); g_parse_current = NULL; }
    /* normalise control flow state before re-raising so the outer handler
     * cannot see leaked break/return flags from within a partly executed
     * loaded script (audit F-102). */
    g_break = 0; g_ret = 0;
    g_err_jmp_set = prev_set;
    memcpy(&g_err_jmp, &prev, sizeof(prev));
    g_src_name = prev_name;
    g_err_line = prev_line;
    /* copy the message off g_err_msg before re-raising: die() writes into
     * g_err_msg via vsnprintf, and vsnprintf(dst, n, "%s", dst) has
     * undefined behaviour under C99 when source and destination overlap. */
    char saved_msg[sizeof(g_err_msg)];
    memcpy(saved_msg, g_err_msg, sizeof(saved_msg));
    die("%s", saved_msg);
    return v_nil(); /* unreachable */
}

/* ---------- registration -------------------------------------------- */

static void define_builtin(Env *e, const char *name, BuiltinFn fn) {
    FnObj *f = (FnObj *)gc_new(sizeof(FnObj), O_FN);
    f->is_builtin = 1;
    f->cfn        = fn;
    f->cname      = name;
    env_define(e, intern_cstr(name), v_obj((Object *)f));
}

void install_builtins(Env *e) {
    /* io */
    define_builtin(e, "print",      bi_print);
    define_builtin(e, "write",      bi_write);
    define_builtin(e, "repr",       bi_repr);
    define_builtin(e, "read",       bi_read);
    /* introspection and conversion */
    define_builtin(e, "len",        bi_len);
    define_builtin(e, "type",       bi_type);
    define_builtin(e, "str",        bi_str);
    define_builtin(e, "num",        bi_num);
    /* strings */
    define_builtin(e, "upper",      bi_upper);
    define_builtin(e, "lower",      bi_lower);
    define_builtin(e, "trim",       bi_trim);
    define_builtin(e, "repeat",     bi_repeat);
    define_builtin(e, "starts_with",bi_starts_with);
    define_builtin(e, "ends_with",  bi_ends_with);
    define_builtin(e, "contains",   bi_contains);
    define_builtin(e, "split",      bi_split);
    define_builtin(e, "join",       bi_join);
    define_builtin(e, "find",       bi_find);
    define_builtin(e, "slice",      bi_slice);
    define_builtin(e, "replace",    bi_replace);
    /* lists */
    define_builtin(e, "push",       bi_push);
    define_builtin(e, "pop",        bi_pop);
    define_builtin(e, "append",     bi_append);
    define_builtin(e, "reverse",    bi_reverse);
    define_builtin(e, "sort",       bi_sort);
    /* maps */
    define_builtin(e, "keys",       bi_keys);
    define_builtin(e, "values",     bi_values);
    define_builtin(e, "has",        bi_has);
    define_builtin(e, "del",        bi_del);
    /* files */
    define_builtin(e, "read_file",  bi_read_file);
    define_builtin(e, "write_file", bi_write_file);
    define_builtin(e, "lines",      bi_lines);
    /* environment */
    define_builtin(e, "args",       bi_args);
    define_builtin(e, "exit",       bi_exit);
    define_builtin(e, "getenv",     bi_getenv);
    define_builtin(e, "clock",      bi_clock);
    define_builtin(e, "time",       bi_time);
    define_builtin(e, "sleep",      bi_sleep);
    /* math */
    define_builtin(e, "abs",        bi_abs);
    define_builtin(e, "min",        bi_min);
    define_builtin(e, "max",        bi_max);
    define_builtin(e, "floor",      bi_floor);
    define_builtin(e, "ceil",       bi_ceil);
    define_builtin(e, "round",      bi_round);
    define_builtin(e, "sqrt",       bi_sqrt);
    define_builtin(e, "pow",        bi_pow);
    define_builtin(e, "rand",       bi_rand);
    define_builtin(e, "seed",       bi_seed);
    /* bytes and formatting */
    define_builtin(e, "chr",        bi_chr);
    define_builtin(e, "ord",        bi_ord);
    define_builtin(e, "format",     bi_format);
    /* error handling */
    define_builtin(e, "error",      bi_error);
    define_builtin(e, "assert",     bi_assert);
    /* modules */
    define_builtin(e, "load",       bi_load);
}
