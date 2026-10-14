/* pstate.c - the project index's view of its units and files (pstate.h,
 * docs/B5_DESIGN.md 7, 8.2). */
#include "pstate.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "srcmgr.h"

#define RACY_NS 2000000000ull   /* 7.2: stat data this close to its time is not trusted */

static uint64_t realtime_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mtime_of(const struct stat *st)
{
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
}

const char *pstate_why(PStale s)
{
    static const char *const names[] = {"fresh", "new", "file", "miss", "lost"};
    return names[s];
}

/* ---- the path table ---------------------------------------------------------- */

static size_t path_slot(PPath *const *slots, size_t cap, const char *path)
{
    size_t i = hash64(path, strlen(path), 0) & (cap - 1);
    while (slots[i] && strcmp(slots[i]->path, path))
        i = (i + 1) & (cap - 1);
    return i;
}

static PPath *path_get(PState *ps, const char *path, bool create)
{
    size_t i;
    if (ps->cap) {
        i = path_slot(ps->slots, ps->cap, path);
        if (ps->slots[i])
            return ps->slots[i];
    }
    if (!create)
        return NULL;
    if ((ps->npaths + 1) * 2 > ps->cap) {
        size_t nc = ps->cap ? ps->cap * 2 : 1024, k;
        PPath **ns = xcalloc(nc, sizeof *ns);
        for (k = 0; k < ps->cap; k++)
            if (ps->slots[k])
                ns[path_slot(ns, nc, ps->slots[k]->path)] = ps->slots[k];
        free(ps->slots);
        ps->slots = ns;
        ps->cap = nc;
    }
    {
        PPath *p = NEW(&ps->a, PPath);
        p->path = arena_strdup(&ps->a, path);
        ps->slots[path_slot(ps->slots, ps->cap, p->path)] = p;
        ps->npaths++;
        return p;
    }
}

PPath *pstate_path(PState *ps, const char *path)
{
    return path_get(ps, path, false);
}

/* stat only: the size and mtime; the hash is not known until settle. */
static void look(PPath *p, uint64_t now)
{
    struct stat st;
    p->seen = true;
    p->taken_ns = now;
    if (stat(p->path, &st) != 0 || !S_ISREG(st.st_mode)) {
        p->size = 0;
        p->mtime_ns = 0;
        p->hash = 0;
        p->known = true;
        p->racy = false;
        return;
    }
    p->size = (uint32_t)st.st_size;
    p->mtime_ns = mtime_of(&st);
    p->known = false;
    p->racy = p->mtime_ns + RACY_NS > now;
}

/* Reads and hashes (or notes the absence of) the file: the state is then
 * what is on disk. */
static void hash_now(PPath *p, uint64_t now)
{
    p->seen = true;
    p->taken_ns = now;
    if (!crec_stat_hash(p->path, &p->size, &p->mtime_ns, &p->hash)) {
        p->size = 0;
        p->mtime_ns = 0;
        p->hash = 0;
        p->racy = false;
    } else {
        p->racy = p->mtime_ns + RACY_NS > now;
    }
    p->known = true;
}

/* An entry of a manifest started at started_ns seeds the file's hash when the
 * stat data is the same and old enough to trust (7.2). */
static void seed(PPath *p, const PFil *e, uint64_t started_ns, uint64_t now)
{
    if (!p->seen)
        look(p, now);
    if (!p->known && e->size == p->size && e->mtime_ns == p->mtime_ns &&
        e->mtime_ns + RACY_NS <= started_ns) {
        p->hash = e->hash;
        p->known = true;
    }
}

/* One sweep step (7.2): a file whose stat data is unchanged and trustworthy
 * is not read.  True if the hash changed. */
static bool refresh(PPath *p, uint64_t now)
{
    uint64_t old = p->hash;
    struct stat st;
    if (p->known && !p->racy) {
        bool exists = stat(p->path, &st) == 0 && S_ISREG(st.st_mode);
        if (exists ? (p->size == (uint32_t)st.st_size && p->mtime_ns == mtime_of(&st))
                   : p->mtime_ns == 0)
            return false;
    }
    hash_now(p, now);
    return p->hash != old;
}

