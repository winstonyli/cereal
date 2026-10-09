/* server.c - protocol loop, documents, units and the builder.
 *
 * A *unit* is a translation unit the server keeps built: a source file,
 * or a header opened on its own until some unit is seen to include it
 * (then the header's features come from the includer, with the includer's
 * flags, as the header is really compiled).  Each unit owns its latest
 * complete snapshot.  Edits bump the unit's wanted generation and cancel
 * its running build; the builder thread builds the newest wanted state
 * from a frozen copy of the editor buffers (an Overlay). */
#include "lsp.h"

#include "../analysis/analysis.h"
#include "../c/frontend.h"
#include "../mgraph.h"
#include "../cell.h"
#include "../par.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct Overlay {
    uint32_t refs;            /* atomic */
    size_t n;
    char **paths;
    char **texts;
    size_t *lens;
    long long *versions;      /* -1: the client sent none */
} Overlay;

typedef struct Unit {
    char *main;
    bool standalone_header;   /* no compile command: may be adopted */
    Snapshot *snap;           /* latest complete, owned reference */
    Interner *in;             /* shared by its builds: ids stay stable */
    CellCache cells;          /* phase-B results of its last build */
    uint32_t in_fresh;        /* identifiers after the interner's first build */
    long long want, built;
    uint32_t cancel;          /* atomic */
    bool queued, building;
} Unit;

typedef struct Doc {
    char *path, *uri;
    char *text;
    size_t len;
    long long version;        /* -1: the last didOpen/didChange had none */
    Unit *unit;
} Doc;

/* A rename's checks, handed to the builder (lsp_check_rename). */
typedef struct RenameJob {
    Snapshot *snap;
    CRename *q;
    char *err;
    bool done;
} RenameJob;

typedef struct Server {
    Mutex m;
    Cond work, done;
    RenameJob *rjob;          /* waiting for the builder */
    VEC(Doc *) docs;
    VEC(Unit *) units;
    VEC(Unit *) queue;
    bool stop, initialized, shutdown;
    PosEncoding enc;
    bool inactive_regions;    /* client takes clangd's inactiveRegions */
    LspConfig cfg;
    ThreadPool pool;
    long long gen;
    pthread_t builder;
} Server;

static Server S;

/* CEREAL_LSP_STATS: timings of builds on stderr */
static double stats_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static bool stats_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("CEREAL_LSP_STATS") != NULL;
    return on;
}

/* ---- overlays --------------------------------------------------------- */

static Overlay *overlay_capture(void) /* S.m held */
{
    Overlay *o = xcalloc(1, sizeof *o);
    size_t i;
    o->refs = 1;
    o->n = S.docs.len;
    o->paths = xcalloc(o->n + 1, sizeof(char *));
    o->texts = xcalloc(o->n + 1, sizeof(char *));
    o->lens = xcalloc(o->n + 1, sizeof(size_t));
    o->versions = xcalloc(o->n + 1, sizeof(long long));
    for (i = 0; i < o->n; i++) {
        Doc *d = S.docs.data[i];
        o->paths[i] = xstrdup(d->path);
        o->texts[i] = xmalloc(d->len + 1);
        memcpy(o->texts[i], d->text, d->len);
        o->texts[i][d->len] = 0;
        o->lens[i] = d->len;
        o->versions[i] = d->version;
    }
    return o;
}

bool lsp_overlay_version(const Overlay *o, const char *path,
                         long long *version)
{
    size_t i;
    for (i = 0; o && i < o->n; i++)
        if (!strcmp(o->paths[i], path)) {
            *version = o->versions[i];
            return o->versions[i] >= 0;
        }
    return false;
}

static void overlay_release(Overlay *o)
{
    size_t i;
    if (!o || atomic_add_u32(&o->refs, (uint32_t)-1) != 1)
        return;
    for (i = 0; i < o->n; i++) {
        free(o->paths[i]);
        free(o->texts[i]);
    }
    free(o->paths);
    free(o->texts);
    free(o->lens);
    free(o->versions);
    free(o);
}

static bool overlay_lookup(void *ctx, const char *path, const char **buf,
                           size_t *len)
{
    Overlay *o = ctx;
    size_t i;
    for (i = 0; i < o->n; i++)
        if (!strcmp(o->paths[i], path)) {
            *buf = o->texts[i];
            *len = o->lens[i];
            return true;
        }
    return false;
}

/* ---- snapshots ---------------------------------------------------------- */

void snapshot_release(Snapshot *s)
{
    if (!s || atomic_add_u32(&s->refs, (uint32_t)-1) != 1)
        return;
    mgraph_free(&s->graph);
    cindex_free(s->cidx);
    index_free(&s->ix);
    tu_free(&s->tu);
    config_options_free(s->opt);
    overlay_release(s->overlay);
    cdiags_free(s);
    free(s->notice);
    free((char *)s->main);
    free(s);
}

static Snapshot *snapshot_ref(Snapshot *s)
{
    if (s)
        atomic_add_u32(&s->refs, 1);
    return s;
}

static void start_tu(Snapshot *s, Analysis *an, Interner *in, uint32_t *cancel)
{
    tu_init_shared(&s->tu, s->opt, in);
    s->tu.sm.overlay = overlay_lookup;
    s->tu.sm.overlay_ctx = s->overlay;
    s->tu.pp.cancel = cancel;
    memset(an, 0, sizeof *an);
    analysis_attach(an, &s->tu.pp);
    index_init(&s->ix, &s->tu.pp);
    s->ix.want_cells = true; /* large files: queries walk the cells */
}

/* Build a unit: preprocess, analyze and index, in parallel when the file
 * is large.  NULL if cancelled. */
