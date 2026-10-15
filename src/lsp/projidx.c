/* projidx.c - the project index in memory (projidx.h). */
#include "projidx.h"

#include <string.h>

#include "c/csymidx.h"

/* A chained hash map of nodes that start with {next, key}. */
typedef struct PiNode {
    struct PiNode *next;
    uint64_t key;
} PiNode;
typedef struct PiMap {
    PiNode **b;
    size_t nb, n;
} PiMap;

static PiNode *map_get(const PiMap *m, uint64_t key)
{
    PiNode *x;
    if (!m->nb)
        return NULL;
    for (x = m->b[key % m->nb]; x; x = x->next)
        if (x->key == key)
            return x;
    return NULL;
}

static void map_put(PiMap *m, PiNode *x)
{
    if (m->n >= m->nb) {                     /* load factor 1 */
        size_t nb = m->nb ? m->nb * 2 : 64, i;
        PiNode **b = xcalloc(nb, sizeof *b);
        for (i = 0; i < m->nb; i++) {
            PiNode *y = m->b[i], *nx;
            for (; y; y = nx) {
                nx = y->next;
                y->next = b[y->key % nb];
                b[y->key % nb] = y;
            }
        }
        free(m->b);
        m->b = b;
        m->nb = nb;
    }
    x->next = m->b[x->key % m->nb];
    m->b[x->key % m->nb] = x;
    m->n++;
}

static void map_del(PiMap *m, PiNode *x)
{
    PiNode **p = &m->b[x->key % m->nb];
    while (*p != x)
        p = &(*p)->next;
    *p = x->next;
    m->n--;
}

typedef struct PiTu {            /* the records one unit holds */
    PiNode *next;
    uint64_t tukey;
    uint64_t *d;                 /* the digests its manifest named */
    uint32_t nd;
    PiRec **r;                   /* those that loaded */
    uint32_t nr;
} PiTu;

typedef VEC(PiRec *) PiRecVec;

struct ProjIndex {
    RwLock l;
    PiMap recs, tus;
    PiRec **list;                /* dense, for the scan */
    size_t nlist, caplist;
    size_t bytes, names;
};

ProjIndex *pi_new(void)
{
    ProjIndex *pi = xcalloc(1, sizeof *pi);
    rwlock_init(&pi->l);
    return pi;
}

static size_t rec_bytes(const PiRec *r)
{
    return sizeof *r + (size_t)r->nsyms * sizeof *r->syms;
}

static void rec_free(PiRec *r)
{
    crec_unmap(&r->map);
    free(r->syms);
    free(r);
}

static void pitu_free(PiTu *t)
{
    free(t->d);
    free(t->r);
    free(t);
}

void pi_free(ProjIndex *pi)
{
    size_t i;
    for (i = 0; i < pi->tus.nb; i++) {
        PiNode *x = pi->tus.b[i], *nx;
        for (; x; x = nx) {
            nx = x->next;
            pitu_free((PiTu *)x);
        }
    }
    for (i = 0; i < pi->nlist; i++)
        rec_free(pi->list[i]);
    free(pi->tus.b);
    free(pi->recs.b);
    free(pi->list);
    rwlock_destroy(&pi->l);
    free(pi);
}

/* The names of a record: per declared key, its defining declaration if it has
 * one, else its first declaration (one per decl, as the open units list them);
 * then the macros. */
static PiRec *rec_load(PState *ps, uint64_t digest)
{
    char *path = crec_path(ps->store->dir, "rec", digest, "rec");
    CRecMap m;
    const char *why = crec_map(path, CREC_RECORD, ps->build_id, &m);
    const CRecView *v = &m.v;
    PiRec *r;
    int32_t *pick;
    uint32_t i, n = 0;
    free(path);
    if (why) {                                   /* the one corruption path (6.3) */
        pstate_reject_rec(ps, digest, why);
        return NULL;
    }
    r = xcalloc(1, sizeof *r);
    r->digest = digest;
    r->map = m;
    r->path = v->file ? v->strs + v->file->path : "";
    r->fhash = v->file ? v->file->content_hash : 0;
    r->syms = xcalloc((size_t)v->ndecls + v->nmacros + 1, sizeof *r->syms);
    pick = xmalloc(((size_t)v->nkeys + 1) * sizeof *pick);
    for (i = 0; i < v->nkeys; i++)
        pick[i] = -1;
    for (i = 0; i < v->ndecls; i++) {
        const CRecDecl *d = &v->decls[i];
        int32_t *p = &pick[d->key];
        if (*p < 0 || (d->role == CIX_DEF && v->decls[*p].role != CIX_DEF))
            *p = (int32_t)i;
    }
    for (i = 0; i < v->ndecls; i++) {
        const CRecDecl *d = &v->decls[i];
        const CRecKey *k = &v->keys[d->key];
        PiSym *s;
        if (pick[d->key] != (int32_t)i || k->kind == CIK_PARAM || k->kind == CIK_LABEL)
            continue;
        s = &r->syms[n++];
        s->name = v->strs + k->name;
        s->container = v->strs + d->container;
        s->off = d->off;
        s->len = d->len;
        s->kind = k->kind;
        s->flags = (uint8_t)((d->role == CIX_DEF ? PIF_DEF : 0) |
                             ((k->kind == CIK_FUNC || k->kind == CIK_OBJ) && k->linkage == 2
                                  ? PIF_EXT : 0));
    }
    for (i = 0; i < v->nmacros; i++) {
        const CRecMacro *mc = &v->macros[i];
        PiSym *s = &r->syms[n++];
        s->name = v->strs + mc->name;
        s->container = "";
        s->off = mc->off;
        s->len = mc->len;
        s->flags = (uint8_t)(PIF_MACRO | (mc->funclike ? PIF_FUNC : 0));
    }
    r->nsyms = n;
    free(pick);
    return r;
}

