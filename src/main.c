/*
 * mew entry point: command line handling, script loader, interactive repl.
 * this file owns argc/argv wiring, stdio glue and the main() function.
 */

#include "mew.h"
#include <locale.h>

char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    /* reject files too large to represent as a mew string */
    if ((unsigned long)sz > 2147483000UL) { fclose(f); return NULL; }
    rewind(f);
    char *b = (char *)xmalloc((size_t)sz + 1);
    size_t rd = fread(b, 1, (size_t)sz, f);
    int err = ferror(f);
    fclose(f);
    if (err) { free(b); return NULL; }
    b[rd] = 0;
    return b;
}

static void repl(void) {
    printf("mew %s. type ctrl-d or ctrl-z to exit.\n", MEW_VERSION);
    /* input accumulator on the heap: easier on tiny-stack platforms,
     * and the buffer lives for the whole session anyway. */
    enum { BUF_MAX = 65536 };
    char *buf = (char *)xmalloc(BUF_MAX);
    int buf_len = 0;
    for (;;) {
        printf(buf_len ? ".. " : "> ");
        fflush(stdout);
        char line[4096];
        if (!fgets(line, sizeof(line), stdin)) { printf("\n"); break; }
        int ln = (int)strlen(line);
        if (buf_len + ln + 1 >= BUF_MAX) {
            buf_len = 0;
            printf("mew: input too long, buffer cleared\n");
            continue;
        }
        memcpy(buf + buf_len, line, (size_t)ln);
        buf_len += ln;
        buf[buf_len] = 0;
        if (parse_needs_more(buf)) continue;

        /* run with error recovery */
        jmp_buf prev; int prev_set = g_err_jmp_set;
        memcpy(&prev, &g_err_jmp, sizeof(prev));
        g_err_jmp_set = 1;
        g_src_name = "<repl>";
        if (setjmp(g_err_jmp) == 0) {
            run_source(buf);
        } else {
            fprintf(stderr, "mew: %s\n", g_err_msg);
            /* reset interpreter state: otherwise each error would leak
             * gc roots on the value stack and over time grow the stack
             * to VSTACK_MAX, after which every new statement would fail
             * with "value stack overflow". */
            g_break = 0; g_ret = 0; g_call_depth = 0;
            g_vsp = 0;
            /* release partial ast if parsing was interrupted by a syntax
             * error (reserved slot published by parse_program). */
            if (g_parse_current) { free_node(g_parse_current); g_parse_current = NULL; }
        }
        g_err_jmp_set = prev_set;
        memcpy(&g_err_jmp, &prev, sizeof(prev));
        buf_len = 0;
    }
    free(buf);
}

static void usage_and_exit(int code) {
    fprintf(stderr,
        "mew " MEW_VERSION " - tiny pocket scripting language\n"
        "usage:\n"
        "  mew script.mew [args...]   run a script\n"
        "  mew -e EXPR                execute EXPR\n"
        "  mew -v                     print version\n"
        "  mew -h                     show this help\n"
        "  mew                        start repl\n");
    exit(code);
}

static void push_arg(const char *s) {
    size_t n = strlen(s);
    if (n > 2147483000) { fprintf(stderr, "mew: argument too long\n"); exit(1); }
    list_push(g_args_list, v_obj((Object *)str_new(s, (int)n)));
}

int main(int argc, char **argv) {
    /* force C locale for strtod / printf %g: a host environment with e.g.
     * ru_RU locale would otherwise parse 3,14 instead of 3.14 and break
     * both the lexer and format_num. this keeps mew portable. */
    setlocale(LC_NUMERIC, "C");

    g_globals = env_new(NULL);
    install_builtins(g_globals);
    g_args_list = list_new();
    env_define(g_globals, intern_cstr("__args"), v_obj((Object *)g_args_list));
    seed_rng_from_time();

    /* no arguments: interactive repl */
    if (argc < 2) { repl(); return 0; }

    /* flag handling */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
        usage_and_exit(0);
    if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
        printf("mew %s\n", MEW_VERSION);
        return 0;
    }

    /* -e EXPR [extra args forwarded to args()] */
    if (strcmp(argv[1], "-e") == 0) {
        if (argc < 3) usage_and_exit(2);
        for (int i = 3; i < argc; i++) push_arg(argv[i]);
        g_src_name = "<cmd>";
        run_source(argv[2]);
        return 0;
    }

    /* script file [args forwarded to args()] */
    for (int i = 2; i < argc; i++) push_arg(argv[i]);
    g_src_name = argv[1];
    char *src = slurp(argv[1]);
    if (!src) { fprintf(stderr, "mew: cannot open %s\n", argv[1]); return 1; }
    run_source(src);
    free(src);
    return 0;
}