/* ---- chunks and manifests ------------------------------------------------------ */

static void report(PState *ps, const char *what, const char *path, const char *why)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s %s: %s", what, path, why);
    vec_push(&ps->log, xstrdup(buf));
    ps->corrupt++;
}

static size_t chunk_slot(PChunk *const *slots, size_t cap, uint64_t digest)
{
    size_t i = hash64(&digest, sizeof digest, 0) & (cap - 1);
    while (slots[i] && slots[i]->digest != digest)
        i = (i + 1) & (cap - 1);
    return i;
}

static void chunk_free(PState *ps, PChunk *c)
{
    size_t i, j;
    free(c->f);
    /* remove with backward-shift deletion so probes stay intact */
    i = chunk_slot(ps->chunks, ps->chunk_cap, c->digest);
    ps->chunks[i] = NULL;
    for (j = (i + 1) & (ps->chunk_cap - 1); ps->chunks[j]; j = (j + 1) & (ps->chunk_cap - 1)) {
        PChunk *m = ps->chunks[j];
        ps->chunks[j] = NULL;
        ps->chunks[chunk_slot(ps->chunks, ps->chunk_cap, m->digest)] = m;
    }
    ps->nchunks--;
    free(c);
}

static void chunk_unref(PState *ps, PChunk *c)
{
    if (c && --c->refs == 0)
        chunk_free(ps, c);
}

/* The chunk with this digest, loaded if needed; NULL (and the file removed,
 * if we may) when it cannot be read. */
static PChunk *chunk_get(PState *ps, uint64_t digest)
{
    PChunk *c;
    size_t i;
    CRecMap m;
    char *path;
    const char *why;
    if (ps->chunk_cap) {
        i = chunk_slot(ps->chunks, ps->chunk_cap, digest);
        if ((c = ps->chunks[i]) != NULL) {
            c->refs++;
            return c;
        }
    }
    path = crec_path(ps->store->dir, "sys", digest, "sys");
    why = crec_map(path, CREC_CHUNK, ps->build_id, &m);
    if (why) {
        if (strcmp(why, "cannot open")) {
            report(ps, "rejected", path, why);
            if (ps->store->writer)
                unlink(path);
        }
        free(path);
        return NULL;
    }
    free(path);
    if ((ps->nchunks + 1) * 2 > ps->chunk_cap) {
        size_t nc = ps->chunk_cap ? ps->chunk_cap * 2 : 256, k;
        PChunk **ns = xcalloc(nc, sizeof *ns);
        for (k = 0; k < ps->chunk_cap; k++)
            if (ps->chunks[k])
                ns[chunk_slot(ns, nc, ps->chunks[k]->digest)] = ps->chunks[k];
        free(ps->chunks);
        ps->chunks = ns;
        ps->chunk_cap = nc;
    }
    c = xcalloc(1, sizeof *c);
    c->digest = digest;
    c->refs = 1;
    c->n = m.v.nsyst;
    c->f = xcalloc(c->n, sizeof *c->f);
    for (i = 0; i < c->n; i++) {
        const CRecSys *e = &m.v.syst[i];
        c->f[i].p = path_get(ps, m.v.strs + e->path, true);
        c->f[i].hash = e->content_hash;
        c->f[i].mtime_ns = e->mtime_ns;
        c->f[i].size = e->size;
    }
    ps->chunks[chunk_slot(ps->chunks, ps->chunk_cap, digest)] = c;
    ps->nchunks++;
    crec_unmap(&m);
    return c;
}

static int u64_cmp(const void *pa, const void *pb)
{
    uint64_t a = *(const uint64_t *)pa, b = *(const uint64_t *)pb;
    return a < b ? -1 : a > b;
}

static size_t sort_unique_u64(uint64_t *v, size_t n)
{
    size_t i, k = 0;
    if (n > 1)
        qsort(v, n, sizeof *v, u64_cmp);
    for (i = 0; i < n; i++)
        if (!k || v[k - 1] != v[i])
            v[k++] = v[i];
    return k;
}

