/* par.c - two-phase parallel `-E` (docs/PARALLEL.md).
 *
 * Phase A runs the directives of the whole TU on the calling thread and
 * produces a Plan.  The plan's segments are split into contiguous ranges,
 * one per worker; each worker preprocesses from its first segment with
 * versioned macro lookups, printing into memory and recording a BoundRec
 * at every segment boundary it crosses.
 *
 * A worker's output is trusted from its start only once it agrees with its
 * predecessor: a worker that has reached its end item keeps going until it
 * hits a boundary where both it and the worker owning that item were
 * *clean* (no expansion in flight, nothing buffered), and the owner printed
 * a positioned token after it.  From there the two produce the same tokens,
 * so the predecessor stops and the merge stitches the owner in, re-rendering
 * the one transition (newlines, linemarker, spacing) that depended on the
 * predecessor's printer state.  If no such boundary exists, the predecessor
 * simply runs on; the result is correct either way.
 *
 * Diagnostics carry the plan item they were reported at; each worker's are
 * kept for the items of its slice and merged with phase A's in item order.
 * Anything plan mode cannot reproduce (__COUNTER__, state-changing
 * _Pragma) marks the worker diverged and the caller reruns sequentially.
 *
 * With a cell cache (no -E output), phase A splits the text at content-
 * defined points, reusable cells found at those points are not rerun
 * (a worker that reaches a reused cell's start cleanly stops there), and
 * workers only cover the rest.  Every slice of the result -- a worker's
 * or a reused cell's, decoded into a worker that never runs -- is joined
 * the same way; the slices the workers ran are split at clean boundaries
 * into new cells for the next build. */
#include "par.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "cell.h"
#include "plan.h"
#include "ppout.h"
#include "thread.h"
#include "toks.h"

#define DEFAULT_CHUNK (64u * 1024)
#define DEFAULT_MIN_BYTES (4u * 1024 * 1024)
#define DEFAULT_WINDOW 256

typedef struct Par Par;
typedef struct Hit Hit;

typedef struct Worker {
    Par *par;
    int idx;
    size_t start_item, end_item;   /* [start, end): the assigned range */
    Arena arena;
    DiagEngine diag;
    PP pp;
    Printer pr;
    OutSink sink;
    /* Boundary records for the first `window` segments from the start:
     * where a predecessor can stitch in.  Preallocated (never moves);
     * nrecs is published with release stores. */
    BoundRec *recs;
    uint32_t nrecs, window;
    bool stopped;                  /* handed off at stop_item */
    size_t stop_item;
    int next;                      /* the worker continuing at stop_item */
    size_t end_off;                /* output length at the end */
    PrintState end_st;             /* printer state there */
    double t_begin, t_end;         /* stats */
    void **wctx;                   /* per client */
    VEC(struct ExpLog) exps;       /* expansions, for Macro counts */
    /* cells */
    CellReads reads;               /* the read set */
    uint32_t ntoks;                /* tokens given so far ... */
    uint64_t thash;                /* ... and their sequence hash */
    VEC(struct TokMark) marks;     /* the same at each clean boundary */
    VEC(uint32_t) cuts;            /* clean cell boundaries crossed */
    Hit *next_hit;                 /* stopped at a reused cell */
    bool cached;                   /* a reused cell: decoded, never run */
} Worker;

/* A reused cell at [s, e). */
struct Hit {
    Cell *cell;
    size_t s, e;
    Macro **rmacro;                /* its reads, resolved in this build */
    Worker w;                      /* decoded */
};

typedef struct Slice {
    Worker *w;
    size_t from, to;
} Slice;

typedef struct TokMark {
    uint32_t item;
    uint32_t ntoks;
    uint64_t hash;
} TokMark;

typedef struct ExpLog {
    uint32_t key;
    uint32_t count;
    Macro *m;
} ExpLog;

struct Par {
    TU *tu;
    Plan plan;
    bool print;                    /* -E output */
    bool linemarkers;
    const ParClient *clients;
    int nclients;
    Worker *w;
    int nw;
    uint32_t abort;                /* atomic: a worker diverged */
    ThreadPool *tp;                /* for the prepare step */
    double t0, t_a, t_b;           /* stats: start, phase A, phase B done */
    /* cells */
    CellCache *cache;
    CellBuild cb;
    Hit *hits;
    size_t nhits;
    Hit **hit_at;                  /* by item: a reused cell starting there */
    double t_cells;                /* stats: lookups done */
    bool placed[CELL_MAX_CLIENTS]; /* clients that took the cells as such */
    TokRegen *toks;                  /* requested: the build's token cells */
    VEC(TokCell) tcells;
    bool tcells_ok;
};

