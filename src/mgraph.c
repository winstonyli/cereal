/* mgraph.c - the macro dependency graph. */
#include "mgraph.h"

#include <string.h>

/* ## can make a name no token spells when an identifier or a parameter
 * is on its left: `x ## y`, `a ## 1`, `p ## q` with p a parameter.  A
 * literal on the left cannot (`1 ## x` is a pp-number), nor can a prefix
 * pasted onto a literal (`L ## "s"` is a string). */
static bool body_is_open(const Macro *m)
{
    uint32_t b;
    for (b = 1; b + 1 < m->body_len; b++) {
        const Tok *l = &m->body[b - 1], *r = &m->body[b + 1];
        if (!tok_is_punct(&m->body[b], P_HASHHASH))
            continue;
        if (l->flags & TF_PARAM)
            return true;
        if (l->kind == TK_IDENT && r->kind != TK_STRING && r->kind != TK_CHAR)
            return true;
    }
    return false;
}

void mgraph_build(MacroGraph *g, PP *pp)
{
    uint32_t *seen, *fill;
    size_t i, total = 0;
    memset(g, 0, sizeof *g);
    g->pp = pp;
    g->n = pp->macros.len;
    g->nodes = xcalloc(g->n + 1, sizeof *g->nodes);
    g->nidents = interner_count(pp->in);
    g->rev_start = xcalloc(g->nidents + 1, sizeof *g->rev_start);
    seen = xcalloc(g->nidents + 1, sizeof *seen);
    for (i = 0; i < g->n; i++) {
        Macro *m = pp->macros.data[i];
        MNode *nd = &g->nodes[m->id];
        uint32_t b, stamp = (uint32_t)i + 1;
        nd->m = m;
        nd->open = body_is_open(m);
        nd->names = m->body_len ? xmalloc(sizeof(Ident *) * m->body_len) : NULL;
        for (b = 0; b < m->body_len; b++) {
            const Tok *t = &m->body[b];
            Ident *id;
            /* every identifier that is not a parameter may be looked up,
             * including ## operands (a failed paste leaves both) */
            if (t->kind != TK_IDENT || (t->flags & TF_PARAM))
                continue;
            id = pp_ident(pp, t);
            if (seen[id->id] == stamp)
                continue;
            seen[id->id] = stamp;
            nd->names[nd->nnames++] = id;
            g->rev_start[id->id + 1]++;
            total++;
        }
    }
    for (i = 0; i < g->nidents; i++)
        g->rev_start[i + 1] += g->rev_start[i];
    g->rev = xmalloc(sizeof(Macro *) * (total + 1));
    fill = xcalloc(g->nidents + 1, sizeof *fill);
    for (i = 0; i < g->n; i++) { /* definition order within each list */
        const MNode *nd = &g->nodes[pp->macros.data[i]->id];
        uint32_t k;
        for (k = 0; k < nd->nnames; k++) {
            uint32_t id = nd->names[k]->id;
            g->rev[g->rev_start[id] + fill[id]++] = nd->m;
        }
    }
    free(fill);
    free(seen);
}

void mgraph_free(MacroGraph *g)
{
    size_t i;
    for (i = 0; i < g->n; i++)
        free(g->nodes[i].names);
    free(g->nodes);
    free(g->rev_start);
    free(g->rev);
    memset(g, 0, sizeof *g);
}

Macro *const *mgraph_users(const MacroGraph *g, const Ident *name, size_t *n)
{
    if (name->id >= g->nidents) {
        *n = 0;
        return NULL;
    }
    *n = g->rev_start[name->id + 1] - g->rev_start[name->id];
    return g->rev + g->rev_start[name->id];
}

/* ---- closure --------------------------------------------------------- */

void mgraph_closure_with(const MacroGraph *g, MScratch *sc,
                         Ident *const *names, size_t n, uint32_t version,
                         MClosure *out)
{
    size_t need = interner_count(g->pp->in) + 1, head = 0, i;
    uint32_t gen;
    if (sc->cap < need || sc->gen == UINT32_MAX) {
        free(sc->mark);
        sc->cap = need + need / 2;
        sc->mark = xcalloc(sc->cap, sizeof *sc->mark);
        sc->gen = 0;
    }
    gen = ++sc->gen;
    memset(out, 0, sizeof *out);
    for (i = 0; i < n; i++)
        if (sc->mark[names[i]->id] != gen) {
            sc->mark[names[i]->id] = gen;
            vec_push(&out->names, names[i]);
        }
    /* breadth-first over names; out->names doubles as the queue */
    while (head < out->names.len) {
        Ident *id = out->names.data[head++];
        Macro *m = macro_at_version(g->pp->mt, id, version);
        const MNode *nd;
        uint32_t k;
        if (!m)
            continue;
        vec_push(&out->macros, m);
        nd = mgraph_node(g, m);
        if (!nd)
            continue;
        if (nd->open)
            out->open = true;
        for (k = 0; k < nd->nnames; k++)
            if (sc->mark[nd->names[k]->id] != gen) {
                sc->mark[nd->names[k]->id] = gen;
                vec_push(&out->names, nd->names[k]);
            }
    }
}

