/* intern.c - concurrent identifier interner. */
#include "intern.h"

#include "c/ckw.h"
#include "hash.h"

#include <string.h>

static InternTable *table_new(size_t cap)
{
    InternTable *t = xcalloc(1, sizeof(InternTable) + sizeof(Ident *) * (cap - 1));
    t->cap = cap;
    return t;
}

void interner_init(Interner *in)
{
    int i;
    memset(in, 0, sizeof *in);
    for (i = 0; i < INTERN_SHARDS; i++) {
        InternShard *s = &in->shards[i];
        mutex_init(&s->m);
        arena_init(&s->arena);
        s->table = table_new(256);
    }
    mutex_init(&in->page_lock);
    in->next_id = 1; /* id 0 means "no identifier" */
    in->pages[0] = xcalloc(ID_PAGE, sizeof(Ident *));
    {
        /* before the interner is shared: plain writes */
        const CKwSpelling *k;
        for (k = ckw_spellings; k->s; k++)
            intern_cstr(in, k->s)->ckw = k->kw;
    }
}

void interner_free(Interner *in)
{
    uint32_t p;
    int i;
    for (i = 0; i < INTERN_SHARDS; i++) {
        InternShard *s = &in->shards[i];
        size_t k;
        for (k = 0; k < s->retired.len; k++)
            free(s->retired.data[k]);
        vec_free(&s->retired);
        free(s->table);
        arena_free(&s->arena);
        mutex_destroy(&s->m);
    }
    for (p = 0; p < ID_PAGES; p++)
        free(in->pages[p]);
    mutex_destroy(&in->page_lock);
}

Interner *interner_new(void)
{
    Interner *in = xmalloc(sizeof *in);
    interner_init(in);
    in->refs = 1;
    return in;
}

Interner *interner_retain(Interner *in)
{
    atomic_add_u32(&in->refs, 1);
    return in;
}

void interner_release(Interner *in)
{
    if (!in || atomic_add_u32(&in->refs, (uint32_t)-1) != 1)
        return;
    interner_free(in);
    free(in);
}

uint32_t interner_count(const Interner *in)
{
    return atomic_load_u32(&in->next_id);
}

static Ident *probe(InternTable *t, uint32_t h, const char *s, size_t n,
                    size_t *empty)
{
    size_t k;
    for (k = h & (t->cap - 1);; k = (k + 1) & (t->cap - 1)) {
        Ident *id = atomic_load_ptr((void *const *)&t->slot[k]);
        if (!id) {
            *empty = k;
            return NULL;
        }
        if (id->hash == h && id->len == n && memcmp(id->str, s, n) == 0)
            return id;
    }
}

static void register_id(Interner *in, Ident *id)
{
    uint32_t page = id->id >> ID_PAGE_BITS;
    Ident **pg;
    if (page >= ID_PAGES)
        fatal("too many identifiers");
    pg = atomic_load_ptr((void *const *)&in->pages[page]);
    if (!pg) {
        mutex_lock(&in->page_lock);
        pg = in->pages[page];
        if (!pg) {
            pg = xcalloc(ID_PAGE, sizeof(Ident *));
            atomic_store_ptr((void **)&in->pages[page], pg);
        }
        mutex_unlock(&in->page_lock);
    }
    atomic_store_ptr((void **)&pg[id->id & (ID_PAGE - 1)], id);
}

static void grow(InternShard *s)
{
    InternTable *old = s->table, *nt = table_new(old->cap * 2);
    size_t i;
    for (i = 0; i < old->cap; i++) {
        Ident *id = old->slot[i];
        size_t k;
        if (!id)
            continue;
        for (k = id->hash & (nt->cap - 1); nt->slot[k]; k = (k + 1) & (nt->cap - 1))
            ;
        nt->slot[k] = id;
    }
    atomic_store_ptr((void **)&s->table, nt);
    vec_push(&s->retired, old); /* readers may still be probing it */
}

Ident *intern(Interner *in, const char *str, size_t n)
{
    uint32_t h = hash_bytes(str, n);
    InternShard *s = &in->shards[h >> 26]; /* top 6 bits pick the shard */
    InternTable *t = atomic_load_ptr((void *const *)&s->table);
    size_t empty;
    Ident *id = probe(t, h, str, n, &empty);
    if (id)
        return id;

    mutex_lock(&s->m);
    t = s->table;
    id = probe(t, h, str, n, &empty);
    if (!id) {
        id = NEW(&s->arena, Ident);
        id->str = arena_strndup(&s->arena, str, n);
        id->len = (uint32_t)n;
        id->hash = h;
        for (size_t k = 0; k < n; k++)
            if (str[k] == '\\' || (unsigned char)str[k] >= 0x80)
                id->ext = 1;
        id->digest = hash64(str, n, 0);
        id->id = atomic_add_u32(&in->next_id, 1);
        register_id(in, id);
        atomic_store_ptr((void **)&t->slot[empty], id);
        if (++s->count * 2 > t->cap)
            grow(s);
    }
    mutex_unlock(&s->m);
    return id;
}

Ident *intern_find(const Interner *in, const char *str, size_t n)
{
    uint32_t h = hash_bytes(str, n);
    const InternShard *s = &in->shards[h >> 26];
    size_t empty;
    return probe(atomic_load_ptr((void *const *)&s->table), h, str, n, &empty);
}

Ident *intern_cstr(Interner *in, const char *s)
{
    return intern(in, s, strlen(s));
}
