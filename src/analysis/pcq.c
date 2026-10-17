/* pcq.c - queries on presence conditions: holds, cheapest cubes, diff. */
#include "pcq.h"
#include "pcfam.h"

#include <stdlib.h>
#include <string.h>

#define INF 0x3fffffffu
#define W_TARGET 1000u          /* one target change outweighs any language ones */
#define W_LANG 100u             /* and one language change any config ones */

uint32_t pc_cube_weight(const PcCube *c)
{
    return c->target * W_TARGET + c->lang * W_LANG + c->cost;
}
#define POP_CAP 20000

/* value of atom at var under v: 0, 1, or -1 when it is not known */
static int vstate(PcCtx *cx, const PcValuation *v, uint32_t var)
{
    const PcAtom *a = pc_atom_at(cx, var);
    bool known = false, val;
    if (!a)
        return -1;
    val = v->get(v->ctx, a, &known);
    return known ? val : -1;
}

typedef struct HoldsCtx {
    PcCtx *cx;
    const PcValuation *v;
} HoldsCtx;

static bool holds_var(void *ctx, uint32_t var)
{
    HoldsCtx *h = ctx;
    return vstate(h->cx, h->v, var) == 1;
}

bool pc_holds(PcCtx *cx, Bdd f, const PcValuation *v)
{
    HoldsCtx h = {cx, v};
    return bdd_eval_fn(&cx->m, f, holds_var, &h);
}

bool pc_exact(PcCtx *cx, Bdd f)
{
    BddVars vs = {0};
    size_t i;
    bool ok = true;
    bdd_vars(&cx->m, f, &vs);
    for (i = 0; i < vs.len; i++) {
        const PcAtom *a = pc_atom_at(cx, vs.data[i]);
        if (!a || a->kind == PA_TEXT)
            ok = false;
    }
    vec_free(&vs);
    return ok;
}

/* ---- the cost DP -------------------------------------------------------- */

typedef struct Q {
    PcCtx *cx;
    BddMgr *m;
    const PcValuation *v;
    BddScratch *memo;       /* cost by node * 2 + deviated */
    uint32_t gen;
} Q;

static bool is_cost_atom(PcCtx *cx, uint32_t var)
{
    const PcAtom *a = pc_atom_at(cx, var);
    return a && !pc_atom_pinned(a);
}

static uint32_t atom_weight(const PcAtom *a)
{
    return a->cls == PC_TARGET ? W_TARGET : a->cls == PC_LANG ? W_LANG
         : a->derived ? 0 : 1;
}

static int fixed_val(void *ctx, uint32_t var)
{
    const Q *q = ctx;
    const PcAtom *a = pc_atom_at(q->cx, var);
    return a && q->v->fixed(q->v->ctx, a) ? vstate(q->cx, q->v, var) : -1;
}

/* Taking value b for var: the added cost, and the "deviated" flag the child
 * starts with (it carries over only inside one macro). */
static void step(Q *q, uint32_t var, int dev, int b, Bdd child, uint32_t *add,
                 int *ndev)
{
    int vs = vstate(q->cx, q->v, var);
    int d = dev || (is_cost_atom(q->cx, var) && vs >= 0 && vs != b);
    *add = d && !dev ? atom_weight(pc_atom_at(q->cx, var)) : 0;
    if (vs >= 0 && vs != b && q->v->fixed && q->v->fixed(q->v->ctx, pc_atom_at(q->cx, var)))
        *add = INF;
    *ndev = child > BDD_TRUE &&
            pc_var_owner(q->cx, q->m->nodes.data[child].var) ==
                pc_var_owner(q->cx, var)
                ? d
                : 0;
}

