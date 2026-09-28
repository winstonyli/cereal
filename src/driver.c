/* driver.c - option parsing and translation-unit setup. */
#include "driver.h"

#include <string.h>

extern const char *const host_include_dirs[];
extern const char *const host_attrs[];
extern const char *const host_builtins[];
extern const char host_predefs[];
extern const char *const host_assertions[];

void options_init(Options *o)
{
    memset(o, 0, sizeof *o);
    o->pp.gnu_extensions = true;
    o->pp.lex.dollar_idents = true;
    o->linemarkers = true;
    o->parallel = 'a';
}

static const char *arg_value(int argc, char **argv, int *i, const char *flag)
{
    size_t n = strlen(flag);
    if (argv[*i][n])
        return argv[*i] + n;
    if (*i + 1 >= argc)
        fatal("missing argument to '%s'", flag);
    return argv[++*i];
}

int options_parse_one(Options *o, int argc, char **argv, int i)
{
    const char *a = argv[i];
    int start = i;
    CmdlineMacro cm;
    if (!strncmp(a, "-I", 2)) {
        vec_push(&o->pp.angle_dirs, arg_value(argc, argv, &i, "-I"));
    } else if (!strncmp(a, "-iquote", 7)) {
        vec_push(&o->pp.quote_dirs, arg_value(argc, argv, &i, "-iquote"));
    } else if (!strncmp(a, "-isystem", 8)) {
        vec_push(&o->pp.system_dirs, arg_value(argc, argv, &i, "-isystem"));
    } else if (!strncmp(a, "-D", 2)) {
        cm.kind = 'D';
        cm.text = arg_value(argc, argv, &i, "-D");
        vec_push(&o->macros, cm);
    } else if (!strncmp(a, "-U", 2)) {
        cm.kind = 'U';
        cm.text = arg_value(argc, argv, &i, "-U");
        vec_push(&o->macros, cm);
    } else if (!strcmp(a, "-include")) {
        cm.kind = 'i';
        cm.text = arg_value(argc, argv, &i, "-include");
        vec_push(&o->macros, cm);
    } else if (!strcmp(a, "-nostdinc")) {
        o->pp.nostdinc = true;
    } else if (!strcmp(a, "-undef")) {
        o->pp.no_predefs = true;
    } else if (!strcmp(a, "-std=c99") || !strcmp(a, "-std=iso9899:1999")) {
        /* default */
    } else if (!strcmp(a, "-std=gnu99")) {
        o->pp.gnu_extensions = true;
        o->pp.gnu_mode = true;
    } else if (!strncmp(a, "-std=", 5)) {
        fatal("only C99 is supported (got '%s')", a);
    } else if (!strcmp(a, "-pedantic") || !strcmp(a, "-Wpedantic")) {
        o->pp.pedantic = true;
        vec_push(&o->wflags, "pedantic");
    } else if (!strcmp(a, "-pedantic-errors")) {
        o->pp.pedantic = true;
        o->pedantic_errors = true;
        vec_push(&o->wflags, "pedantic");
    } else if (!strcmp(a, "-trigraphs")) {
        o->pp.lex.trigraphs = true;
    } else if (!strncmp(a, "-W", 2) && a[2]) {
        vec_push(&o->wflags, a + 2);
    } else if (!strcmp(a, "-fdiagnostics-format=json")) {
        o->json = true;
    } else if (!strcmp(a, "-fcolor-diagnostics") ||
               !strcmp(a, "-fdiagnostics-color")) {
        o->color = true;
    } else if (!strcmp(a, "-fsystem-warnings")) {
        o->show_system = true;
    } else if (a[1] == 'O') {
        /* affects predefined macros only */
        o->opt_level = a[2] ? a[2] : '1';
    } else if (!strcmp(a, "-fcheck-macro-versions")) {
        o->check_versions = true;
    } else if (!strncmp(a, "-fparallel=", 11)) {
        const char *v = a + 11;
        if (!strcmp(v, "on"))
            o->parallel = 'y';
        else if (!strcmp(v, "off"))
            o->parallel = 'n';
        else if (!strcmp(v, "auto"))
            o->parallel = 'a';
        else
            fatal("-fparallel= expects on, off or auto (got '%s')", v);
    } else if (!strncmp(a, "-fparallel-threads=", 19)) {
        o->par_threads = atoi(a + 19);
    } else if (!strncmp(a, "-j", 2)) {
        const char *v = arg_value(argc, argv, &i, "-j");
        o->jobs = atoi(v);
        if (o->jobs < 1)
            fatal("-j expects a positive number (got '%s')", v);
    } else if (!strncmp(a, "-fparallel-window=", 18)) {
        o->par_window = (unsigned)strtoul(a + 18, NULL, 10);
    } else if (!strncmp(a, "-fparallel-chunk=", 17)) {
        o->par_chunk = (size_t)strtoul(a + 17, NULL, 10);
    } else if (!strcmp(a, "-P")) {
        o->linemarkers = false;
    } else if (!strcmp(a, "-o")) {
        o->output = arg_value(argc, argv, &i, "-o");
    } else if (a[0] == '-' && a[1]) {
        return 0;
    } else {
        vec_push(&o->inputs, a);
    }
    return i - start + 1;
}

