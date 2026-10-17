/* bdd.c - reduced ordered binary decision diagrams. */
#include "bdd.h"

#include <stdlib.h>
#include <string.h>

#define TERM_VAR UINT32_MAX

static uint32_t mix(uint32_t a, uint32_t b, uint32_t c)
{
    uint64_t h = a * 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 29)) + b * 0xBF58476D1CE4E5B9ull;
    h = (h ^ (h >> 32)) + c * 0x94D049BB133111EBull;
    return (uint32_t)(h ^ (h >> 31));
}

void bdd_init(BddMgr *m, size_t cap)
{
    BddNode t = {TERM_VAR, 0, 0};
    memset(m, 0, sizeof *m);
    m->cap = cap;
    vec_push(&m->nodes, t);     /* BDD_FALSE */
    vec_push(&m->nodes, t);     /* BDD_TRUE */
    m->tcap = 1024;
    m->table = xcalloc(m->tcap, sizeof *m->table);
    m->ccap = (size_t)1 << 14;
    m->cache = xcalloc(m->ccap, sizeof *m->cache);
}

void bdd_free(BddMgr *m)
{
    vec_free(&m->nodes);
    free(m->table);
    free(m->cache);
    free(m->walk.tag);
    free(m->walk.val);
    free(m->set.tag);
    free(m->set.val);
    free(m->memo.tag);
    free(m->memo.val);
    memset(m, 0, sizeof *m);
}

size_t bdd_size(const BddMgr *m)
{
    return m->nodes.len;
}

static void rehash(BddMgr *m)
{
    size_t i, ncap = m->tcap * 2;
    Bdd *nt = xcalloc(ncap, sizeof *nt);
    for (i = 2; i < m->nodes.len; i++) {
        const BddNode *n = &m->nodes.data[i];
        size_t k = mix(n->var, n->lo, n->hi) & (ncap - 1);
        while (nt[k])
            k = (k + 1) & (ncap - 1);
        nt[k] = (Bdd)i;
    }
    free(m->table);
    m->table = nt;
    m->tcap = ncap;
}

static Bdd mk(BddMgr *m, uint32_t v, Bdd lo, Bdd hi)
{
    size_t k;
    Bdd id;
    BddNode n;
    if (lo == hi)
        return lo;
    k = mix(v, lo, hi) & (m->tcap - 1);
    while (m->table[k]) {
        const BddNode *e = &m->nodes.data[m->table[k]];
        if (e->var == v && e->lo == lo && e->hi == hi)
            return m->table[k];
        k = (k + 1) & (m->tcap - 1);
    }
    if (m->nodes.len >= m->cap) {
        m->over = true;
        return BDD_FALSE;
    }
    n.var = v;
    n.lo = lo;
    n.hi = hi;
    vec_push(&m->nodes, n);
    id = (Bdd)(m->nodes.len - 1);
    m->table[k] = id;
    if (m->nodes.len * 2 > m->tcap)
        rehash(m);
    return id;
}

Bdd bdd_var(BddMgr *m, uint32_t v)
{
    return mk(m, v, BDD_FALSE, BDD_TRUE);
}

static uint32_t top(const BddMgr *m, Bdd f)
{
    return m->nodes.data[f].var;
}

Bdd bdd_ite(BddMgr *m, Bdd f, Bdd g, Bdd h)
{
    uint32_t v;
    Bdd lo, hi, r;
    BddCache *c;
    if (m->over)
        return BDD_FALSE;
    if (f == BDD_TRUE)
        return g;
    if (f == BDD_FALSE)
        return h;
    if (g == h)
        return g;
    if (g == BDD_TRUE && h == BDD_FALSE)
        return f;
    c = &m->cache[mix(f, g, h) & (m->ccap - 1)];
    if (c->f == f && c->g == g && c->h == h)
        return c->r;
    v = MIN(top(m, f), MIN(top(m, g), top(m, h)));
#define COF(x, b) (top(m, x) == v ? m->nodes.data[x].b : (x))
    lo = bdd_ite(m, COF(f, lo), COF(g, lo), COF(h, lo));
    hi = bdd_ite(m, COF(f, hi), COF(g, hi), COF(h, hi));
#undef COF
    r = mk(m, v, lo, hi);
    if (!m->over) {
        c = &m->cache[mix(f, g, h) & (m->ccap - 1)];
        c->f = f;
        c->g = g;
        c->h = h;
        c->r = r;
    }
    return r;
}