static Snapshot *build(const char *main, Overlay *ov, Options *opt,
                       Interner *in, CellCache *cells, uint32_t *cancel)
{
    Snapshot *s = xcalloc(1, sizeof *s);
    Analysis an;
    ParOptions po;
    ParClient cs[2];
    ParResult r;
    s->refs = 1;
    s->main = xstrdup(main);
    s->opt = opt;
    s->overlay = ov;
    start_tu(s, &an, in, cancel);
    memset(&po, 0, sizeof po);
    po.pool = &S.pool;
    po.cells = cells;
    po.cell_config = opt->fingerprint;
    /* -fparallel=on (e.g. in .cereal) forces the parallel path, cells
     * included, for files of any size */
    po.force = opt->parallel == 'y';
    po.threads = opt->par_threads;
    po.chunk = opt->par_chunk;
    po.window = opt->par_window;
    {
        const char *buf;
        size_t len;
        if (overlay_lookup(ov, main, &buf, &len))
            po.size_hint = len ? len : 1;
    }
    cs[0] = analysis_par_client(&an);
    cs[1] = index_par_client(&s->ix);
    {
        double t0 = stats_now(), t1;
        r = par_run(&s->tu, main, NULL, false, &po, cs, 2);
        t1 = stats_now();
        if (r == PAR_DONE)
            analysis_finish(&an);
        if (stats_on())
            fprintf(stderr, "lsp: par_run %.3fs, analyses %.3fs\n", t1 - t0,
                    stats_now() - t1);
    }
    if (r == PAR_DONE) {
        /* finished above */
    } else if (r == PAR_FALLBACK) {
        analysis_discard(&an);
        index_free(&s->ix);
        tu_free(&s->tu);
        start_tu(s, &an, in, cancel);
        if (tu_begin(&s->tu, main)) {
            index_run(&s->ix);
            analysis_finish(&an);
        } else {
            analysis_discard(&an);
        }
    } else {
        analysis_discard(&an); /* could not be opened */
    }
    /* complete: the listeners' state (analysis) was on this stack */
    s->tu.pp.listeners.len = 0;
    if (atomic_load_u32(cancel)) {
        s->overlay = NULL; /* the caller still owns it on failure */
        s->opt = NULL;
        index_free(&s->ix);
        tu_free(&s->tu);
        free((char *)s->main);
        free(s);
        return NULL;
    }
    {
        double t = stats_now();
        mgraph_build(&s->graph, &s->tu.pp);
        if (stats_on())
            fprintf(stderr, "lsp: macro graph %.3fs\n", stats_now() - t);
    }
    return s;
}

/* ---- compiler diagnostics: the second phase -------------------------------- */

/* Parse and check run after the macro snapshot has published, over a fresh
 * TU of their own that is dropped when the phase ends (nothing but its
 * diagnostics is kept until the symbol index exists).  Units whose
 * sources total more than this are skipped, which bounds the memory: the
 * phase holds the file texts, the parser's tokens of one declaration and
 * the checker's file scope (docs/LSP.md has the measurements).
 * CEREAL_LSP_CHECK_MAX overrides it, in bytes. */
#define CHECK_MAX_BYTES (4u << 20)

typedef struct Check {
    TU tu;
    Options *opt;
    CIndex *cidx;            /* until published to the snapshot */
} Check;

static size_t check_limit(void)
{
    const char *e = getenv("CEREAL_LSP_CHECK_MAX");
    return e ? (size_t)strtoull(e, NULL, 10) : (size_t)CHECK_MAX_BYTES;
}

static void put_size(StrBuf *sb, size_t n)
{
    if (n >= (1u << 20))
        sb_printf(sb, "%.1f MiB", (double)n / (1u << 20));
    else if (n >= (1u << 10))
        sb_printf(sb, "%.1f KiB", (double)n / (1u << 10));
    else
        sb_printf(sb, "%zu bytes", n);
}

/* A unit over the size limit gets s->notice, which its publication shows
 * as an Information line at the top of the main file. */
static bool check_eligible(const Unit *u, Snapshot *s)
{
    uint32_t i, n = srcmgr_nfiles(&s->tu.sm);
    size_t total = 0, limit = check_limit();
    if (u->standalone_header) /* a header alone is not a translation unit */
        return false;
    for (i = 0; i < n; i++) {
        SrcFile *f = srcmgr_file(&s->tu.sm, i);
        if (f->kind != SF_VIRTUAL)
            total += f->size;
    }
    if (total > limit) {
        StrBuf sb = {0};
        if (stats_on())
            fprintf(stderr, "lsp: no check: %zu source bytes\n", total);
        sb_puts(&sb, "not checked for compiler errors: its sources total ");
        put_size(&sb, total);
        sb_puts(&sb, ", over the limit of ");
        put_size(&sb, limit);
        sb_puts(&sb, " (CEREAL_LSP_CHECK_MAX)");
        s->notice = xstrdup(sb_cstr(&sb));
        sb_free(&sb);
    }
    return total && total <= limit;
}

/* The line of the first byte where file `path` differs between snapshots
 * a and b (UINT32_MAX: the same text; 0: absent from either). */
static uint32_t first_changed_line(Snapshot *a, Snapshot *b, const char *path)
{
    SrcFile *fa = NULL, *fb = NULL;
    uint32_t i, n = srcmgr_nfiles(&a->tu.sm), line = 0;
    size_t k, m;
    for (i = 0; i < n && !fa; i++) {
        SrcFile *f = srcmgr_file(&a->tu.sm, i);
        if (f->kind == SF_USER && !strcmp(f->path, path))
            fa = f;
    }
    n = srcmgr_nfiles(&b->tu.sm);
    for (i = 0; i < n && !fb; i++) {
        SrcFile *f = srcmgr_file(&b->tu.sm, i);
        if (f->kind == SF_USER && !strcmp(f->path, path))
            fb = f;
    }
    if (!fa || !fb)
        return 0;
    m = MIN(fa->size, fb->size);
    for (k = 0; k < m && fa->buf[k] == fb->buf[k]; k++)
        line += fa->buf[k] == '\n';
    return k == m && fa->size == fb->size ? UINT32_MAX : line;
}