static uint32_t dp(Q *q, Bdd n, int dev)
{
    const BddNode *nd;
    uint32_t best = INF;
    int b;
    if (n == BDD_FALSE)
        return INF;
    if (n == BDD_TRUE)
        return 0;
    if (q->memo->tag[n * 2 + (uint32_t)dev] == q->gen)
        return q->memo->val[n * 2 + (uint32_t)dev];
    nd = &q->m->nodes.data[n];
    for (b = 0; b < 2; b++) {
        Bdd c = b ? nd->hi : nd->lo;
        uint32_t add, r;
        int nd2;
        step(q, nd->var, dev, b, c, &add, &nd2);
        r = dp(q, c, nd2);
        if (r < INF && add < INF && add + r < best)
            best = add + r;
    }
    q->memo->tag[n * 2 + (uint32_t)dev] = q->gen;
    q->memo->val[n * 2 + (uint32_t)dev] = best;
    return best;
}

/* ---- best-first enumeration of paths ------------------------------------ */

typedef struct Lit {
    uint32_t var;
    uint8_t val;
    struct Lit *prev;
} Lit;

typedef struct State {
    uint32_t prio, sofar;
    Bdd n;
    int dev;
    Lit *path;
    uint32_t seq;
} State;

typedef VEC(State) Heap;

static bool st_less(const State *a, const State *b)
{
    return a->prio < b->prio || (a->prio == b->prio && a->seq < b->seq);
}

static void heap_push(Heap *h, State s)
{
    size_t i;
    vec_push(h, s);
    i = h->len - 1;
    while (i) {
        size_t p = (i - 1) / 2;
        State t;
        if (!st_less(&h->data[i], &h->data[p]))
            break;
        t = h->data[i];
        h->data[i] = h->data[p];
        h->data[p] = t;
        i = p;
    }
}

static State heap_pop(Heap *h)
{
    State top = h->data[0];
    size_t i = 0;
    h->data[0] = h->data[--h->len];
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, s = i;
        State t;
        if (l < h->len && st_less(&h->data[l], &h->data[s]))
            s = l;
        if (r < h->len && st_less(&h->data[r], &h->data[s]))
            s = r;
        if (s == i)
            break;
        t = h->data[i];
        h->data[i] = h->data[s];
        h->data[s] = t;
        i = s;
    }
    return top;
}

/* a cube: literals in ascending variable order */
typedef struct CubeLits {
    uint32_t *var;
    uint8_t *val;
    uint32_t n;
} CubeLits;

/* every assignment extending the literals makes n true */
static bool implied(Q *q, Bdd n, const CubeLits *c, uint32_t *ok, uint32_t gen)
{
    const BddNode *nd;
    uint32_t lo = 0, hi = c->n;
    if (n == BDD_TRUE)
        return true;
    if (n == BDD_FALSE)
        return false;
    if (ok[n] == gen)
        return true;
    nd = &q->m->nodes.data[n];
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c->var[mid] < nd->var)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == c->n)             /* below the cube's last variable: n is not constant */
        return false;
    if (c->var[lo] == nd->var) {
        if (!implied(q, c->val[lo] ? nd->hi : nd->lo, c, ok, gen))
            return false;
    } else if (!implied(q, nd->lo, c, ok, gen) || !implied(q, nd->hi, c, ok, gen)) {
        return false;
    }
    ok[n] = gen;
    return true;
}

/* Drop literals, in variable order, while the rest still implies g. */
static void prime(Q *q, Bdd g, CubeLits *c)
{
    uint32_t i;
    size_t nn = bdd_size(q->m);
    BddScratch *set = &q->m->set;       /* tag[n] == gen: n holds under this cube */
    uint32_t *ok = NULL, gen = 0;
    for (i = 0; i < c->n;) {
        CubeLits t = *c;
        bool drop;
        uint32_t k;
        t.var = xmalloc(sizeof(uint32_t) * c->n);
        t.val = xmalloc(c->n);
        for (k = 0, t.n = 0; k < c->n; k++)
            if (k != i) {
                t.var[t.n] = c->var[k];
                t.val[t.n++] = c->val[k];
            }
        gen = bdd_scratch_begin(set, nn);
        ok = set->tag;
        drop = implied(q, g, &t, ok, gen);
        if (drop) {
            free(c->var);
            free(c->val);
            *c = t;
        } else {
            free(t.var);
            free(t.val);
            i++;
        }
    }
}

