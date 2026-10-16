/* projq.c - answering from the project index (projq.h). */
#include "projq.h"

#include <string.h>

#include "project.h"

void pq_init(PqFiles *fs, SrcMgr *sm, WsDocText doc_text, PosEncoding enc)
{
    memset(fs, 0, sizeof *fs);
    fs->sm = sm;
    fs->doc_text = doc_text;
    fs->enc = enc;
    arena_init(&fs->a);
}

void pq_free(PqFiles *fs)
{
    size_t i;
    if (fs->hint)
        project_hint_sweep();
    for (i = 0; i < fs->v.len; i++) {
        PqFile *f = fs->v.data[i];
        free(f->path);
        free(f->own);
        free(f->disk);
        vec_free(&f->lines);
        free(f);
    }
    vec_free(&fs->v);
    arena_free(&fs->a);
}

PqFile *pq_file(PqFiles *fs, const char *path)
{
    PqFile *f;
    size_t i;
    if (fs->last && !strcmp(fs->last->path, path))
        return fs->last;
    for (i = 0; i < fs->v.len; i++)
        if (!strcmp(fs->v.data[i]->path, path))
            return fs->last = fs->v.data[i];
    f = xcalloc(1, sizeof *f);
    f->path = xstrdup(path);
    f->uri = path_to_uri(&fs->a, path);
    vec_push(&fs->v, f);
    return fs->last = f;
}

/* The text the client has: the request's snapshot's, an open document's, else
 * the disk's.  The first two may differ from the disk's (open). */
static bool load(PqFiles *fs, PqFile *f)
{
    SrcFile *sf;
    size_t n;
    if (f->loaded)
        return f->cur != NULL;
    f->loaded = true;
    if (fs->sm && (sf = cindex_srcfile(fs->sm, f->path)) != NULL) {
        f->cur = sf->buf;
        f->curlen = sf->size;
        f->open = true;
    } else if (fs->doc_text && (f->own = fs->doc_text(f->path, &n)) != NULL) {
        f->cur = f->own;
        f->curlen = n;
        f->open = true;
    } else if ((f->own = read_file_nobom(f->path, &n, NULL)) != NULL) {
        f->cur = f->own;
        f->curlen = n;
    }
    return f->cur != NULL;
}

static uint64_t cur_hash(PqFile *f)
{
    if (!f->hashed) {
        f->curhash = cindex_hash(f->cur, f->curlen);
        f->hashed = true;
    }
    return f->curhash;
}

/* The disk's text, for an open file: true if it is there. */
static bool load_disk(PqFile *f)
{
    if (!f->disk_loaded) {
        f->disk_loaded = true;
        f->disk = read_file_nobom(f->path, &f->disklen, NULL);
        if (f->disk)
            f->diskhash = cindex_hash(f->disk, f->disklen);
    }
    return f->disk != NULL;
}

/* Maps off through e; false if [off, off + len) touches the damage (len 0: a
 * point, which only moves). */
static bool through(const CIdxEdit *e, uint32_t off, uint32_t len, uint32_t *out)
{
    if (len && !e->same && off < e->old_end && off + len > e->pre)
        return false;
    *out = cindex_edit_map(e, off);
    return true;
}

/* 10.3: as it is if the client's text is what the record was built from; for an
 * open document whose disk text is, mapped through the edit between them; else
 * nowhere. */
bool pq_place(PqFiles *fs, PqFile *f, uint64_t fhash, uint32_t off, uint32_t len,
              uint32_t *out)
{
    if (!load(fs, f))
        return false;
    if (cur_hash(f) == fhash) {
        *out = off;
        return true;
    }
    if (!f->open || !load_disk(f) || f->diskhash != fhash) {
        fs->hint = true;
        return false;
    }
    if (!f->fwd_done) {
        f->fwd_done = true;
        cindex_text_edit(f->disk, f->disklen, f->cur, f->curlen, &f->fwd);
    }
    return through(&f->fwd, off, len, out);
}

