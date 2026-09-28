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
 * _Pragma) marks the worker diverged and the caller reruns sequentially. */
#include "par.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "plan.h"
#include "ppout.h"
#include "thread.h"

#define DEFAULT_CHUNK (64u * 1024)
#define DEFAULT_MIN_BYTES (4u * 1024 * 1024)
#define DEFAULT_WINDOW 256

typedef struct Par Par;

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
} Worker;

typedef struct ExpLog {
    uint32_t key;
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
    }
    if (w->pp.diverged)
        atomic_store_u32(&w->par->abort, 1);
    if (!w->stopped) {
        w->end_off = w->sink.len;
        w->end_st = w->pr.st;
    }
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
    w->window = idx ? window : 0; /* nothing stitches into worker 0 */
    w->recs = w->window ? xmalloc(sizeof(BoundRec) * w->window) : NULL;
    arena_init(&w->arena);
    diag_init(&w->diag, &w->arena, &tu->sm);
    w->diag.cfg = tu->diag.cfg;
    w->diag.werror = tu->diag.werror;
    w->diag.pedantic = tu->diag.pedantic;
    w->diag.pedantic_errors = tu->diag.pedantic_errors;
    w->diag.show_system = tu->diag.show_system;
    w->diag.max_errors = tu->diag.max_errors;
    pp_init_worker(&w->pp, &tu->pp, &w->arena, &w->diag);
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

static void worker_free(Par *P, Worker *w)
{
    int c;
    for (c = 0; c < P->nclients; c++)
        if (w->wctx[c] && P->clients[c].release)
            P->clients[c].release(P->clients[c].ctx, w->wctx[c]);
    free(w->wctx);
    vec_free(&w->exps);
    pp_free(&w->pp);
    sink_free(&w->sink);
    if (P->nclients) {
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
static void merge_diags(Par *P, const size_t *slice_from,
                        const size_t *slice_to, const int *order, int nslices)
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
        Worker *w = &P->w[order[s]];
        for (i = 0; i < w->diag.all.len; i++) {
            Diagnostic *d = w->diag.all.data[i];
            DiagRef r;
            if (d->key < slice_from[s] || d->key >= slice_to[s])
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
static void join_clients(Par *P, const size_t *from, const size_t *to,
                         const int *order, int ns)
{
    int s, c;
    PrepJob *jobs = xcalloc((size_t)(ns * P->nclients) + 1, sizeof *jobs);
    size_t nj = 0, k;
    for (s = 0; s < ns; s++)
        for (c = 0; c < P->nclients; c++)
            if (P->clients[c].prepare && P->w[order[s]].wctx[c]) {
                jobs[nj].c = &P->clients[c];
                jobs[nj].wctx = P->w[order[s]].wctx[c];
                jobs[nj].from = (uint32_t)from[s];
                jobs[nj].to = (uint32_t)to[s];
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
    for (s = 0; s < ns; s++) {
        Worker *w = &P->w[order[s]];
        size_t i;
        for (i = 0; i < w->exps.len; i++)
            if (w->exps.data[i].key >= from[s] && w->exps.data[i].key < to[s])
                w->exps.data[i].m->expansions++;
        for (c = 0; c < P->nclients; c++)
            if (P->clients[c].join && w->wctx[c])
                P->clients[c].join(P->clients[c].ctx, w->wctx[c],
                                   (uint32_t)from[s], (uint32_t)to[s]);
    }
    for (c = 0; c < P->nclients; c++)
        if (P->clients[c].finish)
            P->clients[c].finish(P->clients[c].ctx, P->tp);
}

static void write_merged(Par *P, FILE *out)
{
    TU *tu = P->tu;
    OutSink o;
    PrintState S;
    Worker *cur = &P->w[0];
    size_t *from = xmalloc(sizeof(size_t) * (size_t)P->nw);
    size_t *to = xmalloc(sizeof(size_t) * (size_t)P->nw);
    int *order = xmalloc(sizeof(int) * (size_t)P->nw);
    int ns = 0;
    memset(&o, 0, sizeof o);
    o.fp = out;
    if (P->print)
        sink_put(&o, cur->sink.buf, cur->end_off);
    S = cur->end_st;
    from[0] = 0;
    order[0] = 0;
    for (;;) {
        Worker *m;
        BoundRec *r;
        PrintState st;
        to[ns++] = cur->stopped ? cur->stop_item : P->plan.items.len;
        if (!cur->stopped)
            break;
        m = &P->w[cur->next];
        if (P->print) {
            r = find_rec(m, m->nrecs, cur->stop_item);
            st = S;
            if (r->events_pre)
                st.pending_flag = r->pre_last;
            print_transition(&tu->sm, &tu->in, P->linemarkers, &st, &r->first,
                             &o);
            sink_put(&o, m->sink.buf + r->first_body,
                     m->end_off - r->first_body);
            S = m->end_st;
        }
        from[ns] = cur->stop_item;
        order[ns] = m->idx;
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
            fprintf(stderr, " w%d[%zu,%zu)", order[k], from[k], to[k]);
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
    merge_diags(P, from, to, order, ns);
    join_clients(P, from, to, order, ns);
    free(from);
    free(to);
    free(order);
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

ParResult par_run(TU *tu, const char *path, FILE *out, bool linemarkers,
                  const ParOptions *po, const ParClient *clients,
                  int nclients)
{
    Par P;
    ThreadPool pool;
    JobGroup g;
    size_t *starts;
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

    starts = xmalloc(sizeof(size_t) * (size_t)want);
    P.nw = partition(&P.plan, want, starts);
    P.w = xmalloc(sizeof(Worker) * (size_t)P.nw);
    for (i = 0; i < P.nw; i++)
        worker_init(&P, &P.w[i], i, starts[i],
                    i + 1 < P.nw ? starts[i + 1] : P.plan.items.len,
                    po->window ? po->window : DEFAULT_WINDOW);
    free(starts);

    if (P.nw == 1) {
        run_worker(&P.w[0]);
    } else {
        ThreadPool *tp = po->pool;
        if (!tp) {
            pool_init(&pool, P.nw); /* counts this thread */
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
    else
        write_merged(&P, out);
    if (P.tp == &pool)
        pool_free(&pool);

    if (getenv("CEREAL_PAR_STATS"))
        fprintf(stderr, "par: merge and joins done at %.3fs\n", now() - P.t0);
    for (i = 0; i < P.nw; i++)
        worker_free(&P, &P.w[i]);
    if (getenv("CEREAL_PAR_STATS"))
        fprintf(stderr, "par: workers freed at %.3fs\n", now() - P.t0);
    free(P.w);
    plan_free(&P.plan);
    tu->pp.plan = NULL;
    return res;
}
