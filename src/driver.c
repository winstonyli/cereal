/* driver.c - option parsing and translation-unit setup. */
#include "driver.h"

#include <ctype.h>
#include <strings.h>
#include <string.h>
#include "c/fuzzy.h"
#include "gcc_wopts.h"
#include "gcc_params.h"

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
    o->pp.lex.bidi = BIDI_UNPAIRED;
    o->linemarkers = true;
    o->parallel = 'a';
}

/* --param NAME=VALUE: the parameter only matters to the middle end, but gcc
 * rejects an unknown name or a value out of range. */
static void check_dump(Options *o, const char *a);
static void bidi_option(Options *o, const char *a);

static void check_param(Options *o, const char *arg)
{
    const char *eq = strchr(arg, '=');
    size_t nlen = eq ? (size_t)(eq - arg) : strlen(arg), k;
    long long v = 0, lo = 0, hi = 0;
    bool known = false, digits;
    const char *p;
    for (k = 0; eq && k < sizeof gcc_params / sizeof *gcc_params; k++)
        if (strlen(gcc_params[k].name) == nlen &&
            !strncmp(gcc_params[k].name, arg, nlen)) {
            known = true;
            lo = gcc_params[k].lo;
            hi = gcc_params[k].hi;
            break;
        }
    if (!known) {
        enum { NP = sizeof gcc_params / sizeof *gcc_params };
        char (*cand)[96] = malloc(NP * sizeof *cand);   /* Best keeps pointers */
        const char *dym = NULL;
        Best b;
        uint64_t work = 0;
        if (cand) {
            best_init(&b, arg, &work);
            for (k = 0; k < NP; k++) {
                snprintf(cand[k], sizeof cand[k], "%s=", gcc_params[k].name);
                best_consider(&b, cand[k]);
            }
            dym = best_get(&b);
        }
        if (dym)
            fprintf(stderr, "cereal: error: unrecognized command-line option "
                    "'--param=%s'; did you mean '--param=%s'?\n", arg, dym);
        else
            fprintf(stderr, "cereal: error: unrecognized command-line option "
                    "'--param=%s'\n", arg);
        free(cand);
        o->bad_options++;
        return;
    }
    if (hi < lo)
        return;                 /* an enumerated argument */
    p = eq + 1;
    digits = *p != '\0';
    for (; *p; p++) {
        if (*p < '0' || *p > '9') {
            digits = false;
            break;
        }
        if (v <= (1LL << 40))
            v = v * 10 + (*p - '0');
    }
    if (!digits) {
        fprintf(stderr, "cereal: error: argument to '--param=%.*s=' should be "
                "a non-negative integer\n", (int)nlen, arg);
        o->bad_options++;
    } else if (v > hi && hi == 2147483647) {
        fprintf(stderr, "cereal: error: argument to '--param=%.*s=' is bigger "
                "than %lld\n", (int)nlen, arg, hi);
        o->bad_options++;
    } else if (v < lo || v > hi) {
        fprintf(stderr, "cereal: error: argument to '--param=%.*s=' is not "
                "between %lld and %lld\n", (int)nlen, arg, lo, hi);
        o->bad_options++;
    }
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
        o->pp.lex.uliterals = true;
    } else if (!strncmp(a, "-std=", 5)) {
        fatal("only C99 is supported (got '%s')", a);
    } else if (!strcmp(a, "-pedantic") || !strcmp(a, "-Wpedantic")) {
        o->pp.pedantic = true;
        o->pp.lex.ucn_c99 = true;
        vec_push(&o->wflags, "pedantic");
    } else if (!strcmp(a, "-pedantic-errors")) {
        o->pp.pedantic = true;
        o->pp.lex.ucn_c99 = true;
        o->pedantic_errors = true;
        vec_push(&o->wflags, "pedantic");
    } else if (!strcmp(a, "-trigraphs")) {
        o->pp.lex.trigraphs = true;
    } else if (!strcmp(a, "-Wfatal-errors")) {
        o->fatal_errors = true;
    } else if (!strcmp(a, "-Wno-fatal-errors")) {
        o->fatal_errors = false;
    } else if (!strcmp(a, "-Wsystem-headers")) {
        o->show_system = true;
    } else if (!strcmp(a, "-Wno-system-headers")) {
        o->show_system = false;
    } else if (!strncmp(a, "-Wbidi-chars", 12) && (!a[12] || a[12] == '=')) {
        bidi_option(o, a);
    } else if (!strcmp(a, "-Wno-bidi-chars")) {
        o->pp.lex.bidi = 0;
    } else if (!strncmp(a, "-W", 2) && a[2]) {
        vec_push(&o->wflags, a + 2);
    } else if (!strcmp(a, "-fdiagnostics-format=json")) {
        o->json = true;
    } else if (!strcmp(a, "-fcolor-diagnostics") ||
               !strcmp(a, "-fdiagnostics-color")) {
        o->color = true;
    } else if (!strcmp(a, "-fshort-enums")) {
        o->short_enums = true;
    } else if (!strcmp(a, "-fno-short-enums")) {
        o->short_enums = false;
    } else if (!strcmp(a, "-fstrict-aliasing")) {
        o->strict_alias = 1;
    } else if (!strcmp(a, "-fno-strict-aliasing")) {
        o->strict_alias = 2;
    } else if (!strcmp(a, "-flax-vector-conversions")) {
        o->lax_vector = true;
    } else if (!strcmp(a, "-fno-lax-vector-conversions")) {
        o->lax_vector = false;
    } else if (!strcmp(a, "-w")) {
        o->no_warnings = true;
    } else if (!strncmp(a, "-ftrack-macro-expansion=", 24)) {
        o->track0 = a[24] == '0';
    } else if (!strncmp(a, "-fdump-", 7) || !strncmp(a, "-fcompare-debug", 15)) {
        /* middle-end only: no effect on diagnostics, but the pass names are
         * checked */
        check_dump(o, a);
    } else if (!strcmp(a, "--param")) {
        check_param(o, arg_value(argc, argv, &i, "--param"));
    } else if (!strncmp(a, "--param=", 8)) {
        check_param(o, a + 8);
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

/* Is name (a -W option without the -W, any no-/error= prefix and value
 * stripped) one of gcc's?  A valued option is looked up as name=. */
static bool gcc_wopt_known(const char *name, bool *valued)
{
    size_t k, n = strlen(name);
    for (k = 0; k < sizeof gcc_wopts / sizeof *gcc_wopts; k++) {
        const char *g = gcc_wopts[k];
        size_t gl = strlen(g);
        if (gl > 1 && g[gl - 1] == '-' && n >= gl && !strncmp(g, name, gl)) {
            *valued = false;    /* a joined form: -Wlarger-than-32768 */
            return true;
        }
        if (gl == n && !strcmp(g, name)) {
            *valued = false;
            return true;
        }
        if (gl == n + 1 && g[n] == '=' && !strncmp(g, name, n)) {
            *valued = true;
            return true;
        }
    }
    return false;
}

/* gcc's size arguments: digits and an optional unit (kB, KiB, MB, ...). */
static bool size_arg_ok(const char *v)
{
    static const char *const units[] = {"", "B", "kB", "KB", "KiB", "MB",
        "MiB", "GB", "GiB", "TB", "TiB", "PB", "PiB", "EB", "EiB"};
    size_t k;
    if (!isdigit((unsigned char)*v))
        return false;
    while (isdigit((unsigned char)*v))
        v++;
    for (k = 0; k < sizeof units / sizeof *units; k++)
        if (!strcasecmp(v, units[k]))
            return true;
    return false;
}

/* -Wbidi-chars[=none|unpaired|any][,ucn]: one of the first three (unpaired
 * when only ucn is given), plus ucn at most once; bare -Wbidi-chars is any. */
static void bidi_option(Options *o, const char *a)
{
    const char *v = a[12] ? a + 13 : "any", *p = v;
    int base = -1, ucn = 0;
    for (;;) {
        size_t n = strcspn(p, ",");
        int b = -1;
        if (n == 4 && !strncmp(p, "none", 4))
            b = 0;
        else if (n == 8 && !strncmp(p, "unpaired", 8))
            b = BIDI_UNPAIRED;
        else if (n == 3 && !strncmp(p, "any", 3))
            b = BIDI_ANY;
        else if (!(n == 3 && !strncmp(p, "ucn", 3))) {
            fprintf(stderr, "cereal: error: argument '%.*s' to '-Wbidi-chars' "
                    "not recognized\ncereal: note: valid arguments to "
                    "'-Wbidi-chars=' are: any none ucn unpaired\n", (int)n, p);
            o->bad_options++;
            return;
        }
        if (b >= 0 ? base >= 0 : ucn) {
            fprintf(stderr, "cereal: error: invalid argument in option "
                    "'-Wbidi-chars=%s'\n", v);
            o->bad_options++;
            return;
        }
        if (b >= 0)
            base = b;
        else
            ucn = BIDI_UCN;
        if (!p[n])
            break;
        p += n + 1;
    }
    o->pp.lex.bidi = (uint8_t)((base < 0 ? BIDI_UNPAIRED : base) | ucn);
}

static void bad_wopt(Options *o, const char *flag)
{
    const char *p = flag, *eq, *dym = NULL;
    char name[128];
    bool valued = false;
    size_t n;
    if (!strncmp(p, "no-error=", 9))
        p += 9;
    else if (!strncmp(p, "error=", 6))
        p += 6;
    else if (!strncmp(p, "no-", 3))
        p += 3;
    eq = strchr(p, '=');
    n = eq ? (size_t)(eq - p) : strlen(p);
    snprintf(name, sizeof name, "%.*s", (int)n, p);
    if (gcc_wopt_known(name, &valued)) {
        size_t m = strlen(name), k;
        bool exact = false;
        for (k = 0; !eq && valued && k < sizeof gcc_wopts / sizeof *gcc_wopts; k++)
            exact |= !strcmp(gcc_wopts[k], name);
        bool sized = !strcmp(name, "larger-than") ||
                     (m > 12 && !strcmp(name + m - 12, "-larger-than"));
        if (!eq && valued && !exact && strncmp(flag, "no-", 3) && sized) {
            /* a valued-only option written without its value */
            fprintf(stderr, "cereal: error: unrecognized command-line option "
                    "'-W%s'; did you mean '-W%s='?\n", flag, name);
            o->bad_options++;
            return;
        }
        if (eq && valued && sized && !size_arg_ok(eq + 1)) {
            fprintf(stderr, "cereal: error: argument to '-W%s=' should be a "
                    "non-negative integer optionally followed by a size "
                    "unit\n", name);
            o->bad_options++;
        }
        return;                 /* a real gcc option cereal does not model */
    }
    if (!strncmp(flag, "no-", 3))
        return;                 /* gcc ignores an unknown -Wno-... */
    {
        Best b;
        uint64_t work = 0;
        size_t k;
        best_init(&b, flag, &work);
        for (k = 0; k < sizeof gcc_wopts / sizeof *gcc_wopts; k++)
            best_consider(&b, gcc_wopts[k]);
        dym = best_get(&b);
    }
    if (dym)
        fprintf(stderr, "cereal: error: unrecognized command-line option "
                "'-W%s'; did you mean '-W%s'?\n", flag, dym);
    else
        fprintf(stderr, "cereal: error: unrecognized command-line option "
                "'-W%s'\n", flag);
    o->bad_options++;
}

/* -fdump-{ipa,tree,rtl}-PASS[-flags][=file]: gcc rejects an unknown pass
 * (the table is src/gcc_dumps.h, probed from gcc-13). */
static void check_dump(Options *o, const char *a)
{
    static const struct { const char *name; int reg; } dumps[] = {
#include "gcc_dumps.h"
    };
    enum { ND = sizeof dumps / sizeof *dumps };
    const char *rest = a + 7, *dym = NULL;
    size_t k, len, cut;
    char *cand;
    Best b;
    uint64_t work = 0;
    if (strncmp(rest, "ipa-", 4) && strncmp(rest, "tree-", 5) &&
        strncmp(rest, "rtl-", 4))
        return;
    len = strcspn(rest, "=");
    /* a pass name followed by dump flags: any '-' boundary may end the name */
    for (cut = len;; cut--) {
        if (cut == len || rest[cut] == '-')
            for (k = 0; k < ND; k++)
                if (strlen(dumps[k].name) == cut &&
                    !strncmp(dumps[k].name, rest, cut))
                    return;
        if (cut == 0)
            break;
    }
    cand = malloc(64 + len);
    if (cand) {
        char (*nm)[96] = malloc(ND * sizeof *nm);
        if (nm) {
            best_init(&b, rest, &work);
            for (k = 0; k < ND; k++) {
                if (!dumps[k].reg)
                    continue;
                snprintf(nm[k], sizeof nm[k], "%s", dumps[k].name);
                best_consider(&b, nm[k]);
            }
            dym = best_get(&b);
        }
        if (dym)
            fprintf(stderr, "cereal: error: unrecognized command-line option "
                    "'%s'; did you mean '-fdump-%s'?\n", a, dym);
        else
            fprintf(stderr, "cereal: error: unrecognized command-line option "
                    "'%s'\n", a);
        free(nm);
        free(cand);
    }
    o->bad_options++;
}

void options_finish(Options *o)
{
    int i;
    size_t k;
    o->diag = diag_config_new();
    pp_options_finish(&o->pp);
    for (k = 0; k < o->wflags.len; k++)
        if (!diag_config_apply(o->diag, o->wflags.data[k]))
            bad_wopt(o, o->wflags.data[k]);
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
    Interner *in = interner_new();
    tu_init_shared(tu, opt, in);
    interner_release(in);
}

void tu_init_shared(TU *tu, Options *opt, Interner *in)
{
    memset(tu, 0, sizeof *tu);
    tu->opt = opt;
    arena_init(&tu->arena);
    tu->in = interner_retain(in);
    srcmgr_init(&tu->sm, &tu->arena);
    diag_init(&tu->diag, &tu->arena, &tu->sm);
    tu->diag.cfg = opt->diag;
    tu->diag.werror = diag_config_werror(opt->diag);
    tu->diag.pedantic = opt->pp.pedantic || diag_config_pedantic(opt->diag);
    /* -w also silences pedwarns, even under -pedantic-errors */
    tu->diag.pedantic_errors = opt->pedantic_errors && !opt->no_warnings;
    tu->diag.color = opt->color;
    tu->diag.show_system = opt->show_system;
    tu->diag.no_warnings = opt->no_warnings;
    tu->diag.fatal_errors = opt->fatal_errors;
    tu->diag.track0 = opt->track0;
    pp_init(&tu->pp, &tu->arena, tu->in, &tu->sm, &tu->diag, &opt->pp);
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
    interner_release(tu->in);
    arena_free(&tu->arena);
    while (tu->adopted.len)
        arena_free(&tu->adopted.data[--tu->adopted.len]);
    vec_free(&tu->adopted);
}