bool pq_unplace(PqFiles *fs, PqFile *f, uint32_t off, uint32_t len, uint32_t *out)
{
    if (!load(fs, f) || !f->open || !load_disk(f) || cur_hash(f) == f->diskhash) {
        *out = off;
        return true;
    }
    if (!f->rev_done) {
        f->rev_done = true;
        cindex_text_edit(f->cur, f->curlen, f->disk, f->disklen, &f->rev);
    }
    return through(&f->rev, off, len, out);
}

void pq_pos(JsonWriter *w, PqFiles *fs, PqFile *f, size_t off)
{
    uint32_t line = 0, ch = 0;
    if (load(fs, f)) {
        if (!f->lines_built) {
            line_table_build(&f->lines, f->cur, f->curlen);
            f->lines_built = true;
        }
        line_table_pos(&f->lines, f->cur, f->curlen, off, fs->enc, &line, &ch);
    }
    json_begin_object(w);
    json_key(w, "line");
    json_int(w, line);
    json_key(w, "character");
    json_int(w, ch);
    json_end_object(w);
}

void pq_range(JsonWriter *w, PqFiles *fs, PqFile *f, size_t off, size_t len)
{
    json_begin_object(w);
    json_key(w, "start");
    pq_pos(w, fs, f, off);
    json_key(w, "end");
    pq_pos(w, fs, f, off + len);
    json_end_object(w);
}

void pq_location(JsonWriter *w, PqFiles *fs, PqFile *f, size_t off, size_t len)
{
    json_begin_object(w);
    json_key(w, "uri");
    json_str(w, f->uri);
    json_key(w, "range");
    pq_range(w, fs, f, off, len);
    json_end_object(w);
}

/* ---- locations ---------------------------------------------------------------- */

static int loc_cmp(const void *pa, const void *pb)
{
    const PqLoc *a = pa, *b = pb;
    int c = a->f == b->f ? 0 : strcmp(a->f->path, b->f->path);
    if (c)
        return c;
    if (a->off != b->off)
        return a->off < b->off ? -1 : 1;
    if (a->len != b->len)
        return a->len < b->len ? -1 : 1;
    return a->seq < b->seq ? -1 : a->seq > b->seq;
}

void pq_locs_sort(PqLocs *v)
{
    size_t i, k = 0;
    if (v->len > 1)
        qsort(v->data, v->len, sizeof *v->data, loc_cmp);
    for (i = 0; i < v->len; i++)
        if (!k || v->data[k - 1].f != v->data[i].f || v->data[k - 1].off != v->data[i].off ||
            v->data[k - 1].len != v->data[i].len)
            v->data[k++] = v->data[i];
    v->len = k;
}

void pq_locs_write(JsonWriter *w, PqFiles *fs, const PqLocs *v)
{
    size_t i;
    for (i = 0; i < v->len; i++)
        pq_location(w, fs, v->data[i].f, v->data[i].off, v->data[i].len);
}

/* ---- keys ---------------------------------------------------------------------- */

int pq_key_cmp(const CIdxKey *a, const CIdxKey *b)
{
    int c = strcmp(a->name, b->name);
    if (c)
        return c;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->linkage != b->linkage)
        return a->linkage < b->linkage ? -1 : 1;
    if ((c = strcmp(a->path, b->path)) != 0)
        return c;
    return a->off < b->off ? -1 : a->off > b->off;
}

static void key_copy(PqFiles *fs, CIdxKey *out, const char *name, uint8_t kind,
                     uint8_t linkage, const char *path, uint32_t off)
{
    out->name = arena_strdup(&fs->a, name);
    out->path = arena_strdup(&fs->a, path);
    out->kind = kind;
    out->linkage = linkage;
    out->off = off;
}