/* Before its check ends, a snapshot shows the compiler diagnostics of the
 * previous one that lie wholly before the first edited line of their file
 * (their positions are the same there; the later ones may have moved or
 * gone, and are dropped).  One with a note in another file is carried
 * only if no file changed. */
static void carry_cdiags(Snapshot *to, Snapshot *from)
{
    size_t i;
    const char *path = NULL;
    uint32_t line = 0;
    int same = -1; /* every file unchanged; -1: not computed yet */
    for (i = 0; i < from->cdiags.len; i++) {
        const CDiag *c = &from->cdiags.data[i];
        CDiag cd;
        if (!path || strcmp(path, c->path)) {
            path = c->path;
            line = first_changed_line(from, to, path);
        }
        if (c->last_line >= line)
            continue;
        if (c->other_files) {
            if (same < 0) {
                uint32_t k, n = srcmgr_nfiles(&from->tu.sm);
                same = 1;
                for (k = 0; k < n && same; k++) {
                    SrcFile *f = srcmgr_file(&from->tu.sm, k);
                    if (f->kind == SF_USER &&
                        first_changed_line(from, to, f->path) != UINT32_MAX)
                        same = 0;
                }
            }
            if (!same)
                continue;
        }
        cd.path = xstrdup(c->path);
        cd.last_line = c->last_line;
        cd.other_files = c->other_files;
        cd.key = xstrdup(c->key);
        cd.json = xstrdup(c->json);
        vec_push(&to->cdiags, cd);
    }
}

static void check_free(Check *c)
{
    if (!c)
        return;
    cindex_free(c->cidx);
    tu_free(&c->tu);
    config_options_free(c->opt);
    free(c);
}

/* gcc stops at a missing include; the check goes on so that the symbol
 * index covers the whole file (B2 decision D2), and its diagnostics after
 * the first missing include are dropped, as gcc never reports them. */
static void drop_after_missing_include(DiagEngine *d)
{
    static const char tail[] = ": No such file or directory";
    size_t i, k, n = sizeof tail - 1;
    for (i = 0; i < d->all.len; i++) {
        const Diagnostic *dg = d->all.data[i];
        size_t m = strlen(dg->msg);
        if (dg->level >= DL_ERROR && !*dg->id && m > n &&
            !strcmp(dg->msg + m - n, tail)) {
            for (k = i + 1; k < d->all.len; k++)
                vec_free(&d->all.data[k]->notes);
            d->all.len = i + 1;
            return;
        }
    }
}

/* NULL if the unit changed meanwhile (cancelled) or could not be opened. */
static Check *check_run(Unit *u, Snapshot *s)
{
    Check *c = xcalloc(1, sizeof *c);
    FrontendOpts fo;
    bool ok;
    if (fault_hit("lsp-check"))
        fatal("injected fault (lsp-check)");
    c->opt = config_options_for(&S.cfg, s->main);
    c->opt->pp.fatal_missing_include = false; /* D2 */
    tu_init(&c->tu, c->opt);
    c->tu.sm.overlay = overlay_lookup;
    c->tu.sm.overlay_ctx = s->overlay;
    c->tu.pp.cancel = &u->cancel;
    memset(&fo, 0, sizeof fo);
    fo.check = true;
    fo.cidx = &c->cidx;
    ok = frontend_run(&c->tu, s->main, &fo);
    if (!ok || atomic_load_u32(&u->cancel)) {
        check_free(c);
        return NULL;
    }
    drop_after_missing_include(&c->tu.diag);
    return c;
}

/* ---- units --------------------------------------------------------------- */

static bool has_command(const char *path)
{
    size_t i;
    for (i = 0; i < S.cfg.cmds.len; i++)
        if (!strcmp(S.cfg.cmds.data[i].file, path))
            return true;
    return false;
}

static bool is_header(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcmp(dot, ".h") || !strcmp(dot, ".inc") ||
                   !strcmp(dot, ".def"));
}

static bool snapshot_has(Snapshot *s, const char *path)
{
    uint32_t i, n;
    if (!s)
        return false;
    n = srcmgr_nfiles(&s->tu.sm);
    for (i = 0; i < n; i++) {
        SrcFile *f = srcmgr_file(&s->tu.sm, i);
        if (f->kind == SF_USER && !strcmp(f->path, path))
            return true;
    }
    return false;
}

static Unit *unit_named(const char *main) /* S.m held */
{
    size_t i;
    Unit *u;
    for (i = 0; i < S.units.len; i++)
        if (!strcmp(S.units.data[i]->main, main))
            return S.units.data[i];
    u = xcalloc(1, sizeof *u);
    u->main = xstrdup(main);
    u->standalone_header = !has_command(main) && is_header(main);
    vec_push(&S.units, u);
    return u;
}

/* The unit giving a document its features: its own, or for a header
 * without a compile command, a unit that includes it. */
static Unit *unit_for(const char *path) /* S.m held */
{
    size_t i;
    if (!has_command(path) && is_header(path))
        for (i = 0; i < S.units.len; i++)
            if (!S.units.data[i]->standalone_header &&
                snapshot_has(S.units.data[i]->snap, path))
                return S.units.data[i];
    return unit_named(path);
}

static void schedule(Unit *u) /* S.m held */
{
    u->want = ++S.gen;
    atomic_store_u32(&u->cancel, 1); /* a running build is stale now */
    if (!u->queued) {
        u->queued = true;
        vec_push(&S.queue, u);
    }
    cond_broadcast(&S.work);
}

/* After a snapshot of u: headers opened on their own that u includes move
 * to u. */
static void adopt_headers(Unit *u) /* S.m held */
{
    size_t i;
    if (u->standalone_header)
        return;
    for (i = 0; i < S.docs.len; i++) {
        Doc *d = S.docs.data[i];
        if (d->unit != u && d->unit->standalone_header &&
            snapshot_has(u->snap, d->path))
            d->unit = u;
    }
}

