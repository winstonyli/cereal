/* cell.c - the cell cache (see cell.h). */
#include "cell.h"

#include <stdlib.h>
#include <string.h>

#include "hash.h"

#define ALIAS_BIT CELL_ALIAS_BIT
#define POISON_MIX 0x5BD1E9955BD1E995ull
#define FP_NONE 1u

/* ---- reads -------------------------------------------------------------- */

void cell_reads_init(CellReads *r)
{
    memset(r, 0, sizeof *r);
    r->cap = 1024;
    r->slot = xcalloc(r->cap, sizeof *r->slot);
}

void cell_reads_free(CellReads *r)
{
    free(r->slot);
    vec_free(&r->list);
    vec_free(&r->line_items);
    memset(r, 0, sizeof *r);
}

static void reads_grow(CellReads *r)
{
    size_t cap = r->cap * 2, i;
    uint64_t *s = xcalloc(cap, sizeof *s);
    for (i = 0; i < r->cap; i++) {
        uint64_t k = r->slot[i];
        size_t j;
        if (!k)
            continue;
        for (j = h64_avalanche(k) & (cap - 1); s[j]; j = (j + 1) & (cap - 1))
            ;
        s[j] = k;
    }
    free(r->slot);
    r->slot = s;
    r->cap = cap;
}

void cell_reads_note(CellReads *r, uint32_t ident, uint32_t item)
{
    uint64_t k = ((uint64_t)item << 32) | ident;
    size_t j;
    uint64_t *memo = &r->recent[ident & (CELL_RECENT - 1)];
    if (*memo == k) /* the same name again in this stretch of text */
        return;
    *memo = k;
    for (j = h64_avalanche(k) & (r->cap - 1); r->slot[j];
         j = (j + 1) & (r->cap - 1))
        if (r->slot[j] == k)
            return;
    r->slot[j] = k;
    vec_push(&r->list, k);
    if (++r->n * 2 > r->cap)
        reads_grow(r);
}

void cell_reads_line(CellReads *r, uint32_t item)
{
    if (!r->line_items.len || vec_last(&r->line_items) != item)
        vec_push(&r->line_items, item);
}