bool pq_key(PqFiles *fs, const CIndex *ix, uint32_t d, CIdxKey *out)
{
    CIdxKey k;
    uint32_t off;
    cindex_key(ix, d, &k);
    key_copy(fs, out, k.name, k.kind, k.linkage, k.path, k.off);
    if (!*k.path)
        return true;
    if (!pq_unplace(fs, pq_file(fs, k.path), k.off, (uint32_t)strlen(k.name), &off))
        return false;
    out->off = off;
    return true;
}

/* kind, linkage, off (4 bytes, little-endian), name NUL, path NUL, in hex. */
const char *pq_key_hex(PqFiles *fs, const CIdxKey *k)
{
    static const char hex[] = "0123456789abcdef";
    size_t nn = strlen(k->name) + 1, np = strlen(k->path) + 1, n = 6 + nn + np, i;
    unsigned char *b = xmalloc(n);
    char *s = arena_alloc(&fs->a, 2 * n + 1);
    b[0] = k->kind;
    b[1] = k->linkage;
    for (i = 0; i < 4; i++)
        b[2 + i] = (unsigned char)(k->off >> (8 * i));
    memcpy(b + 6, k->name, nn);
    memcpy(b + 6 + nn, k->path, np);
    for (i = 0; i < n; i++) {
        s[2 * i] = hex[b[i] >> 4];
        s[2 * i + 1] = hex[b[i] & 15];
    }
    free(b);
    return s;
}

static int unhex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

bool pq_key_parse(PqFiles *fs, const char *hex, CIdxKey *out)
{
    size_t n = strlen(hex) / 2, i, nn;
    unsigned char *b;
    uint32_t off = 0;
    bool ok = n > 7 && !(strlen(hex) & 1);
    if (!ok)
        return false;
    b = xmalloc(n + 1);
    for (i = 0; i < n && ok; i++) {
        int h = unhex(hex[2 * i]), l = unhex(hex[2 * i + 1]);
        ok = h >= 0 && l >= 0;
        b[i] = (unsigned char)(h * 16 + l);
    }
    b[n] = 0;
    ok = ok && !b[n - 1] && memchr(b + 6, 0, n - 6) != NULL;
    if (ok) {
        for (i = 0; i < 4; i++)
            off |= (uint32_t)b[2 + i] << (8 * i);
        nn = strlen((char *)b + 6);
        key_copy(fs, out, (char *)b + 6, b[0], b[1], (char *)b + 7 + nn, off);
        ok = n == 6 + nn + 1 + strlen(out->path) + 1;
    }
    free(b);
    return ok;
}

/* ---- the records' entries ------------------------------------------------------ */

static uint32_t ref_len(const CRecRef *r, const CIdxKey *k)
{
    return r->len == 255 ? (uint32_t)strlen(k->name) : r->len;
}

static void block_note(PqFiles *fs, PqBlock *blk, const PiRec *rec, uint32_t off,
                       uint32_t len, const char *why, bool dropped)
{
    uint32_t at = off;
    PqFile *f = pq_file(fs, rec->path);
    if (dropped) {
        if (!blk->ndropped++) {
            blk->df = f;
            blk->doff = off;
        }
        return;
    }
    if (blk->why)
        return;
    pq_place(fs, f, rec->fhash, off, len, &at);
    blk->f = f;
    blk->off = at;
    blk->why = why;
}