void options_finish(Options *o)
{
    int i;
    size_t k;
    o->diag = diag_config_new();
    pp_options_finish(&o->pp);
    for (k = 0; k < o->wflags.len; k++)
        if (!diag_config_apply(o->diag, o->wflags.data[k]))
            fprintf(stderr, "cereal: warning: unknown warning option "
                            "'-W%s'\n", o->wflags.data[k]);
    if (!o->pp.nostdinc)
        for (i = 0; host_include_dirs[i]; i++)
            vec_push(&o->pp.system_dirs, host_include_dirs[i]);
}

void options_free(Options *o)
{
    vec_free(&o->pp.quote_dirs);
    vec_free(&o->pp.angle_dirs);
    vec_free(&o->pp.system_dirs);
    vec_free(&o->macros);
    vec_free(&o->wflags);
    diag_config_free(o->diag);
    o->diag = NULL;
    vec_free(&o->inputs);
}

void tu_init(TU *tu, Options *opt)
{
    memset(tu, 0, sizeof *tu);
    tu->opt = opt;
    arena_init(&tu->arena);
    interner_init(&tu->in);
    srcmgr_init(&tu->sm, &tu->arena);
    diag_init(&tu->diag, &tu->arena, &tu->sm);
    tu->diag.cfg = opt->diag;
    tu->diag.werror = diag_config_werror(opt->diag);
    tu->diag.pedantic = opt->pp.pedantic || diag_config_pedantic(opt->diag);
    tu->diag.pedantic_errors = opt->pedantic_errors;
    tu->diag.color = opt->color;
    tu->diag.show_system = opt->show_system;
    pp_init(&tu->pp, &tu->arena, &tu->in, &tu->sm, &tu->diag, &opt->pp);
    tu->pp.host_attrs = host_attrs;
    tu->pp.check_versions = opt->check_versions;
    tu->pp.host_builtins = host_builtins;
}

bool tu_begin(TU *tu, const char *path)
{
    size_t i;
    PP *pp = &tu->pp;
    pp_define_builtin_text(pp, "__STDC__", "1");
    pp_define_builtin_text(pp, "__STDC_VERSION__", "199901L");
    pp_define_builtin_text(pp, "__STDC_HOSTED__", "1");
    if (!tu->opt->pp.no_predefs) {
        /* host predefines, one #define per line */
        const char *s = host_predefs;
        while (*s) {
            const char *nl = strchr(s, '\n');
            size_t n = nl ? (size_t)(nl - s) : strlen(s);
            if (n > 8 && !strncmp(s, "#define ", 8)) {
                const char *name = s + 8, *sp = memchr(name, ' ', n - 8);
                if (sp) {
                    char *nm = arena_strndup(&tu->arena, name, (size_t)(sp - name));
                    char *val = arena_strndup(&tu->arena, sp + 1,
                                              n - (size_t)(sp + 1 - s));
                    pp_define_builtin_text(pp, nm, val);
                } else {
                    char *nm = arena_strndup(&tu->arena, name, n - 8);
                    pp_define_builtin_text(pp, nm, "");
                }
            }
            s += n + (nl ? 1 : 0);
        }
    }
    if (!tu->opt->pp.no_predefs)
        for (i = 0; host_assertions[i]; i += 2)
            pp_assert_str(pp, host_assertions[i], host_assertions[i + 1]);
    if (!tu->opt->pp.no_predefs && tu->opt->pp.gnu_mode) {
        /* host -std=gnu99 differs from -std=c99 only in these */
        pp_cmdline_undef(pp, "__STRICT_ANSI__");
        pp_define_builtin_text(pp, "__STDC_UTF_16__", "1");
        pp_define_builtin_text(pp, "__STDC_UTF_32__", "1");
        pp_define_builtin_text(pp, "linux", "1");
        pp_define_builtin_text(pp, "unix", "1");
    }
    if (!tu->opt->pp.no_predefs && tu->opt->opt_level &&
        tu->opt->opt_level != '0') {
        /* mimic the host: -O<n> replaces __NO_INLINE__ by __OPTIMIZE__ */
        pp_cmdline_undef(pp, "__NO_INLINE__");
        pp_define_builtin_text(pp, "__OPTIMIZE__", "1");
        if (tu->opt->opt_level == 's' || tu->opt->opt_level == 'z')
            pp_define_builtin_text(pp, "__OPTIMIZE_SIZE__", "1");
    }
    for (i = 0; i < tu->opt->macros.len; i++) {
        CmdlineMacro *cm = &tu->opt->macros.data[i];
        if (cm->kind == 'D')
            pp_cmdline_define(pp, cm->text);
        else if (cm->kind == 'U')
            pp_cmdline_undef(pp, cm->text);
        else
            pp_cmdline_include(pp, cm->text);
    }
    return pp_enter_main(pp, path);
}

void tu_drain(TU *tu)
{
    Tok t;
    while (pp_next(&tu->pp, &t))
        ;
}

void tu_free(TU *tu)
{
    pp_free(&tu->pp);
    diag_free(&tu->diag);
    srcmgr_free(&tu->sm);
    interner_free(&tu->in);
    arena_free(&tu->arena);
    while (tu->adopted.len)
        arena_free(&tu->adopted.data[--tu->adopted.len]);
    vec_free(&tu->adopted);
}
