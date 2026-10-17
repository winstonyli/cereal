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
    static const char *const names[] = {"fresh", "new", "file", "miss", "lost", "cover"};
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
    if (stat(p->path, &st) != 0 || !file_loadable(&st)) {
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
        bool exists = stat(p->path, &st) == 0 && file_loadable(&st);
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
    struct stat st;
    const char *why;
    PMan *man;
    uint32_t i;
    size_t nrec = 0;
    if (stat(path, &st) != 0)           /* before the read: a newer file is then seen again */
        st.st_ino = 0, st.st_mtim.tv_sec = st.st_mtim.tv_nsec = 0;
    why = crec_map(path, CREC_MANIFEST, ps->build_id, &m);
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
    man->ino = st.st_ino;
    man->mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
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
        man->miss[i].first_dir = m.v.miss[i].first_dir;
        man->miss[i].how = m.v.miss[i].how;
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

/* Does an include a unit missed resolve now?  The search pp_find_include made,
 * repeated from where it began (CRecMiss): the includer's own directory if it
 * was tried first, then the unit's directories from first_dir on, each by the
 * test the loader applies.  (A #include_next of a wrapper header must not find
 * the wrapper again, or the unit would be stale for ever.) */
static bool miss_resolves(PState *ps, PTu *tu, const PMiss *m)
{
    uint32_t i;
    if (m->spelled[0] == '/')
        return file_openable(m->spelled);
    if (m->how & CMISS_CUR) {
        Arena tmp;
        bool hit;
        arena_init(&tmp);
        hit = file_openable(path_join(&tmp, path_dirname(&tmp, m->from), m->spelled));
        arena_free(&tmp);
        if (hit)
            return true;
    }
    if (!tu->dirs)
        tu_dirs(ps, tu);
    for (i = m->first_dir; i < tu->ndirs; i++) {
        char *p = xmalloc(strlen(tu->dirs[i]) + strlen(m->spelled) + 2);
        bool hit;
        sprintf(p, "%s/%s", tu->dirs[i], m->spelled);
        hit = file_openable(p);
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
    if (s == PS_FRESH && ps->vmax && !tu->overlay) {
        if (!tu->cover_known) {         /* the sidecar goes first, so a manifest implies it */
            char *p = crec_path(ps->store->dir, "ovl", tu->tukey, "ovl");
            struct stat st;
            tu->covered = stat(p, &st) == 0;
            tu->cover_known = true;
            free(p);
        }
        if (!tu->covered)
            s = PS_COVER;
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

static void vc_free(PTu *tu);

static void ptu_free(PState *ps, PTu *tu)
{
    uint32_t i;
    man_free(ps, tu->man);
    vc_free(tu);
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
    const PTu *a = *(PTu *const *)pa, *b = *(PTu *const *)pb;
    if (a->entry != b->entry)
        return a->entry < b->entry ? -1 : 1;
    if (!!a->overlay != !!b->overlay)       /* a unit before its virtual ones */
        return a->overlay ? 1 : -1;
    return a->tukey < b->tukey ? -1 : a->tukey > b->tukey;
}

void pstate_set_virtual(PState *ps, double ratio, size_t per_unit)
{
    ps->vratio = ratio;
    ps->vmax = ratio > 0 ? per_unit : 0;
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
        for (i = 0; i < old.len; i++)     /* a unit that left the config keeps its file: the
                                             compdb may be half written; GC takes it after
                                             the grace period (6.4) */
            if (!used.data[i])
                ptu_free(ps, old.data[i]);
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
    {
        size_t added, dropped;
        pstate_allocate(ps, false, &added, &dropped);
    }
}

/* ---- virtual units ------------------------------------------------------------ */

static void vc_free(PTu *tu)
{
    uint32_t i;
    for (i = 0; i < tu->nvc; i++)
        free(tu->vc[i].line);
    free(tu->vc);
    tu->vc = NULL;
    tu->nvc = 0;
    tu->vc_known = false;
}

/* The overlays in tu's sidecar (one per line, at most ps->vmax) with the keys of
 * the units they make: computed once per load of the manifest. */
static void vc_load(PState *ps, PTu *tu)
{
    char *p, *text, *line, *save = NULL;
    size_t len;
    if (tu->vc_known)
        return;
    tu->vc_known = true;
    p = crec_path(ps->store->dir, "ovl", tu->tukey, "ovl");
    text = read_file(p, &len);
    free(p);
    if (!text)
        return;
    tu->vc = xcalloc(ps->vmax, sizeof *tu->vc);
    for (line = strtok_r(text, "\n", &save); line && tu->nvc < ps->vmax;
         line = strtok_r(NULL, "\n", &save)) {
        Options o;
        PVc *c = &tu->vc[tu->nvc++];
        options_init(&o);
        config_options_overlay(ps->cfg, tu->entry, line, &o.rsp, &o, NULL);
        c->line = xstrdup(line);
        c->flag_hash = crec_flag_hash(&o);
        c->tukey = crec_tukey(tu->main, c->flag_hash, ps->build_id);
        options_free(&o);
    }
    free(text);
}

/* A virtual unit the check could not make sense of (an overlay that is not a
 * real configuration, e.g. `-DLUA_BUILD_AS_DLL` on Linux) is not wanted: its
 * manifest says so once it has run, and the budget goes to the next line. */
bool pstate_bad_status(int status)
{
    return status == CTU_PARTIAL || status == CTU_FAILED || status == CTU_OVER_LIMIT;
}

static bool vc_bad(PState *ps, const PTu *parent, const PVc *vc, uint64_t now)
{
    size_t i;
    PTu probe;
    PMan *m;
    bool bad;
    for (i = 0; i < ps->tus.len; i++)
        if (ps->tus.data[i]->overlay && ps->tus.data[i]->tukey == vc->tukey)
            return ps->tus.data[i]->man && pstate_bad_status(ps->tus.data[i]->man->status);
    memset(&probe, 0, sizeof probe);
    probe.main = parent->main;
    probe.flag_hash = vc->flag_hash;
    probe.tukey = vc->tukey;
    m = man_load(ps, &probe, now);
    bad = m && pstate_bad_status(m->status);
    man_free(ps, m);
    return bad;
}

typedef struct Want {            /* a virtual unit that should exist */
    PTu *parent;
    const PVc *vc;
    bool found;
} Want;

static int want_cmp(const void *pa, const void *pb)
{
    const Want *a = pa, *b = pb;
    return a->vc->tukey < b->vc->tukey ? -1 : a->vc->tukey > b->vc->tukey;
}

static int real_main_cmp(const void *pa, const void *pb)
{
    const PTu *a = *(PTu *const *)pa, *b = *(PTu *const *)pb;
    int c = strcmp(a->main, b->main);
    if (c)
        return c;
    return a->tukey < b->tukey ? -1 : a->tukey > b->tukey;
}

/* The budget (ratio times the real units) goes to the real units in main-path
 * order, each up to vmax overlays of its sidecar, so that neither the order the
 * jobs finished in nor the compile database's decides who gets units. */
void pstate_allocate(PState *ps, bool force, size_t *added, size_t *dropped)
{
    PTuVec reals = {0}, keep = {0};
    VEC(Want) want = {0};
    size_t i, budget;
    bool stale = false;
    uint64_t now = realtime_ns();
    *added = *dropped = 0;
    if (!ps->cfg)
        return;
    for (i = 0; i < ps->tus.len; i++) {
        PTu *t = ps->tus.data[i];
        if (!t->overlay) {
            vec_push(&reals, t);
            stale = stale || t->stale != PS_FRESH;
        }
    }
    if (stale && !force) {      /* the answer would change as the units finish */
        vec_free(&reals);
        return;
    }
    budget = ps->vmax ? (size_t)(ps->vratio * (double)reals.len) : 0;
    if (reals.len > 1)
        qsort(reals.data, reals.len, sizeof *reals.data, real_main_cmp);
    for (i = 0; i < reals.len && want.len < budget; i++) {
        uint32_t k;
        vc_load(ps, reals.data[i]);
        for (k = 0; k < reals.data[i]->nvc && want.len < budget; k++) {
            Want w;
            if (vc_bad(ps, reals.data[i], &reals.data[i]->vc[k], now))
                continue;
            w.parent = reals.data[i];
            w.vc = &reals.data[i]->vc[k];
            w.found = false;
            vec_push(&want, w);
        }
    }
    if (want.len > 1)
        qsort(want.data, want.len, sizeof *want.data, want_cmp);
    for (i = 0; i < ps->tus.len; i++) {     /* keep the real units and the wanted ones */
        PTu *t = ps->tus.data[i];
        Want key, *hit = NULL;
        PVc kv;
        if (t->overlay && want.len) {
            kv.tukey = t->tukey;
            key.vc = &kv;
            hit = bsearch(&key, want.data, want.len, sizeof *want.data, want_cmp);
        }
        if (t->overlay && !hit) {
            ptu_free(ps, t);
            (*dropped)++;
        } else {
            if (hit)
                hit->found = true;
            vec_push(&keep, t);
        }
    }
    for (i = 0; i < want.len; i++) {
        PTu *t;
        if (want.data[i].found)
            continue;
        t = xcalloc(1, sizeof *t);
        t->main = want.data[i].parent->main;
        t->entry = want.data[i].parent->entry;
        t->flag_hash = want.data[i].vc->flag_hash;
        t->tukey = want.data[i].vc->tukey;
        t->parent = want.data[i].parent->tukey;
        t->overlay = arena_strdup(&ps->a, want.data[i].vc->line);
        t->man = man_load(ps, t, now);
        vec_push(&keep, t);
        (*added)++;
    }
    vec_free(&ps->tus);
    ps->tus = keep;
    if (*added || *dropped) {
        if (ps->tus.len > 1)
            qsort(ps->tus.data, ps->tus.len, sizeof *ps->tus.data, tu_entry_cmp);
        settle(ps, now);
        for (i = 0; i < ps->tus.len; i++)
            if (ps->tus.data[i]->overlay)
                eval_tu(ps, ps->tus.data[i], now, NULL);
    }
    vec_free(&want);
    vec_free(&reals);
}
bool pstate_refresh(PState *ps, PTu *tu)
{
    char *path = crec_path(ps->store->dir, "tu", tu->tukey, "tu");
    struct stat st;
    bool same = stat(path, &st) != 0 ? !tu->man :
                tu->man && tu->man->ino == st.st_ino &&
                tu->man->mtime_ns == (uint64_t)st.st_mtim.tv_sec * 1000000000u +
                                         (uint64_t)st.st_mtim.tv_nsec;
    free(path);
    if (same)
        return false;
    pstate_reload(ps, tu);
    return true;
}

bool pstate_reload(PState *ps, PTu *tu)
{
    uint64_t now = realtime_ns();
    PMan *m = man_load(ps, tu, now);
    man_free(ps, tu->man);
    tu->man = m;
    tu->cover_known = false;
    vc_free(tu);
    settle(ps, now);
    eval_tu(ps, tu, now, NULL);
    return m != NULL;
}

void pstate_reject_rec(PState *ps, uint64_t digest, const char *why)
{
    size_t i;
    char *path = crec_path(ps->store->dir, "rec", digest, "rec");
    report(ps, "rejected", path, why);
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