static void man_free(PState *ps, PMan *m)
{
    uint32_t i;
    if (!m)
        return;
    for (i = 0; i < m->nsysc; i++)
        chunk_unref(ps, m->chunks[i]);
    free(m->fils);
    free(m->chunks);
    free(m->sysc);
    free(m->miss);
    free(m->recs);
    free(m);
}

static void seed_chunk(const PChunk *c, uint64_t started_ns, uint64_t now)
{
    uint32_t i;
    for (i = 0; i < c->n; i++)
        seed(c->f[i].p, &c->f[i], started_ns, now);
}

/* tu's manifest from the store, or NULL (a rejected file is removed). */
static PMan *man_load(PState *ps, const PTu *tu, uint64_t now)
{
    char *path = crec_path(ps->store->dir, "tu", tu->tukey, "tu");
    CRecMap m;
    const char *why = crec_map(path, CREC_MANIFEST, ps->build_id, &m);
    PMan *man;
    uint32_t i;
    size_t nrec = 0;
    if (why) {
        if (strcmp(why, "cannot open")) {
            report(ps, "rejected", path, why);
            if (ps->store->writer)
                unlink(path);
        }
        free(path);
        return NULL;
    }
    if (m.v.tu->flag_hash != tu->flag_hash || strcmp(m.v.strs + m.v.tu->main_path, tu->main)) {
        report(ps, "rejected", path, "another unit's manifest");
        if (ps->store->writer)
            unlink(path);
        crec_unmap(&m);
        free(path);
        return NULL;
    }
    free(path);
    man = xcalloc(1, sizeof *man);
    man->tukey = tu->tukey;
    man->started_ns = m.v.tu->started_ns;
    man->written_ns = m.v.tu->written_ns;
    man->status = m.v.tu->status;
    man->errors = m.v.tu->errors;
    man->bytes_read = m.v.tu->bytes_read;
    man->index_ms = m.v.tu->index_ms;
    man->nfils = m.v.nfils;
    man->fils = xcalloc(man->nfils + 1, sizeof *man->fils);
    man->recs = xcalloc(man->nfils + 1, sizeof *man->recs);
    for (i = 0; i < man->nfils; i++) {
        const CRecFil *e = &m.v.fils[i];
        PFil *f = &man->fils[i];
        f->p = path_get(ps, m.v.strs + e->path, true);
        f->hash = e->content_hash;
        f->mtime_ns = e->mtime_ns;
        f->size = e->size;
        f->flags = e->flags;
        seed(f->p, f, man->started_ns, now);
        if (e->record_digest)
            man->recs[nrec++] = e->record_digest;
    }
    man->nrecs = (uint32_t)sort_unique_u64(man->recs, nrec);
    man->nmiss = m.v.nmiss;
    man->miss = xcalloc(man->nmiss + 1, sizeof *man->miss);
    for (i = 0; i < man->nmiss; i++) {
        man->miss[i].spelled = arena_strdup(&ps->a, m.v.strs + m.v.miss[i].spelled);
        man->miss[i].from = arena_strdup(&ps->a, m.v.strs + m.v.miss[i].from_path);
        man->miss[i].angled = m.v.miss[i].angled != 0;
    }
    man->nsysc = m.v.nsysc;
    man->sysc = xcalloc(man->nsysc + 1, sizeof *man->sysc);
    if (man->nsysc)
        memcpy(man->sysc, m.v.sysc, man->nsysc * sizeof *man->sysc);
    man->chunks = xcalloc(man->nsysc + 1, sizeof *man->chunks);
    for (i = 0; i < man->nsysc; i++) {
        man->chunks[i] = chunk_get(ps, man->sysc[i]);
        if (man->chunks[i])
            seed_chunk(man->chunks[i], man->started_ns, now);
        else
            man->lost = true;
    }
    crec_unmap(&m);
    return man;
}

/* ---- staleness ------------------------------------------------------------------ */

