/* index.c - the LSP-facing model of a translation unit's macros. */
#include "index.h"
#include "json.h"
#include "cell.h"

#include <string.h>

/* ---- recording ------------------------------------------------------ */

static bool loc_is_system(Index *ix, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(ix->sm, loc);
    return !f || f->system_header || f->kind != SF_USER;
}

static void add_ref(Index *ix, Ident *name, Macro *m, SrcLoc loc, uint32_t len,
                    RefKind kind, unsigned flags, Expansion *e)
{
    IdxRef r;
    if (!loc)
        return;
    memset(&r, 0, sizeof r);
    r.name = name;
    r.macro = m && m->alias_of ? m->alias_of : m; /* pop_macro version */
    r.loc = loc;
    r.len = len;
    r.kind = kind;
    r.flags = flags | (loc_is_system(ix, loc) ? IREF_SYSTEM : 0);
    r.exp = e;
    if (ix->refs.len && (!e || !vec_last(&ix->refs).exp ||
                         e->id < vec_last(&ix->refs).exp->id))
        ix->refs_by_id = -1;
    vec_push(&ix->refs, r);
    ix->sorted = false;
}

static IdxExp *exp_of(Index *ix, Expansion *e)
{
    while (ix->exps.len <= e->id)
        vec_push(&ix->exps, NULL);
    if (!ix->exps.data[e->id]) {
        IdxExp *x = NEW(ix->arena, IdxExp);
        x->e = e;
        ix->exps.data[e->id] = x;
    }
    return ix->exps.data[e->id];
}

static void on_expand(void *ctx, const Expansion *ce, const TokSpan *args,
                      int nargs)
{
    Index *ix = ctx;
    Expansion *e = (Expansion *)ce;
    IdxExp *x;
    unsigned flags = 0;
    SrcLoc loc;
    int i;
    if (!e)
        return;
    x = exp_of(ix, e);
    loc = e->name_loc;
    x->root = ix->pp->expansions.data[e->root];
    x->depth = e->depth;
    x->nargs = nargs;
    if (nargs) {
        x->args = NEW_ARRAY(ix->arena, char *, nargs);
        for (i = 0; i < nargs; i++) {
            uint32_t k;
            x->args[i] = tokens_str(ix->pp, args[i]);
            for (k = 0; k < args[i].n; k++)
                if (args[i].t[k].kind == TK_IDENT)
                    vec_push(&x->arg_names, pp_ident(ix->pp, &args[i].t[k]));
        }
    }
    if (e->name_flags & TF_ORIGIN_BODY)
        flags |= IREF_IN_BODY;
    if (e->name_flags & TF_ORIGIN_ARG)
        flags |= IREF_FROM_ARG;
    if (e->name_flags & TF_PASTED)
        flags |= IREF_PASTED;
    x->key = pp_event_key(ix->pp);
    /* one ref per (location, macro) for body and ## refs: see ensure_sorted */
    add_ref(ix, e->macro->name, e->macro, loc,
            (flags & IREF_PASTED) ? 0 : e->macro->name->len, REF_EXPANSION,
            flags, e);
}

static void on_macro_ref(void *ctx, Ident *id, Macro *m, const Tok *tok,
                         RefKind kind)
{
    Index *ix = ctx;
    if (kind == REF_EXPANSION)
        return;
    add_ref(ix, id, m, tok->loc, tok->len, kind,
            (tok->flags & TF_ORIGIN_BODY) ? IREF_IN_BODY : 0, NULL);
}

static void on_define(void *ctx, Macro *m, Macro *replaced)
{
    Index *ix = ctx;
    uint32_t i;
    (void)replaced;
    if (m->predefined)
        return;
    for (i = 0; i < m->body_len; i++) {
        const Tok *t = &m->body[i], *prev = i ? &m->body[i - 1] : NULL;
        if (t->kind != TK_IDENT)
            continue;
        if (t->flags & TF_PARAM) {
            IdxParamRef r;
            r.macro = m;
            r.param = t->punct;
            r.loc = t->loc;
            r.len = t->len;
            vec_push(&ix->params, r);
        } else if (!(prev && (tok_is_punct(prev, P_DOT) ||
                              tok_is_punct(prev, P_ARROW)))) {
            add_ref(ix, tok_ident(ix->pp->in, t), NULL, t->loc, t->len,
                    REF_EXPANSION, IREF_IN_BODY | IREF_STATIC, NULL);
        }
    }
}

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Index *ix = ctx;
    IdxInclusion *inc = NEW(ix->arena, IdxInclusion);
    IdxCheckpoint cp;
    (void)via;
    inc->file = f;
    cp.loc = f->base;
    cp.seq = ix->pp->seq;
    vec_push(&inc->cps, cp);
    vec_push(&ix->inclusions, inc);
    vec_push(&ix->stack, inc);
}

static void on_file_exit(void *ctx, SrcFile *f)
{
    Index *ix = ctx;
    (void)f;
    if (ix->stack.len)
        ix->stack.len--;
}

static void on_checkpoint(void *ctx, SrcLoc loc, uint32_t seq)
{
    Index *ix = ctx;
    IdxCheckpoint cp;
    if (!ix->stack.len)
        return;
    cp.loc = loc;
    cp.seq = seq;
    vec_push(&vec_last(&ix->stack)->cps, cp);
}

static void on_include(void *ctx, const IncludeEvent *ev)
{
    Index *ix = ctx;
    IdxInclude r;
    r.from = ev->from;
    r.to = ev->file;
    r.hash_loc = ev->hash_loc;
    r.name_loc = ev->name_loc;
    r.name_end = ev->name_end;
    r.spelled = ev->spelled;
    r.result = ev->result;
    vec_push(&ix->includes, r);
}

static void on_skipped(void *ctx, SrcLoc b, SrcLoc e)
{
    Index *ix = ctx;
    SrcRange r;
    r.begin = b;
    r.end = e;
    vec_push(&ix->inactive, r);
}

static void on_cond(void *ctx, const CondEvent *ev)
{
    Index *ix = ctx;
    if (ev->kind == COND_IF || ev->kind == COND_IFDEF || ev->kind == COND_IFNDEF) {
        vec_push(&ix->open_blocks, ev->hash_loc);
    } else if (ev->kind == COND_ENDIF && ix->open_blocks.len) {
        IdxBlock b;
        b.begin = vec_pop(&ix->open_blocks);
        b.end = ev->end_loc;
        vec_push(&ix->blocks, b);
    }
}

void index_init(Index *ix, PP *pp)
{
    PPListener l;
    memset(ix, 0, sizeof *ix);
    ix->pp = pp;
    ix->arena = pp->arena;
    ix->sm = pp->sm;
    memset(&l, 0, sizeof l);
    l.ctx = ix;
    l.expand = on_expand;
    l.macro_ref = on_macro_ref;
    l.define = on_define;
    l.file_enter = on_file_enter;
    l.file_exit = on_file_exit;
    l.checkpoint = on_checkpoint;
    l.include = on_include;
    l.skipped = on_skipped;
    l.cond = on_cond;
    pp_add_listener(pp, l);
}

void index_free(Index *ix)
{
    size_t i;
    for (i = 0; i < ix->inclusions.len; i++)
        vec_free(&ix->inclusions.data[i]->cps);
    for (i = 0; i < ix->exps.len; i++)
        if (ix->exps.data[i]) {
            sb_free(&ix->exps.data[i]->text);
            vec_free(&ix->exps.data[i]->arg_names);
        }
    vec_free(&ix->refs);
    vec_free(&ix->params);
    vec_free(&ix->exps);
    vec_free(&ix->inclusions);
    vec_free(&ix->stack);
    vec_free(&ix->includes);
    vec_free(&ix->inactive);
    vec_free(&ix->blocks);
    vec_free(&ix->open_blocks);
    vec_free(&ix->prep_kept);
    vec_free(&ix->prep_refs);
    vec_free(&ix->runs);
    for (i = 0; i < ix->cells.len; i++)
        cell_release(ix->cells.data[i].cell);
    vec_free(&ix->cells);
}

/* An output token: part of its file-level expansion's text (hover). */
static void index_token(Index *ix, const Tok *t)
{
    IdxExp *x;
    if (ix->pp->out_root == NO_EXP)
        return;
    x = exp_of(ix, ix->pp->expansions.data[ix->pp->out_root]);
    if (x->text.len && (t->flags & (TF_SPACE | TF_BOL)))
        sb_putc(&x->text, ' ');
    sb_putn(&x->text, pp_text(ix->pp, t), t->len);
}

void index_run(Index *ix)
{
    Tok t;
    while (pp_next(ix->pp, &t))
        index_token(ix, &t);
}

/* ---- parallel runs -------------------------------------------------- *
 * Phase A reaches the main index live: definitions, directives, includes,
 * checkpoints, #if expansions.  Each worker gets its own index listening
 * only to expansions and output tokens.  A join copies a worker's
 * expansions and refs for its slice into the main index; finish then
 * renumbers every expansion in sequential order (by plan item, worker
 * events first at a tie, as for diagnostics), so ids, the expansion list
 * and everything derived from them match a sequential run. */

static int ref_cmp(const void *a, const void *b);
static void dedupe_refs(Index *ix);

static void *ix_fork(void *ctx, PP *wpp)
{
    Index *w = NEW(wpp->arena, Index);
    PPListener l;
    (void)ctx;
    memset(w, 0, sizeof *w);
    w->pp = wpp;
    w->arena = wpp->arena;
    w->sm = wpp->sm;
    memset(&l, 0, sizeof l);
    l.ctx = w;
    l.expand = on_expand;
    pp_add_listener(wpp, l);
    return w;
}

static void ix_token(void *wctx, const Tok *t)
{
    index_token(wctx, t);
}

