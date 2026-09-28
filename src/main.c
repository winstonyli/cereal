/* main.c - the `cereal` command. */
#include "driver.h"
#include "par.h"
#include "ppout.h"
#include "analysis/analysis.h"
#include "index.h"

#include <string.h>

static void usage(FILE *o)
{
    fputs(
        "usage: cereal <mode> [options] <file.c>...\n"
        "\n"
        "modes:\n"
        "  -E            preprocess to stdout (or -o FILE)\n"
        "  lint          run the preprocessor analyses\n"
        "  index         dump the macro index (LSP model) as JSON\n"
        "  query KIND FILE:LINE:COL   KIND = def | refs | hover | visible | expand\n"
        "  --list-warnings  list every -W option\n"
        "\n"
        "options:\n"
        "  -I DIR  -iquote DIR  -isystem DIR  -nostdinc\n"
        "  -D NAME[=VAL]  -U NAME  -include FILE  -undef\n"
        "  -std=c99  -pedantic  -pedantic-errors  -trigraphs\n"
        "  -W<name>  -Wno-<name>  -W<group>  -Wall  -Werror  -Weverything\n"
        "  -fdiagnostics-format=json  -fcolor-diagnostics  -P  -o FILE\n"
        "  -fparallel=auto|on|off  -fparallel-threads=N  -fparallel-chunk=BYTES\n"
        "  -j N          translation units at a time (default: all cores)\n",
        o);
}

static int finish(TU *tu, FILE *out)
{
    if (tu->opt->json)
        diag_print_json(&tu->diag, out);
    else
        diag_flush(&tu->diag);
    return tu->diag.nerrors ? 1 : 0;
}

static ThreadPool *shared_pool(Options *o)
{
    static ThreadPool pool;
    static bool made;
    if (!made) {
        pool_init(&pool, o->jobs); /* the main thread helps too */
        made = true;
    }
    return &pool;
}

/* One translation unit of a mode: text to out, diagnostics to err. */
typedef int (*TuFn)(Options *o, const char *path, FILE *out, FILE *err);

static int preprocess_one(Options *o, const char *path, FILE *out, FILE *err)
{
    TU tu;
    int rc;
    tu_init(&tu, o);
    tu.diag.out = err;
    if (o->parallel != 'n') {
        ParOptions po;
        ParResult r;
        memset(&po, 0, sizeof po);
        po.threads = o->par_threads;
        po.chunk = o->par_chunk;
        po.window = o->par_window;
        po.force = o->parallel == 'y';
        po.pool = shared_pool(o);
        r = par_write_output(&tu, path, out, o->linemarkers, &po);
        if (r != PAR_FALLBACK)
            goto done;
        tu_free(&tu); /* diverged or not worth it: start over */
        tu_init(&tu, o);
        tu.diag.out = err;
    }
    if (tu_begin(&tu, path))
        pp_write_output(&tu.pp, out, o->linemarkers);
done:
    rc = finish(&tu, out);
    tu_free(&tu);
    return rc;
}

static int lint_one(Options *o, const char *path, FILE *out, FILE *err)
{
    TU tu;
    Analysis an;
    int rc;
    tu_init(&tu, o);
    tu.diag.out = err;
    memset(&an, 0, sizeof an);
    analysis_attach(&an, &tu.pp);
    if (tu_begin(&tu, path)) {
        tu_drain(&tu);
        analysis_finish(&an);
    }
    rc = finish(&tu, out);
    tu_free(&tu);
    return rc;
}

/* ---- the TU pool: -j ------------------------------------------------ */

typedef struct Batch {
    Mutex m;
    Cond done;
} Batch;

typedef struct TuJob {
    Batch *batch;
    Options *o;
    TuFn fn;
    const char *path;
    char *out, *err;               /* buffered, written in input order */
    size_t out_len, err_len;
    int rc;
    bool finished;                 /* guarded by batch->m */
} TuJob;

static void run_tu_job(void *arg)
{
    TuJob *j = arg;
    FILE *out = open_memstream(&j->out, &j->out_len);
    FILE *err = open_memstream(&j->err, &j->err_len);
    if (!out || !err)
        fatal("out of memory");
    j->rc = j->fn(j->o, j->path, out, err);
    fclose(out);
    fclose(err);
    mutex_lock(&j->batch->m);
    j->finished = true;
    cond_broadcast(&j->batch->done);
    mutex_unlock(&j->batch->m);
}

/* Every input through fn, up to -j at a time.  Output and diagnostics
 * appear in input order, exactly as a one-at-a-time run would print them. */