static int u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static int u32_cmp(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

void cell_reads_sort(CellReads *r)
{
    if (r->list.len > 1)
        qsort(r->list.data, r->list.len, sizeof *r->list.data, u64_cmp);
    if (r->line_items.len > 1)
        qsort(r->line_items.data, r->line_items.len,
              sizeof *r->line_items.data, u32_cmp);
}

/* ---- cache ---------------------------------------------------------------- */

void cell_cache_init(CellCache *c)
{
    memset(c, 0, sizeof *c);
}

Cell *cell_retain(Cell *c)
{
    atomic_add_u32(&c->refs, 1);
    return c;
}

void cell_release(Cell *c)
{
    if (!c || atomic_add_u32(&c->refs, (uint32_t)-1) != 1)
        return;
    arena_free(&c->arena);
    free(c);
}

void cell_cache_free(CellCache *c)
{
    size_t i;
    for (i = 0; i < c->nbuckets; i++) {
        Cell *x = c->bucket[i], *nx;
        for (; x; x = nx) {
            nx = x->next;
            cell_release(x);
        }
    }
    free(c->bucket);
    memset(c, 0, sizeof *c);
}

static void cache_insert(CellCache *c, Cell *cell)
{
    size_t b;
    if (c->n + 1 > c->nbuckets) { /* keep chains short */
        size_t nb = c->nbuckets ? c->nbuckets * 2 : 256, i;
        Cell **nbk = xcalloc(nb, sizeof *nbk);
        for (i = 0; i < c->nbuckets; i++) {
            Cell *x = c->bucket[i], *nx;
            for (; x; x = nx) {
                nx = x->next;
                b = x->item_hash[0] & (nb - 1);
                x->next = nbk[b];
                nbk[b] = x;
            }
        }
        free(c->bucket);
        c->bucket = nbk;
        c->nbuckets = nb;
    }
    b = cell->item_hash[0] & (c->nbuckets - 1);
    cell->next = c->bucket[b];
    c->bucket[b] = cell;
    c->n++;
}

void cell_cache_put(CellCache *c, Cell *cell)
{
    cell->gen = c->gen;
    cache_insert(c, cell);
}

void cell_cache_begin(CellCache *c, uint64_t config, int nclients)
{
    if (c->config != config || c->nclients != nclients) {
        cell_cache_free(c);
        c->config = config;
        c->nclients = nclients;
    }
    c->gen++;
    memset(&c->last, 0, sizeof c->last);
}

void cell_cache_end(CellCache *c)
{
    size_t i;
    for (i = 0; i < c->nbuckets; i++) {
        Cell **p = &c->bucket[i];
        while (*p) {
            Cell *x = *p;
            if (x->gen != c->gen) {
                *p = x->next;
                cell_release(x);
                c->n--;
            } else {
                p = &x->next;
            }
        }
    }
    c->last.cells = c->n;
}

/* ---- per build ------------------------------------------------------------ */

typedef struct FrameMemo {
    const PlanFrame *f;
    uint64_t h;
} FrameMemo;

struct CellFrames {
    FrameMemo *slot;
    size_t cap, n;
};

static uint64_t frame_digest(CellBuild *b, const PlanFrame *f);

static uint64_t frame_compute(CellBuild *b, const PlanFrame *f)
{
    uint64_t h = hash64_mix(frame_digest(b, f->parent), 0xF4A3u);
    h = hash64_mix(h, hash64_str(f->file ? f->file->name : NULL, 1));
    h = hash64_mix(h, hash64_str(f->presumed_name, 2));
    h = hash64_mix(h, (uint64_t)f->depth << 8 | (uint64_t)f->system << 1 |
                          (f->file && f->file->system_header));
    h = hash64_mix(h, f->file ? (uint64_t)f->file->kind : 99);
    return h;
}

static uint64_t frame_digest(CellBuild *b, const PlanFrame *f)
{
    struct CellFrames *m = b->frames;
    size_t j;
    uint64_t h;
    if (!f)
        return 0x0F0F0F0Full;
    for (j = h64_avalanche((uint64_t)(uintptr_t)f) & (m->cap - 1); m->slot[j].f;
         j = (j + 1) & (m->cap - 1))
        if (m->slot[j].f == f)
            return m->slot[j].h;
    h = frame_compute(b, f);
    if ((m->n + 1) * 2 > m->cap) {
        size_t cap = m->cap * 2, i;
        FrameMemo *s = xcalloc(cap, sizeof *s);
        for (i = 0; i < m->cap; i++)
            if (m->slot[i].f) {
                size_t k;
                for (k = h64_avalanche((uint64_t)(uintptr_t)m->slot[i].f) &
                         (cap - 1);
                     s[k].f; k = (k + 1) & (cap - 1))
                    ;
                s[k] = m->slot[i];
            }
        free(m->slot);
        m->slot = s;
        m->cap = cap;
    }
    for (j = h64_avalanche((uint64_t)(uintptr_t)f) & (m->cap - 1); m->slot[j].f;
         j = (j + 1) & (m->cap - 1))
        ;
    m->slot[j].f = f;
    m->slot[j].h = h;
    m->n++;
    return h;
}

static const char *loc_text(SrcMgr *sm, SrcLoc b, SrcLoc e, size_t *n)
{
    SrcFile *f = srcmgr_file_of(sm, b);
    if (!f || e < b || e - f->base > f->size) {
        *n = 0;
        return NULL;
    }
    *n = e - b;
    return f->buf + (b - f->base);
}

static uint64_t item_hash(CellBuild *b, const PlanItem *it)
{
    uint64_t h = hash64_mix(0xC311u, (uint64_t)it->kind << 2 |
                                         (uint64_t)it->split << 1 | it->cut);
    h = hash64_mix(h, frame_digest(b, it->frame));
    if (it->kind == PI_SEG) {
        size_t n;
        const char *t = loc_text(b->pp->sm, it->begin, it->end, &n);
        h = hash64_mix(h, hash64(t ? t : "", n, 3));
    } else if (it->kind == PI_PRAGMA) {
        const Tok *t = &b->plan->pragmas.data[it->end];
        h = hash64_mix(h, hash64(tok_text_raw(b->pp->sm, b->pp->in, t),
                                 t->len, 4));
    }
    return h;
}

static uint64_t macro_fp(CellBuild *b, const Macro *m)
{
    uint64_t h = hash64_mix(0x3AC20u, (uint64_t)m->builtin << 8 |
                                          (uint64_t)m->funclike << 4 |
                                          (uint64_t)m->variadic << 3 |
                                          (uint64_t)m->gnu_named_variadic << 2 |
                                          (uint64_t)m->predefined << 1 |
                                          (m->alias_of != NULL));
    SrcFile *f = m->hash_loc ? srcmgr_file_of(b->pp->sm, m->hash_loc) : NULL;
    h = hash64_mix(h, hash64(m->name->str, m->name->len, 5));
    if (f) {
        size_t n;
        const char *t = loc_text(b->pp->sm, m->hash_loc, m->end_loc, &n);
        h = hash64_mix(h, hash64(t ? t : "", n, 6));
        h = hash64_mix(h, hash64_str(f->name, 7));
        h = hash64_mix(h, (uint64_t)f->kind << 1 | f->system_header);
    }
    return h == FP_NONE ? FP_NONE + 1 : h;
}

void cell_build_init(CellBuild *b, PP *pp, Plan *plan)
{
    size_t i;
    memset(b, 0, sizeof *b);
    b->pp = pp;
    b->plan = plan;
    b->frames = xcalloc(1, sizeof *b->frames);
    b->frames->cap = 64;
    b->frames->slot = xcalloc(64, sizeof *b->frames->slot);
    b->item_hash = xmalloc(sizeof(uint64_t) * (plan->items.len + 1));
    for (i = 0; i < plan->items.len; i++)
        b->item_hash[i] = item_hash(b, &plan->items.data[i]);
    b->nfp = pp->macros.len;
    b->fp = xmalloc(sizeof(uint64_t) * (b->nfp + 1));
    b->stamp = xcalloc(b->nfp + 1, sizeof *b->stamp);
    for (i = 0; i < b->nfp; i++)
        b->fp[i] = macro_fp(b, pp->macros.data[i]);
}

void cell_build_free(CellBuild *b)
{
    free(b->item_hash);
    free(b->fp);
    free(b->stamp);
    if (b->frames) {
        free(b->frames->slot);
        free(b->frames);
    }
    memset(b, 0, sizeof *b);
}

bool cell_candidate(const Plan *plan, size_t i)
{
    return i == 0 || (plan->items.data[i].kind == PI_SEG &&
                      plan->items.data[i].cut);
}

/* The presumed line of an item's start (segments; 0 otherwise). */
static uint32_t item_line(CellBuild *b, const PlanItem *it)
{
    SrcFile *f;
    uint32_t line = 0, col;
    if (it->kind != PI_SEG || !it->frame)
        return 0;
    f = srcmgr_file_of(b->pp->sm, it->begin);
    if (!f)
        return 0;
    srcmgr_linecol(f, it->begin, &line, &col);
    return (uint32_t)((int32_t)line + line_adj_delta(it->frame->adj, line));
}

static uint64_t read_val(CellBuild *b, const Ident *id, uint32_t version,
                         Macro **found)
{
    const MacroTab *mt = b->pp->mt;
    Macro *m = macro_at_version(mt, id, version);
    uint32_t ps = mt_slot(mt, id->id)->poison_seq;
    uint64_t v = m ? b->fp[m->id] : FP_NONE;
    if (ps && version >= ps)
        v ^= POISON_MIX;
    *found = m;
    return v;
}

static bool validate(CellBuild *b, const Cell *c, size_t s, Macro **out)
{
    const PlanItem *items = b->plan->items.data;
    uint32_t i;
    if (s + c->nitems > b->plan->items.len ||
        c->at_end != (s + c->nitems == b->plan->items.len) ||
        memcmp(c->item_hash, b->item_hash + s, sizeof(uint64_t) * c->nitems))
        return false;
    if (c->lines)
        for (i = 0; i < c->nitems; i++)
            if (c->lines[i] != item_line(b, &items[s + i]))
                return false;
    if (++b->gen == 0) {
        memset(b->stamp, 0, sizeof *b->stamp * (b->nfp + 1));
        b->gen = 1;
    }
    for (i = 0; i < c->nreads; i++) {
        const CellRead *r = &c->reads[i];
        Macro *m;
        if (read_val(b, r->name, items[s + r->item].version, &m) != r->val)
            return false;
        out[i] = m;
        if (r->cls == UINT32_MAX)
            continue; /* the value says: none */
        if (r->cls == i) { /* first of its definition: must be new here */
            if (b->stamp[m->id] == b->gen)
                return false;
            b->stamp[m->id] = b->gen;
        } else if (out[r->cls] != m) {
            return false;
        }
    }
    return true;
}

Cell *cell_lookup(CellCache *c, CellBuild *b, size_t s, Macro ***out)
{
    Cell *x;
    uint64_t h = b->item_hash[s];
    *out = NULL;
    if (!c->nbuckets)
        return NULL;
    for (x = c->bucket[h & (c->nbuckets - 1)]; x; x = x->next) {
        Macro **m;
        if (x->item_hash[0] != h)
            continue;
        m = xmalloc(sizeof(Macro *) * (x->nreads + 1));
        if (validate(b, x, s, m)) {
            x->gen = c->gen;
            *out = m;
            return x;
        }
        free(m);
    }
    return NULL;
}

/* ---- encoding ------------------------------------------------------------- */

typedef struct CellSpan {
    SrcLoc begin, end;        /* inclusive */
    uint32_t a;
    uint8_t kind;
} CellSpan;

typedef struct CellMacIdx {
    const Macro *m;
    uint32_t idx;
} CellMacIdx;

static int span_cmp(const void *x, const void *y)
{
    const CellSpan *a = x, *b = y;
    if (a->begin != b->begin)
        return a->begin < b->begin ? -1 : 1;
    return a->kind - b->kind;
}

static int mac_cmp(const void *x, const void *y)
{
    uintptr_t a = (uintptr_t)((const CellMacIdx *)x)->m,
              b = (uintptr_t)((const CellMacIdx *)y)->m;
    return a < b ? -1 : a > b;
}

static size_t lower_u64(const uint64_t *v, size_t n, uint64_t key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (v[mid] < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

void cell_enc_begin(CellEnc *e, CellBuild *b, const CellReads *r, size_t s,
                    size_t end)
{
    Cell *c = xcalloc(1, sizeof *c);
    const PlanItem *items = b->plan->items.data;
    size_t lo, hi, i, n;
    VEC(CellSpan) spans = {0};
    VEC(CellMacIdx) macs = {0};
    memset(e, 0, sizeof *e);
    e->b = b;
    e->cell = c;
    e->s = s;
    e->e = end;
    e->ok = end > s && end - s <= CLOC_AMAX;
    c->refs = 1;
    arena_init(&c->arena);
    c->nitems = (uint32_t)(end - s);
    c->item_hash = NEW_ARRAY(&c->arena, uint64_t, c->nitems + 1);
    memcpy(c->item_hash, b->item_hash + s, sizeof(uint64_t) * c->nitems);
    c->at_end = end == b->plan->items.len;

    /* reads with items in [s, end): r->list is sorted by item */
    lo = lower_u64(r->list.data, r->list.len, (uint64_t)s << 32);
    hi = lower_u64(r->list.data, r->list.len, (uint64_t)end << 32);
    n = hi - lo;
    c->nreads = (uint32_t)n;
    c->reads = NEW_ARRAY(&c->arena, CellRead, n + 1);
    e->rmacro = xmalloc(sizeof(Macro *) * (n + 1));
    for (i = 0; i < n; i++) {
        uint64_t k = r->list.data[lo + i];
        CellRead *cr = &c->reads[i];
        uint32_t item = (uint32_t)(k >> 32);
        cr->name = ident_by_id(b->pp->in, (uint32_t)k);
        cr->item = item - (uint32_t)s;
        cr->val = read_val(b, cr->name, items[item].version, &e->rmacro[i]);
        if (e->rmacro[i]) {
            CellMacIdx mi;
            mi.m = e->rmacro[i];
            mi.idx = (uint32_t)i;
            vec_push(&macs, mi);
        }
    }
    /* one index per definition (the first read that found it) */
    if (macs.len > 1)
        qsort(macs.data, macs.len, sizeof *macs.data, mac_cmp);
    {
        size_t w = 0, k;
        for (k = 0; k < macs.len; k++) {
            if (w && macs.data[w - 1].m == macs.data[k].m) {
                if (macs.data[k].idx < macs.data[w - 1].idx)
                    macs.data[w - 1].idx = macs.data[k].idx;
                continue;
            }
            macs.data[w++] = macs.data[k];
        }
        macs.len = w;
    }
    for (i = 0; i < n; i++) {
        CellMacIdx key, *hit;
        if (!e->rmacro[i]) {
            c->reads[i].cls = UINT32_MAX;
            continue;
        }
        key.m = e->rmacro[i];
        hit = bsearch(&key, macs.data, macs.len, sizeof key, mac_cmp);
        c->reads[i].cls = hit->idx;
    }
    for (i = 0; i < macs.len; i++) {
        const Macro *m = macs.data[i].m;
        CellSpan sp;
        if (!m->hash_loc || m->end_loc < m->hash_loc)
            continue;
        sp.begin = m->hash_loc;
        sp.end = m->end_loc;
        sp.a = macs.data[i].idx;
        sp.kind = CL_MACRO;
        vec_push(&spans, sp);
    }
    for (i = s; i < end; i++) {
        const PlanItem *it = &items[i];
        CellSpan sp;
        if (!it->begin)
            continue;
        sp.begin = it->begin;
        sp.end = it->kind == PI_SEG ? it->end : it->begin;
        sp.a = (uint32_t)(i - s);
        sp.kind = CL_ITEM;
        vec_push(&spans, sp);
    }
    if (spans.len > 1)
        qsort(spans.data, spans.len, sizeof *spans.data, span_cmp);
    e->spans = spans.data;
    e->nspans = spans.len;
    e->macs = macs.data;
    e->nmacs = macs.len;

    /* __LINE__ */
    lo = 0;
    while (lo < r->line_items.len && r->line_items.data[lo] < s)
        lo++;
    if (lo < r->line_items.len && r->line_items.data[lo] < end) {
        c->lines = NEW_ARRAY(&c->arena, uint32_t, c->nitems + 1);
        for (i = 0; i < c->nitems; i++)
            c->lines[i] = item_line(b, &items[s + i]);
    }
}

Arena *cenc_arena(CellEnc *e)
{
    return &e->cell->arena;
}

static CLoc frame_loc(CellEnc *e, SrcLoc loc)
{
    const PlanItem *items = e->b->plan->items.data;
    size_t k;
    for (k = e->s; k < e->e; k++) {
        const PlanFrame *f = items[k].kind == PI_ENTER && items[k].frame
                                 ? items[k].frame->parent : items[k].frame;
        uint32_t j;
        for (j = 0; f; f = f->parent, j++)
            if (f->include_loc == loc)
                return CLOC(CL_FRAME, k - e->s, j | (items[k].kind == PI_ENTER
                                                         ? 0x80000000u : 0));
    }
    return 0;
}

CLoc cenc_loc(CellEnc *e, SrcLoc loc)
{
    size_t lo = 0, hi = e->nspans;
    CLoc r;
    if (!loc)
        return 0;
    /* the last span starting at or before loc, then backwards over
     * spans that may still contain it (definitions can be long) */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (e->spans[mid].begin <= loc)
            lo = mid + 1;
        else
            hi = mid;
    }
    while (lo-- > 0) {
        const CellSpan *sp = &e->spans[lo];
        if (loc <= sp->end)
            return CLOC(sp->kind, sp->a, loc - sp->begin);
    }
    r = frame_loc(e, loc);
    if (!r)
        e->ok = false;
    return r;
}

uint32_t cenc_macro(CellEnc *e, const Macro *m)
{
    CellMacIdx key, *hit;
    size_t i;
    if (!m)
        return UINT32_MAX;
    key.m = m;
    hit = bsearch(&key, e->macs, e->nmacs, sizeof key, mac_cmp);
    if (hit)
        return hit->idx;
    for (i = 0; i < e->nmacs; i++) /* a pop_macro version's original */
        if (e->macs[i].m->alias_of == m)
            return e->macs[i].idx | ALIAS_BIT;
    e->ok = false;
    return UINT32_MAX;
}

uint32_t cenc_item(CellEnc *e, uint32_t item)
{
    if (item < e->s || item > e->e) {
        e->ok = false;
        return 0;
    }
    return item - (uint32_t)e->s;
}

const char *cenc_str(CellEnc *e, const char *s)
{
    return s ? arena_strdup(&e->cell->arena, s) : NULL;
}

void cenc_diag(CellEnc *e, const Diagnostic *d)
{
    Cell *c = e->cell;
    Arena *a = &c->arena;
    CellDiag *cd, blank;
    size_t i;
    memset(&blank, 0, sizeof blank);
    vec_push(&e->diags, blank);
    cd = &vec_last(&e->diags);
    cd->level = (uint8_t)d->level;
    cd->once = d->once;
    cd->id = d->id;
    cd->key = cenc_item(e, d->key);
    cd->loc = cenc_loc(e, d->loc);
    if (d->range.end) {
        cd->range_b = cenc_loc(e, d->range.begin);
        cd->range_e = cenc_loc(e, d->range.end);
    }
    cd->msg = cenc_str(e, d->msg);
    cd->fixit = cenc_str(e, d->fixit);
    cd->nnotes = (uint32_t)d->notes.len;
    cd->notes = NEW_ARRAY(a, CellNote, d->notes.len + 1);
    for (i = 0; i < d->notes.len; i++) {
        cd->notes[i].loc = cenc_loc(e, d->notes.data[i].loc);
        cd->notes[i].msg = cenc_str(e, d->notes.data[i].msg);
    }
    cd->ninc = (uint32_t)d->ninc;
    cd->inc = NEW_ARRAY(a, CLoc, (size_t)d->ninc + 1);
    for (i = 0; i < (size_t)d->ninc; i++)
        cd->inc[i] = cenc_loc(e, d->inc_chain[i]);
}

void cenc_exp(CellEnc *e, uint32_t key, const Macro *m)
{
    uint32_t mac = cenc_macro(e, m), i;
    (void)cenc_item(e, key); /* checks it is in the cell */
    if ((e->exps.len + 1) * 2 > e->exp_cap) {
        uint32_t cap = e->exp_cap ? e->exp_cap * 2 : 64, k;
        uint32_t *ns = xcalloc(cap, sizeof *ns);
        for (k = 0; k < e->exps.len; k++) {
            for (i = (e->exps.data[k].macro * 0x9E3779B1u) & (cap - 1); ns[i];
                 i = (i + 1) & (cap - 1))
                ;
            ns[i] = k + 1;
        }
        free(e->exp_slot);
        e->exp_slot = ns;
        e->exp_cap = cap;
    }
    for (i = (mac * 0x9E3779B1u) & (e->exp_cap - 1); e->exp_slot[i];
         i = (i + 1) & (e->exp_cap - 1))
        if (e->exps.data[e->exp_slot[i] - 1].macro == mac) {
            e->exps.data[e->exp_slot[i] - 1].count++;
            return;
        }
    {
        CellExp x;
        x.macro = mac;
        x.count = 1;
        vec_push(&e->exps, x);
        e->exp_slot[i] = (uint32_t)e->exps.len;
    }
}

Cell *cell_enc_end(CellEnc *e, Macro ***rmacro_out)
{
    Cell *c = e->cell;
    if (rmacro_out && e->ok)
        *rmacro_out = e->rmacro;
    else
        free(e->rmacro);
    free(e->spans);
    free(e->macs);
    /* exact sizes in the cell */
    c->ndiags = (uint32_t)e->diags.len;
    c->diags = NEW_ARRAY(&c->arena, CellDiag, e->diags.len + 1);
    if (e->diags.len)
        memcpy(c->diags, e->diags.data, sizeof(CellDiag) * e->diags.len);
    c->nexps = (uint32_t)e->exps.len;
    c->exps = NEW_ARRAY(&c->arena, CellExp, e->exps.len + 1);
    if (e->exps.len)
        memcpy(c->exps, e->exps.data, sizeof(CellExp) * e->exps.len);
    vec_free(&e->diags);
    vec_free(&e->exps);
    free(e->exp_slot);
    e->exp_slot = NULL;
    e->rmacro = NULL;
    e->spans = NULL;
    e->macs = NULL;
    e->cell = NULL;
    if (!e->ok) {
        cell_release(c);
        return NULL;
    }
    return c;
}

/* ---- decoding ------------------------------------------------------------- */

SrcLoc cdec_loc(const CellDec *d, CLoc l)
{
    const PlanItem *items = d->b->plan->items.data;
    switch (CLOC_KIND(l)) {
    case CL_ITEM:
        return items[d->s + CLOC_A(l)].begin + CLOC_OFF(l);
    case CL_MACRO:
        return d->rmacro[CLOC_A(l)]->hash_loc + CLOC_OFF(l);
    case CL_FRAME: {
        const PlanItem *it = &items[d->s + CLOC_A(l)];
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

Macro *cdec_macro(const CellDec *d, uint32_t m)
{
    return cell_macro(d->rmacro, m);
}

uint32_t cdec_item(const CellDec *d, uint32_t rel)
{
    return (uint32_t)d->s + rel;
}

uint32_t cdec_version(const CellDec *d, uint32_t rel)
{
    return d->b->plan->items.data[d->s + rel].version;
}

Diagnostic *cdec_diag(const CellDec *d, const CellDiag *cd)
{
    Diagnostic *g = NEW(d->arena, Diagnostic);
    uint32_t i;
    memset(g, 0, sizeof *g);
    g->level = (DiagLevel)cd->level;
    g->id = cd->id;
    g->once = cd->once;
    g->key = cdec_item(d, cd->key);
    g->loc = cdec_loc(d, cd->loc);
    if (cd->range_e) {
        g->range.begin = cdec_loc(d, cd->range_b);
        g->range.end = cdec_loc(d, cd->range_e);
    }
    /* copied: the cell may be dropped while this build lives on */
    g->msg = arena_strdup(d->arena, cd->msg);
    g->fixit = cd->fixit ? arena_strdup(d->arena, cd->fixit) : NULL;
    for (i = 0; i < cd->nnotes; i++) {
        DiagNote n;
        n.loc = cdec_loc(d, cd->notes[i].loc);
        n.msg = arena_strdup(d->arena, cd->notes[i].msg);
        vec_push(&g->notes, n);
    }
    g->ninc = (int)cd->ninc;
    if (cd->ninc) {
        g->inc_chain = NEW_ARRAY(d->arena, SrcLoc, cd->ninc);
        for (i = 0; i < cd->ninc; i++)
            g->inc_chain[i] = cdec_loc(d, cd->inc[i]);
    }
    return g;
}