/* The worker's arena outlives it (the runner hands it to the TU), so its
 * records are joined in place.  prepare (concurrent, per slice) keeps what
 * belongs to the slice and sorts its refs; join just collects. */
static void ix_prepare(void *wctx, uint32_t from, uint32_t to)
{
    Index *w = wctx;
    size_t n = w->exps.len, i;
    bool *kept = xcalloc(n + 1, sizeof *kept);
    for (i = 0; i < n; i++) {
        IdxExp *x = w->exps.data[i];
        IdxJoined j;
        if (!x || x->key < from || x->key >= to)
            continue;
        kept[i] = true;
        /* a tree lies within one slice (stitches are clean), so the
         * parents and roots of kept expansions are kept too */
        j.x = x;
        j.parent = x->e->parent == NO_EXP ? NULL
                                          : w->pp->expansions.data[x->e->parent];
        j.root = w->pp->expansions.data[x->e->root];
        vec_push(&w->prep_kept, j);
    }
    for (i = 0; i < w->refs.len; i++) {
        IdxRef *r = &w->refs.data[i];
        if (r->exp && r->exp->id < n && kept[r->exp->id])
            vec_push(&w->prep_refs, *r);
    }
    /* worker-local expansion ids order like the final ones */
    if (w->prep_refs.len)
        qsort(w->prep_refs.data, w->prep_refs.len, sizeof(IdxRef), ref_cmp);
    for (i = 0; i < n; i++)
        if (kept[i])
            w->exps.data[i] = NULL; /* owned by the main index now */
    free(kept);
}

static void ix_join(void *ctx, void *wctx, uint32_t from, uint32_t to)
{
    Index *ix = ctx;
    (void)from;
    (void)to;
    vec_push(&ix->runs, (Index *)wctx); /* in slice order; finish does it */
    ix->sorted = false;
}

/* Merge the sorted ref runs: phase A's own and each worker's. */
static void merge_refs(Index *ix)
{
    size_t nr = ix->runs.len + 1, total = ix->refs.len, r;
    const IdxRef **cur = xcalloc(nr, sizeof *cur), **end = xcalloc(nr, sizeof *end);
    IdxRef *own = NULL, *out;
    size_t o = 0;
    if (ix->refs.len) {
        own = xmalloc(sizeof(IdxRef) * ix->refs.len);
        memcpy(own, ix->refs.data, sizeof(IdxRef) * ix->refs.len);
        qsort(own, ix->refs.len, sizeof(IdxRef), ref_cmp);
    }
    cur[0] = own;
    end[0] = own ? own + ix->refs.len : NULL;
    for (r = 1; r < nr; r++) {
        Index *w = ix->runs.data[r - 1];
        cur[r] = w->prep_refs.data;
        end[r] = w->prep_refs.data + w->prep_refs.len;
        total += w->prep_refs.len;
    }
    out = xmalloc(sizeof(IdxRef) * (total + 1));
    {
        /* a binary heap of run indexes, by each run's current ref */
        size_t *heap = xmalloc(sizeof(size_t) * (nr + 1)), nh = 0, i;
#define HLESS(a, b) (ref_cmp(cur[heap[a]], cur[heap[b]]) < 0 || \
                     (ref_cmp(cur[heap[a]], cur[heap[b]]) == 0 && \
                      heap[a] < heap[b]))
        for (r = 0; r < nr; r++)
            if (cur[r] != end[r]) {
                size_t k = nh++;
                heap[k] = r;
                while (k && HLESS(k, (k - 1) / 2)) {
                    size_t t = heap[k];
                    heap[k] = heap[(k - 1) / 2];
                    heap[(k - 1) / 2] = t;
                    k = (k - 1) / 2;
                }
            }
        while (nh) {
            size_t top = heap[0];
            out[o++] = *cur[top]++;
            if (cur[top] == end[top])
                heap[0] = heap[--nh];
            for (i = 0;;) { /* sift down */
                size_t l = 2 * i + 1, m = i, t;
                if (l < nh && HLESS(l, m))
                    m = l;
                if (l + 1 < nh && HLESS(l + 1, m))
                    m = l + 1;
                if (m == i)
                    break;
                t = heap[i];
                heap[i] = heap[m];
                heap[m] = t;
                i = m;
            }
        }
#undef HLESS
        free(heap);
    }
    vec_free(&ix->refs);
    ix->refs.data = out;
    ix->refs.len = o;
    ix->refs.cap = total + 1;
    free(own);
    free(cur);
    free(end);
    dedupe_refs(ix);
    vec_free(&ix->runs);
    ix->sorted = true;
}

/* Sequential order is by plan item, a worker's expansions before phase
 * A's at a tie.  The workers' runs are key-sorted and consecutive (a
 * worker's plan position only grows; slices are joined in order), phase
 * A's list likewise and short; so a worker expansion's final id is its
 * position among the workers' plus the number of phase A expansions with
 * a smaller key, and a phase A expansion's the mirror image.  Ids are
 * assigned per run concurrently, then links are rewritten concurrently
 * (every write goes to a distinct slot). */
typedef struct RenumJob {
    Index *ix;
    Index *run;
    size_t base;               /* workers' expansions before this run */
    const uint32_t *akeys;     /* phase A keys, sorted */
    size_t na;
    int pass;
} RenumJob;