static int run_inputs(Options *o, TuFn fn, FILE *out)
{
    ThreadPool *pool;
    JobGroup g;
    Batch b;
    TuJob *jobs;
    size_t i, n = o->inputs.len;
    int rc = 0;
    if (n == 1 || o->jobs == 1) {
        for (i = 0; i < n; i++)
            rc |= fn(o, o->inputs.data[i], out, stderr);
        return rc;
    }
    pool = shared_pool(o);
    mutex_init(&b.m);
    cond_init(&b.done);
    group_init(&g);
    jobs = xcalloc(n, sizeof *jobs);
    for (i = 0; i < n; i++) {
        jobs[i].batch = &b;
        jobs[i].o = o;
        jobs[i].fn = fn;
        jobs[i].path = o->inputs.data[i];
        pool_submit(pool, &g, run_tu_job, &jobs[i]);
    }
    for (i = 0; i < n; i++) {
        /* help until input i is done, then commit it */
        for (;;) {
            bool fin;
            mutex_lock(&b.m);
            fin = jobs[i].finished;
            mutex_unlock(&b.m);
            if (fin)
                break;
            if (pool_run_one(pool))
                continue;
            mutex_lock(&b.m); /* it is running elsewhere */
            while (!jobs[i].finished)
                cond_wait(&b.done, &b.m);
            mutex_unlock(&b.m);
            break;
        }
        fwrite(jobs[i].out, 1, jobs[i].out_len, out);
        fflush(out);
        fwrite(jobs[i].err, 1, jobs[i].err_len, stderr);
        free(jobs[i].out);
        free(jobs[i].err);
        rc |= jobs[i].rc;
    }
    group_wait(pool, &g);
    group_free(&g);
    cond_destroy(&b.done);
    mutex_destroy(&b.m);
    free(jobs);
    return rc;
}

static int mode_preprocess(Options *o)
{
    FILE *out = stdout;
    int rc;
    if (o->inputs.len == 0) {
        fputs("cereal: -E needs an input file\n", stderr);
        return 2;
    }
    if (o->output && o->inputs.len > 1) {
        fputs("cereal: cannot specify -o with -E and multiple files\n", stderr);
        return 2;
    }
    if (o->output && !(out = fopen(o->output, "w")))
        fatal("cannot open '%s' for writing", o->output);
    o->pp.fatal_missing_include = true;
    rc = run_inputs(o, preprocess_one, out);
    if (out != stdout)
        fclose(out);
    return rc;
}

static int mode_lint(Options *o)
{
    if (o->inputs.len == 0) {
        fputs("cereal: lint needs input files\n", stderr);
        return 2;
    }
    return run_inputs(o, lint_one, stdout);
}

static const char *loc_str(TU *tu, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(&tu->sm, loc);
    uint32_t line = 0, col = 0;
    if (!f)
        return "<unknown>";
    srcmgr_linecol(f, loc, &line, &col);
    return arena_printf(&tu->arena, "%s:%u:%u", f->name, line, col);
}

static int mode_index(Options *o, bool all)
{
    TU tu;
    Index ix;
    int rc;
    if (o->inputs.len != 1) {
        fputs("cereal: index needs exactly one input file\n", stderr);
        return 2;
    }
    tu_init(&tu, o);
    index_init(&ix, &tu.pp);
    if (tu_begin(&tu, o->inputs.data[0])) {
        index_run(&ix);
        index_dump_json(&ix, stdout, all);
    }
    rc = tu.diag.nerrors ? 1 : 0;
    if (tu.diag.nerrors)
        diag_flush(&tu.diag);
    index_free(&ix);
    tu_free(&tu);
    return rc;
}

static const char *ref_flags(const IdxRef *r)
{
    static char buf[64];
    buf[0] = 0;
    if (r->flags & IREF_STATIC)
        strcat(buf, " body-text");
    else if (r->flags & IREF_IN_BODY)
        strcat(buf, " in-body");
    if (r->flags & IREF_FROM_ARG)
        strcat(buf, " from-arg");
    if (r->flags & IREF_PASTED)
        strcat(buf, " pasted(not-renamable)");
    if (r->flags & IREF_SYSTEM)
        strcat(buf, " system");
    return buf;
}

static void print_exp_tree(TU *tu, Index *ix, IdxExp *root)
{
    size_t i;
    int k;
    for (i = 0; i < ix->exps.len; i++) {
        IdxExp *x = ix->exps.data[i];
        if (!x || x->root != root->e)
            continue;
        printf("%*s%s", x->depth * 2, "", x->e->macro->name->str);
        if (x->e->macro->funclike) {
            putchar('(');
            for (k = 0; k < x->nargs; k++)
                printf("%s%s", k ? ", " : "", x->args[k]);
            putchar(')');
        }
        printf("  [%s]\n", loc_str(tu, x->e->name_loc));
    }
    printf("=> %s\n", sb_cstr(&root->text));
}