void plan_free(Plan *p)
{
    vec_free(&p->items);
    vec_free(&p->pragmas);
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ---- boundaries ------------------------------------------------------ */

/* Records are appended in item order; n is a published count. */
static BoundRec *find_rec(Worker *w, uint32_t n, size_t item)
{
    uint32_t lo = 0, hi = n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (w->recs[mid].item < item)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n && w->recs[lo].item == item ? &w->recs[lo] : NULL;
}

/* The later worker whose range contains item. */
static Worker *owner_of(Par *P, const Worker *w, size_t item)
{
    Worker *m = NULL;
    int i;
    for (i = w->idx + 1; i < P->nw && P->w[i].start_item <= item; i++)
        m = &P->w[i];
    return m;
}

static bool on_boundary(void *ctx, size_t item, bool clean)
{
    Worker *w = ctx;
    Par *P = w->par;
    if (w->pp.diverged)
        atomic_store_u32(&P->abort, 1);
    if (atomic_load_u32(&P->abort))
        return false;
    if (P->cache && clean && vec_last(&w->marks).item != item) {
        TokMark m;
        m.item = (uint32_t)item;
        m.ntoks = w->ntoks;
        m.hash = w->thash;
        vec_push(&w->marks, m);
    }
    if (w->nrecs < w->window) {
        BoundRec *r = &w->recs[w->nrecs];
        memset(r, 0, sizeof *r);
        r->item = item;
        r->clean = clean;
        if (P->print)
            w->pr.open = r;
        atomic_store_u32(&w->nrecs, w->nrecs + 1);
    } else {
        w->pr.open = NULL;
    }
    if (P->cache && clean && item > w->start_item) {
        if (cell_candidate(&P->plan, item))
            vec_push(&w->cuts, (uint32_t)item);
        if (item >= w->end_item && P->hit_at[item]) {
            /* a reused cell is the fresh run from its start */
            w->stopped = true;
            w->stop_item = item;
            w->next = -1;
            w->next_hit = P->hit_at[item];
            return false;
        }
    }
    if (clean && item >= w->end_item) {
        Worker *m = owner_of(P, w, item);
        BoundRec *mr = m ? find_rec(m, atomic_load_u32(&m->nrecs), item) : NULL;
        /* printing also needs the owner's next token, to re-render the
         * transition from the predecessor's printer state */
        if (mr && mr->clean &&
            (!P->print ||
             (atomic_load_u32(&mr->has_first) && mr->first.f))) {
            w->stopped = true;
            w->stop_item = item;
            w->next = m->idx;
            w->end_off = w->sink.len;
            w->end_st = w->pr.st;
            return false;
        }
    }
    return true;
}

static void run_worker(void *arg)
{
    Worker *w = arg;
    Par *P = w->par;
    Tok t;
    int c;
    w->t_begin = now();
    while (pp_next(&w->pp, &t)) {
        if (w->pp.diverged)
            break;
        if (P->print)
            printer_token(&w->pr, &t);
        for (c = 0; c < P->nclients; c++)
            if (P->clients[c].token && w->wctx[c])
                P->clients[c].token(w->wctx[c], &t);
        if (P->cache && tok_in_stream(&t)) {
            w->ntoks++;
            w->thash = m61_push(w->thash, tok_hash(&w->pp, &t));
        }
    }
    if (w->pp.diverged)
        atomic_store_u32(&w->par->abort, 1);
    if (!w->stopped) {
        w->end_off = w->sink.len;
        w->end_st = w->pr.st;
        if (P->cache) {
            TokMark m;
            m.item = (uint32_t)P->plan.items.len;
            m.ntoks = w->ntoks;
            m.hash = w->thash;
            vec_push(&w->marks, m);
        }
    }
    if (P->cache)
        cell_reads_sort(&w->reads); /* by item, for encoding cells */
    w->t_end = now();
}

/* ---- setup ----------------------------------------------------------- */

static void log_expansion(void *ctx, const Expansion *e, const TokSpan *args,
                          int nargs)
{
    Worker *w = ctx;
    ExpLog l;
    (void)args;
    (void)nargs;
    l.key = pp_event_key(&w->pp);
    l.count = 1;
    l.m = e->macro;
    vec_push(&w->exps, l);
}

static void worker_init(Par *P, Worker *w, int idx, size_t start, size_t end,
                        uint32_t window)
{
    TU *tu = P->tu;
    int c;
    memset(w, 0, sizeof *w);
    w->par = P;
    w->idx = idx;
    w->start_item = start;
    w->end_item = end;
    w->window = start ? window : 0; /* nothing stitches into item 0 */
    w->recs = w->window ? xmalloc(sizeof(BoundRec) * w->window) : NULL;
    arena_init(&w->arena);
    diag_init_worker(&w->diag, &w->arena, &tu->sm, &tu->diag);
    pp_init_worker(&w->pp, &tu->pp, &w->arena, &w->diag);
    if (P->cache) {
        TokMark m;
        cell_reads_init(&w->reads);
        w->pp.reads = &w->reads;
        memset(&m, 0, sizeof m); /* no tokens yet at the start */
        m.item = (uint32_t)start;
        vec_push(&w->marks, m);
    }
    w->pp.on_boundary = on_boundary;
    w->pp.boundary_ctx = w;
    if (P->print)
        printer_init(&w->pr, &w->pp, &w->sink, P->linemarkers);
    if (tu->pp.track != TRACK_NONE) {
        /* Macro.expansions: counted at the join, for kept slices only */
        PPListener l;
        memset(&l, 0, sizeof l);
        l.ctx = w;
        l.expand = log_expansion;
        pp_add_listener(&w->pp, l);
    }
    w->wctx = xcalloc((size_t)P->nclients + 1, sizeof *w->wctx);
    for (c = 0; c < P->nclients; c++)
        if (P->clients[c].fork)
            w->wctx[c] = P->clients[c].fork(P->clients[c].ctx, &w->pp);
    pp_plan_start(&w->pp, &P->plan, start);
}

static bool adopts(const Par *P)
{
    int c;
    for (c = 0; c < P->nclients; c++)
        if (P->clients[c].adopts && !P->placed[c])
            return true;
    return false;
}

static void worker_free(Par *P, Worker *w)
{
    int c;
    for (c = 0; c < P->nclients; c++)
        if (w->wctx[c] && P->clients[c].release)
            P->clients[c].release(P->clients[c].ctx, w->wctx[c]);
    free(w->wctx);
    vec_free(&w->exps);
    cell_reads_free(&w->reads);
    vec_free(&w->cuts);
    vec_free(&w->marks);
    pp_free(&w->pp);
    sink_free(&w->sink);
    if (adopts(P)) {
        /* joined results (expansions, strings) live on in place */
        vec_push(&P->tu->adopted, w->arena);
        memset(&w->arena, 0, sizeof w->arena);
    }
    free(w->recs);
    diag_free(&w->diag);
    arena_free(&w->arena);
}

/* Split the segment bytes evenly; every range starts at a segment. */
static int partition(const Plan *plan, int want, size_t *starts)
{
    uint64_t total = plan->text_bytes, cum = 0;
    int n = 1, k;
    size_t i;
    starts[0] = 0;
    for (i = 0; i < plan->items.len && n < want; i++) {
        const PlanItem *it = &plan->items.data[i];
        if (it->kind != PI_SEG)
            continue;
        k = n;
        if (i > starts[n - 1] && cum * (uint64_t)want >= total * (uint64_t)k)
            starts[n++] = i;
        cum += it->end - it->begin;
    }
    return n;
}

/* ---- merge ----------------------------------------------------------- */

typedef struct DiagRef {
    Diagnostic *d;
    uint32_t key;
    int rank;
    size_t seq;
} DiagRef;

static int diagref_cmp(const void *a, const void *b)
{
    const DiagRef *x = a, *y = b;
    if (x->key != y->key)
        return x->key < y->key ? -1 : 1;
    if (x->rank != y->rank)
        return x->rank - y->rank;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

static Diagnostic *diag_copy(Arena *a, const Diagnostic *s)
{
    Diagnostic *d = NEW(a, Diagnostic);
    size_t i;
    *d = *s;
    memset(&d->notes, 0, sizeof d->notes);
    d->msg = arena_strdup(a, s->msg);
    if (s->fixit)
        d->fixit = arena_strdup(a, s->fixit);
    if (s->ninc) {
        d->inc_chain = NEW_ARRAY(a, SrcLoc, s->ninc);
        memcpy(d->inc_chain, s->inc_chain, sizeof(SrcLoc) * (size_t)s->ninc);
    }
    for (i = 0; i < s->notes.len; i++) {
        DiagNote n = s->notes.data[i];
        n.msg = arena_strdup(a, n.msg);
        vec_push(&d->notes, n);
    }
    return d;
}

/* A worker's diagnostics at a DIR or EXIT item are those of an invocation
 * reading into the directive or the end of the file, which the sequential
 * engine reports before handling either: workers first on ties. */
static void merge_diags(Par *P, const Slice *sl, int nslices)
{
    TU *tu = P->tu;
    VEC(DiagRef) refs;
    size_t i, seq = 0;
    int s;
    memset(&refs, 0, sizeof refs);
    for (i = 0; i < tu->diag.all.len; i++) {
        DiagRef r;
        Diagnostic *d = tu->diag.all.data[i];
        r.d = d;
        r.key = d->key;
        r.rank = 1;
        r.seq = seq++;
        vec_push(&refs, r);
    }
    for (s = 0; s < nslices; s++) {
        Worker *w = sl[s].w;
        for (i = 0; i < w->diag.all.len; i++) {
            Diagnostic *d = w->diag.all.data[i];
            DiagRef r;
            if (d->key < sl[s].from || d->key >= sl[s].to)
                continue;
            r.d = diag_copy(&tu->arena, d);
            r.key = d->key;
            r.rank = 0;
            r.seq = seq++;
            if (d->level >= DL_ERROR)
                tu->diag.nerrors++;
            else if (d->level == DL_WARNING)
                tu->diag.nwarnings++;
            vec_push(&refs, r);
        }
    }
    if (refs.len > 1)
        qsort(refs.data, refs.len, sizeof *refs.data, diagref_cmp);
    tu->diag.all.len = 0;
    for (i = 0; i < refs.len; i++) {
        Diagnostic *d = refs.data[i].d;
        if (d->once) { /* first in sequential order wins */
            size_t k;
            bool dup = false;
            for (k = 0; k < tu->diag.all.len && !dup; k++)
                dup = tu->diag.all.data[k]->once == d->once &&
                      tu->diag.all.data[k]->loc == d->loc;
            if (dup) {
                if (d->level >= DL_ERROR)
                    tu->diag.nerrors--;
                else if (d->level == DL_WARNING)
                    tu->diag.nwarnings--;
                continue;
            }
        }
        vec_push(&tu->diag.all, d);
    }
    vec_free(&refs);
}

typedef struct PrepJob {
    const ParClient *c;
    void *wctx;
    uint32_t from, to;
} PrepJob;

static void run_prepare(void *arg)
{
    PrepJob *j = arg;
    j->c->prepare(j->wctx, j->from, j->to);
}

/* Text-driven results, slice by slice: expansion counts, then clients. */
static void join_clients(Par *P, const Slice *sl, int ns)
{
    int s, c;
    double t0 = now(), t1, t2;
    PrepJob *jobs = xcalloc((size_t)(ns * P->nclients) + 1, sizeof *jobs);
    size_t nj = 0, k;
    for (s = 0; s < ns; s++)
        for (c = 0; c < P->nclients; c++)
            if (P->clients[c].prepare && sl[s].w->wctx[c] && !P->placed[c]) {
                jobs[nj].c = &P->clients[c];
                jobs[nj].wctx = sl[s].w->wctx[c];
                jobs[nj].from = (uint32_t)sl[s].from;
                jobs[nj].to = (uint32_t)sl[s].to;
                nj++;
            }
    if (nj > 1 && P->tp) {
        JobGroup g;
        group_init(&g);
        for (k = 0; k < nj; k++)
            pool_submit(P->tp, &g, run_prepare, &jobs[k]);
        group_wait(P->tp, &g);
        group_free(&g);
    } else {
        for (k = 0; k < nj; k++)
            run_prepare(&jobs[k]);
    }
    free(jobs);
    t1 = now();
    for (s = 0; s < ns; s++) {
        Worker *w = sl[s].w;
        size_t i;
        for (i = 0; i < w->exps.len; i++)
            if (w->exps.data[i].key >= sl[s].from &&
                w->exps.data[i].key < sl[s].to)
                w->exps.data[i].m->expansions += w->exps.data[i].count;
        for (c = 0; c < P->nclients; c++)
            if (P->clients[c].join && w->wctx[c] && !P->placed[c])
                P->clients[c].join(P->clients[c].ctx, w->wctx[c],
                                   (uint32_t)sl[s].from, (uint32_t)sl[s].to);
    }
    t2 = now();
    for (c = 0; c < P->nclients; c++)
        if (P->clients[c].finish)
            P->clients[c].finish(P->clients[c].ctx, P->tp);
    if (getenv("CEREAL_PAR_STATS"))
        fprintf(stderr, "par: prepare %.3fs, join %.3fs, finish %.3fs\n",
                t1 - t0, t2 - t1, now() - t2);
}

static void write_merged(Par *P, FILE *out)
{
    TU *tu = P->tu;
    OutSink o;
    PrintState S;
    Worker *cur = &P->w[0];
    Slice *sl = xmalloc(sizeof(Slice) * (size_t)P->nw);
    int ns = 0;
    memset(&o, 0, sizeof o);
    o.fp = out;
    if (P->print)
        sink_put(&o, cur->sink.buf, cur->end_off);
    S = cur->end_st;
    sl[0].w = cur;
    sl[0].from = 0;
    for (;;) {
        Worker *m;
        BoundRec *r;
        PrintState st;
        sl[ns++].to = cur->stopped ? cur->stop_item : P->plan.items.len;
        if (!cur->stopped)
            break;
        m = &P->w[cur->next];
        if (P->print) {
            r = find_rec(m, m->nrecs, cur->stop_item);
            st = S;
            if (r->events_pre)
                st.pending_flag = r->pre_last;
            print_transition(&tu->sm, tu->in, P->linemarkers, &st, &r->first,
                             &o);
            sink_put(&o, m->sink.buf + r->first_body,
                     m->end_off - r->first_body);
            S = m->end_st;
        }
        sl[ns].from = cur->stop_item;
        sl[ns].w = m;
        cur = m;
    }
    if (P->print && !S.at_bol)
        sink_put(&o, "\n", 1);
    if (getenv("CEREAL_PAR_STATS")) {
        int k;
        fprintf(stderr, "par: A %.3fs B %.3fs merge %.3fs; %zu items, %llu "
                "bytes, %d workers, %d slices:", P->t_a - P->t0,
                P->t_b - P->t_a, now() - P->t_b, P->plan.items.len,
                (unsigned long long)P->plan.text_bytes, P->nw, ns);
        for (k = 0; k < ns; k++)
            fprintf(stderr, " w%d[%zu,%zu)", sl[k].w->idx, sl[k].from,
                    sl[k].to);
        fputc('\n', stderr);
        for (k = 0; k < P->nw; k++) {
            Worker *w = &P->w[k];
            fprintf(stderr, "  w%d [%zu,%zu) stop %zu next %d: %.3f..%.3f, "
                    "%u recs, %zu bytes out\n", k, w->start_item, w->end_item,
                    w->stopped ? w->stop_item : P->plan.items.len, w->next,
                    w->t_begin - P->t_a, w->t_end - P->t_a, w->nrecs,
                    w->sink.len);
        }
        if (getenv("CEREAL_PAR_STATS")[0] == '2') {
            size_t i;
            for (i = 0; i < P->plan.items.len; i++) {
                const PlanItem *it = &P->plan.items.data[i];
                SrcFile *f = it->frame ? it->frame->file : NULL;
                uint32_t l = 0, c = 0;
                if (f && it->begin >= f->base && it->begin < f->base + f->span)
                    srcmgr_linecol(f, it->begin, &l, &c);
                fprintf(stderr, "  %zu %c %s:%u v%u\n", i, "SDEXP"[it->kind],
                        f ? f->name : "-", l, it->version);
            }
        }
    }
    sink_flush(&o);
    sink_free(&o);
    merge_diags(P, sl, ns);
    join_clients(P, sl, ns);
    free(sl);
}

/* ---- cells ----------------------------------------------------------- */

/* Reusable cells, scanning from the start: a lookup at every candidate
 * boundary and at the end of every reused cell. */
static void find_hits(Par *P)
{
    size_t n = P->plan.items.len, p = 0, last_end = SIZE_MAX;
    VEC(Hit) v = {0};
    P->hit_at = xcalloc(n + 1, sizeof *P->hit_at);
    while (p < n) {
        Cell *c = NULL;
        Macro **rm = NULL;
        if (cell_candidate(&P->plan, p) || p == last_end)
            c = cell_lookup(P->cache, &P->cb, p, &rm);
        if (c) {
            Hit h;
            memset(&h, 0, sizeof h);
            h.cell = c;
            h.s = p;
            h.e = p + c->nitems;
            h.rmacro = rm;
            vec_push(&v, h);
            P->cache->last.hits++;
            P->cache->last.hit_items += c->nitems;
            p = last_end = h.e;
        } else {
            p++;
        }
    }
    P->hits = v.data;
    P->nhits = v.len;
    for (p = 0; p < P->nhits; p++)
        P->hit_at[P->hits[p].s] = &P->hits[p];
}

/* Workers for the text no reused cell covers: each gap between reused
 * cells gets workers in proportion to its text, starting at candidate
 * boundaries (or the gap's start). */
static size_t plan_workers(Par *P, int want, size_t **starts_out,
                           size_t **ends_out)
{
    const Plan *plan = &P->plan;
    size_t n = plan->items.len, h = 0, p = 0, i;
    uint64_t dirty = 0;
    VEC(size_t) starts = {0}, ends = {0};
    for (i = 0, p = 0; i < n; i++) {
        if (P->hit_at[i]) {
            i = P->hit_at[i]->e - 1;
            continue;
        }
        if (plan->items.data[i].kind == PI_SEG)
            dirty += plan->items.data[i].end - plan->items.data[i].begin;
    }
    p = 0;
    while (p < n) {
        size_t lo = p, hi = p;
        uint64_t bytes = 0, cum = 0;
        int k, made = 1;
        if (h < P->nhits && P->hits[h].s == p) {
            p = P->hits[h++].e;
            continue;
        }
        while (hi < n && !P->hit_at[hi]) {
            if (plan->items.data[hi].kind == PI_SEG)
                bytes += plan->items.data[hi].end - plan->items.data[hi].begin;
            hi++;
        }
        k = dirty ? (int)((bytes * (uint64_t)want + dirty - 1) / dirty) : 1;
        if (k < 1)
            k = 1;
        vec_push(&starts, lo);
        for (i = lo; i < hi && made < k; i++) {
            const PlanItem *it = &plan->items.data[i];
            if (i > lo && cell_candidate(plan, i) &&
                cum * (uint64_t)k >= bytes * (uint64_t)made) {
                vec_push(&ends, i);
                vec_push(&starts, i);
                made++;
            }
            if (it->kind == PI_SEG)
                cum += it->end - it->begin;
        }
        vec_push(&ends, hi);
        p = hi;
    }
    *starts_out = starts.data;
    *ends_out = ends.data;
    return starts.len;
}

typedef struct CellJob {
    Par *P;
    Worker *w;                     /* encode: a worker's slice */
    size_t from, to;
    Hit *hit;                      /* decode: a reused cell */
    Cell *cell;                    /* encoded (NULL: could not be) */
    Macro **rmacro;                /* its reads in this build */
} CellJob;

static const TokMark *find_mark(const Worker *w, size_t item)
{
    size_t lo = 0, hi = w->marks.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (w->marks.data[mid].item < item)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < w->marks.len && w->marks.data[lo].item == item
               ? &w->marks.data[lo] : NULL;
}

/* The tokens a worker gave for items [x, y), both clean boundaries. */
static bool tok_range(const Worker *w, size_t x, size_t y, uint32_t *n,
                      uint64_t *h)
{
    const TokMark *a = find_mark(w, x), *b = find_mark(w, y);
    if (!a || !b)
        return false;
    *n = b->ntoks - a->ntoks;
    *h = m61_range(b->hash, a->hash, *n);
    return true;
}

static void encode_cell(CellJob *j, size_t x, size_t y)
{
    Par *P = j->P;
    Worker *w = j->w;
    CellEnc e;
    Cell *c;
    size_t i;
    int k;
    cell_enc_begin(&e, &P->cb, &w->reads, x, y);
    if (!tok_range(w, x, y, &e.cell->ntoks, &e.cell->tok_hash))
        e.ok = false;
    for (i = 0; i < w->diag.all.len; i++) {
        const Diagnostic *d = w->diag.all.data[i];
        if (d->key >= x && d->key < y)
            cenc_diag(&e, d);
    }
    /* the log is in plan order: from the first key >= x */
    {
        size_t lo = 0, hi = w->exps.len;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (w->exps.data[mid].key < x)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (i = lo; i < w->exps.len && w->exps.data[i].key < y; i++)
            cenc_exp(&e, w->exps.data[i].key, w->exps.data[i].m);
    }
    for (k = 0; k < P->nclients; k++)
        if (P->clients[k].encode && w->wctx[k])
            e.cell->blob[k] = P->clients[k].encode(P->clients[k].ctx,
                                                   w->wctx[k], (uint32_t)x,
                                                   (uint32_t)y, &e);
    c = cell_enc_end(&e, &j->rmacro);
    j->cell = c;
}

static void decode_hit(Par *P, Hit *h)
{
    Worker *w = &h->w;
    TU *tu = P->tu;
    CellDec d;
    uint32_t i;
    int k;
    memset(w, 0, sizeof *w);
    w->par = P;
    w->idx = -1;
    w->cached = true;
    w->start_item = h->s;
    w->end_item = h->e;
    arena_init(&w->arena);
    diag_init_worker(&w->diag, &w->arena, &tu->sm, &tu->diag);
    pp_init_worker(&w->pp, &tu->pp, &w->arena, &w->diag);
    memset(&d, 0, sizeof d);
    d.b = &P->cb;
    d.cell = h->cell;
    d.s = h->s;
    d.rmacro = h->rmacro;
    d.arena = &w->arena;
    for (i = 0; i < h->cell->ndiags; i++)
        vec_push(&w->diag.all, cdec_diag(&d, &h->cell->diags[i]));
    for (i = 0; i < h->cell->nexps; i++) {
        ExpLog l;
        l.key = (uint32_t)h->s;
        l.count = h->cell->exps[i].count;
        l.m = cdec_macro(&d, h->cell->exps[i].macro);
        vec_push(&w->exps, l);
    }
    w->wctx = xcalloc((size_t)P->nclients + 1, sizeof *w->wctx);
    for (k = 0; k < P->nclients; k++)
        if (P->clients[k].decode && h->cell->blob[k] && !P->placed[k])
            w->wctx[k] = P->clients[k].decode(P->clients[k].ctx, &w->pp,
                                              h->cell->blob[k], &d);
}

static void run_encode_job(void *arg)
{
    CellJob *j = arg;
    encode_cell(j, j->from, j->to);
}

static void run_decode_job(void *arg)
{
    CellJob *j = arg;
    decode_hit(j->P, j->hit);
}

static void run_jobs(Par *P, CellJob *jobs, size_t n, bool hits,
                     void (*fn)(void *))
{
    JobGroup g;
    size_t k;
    group_init(&g);
    for (k = 0; k < n; k++)
        if ((jobs[k].hit != NULL) == hits) {
            if (P->tp)
                pool_submit(P->tp, &g, fn, &jobs[k]);
            else
                fn(&jobs[k]);
        }
    if (P->tp)
        group_wait(P->tp, &g);
    group_free(&g);
}

static bool cancelled(Par *P)
{
    const uint32_t *c = P->tu->pp.cancel;
    return P->tu->pp.halted || (c && atomic_load_u32(c));
}

/* The cell run's merge: the slices in order, reused cells decoded and
 * the workers' slices stored as cells, then joined like any run. */
static void merge_cells(Par *P)
{
    size_t n = P->plan.items.len, p = 0, k;
    VEC(Slice) sl = {0};
    CellJob *jobs;
    size_t nj = 0;
    bool complete = true;          /* every slice has its cell */
    bool covered = true;           /* every slice ran to its end */
    int c;
    VEC(CellPlace) places = {0};
    Worker *next = NULL;           /* where the last worker stitched */
    while (p < n) {
        Slice s;
        Hit *h = P->hit_at[p];
        s.from = p;
        if (!next && h) {
            s.w = &h->w;
            s.to = h->e;
        } else {
            Worker *w = next;
            int i;
            for (i = 0; i < P->nw && !w; i++) /* after a reused cell */
                if (P->w[i].start_item == p)
                    w = &P->w[i];
            if (!w)
                fatal("parallel cells: no worker at item %zu", p);
            s.w = w;
            s.to = w->stopped ? w->stop_item : n;
            next = w->stopped && !w->next_hit ? &P->w[w->next] : NULL;
        }
        vec_push(&sl, s);
        p = s.to;
    }
    {
        size_t cap = sl.len + 1;
        int i;
        for (i = 0; i < P->nw; i++)
            cap += P->w[i].cuts.len;
        jobs = xcalloc(cap, sizeof *jobs);
    }
    for (k = 0; k < sl.len; k++) {
        Slice *s = &sl.data[k];
        CellJob *j;
        if (P->hit_at[s->from] && s->w == &P->hit_at[s->from]->w) {
            j = &jobs[nj++];
            j->P = P;
            j->hit = P->hit_at[s->from];
        } else if (s->w->pp.halted) { /* cancelled, or fatal */
            complete = false;
            covered = false;
        } else {
            /* a cell per stretch between the clean boundaries crossed */
            size_t x = s->from, i;
            for (i = 0; i <= s->w->cuts.len; i++) {
                size_t y = i < s->w->cuts.len ? s->w->cuts.data[i] : s->to;
                if (y <= x || y > s->to)
                    continue;
                j = &jobs[nj++];
                j->P = P;
                j->w = s->w;
                j->from = x;
                j->to = y;
                x = y;
            }
        }
    }
    /* encode first: whether clients can take the cells themselves (place)
     * depends on every slice having one */
    run_jobs(P, jobs, nj, false, run_encode_job);
    if (P->toks) {
        /* the build's token cells: reused ones as stored, the rest from
         * the workers' marks (whether or not they could be cached) */
        P->tcells_ok = covered;
        for (k = 0; k < nj && P->tcells_ok; k++) {
            TokCell tc;
            memset(&tc, 0, sizeof tc);
            if (jobs[k].hit) {
                tc.s = (uint32_t)jobs[k].hit->s;
                tc.e = (uint32_t)jobs[k].hit->e;
                tc.ntoks = jobs[k].hit->cell->ntoks;
                tc.hash = jobs[k].hit->cell->tok_hash;
                tc.reused = true;
            } else {
                tc.s = (uint32_t)jobs[k].from;
                tc.e = (uint32_t)jobs[k].to;
                if (!tok_range(jobs[k].w, jobs[k].from, jobs[k].to, &tc.ntoks,
                               &tc.hash))
                    P->tcells_ok = false;
            }
            vec_push(&P->tcells, tc);
        }
    }
    for (k = 0; k < nj; k++)
        if (!jobs[k].hit && !jobs[k].cell) {
            complete = false;
            P->cache->last.uncacheable++;
        }
    if (cancelled(P))
        complete = false;
    for (c = 0; c < P->nclients; c++)
        P->placed[c] = complete && P->clients[c].place;
    run_jobs(P, jobs, nj, true, run_decode_job);
    for (k = 0; k < nj; k++) {
        CellPlace pl;
        if (jobs[k].hit) {
            pl.cell = jobs[k].hit->cell;
            pl.s = jobs[k].hit->s;
            pl.rmacro = jobs[k].hit->rmacro;
        } else if (jobs[k].cell) {
            pl.cell = jobs[k].cell;
            pl.s = jobs[k].from;
            pl.rmacro = jobs[k].rmacro;
            if (cancelled(P)) { /* results of a cut-short run */
                cell_release(jobs[k].cell);
                jobs[k].cell = NULL;
                continue;
            }
            cell_cache_put(P->cache, cell_retain(jobs[k].cell));
            P->cache->last.stored++;
            P->cache->last.miss_items += jobs[k].cell->nitems;
        } else {
            continue;
        }
        vec_push(&places, pl);
    }
    for (c = 0; c < P->nclients; c++)
        if (P->placed[c])
            P->clients[c].place(P->clients[c].ctx, c, places.data,
                                places.len, &P->plan);
    for (k = 0; k < nj; k++) {
        if (jobs[k].cell)
            cell_release(jobs[k].cell); /* the job's reference */
        free(jobs[k].rmacro);
    }
    vec_free(&places);
    free(jobs);
    if (getenv("CEREAL_PAR_STATS")) {
        const CellStats *st = &P->cache->last;
        size_t cand = 0, i;
        for (i = 0; i < n; i++)
            cand += cell_candidate(&P->plan, i);
        fprintf(stderr, "par: A %.3fs lookups %.3fs B %.3fs; %zu items (%zu "
                "boundaries), %d workers, %zu slices; cells: %zu reused (%zu "
                "items), %zu stored (%zu items), %zu uncacheable%s\n",
                P->t_a - P->t0, P->t_cells - P->t_a, P->t_b - P->t_cells, n,
                cand, P->nw, sl.len,
                st->hits, st->hit_items, st->stored, st->miss_items,
                st->uncacheable, complete ? "; placed" : "");
    }
    {
        double t1 = now(), t2;
        merge_diags(P, sl.data, (int)sl.len);
        t2 = now();
        if (getenv("CEREAL_PAR_STATS"))
            fprintf(stderr, "par: cells coded %.3fs, diagnostics %.3fs\n",
                    t1 - P->t_b, t2 - t1);
    }
    join_clients(P, sl.data, (int)sl.len);
    vec_free(&sl);
}

/* ---- entry ----------------------------------------------------------- */

bool par_worth_it(const char *path, const ParOptions *po)
{
    /* the target is large generated files: don't pay phase A on the way
     * to a sequential run for everything else */
    struct stat sb;
    size_t min = po->min_bytes ? po->min_bytes : DEFAULT_MIN_BYTES;
    if (po->force)
        return true;
    if ((po->threads > 0 ? po->threads : cpu_count()) <= 1)
        return false;
    if (po->size_hint)
        return po->size_hint >= min;
    return stat(path, &sb) == 0 && (size_t)sb.st_size >= min;
}

ParResult par_write_output(TU *tu, const char *path, FILE *out,
                           bool linemarkers, const ParOptions *po)
{
    return par_run(tu, path, out, linemarkers, po, NULL, 0);
}

/* Can the clients' results be cached? */
static bool cells_usable(const ParOptions *po, FILE *out,
                         const ParClient *clients, int nclients)
{
    int c;
    if (!po->cells || out || nclients > CELL_MAX_CLIENTS)
        return false;
    for (c = 0; c < nclients; c++)
        if (clients[c].fork && (!clients[c].encode || !clients[c].decode))
            return false;
    return true;
}

static void free_hits(Par *P)
{
    size_t i;
    for (i = 0; i < P->nhits; i++) {
        if (P->hits[i].w.wctx)
            worker_free(P, &P->hits[i].w);
        free(P->hits[i].rmacro);
    }
    free(P->hits);
    free(P->hit_at);
    P->hits = NULL;
    P->hit_at = NULL;
    P->nhits = 0;
}

ParResult par_run(TU *tu, const char *path, FILE *out, bool linemarkers,
                  const ParOptions *po, const ParClient *clients,
                  int nclients)
{
    Par P;
    ThreadPool pool;
    JobGroup g;
    size_t *starts, *ends = NULL;
    int want, i;
    ParResult res = PAR_DONE;

    if (!par_worth_it(path, po))
        return PAR_FALLBACK;
    memset(&P, 0, sizeof P);
    P.t0 = now();
    P.tu = tu;
    P.print = out != NULL;
    P.linemarkers = linemarkers;
    P.clients = clients;
    P.nclients = nclients;
    if (cells_usable(po, out, clients, nclients)) {
        P.cache = po->cells;
        P.plan.cdc = true;
        P.toks = po->toks;
    }
    P.plan.chunk = po->chunk ? po->chunk : DEFAULT_CHUNK;
    tu->pp.mode = PPM_PHASE_A;
    tu->pp.plan = &P.plan;
    if (!tu_begin(tu, path)) {
        plan_free(&P.plan);
        return PAR_FAILED;
    }
    pp_run_phase_a(&tu->pp, &P.plan);
    P.t_a = now();

    want = po->threads > 0 ? po->threads : cpu_count();
    if (!po->force) {
        size_t min = po->min_bytes ? po->min_bytes : DEFAULT_MIN_BYTES;
        uint64_t per = P.plan.text_bytes / (want > 0 ? (unsigned)want : 1u);
        if (want <= 1 || P.plan.text_bytes < min) {
            plan_free(&P.plan);
            return PAR_FALLBACK;
        }
        if (per < min / 4) /* keep ranges worth a thread */
            want = (int)(P.plan.text_bytes / (min / 4)) + 1;
    }
    if (want < 1)
        want = 1;

    if (P.cache) {
        cell_cache_begin(P.cache, po->cell_config, nclients);
        cell_build_init(&P.cb, &tu->pp, &P.plan);
        find_hits(&P);
        P.t_cells = now();
        P.nw = (int)plan_workers(&P, want, &starts, &ends);
    } else {
        P.t_cells = P.t_a;
        starts = xmalloc(sizeof(size_t) * (size_t)want);
        P.nw = partition(&P.plan, want, starts);
    }
    P.w = xmalloc(sizeof(Worker) * ((size_t)P.nw + 1));
    for (i = 0; i < P.nw; i++)
        worker_init(&P, &P.w[i], i, starts[i],
                    ends ? ends[i]
                         : i + 1 < P.nw ? starts[i + 1] : P.plan.items.len,
                    po->window ? po->window : DEFAULT_WINDOW);
    free(starts);
    free(ends);

    if (P.nw <= 1 && !P.cache) {
        if (P.nw)
            run_worker(&P.w[0]);
    } else {
        ThreadPool *tp = po->pool;
        if (!tp) {
            pool_init(&pool, P.nw > 1 ? P.nw : cpu_count()); /* counts this
                                                                 thread */
            tp = &pool;
        }
        group_init(&g);
        for (i = 0; i < P.nw; i++)
            pool_submit(tp, &g, run_worker, &P.w[i]);
        group_wait(tp, &g);
        group_free(&g);
        P.tp = tp;
    }

    P.t_b = now();
    if (P.abort)
        res = PAR_FALLBACK;
    else if (P.cache)
        merge_cells(&P);
    else
        write_merged(&P, out);
    if (P.tp == &pool)
        pool_free(&pool);
    if (P.cache) {
        free_hits(&P);
        cell_build_free(&P.cb);
        if (res == PAR_DONE && !cancelled(&P))
            cell_cache_end(P.cache); /* else keep what was there */
    }

    if (getenv("CEREAL_PAR_STATS"))
        fprintf(stderr, "par: merge and joins done at %.3fs\n", now() - P.t0);
    for (i = 0; i < P.nw; i++)
        worker_free(&P, &P.w[i]);
    if (getenv("CEREAL_PAR_STATS"))
        fprintf(stderr, "par: workers freed at %.3fs\n", now() - P.t0);
    free(P.w);
    if (P.toks) {
        if (res == PAR_DONE && P.tcells_ok && !cancelled(&P)) {
            P.toks->tu = tu;
            P.toks->plan = P.plan;
            memset(&P.plan, 0, sizeof P.plan);
            P.toks->cells = P.tcells.data;
            P.toks->ncells = P.tcells.len;
        } else {
            vec_free(&P.tcells);
        }
    }
    plan_free(&P.plan);
    tu->pp.plan = NULL;
    return res;
}