static size_t count_below(const uint32_t *k, size_t n, uint32_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static void renum_run(void *arg)
{
    RenumJob *j = arg;
    PP *pp = j->ix->pp;
    size_t i, below = 0;
    uint32_t last = 0;
    for (i = 0; i < j->run->prep_kept.len; i++) {
        IdxJoined *e = &j->run->prep_kept.data[i];
        IdxExp *x = e->x;
        if (j->pass == 0) {
            if (i == 0 || x->key != last) {
                below = count_below(j->akeys, j->na, x->key);
                last = x->key;
            }
            x->e->id = (uint32_t)(j->base + i + below);
        } else {
            x->e->parent = e->parent ? e->parent->id : NO_EXP;
            x->e->root = e->root->id;
            x->root = e->root;
            pp->expansions.data[x->e->id] = x->e;
            j->ix->exps.data[x->e->id] = x;
        }
    }
}

static void ix_finish(void *ctx, ThreadPool *pool)
{
    Index *ix = ctx;
    PP *pp = ix->pp;
    VEC(IdxExp *) a = {0};
    VEC(Expansion *) aparent = {0}, aroot = {0};
    uint32_t *akeys;
    RenumJob *jobs;
    size_t i, nw = 0, total, r;
    int pass;
    for (i = 0; i < ix->exps.len; i++) /* phase A: #if and friends */
        if (ix->exps.data[i]) {
            IdxExp *x = ix->exps.data[i];
            vec_push(&a, x);
            vec_push(&aparent, x->e->parent == NO_EXP
                                   ? NULL : pp->expansions.data[x->e->parent]);
            vec_push(&aroot, pp->expansions.data[x->e->root]);
        }
    akeys = xmalloc(sizeof *akeys * (a.len + 1));
    for (i = 0; i < a.len; i++)
        akeys[i] = a.data[i]->key;
    jobs = xcalloc(ix->runs.len + 1, sizeof *jobs);
    for (r = 0; r < ix->runs.len; r++) {
        jobs[r].ix = ix;
        jobs[r].run = ix->runs.data[r];
        jobs[r].base = nw;
        jobs[r].akeys = akeys;
        jobs[r].na = a.len;
        nw += ix->runs.data[r]->prep_kept.len;
    }
    total = nw + a.len;
    /* phase A ids: position plus the workers' expansions with key <= own */
    for (i = 0; i < a.len; i++) {
        size_t upto = 0; /* workers' expansions with key <= akeys[i] */
        for (r = 0; r < ix->runs.len; r++) {
            Index *w = ix->runs.data[r];
            size_t n = w->prep_kept.len, lo = 0, hi = n;
            while (lo < hi) {
                size_t mid = lo + (hi - lo) / 2;
                if (w->prep_kept.data[mid].x->key <= akeys[i])
                    lo = mid + 1;
                else
                    hi = mid;
            }
            upto += lo;
            if (lo < n)
                break; /* later runs have larger keys */
        }
        a.data[i]->e->id = (uint32_t)(i + upto);
    }
    vec_free(&pp->expansions);
    vec_free(&ix->exps);
    pp->expansions.data = xcalloc(total + 1, sizeof(Expansion *));
    pp->expansions.len = pp->expansions.cap = total;
    ix->exps.data = xcalloc(total + 1, sizeof(IdxExp *));
    ix->exps.len = ix->exps.cap = total;
    for (pass = 0; pass < 2; pass++) {
        for (r = 0; r < ix->runs.len; r++)
            jobs[r].pass = pass;
        if (pool && ix->runs.len > 1) {
            JobGroup g;
            group_init(&g);
            for (r = 0; r < ix->runs.len; r++)
                pool_submit(pool, &g, renum_run, &jobs[r]);
            group_wait(pool, &g);
            group_free(&g);
        } else {
            for (r = 0; r < ix->runs.len; r++)
                renum_run(&jobs[r]);
        }
        if (pass == 1)
            for (i = 0; i < a.len; i++) {
                IdxExp *x = a.data[i];
                x->e->parent = aparent.data[i] ? aparent.data[i]->id : NO_EXP;
                x->e->root = aroot.data[i]->id;
                x->root = aroot.data[i];
                pp->expansions.data[x->e->id] = x->e;
                ix->exps.data[x->e->id] = x;
            }
    }
    free(jobs);
    free(akeys);
    vec_free(&a);
    vec_free(&aparent);
    vec_free(&aroot);
    merge_refs(ix); /* ids are final now */
}

/* ---- cells ---------------------------------------------------------- *
 * A slice's expansions and their refs, relative to the cell.  Expansions
 * are numbered within the cell (a tree never leaves its slice). */
typedef struct CExp {
    uint32_t key, parent, root, seq_item, macro;
    uint16_t depth, name_flags;
    CLoc name_loc, end_loc;
    int nargs;
    const char **args;
    const char *text;
    uint32_t text_len;
    struct Ident **arg_names;
    uint32_t narg_names;
} CExp;

typedef struct CRef {
    struct Ident *name;
    uint32_t macro, len, exp;
    CLoc loc;
    uint8_t kind;
    uint8_t flags;
} CRef;

/* Observed calls: expansions whose name came from another expansion,
 * per (parent's definition, child's definition, the name's origin flags),
 * in the order of their first expansion. */
typedef struct CEdge {
    uint32_t parent, child;     /* encoded definitions */
    uint16_t flags;             /* TF_ORIGIN_ARG | TF_ORIGIN_BODY | TF_PASTED */
    uint32_t count;
    uint32_t first;             /* local id of the first such expansion */
} CEdge;

typedef struct IxBlob {
    CExp *exps;
    uint32_t nexps;
    CRef *refs;                 /* by location (CLoc), then as ref_cmp */
    uint32_t nrefs;
    uint32_t *by_name;          /* ref indexes by name id, then position */
    CEdge *edges;
    uint32_t nedges;
    uint32_t max_len;           /* longest ref */
} IxBlob;

static int cref_cmp(const void *a, const void *b)
{
    const CRef *x = a, *y = b;
    if (x->loc != y->loc)
        return x->loc < y->loc ? -1 : 1;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    if (x->exp != y->exp)
        return x->exp < y->exp ? -1 : 1;
    if (x->macro != y->macro)
        return x->macro < y->macro ? -1 : 1;
    if (x->flags != y->flags)
        return x->flags < y->flags ? -1 : 1;
    if (x->len != y->len)
        return x->len < y->len ? -1 : 1;
    return x->name->id < y->name->id ? -1 : x->name->id > y->name->id;
}

static int u64cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

/* The tables queries use, made once per cell. */
static void blob_tables(IxBlob *b, Arena *a)
{
    uint32_t i, *slot, cap = 64;
    uint64_t *keys;
    VEC(CEdge) ed = {0};
    if (b->nrefs > 1)
        qsort(b->refs, b->nrefs, sizeof *b->refs, cref_cmp);
    b->by_name = NEW_ARRAY(a, uint32_t, b->nrefs + 1);
    keys = xmalloc(sizeof(uint64_t) * (b->nrefs + 1));
    for (i = 0; i < b->nrefs; i++) {
        keys[i] = (uint64_t)b->refs[i].name->id << 32 | i;
        if (b->refs[i].len > b->max_len)
            b->max_len = b->refs[i].len;
    }
    if (b->nrefs > 1)
        qsort(keys, b->nrefs, sizeof *keys, u64cmp);
    for (i = 0; i < b->nrefs; i++)
        b->by_name[i] = (uint32_t)keys[i];
    free(keys);
    /* edges: open addressing on (parent, child, flags), UINT32_MAX empty */
    while (cap < 2 * b->nexps)
        cap *= 2;
    slot = xmalloc(sizeof(uint32_t) * cap);
    memset(slot, 0xFF, sizeof(uint32_t) * cap);
    for (i = 0; i < b->nexps; i++) {
        const CExp *c = &b->exps[i];
        uint16_t f = c->name_flags & (TF_ORIGIN_ARG | TF_ORIGIN_BODY |
                                      TF_PASTED);
        uint32_t pm, k, h;
        if (c->parent == UINT32_MAX)
            continue;
        pm = b->exps[c->parent].macro;
        h = (pm * 0x9E3779B1u) ^ (c->macro * 0x85EBCA77u) ^ f;
        for (k = h & (cap - 1); slot[k] != UINT32_MAX; k = (k + 1) & (cap - 1)) {
            CEdge *x = &ed.data[slot[k]];
            if (x->parent == pm && x->child == c->macro && x->flags == f)
                break;
        }
        if (slot[k] != UINT32_MAX) {
            ed.data[slot[k]].count++;
        } else {
            slot[k] = (uint32_t)ed.len;
            CEdge e;
            e.parent = pm;
            e.child = c->macro;
            e.flags = f;
            e.count = 1;
            e.first = i;
            vec_push(&ed, e);
        }
    }
    free(slot);
    b->nedges = (uint32_t)ed.len;
    b->edges = NEW_ARRAY(a, CEdge, ed.len + 1);
    if (ed.len)
        memcpy(b->edges, ed.data, sizeof(CEdge) * ed.len);
    vec_free(&ed);
}

/* Expansion ids [lo, hi) holding every expansion with key in [from, to):
 * keys do not decrease with ids in a worker (NULL slots: none). */
static void exp_range(Index *w, uint32_t from, uint32_t to, size_t *lo,
                      size_t *hi)
{
    size_t n = w->exps.len, a = 0, b = n;
    while (a < b) { /* first id with key >= from (NULLs skipped forward) */
        size_t mid = a + (b - a) / 2, m = mid;
        while (m < b && !w->exps.data[m])
            m++;
        if (m == b || w->exps.data[m]->key >= from)
            b = mid;
        else
            a = m + 1;
    }
    *lo = a;
    b = n;
    while (a < b) {
        size_t mid = a + (b - a) / 2, m = mid;
        while (m < b && !w->exps.data[m])
            m++;
        if (m == b || w->exps.data[m]->key >= to)
            b = mid;
        else
            a = m + 1;
    }
    *hi = a;
}

/* Refs [rlo, rhi) whose expansions have ids in [lo, hi): refs are recorded
 * with their expansions, in id order. */
static void ref_range(Index *w, size_t lo, size_t hi, size_t *rlo,
                      size_t *rhi)
{
    size_t n = w->refs.len, a = 0, b = n;
    if (w->refs_by_id < 0) { /* the caller filters */
        *rlo = 0;
        *rhi = n;
        return;
    }
    while (a < b) {
        size_t mid = a + (b - a) / 2;
        if (w->refs.data[mid].exp->id < lo)
            a = mid + 1;
        else
            b = mid;
    }
    *rlo = a;
    b = n;
    while (a < b) {
        size_t mid = a + (b - a) / 2;
        if (w->refs.data[mid].exp->id < hi)
            a = mid + 1;
        else
            b = mid;
    }
    *rhi = a;
}

static void *ix_encode(void *ctx, void *wctx, uint32_t from, uint32_t to,
                       CellEnc *e)
{
    Index *w = wctx;
    Arena *a = cenc_arena(e);
    IxBlob *b = NEW(a, IxBlob);
    size_t n = w->exps.len, i, k = 0, nr = 0;
    size_t lo, hi, rlo, rhi;
    uint32_t *local;
    (void)ctx;
    /* a worker records in plan order: keys never decrease with ids, and
     * refs follow their expansions' ids */
    exp_range(w, from, to, &lo, &hi);
    local = xmalloc(sizeof(uint32_t) * (hi - lo + 1));
    for (i = lo; i < hi; i++) {
        IdxExp *x = w->exps.data[i];
        local[i - lo] = x && x->key >= from && x->key < to ? (uint32_t)k++
                                                             : UINT32_MAX;
    }
#define LOCAL(id) ((id) >= lo && (id) < hi ? local[(id) - lo] : UINT32_MAX)
    b->nexps = (uint32_t)k;
    b->exps = NEW_ARRAY(a, CExp, k + 1);
    for (i = lo; i < hi; i++) {
        IdxExp *x = w->exps.data[i];
        CExp *c;
        Expansion *ex;
        int j;
        if (local[i - lo] == UINT32_MAX)
            continue;
        c = &b->exps[local[i - lo]];
        ex = x->e;
        c->key = cenc_item(e, x->key);
        c->parent = ex->parent == NO_EXP ? UINT32_MAX : LOCAL(ex->parent);
        c->root = LOCAL(ex->root);
        if (c->root == UINT32_MAX ||
            (ex->parent != NO_EXP && c->parent == UINT32_MAX))
            e->ok = false; /* cannot happen: trees stay in their slice */
        c->seq_item = cenc_item(e, ex->seq_item);
        c->macro = cenc_macro(e, ex->macro);
        c->depth = ex->depth;
        c->name_flags = ex->name_flags;
        c->name_loc = cenc_loc(e, ex->name_loc);
        c->end_loc = cenc_loc(e, ex->end_loc);
        if (ex->in_directive)
            e->ok = false; /* phase A's: never in a worker */
        c->nargs = x->nargs;
        c->args = NEW_ARRAY(a, const char *, x->nargs + 1);
        for (j = 0; j < x->nargs; j++)
            c->args[j] = cenc_str(e, x->args[j]);
        c->text_len = (uint32_t)x->text.len;
        c->text = x->text.len ? arena_strndup(a, x->text.data, x->text.len)
                              : NULL;
        c->narg_names = (uint32_t)x->arg_names.len;
        c->arg_names = NEW_ARRAY(a, struct Ident *, x->arg_names.len + 1);
        if (x->arg_names.len)
            memcpy(c->arg_names, x->arg_names.data,
                   sizeof(struct Ident *) * x->arg_names.len);
    }
    ref_range(w, lo, hi, &rlo, &rhi);
    for (i = rlo; i < rhi; i++)
        if (w->refs.data[i].exp && LOCAL(w->refs.data[i].exp->id) != UINT32_MAX)
            nr++;
    b->refs = NEW_ARRAY(a, CRef, nr + 1);
    for (i = rlo; i < rhi; i++) {
        IdxRef *r = &w->refs.data[i];
        CRef *c;
        if (!(r->exp && LOCAL(r->exp->id) != UINT32_MAX))
            continue;
        c = &b->refs[b->nrefs++];
        c->name = r->name;
        c->macro = cenc_macro(e, r->macro);
        c->len = r->len;
        c->exp = LOCAL(r->exp->id);
        c->loc = cenc_loc(e, r->loc);
        c->kind = (uint8_t)r->kind;
        c->flags = (uint8_t)r->flags;
    }
#undef LOCAL
    blob_tables(b, a);
    free(local);
    (void)n;
    return b;
}

static void *ix_decode(void *ctx, PP *wpp, const void *blob, const CellDec *d)
{
    const IxBlob *b = blob;
    Index *w = NEW(wpp->arena, Index);
    Arena *a = wpp->arena;
    uint32_t i;
    (void)ctx;
    memset(w, 0, sizeof *w);
    w->pp = wpp;
    w->arena = a;
    w->sm = wpp->sm;
    for (i = 0; i < b->nexps; i++) {
        const CExp *c = &b->exps[i];
        Expansion *ex = NEW(a, Expansion);
        ex->id = i;
        ex->parent = c->parent == UINT32_MAX ? NO_EXP : c->parent;
        ex->root = c->root;
        ex->depth = c->depth;
        ex->name_flags = c->name_flags;
        ex->macro = cdec_macro(d, c->macro);
        ex->name_loc = cdec_loc(d, c->name_loc);
        ex->end_loc = cdec_loc(d, c->end_loc);
        ex->seq_item = cdec_item(d, c->seq_item);
        ex->seq = cdec_version(d, c->seq_item);
        vec_push(&wpp->expansions, ex);
    }
    for (i = 0; i < b->nexps; i++) {
        const CExp *c = &b->exps[i];
        IdxExp *x = exp_of(w, wpp->expansions.data[i]);
        int j;
        x->root = wpp->expansions.data[c->root];
        x->depth = c->depth;
        x->key = cdec_item(d, c->key);
        x->nargs = c->nargs;
        if (c->nargs) {
            x->args = NEW_ARRAY(a, char *, c->nargs);
            for (j = 0; j < c->nargs; j++)
                x->args[j] = arena_strdup(a, c->args[j]);
        }
        if (c->text_len)
            sb_putn(&x->text, c->text, c->text_len);
        for (j = 0; j < (int)c->narg_names; j++)
            vec_push(&x->arg_names, c->arg_names[j]);
    }
    for (i = 0; i < b->nrefs; i++) {
        const CRef *c = &b->refs[i];
        IdxRef r;
        memset(&r, 0, sizeof r);
        r.name = c->name;
        r.macro = cdec_macro(d, c->macro);
        r.loc = cdec_loc(d, c->loc);
        r.len = c->len;
        r.kind = (RefKind)c->kind;
        r.flags = c->flags;
        r.exp = wpp->expansions.data[c->exp];
        vec_push(&w->refs, r);
    }
    return w;
}

/* Cells mode: keep the build's cells as they are (see Index). */
static void ix_place(void *ctx, int client, const CellPlace *cells, size_t n,
                     const Plan *plan)
{
    Index *ix = ctx;
    size_t i;
    uint32_t k;
    ix->cells_mode = true;
    ix->nitems = plan->items.len;
    ix->items = NEW_ARRAY(ix->arena, PlanItem, ix->nitems + 1);
    if (ix->nitems)
        memcpy(ix->items, plan->items.data, sizeof(PlanItem) * ix->nitems);
    ix->item_cell = NEW_ARRAY(ix->arena, uint32_t, ix->nitems + 1);
    for (i = 0; i < n; i++) {
        IdxCell c;
        c.cell = cell_retain(cells[i].cell);
        c.blob = cells[i].cell->blob[client];
        c.s = (uint32_t)cells[i].s;
        c.nreads = cells[i].cell->nreads;
        c.rmacro = NEW_ARRAY(ix->arena, Macro *, c.nreads + 1);
        if (c.nreads)
            memcpy(c.rmacro, cells[i].rmacro, sizeof(Macro *) * c.nreads);
        for (k = 0; k < cells[i].cell->nitems; k++)
            ix->item_cell[c.s + k] = (uint32_t)ix->cells.len;
        vec_push(&ix->cells, c);
    }
    ix->max_len = 0;
}

static void ix_release(void *ctx, void *wctx)
{
    (void)ctx;
    index_free(wctx);
}

ParClient index_par_client(Index *ix)
{
    ParClient c;
    memset(&c, 0, sizeof c);
    c.ctx = ix;
    c.fork = ix_fork;
    c.token = ix_token;
    c.prepare = ix_prepare;
    c.join = ix_join;
    c.finish = ix_finish;
    c.release = ix_release;
    c.encode = ix_encode;
    c.decode = ix_decode;
    c.adopts = true;
    if (ix->want_cells)
        c.place = ix_place;
    return c;
}


/* ---- queries -------------------------------------------------------- *
 * Both modes answer through one path: the refs a query needs are gathered
 * (covering a location, with a name, in a file), put in index order and
 * deduplicated, then examined as before.  Materialized, they come from
 * ix->refs, already in order; in cells mode from phase A's refs and the
 * cells (absolute positions from the plan's items, definitions from what
 * each cell's reads found), then sorted. */

typedef VEC(IdxRef) RefVec;

static int name_cmp(const Ident *x, const Ident *y)
{
    /* by spelling: ids depend on what an interner saw before */
    return x == y ? 0 : strcmp(x->str, y->str);
}

/* A total order (the result must not depend on the order refs were
 * recorded in, which differs in parallel runs): location, then kind and
 * definition, then the expansion's sequential number. */
static int ref_order(const IdxRef *x, const IdxRef *y, uint64_t xe,
                     uint64_t ye)
{
    uint32_t xm = x->macro ? x->macro->id : UINT32_MAX,
             ym = y->macro ? y->macro->id : UINT32_MAX;
    if (x->loc != y->loc)
        return x->loc < y->loc ? -1 : 1;
    if (x->kind != y->kind)
        return x->kind < y->kind ? -1 : 1;
    if (xm != ym)
        return xm < ym ? -1 : 1;
    if (xe != ye)
        return xe < ye ? -1 : 1;
    if (x->flags != y->flags)
        return x->flags < y->flags ? -1 : 1;
    if (x->len != y->len)
        return x->len < y->len ? -1 : 1;
    return name_cmp(x->name, y->name);
}

static int ref_cmp(const void *a, const void *b)
{
    const IdxRef *x = a, *y = b;
    return ref_order(x, y, x->exp ? x->exp->id : UINT64_MAX,
                     y->exp ? y->exp->id : UINT64_MAX);
}

/* Cells mode: expansions have no global ids; `order` places them as the
 * ids would (by plan item, a worker's before phase A's at a tie). */
static int ref_qcmp(const void *a, const void *b)
{
    const IdxRef *x = a, *y = b;
    return ref_order(x, y, x->order, y->order);
}

/* A body token or ## result names one macro once per location, however
 * often it was expanded: keep the first expansion's ref (refs sorted). */
static size_t dedupe(IdxRef *v, size_t n)
{
    size_t i, w = 0;
    for (i = 0; i < n; i++) {
        IdxRef *r = &v[i];
        if (w && (r->flags & (IREF_IN_BODY | IREF_PASTED))) {
            size_t k;
            bool dup = false;
            for (k = w; k-- > 0 && v[k].loc == r->loc;)
                if (v[k].macro == r->macro && v[k].kind == r->kind &&
                    (v[k].flags & (IREF_IN_BODY | IREF_PASTED))) {
                    dup = true;
                    break;
                }
            if (dup)
                continue;
        }
        v[w++] = *r;
    }
    return w;
}

static void dedupe_refs(Index *ix)
{
    ix->refs.len = dedupe(ix->refs.data, ix->refs.len);
}

static void ensure_sorted(Index *ix)
{
    if (ix->sorted)
        return;
    if (ix->refs.len)
        qsort(ix->refs.data, ix->refs.len, sizeof(IdxRef), ref_cmp);
    dedupe_refs(ix);
    ix->sorted = true;
}

/* The longest ref anywhere (at least 1): how far back a ref covering a
 * location can start. */
static uint32_t max_len(Index *ix)
{
    size_t i;
    if (ix->max_len)
        return ix->max_len;
    ix->max_len = 1;
    for (i = 0; i < ix->refs.len; i++)
        if (ix->refs.data[i].len > ix->max_len)
            ix->max_len = ix->refs.data[i].len;
    for (i = 0; i < ix->cells.len; i++) {
        const IxBlob *b = ix->cells.data[i].blob;
        if (b->max_len > ix->max_len)
            ix->max_len = b->max_len;
    }
    return ix->max_len;
}

/* ---- cells mode: from cells to absolute refs ---- */

typedef struct IdxSpan {
    SrcLoc begin, end;        /* inclusive */
    uint32_t item;
} IdxSpan;

typedef struct IdxMacRead {
    uint32_t cell, read;
} IdxMacRead;

static SrcLoc cloc_abs(const Index *ix, const IdxCell *c, CLoc l)
{
    switch (CLOC_KIND(l)) {
    case CL_ITEM:
        return ix->items[c->s + CLOC_A(l)].begin + CLOC_OFF(l);
    case CL_MACRO:
        return c->rmacro[CLOC_A(l)]->hash_loc + CLOC_OFF(l);
    case CL_FRAME: {
        const PlanItem *it = &ix->items[c->s + CLOC_A(l)];
        uint32_t j = CLOC_OFF(l) & 0x7FFFFFFFu;
        const PlanFrame *f = (CLOC_OFF(l) & 0x80000000u) ? it->frame->parent
                                                         : it->frame;
        while (j-- && f)
            f = f->parent;
        return f ? f->include_loc : 0;
    }
    default:
        return 0;
    }
}

static void cref_get(const Index *ix, uint32_t ci, uint32_t k, IdxRef *r)
{
    const IdxCell *c = &ix->cells.data[ci];
    const IxBlob *b = c->blob;
    const CRef *x = &b->refs[k];
    memset(r, 0, sizeof *r);
    r->name = x->name;
    r->macro = cell_macro(c->rmacro, x->macro);
    r->loc = cloc_abs(ix, c, x->loc);
    r->len = x->len;
    r->kind = (RefKind)x->kind;
    r->flags = x->flags;
    r->order = ((uint64_t)(c->s + b->exps[x->exp].key) << 33) | x->exp;
    r->cell = ci + 1;
    r->cexp = x->exp;
}

/* Phase A's refs, in a list with cells' refs. */
static IdxRef own_ref(Index *ix, const IdxRef *r)
{
    IdxRef x = *r;
    x.order = UINT64_MAX;
    if (r->exp && r->exp->id < ix->exps.len && ix->exps.data[r->exp->id])
        x.order = ((uint64_t)ix->exps.data[r->exp->id]->key << 33) |
                  (1ull << 32) | r->exp->id;
    return x;
}

static int span_cmp(const void *a, const void *b)
{
    const IdxSpan *x = a, *y = b;
    if (x->begin != y->begin)
        return x->begin < y->begin ? -1 : 1;
    return x->item < y->item ? -1 : x->item > y->item;
}

/* The items' text ranges, as the encoder saw them (cell_enc_begin). */
static void build_spans(Index *ix)
{
    size_t i, n = 0;
    if (ix->spans)
        return;
    ix->spans = NEW_ARRAY(ix->arena, IdxSpan, ix->nitems + 1);
    for (i = 0; i < ix->nitems; i++) {
        const PlanItem *it = &ix->items[i];
        if (!it->begin)
            continue;
        ix->spans[n].begin = it->begin;
        ix->spans[n].end = it->kind == PI_SEG ? it->end : it->begin;
        ix->spans[n].item = (uint32_t)i;
        n++;
    }
    if (n > 1)
        qsort(ix->spans, n, sizeof *ix->spans, span_cmp);
    ix->nspans = n;
}

/* For every definition, the cells whose reads found it (the read that
 * locations inside it are anchored to). */
static void build_mr(Index *ix)
{
    size_t nm = ix->pp->macros.len, total = 0, i;
    uint32_t *fill;
    if (ix->mr_start)
        return;
    ix->mr_start = NEW_ARRAY(ix->arena, uint32_t, nm + 2);
    for (i = 0; i < ix->cells.len; i++) {
        const IdxCell *c = &ix->cells.data[i];
        uint32_t k;
        for (k = 0; k < c->nreads; k++)
            if (c->rmacro[k] && c->cell->reads[k].cls == k) {
                ix->mr_start[c->rmacro[k]->id + 1]++;
                total++;
            }
    }
    for (i = 0; i < nm; i++)
        ix->mr_start[i + 1] += ix->mr_start[i];
    ix->mr = NEW_ARRAY(ix->arena, IdxMacRead, total + 1);
    fill = xcalloc(nm + 1, sizeof *fill);
    for (i = 0; i < ix->cells.len; i++) {
        const IdxCell *c = &ix->cells.data[i];
        uint32_t k;
        for (k = 0; k < c->nreads; k++)
            if (c->rmacro[k] && c->cell->reads[k].cls == k) {
                uint32_t id = c->rmacro[k]->id;
                IdxMacRead *m = &ix->mr[ix->mr_start[id] + fill[id]++];
                m->cell = (uint32_t)i;
                m->read = k;
            }
    }
    free(fill);
}

static bool in_range(SrcLoc loc, SrcLoc b, uint32_t len)
{
    return loc >= b && loc < b + (len ? len : 1);
}

/* A cell's refs anchored in [lo, hi] that cover loc. */
static void cell_range(Index *ix, uint32_t ci, CLoc lo, CLoc hi, SrcLoc loc,
                       RefVec *v)
{
    const IxBlob *b = ix->cells.data[ci].blob;
    uint32_t a = 0, z = b->nrefs;
    while (a < z) {
        uint32_t mid = a + (z - a) / 2;
        if (b->refs[mid].loc < lo)
            a = mid + 1;
        else
            z = mid;
    }
    for (; a < b->nrefs && b->refs[a].loc <= hi; a++) {
        IdxRef r;
        cref_get(ix, ci, a, &r);
        if (in_range(loc, r.loc, r.len))
            vec_push(v, r);
    }
}

/* Refs covering loc: they start in [loc - max_len + 1, loc]. */
static void covering_refs(Index *ix, SrcLoc loc, RefVec *v)
{
    uint32_t w = max_len(ix);
    SrcLoc wlo = loc >= w - 1 ? loc - (w - 1) : 0;
    size_t lo = 0, hi = ix->refs.len, i;
    ensure_sorted(ix);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ix->refs.data[mid].loc < wlo)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < ix->refs.len && ix->refs.data[i].loc <= loc; i++)
        if (in_range(loc, ix->refs.data[i].loc, ix->refs.data[i].len))
            vec_push(v, ix->cells_mode ? own_ref(ix, &ix->refs.data[i])
                                       : ix->refs.data[i]);
    if (!ix->cells_mode)
        return;
    /* text: the items whose range meets the window (ranges of one file
     * are disjoint; a header read twice repeats them) */
    build_spans(ix);
    lo = 0;
    hi = ix->nspans;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ix->spans[mid].begin <= loc)
            lo = mid + 1;
        else
            hi = mid;
    }
    while (lo-- > 0) {
        const IdxSpan *sp = &ix->spans[lo];
        uint32_t ci, rel;
        if (sp->end < wlo) {
            if (lo == 0 || ix->spans[lo - 1].begin != sp->begin)
                break;
            continue;
        }
        ci = ix->item_cell[sp->item];
        rel = sp->item - ix->cells.data[ci].s;
        cell_range(ix, ci, CLOC(CL_ITEM, rel, wlo > sp->begin ? wlo - sp->begin : 0),
                   CLOC(CL_ITEM, rel, loc - sp->begin), loc, v);
    }
    /* definitions' text */
    build_mr(ix);
    for (i = 0; i < ix->pp->macros.len; i++) {
        const Macro *m = ix->pp->macros.data[i];
        uint32_t k;
        if (!m->hash_loc || m->hash_loc > loc || m->end_loc < wlo)
            continue;
        for (k = ix->mr_start[m->id]; k < ix->mr_start[m->id + 1]; k++)
            cell_range(ix, ix->mr[k].cell,
                       CLOC(CL_MACRO, ix->mr[k].read,
                            wlo > m->hash_loc ? wlo - m->hash_loc : 0),
                       CLOC(CL_MACRO, ix->mr[k].read, loc - m->hash_loc), loc,
                       v);
    }
}