static bool doc_open_in(void *ctx, const char *path)
{
    Unit *u = ctx;
    size_t i;
    for (i = 0; i < S.docs.len; i++)
        if (S.docs.data[i]->unit == u && !strcmp(S.docs.data[i]->path, path))
            return true;
    return false;
}

/* fatal() inside a build or a check drops that phase, not the server: the
 * phase's partial state is leaked (it may be inconsistent), after a build
 * the unit starts over with a new interner and cell cache (the check has
 * its own), and the message goes to stderr and to the client's log
 * (window/logMessage, an error). */
static void phase_failed(Unit *u, const char *phase, const char *msg)
{
    StrBuf sb = {0}, text = {0};
    JsonWriter w;
    fprintf(stderr, "cereal: fatal: %s (the %s of %s is dropped)\n", msg,
            phase, u->main);
    if (!strcmp(phase, "build")) {
        interner_release(u->in);
        u->in = NULL;
        cell_cache_free(&u->cells);
    }
    sb_printf(&text, "cereal: the %s of %s failed: %s", phase, u->main, msg);
    json_init_buf(&w, &sb);
    json_begin_object(&w);
    json_key(&w, "jsonrpc");
    json_str(&w, "2.0");
    json_key(&w, "method");
    json_str(&w, "window/logMessage");
    json_key(&w, "params");
    json_begin_object(&w);
    json_key(&w, "type");
    json_int(&w, 1);
    json_key(&w, "message");
    json_str(&w, sb_cstr(&text));
    json_end_object(&w);
    json_end_object(&w);
    rpc_write(sb.data, sb.len);
    sb_free(&sb);
    sb_free(&text);
}

/* The macro phase of a unit under a fatal() trap; NULL if it was
 * cancelled, could not be opened or failed (*failed). */
static Snapshot *build_unit(Unit *u, Overlay *ov, bool *failed)
{
    FatalTrap tr;
    Options *opt;
    Snapshot *snap;
    *failed = false;
    fatal_trap_push(&tr);
    if (setjmp(tr.jb)) {
        phase_failed(u, "build", tr.msg);
        *failed = true;
        return NULL;
    }
    opt = config_options_for(&S.cfg, u->main);
    snap = build(u->main, ov, opt, u->in, &u->cells, &u->cancel);
    fatal_trap_pop(&tr);
    if (!snap)
        config_options_free(opt);
    return snap;
}

/* The check phase under a fatal() trap (see check_run). */
static Check *check_unit(Unit *u, Snapshot *s)
{
    FatalTrap tr;
    Check *c;
    fatal_trap_push(&tr);
    if (setjmp(tr.jb)) {
        phase_failed(u, "check", tr.msg);
        return NULL;
    }
    c = check_run(u, s);
    if (c && c->cidx) /* files changed on disk between the phases: stale */
        cindex_validate(c->cidx, &s->tu.sm);
    fatal_trap_pop(&tr);
    return c;
}

/* A rename job's checks under a fatal() trap, with the options and buffers
 * of its snapshot. */
static void rename_run(RenameJob *j)
{
    FatalTrap tr;
    Options *opt = config_options_for(&S.cfg, j->snap->main);
    opt->pp.fatal_missing_include = false; /* as the check's (D2) */
    j->q->o = opt;
    j->q->main = j->snap->main;
    j->q->overlay = j->snap->overlay ? overlay_lookup : NULL;
    j->q->overlay_ctx = j->snap->overlay;
    fatal_trap_push(&tr);
    if (setjmp(tr.jb)) {
        StrBuf sb = {0};
        sb_printf(&sb, "the rename check failed: %s", tr.msg);
        sb_cstr(&sb);
        j->err = sb.data;
        config_options_free(opt);
        return;
    }
    j->err = c_rename(j->q);
    fatal_trap_pop(&tr);
    config_options_free(opt);
}

char *lsp_check_rename(Snapshot *snap, CRename *q)
{
    RenameJob j;
    memset(&j, 0, sizeof j);
    j.snap = snap;
    j.q = q;
    mutex_lock(&S.m);
    S.rjob = &j;
    cond_broadcast(&S.work);
    while (!j.done) {
        if (S.stop && S.rjob == &j) { /* the builder has gone */
            S.rjob = NULL;
            mutex_unlock(&S.m);
            return xstrdup("the server is stopping");
        }
        cond_wait(&S.done, &S.m);
    }
    mutex_unlock(&S.m);
    return j.err;
}