static int mode_query(Options *o, const char *kind, const char *at)
{
    TU tu;
    Index ix;
    int rc = 0;
    char *file, *p1, *p2;
    unsigned line, col;
    SrcFile *f;
    SrcLoc loc;
    IdxTarget t;
    int k;

    if (o->inputs.len != 1 || !at) {
        fputs("cereal: usage: cereal query KIND FILE:LINE:COL main.c\n", stderr);
        return 2;
    }
    file = xstrdup(at);
    p2 = strrchr(file, ':');
    if (!p2) {
        free(file);
        fputs("cereal: position must be FILE:LINE:COL\n", stderr);
        return 2;
    }
    *p2 = 0;
    p1 = strrchr(file, ':');
    if (!p1) {
        free(file);
        fputs("cereal: position must be FILE:LINE:COL\n", stderr);
        return 2;
    }
    *p1 = 0;
    line = (unsigned)atoi(p1 + 1);
    col = (unsigned)atoi(p2 + 1);

    tu_init(&tu, o);
    index_init(&ix, &tu.pp);
    if (!tu_begin(&tu, o->inputs.data[0])) {
        diag_flush(&tu.diag);
        free(file);
        return 1;
    }
    index_run(&ix);
    f = index_find_file(&ix, file);
    if (!f) {
        fprintf(stderr, "cereal: '%s' is not part of this translation unit\n",
                file);
        rc = 1;
        goto out;
    }
    loc = srcmgr_loc_of(f, line, col);
    t = index_resolve(&ix, loc);

    if (!strcmp(kind, "def")) {
        if (t.kind == TGT_INCLUDE)
            printf("%s:1:1\n", t.file->name);
        else if (t.kind == TGT_PARAM)
            printf("%s param %s of %s\n",
                   loc_str(&tu, t.macros[0]->param_locs[t.param]),
                   t.name->str, t.macros[0]->name->str);
        else
            for (k = 0; k < t.nmacros; k++)
                printf("%s %s\n", loc_str(&tu, t.macros[k]->name_loc),
                       macro_signature(&tu.arena, t.macros[k]));
    } else if (!strcmp(kind, "refs")) {
        IdxRef *refs;
        size_t n = index_references(&ix, &t, &refs), i;
        for (i = 0; i < n; i++)
            printf("%s %s%s\n", loc_str(&tu, refs[i].loc),
                   i < (size_t)(t.kind == TGT_PARAM ? 1 : t.nmacros)
                       ? "definition"
                       : refs[i].kind == REF_EXPANSION ? "expansion"
                       : refs[i].kind == REF_IFDEF ? "ifdef"
                       : refs[i].kind == REF_DEFINED ? "defined"
                       : refs[i].kind == REF_UNDEF ? "undef"
                       : refs[i].kind == REF_IF_VALUE ? "if-value" : "pragma",
                   ref_flags(&refs[i]));
    } else if (!strcmp(kind, "hover")) {
        if (t.kind == TGT_PARAM) {
            printf("parameter %s of %s\n", t.name->str,
                   macro_signature(&tu.arena, t.macros[0]));
        } else if (t.kind == TGT_INCLUDE) {
            printf("%s\n", t.file->path);
        } else {
            for (k = 0; k < t.nmacros; k++) {
                Macro *m = t.macros[k];
                printf("#define %s %s\n", macro_signature(&tu.arena, m),
                       macro_body_str(&tu.pp, m));
                printf("  defined at %s", loc_str(&tu, m->name_loc));
                if (m->undef_loc)
                    printf(", undefined at %s", loc_str(&tu, m->undef_loc));
                putchar('\n');
            }
            if (t.kind == TGT_MACRO && !t.nmacros)
                printf("%s: not defined\n", t.name->str);
            if (t.top)
                printf("expands to: %s\n", sb_cstr(&t.top->text));
        }
    } else if (!strcmp(kind, "visible")) {
        Macro **v;
        size_t n = index_visible(&ix, loc, &v), i;
        for (i = 0; i < n; i++)
            if (!v[i]->predefined)
                printf("%s\n", macro_signature(&tu.arena, v[i]));
    } else if (!strcmp(kind, "expand")) {
        if (t.top)
            print_exp_tree(&tu, &ix, t.top);
        else
            puts("no macro invocation here");
    } else {
        fprintf(stderr, "cereal: unknown query '%s'\n", kind);
        rc = 2;
    }
out:
    free(file);
    index_free(&ix);
    tu_free(&tu);
    return rc;
}

int main(int argc, char **argv)
{
    Options o;
    const char *mode = NULL, *qkind = NULL, *qat = NULL;
    bool all = false;
    int i, rc = 0;
    options_init(&o);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        int n;
        if (!mode && (!strcmp(argv[i], "-E") || !strcmp(argv[i], "lint") ||
                      !strcmp(argv[i], "index"))) {
            mode = argv[i];
            continue;
        }
        if (!mode && !strcmp(argv[i], "query") && i + 2 < argc) {
            mode = argv[i];
            qkind = argv[++i];
            qat = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--all")) {
            all = true;
            continue;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage(stdout);
            return 0;
        }
        if (!strcmp(argv[i], "--list-warnings")) {
            diag_list_options(stdout);
            return 0;
        }
        n = options_parse_one(&o, argc, argv, i);
        if (n <= 0) {
            fprintf(stderr, "cereal: unknown option '%s'\n", argv[i]);
            return 2;
        }
        i += n - 1;
    }
    options_finish(&o);
    if (!mode) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(mode, "-E"))
        rc = mode_preprocess(&o);
    else if (!strcmp(mode, "lint"))
        rc = mode_lint(&o);
    else if (!strcmp(mode, "index"))
        rc = mode_index(&o, all);
    else if (!strcmp(mode, "query"))
        rc = mode_query(&o, qkind, qat);
    options_free(&o);
    return rc;
}