/* Refs named `name`. */
static void named_refs(Index *ix, const Ident *name, RefVec *v)
{
    size_t i;
    ensure_sorted(ix);
    for (i = 0; i < ix->refs.len; i++)
        if (ix->refs.data[i].name == name)
            vec_push(v, ix->cells_mode ? own_ref(ix, &ix->refs.data[i])
                                       : ix->refs.data[i]);
    for (i = 0; i < ix->cells.len; i++) {
        const IxBlob *b = ix->cells.data[i].blob;
        uint32_t a = 0, z = b->nrefs;
        while (a < z) {
            uint32_t mid = a + (z - a) / 2;
            if (b->refs[b->by_name[mid]].name->id < name->id)
                a = mid + 1;
            else
                z = mid;
        }
        for (; a < b->nrefs && b->refs[b->by_name[a]].name == name; a++) {
            IdxRef r;
            cref_get(ix, (uint32_t)i, b->by_name[a], &r);
            vec_push(v, r);
        }
    }
}

/* Put gathered refs in index order, one per (location, definition) for
 * body and ## refs. */
static void finish_refs(Index *ix, RefVec *v)
{
    if (v->len > 1)
        qsort(v->data, v->len, sizeof(IdxRef),
              ix->cells_mode ? ref_qcmp : ref_cmp);
    v->len = dedupe(v->data, v->len);
}

