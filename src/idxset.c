/* idxset.c - see idxset.h. */
#include <stdio.h>
#include <string.h>

#include "idxset.h"

static const uint32_t none;   /* the probe of a set with no table yet: never written (reserve first) */

uint32_t *idxset_probe(const IdxSet *s, uint64_t h, IdxEq eq, void *ctx)
{
    size_t m, k;
    if (!s->cap)
        return (uint32_t *)&none;
    m = s->cap - 1;
    for (k = h & m; s->slot[k]; k = (k + 1) & m)
        if (eq(ctx, s->slot[k] - 1))
            break;
    return &s->slot[k];
}

void idxset_reserve(IdxSet *s, size_t n, IdxHash hash, void *ctx)
{
    size_t cap = s->cap ? s->cap : 64, i, k;
    uint32_t *ns;
    if (s->cap && 2 * n <= s->cap)
        return;
    while (cap < 2 * n)
        cap *= 2;
    ns = xcalloc(cap, sizeof *ns);
    for (i = 0; i < s->cap; i++)
        if (s->slot[i]) {
            for (k = hash(ctx, s->slot[i] - 1) & (cap - 1); ns[k]; k = (k + 1) & (cap - 1))
                ;
            ns[k] = s->slot[i];
        }
    free(s->slot);
    s->slot = ns;
    s->cap = cap;
}

void idxset_free(IdxSet *s)
{
    free(s->slot);
    memset(s, 0, sizeof *s);
}

typedef struct PoolKey {
    StrPool *p;
    const char *s;
    size_t n;
} PoolKey;

static bool pool_eq(void *ctx, uint32_t off)
{
    PoolKey *k = ctx;
    const char *t = k->p->b.data + off;
    return !strncmp(t, k->s, k->n) && !t[k->n];
}

static uint64_t pool_hash(void *ctx, uint32_t off)
{
    const char *t = ((PoolKey *)ctx)->p->b.data + off;
    return hash_bytes(t, strlen(t));
}

uint32_t strpool_add(StrPool *p, const char *s, size_t n)
{
    PoolKey k = {p, s, n};
    uint32_t *e;
    if (!n)
        return 0;
    if (!p->b.len)
        sb_putc(&p->b, 0);
    idxset_reserve(&p->set, p->set.n + 1, pool_hash, &k);
    e = idxset_probe(&p->set, hash_bytes(s, n), pool_eq, &k);
    if (!*e) {
        uint32_t off = (uint32_t)p->b.len;
        sb_putn(&p->b, s, n);
        sb_putc(&p->b, 0);
        idxset_set(&p->set, e, off);
    }
    return *e - 1;
}

void strpool_free(StrPool *p)
{
    sb_free(&p->b);
    idxset_free(&p->set);
}

/* ---- selftest -------------------------------------------------------------- */

typedef struct TestCtx {
    const uint32_t *keys;
    uint32_t key;
    unsigned mask;
} TestCtx;

static bool test_eq(void *ctx, uint32_t i)
{
    TestCtx *t = ctx;
    return t->keys[i] == t->key;
}

static uint64_t test_hash(void *ctx, uint32_t i)
{
    TestCtx *t = ctx;
    return t->keys[i] & t->mask;    /* a poor hash: long probe chains */
}

int idxset_selftest(void)
{
    enum { N = 5000 };
    uint32_t keys[N];
    IdxSet s = {0};
    StrPool sp = {0};
    TestCtx t = {keys, 0, 0xff};
    char buf[16];
    uint32_t i, off[N], bad = 0, *e;
    for (i = 0; i < N; i++) {
        keys[i] = i * 7;
        t.key = keys[i];
        idxset_reserve(&s, s.n + 1, test_hash, &t);
        e = idxset_probe(&s, t.key & t.mask, test_eq, &t);
        if (*e)
            bad++;                  /* distinct keys: never found before */
        idxset_set(&s, e, i);
    }
    for (i = 0; i < N; i++) {
        t.key = keys[i];
        e = idxset_probe(&s, t.key & t.mask, test_eq, &t);
        if (*e != i + 1)
            bad++;
    }
    t.key = 1;                      /* absent: ends on an empty slot */
    if (*idxset_probe(&s, t.key & t.mask, test_eq, &t))
        bad++;
    if (s.n != N)
        bad++;
    idxset_set(&s, idxset_probe(&s, 0, test_eq, &(TestCtx){keys, keys[0], 0xff}), 3);   /* replace: n stays */
    if (s.n != N)
        bad++;
    for (i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "s%u", i % 1000);
        off[i] = strpool_add(&sp, buf, strlen(buf));
        if (off[i] != off[i % 1000] || strcmp(sp.b.data + off[i], buf))
            bad++;
    }
    if (strpool_add(&sp, "", 0) != 0 || sp.set.n != 1000)
        bad++;
    idxset_free(&s);
    strpool_free(&sp);
    if (bad)
        fprintf(stderr, "idxset: %u failures\n", bad);
    return bad != 0;
}