Bdd bdd_not(BddMgr *m, Bdd f)
{
    return bdd_ite(m, f, BDD_FALSE, BDD_TRUE);
}

Bdd bdd_and(BddMgr *m, Bdd f, Bdd g)
{
    return bdd_ite(m, f, g, BDD_FALSE);
}

Bdd bdd_or(BddMgr *m, Bdd f, Bdd g)
{
    return bdd_ite(m, f, BDD_TRUE, g);
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

uint32_t bdd_scratch_begin(BddScratch *s, size_t n)
{
    if (s->cap < n) {
        size_t c = n * 2;
        s->tag = xrealloc(s->tag, c * sizeof *s->tag);
        s->val = xrealloc(s->val, c * sizeof *s->val);
        memset(s->tag + s->cap, 0, (c - s->cap) * sizeof *s->tag);
        s->cap = c;
    }
    if (++s->gen == 0) {
        memset(s->tag, 0, s->cap * sizeof *s->tag);
        s->gen = 1;
    }
    return s->gen;
}

typedef struct Restrict {
    BddMgr *m;
    int (*val)(void *ctx, uint32_t v);
    void *ctx;
    uint32_t gen;
} Restrict;

static Bdd restrict_rec(Restrict *r, Bdd f)
{
    BddScratch *w = &r->m->walk;
    BddNode nd;
    int v;
    Bdd res;
    if (f < 2)
        return f;
    if (w->tag[f] == r->gen)
        return w->val[f];
    nd = r->m->nodes.data[f];
    v = r->val(r->ctx, nd.var);
    res = v >= 0 ? restrict_rec(r, v ? nd.hi : nd.lo)
                 : mk(r->m, nd.var, restrict_rec(r, nd.lo), restrict_rec(r, nd.hi));
    w->tag[f] = r->gen;
    w->val[f] = res;
    return res;
}

Bdd bdd_restrict(BddMgr *m, Bdd f, int (*val)(void *ctx, uint32_t v), void *ctx)
{
    Restrict r = {m, val, ctx, 0};
    if (f < 2)
        return f;
    r.gen = bdd_scratch_begin(&m->walk, m->nodes.len);
    return restrict_rec(&r, f);
}

void bdd_vars(BddMgr *m, Bdd f, BddVars *out)
{
    VEC(Bdd) st = {0};
    BddVars all = {0};
    uint32_t *vis, gen;
    size_t i;
    if (f < 2)
        return;
    gen = bdd_scratch_begin(&m->walk, m->nodes.len);
    vis = m->walk.tag;
    vec_push(&st, f);
    while (st.len) {
        Bdd x = vec_pop(&st);
        const BddNode *n;
        if (x < 2 || vis[x] == gen)
            continue;
        vis[x] = gen;
        n = &m->nodes.data[x];
        vec_push(&all, n->var);
        vec_push(&st, n->lo);
        vec_push(&st, n->hi);
    }
    vec_free(&st);
    qsort(all.data, all.len, sizeof *all.data, cmp_u32);
    for (i = 0; i < all.len; i++)
        if (!i || all.data[i] != all.data[i - 1])
            vec_push(out, all.data[i]);
    vec_free(&all);
}

bool bdd_eval(const BddMgr *m, Bdd f, const bool *assign)
{
    while (f >= 2) {
        const BddNode *n = &m->nodes.data[f];
        f = assign[n->var] ? n->hi : n->lo;
    }
    return f == BDD_TRUE;
}

size_t bdd_count(BddMgr *m, Bdd f)
{
    VEC(Bdd) st = {0};
    uint32_t *vis, gen;
    size_t n = 0;
    if (f < 2)
        return 0;
    gen = bdd_scratch_begin(&m->walk, m->nodes.len);
    vis = m->walk.tag;
    vec_push(&st, f);
    while (st.len) {
        Bdd x = vec_pop(&st);
        if (x < 2 || vis[x] == gen)
            continue;
        vis[x] = gen;
        n++;
        vec_push(&st, m->nodes.data[x].lo);
        vec_push(&st, m->nodes.data[x].hi);
    }
    vec_free(&st);
    return n;
}

bool bdd_eval_fn(const BddMgr *m, Bdd f, bool (*val)(void *ctx, uint32_t v),
                 void *ctx)
{
    while (f >= 2) {
        const BddNode *n = &m->nodes.data[f];
        f = val(ctx, n->var) ? n->hi : n->lo;
    }
    return f == BDD_TRUE;
}

/* ---- self-test ------------------------------------------------------ */

#define NV 8

static unsigned rng_state = 12345;
static unsigned rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* a random expression over NV variables, built as a BDD and as a 256-bit
 * truth table */
typedef struct TT { uint64_t w[4]; } TT;

static bool tt_get(const TT *t, unsigned a)
{
    return t->w[a / 64] >> (a % 64) & 1;
}

static void expr(BddMgr *m, int depth, Bdd *f, TT *t)
{
    unsigned op = depth <= 0 ? 0 : rnd() % 5;
    Bdd f1 = 0, f2 = 0, f3 = 0;
    TT t1 = {{0}}, t2 = {{0}}, t3 = {{0}};
    int i;
    if (op == 0) {
        unsigned v = rnd() % NV, a;
        *f = bdd_var(m, v);
        memset(t, 0, sizeof *t);
        for (a = 0; a < 256; a++)
            if (a >> v & 1)
                t->w[a / 64] |= 1ull << (a % 64);
        return;
    }
    expr(m, depth - 1, &f1, &t1);
    if (op != 1)
        expr(m, depth - 1, &f2, &t2);
    if (op == 4)
        expr(m, depth - 1, &f3, &t3);
    for (i = 0; i < 4; i++)
        t->w[i] = op == 1   ? ~t1.w[i]
                  : op == 2 ? t1.w[i] & t2.w[i]
                  : op == 3 ? t1.w[i] | t2.w[i]
                            : (t1.w[i] & t2.w[i]) | (~t1.w[i] & t3.w[i]);
    *f = op == 1   ? bdd_not(m, f1)
         : op == 2 ? bdd_and(m, f1, f2)
         : op == 3 ? bdd_or(m, f1, f2)
                   : bdd_ite(m, f1, f2, f3);
}

int bdd_selftest(void)
{
    BddMgr m;
    int round, bad = 0;
    bdd_init(&m, (size_t)1 << 20);
    for (round = 0; round < 300 && !bad; round++) {
        Bdd f;
        TT t;
        unsigned a;
        bool seen[NV] = {0}, as[NV];
        BddVars vs = {0};
        size_t k;
        expr(&m, 6, &f, &t);
        for (a = 0; a < 256; a++) {
            unsigned v;
            for (v = 0; v < NV; v++)
                as[v] = a >> v & 1;
            if (bdd_eval(&m, f, as) != tt_get(&t, a))
                bad = 1;
        }
        /* canonical: the same function built again is the same node */
        if (bdd_not(&m, bdd_not(&m, f)) != f)
            bad = 1;
        /* the support is exactly the variables the table depends on */
        bdd_vars(&m, f, &vs);
        for (k = 0; k < vs.len; k++)
            seen[vs.data[k]] = true;
        vec_free(&vs);
        for (a = 0; a < NV; a++) {
            unsigned x;
            bool dep = false;
            for (x = 0; x < 256; x++)
                dep |= tt_get(&t, x) != tt_get(&t, x ^ (1u << a));
            if (dep != seen[a])
                bad = 1;
        }
    }
    if (m.over)
        bad = 1;
    bdd_free(&m);
    /* the node cap: past it the result is FALSE and `over` is set */
    bdd_init(&m, 8);
    {
        Bdd f = BDD_TRUE;
        uint32_t v;
        for (v = 0; v < 20; v++)
            f = bdd_and(&m, f, bdd_var(&m, v));
        if (!m.over || f != BDD_FALSE || bdd_size(&m) > 8)
            bad = 1;
    }
    bdd_free(&m);
    if (bad)
        fputs("selftest bdd: FAIL\n", stderr);
    return bad;
}