/* The directories an include of tu is searched in (7.2, MISS), from its options. */
static void tu_dirs(PState *ps, PTu *tu)
{
    Options o;
    size_t i;
    const char *cwd;
    options_init(&o);
    config_options_entry(ps->cfg, tu->entry, &o.rsp, &o, NULL);
    cwd = o.cwd ? o.cwd : "/";
    tu->ndirs = (uint32_t)(o.pp.quote_dirs.len + o.pp.angle_dirs.len + o.pp.system_dirs.len);
    tu->dirs = xcalloc(tu->ndirs + 1, sizeof *tu->dirs);
    tu->first_angle = (uint32_t)o.pp.quote_dirs.len;
#define ADD(V)                                                                  \
    for (i = 0; i < (V).len; i++) {                                             \
        const char *d = (V).data[i];                                            \
        tu->dirs[k++] = xstrdup(d[0] == '/' ? d : path_join(&ps->a, cwd, d));   \
    }
    {
        size_t k = 0;
        ADD(o.pp.quote_dirs)
        ADD(o.pp.angle_dirs)
        ADD(o.pp.system_dirs)
    }
#undef ADD
    options_free(&o);
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Does an include a unit missed resolve now (pp_find_include's search)? */
static bool miss_resolves(PState *ps, PTu *tu, const PMiss *m)
{
    uint32_t i;
    char *p;
    if (m->spelled[0] == '/')
        return is_file(m->spelled);
    if (!m->angled) {
        Arena tmp;
        bool hit;
        arena_init(&tmp);
        p = path_join(&tmp, path_dirname(&tmp, m->from), m->spelled);
        hit = is_file(p);
        arena_free(&tmp);
        if (hit)
            return true;
    }
    if (!tu->dirs)
        tu_dirs(ps, tu);
    for (i = m->angled ? tu->first_angle : 0; i < tu->ndirs; i++) {
        bool hit;
        p = xmalloc(strlen(tu->dirs[i]) + strlen(m->spelled) + 2);
        sprintf(p, "%s/%s", tu->dirs[i], m->spelled);
        hit = is_file(p);
        free(p);
        if (hit)
            return true;
    }
    return false;
}

static const PPath *differs(const PFil *f, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (f[i].p->hash != f[i].hash)
            return f[i].p;
    return NULL;
}

static void eval_tu(PState *ps, PTu *tu, uint64_t now, size_t *became)
{
    PStale s = PS_FRESH;
    const char *why = NULL;
    const PMan *m = tu->man;
    if (!m) {
        s = PS_NEW;
    } else if (m->lost) {
        s = PS_LOST;
    } else {
        const PPath *d = differs(m->fils, m->nfils);
        uint32_t i;
        for (i = 0; !d && i < m->nsysc; i++)
            d = differs(m->chunks[i]->f, m->chunks[i]->n);
        if (d) {
            s = PS_FILE;
            why = d->path;
        }
        for (i = 0; s == PS_FRESH && i < m->nmiss; i++)
            if (miss_resolves(ps, tu, &m->miss[i])) {
                s = PS_MISS;
                why = m->miss[i].spelled;
            }
    }
    if (s != PS_FRESH && tu->stale == PS_FRESH) {
        tu->stale_ns = now;
        if (became)
            (*became)++;
    }
    tu->stale = s;
    tu->why = why;
}

size_t pstate_eval(PState *ps)
{
    size_t i, became = 0;
    uint64_t now = realtime_ns();
    for (i = 0; i < ps->tus.len; i++)
        eval_tu(ps, ps->tus.data[i], now, &became);
    return became;
}

size_t pstate_sweep(PState *ps, const char *path)
{
    uint64_t now = realtime_ns();
    size_t i, changed = 0;
    if (path) {
        PPath *p = path_get(ps, path, false);
        return p && refresh(p, now);
    }
    for (i = 0; i < ps->cap; i++)
        if (ps->slots[i] && refresh(ps->slots[i], now))
            changed++;
    return changed;
}

/* Every file whose hash is not known yet is read (the startup sweep, 7.2). */
static void settle(PState *ps, uint64_t now)
{
    size_t i;
    for (i = 0; i < ps->cap; i++) {
        PPath *p = ps->slots[i];
        if (p && !p->known)
            hash_now(p, now);
    }
}

/* ---- units ------------------------------------------------------------------------- */

void pstate_init(PState *ps, Store *store, uint64_t build_id)
{
    memset(ps, 0, sizeof *ps);
    ps->store = store;
    ps->build_id = build_id;
    arena_init(&ps->a);
}

static void ptu_free(PState *ps, PTu *tu)
{
    uint32_t i;
    man_free(ps, tu->man);
    for (i = 0; i < tu->ndirs; i++)
        free(tu->dirs[i]);
    free(tu->dirs);
    free(tu);
}

void pstate_free(PState *ps)
{
    size_t i;
    for (i = 0; i < ps->tus.len; i++)
        ptu_free(ps, ps->tus.data[i]);
    vec_free(&ps->tus);
    for (i = 0; i < ps->log.len; i++)
        free(ps->log.data[i]);
    vec_free(&ps->log);
    free(ps->slots);
    free(ps->chunks);
    arena_free(&ps->a);
    memset(ps, 0, sizeof *ps);
}

void pstate_drop_manifest(PState *ps, PTu *tu, bool remove_file)
{
    man_free(ps, tu->man);
    tu->man = NULL;
    if (remove_file) {
        char *path = crec_path(ps->store->dir, "tu", tu->tukey, "tu");
        unlink(path);
        free(path);
    }
}

typedef struct Cand {            /* an entry of the new unit set */
    size_t entry;
    uint64_t flag_hash, tukey;
} Cand;

static int cand_cmp(const void *pa, const void *pb)
{
    const Cand *a = pa, *b = pb;
    if (a->tukey != b->tukey)
        return a->tukey < b->tukey ? -1 : 1;
    return a->entry < b->entry ? -1 : a->entry > b->entry;
}

static int tup_cmp(const void *pa, const void *pb)
{
    uint64_t a = (*(PTu *const *)pa)->tukey, b = (*(PTu *const *)pb)->tukey;
    return a < b ? -1 : a > b;
}

static int tu_entry_cmp(const void *pa, const void *pb)
{
    size_t a = (*(PTu *const *)pa)->entry, b = (*(PTu *const *)pb)->entry;
    return a < b ? -1 : a > b;
}

void pstate_set_config(PState *ps, LspConfig *cfg,
                       bool (*pass)(const CompileEntry *, void *), void *ctx)
{
    VEC(Cand) cands = {0};
    PTuVec keep = {0}, old = {0};
    size_t i;
    uint64_t now = realtime_ns();
    ps->cfg = cfg;
    for (i = 0; i < cfg->cmds.len; i++) {
        const CompileEntry *e = &cfg->cmds.data[i];
        Cand c;
        Options o;
        if (!entry_is_c(e) || (pass && !pass(e, ctx)))
            continue;
        options_init(&o);
        config_options_entry(cfg, i, &o.rsp, &o, NULL);
        c.entry = i;
        c.flag_hash = crec_flag_hash(&o);
        c.tukey = crec_tukey(e->abs_file, c.flag_hash, ps->build_id);
        options_free(&o);
        vec_push(&cands, c);
    }
    if (cands.len > 1)
        qsort(cands.data, cands.len, sizeof *cands.data, cand_cmp);
    for (i = 0; i < ps->tus.len; i++)
        vec_push(&old, ps->tus.data[i]);
    if (old.len > 1)
        qsort(old.data, old.len, sizeof *old.data, tup_cmp);
    {
        VEC(bool) used = {0};
        size_t k;
        for (i = 0; i < old.len; i++)
            vec_push(&used, false);
        for (i = 0; i < cands.len; i++) {
            PTu key, *kp = &key, **hit;
            PTu *tu;
            if (i && cands.data[i - 1].tukey == cands.data[i].tukey)
                continue;               /* the same file under the same flags: one unit */
            key.tukey = cands.data[i].tukey;
            hit = old.len ? bsearch(&kp, old.data, old.len, sizeof *old.data, tup_cmp) : NULL;
            if (hit) {
                tu = *hit;
                used.data[hit - old.data] = true;
            } else {
                const CompileEntry *e = &cfg->cmds.data[cands.data[i].entry];
                tu = xcalloc(1, sizeof *tu);
                tu->main = arena_strdup(&ps->a, e->abs_file);
                tu->flag_hash = cands.data[i].flag_hash;
                tu->tukey = cands.data[i].tukey;
                tu->man = man_load(ps, tu, now);
            }
            if (tu->dirs && tu->entry != cands.data[i].entry) {  /* dirs came from the old config */
                for (k = 0; k < tu->ndirs; k++)
                    free(tu->dirs[k]);
                free(tu->dirs);
                tu->dirs = NULL;
                tu->ndirs = 0;
            }
            tu->entry = cands.data[i].entry;
            vec_push(&keep, tu);
        }
        for (i = 0; i < old.len; i++)
            if (!used.data[i]) {
                pstate_drop_manifest(ps, old.data[i], ps->store->writer);
                ptu_free(ps, old.data[i]);
            }
        vec_free(&used);
    }
    if (keep.len > 1)
        qsort(keep.data, keep.len, sizeof *keep.data, tu_entry_cmp);
    vec_free(&ps->tus);
    ps->tus = keep;
    vec_free(&old);
    vec_free(&cands);
    settle(ps, now);
    pstate_eval(ps);
}

bool pstate_reload(PState *ps, PTu *tu)
{
    uint64_t now = realtime_ns();
    PMan *m = man_load(ps, tu, now);
    man_free(ps, tu->man);
    tu->man = m;
    settle(ps, now);
    eval_tu(ps, tu, now, NULL);
    return m != NULL;
}

void pstate_reject_rec(PState *ps, uint64_t digest)
{
    size_t i;
    char *path = crec_path(ps->store->dir, "rec", digest, "rec");
    if (ps->store->writer)
        unlink(path);
    free(path);
    for (i = 0; i < ps->tus.len; i++) {
        PMan *m = ps->tus.data[i]->man;
        if (m && m->nrecs && bsearch(&digest, m->recs, m->nrecs, sizeof digest, u64_cmp)) {
            m->lost = true;
            if (ps->tus.data[i]->stale == PS_FRESH)
                ps->tus.data[i]->stale_ns = realtime_ns();
            ps->tus.data[i]->stale = PS_LOST;
        }
    }
}

void pstate_check_records(PState *ps)
{
    StoreLive live;
    size_t i;
    pstate_live(ps, &live);
    for (i = 0; i < live.nrecs; i++) {
        CRecMap m;
        char *path = crec_path(ps->store->dir, "rec", live.recs[i], "rec");
        const char *why = crec_map(path, CREC_RECORD, ps->build_id, &m);
        if (!why)
            crec_unmap(&m);
        else {
            report(ps, "rejected", path, why);
            pstate_reject_rec(ps, live.recs[i]);
        }
        free(path);
    }
    free((void *)live.tus);
    free((void *)live.recs);
    free((void *)live.chunks);
}

void pstate_live(PState *ps, StoreLive *live)
{
    size_t i, nt = 0, nr = 0, nc = 0, cap_r = 0, cap_c = 0;
    uint64_t *tus = xcalloc(ps->tus.len + 1, sizeof *tus), *recs, *chunks;
    for (i = 0; i < ps->tus.len; i++) {
        const PMan *m = ps->tus.data[i]->man;
        tus[nt++] = ps->tus.data[i]->tukey;
        if (m) {
            cap_r += m->nrecs;
            cap_c += m->nsysc;
        }
    }
    recs = xcalloc(cap_r + 1, sizeof *recs);
    chunks = xcalloc(cap_c + 1, sizeof *chunks);
    for (i = 0; i < ps->tus.len; i++) {
        const PMan *m = ps->tus.data[i]->man;
        if (m) {
            memcpy(recs + nr, m->recs, m->nrecs * sizeof *recs);
            nr += m->nrecs;
            memcpy(chunks + nc, m->sysc, m->nsysc * sizeof *chunks);
            nc += m->nsysc;
        }
    }
    live->ntus = sort_unique_u64(tus, nt);
    live->nrecs = sort_unique_u64(recs, nr);
    live->nchunks = sort_unique_u64(chunks, nc);
    live->tus = tus;
    live->recs = recs;
    live->chunks = chunks;
}