static void *builder_main(void *arg)
{
    (void)arg;
    mutex_lock(&S.m);
    while (!S.stop) {
        Unit *u;
        Overlay *ov;
        Snapshot *snap, *old = NULL, *checking = NULL;
        bool failed;
        long long want;
        double t0, t1, t2;
        if (S.rjob) {
            RenameJob *j = S.rjob;
            S.rjob = NULL;
            mutex_unlock(&S.m);
            rename_run(j);
            mutex_lock(&S.m);
            j->done = true;
            cond_broadcast(&S.done);
            continue;
        }
        if (!S.queue.len) {
            cond_wait(&S.work, &S.m);
            continue;
        }
        u = S.queue.data[0];
        memmove(S.queue.data, S.queue.data + 1,
                sizeof(Unit *) * --S.queue.len);
        u->queued = false;
        u->building = true;
        want = u->want;
        atomic_store_u32(&u->cancel, 0);
        t0 = stats_now();
        ov = overlay_capture();
        mutex_unlock(&S.m);
        t1 = stats_now();

        /* names typed and deleted pile up in a shared interner: start a
         * new one when it has grown well past what a build needs */
        if (u->in && interner_count(u->in) > 2 * u->in_fresh + 65536) {
            interner_release(u->in);
            u->in = NULL;
        }
        if (!u->in) {
            u->in = interner_new();
            u->in_fresh = 0;
            cell_cache_free(&u->cells); /* cells hold identifiers */
        }
        snap = build_unit(u, ov, &failed);
        if (snap && !u->in_fresh)
            u->in_fresh = interner_count(u->in);
        if (!snap)
            overlay_release(ov);

        mutex_lock(&S.m);
        if (snap && want > u->built) {
            snap->gen = want;
            old = u->snap;
            u->snap = snap;
            u->built = want;
            adopt_headers(u);
            if (check_eligible(u, snap)) {
                snap->check_state = CHECK_PENDING;
                checking = snapshot_ref(snap);
                if (old) /* shown until the check ends: no flicker */
                    carry_cdiags(snap, old);
            }
            t2 = stats_now();
            lsp_publish_diagnostics(snap, NULL, S.enc, doc_open_in, u);
            if (stats_on())
                fprintf(stderr, "lsp: overlay %.3fs, build %.3fs, publish "
                        "%.3fs\n", t1 - t0, t2 - t1, stats_now() - t2);
            if (S.inactive_regions) {
                size_t i;
                for (i = 0; i < S.docs.len; i++)
                    if (S.docs.data[i]->unit == u)
                        lsp_publish_inactive(snap, S.enc,
                                             S.docs.data[i]->path);
            }
        } else if (snap) {
            snapshot_release(snap);
        }
        if (!checking) /* a barrier (waitIdle) also waits for the check */
            u->building = false;
        cond_broadcast(&S.done);
        mutex_unlock(&S.m);
        snapshot_release(old);
        if (checking) {
            /* Phase 2, outside the lock; an edit sets u->cancel, which
             * the preprocessor sees at the next token.  What it finds is
             * published only if no edit came in since (u->want is bumped
             * under the lock), so it always matches the buffers it read. */
            Check *c;
            double t3 = stats_now();
            c = check_unit(u, checking);
            mutex_lock(&S.m);
            if (stats_on())
                fprintf(stderr, "lsp: check %.3fs%s, symbols %zu bytes\n",
                        stats_now() - t3, c ? "" : " (cancelled)",
                        c ? cindex_bytes(c->cidx) : 0);
            if (u->want == want && !atomic_load_u32(&u->cancel)) {
                if (c) {
                    lsp_publish_diagnostics(checking, &c->tu, S.enc,
                                            doc_open_in, u);
                    checking->cidx = c->cidx;
                    c->cidx = NULL;
                } else if (checking->cdiags.len) {
                    /* failed, not superseded: the carried ones go */
                    cdiags_free(checking);
                    lsp_publish_diagnostics(checking, NULL, S.enc,
                                            doc_open_in, u);
                }
            }
            checking->check_state = CHECK_DONE;
            u->building = false;
            cond_broadcast(&S.done);
            mutex_unlock(&S.m);
            check_free(c);
            snapshot_release(checking);
        }
        mutex_lock(&S.m);
    }
    mutex_unlock(&S.m);
    return NULL;
}

/* ---- documents ------------------------------------------------------------ */

static Doc *doc_find(const char *path) /* S.m held */
{
    size_t i;
    for (i = 0; i < S.docs.len; i++)
        if (!strcmp(S.docs.data[i]->path, path))
            return S.docs.data[i];
    return NULL;
}

static void doc_free(Doc *d)
{
    free(d->path);
    free(d->uri);
    free(d->text);
    free(d);
}

/* Apply one contentChanges entry: a range edit, or the whole text. */
static void apply_change(Doc *d, const JsonValue *ch)
{
    const JsonValue *range = json_get(ch, "range");
    const char *text = json_str_of(json_get(ch, "text"), "");
    size_t tl = strlen(text);
    if (!range) {
        free(d->text);
        d->text = xstrdup(text);
        d->len = tl;
        return;
    }
    {
        const JsonValue *st = json_get(range, "start"),
                        *en = json_get(range, "end");
        size_t a = pos_to_offset(d->text, d->len,
                                 (uint32_t)json_int_of(json_get(st, "line"), 0),
                                 (uint32_t)json_int_of(json_get(st, "character"), 0),
                                 S.enc);
        size_t b = pos_to_offset(d->text, d->len,
                                 (uint32_t)json_int_of(json_get(en, "line"), 0),
                                 (uint32_t)json_int_of(json_get(en, "character"), 0),
                                 S.enc);
        char *nt;
        if (b < a)
            b = a;
        nt = xmalloc(d->len - (b - a) + tl + 1);
        memcpy(nt, d->text, a);
        memcpy(nt + a, text, tl);
        memcpy(nt + a + tl, d->text + b, d->len - b);
        d->len = d->len - (b - a) + tl;
        nt[d->len] = 0;
        free(d->text);
        d->text = nt;
    }
}

/* ---- responses ---------------------------------------------------------- */

static void send_json(StrBuf *sb)
{
    rpc_write(sb->data, sb->len);
}

static void begin_response(JsonWriter *w, StrBuf *sb, const JsonValue *id)
{
    json_init_buf(w, sb);
    json_begin_object(w);
    json_key(w, "jsonrpc");
    json_str(w, "2.0");
    json_key(w, "id");
    if (id)
        json_raw(w, id->text, id->text_len);
    else
        json_null(w);
}

static void respond_error(const JsonValue *id, int code, const char *msg)
{
    StrBuf sb = {0};
    JsonWriter w;
    begin_response(&w, &sb, id);
    json_key(&w, "error");
    json_begin_object(&w);
    json_key(&w, "code");
    json_int(&w, code);
    json_key(&w, "message");
    json_str(&w, msg);
    json_end_object(&w);
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
}

static void respond_null(const JsonValue *id)
{
    StrBuf sb = {0};
    JsonWriter w;
    begin_response(&w, &sb, id);
    json_key(&w, "result");
    json_null(&w);
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
}