size_t index_file_refs(Index *ix, const SrcFile *f, IdxRef **out)
{
    RefVec v = {0};
    size_t i, n;
    ensure_sorted(ix);
#define IN_F(l) ((l) >= f->base && (l) <= f->base + f->size)
    for (i = 0; i < ix->refs.len; i++)
        if (IN_F(ix->refs.data[i].loc))
            vec_push(&v, ix->cells_mode ? own_ref(ix, &ix->refs.data[i])
                                        : ix->refs.data[i]);
    for (i = 0; i < ix->cells.len; i++) {
        const IxBlob *b = ix->cells.data[i].blob;
        uint32_t k;
        for (k = 0; k < b->nrefs; k++) {
            IdxRef r;
            cref_get(ix, (uint32_t)i, k, &r);
            if (IN_F(r.loc))
                vec_push(&v, r);
        }
    }
#undef IN_F
    if (ix->cells_mode)
        finish_refs(ix, &v);
    *out = NEW_ARRAY(ix->arena, IdxRef, v.len + 1);
    if (v.len)
        memcpy(*out, v.data, sizeof(IdxRef) * v.len);
    n = v.len;
    vec_free(&v);
    return n;
}

/* The file-level expansion a ref belongs to, for hover. */
static IdxExp *ref_top(Index *ix, const IdxRef *r)
{
    const IdxCell *c;
    const IxBlob *b;
    const CExp *x;
    IdxExp *t;
    if (r->flags & IREF_IN_BODY)
        return NULL;
    if (!r->cell) {
        if (r->exp && r->exp->parent == NO_EXP && r->exp->id < ix->exps.len)
            return ix->exps.data[r->exp->id];
        return NULL;
    }
    c = &ix->cells.data[r->cell - 1];
    b = c->blob;
    x = &b->exps[r->cexp];
    if (x->parent != UINT32_MAX)
        return NULL;
    /* made for the query: only what hover and expandMacro read */
    t = NEW(ix->arena, IdxExp);
    t->e = NEW(ix->arena, Expansion);
    t->e->id = NO_EXP;
    t->e->parent = NO_EXP;
    t->e->macro = cell_macro(c->rmacro, x->macro);
    t->e->name_loc = cloc_abs(ix, c, x->name_loc);
    t->e->end_loc = cloc_abs(ix, c, x->end_loc);
    t->e->name_flags = x->name_flags;
    t->e->depth = x->depth;
    t->root = t->e;
    t->nargs = x->nargs;
    t->args = (char **)x->args;
    t->text.data = arena_strndup(ix->arena, x->text ? x->text : "", x->text_len);
    t->text.len = x->text_len;
    t->text.cap = x->text_len + 1;
    return t;
}