/* Under the write lock: tu no longer holds its records. */
static void tu_release(ProjIndex *pi, PiTu *t, PiRecVec *dead)
{
    uint32_t i;
    for (i = 0; i < t->nr; i++) {
        PiRec *r = t->r[i];
        if (--r->refs)
            continue;
        map_del(&pi->recs, (PiNode *)r);
        pi->list[r->slot] = pi->list[--pi->nlist];
        pi->list[r->slot]->slot = r->slot;
        pi->bytes -= rec_bytes(r);
        pi->names -= r->nsyms;
        vec_push(dead, r);
    }
    pi->bytes -= sizeof *t + (size_t)(t->nd + t->nr) * 8;
}

static void free_dead(PiRecVec *dead)
{
    size_t i;
    for (i = 0; i < dead->len; i++)
        rec_free(dead->data[i]);
    vec_free(dead);
}

void pi_set_tu(ProjIndex *pi, PState *ps, const PTu *tu)
{
    PiTu *t = (PiTu *)map_get(&pi->tus, tu->tukey);
    const uint64_t *d = tu->man ? tu->man->recs : NULL;
    uint32_t nd = tu->man ? tu->man->nrecs : 0, nr = 0, i;
    PiRecVec fresh = {0}, dead = {0};
    PiRec **r;
    if (t ? t->nd == nd && t->nr == nd && (!nd || !memcmp(t->d, d, nd * sizeof *d)) : !nd)
        return;                      /* nothing changed, and every record is in */
    r = xmalloc(((size_t)nd + 1) * sizeof *r);
    for (i = 0; i < nd; i++) {                   /* outside the lock */
        PiRec *x = (PiRec *)map_get(&pi->recs, d[i]);
        if (!x && (x = rec_load(ps, d[i])) != NULL)
            vec_push(&fresh, x);
        if (x)
            r[nr++] = x;
    }
    rwlock_wrlock(&pi->l);
    for (i = 0; i < fresh.len; i++) {
        PiRec *x = fresh.data[i];
        if (pi->nlist == pi->caplist) {
            pi->caplist = pi->caplist ? pi->caplist * 2 : 256;
            pi->list = xrealloc(pi->list, pi->caplist * sizeof *pi->list);
        }
        x->slot = pi->nlist;
        pi->list[pi->nlist++] = x;
        map_put(&pi->recs, (PiNode *)x);
        pi->bytes += rec_bytes(x);
        pi->names += x->nsyms;
    }
    for (i = 0; i < nr; i++)
        r[i]->refs++;
    if (t)
        tu_release(pi, t, &dead);                /* after the new refs: shared ones stay */
    else {
        t = xcalloc(1, sizeof *t);
        t->tukey = tu->tukey;
        map_put(&pi->tus, (PiNode *)t);
    }
    free(t->d);
    free(t->r);
    t->d = nd ? xmalloc(nd * sizeof *d) : NULL;
    if (nd)
        memcpy(t->d, d, nd * sizeof *d);
    t->nd = nd;
    t->r = r;
    t->nr = nr;
    pi->bytes += sizeof *t + (size_t)(nd + nr) * 8;
    rwlock_unlock(&pi->l);
    vec_free(&fresh);
    free_dead(&dead);
}

void pi_prune(ProjIndex *pi, const PState *ps)
{
    size_t n = ps->tus.len, i;
    uint64_t *keys = xmalloc((n + 1) * sizeof *keys);
    PiRecVec dead = {0};
    VEC(PiTu *) gone = {0};
    for (i = 0; i < n; i++)
        keys[i] = ps->tus.data[i]->tukey;
    qsort(keys, n, sizeof *keys, u64_cmp);
    for (i = 0; i < pi->tus.nb; i++) {
        PiNode *x;
        for (x = pi->tus.b[i]; x; x = x->next)
            if (!bsearch(&x->key, keys, n, sizeof *keys, u64_cmp))
                vec_push(&gone, (PiTu *)x);
    }
    free(keys);
    if (gone.len) {
        rwlock_wrlock(&pi->l);
        for (i = 0; i < gone.len; i++) {
            tu_release(pi, gone.data[i], &dead);
            map_del(&pi->tus, (PiNode *)gone.data[i]);
        }
        rwlock_unlock(&pi->l);
        for (i = 0; i < gone.len; i++)
            pitu_free(gone.data[i]);
        free_dead(&dead);
    }
    vec_free(&gone);
}

void pi_rdlock(ProjIndex *pi) { rwlock_rdlock(&pi->l); }
void pi_unlock(ProjIndex *pi) { rwlock_unlock(&pi->l); }
size_t pi_nrecs(const ProjIndex *pi) { return pi->nlist; }
const PiRec *pi_rec(const ProjIndex *pi, size_t i) { return pi->list[i]; }

PiStats pi_stats(ProjIndex *pi)
{
    PiStats s;
    rwlock_rdlock(&pi->l);
    s.bytes = pi->bytes + pi->recs.nb * sizeof(void *) + pi->tus.nb * sizeof(void *) +
              pi->caplist * sizeof(void *);
    s.names = pi->names;
    s.recs = pi->nlist;
    s.tus = pi->tus.n;
    rwlock_unlock(&pi->l);
    return s;
}