static void initialize(const JsonValue *id, const JsonValue *params)
{
    StrBuf sb = {0};
    JsonWriter w;
    const JsonValue *encs =
        json_path(params, "capabilities.general.positionEncodings");
    const char *root_uri = json_str_of(json_get(params, "rootUri"), NULL);
    const char *root = NULL;
    Arena a;
    size_t i;
    arena_init(&a);
    S.enc = ENC_UTF16;
    for (i = 0; encs && encs->kind == JV_ARR && i < encs->len; i++)
        if (!strcmp(json_str_of(encs->items[i], ""), "utf-8"))
            S.enc = ENC_UTF8;
    S.inactive_regions = json_bool_of(
        json_path(params, "capabilities.textDocument.inactiveRegionsCapabilities"
                          ".inactiveRegions"), false);
    if (root_uri)
        root = uri_to_path(&a, root_uri);
    if (!root)
        root = json_str_of(json_get(params, "rootPath"), NULL);
    config_init(&S.cfg, root);
    arena_free(&a);

    begin_response(&w, &sb, id);
    json_key(&w, "result");
    json_begin_object(&w);
    json_key(&w, "capabilities");
    json_begin_object(&w);
    json_key(&w, "positionEncoding");
    json_str(&w, S.enc == ENC_UTF8 ? "utf-8" : "utf-16");
    json_key(&w, "textDocumentSync");
    json_begin_object(&w);
    json_key(&w, "openClose");
    json_bool(&w, true);
    json_key(&w, "change");
    json_int(&w, 2); /* incremental */
    json_end_object(&w);
    json_key(&w, "definitionProvider");
    json_bool(&w, true);
    json_key(&w, "declarationProvider");
    json_bool(&w, true);
    json_key(&w, "typeDefinitionProvider");
    json_bool(&w, true);
    json_key(&w, "referencesProvider");
    json_bool(&w, true);
    json_key(&w, "documentHighlightProvider");
    json_bool(&w, true);
    json_key(&w, "hoverProvider");
    json_bool(&w, true);
    json_key(&w, "completionProvider");
    json_begin_object(&w);
    json_key(&w, "resolveProvider");
    json_bool(&w, false);
    json_end_object(&w);
    json_key(&w, "documentSymbolProvider");
    json_bool(&w, true);
    json_key(&w, "semanticTokensProvider");
    json_begin_object(&w);
    json_key(&w, "legend");
    json_begin_object(&w);
    json_key(&w, "tokenTypes");
    json_begin_array(&w);
    for (i = 0; lsp_token_types[i]; i++)
        json_str(&w, lsp_token_types[i]);
    json_end_array(&w);
    json_key(&w, "tokenModifiers");
    json_begin_array(&w);
    for (i = 0; lsp_token_modifiers[i]; i++)
        json_str(&w, lsp_token_modifiers[i]);
    json_end_array(&w);
    json_end_object(&w);
    json_key(&w, "range");
    json_bool(&w, true);
    json_key(&w, "full");
    json_begin_object(&w);
    json_key(&w, "delta");
    json_bool(&w, true);
    json_end_object(&w);
    json_end_object(&w);
    json_key(&w, "foldingRangeProvider");
    json_bool(&w, true);
    json_key(&w, "renameProvider");
    json_begin_object(&w);
    json_key(&w, "prepareProvider");
    json_bool(&w, true);
    json_end_object(&w);
    json_key(&w, "callHierarchyProvider");
    json_bool(&w, true);
    json_key(&w, "signatureHelpProvider");
    json_begin_object(&w);
    json_key(&w, "triggerCharacters");
    json_begin_array(&w);
    json_str(&w, "(");
    json_str(&w, ",");
    json_end_array(&w);
    json_end_object(&w);
    json_key(&w, "inactiveRegionsProvider"); /* clangd extension */
    json_bool(&w, true);
    json_end_object(&w);
    json_key(&w, "serverInfo");
    json_begin_object(&w);
    json_key(&w, "name");
    json_str(&w, "cereal");
    json_end_object(&w);
    json_end_object(&w);
    json_end_object(&w);
    send_json(&sb);
    sb_free(&sb);
    S.initialized = true;
}

/* ---- requests ------------------------------------------------------------ */

typedef enum {
    R_DEF, R_DECL, R_TYPEDEF, R_REFS, R_HIGHLIGHT, R_HOVER, R_COMPLETION,
    R_SYMBOLS, R_SEMTOK, R_SEMTOK_DELTA, R_SEMTOK_RANGE, R_FOLDING,
    R_PREP_RENAME, R_RENAME, R_PREP_CALLS, R_IN_CALLS, R_OUT_CALLS,
    R_SIGHELP, R_EXPAND
} ReqKind;

static const struct {
    const char *method;
    ReqKind kind;
    bool cidx;               /* reads the C index: waits for it (D1) */
} REQS[] = {
    {"textDocument/definition", R_DEF, true},
    {"textDocument/declaration", R_DECL, true},
    {"textDocument/typeDefinition", R_TYPEDEF, true},
    {"textDocument/references", R_REFS, true},
    {"textDocument/documentHighlight", R_HIGHLIGHT, true},
    {"textDocument/hover", R_HOVER, true},
    {"textDocument/completion", R_COMPLETION, true},
    {"textDocument/documentSymbol", R_SYMBOLS, true},
    {"textDocument/semanticTokens/full", R_SEMTOK, true},
    {"textDocument/semanticTokens/full/delta", R_SEMTOK_DELTA, true},
    {"textDocument/semanticTokens/range", R_SEMTOK_RANGE, true},
    {"textDocument/foldingRange", R_FOLDING, false},
    {"textDocument/prepareRename", R_PREP_RENAME, true},
    {"textDocument/rename", R_RENAME, true},
    {"textDocument/prepareCallHierarchy", R_PREP_CALLS, false},
    {"callHierarchy/incomingCalls", R_IN_CALLS, false},
    {"callHierarchy/outgoingCalls", R_OUT_CALLS, false},
    {"textDocument/signatureHelp", R_SIGHELP, true},
    {"cereal/expandMacro", R_EXPAND, false},
    {NULL, R_DEF, false}};

/* The document a request is about: textDocument.uri, or item.uri for
 * call hierarchy follow-ups. */