static void add_candidate(IdxTarget *t, Macro *m)
{
    int i;
    for (i = 0; i < t->nmacros; i++)
        if (t->macros[i] == m)
            return;
    if (t->nmacros < (int)ARRAY_LEN(t->macros))
        t->macros[t->nmacros++] = m;
}

/* identifier spelled at loc in the raw buffer */
static Ident *ident_at(Index *ix, SrcLoc loc, SrcRange *r)
{
    SrcFile *f = srcmgr_file_of(ix->sm, loc);
    uint32_t off, b, e;
    if (!f)
        return NULL;
    off = loc - f->base;
#define IDC(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || \
                ((c) >= '0' && (c) <= '9') || (c) == '_')
    b = e = off;
    while (b > 0 && IDC(f->buf[b - 1]))
        b--;
    while (e < f->size && IDC(f->buf[e]))
        e++;
#undef IDC
    if (b == e || (f->buf[b] >= '0' && f->buf[b] <= '9'))
        return NULL;
    r->begin = f->base + b;
    r->end = f->base + e;
    return intern_find(ix->pp->in, f->buf + b, e - b); /* never defined if new */
}

uint32_t index_seq_at(Index *ix, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(ix->sm, loc);
    size_t i;
    for (i = 0; i < ix->inclusions.len; i++) {
        IdxInclusion *inc = ix->inclusions.data[i];
        size_t lo = 0, hi = inc->cps.len;
        if (inc->file != f)
            continue;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (inc->cps.data[mid].loc <= loc)
                lo = mid;
            else
                hi = mid;
        }
        return inc->cps.data[lo].seq;
    }
    return ix->pp->seq; /* never entered: end-of-TU state */
}

static bool live_at(const Macro *m, uint32_t seq)
{
    return macro_live_at(m, seq);
}

size_t index_visible(Index *ix, SrcLoc loc, Macro ***out)
{
    uint32_t seq = index_seq_at(ix, loc);
    size_t i, n = 0;
    Macro **v = NEW_ARRAY(ix->arena, Macro *, ix->pp->macros.len + 1);
    for (i = 0; i < ix->pp->macros.len; i++)
        if (live_at(ix->pp->macros.data[i], seq))
            v[n++] = ix->pp->macros.data[i];
    *out = v;
    return n;
}

IdxTarget index_resolve(Index *ix, SrcLoc loc)
{
    IdxTarget t;
    size_t i;
    RefVec cov = {0};
    memset(&t, 0, sizeof t);
    t.param = -1;
    ensure_sorted(ix);

    /* #include operand */
    for (i = 0; i < ix->includes.len; i++) {
        IdxInclude *inc = &ix->includes.data[i];
        if (loc >= inc->name_loc && loc < inc->name_end && inc->to) {
            t.kind = TGT_INCLUDE;
            t.file = inc->to;
            t.range.begin = inc->name_loc;
            t.range.end = inc->name_end;
            return t;
        }
    }

    /* definitions and parameters */
    for (i = 0; i < ix->pp->macros.len; i++) {
        Macro *m = ix->pp->macros.data[i];
        int k;
        if (m->predefined)
            continue;
        if (in_range(loc, m->name_loc, m->name->len)) {
            t.kind = TGT_MACRO;
            t.name = m->name;
            add_candidate(&t, m);
            t.range.begin = m->name_loc;
            t.range.end = m->name_loc + m->name->len;
            return t;
        }
        for (k = 0; k < m->nparams; k++)
            if (in_range(loc, m->param_locs[k], m->params[k]->len)) {
                t.kind = TGT_PARAM;
                t.name = m->params[k];
                t.macros[0] = m;
                t.nmacros = 1;
                t.param = k;
                t.range.begin = m->param_locs[k];
                t.range.end = m->param_locs[k] + m->params[k]->len;
                return t;
            }
    }
    for (i = 0; i < ix->params.len; i++) {
        IdxParamRef *p = &ix->params.data[i];
        if (in_range(loc, p->loc, p->len)) {
            t.kind = TGT_PARAM;
            t.name = p->macro->params[p->param];
            t.macros[0] = p->macro;
            t.nmacros = 1;
            t.param = p->param;
            t.range.begin = p->loc;
            t.range.end = p->loc + p->len;
            return t;
        }
    }

    /* recorded references covering loc, in index order */
    covering_refs(ix, loc, &cov);
    if (ix->cells_mode)
        finish_refs(ix, &cov);
    for (i = 0; i < cov.len; i++) {
        IdxRef *r = &cov.data[i];
        IdxExp *top;
        t.kind = TGT_MACRO;
        t.name = r->name;
        t.range.begin = r->loc;
        t.range.end = r->loc + r->len;
        if (r->macro)
            add_candidate(&t, r->macro);
        if ((top = ref_top(ix, r)) != NULL)
            t.top = top;
    }
    vec_free(&cov);
    if (t.kind == TGT_MACRO && t.nmacros == 0) {
        /* static body ref or undefined name: every definition by name */
        Macro *m;
        for (m = mt_hist(ix->pp->mt, t.name); m; m = m->prev)
            if (!m->builtin)
                add_candidate(&t, m);
        if (!t.nmacros && !mt_hist(ix->pp->mt, t.name) && !t.top) {
            /* not a macro at all (e.g. a function named in a body), unless
             * it is tested with #ifdef: keep those as unresolved refs */
            RefVec nv = {0};
            bool tested = false;
            named_refs(ix, t.name, &nv);
            for (i = 0; i < nv.len; i++)
                if (!(nv.data[i].flags & IREF_STATIC))
                    tested = true;
            vec_free(&nv);
            if (!tested)
                memset(&t, 0, sizeof t);
        }
    }
    if (t.kind != TGT_NONE)
        return t;

    /* plain identifier (inactive code, comments excluded) */
    {
        SrcRange r;
        Ident *id = ident_at(ix, loc, &r);
        Macro **vis;
        size_t n, k;
        if (!id || !mt_hist(ix->pp->mt, id))
            return t;
        t.kind = TGT_MACRO;
        t.name = id;
        t.range = r;
        n = index_visible(ix, loc, &vis);
        for (k = 0; k < n; k++)
            if (vis[k]->name == id)
                add_candidate(&t, vis[k]);
        if (!t.nmacros) {
            Macro *m;
            for (m = mt_hist(ix->pp->mt, id); m; m = m->prev)
                if (!m->builtin)
                    add_candidate(&t, m);
        }
    }
    return t;
}

/* Locations -> an index (open addressing; UINT32_MAX empty). */
typedef struct LocMap {
    SrcLoc *key;
    uint32_t *val;
    size_t cap, n;
} LocMap;

static void locmap_init(LocMap *m, size_t n)
{
    m->cap = 16;
    while (m->cap < 2 * n + 2)
        m->cap *= 2;
    m->key = xmalloc(sizeof(SrcLoc) * m->cap);
    m->val = xmalloc(sizeof(uint32_t) * m->cap);
    memset(m->val, 0xFF, sizeof(uint32_t) * m->cap);
    m->n = 0;
}

static uint32_t *locmap_slot(LocMap *m, SrcLoc k)
{
    size_t i = ((uint32_t)k * 0x9E3779B1u) & (m->cap - 1);
    while (m->val[i] != UINT32_MAX && m->key[i] != k)
        i = (i + 1) & (m->cap - 1);
    m->key[i] = k;
    return &m->val[i];
}

static void locmap_free(LocMap *m)
{
    free(m->key);
    free(m->val);
}