/* ---- cubes to requirements ---------------------------------------------- */

static void lit_text(PcCtx *cx, uint32_t var, int val, StrBuf *sb)
{
    const PcAtom *a = pc_atom_at(cx, var);
    switch (a->kind) {
    case PA_DEF:
        sb_printf(sb, val ? "defined(%s)" : "!defined(%s)", a->id->str);
        break;
    case PA_GE:
        sb_printf(sb, val ? "%s >= %jd" : "%s < %jd", a->id->str, a->c);
        break;
    case PA_LEAF:
        sb_printf(sb, val ? "%s = %s" : "%s != %s", a->text, pc_leaf_name(a));
        break;
    default:
        sb_printf(sb, val ? "[%s]" : "![%s]", a->text);
        break;
    }
}

static PcCube make_cube(Q *q, const CubeLits *c)
{
    PcCube r = {0};
    uint32_t i, j, k;
    uint32_t *owners = xmalloc(sizeof(uint32_t) * (c->n ? c->n : 1));
    bool *derived;
    uint32_t no = 0;
    for (i = 0; i < c->n; i++) {
        uint32_t o = pc_var_owner(q->cx, c->var[i]);
        for (j = 0; j < no && owners[j] != o; j++)
            ;
        if (j == no)
            owners[no++] = o;
    }
    r.reqs = xmalloc(sizeof(PcReq) * (no ? no : 1));
    derived = xcalloc(no ? no : 1, sizeof *derived);
    for (j = 0; j < no; j++) {
        StrBuf sb = {0};
        bool unmet = false, unknown = false, text = false, pos = false,
             env = false, residual = false;
        PcClass cls = PC_CFG;
        Ident *id = NULL;
        int defv = -1, axis = -1, lf = -1;
        intmax_t lo = 0, hi = INTMAX_MAX;
        bool ge_pos = false;
        uint32_t ruled = 0;
        for (i = 0; i < c->n; i++)       /* a leaf that holds says the rest */
            if (pc_var_owner(q->cx, c->var[i]) == owners[j] &&
                pc_atom_at(q->cx, c->var[i])->kind == PA_LEAF && c->val[i])
                pos = true;
        for (i = 0; i < c->n; i++) {
            const PcAtom *a;
            int vs;
            if (pc_var_owner(q->cx, c->var[i]) != owners[j])
                continue;
            a = pc_atom_at(q->cx, c->var[i]);
            text = a->kind == PA_TEXT;
            env = a->kind == PA_HAS;
            cls = a->cls;
            id = a->id;
            residual |= a->kind == PA_LEAF && c->val[i] && pc_leaf_residual(a);
            derived[j] = a->derived;
            if (a->kind == PA_DEF)
                defv = c->val[i];
            else if (a->kind == PA_GE && c->val[i]) {
                lo = a->c > lo ? a->c : lo;
                ge_pos = true;
            } else if (a->kind == PA_GE && a->c < hi)
                hi = a->c;
            else if (a->kind == PA_LEAF) {
                axis = pc_leaf_axis(a);
                if (c->val[i])
                    lf = (int)a->c;
                else
                    ruled |= 1u << a->c;
            }
            vs = vstate(q->cx, q->v, c->var[i]);
            if (vs < 0)
                unknown = true;
            else if (vs != c->val[i])
                unmet = true;
            if (pos && a->kind == PA_LEAF && !c->val[i])
                continue;
            if (sb.len)
                sb_puts(&sb, " && ");
            lit_text(q->cx, c->var[i], c->val[i], &sb);
        }
        r.reqs[j].clause = xstrdup(sb_cstr(&sb));
        sb_free(&sb);
        r.reqs[j].kind = text ? PQ_OPAQUE : env ? PQ_ENV
                       : cls == PC_TARGET ? PQ_TARGET
                       : cls == PC_LANG ? PQ_LANG : PQ_CONFIG;
        r.reqs[j].status = unmet ? PQ_UNMET : unknown ? PQ_UNKNOWN : PQ_MET;
        r.reqs[j].id = id;
        r.reqs[j].word = NULL;
        r.reqs[j].axis = axis;
        for (i = 0; axis >= 0 && lf < 0 && i < (uint32_t)pc_axis_size(axis); i++)
            if (!(ruled & (1u << i)))
                lf = (int)i;
        r.reqs[j].leaf = lf;
        if (unmet && cls == PC_CFG && !text && !env && id) {
            StrBuf w = {0};
            if (defv == 0 || (defv < 0 && !ge_pos))
                sb_printf(&w, "-U%s", id->str);
            else if (lo > 0)
                sb_printf(&w, "-D%s=%jd", id->str, lo);
            else if (hi <= 1)
                sb_printf(&w, "-D%s=%jd", id->str, hi - 1);
            else
                sb_printf(&w, "-D%s", id->str);
            r.reqs[j].word = xstrdup(sb_cstr(&w));
            sb_free(&w);
        }
        r.reqs[j].residual = residual;
        r.residual |= unmet && residual;
        if (unmet && cls == PC_TARGET)
            r.target++;
        else if (unmet && cls == PC_LANG)
            r.lang++;
        else if (unmet && !text && !env && !derived[j])
            r.cost++;
    }
    /* a macro the table derives from its leaf is not asked for separately
     * when the leaf itself must change */
    for (j = k = 0; j < no; j++) {
        if ((r.target || r.lang) && derived[j] &&
            r.reqs[j].status == PQ_UNMET) {
            free((void *)r.reqs[j].clause);
            free(r.reqs[j].word);
            continue;
        }
        r.reqs[k++] = r.reqs[j];
    }
    r.n = k;
    free(derived);
    free(owners);
    return r;
}