static const char *request_uri(const JsonValue *params)
{
    const char *u = json_str_of(json_path(params, "textDocument.uri"), NULL);
    return u ? u : json_str_of(json_path(params, "item.uri"), NULL);
}

static void handle_request(const JsonValue *id, ReqKind k, bool cidx,
                           const JsonValue *params)
{
    Arena a;
    Req r;
    const char *uri = request_uri(params);
    char *path;
    Doc *d;
    Unit *u;
    StrBuf sb = {0};
    JsonWriter w;
    const char *err = NULL;
    double t0 = stats_now(), t1;
    arena_init(&a);
    path = uri ? uri_to_path(&a, uri) : NULL;
    memset(&r, 0, sizeof r);
    mutex_lock(&S.m);
    d = path ? doc_find(path) : NULL;
    if (!d) {
        mutex_unlock(&S.m);
        arena_free(&a);
        respond_null(id);
        return;
    }
    u = d->unit;
    while (!u->snap && !S.stop) /* first build of this unit */
        cond_wait(&S.done, &S.m);
    if (cidx) {
        /* the C index comes with the check of the newest edit's snapshot:
         * wait for it a little (B2 decision D1), then answer from what is
         * there (the macros alone if the check has not published) */
        struct timespec dl = cond_deadline(1.5);
        while (!S.stop && (u->snap->gen < u->want ||
                           u->snap->check_state == CHECK_PENDING))
            if (!cond_timedwait(&S.done, &S.m, &dl))
                break;
    }
    r.snap = snapshot_ref(u->snap);
    r.cidx = r.snap ? r.snap->cidx : NULL;
    r.c_fresh = r.cidx && u->snap->gen == u->want &&
                u->snap->check_state == CHECK_DONE;
    r.text = arena_strndup(&a, d->text, d->len);
    r.text_len = d->len;
    mutex_unlock(&S.m);
    if (!r.snap) {
        arena_free(&a);
        respond_null(id);
        return;
    }
    r.path = path;
    r.enc = S.enc;
    r.arena = &a;
    r.params = params;
    r.file = index_find_file(&r.snap->ix, path);

    begin_response(&w, &sb, id);
    json_key(&w, "result");
    if (!r.file) {
        json_null(&w);
    } else {
        switch (k) {
        case R_DEF: lsp_definition(&r, &w); break;
        case R_DECL: lsp_declaration(&r, &w); break;
        case R_TYPEDEF: lsp_type_definition(&r, &w); break;
        case R_REFS: lsp_references(&r, &w); break;
        case R_HIGHLIGHT: lsp_document_highlight(&r, &w); break;
        case R_HOVER: lsp_hover(&r, &w); break;
        case R_COMPLETION: lsp_completion(&r, &w); break;
        case R_SYMBOLS: lsp_document_symbols(&r, &w); break;
        case R_SEMTOK: lsp_semantic_tokens(&r, &w, NULL); break;
        case R_SEMTOK_DELTA:
            lsp_semantic_tokens(&r, &w, json_str_of(json_get(params,
                                                             "previousResultId"),
                                                    ""));
            break;
        case R_SEMTOK_RANGE: lsp_semantic_tokens_range(&r, &w); break;
        case R_FOLDING: lsp_folding(&r, &w); break;
        case R_PREP_RENAME:
        case R_RENAME:
            if (!(k == R_RENAME ? lsp_rename : lsp_prepare_rename)(&r, &w,
                                                                   &err)) {
                sb.len = 0;
                snapshot_release(r.snap);
                respond_error(id, -32803 /* RequestFailed */, err);
                arena_free(&a);
                sb_free(&sb);
                return;
            }
            break;
        case R_PREP_CALLS: lsp_prepare_call_hierarchy(&r, &w); break;
        case R_IN_CALLS: lsp_calls(&r, &w, true); break;
        case R_OUT_CALLS: lsp_calls(&r, &w, false); break;
        case R_SIGHELP: lsp_signature_help(&r, &w); break;
        case R_EXPAND: lsp_expand_macro(&r, &w); break;
        }
    }
    json_end_object(&w);
    t1 = stats_now();
    send_json(&sb);
    if (stats_on())
        fprintf(stderr, "lsp: request %d: %.3fs, sent %zu bytes in %.3fs\n",
                (int)k, t1 - t0, sb.len, stats_now() - t1);
    sb_free(&sb);
    snapshot_release(r.snap);
    arena_free(&a);
}

static void did_open(const JsonValue *params)
{
    const JsonValue *td = json_get(params, "textDocument");
    const char *uri = json_str_of(json_get(td, "uri"), NULL);
    const char *text = json_str_of(json_get(td, "text"), "");
    Arena a;
    char *path;
    Doc *d;
    if (!uri)
        return;
    arena_init(&a);
    path = uri_to_path(&a, uri);
    if (path) {
        mutex_lock(&S.m);
        if (!(d = doc_find(path))) {
            d = xcalloc(1, sizeof *d);
            d->path = xstrdup(path);
            d->uri = xstrdup(uri);
            vec_push(&S.docs, d);
        }
        free(d->text);
        d->text = xstrdup(text);
        d->len = strlen(text);
        d->version = json_int_of(json_get(td, "version"), -1);
        d->unit = unit_for(path);
        schedule(d->unit);
        mutex_unlock(&S.m);
    }
    arena_free(&a);
}

static void did_change(const JsonValue *params)
{
    const char *uri = json_str_of(json_path(params, "textDocument.uri"), NULL);
    const JsonValue *changes = json_get(params, "contentChanges");
    Arena a;
    char *path;
    Doc *d;
    size_t i;
    if (!uri)
        return;
    arena_init(&a);
    path = uri_to_path(&a, uri);
    mutex_lock(&S.m);
    if (path && (d = doc_find(path)) != NULL) {
        for (i = 0; changes && changes->kind == JV_ARR && i < changes->len; i++)
            apply_change(d, changes->items[i]);
        d->version = json_int_of(json_path(params, "textDocument.version"),
                                 -1);
        schedule(d->unit);
    }
    mutex_unlock(&S.m);
    arena_free(&a);
}