size_t index_references(Index *ix, const IdxTarget *t, IdxRef **out)
{
    VEC(IdxRef) v = {0};
    RefVec cand = {0};
    LocMap seen;
    size_t i;
    int k;
    IdxRef r;
    ensure_sorted(ix);
    memset(&r, 0, sizeof r);
    if (t->kind == TGT_PARAM) {
        Macro *m = t->macros[0];
        r.name = t->name;
        r.macro = m;
        r.loc = m->param_locs[t->param];
        r.len = t->name->len;
        r.kind = REF_EXPANSION;
        vec_push(&v, r);
        for (i = 0; i < ix->params.len; i++) {
            IdxParamRef *p = &ix->params.data[i];
            if (p->macro == m && p->param == t->param) {
                r.loc = p->loc;
                r.len = p->len;
                r.flags = IREF_IN_BODY;
                vec_push(&v, r);
            }
        }
    } else if (t->kind == TGT_MACRO) {
        for (k = 0; k < t->nmacros; k++) {
            Macro *m = t->macros[k];
            r.name = m->name;
            r.macro = m;
            r.loc = m->name_loc;
            r.len = m->name->len;
            r.kind = REF_EXPANSION;
            r.flags = 0;
            vec_push(&v, r);
        }
        /* candidates: refs spelled like the target or its definitions */
        named_refs(ix, t->name, &cand);
        for (k = 0; k < t->nmacros; k++) {
            int j;
            bool named = t->macros[k]->name == t->name;
            for (j = 0; j < k && !named; j++)
                named = t->macros[j]->name == t->macros[k]->name;
            if (!named)
                named_refs(ix, t->macros[k]->name, &cand);
        }
        finish_refs(ix, &cand);
        locmap_init(&seen, v.len + cand.len);
        for (i = 0; i < v.len; i++)
            *locmap_slot(&seen, v.data[i].loc) = 0;
        for (i = 0; i < cand.len; i++) {
            IdxRef *x = &cand.data[i];
            bool match = false;
            if (x->macro) {
                for (k = 0; k < t->nmacros; k++)
                    if (x->macro == t->macros[k])
                        match = true;
            } else if (x->name == t->name) {
                match = true; /* undefined-at-the-time or static textual */
            }
            if (!match)
                continue;
            /* a static body ref that was also recorded dynamically */
            if ((x->flags & IREF_STATIC) &&
                *locmap_slot(&seen, x->loc) != UINT32_MAX)
                continue;
            *locmap_slot(&seen, x->loc) = 0;
            vec_push(&v, *x);
        }
        locmap_free(&seen);
        vec_free(&cand);
    }
    /* one entry per location; a resolved (dynamic) ref beats the static
     * textual one recorded at #define time */
    {
        size_t w = 0;
        LocMap kept; /* location -> the entry kept for it (len > 0) */
        locmap_init(&kept, v.len);
        for (i = 0; i < v.len; i++) {
            uint32_t *slot = v.data[i].len
                                 ? locmap_slot(&kept, v.data[i].loc) : NULL;
            if (slot && *slot != UINT32_MAX) {
                IdxRef *j = &v.data[*slot];
                if ((j->flags & IREF_STATIC) &&
                    !(v.data[i].flags & IREF_STATIC))
                    *j = v.data[i];
                continue;
            }
            if (slot)
                *slot = (uint32_t)w;
            v.data[w++] = v.data[i];
        }
        locmap_free(&kept);
        v.len = w;
    }
    *out = NEW_ARRAY(ix->arena, IdxRef, v.len + 1);
    if (v.len)
        memcpy(*out, v.data, sizeof(IdxRef) * v.len);
    i = v.len;
    vec_free(&v);
    return i;
}

/* ---- call hierarchy ------------------------------------------------ */

static bool lives_overlap(const Macro *a, const Macro *b)
{
    return a->def_seq < b->undef_seq && b->def_seq < a->undef_seq;
}

static const Macro *canon(const Macro *m)
{
    return m->alias_of ? m->alias_of : m;
}

static bool observed_edge(uint16_t name_flags)
{
    return !(name_flags & TF_ORIGIN_ARG) &&
           (name_flags & (TF_ORIGIN_BODY | TF_PASTED));
}

/* Expansions of `to` whose name came from the body of an expansion of
 * `from` (spelled there or pasted there), not from an argument. */
static unsigned observed_calls(Index *ix, const Macro *from, const Macro *to,
                               bool *pasted_only)
{
    unsigned n = 0, spelled = 0;
    size_t i;
    for (i = 0; i < ix->exps.len; i++) {
        IdxExp *x = ix->exps.data[i];
        Expansion *e, *p;
        if (!x)
            continue;
        e = x->e;
        if (e->parent == NO_EXP || canon(e->macro) != canon(to) ||
            !observed_edge(e->name_flags))
            continue;
        p = ix->pp->expansions.data[e->parent];
        if (canon(p->macro) != canon(from))
            continue;
        n++;
        if (!(e->name_flags & TF_PASTED))
            spelled++;
    }
    for (i = 0; i < ix->cells.len; i++) { /* cells mode */
        const IdxCell *c = &ix->cells.data[i];
        const IxBlob *b = c->blob;
        uint32_t k;
        for (k = 0; k < b->nedges; k++) {
            const CEdge *e = &b->edges[k];
            if (!observed_edge(e->flags) ||
                canon(cell_macro(c->rmacro, e->child)) != canon(to) ||
                canon(cell_macro(c->rmacro, e->parent)) != canon(from))
                continue;
            n += e->count;
            if (!(e->flags & TF_PASTED))
                spelled += e->count;
        }
    }
    if (pasted_only)
        *pasted_only = n && !spelled;
    return n;
}

static bool call_listed(const IdxCall *c, size_t n, const Macro *m)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (c[i].macro && canon(c[i].macro) == canon(m))
            return true;
    return false;
}

/* Names formed by ##: (parent, child) definitions of expansions whose
 * name was pasted, in the order of their first expansion. */
typedef struct Pasted {
    uint64_t order;
    Macro *parent, *child;
} Pasted;

static int pasted_cmp(const void *a, const void *b)
{
    uint64_t x = ((const Pasted *)a)->order, y = ((const Pasted *)b)->order;
    return x < y ? -1 : x > y;
}

static size_t pasted_calls(Index *ix, Pasted **out)
{
    VEC(Pasted) v = {0};
    size_t i;
    for (i = 0; i < ix->exps.len; i++) {
        IdxExp *x = ix->exps.data[i];
        Pasted p;
        if (!x || x->e->parent == NO_EXP || !(x->e->name_flags & TF_PASTED))
            continue;
        p.order = ix->cells_mode ? ((uint64_t)x->key << 33) | (1ull << 32) | i
                                 : i;
        p.parent = ix->pp->expansions.data[x->e->parent]->macro;
        p.child = x->e->macro;
        vec_push(&v, p);
    }
    for (i = 0; i < ix->cells.len; i++) {
        const IdxCell *c = &ix->cells.data[i];
        const IxBlob *b = c->blob;
        uint32_t k;
        for (k = 0; k < b->nedges; k++) {
            const CEdge *e = &b->edges[k];
            Pasted p;
            if (!(e->flags & TF_PASTED))
                continue;
            p.order = ((uint64_t)(c->s + b->exps[e->first].key) << 33) |
                      e->first;
            p.parent = cell_macro(c->rmacro, e->parent);
            p.child = cell_macro(c->rmacro, e->child);
            vec_push(&v, p);
        }
    }
    if (v.len > 1)
        qsort(v.data, v.len, sizeof *v.data, pasted_cmp);
    *out = v.data;
    return v.len;
}

size_t index_callees(Index *ix, const MacroGraph *g, Macro *m, IdxCall **out)
{
    VEC(IdxCall) v = {0};
    const MNode *nd = mgraph_node(g, m);
    Pasted *ps;
    size_t i, np;
    uint32_t k;
    for (k = 0; nd && k < nd->nnames; k++) {
        Ident *name = nd->names[k];
        Macro *d;
        bool any = false;
        for (d = mt_hist(ix->pp->mt, name); d; d = d->prev) {
            IdxCall c;
            if (!lives_overlap(d, m) || call_listed(v.data, v.len, d))
                continue;
            c.name = name;
            c.macro = d;
            c.observed = observed_calls(ix, m, d, NULL);
            c.pasted = false;
            vec_push(&v, c);
            any = true;
        }
        if (!any) {
            IdxCall c;
            memset(&c, 0, sizeof c);
            c.name = name;
            vec_push(&v, c);
        }
    }
    /* names only ## formed */
    np = pasted_calls(ix, &ps);
    for (i = 0; i < np; i++) {
        IdxCall c;
        if (canon(ps[i].parent) != canon(m) ||
            call_listed(v.data, v.len, ps[i].child))
            continue;
        c.name = ps[i].child->name;
        c.macro = ps[i].child;
        c.observed = observed_calls(ix, m, c.macro, &c.pasted);
        vec_push(&v, c);
    }
    free(ps);
    *out = NEW_ARRAY(ix->arena, IdxCall, v.len + 1);
    if (v.len)
        memcpy(*out, v.data, sizeof(IdxCall) * v.len);
    i = v.len;
    vec_free(&v);
    return i;
}

size_t index_callers(Index *ix, const MacroGraph *g, Macro *m, IdxCall **out)
{
    VEC(IdxCall) v = {0};
    Macro *const *users;
    Pasted *ps;
    size_t n, i, np;
    users = mgraph_users(g, m->name, &n);
    for (i = 0; i < n; i++) {
        IdxCall c;
        if (!lives_overlap(users[i], m) || call_listed(v.data, v.len, users[i]))
            continue;
        c.name = users[i]->name;
        c.macro = users[i];
        c.observed = observed_calls(ix, users[i], m, NULL);
        c.pasted = false;
        vec_push(&v, c);
    }
    np = pasted_calls(ix, &ps);
    for (i = 0; i < np; i++) {
        Macro *p = ps[i].parent;
        IdxCall c;
        if (canon(ps[i].child) != canon(m) || call_listed(v.data, v.len, p))
            continue;
        c.name = p->name;
        c.macro = p;
        c.observed = observed_calls(ix, p, m, &c.pasted);
        vec_push(&v, c);
    }
    free(ps);
    *out = NEW_ARRAY(ix->arena, IdxCall, v.len + 1);
    if (v.len)
        memcpy(*out, v.data, sizeof(IdxCall) * v.len);
    i = v.len;
    vec_free(&v);
    return i;
}

/* ---- graph soundness ---------------------------------------------- */

static bool closure_has(const MClosure *c, const Ident *id)
{
    size_t i;
    for (i = 0; i < c->names.len; i++)
        if (c->names.data[i] == id)
            return true;
    return false;
}