void pq_entries(PqFiles *fs, ProjIndex *pi, const CIdxKey *k, unsigned what, PqLocs *out,
                PqBlock *blk)
{
    PiHit *h;
    size_t n, i;
    pi_rdlock(pi);
    n = pi_find(pi, k, &h);
    for (i = 0; i < n; i++) {
        const PiRec *rec = h[i].rec;
        const CRecView *v = &rec->map.v;
        PqFile *f = pq_file(fs, rec->path);
        uint32_t j, off;
        for (j = 0; (what & PQ_REF) && j < v->nrefs; j++) {
            const CRecRef *r = &v->refs[j];
            uint32_t len = ref_len(r, k);
            if (r->key != h[i].ki)
                continue;
            if (blk && cindex_rename_blocker(r->flags)) {
                block_note(fs, blk, rec, r->off, len, cindex_rename_blocker(r->flags), false);
                continue;
            }
            if (r->flags & CIX_MACRO_BODY)
                continue;
            if (pq_place(fs, f, rec->fhash, r->off, len, &off))
                vec_push(out, ((PqLoc){f, off, len, 1}));
            else if (blk)
                block_note(fs, blk, rec, r->off, len, NULL, true);
        }
        for (j = 0; (what & (PQ_DECL | PQ_DEF)) && j < v->ndecls; j++) {
            const CRecDecl *d = &v->decls[j];
            if (d->key != h[i].ki || !(what & (d->role == CIX_DEF ? PQ_DEF : PQ_DECL)))
                continue;
            if (pq_place(fs, f, rec->fhash, d->off, d->len, &off))
                vec_push(out, ((PqLoc){f, off, d->len, 1}));
            else if (blk)
                block_note(fs, blk, rec, d->off, d->len, NULL, true);
        }
    }
    pi_unlock(pi);
    free(h);
}

/* Sets sym's place from the DECL entries of key index ki in rec's view (its
 * definition if there is one), unless it has a definition already. */
static void sym_from_rec(PqFiles *fs, const PiRec *rec, uint32_t ki, PqSym *sym)
{
    const CRecView *v = &rec->map.v;
    PqFile *f = pq_file(fs, rec->path);
    uint32_t j, off, eb, ee;
    for (j = 0; j < v->ndecls; j++) {
        const CRecDecl *d = &v->decls[j];
        bool def = d->role == CIX_DEF;
        if (d->key != ki || (sym->f && (sym->def || !def)))
            continue;
        if (!pq_place(fs, f, rec->fhash, d->off, d->len, &off) ||
            !pq_place(fs, f, rec->fhash, d->ext_begin, 0, &eb) ||
            !pq_place(fs, f, rec->fhash, d->ext_end, 0, &ee))
            continue;
        sym->f = f;
        sym->off = off;
        sym->len = d->len;
        sym->ext_b = eb < off ? eb : off;
        sym->ext_e = ee > off + d->len ? ee : off + d->len;
        sym->def = def;
    }
}

void pq_calls(PqFiles *fs, ProjIndex *pi, const CIdxKey *k, bool incoming, PqCalls *out)
{
    PiHit *h;
    size_t n, i;
    pi_rdlock(pi);
    n = pi_find(pi, k, &h);
    for (i = 0; i < n; i++) {
        const PiRec *rec = h[i].rec;
        const CRecView *v = &rec->map.v;
        PqFile *f = pq_file(fs, rec->path);
        uint32_t j;
        for (j = 0; j < v->ncalls; j++) {
            const CRecCall *c = &v->calls[j];
            uint32_t mine = incoming ? c->callee : c->caller;
            uint32_t oi = incoming ? c->caller : c->callee, off;
            const CRecKey *ok = &v->keys[oi];
            uint32_t len = c->len == 255 ? (uint32_t)strlen(incoming ? k->name
                                                             : v->strs + ok->name)
                                         : c->len;
            PqCall pc;
            if (mine != h[i].ki)
                continue;
            memset(&pc, 0, sizeof pc);
            pc.other.d = -1;
            key_copy(fs, &pc.other.key, v->strs + ok->name, ok->kind, ok->linkage,
                     v->strs + ok->path, ok->off);
            if (!pq_place(fs, f, rec->fhash, c->off, len, &off))
                continue;
            pc.site = (PqLoc){f, off, len, 1};
            sym_from_rec(fs, rec, oi, &pc.other);
            vec_push(out, pc);
        }
    }
    pi_unlock(pi);
    free(h);
}