void mgraph_closure(const MacroGraph *g, Ident *const *names, size_t n,
                    uint32_t version, MClosure *out)
{
    MScratch sc;
    memset(&sc, 0, sizeof sc);
    mgraph_closure_with(g, &sc, names, n, version, out);
    mscratch_free(&sc);
}

void mscratch_free(MScratch *s)
{
    free(s->mark);
    memset(s, 0, sizeof *s);
}

void mclosure_free(MClosure *c)
{
    vec_free(&c->names);
    vec_free(&c->macros);
}

/* ---- cycles: iterative Tarjan (generated code nests deep) ------------- */

typedef struct TFrame {
    uint32_t v;               /* node index (Macro.id) */
    uint32_t next;            /* next name to follow */
} TFrame;

static Macro *target(const MacroGraph *g, const MNode *nd, uint32_t k,
                     uint32_t version)
{
    return macro_at_version(g->pp->mt, nd->names[k], version);
}

static int macro_id_cmp(const void *a, const void *b)
{
    const Macro *x = *(Macro *const *)a, *y = *(Macro *const *)b;
    return x->id < y->id ? -1 : x->id > y->id;
}

static int comp_cmp(const void *a, const void *b)
{
    return macro_id_cmp(*(Macro **const *)a, *(Macro **const *)b);
}

Macro ***mgraph_cycles(const MacroGraph *g, uint32_t version, Arena *a,
                       size_t *ncomps)
{
    Macro ***out;
    uint32_t *index = xcalloc(g->n + 1, sizeof *index); /* 0: unvisited */
    uint32_t *low = xcalloc(g->n + 1, sizeof *low);
    bool *on = xcalloc(g->n + 1, sizeof *on);
    VEC(uint32_t) stack = {0};
    VEC(TFrame) call = {0};
    VEC(Macro **) comps = {0};
    uint32_t counter = 0;
    size_t s;
    for (s = 0; s < g->n; s++) {
        if (index[s] || !macro_live_at(g->nodes[s].m, version))
            continue;
        {
            TFrame f;
            f.v = (uint32_t)s;
            f.next = 0;
            vec_push(&call, f);
            index[s] = low[s] = ++counter;
            vec_push(&stack, (uint32_t)s);
            on[s] = true;
        }
        while (call.len) {
            TFrame *f = &vec_last(&call);
            const MNode *nd = &g->nodes[f->v];
            if (f->next < nd->nnames) {
                Macro *t = target(g, nd, f->next++, version);
                uint32_t w;
                if (!t || t->id >= g->n)
                    continue;
                w = t->id;
                if (!index[w]) {
                    TFrame nf;
                    nf.v = w;
                    nf.next = 0;
                    index[w] = low[w] = ++counter;
                    vec_push(&stack, w);
                    on[w] = true;
                    vec_push(&call, nf); /* f is invalid from here */
                } else if (on[w] && index[w] < low[f->v]) {
                    low[f->v] = index[w];
                }
                continue;
            }
            /* all edges done: pop */
            {
                uint32_t v = f->v;
                call.len--;
                if (call.len) {
                    uint32_t u = vec_last(&call).v;
                    if (low[v] < low[u])
                        low[u] = low[v];
                }
                if (low[v] == index[v]) {
                    size_t start = stack.len;
                    do
                        start--;
                    while (stack.data[start] != v);
                    if (stack.len - start > 1) {
                        size_t k, m = stack.len - start;
                        Macro **c = NEW_ARRAY(a, Macro *, m + 1);
                        for (k = 0; k < m; k++)
                            c[k] = g->nodes[stack.data[start + k]].m;
                        qsort(c, m, sizeof *c, macro_id_cmp);
                        vec_push(&comps, c);
                    }
                    for (; stack.len > start; stack.len--)
                        on[stack.data[stack.len - 1]] = false;
                }
            }
        }
    }
    qsort(comps.data, comps.len, sizeof *comps.data, comp_cmp);
    out = NEW_ARRAY(a, Macro **, comps.len + 1);
    if (comps.len)
        memcpy(out, comps.data, sizeof(Macro **) * comps.len);
    *ncomps = comps.len;
    vec_free(&comps);
    vec_free(&call);
    vec_free(&stack);
    free(index);
    free(low);
    free(on);
    return out;
}
