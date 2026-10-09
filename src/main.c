/* main.c - the `cereal` command. */
#include "driver.h"
#include "par.h"
#include "ppout.h"
#include "analysis/analysis.h"
#include "index.h"
#include "cell.h"
#include "toks.h"
#include "c/csymidx.h"
#include "c/frontend.h"
#include "lsp/lsp.h"

#include <string.h>
#include <time.h>

static void usage(FILE *o)
{
    fputs(
        "usage: cereal <mode> [options] <file.c>...\n"
        "\n"
        "modes:\n"
        "  -E            preprocess to stdout (or -o FILE)\n"
        "  lint          run the preprocessor analyses\n"
        "  index         dump the macro index (LSP model) as JSON\n"
        "                --all  --check-graph\n"
        "                --replay [--no-cells] [--no-output]: rebuild on each\n"
        "                line of stdin, reusing cells (testing, timing);\n"
        "                --transcript: answer queries at every identifier\n"
        "                --tokens[=check|digest]: the token stream\n"
        "                (regenerated from the cells), or its count and hash\n"
        "                (regenerated and checked, or from the cells' records)\n"
        "  parse         parse (C99 + GNU), report syntax errors\n"
        "                --dump: print the syntax trees; --cells: parse\n"
        "                tokens regenerated from a cell build\n"
        "  check         parse and type-check (declarations, types,\n"
        "                constant expressions), gcc's diagnostics\n"
        "                --dump-types: print declarations and layouts\n"
        "                --dump-symbols: print the C symbol index\n"
        "                --verify-symbols: check it, report names it missed\n"
        "                --summaries: compute them only (timing)\n"
        "                --dump-summaries: print each unit's summary and read set\n"
        "                --validate-summaries=FILE: check each unit's read set in\n"
        "                FILE (a --dump-summaries output) against the state at\n"
        "                its entry\n"
        "  -fsyntax-only  the same as check\n"
        "  lsp           language server on stdin/stdout\n"
        "  query KIND FILE:LINE:COL   KIND = def | decl | refs | uses | highlight |\n"
        "                hover | visible | expand | callers | callees | deps\n"
        "  --list-warnings  list every -W option\n"
        "\n"
        "options:\n"
        "  -I DIR  -iquote DIR  -isystem DIR  -nostdinc\n"
        "  -D NAME[=VAL]  -U NAME  -include FILE  -undef\n"
        "  -std=c99  -pedantic  -pedantic-errors  -trigraphs\n"
        "  -W<name>  -Wno-<name>  -W<group>  -Wall  -Wextra  -Werror\n"
        "  -Werror=<name>  -Weverything\n"
        "  --target=NAME  the ABI for check (default: the host's)\n"
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

static ParOptions par_options(Options *o)
{
    ParOptions po;
    memset(&po, 0, sizeof po);
    po.threads = o->par_threads;
    po.chunk = o->par_chunk;
    po.window = o->par_window;
    po.force = o->parallel == 'y';
    po.pool = shared_pool(o);
    return po;
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
        ParOptions po = par_options(o);
        ParResult r = par_write_output(&tu, path, out, o->linemarkers, &po);
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
    if (o->parallel != 'n') {
        ParOptions po = par_options(o);
        ParClient c = analysis_par_client(&an);
        ParResult r = par_run(&tu, path, NULL, false, &po, &c, 1);
        if (r == PAR_DONE)
            analysis_finish(&an);
        else if (r == PAR_FAILED)
            analysis_discard(&an);
        if (r != PAR_FALLBACK)
            goto done;
        analysis_discard(&an); /* diverged or not worth it: start over */
        tu_free(&tu);
        tu_init(&tu, o);
        tu.diag.out = err;
        memset(&an, 0, sizeof an);
        analysis_attach(&an, &tu.pp);
    }
    if (tu_begin(&tu, path)) {
        tu_drain(&tu);
        analysis_finish(&an);
    } else {
        analysis_discard(&an);
    }
done:
    rc = finish(&tu, out);
    tu_free(&tu);
    return rc;
}

/* ---- parse, -fsyntax-only ------------------------------------------------ */

static FrontendOpts fe_opts;
static bool parse_cells, dump_symbols, verify_symbols;

static int parse_one(Options *o, const char *path, FILE *out, FILE *err)
{
    TU tu;
    FrontendOpts fo = fe_opts;
    ParOptions po;
    CIndex *ix = NULL;
    int rc;
    tu_init(&tu, o);
    tu.diag.out = err;
    fo.out = out;
    if (parse_cells) {
        po = par_options(o);
        fo.cells_par = &po;
    }
    if (dump_symbols || verify_symbols) {
        fo.cidx = &ix;
        fo.symidx_verify = verify_symbols ? out : NULL;
    }
    frontend_run(&tu, path, &fo);
    rc = finish(&tu, out);
    if (ix && dump_symbols)
        cindex_dump(ix, &tu.sm, out);
    if (ix && verify_symbols && cindex_verify(ix, &tu.sm, out) && !rc)
        rc = 1;
    cindex_free(ix);
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

/* Initialize tu and ix and index the TU (in parallel when worth it).
 * False if the input could not be opened. */
static bool index_tu(Options *o, TU *tu, Index *ix, const char *path)
{
    tu_init(tu, o);
    index_init(ix, &tu->pp);
    if (o->parallel != 'n') {
        ParOptions po = par_options(o);
        ParClient c = index_par_client(ix);
        ParResult r = par_run(tu, path, NULL, false, &po, &c, 1);
        if (r != PAR_FALLBACK)
            return r == PAR_DONE;
        index_free(ix); /* diverged or not worth it: start over */
        tu_free(tu);
        tu_init(tu, o);
        index_init(ix, &tu->pp);
    }
    if (!tu_begin(tu, path))
        return false;
    index_run(ix);
    return true;
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

static int mode_index(Options *o, bool all, bool check_graph)
{
    TU tu;
    Index ix;
    int rc;
    if (o->inputs.len != 1) {
        fputs("cereal: index needs exactly one input file\n", stderr);
        return 2;
    }
    rc = 0;
    if (index_tu(o, &tu, &ix, o->inputs.data[0])) {
        if (check_graph) {
            MacroGraph g;
            size_t checked, bad;
            mgraph_build(&g, &tu.pp);
            bad = index_check_graph(&ix, &g, stdout, &checked);
            printf("graph: %zu expansions checked, %zu outside their "
                   "closure\n", checked, bad);
            rc = bad ? 1 : 0;
            mgraph_free(&g);
        } else {
            index_dump_json(&ix, stdout, all);
        }
    }
    rc |= tu.diag.nerrors ? 1 : 0;
    if (tu.diag.nerrors)
        diag_flush(&tu.diag);
    index_free(&ix);
    tu_free(&tu);
    return rc;
}

/* ---- query transcripts (index --replay --transcript) ------------------- *
 * What the language server would answer at every identifier of every user
 * file: for comparing an index that walks cells with a materialized one. */

static const char *mac_str(TU *tu, const Macro *m)
{
    if (!m)
        return "-";
    return m->name_loc ? arena_printf(&tu->arena, "%s@%s", m->name->str,
                                      loc_str(tu, m->name_loc))
                       : arena_printf(&tu->arena, "%s@builtin", m->name->str);
}

static void print_refs(TU *tu, const IdxRef *r, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        printf("  ref %s+%u k%d f%u %s\n", loc_str(tu, r[i].loc), r[i].len,
               (int)r[i].kind, r[i].flags, mac_str(tu, r[i].macro));
}

static int file_name_cmp(const void *a, const void *b)
{
    return strcmp((*(SrcFile *const *)a)->name, (*(SrcFile *const *)b)->name);
}

static void transcript(TU *tu, Index *ix, const MacroGraph *g)
{
    VEC(SrcFile *) files = {0};
    uint32_t i;
    size_t k;
    for (i = 0; i < srcmgr_nfiles(&tu->sm); i++) {
        SrcFile *f = srcmgr_file(&tu->sm, i);
        if (f && f->kind == SF_USER && !f->system_header)
            vec_push(&files, f);
    }
    if (files.len > 1)
        qsort(files.data, files.len, sizeof *files.data, file_name_cmp);
    for (k = 0; k < files.len; k++) {
        SrcFile *f = files.data[k];
        IdxRef *refs;
        size_t n = index_file_refs(ix, f, &refs);
        uint32_t p;
        printf("# %s\n", f->name);
        print_refs(tu, refs, n);
#define IDC(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || \
                ((c) >= '0' && (c) <= '9') || (c) == '_')
        for (p = 0; p < f->size; p++) {
            IdxTarget t;
            int j;
            if (!IDC(f->buf[p]) || (p && IDC(f->buf[p - 1])) ||
                (f->buf[p] >= '0' && f->buf[p] <= '9'))
                continue;
            t = index_resolve(ix, f->base + p);
            if (t.kind == TGT_NONE)
                continue;
            printf("%s: kind %d %s %s..%s", loc_str(tu, f->base + p),
                   (int)t.kind, t.name ? t.name->str : "-",
                   loc_str(tu, t.range.begin), loc_str(tu, t.range.end));
            for (j = 0; j < t.nmacros; j++)
                printf(" %s", mac_str(tu, t.macros[j]));
            printf("\n");
            if (t.top)
                printf("  top %s %s..%s: %s\n", mac_str(tu, t.top->e->macro),
                       loc_str(tu, t.top->e->name_loc),
                       loc_str(tu, t.top->e->end_loc), sb_cstr(&t.top->text));
            if (t.kind == TGT_MACRO || t.kind == TGT_PARAM) {
                n = index_references(ix, &t, &refs);
                print_refs(tu, refs, n);
            }
            if (t.kind == TGT_MACRO && t.nmacros) {
                IdxCall *c;
                size_t nc, q;
                int dir;
                for (dir = 0; dir < 2; dir++) {
                    nc = dir ? index_callers(ix, g, t.macros[0], &c)
                             : index_callees(ix, g, t.macros[0], &c);
                    for (q = 0; q < nc; q++)
                        printf("  %s %s %s %u%s\n", dir ? "caller" : "callee",
                               c[q].name->str, mac_str(tu, c[q].macro),
                               c[q].observed, c[q].pasted ? " pasted" : "");
                }
            }
        }
#undef IDC
    }
    vec_free(&files);
}

/* ---- token streams (index --replay --tokens) ---------------------------- */

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct TokDump {
    bool full;                  /* print every token */
    uint64_t n, hash;
} TokDump;

static void dump_tok(TokDump *d, TU *tu, const PP *pp, const Tok *t,
                     SrcLoc exp_loc)
{
    const char *s = pp_text(pp, t);
    uint32_t i;
    d->n++;
    d->hash = m61_push(d->hash, tok_hash(pp, t));
    if (!d->full)
        return;
    printf("%d %d %04x %s %s ", t->kind, t->punct, t->flags,
           loc_str(tu, t->loc), loc_str(tu, exp_loc));
    for (i = 0; i < t->len; i++)
        putchar((unsigned char)s[i] < ' ' ? '?' : s[i]);
    putchar('\n');
}

/* The reference: a sequential run of its own. */
static void seq_tokens(Options *o, Interner *in, const char *path,
                       TokDump *d)
{
    TU tu;
    Tok t;
    tu_init_shared(&tu, o, in);
    if (tu_begin(&tu, path))
        while (pp_next(&tu.pp, &t))
            if (tok_in_stream(&t))
                dump_tok(d, &tu, &tu.pp, &t,
                         t.kind == TK_PRAGMA ? t.loc : tu.pp.out_exp_loc);
    tu_free(&tu);
}

/* Regenerated from a cell build, cell by cell, checking each cell's
 * record. */
static void cell_tokens(TokRegen *src, TokDump *d)
{
    size_t i;
    for (i = 0; i < src->ncells; i++) {
        const TokCell *c = &src->cells[i];
        TokCursor cur;
        Tok t;
        SrcLoc el;
        uint64_t n0 = d->n, h = 0;
        tokcur_open(&cur, src, c->s, c->e);
        while (tokcur_next(&cur, &t, &el, NULL)) {
            h = m61_push(h, tok_hash(&cur.pp, &t));
            dump_tok(d, src->tu, &cur.pp, &t, el);
        }
        if (cur.unclean || d->n - n0 != c->ntoks || h != c->hash)
            printf("!! cell %zu [%u, %u)%s: %u tokens recorded, %llu "
                   "regenerated%s%s\n", i, c->s, c->e,
                   c->reused ? " (reused)" : "", c->ntoks,
                   (unsigned long long)(d->n - n0),
                   h != c->hash ? ", hash differs" : "",
                   cur.unclean ? ", end not clean" : "");
        tokcur_close(&cur, false);
    }
}

/* index --replay: rebuild the input each time a line arrives on stdin (the
 * files are re-read), as the language server does after an edit: analyses
 * and index, one interner and, unless --no-cells, one cell cache for all
 * builds.  Prints each build's diagnostics (JSON) and index (unless
 * --no-output), then a line "=== end".  For differential tests of the
 * cache, and timing. */
static int mode_replay(Options *o, bool all, bool cells, bool quiet,
                       bool queries, int tokens)
{
    Interner *in = interner_new();
    CellCache cache;
    char line[256];
    int rc = 0;
    if (o->inputs.len != 1) {
        fputs("cereal: index --replay needs exactly one input file\n", stderr);
        return 2;
    }
    cell_cache_init(&cache);
    while (fgets(line, sizeof line, stdin)) {
        const char *path = o->inputs.data[0];
        TU tu;
        Index ix;
        Analysis an;
        ParOptions po = par_options(o);
        ParClient cs[2];
        ParResult r;
        TokRegen toks;
        if (line[0] == 'q')
            break;
        tokregen_init(&toks);
        po.toks = tokens ? &toks : NULL;
        tu_init_shared(&tu, o, in);
        memset(&an, 0, sizeof an);
        analysis_attach(&an, &tu.pp);
        index_init(&ix, &tu.pp);
        ix.want_cells = queries; /* only a materialized index dumps */
        po.cells = cells ? &cache : NULL;
        cs[0] = analysis_par_client(&an);
        cs[1] = index_par_client(&ix);
        r = o->parallel == 'n' ? PAR_FALLBACK
                               : par_run(&tu, path, NULL, false, &po, cs, 2);
        if (r == PAR_DONE) {
            analysis_finish(&an);
        } else if (r == PAR_FALLBACK) {
            analysis_discard(&an);
            index_free(&ix);
            tu_free(&tu);
            tu_init_shared(&tu, o, in);
            memset(&an, 0, sizeof an);
            analysis_attach(&an, &tu.pp);
            index_init(&ix, &tu.pp);
            if (tu_begin(&tu, path)) {
                index_run(&ix);
                analysis_finish(&an);
            } else {
                analysis_discard(&an);
            }
        } else {
            analysis_discard(&an);
        }
        if (getenv("CEREAL_CELL_STATS") && ix.cells_mode)
            index_cell_stats(&ix, stderr);
        if (!quiet) {
            diag_print_json(&tu.diag, stdout);
            if (tokens) {
                TokDump d;
                double t0 = now_sec();
                memset(&d, 0, sizeof d);
                d.full = tokens == 1;
                if (toks.ncells && tokens != 2) {
                    cell_tokens(&toks, &d);
                } else if (toks.ncells) {
                    d.hash = tokregen_hash(&toks, &d.n);
                } else {
                    seq_tokens(o, in, path, &d);
                }
                printf("tokens %llu %016llx\n", (unsigned long long)d.n,
                       (unsigned long long)d.hash);
                if (getenv("CEREAL_TOK_STATS"))
                    fprintf(stderr, "tokens: %zu cells, %s %.3fs\n",
                            toks.ncells,
                            toks.ncells ? (tokens != 2 ? "regenerated"
                                                       : "from records")
                                        : "sequential",
                            now_sec() - t0);
            } else if (queries) {
                MacroGraph g;
                mgraph_build(&g, &tu.pp);
                transcript(&tu, &ix, &g);
                mgraph_free(&g);
            } else {
                index_dump_json(&ix, stdout, all);
            }
        }
        fputs("=== end\n", stdout);
        fflush(stdout);
        rc |= tu.diag.nerrors ? 1 : 0;
        tokregen_free(&toks);
        index_free(&ix);
        tu_free(&tu);
    }
    cell_cache_free(&cache);
    interner_release(in);
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

/* `cereal query def|decl|refs|uses|highlight` on a C name (B2): the C
 * symbol index of a second, checking run over the unit, as the language
 * server's check phase builds it; one line per location (file:line:col,
 * role or for highlight write/read, kind, name, macro flags).  False: no C
 * entity at loc (the macro answer, if any, stands). */
static bool query_c(Options *o, TU *tu, SrcFile *f, SrcLoc loc, const char *kind)
{
    TU ct;
    FrontendOpts fo;
    CIndex *cx = NULL;
    uint32_t decls[16], *ev = NULL;
    size_t nd = 0, n = 0, i;
    bool hl = !strcmp(kind, "highlight");
    CIdxQuery q = !strcmp(kind, "def")    ? CIQ_DEF
                  : !strcmp(kind, "decl") ? CIQ_DECL
                  : !strcmp(kind, "uses") ? CIQ_USES
                                          : CIQ_REFS;
    memset(&fo, 0, sizeof fo);
    fo.check = true;
    fo.cidx = &cx;
    tu_init(&ct, o);
    if (frontend_run(&ct, o->inputs.data[0], &fo) && cx) {
        cindex_validate(cx, &tu->sm);
        nd = cindex_decls_at(cx, f->path, loc - f->base, decls, 16);
    }
    tu_free(&ct);
    if (nd > 1) /* e.g. a #define body token, one entity per expansion */
        printf("%zu C entities here\n", nd);
    if (nd)
        n = cindex_select(cx, decls, nd, q, hl ? cindex_file(cx, f->path) : -1,
                          &ev);
    for (i = 0; i < n; i++) {
        const CIdxEvent *e = &cx->ev[ev[i]];
        SrcFile *ef = cindex_srcfile(&tu->sm, cx->files[e->file].path);
        if (!ef)
            continue;
        printf("%s %s %s %s", loc_str(tu, ef->base + e->off),
               !hl ? cindex_role_name(e->flags)
               : (e->flags & CIX_ROLE) == CIX_REF ? "read" : "write",
               cindex_kind_name(cx->decls[e->decl].kind), cindex_name(cx, e->decl));
        cindex_print_flags(stdout, e->flags);
        putchar('\n');
    }
    free(ev);
    cindex_free(cx);
    return nd != 0;
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

    if (!index_tu(o, &tu, &ix, o->inputs.data[0])) {
        diag_flush(&tu.diag);
        index_free(&ix);
        tu_free(&tu);
        free(file);
        return 1;
    }
    f = index_find_file(&ix, file);
    if (!f) {
        fprintf(stderr, "cereal: '%s' is not part of this translation unit\n",
                file);
        rc = 1;
        goto out;
    }
    loc = srcmgr_loc_of(f, line, col);
    t = index_resolve(&ix, loc);

    /* the macro index's answer stands unless it has nothing or only knows
     * the name by its plain identifier (weak): then a C entity answers */
    if ((t.kind == TGT_NONE || t.weak) &&
        (!strcmp(kind, "def") || !strcmp(kind, "decl") || !strcmp(kind, "refs") ||
         !strcmp(kind, "uses") || !strcmp(kind, "highlight")) &&
        query_c(o, &tu, f, loc, kind))
        goto out;
    if (!strcmp(kind, "def") || !strcmp(kind, "decl")) {
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
    } else if (!strcmp(kind, "refs") || !strcmp(kind, "uses") ||
               !strcmp(kind, "highlight")) {
        /* uses: without the definitions; highlight: in the queried file */
        IdxRef *refs;
        size_t n = index_references(&ix, &t, &refs), i;
        size_t ndef = (size_t)(t.kind == TGT_PARAM ? 1 : t.nmacros);
        for (i = 0; i < n; i++) {
            if ((kind[0] == 'u' && i < ndef) ||
                (kind[0] == 'h' && srcmgr_file_of(&tu.sm, refs[i].loc) != f))
                continue;
            printf("%s %s%s\n", loc_str(&tu, refs[i].loc),
                   i < ndef
                       ? "definition"
                       : refs[i].kind == REF_EXPANSION ? "expansion"
                       : refs[i].kind == REF_IFDEF ? "ifdef"
                       : refs[i].kind == REF_DEFINED ? "defined"
                       : refs[i].kind == REF_UNDEF ? "undef"
                       : refs[i].kind == REF_IF_VALUE ? "if-value" : "pragma",
                   ref_flags(&refs[i]));
        }
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
    } else if (!strcmp(kind, "callees") || !strcmp(kind, "callers")) {
        MacroGraph g;
        bool out_calls = !strcmp(kind, "callees");
        mgraph_build(&g, &tu.pp);
        for (k = 0; k < t.nmacros; k++) {
            IdxCall *c;
            size_t n = out_calls ? index_callees(&ix, &g, t.macros[k], &c)
                                 : index_callers(&ix, &g, t.macros[k], &c), i;
            if (t.nmacros > 1)
                printf("%s %s:\n", loc_str(&tu, t.macros[k]->name_loc),
                       macro_signature(&tu.arena, t.macros[k]));
            for (i = 0; i < n; i++) {
                if (!c[i].macro)
                    printf("%s (never a macro while %s is defined)\n",
                           c[i].name->str, t.name->str);
                else
                    printf("%s %s", loc_str(&tu, c[i].macro->name_loc),
                           macro_signature(&tu.arena, c[i].macro));
                if (c[i].macro && c[i].observed)
                    printf("  [expanded %u time%s%s]", c[i].observed,
                           c[i].observed == 1 ? "" : "s",
                           c[i].pasted ? ", name formed by ##" : "");
                if (c[i].macro)
                    putchar('\n');
            }
            if (!n)
                printf("no %s\n", out_calls ? "callees" : "callers");
        }
        if (t.kind != TGT_MACRO || !t.nmacros)
            puts("no macro definition here");
        mgraph_free(&g);
    } else if (!strcmp(kind, "deps")) {
        MacroGraph g;
        MClosure cl;
        size_t i;
        if (t.kind != TGT_MACRO) {
            puts("no macro name here");
        } else {
            mgraph_build(&g, &tu.pp);
            mgraph_closure(&g, &t.name, 1, index_seq_at(&ix, loc), &cl);
            for (i = 0; i < cl.macros.len; i++)
                printf("%s %s\n", loc_str(&tu, cl.macros.data[i]->name_loc),
                       macro_signature(&tu.arena, cl.macros.data[i]));
            for (i = 0; i < cl.names.len; i++)
                if (!macro_at_version(tu.pp.mt, cl.names.data[i], index_seq_at(&ix, loc)))
                    printf("%s (not a macro here: defining it would change "
                           "the result)\n", cl.names.data[i]->str);
            if (cl.open)
                puts("open: ## may form further names");
            mclosure_free(&cl);
            mgraph_free(&g);
        }
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
    bool all = false, check_graph = false, replay = false, no_cells = false,
         quiet = false, queries = false;
    int i, rc = 0, tokens = 0;
    options_init(&o);
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    for (i = 1; i < argc; i++) {
        int n;
        if (!mode && !strcmp(argv[i], "lsp"))
            return lsp_main(stdin, stdout);
        if (!mode && (!strcmp(argv[i], "-E") || !strcmp(argv[i], "lint") ||
                      !strcmp(argv[i], "index") ||
                      !strcmp(argv[i], "parse") ||
                      !strcmp(argv[i], "check") ||
                      !strcmp(argv[i], "-fsyntax-only"))) {
            mode = argv[i];
            continue;
        }
        if (!mode && !strcmp(argv[i], "query") && i + 2 < argc) {
            mode = argv[i];
            qkind = argv[++i];
            qat = argv[++i];
            continue;
        }
        if (!strcmp(argv[i], "--dump")) {
            fe_opts.dump = true;
            continue;
        }
        if (!strcmp(argv[i], "--dump-types")) {
            fe_opts.dump_types = true;
            continue;
        }
        if (!strcmp(argv[i], "--dump-symbols")) {
            dump_symbols = true;
            continue;
        }
        if (!strcmp(argv[i], "--verify-symbols")) {
            verify_symbols = true;
            continue;
        }
        if (!strcmp(argv[i], "--summaries")) {
            fe_opts.keep_summaries = true;
            continue;
        }
        if (!strcmp(argv[i], "--dump-summaries")) {
            fe_opts.dump_summaries = true;
            continue;
        }
        if (!strncmp(argv[i], "--validate-summaries=", 21)) {
            fe_opts.validate_summaries = argv[i] + 21;
            fe_opts.dump_summaries = true;
            continue;
        }
        if (!strncmp(argv[i], "--target=", 9)) {
            fe_opts.target = target_find(argv[i] + 9);
            if (!fe_opts.target) {
                fprintf(stderr, "cereal: unknown target '%s' (known: %s)\n",
                        argv[i] + 9, target_names);
                return 2;
            }
            continue;
        }
        if (!strcmp(argv[i], "--cells")) {
            parse_cells = true;
            continue;
        }
        if (!strcmp(argv[i], "--all")) {
            all = true;
            continue;
        }
        if (!strcmp(argv[i], "--check-graph")) {
            check_graph = true;
            continue;
        }
        if (!strcmp(argv[i], "--replay")) {
            replay = true;
            continue;
        }
        if (!strcmp(argv[i], "--no-cells")) {
            no_cells = true;
            continue;
        }
        if (!strcmp(argv[i], "--no-output")) {
            quiet = true;
            continue;
        }
        if (!strcmp(argv[i], "--tokens")) {
            tokens = 1;
            continue;
        }
        if (!strcmp(argv[i], "--tokens=digest")) {
            tokens = 2;
            continue;
        }
        if (!strcmp(argv[i], "--tokens=check")) {
            tokens = 3;
            continue;
        }
        if (!strcmp(argv[i], "--transcript")) {
            queries = true;
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
    for (i = 0; i < (int)o.ignored_semantic.len; i++)
        fprintf(stderr, "cereal: note: '%s' is ignored and may change "
                "diagnostics\n", o.ignored_semantic.data[i]);
    for (i = 0; i < (int)o.ignored_deps.len; i++)
        fprintf(stderr, "cereal: note: '%s' is accepted but no dependency "
                "file is written\n", o.ignored_deps.data[i]);
    options_finish(&o);
    if (o.bad_options)
        return 1;
    if (!mode) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(mode, "-E"))
        rc = mode_preprocess(&o);
    else if (!strcmp(mode, "lint"))
        rc = mode_lint(&o);
    else if (!strcmp(mode, "index") && replay)
        rc = mode_replay(&o, all, !no_cells, quiet, queries, tokens);
    else if (!strcmp(mode, "index"))
        rc = mode_index(&o, all, check_graph);
    else if (!strcmp(mode, "parse") || !strcmp(mode, "check") ||
             !strcmp(mode, "-fsyntax-only")) {
        fe_opts.check = strcmp(mode, "parse") != 0;
        o.pp.fatal_missing_include = fe_opts.check; /* gcc stops at a missing include */
        rc = o.inputs.len ? run_inputs(&o, parse_one, stdout)
                          : (fprintf(stderr, "cereal: %s needs input files\n",
                                     mode),
                             2);
    }
    else if (!strcmp(mode, "query"))
        rc = mode_query(&o, qkind, qat);
    options_free(&o);
    return rc;
}