void pq_sym(PqFiles *fs, ProjIndex *pi, PqSym *sym)
{
    PiHit *h;
    size_t n, i;
    pi_rdlock(pi);
    n = pi_find(pi, &sym->key, &h);
    for (i = 0; i < n; i++)
        sym_from_rec(fs, h[i].rec, h[i].ki, sym);
    pi_unlock(pi);
    free(h);
}

/* ---- rename: what the records say against it ------------------------------------ */

typedef struct Mine {
    const char *path;
    uint64_t fhash;
    uint32_t off;
} Mine;

static int mine_cmp(const void *pa, const void *pb)
{
    const Mine *a = pa, *b = pb;
    int c = strcmp(a->path, b->path);
    if (c)
        return c;
    if (a->fhash != b->fhash)
        return a->fhash < b->fhash ? -1 : 1;
    return a->off < b->off ? -1 : a->off > b->off;
}

bool pq_ambiguous(PqFiles *fs, ProjIndex *pi, const CIdxKey *k, PqBlock *blk)
{
    PiHit *h;
    VEC(Mine) mine = {0};
    size_t n, i, r, nr;
    bool amb = false;
    pi_rdlock(pi);
    n = pi_find(pi, k, &h);
    for (i = 0; i < n; i++) {
        const CRecView *v = &h[i].rec->map.v;
        uint32_t j;
        for (j = 0; j < v->nrefs; j++)
            if (v->refs[j].key == h[i].ki)
                vec_push(&mine, ((Mine){h[i].rec->path, h[i].rec->fhash, v->refs[j].off}));
        for (j = 0; j < v->ndecls; j++)
            if (v->decls[j].key == h[i].ki)
                vec_push(&mine, ((Mine){h[i].rec->path, h[i].rec->fhash, v->decls[j].off}));
    }
    if (mine.len > 1)
        qsort(mine.data, mine.len, sizeof *mine.data, mine_cmp);
    nr = pi_nrecs(pi);
    for (r = 0; r < nr && !amb && mine.len; r++) {
        const PiRec *rec = pi_rec(pi, r);
        const CRecView *v = &rec->map.v;
        uint32_t j;
        for (j = 0; j < v->nrefs + v->ndecls && !amb; j++) {
            bool isref = j < v->nrefs;
            uint32_t key = isref ? v->refs[j].key : v->decls[j - v->nrefs].key;
            Mine m = {rec->path, rec->fhash, isref ? v->refs[j].off : v->decls[j - v->nrefs].off};
            size_t q;
            for (q = 0; q < n; q++)         /* our own key in a record that has it */
                if (h[q].rec == rec && h[q].ki == key)
                    break;
            if (q < n || !bsearch(&m, mine.data, mine.len, sizeof *mine.data, mine_cmp))
                continue;
            amb = true;
            if (blk) {
                PqFile *f = pq_file(fs, rec->path);
                uint32_t at = m.off;
                pq_place(fs, f, rec->fhash, m.off, 1, &at);
                blk->f = f;
                blk->off = at;
            }
        }
    }
    pi_unlock(pi);
    vec_free(&mine);
    free(h);
    return amb;
}

void pq_where(PqFiles *fs, PqFile *f, uint32_t off, char *buf, size_t bufsz)
{
    uint32_t line = 1, col = 1, i;
    if (load(fs, f))
        for (i = 0; i < off && i < f->curlen; i++) {
            if (f->cur[i] == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
    snprintf(buf, bufsz, "%s:%u:%u", f->path, line, col);
}

bool pq_current(PqFiles *fs, PqFile *f, long long *version)
{
    uint64_t live;
    char *disk;
    size_t n;
    bool same;
    if (!load(fs, f))
        return false;
    if (lsp_doc_state(f->path, version, &live))
        return live == cur_hash(f);
    *version = -2;                      /* not open: the disk's */
    disk = read_file_nobom(f->path, &n, NULL);
    same = disk && cindex_hash(disk, n) == cur_hash(f);
    free(disk);
    return same;
}