static void did_close(const JsonValue *params)
{
    const char *uri = json_str_of(json_path(params, "textDocument.uri"), NULL);
    Arena a;
    char *path;
    size_t i;
    if (!uri)
        return;
    arena_init(&a);
    path = uri_to_path(&a, uri);
    mutex_lock(&S.m);
    for (i = 0; path && i < S.docs.len; i++)
        if (!strcmp(S.docs.data[i]->path, path)) {
            Doc *d = S.docs.data[i];
            Unit *u = d->unit;
            S.docs.data[i] = S.docs.data[--S.docs.len];
            doc_free(d);
            schedule(u); /* back to the file on disk */
            break;
        }
    mutex_unlock(&S.m);
    if (path)
        lsp_forget_tokens(path);
    if (uri) { /* clear its diagnostics */
        StrBuf sb = {0};
        JsonWriter w;
        json_init_buf(&w, &sb);
        json_begin_object(&w);
        json_key(&w, "jsonrpc");
        json_str(&w, "2.0");
        json_key(&w, "method");
        json_str(&w, "textDocument/publishDiagnostics");
        json_key(&w, "params");
        json_begin_object(&w);
        json_key(&w, "uri");
        json_str(&w, uri);
        json_key(&w, "diagnostics");
        json_begin_array(&w);
        json_end_array(&w);
        json_end_object(&w);
        json_end_object(&w);
        send_json(&sb);
        sb_free(&sb);
    }
    arena_free(&a);
}

/* ---- main loop ------------------------------------------------------------ */

int lsp_main(FILE *in, FILE *out)
{
    char *body;
    size_t len;
    int rc = 1;
    memset(&S, 0, sizeof S);
    mutex_init(&S.m);
    cond_init(&S.work);
    cond_init(&S.done);
    pool_init(&S.pool, 0);
    S.pool.trap_fatal = true; /* a build's fatal() ends at its builder */
    rpc_set_output(out);
    if (pthread_create(&S.builder, NULL, builder_main, NULL) != 0)
        fatal("cannot start the builder thread");
    for (;;) {
        Arena a;
        JsonValue *msg, *id, *params;
        const char *method;
        double t0 = stats_now(), t1;
        if (!(body = rpc_read(in, &len)))
            break;
        t1 = stats_now();
        arena_init(&a);
        msg = json_parse(&a, body, len); /* values point into body */
        if (stats_on() && len > (1u << 20))
            fprintf(stderr, "lsp: %zu-byte message: read %.3fs, parse %.3fs\n",
                    len, t1 - t0, stats_now() - t1);
        if (!msg || msg->kind != JV_OBJ) {
            respond_error(NULL, -32700, "parse error");
            free(body);
            arena_free(&a);
            continue;
        }
        method = json_str_of(json_get(msg, "method"), NULL);
        id = json_get(msg, "id");
        params = json_get(msg, "params");
        if (!method) { /* a response to something we never send */
            free(body);
            arena_free(&a);
            continue;
        }
        if (!strcmp(method, "exit")) {
            rc = S.shutdown ? 0 : 1;
            free(body);
            arena_free(&a);
            break;
        }
        if (!strcmp(method, "initialize")) {
            initialize(id, params);
        } else if (!S.initialized) {
            if (id)
                respond_error(id, -32002, "server not initialized");
        } else if (!strcmp(method, "shutdown")) {
            S.shutdown = true;
            respond_null(id);
        } else if (!strcmp(method, "textDocument/didOpen")) {
            did_open(params);
        } else if (!strcmp(method, "textDocument/didChange")) {
            did_change(params);
        } else if (!strcmp(method, "textDocument/didClose")) {
            did_close(params);
        } else if (!strcmp(method, "cereal/waitIdle") && id) {
            /* answers once no build is queued or running: everything
             * published so far is current (a barrier for tests) */
            size_t i;
            bool busy;
            mutex_lock(&S.m);
            do {
                busy = S.queue.len > 0;
                for (i = 0; i < S.units.len && !busy; i++)
                    busy = S.units.data[i]->building;
                if (busy)
                    cond_wait(&S.done, &S.m);
            } while (busy && !S.stop);
            mutex_unlock(&S.m);
            respond_null(id);
        } else if (id) {
            int i;
            for (i = 0; REQS[i].method; i++)
                if (!strcmp(method, REQS[i].method))
                    break;
            if (REQS[i].method)
                handle_request(id, REQS[i].kind, REQS[i].cidx, params);
            else
                respond_error(id, -32601, "method not found");
        }
        /* other notifications ($/cancelRequest, didSave, initialized, ...)
         * need nothing: requests are answered synchronously */
        free(body);
        arena_free(&a);
    }
    mutex_lock(&S.m);
    S.stop = true;
    {
        size_t i; /* a running build or check need not finish */
        for (i = 0; i < S.units.len; i++)
            atomic_store_u32(&S.units.data[i]->cancel, 1);
    }
    cond_broadcast(&S.work);
    cond_broadcast(&S.done);
    mutex_unlock(&S.m);
    pthread_join(S.builder, NULL);
    {
        size_t i;
        for (i = 0; i < S.units.len; i++) {
            snapshot_release(S.units.data[i]->snap);
            cell_cache_free(&S.units.data[i]->cells);
            interner_release(S.units.data[i]->in);
            free(S.units.data[i]->main);
            free(S.units.data[i]);
        }
        for (i = 0; i < S.docs.len; i++)
            doc_free(S.docs.data[i]);
        vec_free(&S.units);
        vec_free(&S.docs);
        vec_free(&S.queue);
    }
    lsp_forget_tokens(NULL);
    if (S.initialized)
        config_free(&S.cfg);
    pool_free(&S.pool);
    return rc;
}