void pc_cube_free(PcCube *c)
{
    uint32_t i;
    for (i = 0; i < c->n; i++) {
        free((void *)c->reqs[i].clause);
        free(c->reqs[i].word);
    }
    free(c->reqs);
    memset(c, 0, sizeof *c);
}

bool pc_enable(PcCtx *cx, const PcCube *c, const PcValuation *v, StrBuf *sb)
{
    uint32_t i;
    int std = -1, mode = -1;
    for (i = 0; i < c->n; i++) {
        const PcReq *q = &c->reqs[i];
        if (q->status == PQ_MET)
            continue;
        if (q->status == PQ_UNKNOWN || q->residual ||
            (q->kind != PQ_CONFIG && q->kind != PQ_LANG))
            return false;
        if (q->kind == PQ_CONFIG) {
            if (!q->word)
                return false;
            sb_printf(sb, "%s%s", sb->len ? " " : "", q->word);
        } else if (q->axis == pc_axis_by_name("std")) {
            std = q->leaf;
        } else {
            mode = q->leaf;
        }
    }
    if (std >= 0 || mode >= 0) {
        const char *w = pc_std_word(cx, v, std, mode);
        if (!w)
            return false;
        sb_printf(sb, "%s%s", sb->len ? " " : "", w);
    }
    return true;
}

char *pc_cube_render(const PcCube *c, bool only_unmet)
{
    StrBuf sb = {0};
    uint32_t i;
    char *r;
    for (i = 0; i < c->n; i++) {
        static const char *const st[] = {"met", "unmet", "unknown"};
        const PcReq *q = &c->reqs[i];
        if (only_unmet && q->status == PQ_MET)
            continue;
        if (sb.len)
            sb_puts(&sb, "; ");
        static const char *const kn[] = {"", "target ", "env ", "opaque ",
                                         "language "};
        sb_printf(&sb, "%s%s [%s]", kn[q->kind], q->clause, st[q->status]);
    }
    r = xstrdup(sb_cstr(&sb));
    sb_free(&sb);
    return r;
}