size_t index_check_graph(Index *ix, const MacroGraph *g, FILE *out,
                         size_t *checked)
{
    size_t n = ix->exps.len, r, i, bad = 0;
    MScratch sc = {0};
    uint32_t *start, *fill, *by_root;
    if (ix->cells_mode)
        fatal("index_check_graph: needs a materialized index");
    start = xcalloc(n + 2, sizeof *start);
    *checked = 0;
    /* bucket expansions by root (directives inside arguments interleave
     * trees, so ids alone do not group them) */
    for (i = 0; i < n; i++)
        if (ix->exps.data[i])
            start[ix->exps.data[i]->e->root + 1]++;
    for (i = 0; i < n; i++)
        start[i + 1] += start[i];
    by_root = xmalloc(sizeof *by_root * (n + 1));
    fill = xcalloc(n + 1, sizeof *fill);
    for (i = 0; i < n; i++)
        if (ix->exps.data[i]) {
            uint32_t rt = ix->exps.data[i]->e->root;
            by_root[start[rt] + fill[rt]++] = (uint32_t)i;
        }
    for (r = 0; r < n; r++) {
        IdxExp *root = ix->exps.data[r];
        VEC(Ident *) seeds = {0};
        MClosure at_root;
        uint32_t k;
        if (!root || root->e->root != root->e->id)
            continue;
        /* seeds: the name, and everything spelled in the tree's arguments */
        vec_push(&seeds, root->e->macro->name);
        for (k = start[r]; k < start[r + 1]; k++) {
            IdxExp *x = ix->exps.data[by_root[k]];
            size_t j;
            for (j = 0; j < x->arg_names.len; j++)
                vec_push(&seeds, x->arg_names.data[j]);
        }
        mgraph_closure_with(g, &sc, seeds.data, seeds.len, root->e->seq,
                            &at_root);
        for (k = start[r]; k < start[r + 1]; k++) {
            IdxExp *x = ix->exps.data[by_root[k]];
            const char *why = NULL;
            ++*checked;
            if (at_root.open) {
                /* ## may form any name: nothing more to check */
            } else if (x->e->name_flags & TF_PASTED) {
                why = "name formed by ## but the closure is not open";
            } else if (!closure_has(&at_root, x->e->macro->name)) {
                MClosure at_e; /* a directive in the arguments moved on */
                mgraph_closure_with(g, &sc, seeds.data, seeds.len, x->e->seq,
                                    &at_e);
                if (!closure_has(&at_e, x->e->macro->name))
                    why = "not in the closure";
                mclosure_free(&at_e);
            }
            if (why) {
                uint32_t l1 = 0, c1 = 0, l2 = 0, c2 = 0;
                SrcFile *f1 = srcmgr_file_of(ix->sm, x->e->name_loc);
                SrcFile *f2 = srcmgr_file_of(ix->sm, root->e->name_loc);
                if (f1)
                    srcmgr_linecol(f1, x->e->name_loc, &l1, &c1);
                if (f2)
                    srcmgr_linecol(f2, root->e->name_loc, &l2, &c2);
                fprintf(out, "graph: %s:%u:%u: '%s' under '%s' at %s:%u:%u: %s\n",
                        f1 ? f1->name : "?", l1, c1, x->e->macro->name->str,
                        root->e->macro->name->str, f2 ? f2->name : "?", l2, c2,
                        why);
                bad++;
            }
        }
        mclosure_free(&at_root);
        vec_free(&seeds);
    }
    free(start);
    free(fill);
    free(by_root);
    mscratch_free(&sc);
    return bad;
}

SrcFile *index_find_file(Index *ix, const char *path)
{
    char *norm = path_normalize(ix->arena, path);
    size_t i, n = strlen(norm);
    for (i = 0; i < srcmgr_nfiles(ix->sm); i++) {
        SrcFile *f = srcmgr_file(ix->sm, (uint32_t)i);
        size_t m = strlen(f->path);
        if (f->kind == SF_VIRTUAL)
            continue;
        if (!strcmp(f->path, norm))
            return f;
        if (m > n && f->path[m - n - 1] == '/' && !strcmp(f->path + m - n, norm))
            return f;
        if (n > m && norm[n - m - 1] == '/' && !strcmp(norm + n - m, f->path))
            return f;
    }
    return NULL;
}

/* ---- JSON dump ------------------------------------------------------ */

static void jloc(JsonWriter *w, SrcMgr *sm, const char *prefix, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(sm, loc);
    uint32_t line = 0, col = 0;
    char key[32];
    if (f)
        srcmgr_linecol(f, loc, &line, &col);
    if (!*prefix) {
        json_key(w, "file");
        if (f)
            json_str(w, f->name);
        else
            json_null(w);
    }
    sprintf(key, "%sline", prefix);
    json_key(w, key);
    json_int(w, line);
    sprintf(key, "%scol", prefix);
    json_key(w, key);
    json_int(w, col);
}

static const char *ref_kind_name(RefKind k)
{
    switch (k) {
    case REF_EXPANSION: return "expansion";
    case REF_IFDEF: return "ifdef";
    case REF_DEFINED: return "defined";
    case REF_UNDEF: return "undef";
    case REF_PRAGMA: return "pragma";
    case REF_IF_VALUE: return "if-value";
    }
    return "?";
}

void index_dump_json(Index *ix, FILE *out, bool all)
{
    JsonWriter w;
    size_t i;
    PP *pp = ix->pp;
    if (ix->cells_mode)
        fatal("index_dump_json: needs a materialized index");
    ensure_sorted(ix);
    json_init(&w, out);
    w.pretty = true;
    json_begin_object(&w);

    json_key(&w, "macros");
    json_begin_array(&w);
    for (i = 0; i < pp->macros.len; i++) {
        Macro *m = pp->macros.data[i];
        int k;
        if (!all && (m->predefined || !m->file || m->file->kind != SF_USER ||
                     m->file->system_header))
            continue;
        json_begin_object(&w);
        json_key(&w, "id");
        json_int(&w, m->id);
        json_key(&w, "name");
        json_str(&w, m->name->str);
        json_key(&w, "kind");
        json_str(&w, macro_kind_str(m));
        json_key(&w, "signature");
        json_str(&w, macro_signature(ix->arena, m));
        json_key(&w, "body");
        json_str(&w, macro_body_str(ix->pp, m));
        jloc(&w, ix->sm, "", m->name_loc);
        json_key(&w, "params");
        json_begin_array(&w);
        for (k = 0; k < m->nparams; k++) {
            json_begin_object(&w);
            json_key(&w, "name");
            json_str(&w, m->params[k]->str);
            jloc(&w, ix->sm, "", m->param_locs[k]);
            json_end_object(&w);
        }
        json_end_array(&w);
        json_key(&w, "undef");
        if (m->undef_loc) {
            json_begin_object(&w);
            jloc(&w, ix->sm, "", m->undef_loc);
            json_end_object(&w);
        } else {
            json_null(&w);
        }
        json_key(&w, "expansions");
        json_int(&w, m->expansions);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "references");
    json_begin_array(&w);
    for (i = 0; i < ix->refs.len; i++) {
        IdxRef *r = &ix->refs.data[i];
        if (!all && (r->flags & IREF_SYSTEM))
            continue;
        json_begin_object(&w);
        json_key(&w, "name");
        json_str(&w, r->name->str);
        json_key(&w, "macro");
        if (r->macro)
            json_int(&w, r->macro->id);
        else
            json_null(&w);
        json_key(&w, "kind");
        json_str(&w, (r->flags & IREF_STATIC) ? "body-text" : ref_kind_name(r->kind));
        jloc(&w, ix->sm, "", r->loc);
        json_key(&w, "len");
        json_int(&w, r->len);
        json_key(&w, "inBody");
        json_bool(&w, (r->flags & IREF_IN_BODY) != 0);
        json_key(&w, "renamable");
        json_bool(&w, !(r->flags & (IREF_PASTED | IREF_SYSTEM)));
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "expansions");
    json_begin_array(&w);
    for (i = 0; i < ix->exps.len; i++) {
        IdxExp *x = ix->exps.data[i];
        if (!x || x->root != x->e || x->e->in_directive ||
            (!all && loc_is_system(ix, x->e->name_loc)))
            continue;
        json_begin_object(&w);
        json_key(&w, "macro");
        json_int(&w, x->e->macro->id);
        json_key(&w, "name");
        json_str(&w, x->e->macro->name->str);
        jloc(&w, ix->sm, "", x->e->name_loc);
        jloc(&w, ix->sm, "end", x->e->end_loc);
        json_key(&w, "text");
        json_str(&w, sb_cstr(&x->text));
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "inactive");
    json_begin_array(&w);
    for (i = 0; i < ix->inactive.len; i++) {
        SrcRange r = ix->inactive.data[i];
        if (!all && loc_is_system(ix, r.begin))
            continue;
        json_begin_object(&w);
        jloc(&w, ix->sm, "", r.begin);
        jloc(&w, ix->sm, "end", r.end);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "folding");
    json_begin_array(&w);
    for (i = 0; i < ix->blocks.len; i++) {
        IdxBlock b = ix->blocks.data[i];
        if (!all && loc_is_system(ix, b.begin))
            continue;
        json_begin_object(&w);
        jloc(&w, ix->sm, "", b.begin);
        jloc(&w, ix->sm, "end", b.end);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "includes");
    json_begin_array(&w);
    for (i = 0; i < ix->includes.len; i++) {
        IdxInclude *inc = &ix->includes.data[i];
        static const char *const res[] = {"included", "skipped-guard",
                                          "skipped-once", "not-found"};
        if (!all && loc_is_system(ix, inc->hash_loc))
            continue;
        json_begin_object(&w);
        jloc(&w, ix->sm, "", inc->hash_loc);
        json_key(&w, "spelled");
        json_str(&w, inc->spelled);
        json_key(&w, "target");
        if (inc->to)
            json_str(&w, inc->to->name);
        else
            json_null(&w);
        json_key(&w, "result");
        json_str(&w, res[inc->result]);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_end_object(&w);
    fputc('\n', out);
}