typedef struct Ranked {
    PcCube c;
    char *key;
} Ranked;

static int ranked_cmp(const void *a, const void *b)
{
    const Ranked *x = a, *y = b;
    uint32_t wx = pc_cube_weight(&x->c), wy = pc_cube_weight(&y->c);
    if (wx != wy)
        return wx < wy ? -1 : 1;
    return strcmp(x->key, y->key);
}

/* Every unmet requirement of a is also one of b: b asks for all that a does
 * and more, so it is no alternative to it. */
static bool subsumes(const PcCube *a, const PcCube *b)
{
    uint32_t i, j;
    for (i = 0; i < a->n; i++) {
        bool in = a->reqs[i].status == PQ_MET;
        for (j = 0; j < b->n && !in; j++)
            in = b->reqs[j].status != PQ_MET &&
                 !strcmp(a->reqs[i].clause, b->reqs[j].clause);
        if (!in)
            return false;
    }
    return true;
}

size_t pc_preconditions(PcCtx *cx, Bdd f, const PcValuation *v, size_t k,
                        PcCube *out)
{
    Q q = {cx, &cx->m, v, NULL, 0};
    Bdd g;
    Heap h = {0};
    VEC(Ranked) found = {0};
    VEC(Lit *) lits = {0};
    uint32_t seq = 0, pops = 0, tot;
    size_t i, n = 0;
    if (cx->m.over)
        return 0;
    if (v->fixed)               /* atoms that cannot flip are what the host has */
        f = bdd_restrict(&cx->m, f, fixed_val, &q);
    g = pc_constrain(cx, f);
    if (cx->m.over || g == BDD_FALSE)
        return 0;
    q.memo = &cx->m.memo;
    q.gen = bdd_scratch_begin(q.memo, 2 * bdd_size(&cx->m));
    tot = dp(&q, g, 0);
    if (tot < INF) {
        State s = {tot, 0, g, 0, NULL, seq++};
        heap_push(&h, s);
    }
    /* priming reorders the paths by a little: a few more than k are enough (4k+4
     * gave the same covers as 64 on the corpus, at a quarter of the priming) */
    while (h.len && found.len < 4 * k + 4 && pops++ < POP_CAP) {
        State s = heap_pop(&h);
        const BddNode *nd;
        int b;
        if (s.n == BDD_TRUE) {
            CubeLits c;
            Lit *l;
            Ranked r;
            uint32_t cnt = 0, j;
            for (l = s.path; l; l = l->prev)
                cnt++;
            c.var = xmalloc(sizeof(uint32_t) * (cnt ? cnt : 1));
            c.val = xmalloc(cnt ? cnt : 1);
            c.n = cnt;
            for (l = s.path, j = cnt; l; l = l->prev) {
                c.var[--j] = l->var;
                c.val[j] = l->val;
            }
            prime(&q, g, &c);
            r.c = make_cube(&q, &c);
            r.key = pc_cube_render(&r.c, true);  /* what must change */
            free(c.var);
            free(c.val);
            for (j = 0; j < found.len && strcmp(found.data[j].key, r.key); j++)
                ;
            if (j < found.len) {
                pc_cube_free(&r.c);
                free(r.key);
            } else {
                vec_push(&found, r);
            }
            continue;
        }
        nd = &cx->m.nodes.data[s.n];
        for (b = 0; b < 2; b++) {
            Bdd c = b ? nd->hi : nd->lo;
            uint32_t add, r;
            int nd2;
            State t;
            Lit *l;
            if (c == BDD_FALSE)
                continue;
            step(&q, nd->var, s.dev, b, c, &add, &nd2);
            r = dp(&q, c, nd2);
            if (r >= INF || add >= INF)
                continue;
            l = xmalloc(sizeof *l);
            l->var = nd->var;
            l->val = (uint8_t)b;
            l->prev = s.path;
            vec_push(&lits, l);
            t.sofar = s.sofar + add;
            t.prio = t.sofar + r;
            t.n = c;
            t.dev = nd2;
            t.path = l;
            t.seq = seq++;
            heap_push(&h, t);
        }
    }
    qsort(found.data, found.len, sizeof(Ranked), ranked_cmp);
    for (i = 0; i < found.len; i++) {
        size_t m;
        for (m = 0; m < n && !subsumes(&out[m], &found.data[i].c); m++)
            ;
        if (n < k && m == n)
            out[n++] = found.data[i].c;
        else
            pc_cube_free(&found.data[i].c);
        free(found.data[i].key);
    }
    for (i = 0; i < lits.len; i++)
        free(lits.data[i]);
    vec_free(&lits);
    vec_free(&found);
    vec_free(&h);
    return n;
}

/* ---- diff --------------------------------------------------------------- */

typedef VEC(uint32_t) U32Vec;

typedef struct MixCtx {
    PcCtx *cx;
    const PcValuation *a, *b;
    uint32_t owner;
} MixCtx;

static bool mix_var(void *ctx, uint32_t var)
{
    MixCtx *x = ctx;
    return vstate(x->cx,
                  pc_var_owner(x->cx, var) == x->owner ? x->b : x->a, var) == 1;
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

size_t pc_diff(PcCtx *cx, Bdd f, const PcValuation *v1, const PcValuation *v2,
               const char ***names)
{
    BddVars vs = {0};
    U32Vec cand = {0};
    U32Vec hit = {0};
    const char **out;
    size_t i, j, n;
    bool h1 = pc_holds(cx, f, v1), h2 = pc_holds(cx, f, v2);
    *names = NULL;
    if (h1 == h2)
        return 0;
    bdd_vars(&cx->m, f, &vs);
    for (i = 0; i < vs.len; i++) {
        int a = vstate(cx, v1, vs.data[i]), b = vstate(cx, v2, vs.data[i]);
        uint32_t o = pc_var_owner(cx, vs.data[i]);
        if (a < 0 || b < 0 || a == b)
            continue;
        for (j = 0; j < cand.len && cand.data[j] != o; j++)
            ;
        if (j == cand.len)
            vec_push(&cand, o);
    }
    for (i = 0; i < cand.len; i++) {
        MixCtx x = {cx, v1, v2, cand.data[i]};
        if (bdd_eval_fn(&cx->m, f, mix_var, &x) == h2)
            vec_push(&hit, cand.data[i]);
    }
    {
        U32Vec *use = hit.len ? &hit : &cand;
        n = use->len;
        out = xmalloc(sizeof(char *) * (n ? n : 1));
        for (i = 0; i < n; i++)
            out[i] = pc_owner_name(cx, use->data[i]);
    }
    qsort(out, n, sizeof *out, name_cmp);
    vec_free(&vs);
    vec_free(&cand);
    vec_free(&hit);
    *names = out;
    return n;
}

/* ---- the independent reference: enumerate every assignment ----------------- */

bool pc_cost_brute(PcCtx *cx, Bdd g, const PcValuation *v, size_t maxvars,
                   uint32_t *best)
{
    BddVars vs = {0};
    bool *as;
    uint32_t *seen, a, maxv = 0;
    size_t k;
    bdd_vars(&cx->m, g, &vs);
    if (vs.len > maxvars) {
        vec_free(&vs);
        return false;
    }
    for (k = 0; k < vs.len; k++)
        maxv = MAX(maxv, vs.data[k]);
    as = xcalloc((size_t)maxv + 1, sizeof *as);
    seen = xmalloc(sizeof *seen * (vs.len ? vs.len : 1));
    *best = INF;
    for (a = 0; a < (1u << vs.len); a++) {
        uint32_t cost = 0, ns = 0, j;
        for (k = 0; k < vs.len; k++)
            as[vs.data[k]] = a >> k & 1;
        if (!bdd_eval(&cx->m, g, as))
            continue;
        for (k = 0; k < vs.len; k++) {
            const PcAtom *at = pc_atom_at(cx, vs.data[k]);
            uint32_t o = pc_var_owner(cx, vs.data[k]);
            bool known = false, vv;
            if (pc_atom_pinned(at))
                continue;
            vv = v->get(v->ctx, at, &known);
            if (!known || vv == as[vs.data[k]])
                continue;
            for (j = 0; j < ns && seen[j] != o; j++)
                ;
            if (j == ns) {
                seen[ns++] = o;
                cost += atom_weight(at);
            }
        }
        if (cost < *best)
            *best = cost;
    }
    free(as);
    free(seen);
    vec_free(&vs);
    return true;
}

int pc_check_rank1(PcCtx *cx, Bdd f, const PcValuation *v)
{
    Bdd g;
    PcCube c;
    uint32_t best;
    int r;
    if (cx->m.over)
        return -1;
    g = pc_constrain(cx, f);
    if (cx->m.over || g == BDD_FALSE || !pc_cost_brute(cx, g, v, 20, &best))
        return -1;
    if (!pc_preconditions(cx, f, v, 1, &c))
        return 1;
    r = pc_cube_weight(&c) != best;
    pc_cube_free(&c);
    return r;
}

/* ---- selftest: the DP against brute force -------------------------------- */

#define NM 6                /* macros */
#define NT 2                /* opaque texts */
#define NTH 3               /* thresholds GE(1..3) */

static Ident st_ids[NM];
static const char *const st_names[NM] = {"A", "B", "__linux__", "_MSC_VER",
                                         "__STDC_VERSION__", "__BYTE_ORDER__"};
static const char *const st_texts[NT] = {"t0", "t1"};

#define NLEAF 6

typedef struct SelfV {
    bool def[NM];
    int val[NM];            /* 0..4 */
    int known_t[NT];        /* -1 free, else the value */
    int leaf[NLEAF];        /* the leaf of each axis (pc_leaf_axis) */
} SelfV;

/* the leaves of each axis of the family table: os, cc, dm, std, mode, endian */
static const int st_leaves[NLEAF] = {5, 5, 3, 5, 2, 2};

static bool self_get(void *ctx, const PcAtom *a, bool *known)
{
    const SelfV *v = ctx;
    int i;
    *known = true;
    if (a->kind == PA_LEAF)
        return a->c == (unsigned)v->leaf[pc_leaf_axis(a)];
    if (a->kind == PA_TEXT) {
        for (i = 0; i < NT; i++)
            if (!strcmp(a->text, st_texts[i])) {
                *known = v->known_t[i] >= 0;
                return v->known_t[i] > 0;
            }
        return false;
    }
    for (i = 0; i < NM; i++)
        if (a->id == &st_ids[i])
            return a->kind == PA_DEF ? v->def[i] : v->def[i] && v->val[i] >= a->c;
    return false;
}

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return rnd_state >> 16;
}

static Bdd rnd_expr(PcCtx *cx, Bdd *atoms, size_t na, int depth)
{
    Bdd a, b;
    if (depth == 0 || rnd() % 4 == 0)
        return atoms[rnd() % na];
    a = rnd_expr(cx, atoms, na, depth - 1);
    b = rnd_expr(cx, atoms, na, depth - 1);
    switch (rnd() % 3) {
    case 0: return bdd_and(&cx->m, a, b);
    case 1: return bdd_or(&cx->m, a, b);
    default: return bdd_not(&cx->m, bdd_and(&cx->m, a, bdd_not(&cx->m, b)));
    }
}

int pcq_selftest(void)
{
    PcCtx cx;
    Bdd atoms[NM * (1 + NTH) + NT];
    size_t na = 0;
    int i, c, round, bad = 0;
    int compared = 0, multi = 0, leafc = 0;
    for (i = 0; i < NM; i++) {
        memset(&st_ids[i], 0, sizeof st_ids[i]);
        st_ids[i].str = st_names[i];
    }
    for (round = 0; round < 400 && !bad; round++) {
        Bdd f, g;
        BddVars vs = {0};
        SelfV v;
        PcValuation val;
        PcCube cubes[3];
        size_t nc;
        uint32_t best = INF, a;
        bool have;
        int m;
        pc_init(&cx, NULL, (size_t)1 << 20);
        na = 0;
        for (i = 0; i < NM; i++) {
            atoms[na++] = pc_atom(&cx, PA_DEF, &st_ids[i], 0, NULL);
            for (c = 1; c <= NTH; c++)
                atoms[na++] = pc_atom(&cx, PA_GE, &st_ids[i], c, NULL);
        }
        for (i = 0; i < NT; i++)
            atoms[na++] = pc_atom(&cx, PA_TEXT, NULL, 0, st_texts[i]);
        f = rnd_expr(&cx, atoms, na, 5);
        for (i = 0; i < NM; i++) {
            v.def[i] = rnd() & 1;
            v.val[i] = (int)(rnd() % 5);
        }
        for (i = 0; i < NT; i++)
            v.known_t[i] = (int)(rnd() % 3) - 1;
        for (i = 0; i < NLEAF; i++)
            v.leaf[i] = (int)(rnd() % (unsigned)st_leaves[i]);
        val.ctx = &v;
        val.get = self_get;
        g = pc_constrain(&cx, f);
        bdd_vars(&cx.m, g, &vs);
        have = pc_cost_brute(&cx, g, &val, 16, &best);
        nc = pc_preconditions(&cx, f, &val, 3, cubes);
        if (g == BDD_FALSE) {
            if (nc)
                bad = 1;
        } else if (!nc) {
            bad = 2;
        } else if (have && pc_cube_weight(&cubes[0]) == best) {
            compared++;
            for (a = 0; a < vs.len; a++)
                if (pc_atom_at(&cx, vs.data[a])->kind == PA_LEAF) {
                    leafc++;
                    break;
                }
            multi += nc > 1;
        } else if (have && cubes[0].cost != best) {
            fprintf(stderr, "pcq: round %d cost %u, brute force %u\n", round,
                    cubes[0].cost, best);
            bad = 3;
        }
        for (m = 0; m < (int)nc; m++) {
            if (m && pc_cube_weight(&cubes[m]) <
                         pc_cube_weight(&cubes[m - 1]))
                bad = 4;
            pc_cube_free(&cubes[m]);
        }
        {
            SelfV v2 = v;
            PcValuation val2 = {&v2, self_get, NULL};
            const char **names;
            size_t nn;
            for (i = 0; i < NM; i++) {
                v2.def[i] = rnd() & 1;
                v2.val[i] = (int)(rnd() % 5);
            }
            for (i = 0; i < NLEAF; i++)
                v2.leaf[i] = (int)(rnd() % (unsigned)st_leaves[i]);
            nn = pc_diff(&cx, f, &val, &val2, &names);
            if ((pc_holds(&cx, f, &val) == pc_holds(&cx, f, &val2)) != (nn == 0))
                bad = 6;       /* equal values: nothing decides */
            free(names);
        }
        vec_free(&vs);
        pc_free(&cx);
    }
    if (!bad && (compared < 100 || leafc < 20)) {
        fprintf(stderr, "pcq selftest: only %d compared, %d with leaves\n",
                compared, leafc);
        bad = 7;
    }
    if (bad)
        fprintf(stderr, "pcq selftest: failure %d\n", bad);
    return bad;
}
