/* cidxrec.c - the project index's records and manifests (cidxrec.h,
 * docs/B5_DESIGN.md section 5): building them from a checked unit, the
 * validating reader, the store layout and the text dump. */
#include "cidxrec.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "c/csymidx.h"
#include "c/frontend.h"
#include "hash.h"

#define TAG(a, b, c, d) \
    ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
enum {
    T_STR = TAG('S', 'T', 'R', ' '), T_FILE = TAG('F', 'I', 'L', 'E'),
    T_KEYS = TAG('K', 'E', 'Y', 'S'), T_DECL = TAG('D', 'E', 'C', 'L'),
    T_REFS = TAG('R', 'E', 'F', 'S'), T_CALL = TAG('C', 'A', 'L', 'L'),
    T_MACR = TAG('M', 'A', 'C', 'R'), T_TU = TAG('T', 'U', ' ', ' '),
    T_FILS = TAG('F', 'I', 'L', 'S'), T_MISS = TAG('M', 'I', 'S', 'S'),
    T_SYST = TAG('S', 'Y', 'S', 'T'), T_SYSC = TAG('S', 'Y', 'S', 'C')
};
#define BOM 0x01020304u
#define SEED(build_id) ((build_id) ^ CREC_FORMAT)

_Static_assert(sizeof(CRecHeader) == 32, "header");
_Static_assert(sizeof(CRecFile) == 16 && sizeof(CRecKey) == 16 &&
               sizeof(CRecDecl) == 28 && sizeof(CRecRef) == 12 &&
               sizeof(CRecCall) == 16 && sizeof(CRecMacro) == 12 &&
               sizeof(CRecTu) == 48 && sizeof(CRecFil) == 40 &&
               sizeof(CRecMiss) == 16 && sizeof(CRecSys) == 24, "entries");

/* ---- the reader ------------------------------------------------------------ */

uint64_t crec_build_id(void)
{
    size_t n;
    char *exe = read_file("/proc/self/exe", &n);
    uint64_t h = exe ? hash64(exe, n, 0x63726c78) : 0;
    free(exe);
    return h;
}

int crec_kind(const void *p, size_t n)
{
    const CRecHeader *h = p;
    if (n < sizeof *h || memcmp(h->magic, "CRLIDX\0\0", 8))
        return 0;
    return h->kind;
}

/* Where a section lives: count entries of esz bytes in [off, off + bytes). */
static const void *section(const char *base, const CRecSecDir *d, size_t esz,
                           uint32_t *count)
{
    if ((uint64_t)d->count * esz != d->bytes)
        return NULL;
    *count = d->count;
    return base + d->offset;
}

const char *crec_parse(const void *p, size_t n, int kind, uint64_t build_id,
                       CRecView *v)
{
    const CRecHeader *h = p;
    const CRecSecDir *dir;
    const char *base = p;
    uint64_t dir_end;
    uint32_t i, nfile = 0, ntu = 0, k;
    memset(v, 0, sizeof *v);
    if (n < sizeof *h || ((uintptr_t)p & 7))
        return "short or misaligned file";
    if (memcmp(h->magic, "CRLIDX\0\0", 8))
        return "bad magic";
    if (h->bom != BOM)
        return "other byte order";
    if (h->version != CREC_FORMAT)
        return "other format version";
    if (h->kind != kind)
        return "other kind of file";
    if (h->build_id != build_id)
        return "other build";
    dir_end = sizeof *h + (uint64_t)h->nsections * sizeof *dir;
    if (h->nsections > 32 || dir_end > n)
        return "section directory out of bounds";
    if ((uint32_t)hash64(base + sizeof *h, n - sizeof *h, 0) != h->payload_hash)
        return "checksum mismatch";
    dir = (const CRecSecDir *)(base + sizeof *h);
    v->kind = kind;
    v->build_id = h->build_id;
    for (i = 0; i < h->nsections; i++) {
        const CRecSecDir *d = &dir[i];
        uint32_t cnt = 0;
        const void *s;
        if (d->offset % 8 || d->offset < dir_end || d->offset > n ||
            d->bytes > n - d->offset)
            return "section out of bounds";
        for (k = 0; k < i; k++)
            if (dir[k].tag == d->tag)
                return "duplicate section";
        s = base + d->offset;
        switch (d->tag) {
        case T_STR:
            if (d->count != d->bytes || !d->bytes || ((const char *)s)[d->bytes - 1])
                return "bad string section";
            v->strs = s;
            v->nstrs = d->bytes;
            break;
#define SEC(TAGV, T, FIELD, N)                                    \
    case TAGV:                                                    \
        if (!(v->FIELD = section(base, d, sizeof(T), &cnt)))      \
            return "bad section size";                            \
        N = cnt;                                                  \
        break;
        SEC(T_FILE, CRecFile, file, nfile)
        SEC(T_KEYS, CRecKey, keys, v->nkeys)
        SEC(T_DECL, CRecDecl, decls, v->ndecls)
        SEC(T_REFS, CRecRef, refs, v->nrefs)
        SEC(T_CALL, CRecCall, calls, v->ncalls)
        SEC(T_MACR, CRecMacro, macros, v->nmacros)
        SEC(T_TU, CRecTu, tu, ntu)
        SEC(T_FILS, CRecFil, fils, v->nfils)
        SEC(T_MISS, CRecMiss, miss, v->nmiss)
        SEC(T_SYST, CRecSys, syst, v->nsyst)
        SEC(T_SYSC, uint64_t, sysc, v->nsysc)
#undef SEC
        default:
            break;               /* unknown: skipped */
        }
    }
    if (!v->strs)
        return "no string section";
    if (kind == CREC_RECORD ? nfile != 1 : kind == CREC_MANIFEST ? ntu != 1
                                                                  : v->nsyst < 1)
        return "bad entry count";
    if ((kind != CREC_MANIFEST && (v->tu || v->nfils || v->nsysc)) ||
        (kind != CREC_RECORD && v->file) || (kind != CREC_CHUNK && v->nsyst))
        return "section of the other kind";
    /* every string offset and key index, once */
#define STR_OK(o) ((o) < v->nstrs)
    if (v->file && (!STR_OK(v->file->path)))
        return "string offset out of range";
    for (k = 0; k < v->nkeys; k++) {
        if (!STR_OK(v->keys[k].name) || !STR_OK(v->keys[k].path))
            return "string offset out of range";
        if (v->keys[k].kind > CIK_LABEL || v->keys[k].linkage > 2)
            return "bad key entry";      /* kind indexes tables in the readers */
    }
    for (k = 0; k < v->ndecls; k++) {
        const CRecDecl *e = &v->decls[k];
        if (e->key >= v->nkeys)
            return "key index out of range";
        if (!STR_OK(e->container) || e->role > 1)
            return "bad declaration entry";
    }
    for (k = 0; k < v->nrefs; k++)
        if (v->refs[k].key >= v->nkeys)
            return "key index out of range";
    for (k = 0; k < v->ncalls; k++)
        if (v->calls[k].caller >= v->nkeys || v->calls[k].callee >= v->nkeys)
            return "key index out of range";
    for (k = 0; k < v->nmacros; k++)
        if (!STR_OK(v->macros[k].name))
            return "string offset out of range";
    if (v->tu && !STR_OK(v->tu->main_path))
        return "string offset out of range";
    for (k = 0; k < v->nfils; k++)
        if (!STR_OK(v->fils[k].path))
            return "string offset out of range";
    for (k = 0; k < v->nmiss; k++) {
        if (!STR_OK(v->miss[k].spelled) || !STR_OK(v->miss[k].from_path))
            return "string offset out of range";
        if (v->miss[k].how & ~(CMISS_ANGLED | CMISS_CUR))
            return "bad include entry";
    }
    for (k = 0; k < v->nsyst; k++)
        if (!STR_OK(v->syst[k].path))
            return "string offset out of range";
#undef STR_OK
    return NULL;
}

const char *crec_map(const char *file, int kind, uint64_t build_id, CRecMap *m)
{
    struct stat st;
    const char *why;
    void *p;
    int fd = open(file, O_RDONLY);
    memset(m, 0, sizeof *m);
    if (fd < 0)
        return "cannot open";
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 8) {
        close(fd);
        return "short or not a file";
    }
    p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return "cannot map";
    why = crec_parse(p, (size_t)st.st_size, kind, build_id, &m->v);
    if (why) {
        munmap(p, (size_t)st.st_size);
        return why;
    }
    m->p = p;
    m->n = (size_t)st.st_size;
    return NULL;
}

void crec_unmap(CRecMap *m)
{
    if (m->p)
        munmap((void *)m->p, m->n);
    memset(m, 0, sizeof *m);
}

uint64_t crec_flag_hash(const Options *o)
{
    return hash64_mix(o->fingerprint, CREC_FORMAT);
}

uint64_t crec_tukey(const char *main_path, uint64_t flag_hash, uint64_t build_id)
{
    return hash64(main_path, strlen(main_path), hash64_mix(flag_hash, build_id));
}

/* ---- the writer ------------------------------------------------------------ */

typedef struct StrTab {          /* deduplicated strings; offset 0 is "" */
    StrBuf b;
    uint32_t *slot;
    size_t cap, n;
} StrTab;

static uint32_t st_add(StrTab *t, const char *s)
{
    size_t len = strlen(s), i, mask;
    uint32_t off;
    if (!len)
        return 0;
    if (!t->b.len)
        sb_putc(&t->b, 0);
    if ((t->n + 1) * 2 > t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 64, k;
        uint32_t *ns = xcalloc(nc, sizeof *ns);
        for (k = 0; k < t->cap; k++)
            if (t->slot[k]) {
                const char *str = t->b.data + t->slot[k];
                size_t j = hash_bytes(str, strlen(str)) & (nc - 1);
                while (ns[j])
                    j = (j + 1) & (nc - 1);
                ns[j] = t->slot[k];
            }
        free(t->slot);
        t->slot = ns;
        t->cap = nc;
    }
    mask = t->cap - 1;
    for (i = hash_bytes(s, len) & mask; t->slot[i]; i = (i + 1) & mask)
        if (!strcmp(t->b.data + t->slot[i], s))
            return t->slot[i];
    off = (uint32_t)t->b.len;
    sb_putn(&t->b, s, len + 1);
    t->slot[i] = off;
    t->n++;
    return off;
}

typedef struct Sec {
    uint32_t tag, count, esz;
    StrBuf b;
} Sec;

typedef struct Wr {
    Sec s[9];
    int n;
    StrTab st;
} Wr;

static Sec *wr_sec(Wr *w, uint32_t tag, size_t esz)
{
    Sec *s = &w->s[w->n++];
    s->tag = tag;
    s->esz = (uint32_t)esz;
    return s;
}

static void sec_put(Sec *s, const void *e)
{
    sb_putn(&s->b, e, s->esz);
    s->count++;
}

static void wr_free(Wr *w)
{
    int i;
    for (i = 0; i < w->n; i++)
        sb_free(&w->s[i].b);
    sb_free(&w->st.b);
    free(w->st.slot);
}

/* Header, directory and the sections (the string blob last), 8-aligned and
 * zero padded, so equal contents are equal bytes. */
static void wr_finish(Wr *w, int kind, uint64_t build_id, CRecBlob *out)
{
    CRecHeader h;
    size_t pos, i;
    Sec *str;
    char *buf;
    if (!w->st.b.len)
        sb_putc(&w->st.b, 0);
    str = wr_sec(w, T_STR, 1);
    sb_putn(&str->b, w->st.b.data, w->st.b.len);
    str->count = (uint32_t)w->st.b.len;
    pos = sizeof h + (size_t)w->n * sizeof(CRecSecDir);
    for (i = 0; i < (size_t)w->n; i++)
        pos = ((pos + 7) & ~(size_t)7) + w->s[i].b.len;
    pos = (pos + 7) & ~(size_t)7;
    buf = xcalloc(1, pos);
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "CRLIDX\0\0", 8);
    h.bom = BOM;
    h.version = CREC_FORMAT;
    h.kind = (uint16_t)kind;
    h.build_id = build_id;
    h.nsections = (uint32_t)w->n;
    pos = sizeof h + (size_t)w->n * sizeof(CRecSecDir);
    for (i = 0; i < (size_t)w->n; i++) {
        CRecSecDir d;
        pos = (pos + 7) & ~(size_t)7;
        d.tag = w->s[i].tag;
        d.offset = (uint32_t)pos;
        d.count = w->s[i].count;
        d.bytes = (uint32_t)w->s[i].b.len;
        memcpy(buf + sizeof h + i * sizeof d, &d, sizeof d);
        if (d.bytes)
            memcpy(buf + pos, w->s[i].b.data, d.bytes);
        pos += d.bytes;
    }
    pos = (pos + 7) & ~(size_t)7;
    h.payload_hash = (uint32_t)hash64(buf + sizeof h, pos - sizeof h, 0);
    memcpy(buf, &h, sizeof h);
    out->data = buf;
    out->len = pos;
    out->digest = hash64(buf + sizeof h, pos - sizeof h, SEED(build_id));
}

/* ---- records from an index --------------------------------------------------- */

typedef struct DeclRow {
    uint32_t decl, off, ext_b, ext_e, flags;
    uint16_t len;
    uint8_t role;
    const char *container;
} DeclRow;
typedef struct RefRow {
    uint32_t decl, off;
    uint8_t len, flags;
} RefRow;
typedef struct CallRow {
    uint32_t caller, callee, off;
    uint8_t len, flags;
} CallRow;
typedef struct MacRow {
    const char *name;
    uint32_t off;
    uint16_t len;
    uint8_t funclike;
} MacRow;
typedef struct FileAcc {         /* what one file contributes */
    VEC(DeclRow) d;
    VEC(RefRow) r;
    VEC(CallRow) c;
    VEC(MacRow) m;
} FileAcc;

typedef struct Build {
    const CIndex *ix;
    FileAcc *acc;                /* by SrcFile id */
    int *to_sm;                  /* ix file -> SrcFile id, -1: not a user file */
    uint8_t *exported;           /* decl has a symbol event in a user file */
    CIdxKey *kc;                 /* keys by decl, computed on demand */
    uint8_t *kc_ok;
    uint32_t *kidx, *kstamp, stamp;
} Build;

static const CIdxKey *get_key(Build *b, uint32_t d)
{
    if (!b->kc_ok[d]) {
        cindex_key(b->ix, d, &b->kc[d]);
        b->kc_ok[d] = 1;
    }
    return &b->kc[d];
}

static int key_cmp(const CIdxKey *a, const CIdxKey *b)
{
    int c = strcmp(a->name, b->name);
    if (c)
        return c;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    if (a->linkage != b->linkage)
        return a->linkage < b->linkage ? -1 : 1;
    c = strcmp(a->path, b->path);
    if (c)
        return c;
    return a->off < b->off ? -1 : a->off > b->off;
}

typedef struct KeyRef {
    const CIdxKey *k;
    uint32_t decl;
} KeyRef;

static int keyref_cmp(const void *pa, const void *pb)
{
    const KeyRef *a = pa, *b = pb;
    int c = key_cmp(a->k, b->k);
    return c ? c : a->decl < b->decl ? -1 : a->decl > b->decl;
}

#define CMP3(a, b) ((a) < (b) ? -1 : (a) > (b))
static int decl_cmp(const void *pa, const void *pb)
{
    const CRecDecl *a = pa, *b = pb;
    int c = CMP3(a->off, b->off);
    return c ? c : (c = CMP3(a->len, b->len)) ? c
         : (c = CMP3(a->key, b->key)) ? c : memcmp(a, b, sizeof *a);
}
static int ref_cmp(const void *pa, const void *pb)
{
    const CRecRef *a = pa, *b = pb;
    int c = CMP3(a->off, b->off);
    return c ? c : (c = CMP3(a->len, b->len)) ? c
         : (c = CMP3(a->key, b->key)) ? c : memcmp(a, b, sizeof *a);
}
static int call_cmp(const void *pa, const void *pb)
{
    const CRecCall *a = pa, *b = pb;
    int c = CMP3(a->off, b->off);
    return c ? c : (c = CMP3(a->caller, b->caller)) ? c
         : (c = CMP3(a->callee, b->callee)) ? c : memcmp(a, b, sizeof *a);
}
static int macro_cmp(const void *pa, const void *pb)
{
    const CRecMacro *a = pa, *b = pb;
    int c = CMP3(a->off, b->off);
    return c ? c : memcmp(a, b, sizeof *a);
}

/* Sorted, and equal neighbours merged. */
static size_t sort_unique(void *base, size_t n, size_t sz,
                          int (*cmp)(const void *, const void *))
{
    size_t i, k = 0;
    char *p = base;
    if (n > 1)
        qsort(base, n, sz, cmp);
    for (i = 0; i < n; i++)
        if (!k || memcmp(p + (k - 1) * sz, p + i * sz, sz))
            memcpy(p + k++ * sz, p + i * sz, sz);
    return k;
}

static void build_record(Build *b, const SrcFile *f, FileAcc *a, uint64_t build_id,
                         CRecBlob *out)
{
    Wr w;
    VEC(KeyRef) ids = {0};
    CRecDecl *decl = xmalloc((a->d.len + 1) * sizeof *decl);
    CRecRef *ref = xmalloc((a->r.len + 1) * sizeof *ref);
    CRecCall *call = xmalloc((a->c.len + 1) * sizeof *call);
    CRecMacro *mac = xmalloc((a->m.len + 1) * sizeof *mac);
    size_t i, nd, nr, nc, nm, nk = 0;
    Sec *sec;
    memset(&w, 0, sizeof w);
    b->stamp++;
    for (i = 0; i < a->d.len; i++)
        vec_push(&ids, ((KeyRef){get_key(b, a->d.data[i].decl), a->d.data[i].decl}));
    for (i = 0; i < a->r.len; i++)
        vec_push(&ids, ((KeyRef){get_key(b, a->r.data[i].decl), a->r.data[i].decl}));
    for (i = 0; i < a->c.len; i++) {
        vec_push(&ids, ((KeyRef){get_key(b, a->c.data[i].caller), a->c.data[i].caller}));
        vec_push(&ids, ((KeyRef){get_key(b, a->c.data[i].callee), a->c.data[i].callee}));
    }
    if (ids.len > 1)
        qsort(ids.data, ids.len, sizeof *ids.data, keyref_cmp);
    sec = wr_sec(&w, T_FILE, sizeof(CRecFile));
    {
        CRecFile e = {st_add(&w.st, f->path), f->size, cindex_hash(f->buf, f->size)};
        sec_put(sec, &e);
    }
    sec = ids.len ? wr_sec(&w, T_KEYS, sizeof(CRecKey)) : NULL;
    for (i = 0; i < ids.len; i++) {       /* equal keys share an index */
        const CIdxKey *k = ids.data[i].k;
        if (!i || key_cmp(ids.data[i - 1].k, k)) {
            CRecKey e;
            memset(&e, 0, sizeof e);
            e.name = st_add(&w.st, k->name);
            e.kind = k->kind;
            e.linkage = k->linkage;
            e.path = st_add(&w.st, k->path);
            e.off = k->off;
            sec_put(sec, &e);
            nk++;
        }
        b->kidx[ids.data[i].decl] = (uint32_t)nk - 1;
        b->kstamp[ids.data[i].decl] = b->stamp;
    }
    for (i = 0; i < a->d.len; i++) {
        const DeclRow *r = &a->d.data[i];
        CRecDecl *e = &decl[i];
        memset(e, 0, sizeof *e);
        e->key = b->kidx[r->decl];
        e->off = r->off;
        e->len = r->len;
        e->role = r->role;
        e->container = st_add(&w.st, r->container);
        e->ext_begin = r->ext_b;
        e->ext_end = r->ext_e;
        e->flags = r->flags;
    }
    nd = sort_unique(decl, a->d.len, sizeof *decl, decl_cmp);
    for (i = 0; i < a->r.len; i++) {
        const RefRow *r = &a->r.data[i];
        memset(&ref[i], 0, sizeof ref[i]);
        ref[i].key = b->kidx[r->decl];
        ref[i].off = r->off;
        ref[i].len = r->len;
        ref[i].flags = r->flags;
    }
    nr = sort_unique(ref, a->r.len, sizeof *ref, ref_cmp);
    for (i = 0; i < a->c.len; i++) {
        const CallRow *r = &a->c.data[i];
        memset(&call[i], 0, sizeof call[i]);
        call[i].caller = b->kidx[r->caller];
        call[i].callee = b->kidx[r->callee];
        call[i].off = r->off;
        call[i].len = r->len;
        call[i].flags = r->flags;
    }
    nc = sort_unique(call, a->c.len, sizeof *call, call_cmp);
    for (i = 0; i < a->m.len; i++) {
        const MacRow *r = &a->m.data[i];
        memset(&mac[i], 0, sizeof mac[i]);
        mac[i].name = st_add(&w.st, r->name);
        mac[i].off = r->off;
        mac[i].len = r->len;
        mac[i].funclike = r->funclike;
    }
    nm = sort_unique(mac, a->m.len, sizeof *mac, macro_cmp);
#define PUT(TAGV, T, ARR, N)                                      \
    if (N) {                                                      \
        size_t k_;                                                \
        sec = wr_sec(&w, TAGV, sizeof(T));                        \
        for (k_ = 0; k_ < (N); k_++)                              \
            sec_put(sec, &(ARR)[k_]);                             \
    }
    PUT(T_DECL, CRecDecl, decl, nd)
    PUT(T_REFS, CRecRef, ref, nr)
    PUT(T_CALL, CRecCall, call, nc)
    PUT(T_MACR, CRecMacro, mac, nm)
#undef PUT
    wr_finish(&w, CREC_RECORD, build_id, out);
    out->path = xstrdup(f->path);
    wr_free(&w);
    vec_free(&ids);
    free(decl);
    free(ref);
    free(call);
    free(mac);
}

static void on_call(void *ctx, const CIdxEvent *e, uint32_t caller, bool xcall)
{
    Build *b = ctx;
    const CIdxDecl *x = &b->ix->decls[e->decl];
    int sf = b->to_sm[e->file];
    CallRow r;
    if (sf < 0 || !(x->kind == CIK_FUNC || b->exported[e->decl]))
        return;
    r.caller = caller;
    r.callee = e->decl;
    r.off = e->off;
    r.len = e->len;
    r.flags = (uint8_t)((e->flags & (CIX_ARG | CIX_AT_EXPANSION)) |
                        (xcall ? CIX_AT_EXPANSION : 0));
    vec_push(&b->acc[sf].c, r);
}

/* Fills the accumulators from the index and the unit's macros. */
static void collect(Build *b, TU *tu)
{
    const CIndex *ix = b->ix;
    uint32_t i;
    size_t k;
    for (i = 0; i < ix->nev; i++) {
        const CIdxEvent *e = &ix->ev[i];
        const CIdxDecl *x = &ix->decls[e->decl];
        uint32_t s;
        DeclRow r;
        if (b->to_sm[e->file] < 0 || !cindex_symbol_event(ix, i, &s))
            continue;
        b->exported[e->decl] = 1;
        r.decl = e->decl;
        r.off = e->off;
        r.len = e->len == 255 ? (uint16_t)strlen(cindex_name(ix, e->decl)) : e->len;
        r.role = e->flags & CIX_ROLE;
        r.ext_b = e->off;
        r.ext_e = e->off + r.len;
        if (s && x->kind != CIK_FIELD && x->kind != CIK_ENUMCONST) {
            if (ix->scopes[s - 1].begin < r.ext_b)
                r.ext_b = ix->scopes[s - 1].begin;
            if (ix->scopes[s - 1].end > r.ext_e)
                r.ext_e = ix->scopes[s - 1].end;
        }
        r.flags = x->flags;
        r.container = x->parent ? cindex_name(ix, x->parent - 1) : "";
        vec_push(&b->acc[b->to_sm[e->file]].d, r);
    }
    for (i = 0; i < ix->nev; i++) {
        const CIdxEvent *e = &ix->ev[i];
        RefRow r;
        if (b->to_sm[e->file] < 0 || (e->flags & CIX_ROLE) != CIX_REF ||
            !b->exported[e->decl])
            continue;
        r.decl = e->decl;
        r.off = e->off;
        r.len = e->len;
        r.flags = e->flags & (CIX_CALL | CIX_ARG | CIX_AT_EXPANSION | CIX_MACRO_BODY);
        vec_push(&b->acc[b->to_sm[e->file]].r, r);
    }
    cindex_each_call(ix, on_call, b);
    for (k = 0; k < tu->pp.macros.len; k++) {
        const Macro *m = tu->pp.macros.data[k];
        SrcFile *f = m->name_loc && !m->alias_of ? srcmgr_file_of(&tu->sm, m->name_loc)
                                                 : NULL;
        MacRow r;
        if (!f || f->kind != SF_USER)
            continue;
        r.name = m->name->str;
        r.off = m->name_loc - f->base;
        r.len = (uint16_t)m->name->len;
        r.funclike = m->funclike;
        vec_push(&b->acc[f->id].m, r);
    }
}

/* ---- the manifest ---------------------------------------------------------- */

typedef struct FilRow {
    const char *path;
    uint32_t size, flags;
    uint64_t mtime_ns, hash, digest;
} FilRow;

static int filrow_cmp(const void *pa, const void *pb)
{
    const FilRow *a = pa, *b = pb;
    int c = strcmp(a->path, b->path);
    return c ? c : CMP3(a->flags, b->flags);
}

typedef struct Miss {
    const char *spelled, *from;
    uint32_t first_dir, how;
} Miss;

static int miss_cmp(const void *pa, const void *pb)
{
    const Miss *a = pa, *b = pb;
    int c = strcmp(a->from, b->from);
    if (!c)
        c = strcmp(a->spelled, b->spelled);
    if (!c)
        c = CMP3(a->first_dir, b->first_dir);
    return c ? c : CMP3(a->how, b->how);
}

typedef struct MissList {
    VEC(Miss) v;
    Arena *a;
} MissList;

static void on_miss(void *ctx, const char *spelled, bool angled, size_t first_dir, bool cur,
                    SrcFile *from)
{
    MissList *l = ctx;
    Miss m;
    m.spelled = arena_strdup(l->a, spelled);
    m.from = from->path;
    m.first_dir = (uint32_t)first_dir;
    m.how = (angled ? CMISS_ANGLED : 0) | (cur ? CMISS_CUR : 0);
    vec_push(&l->v, m);
}

static uint64_t now_ns(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Stat data and content hash of `path` as the index sees a file (7.2). */
bool crec_stat_hash(const char *path, uint32_t *size, uint64_t *mtime_ns,
                    uint64_t *hash)
{
    struct stat st;
    size_t len;
    char *text = read_file_nobom(path, &len, &st);
    if (!text)
        return false;
    *size = (uint32_t)st.st_size;
    *mtime_ns = (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
    *hash = cindex_hash(text, len);
    free(text);
    return true;
}

void crec_stat_files(const StrVec *paths, CRecStatVec *out)
{
    size_t i;
    for (i = 0; i < paths->len; i++) {
        CRecStat s;
        memset(&s, 0, sizeof s);
        s.path = paths->data[i];
        s.absent = !crec_stat_hash(s.path, &s.size, &s.mtime_ns, &s.hash);
        vec_push(out, s);
    }
}

typedef VEC(CRecBlob) BlobVec;

/* The sorted system files as chunks: one ends after a path whose hash is 0
 * mod 16, so about 16 files and a changed header alters one or two chunks. */
static void build_chunks(const FilRow *rows, size_t n, uint64_t build_id,
                         BlobVec *chunks)
{
    size_t i, start = 0;
    for (i = 0; i < n; i++) {
        if (i + 1 == n || hash64(rows[i].path, strlen(rows[i].path), 0) % 16 == 0) {
            Wr w;
            Sec *sec;
            CRecBlob blob;
            size_t k;
            memset(&w, 0, sizeof w);
            sec = wr_sec(&w, T_SYST, sizeof(CRecSys));
            for (k = start; k <= i; k++) {
                CRecSys e;
                memset(&e, 0, sizeof e);
                e.path = st_add(&w.st, rows[k].path);
                e.size = rows[k].size;
                e.mtime_ns = rows[k].mtime_ns;
                e.content_hash = rows[k].hash;
                sec_put(sec, &e);
            }
            wr_finish(&w, CREC_CHUNK, build_id, &blob);
            blob.path = NULL;
            wr_free(&w);
            vec_push(chunks, blob);
            start = i + 1;
        }
    }
}

CRecResult crec_index(Options *o, const char *path, const CRecStatVec *cfg,
                      uint64_t build_id, const CRecLimits *lim, CRecSet *out)
{
    TU tu;
    FrontendOpts fo;
    CIndex *ix = NULL;
    MissList ml;
    Arena ar;
    PPListener l;
    Build b;
    Wr w;
    VEC(FilRow) fils = {0}, sys = {0};
    BlobVec recs = {0}, chunks = {0};
    uint64_t t0 = now_ns(CLOCK_MONOTONIC), started = now_ns(CLOCK_REALTIME),
             bytes = 0, flag_hash = crec_flag_hash(o);
    uint32_t i, nsm, *rec_of = NULL;
    size_t k, nmiss;
    bool ok, over;
    Sec *sec;
    CRecTu t;
    arena_init(&ar);
    memset(&ml, 0, sizeof ml);
    ml.a = &ar;
    o->pp.fatal_missing_include = false;     /* the check goes on (B2 D2) */
    tu_init(&tu, o);
    if (lim) {
        tu.sm.max_bytes = lim->max_bytes;
        tu.sm.limit_cancel = lim->cancel;
        tu.pp.cancel = lim->cancel;
    }
    memset(&l, 0, sizeof l);
    l.ctx = &ml;
    l.include_miss = on_miss;
    pp_add_listener(&tu.pp, l);
    memset(&fo, 0, sizeof fo);
    fo.check = true;
    fo.cidx = &ix;
    if (fault_hit("index-crec"))
        fatal("fault injected at index-crec");
    ok = frontend_run(&tu, path, &fo);
    over = tu.sm.over_limit;
    if (!over && (!ok || !ix)) {
        bool cancelled = ok && lim && lim->cancel && atomic_load_u32(lim->cancel);
        cindex_free(ix);
        tu_free(&tu);
        vec_free(&ml.v);
        arena_free(&ar);
        return cancelled ? CREC_CANCELLED : CREC_UNOPENED;
    }
    memset(&b, 0, sizeof b);
    memset(out, 0, sizeof *out);
    nsm = srcmgr_nfiles(&tu.sm);
    if (!over) {                             /* records: none for a unit over its budget */
        b.ix = ix;
        b.acc = xcalloc(nsm + 1, sizeof *b.acc);
        b.to_sm = xmalloc((ix->nfiles + 1) * sizeof *b.to_sm);
        b.exported = xcalloc(ix->ndecls + 1, 1);
        b.kc = xcalloc(ix->ndecls + 1, sizeof *b.kc);
        b.kc_ok = xcalloc(ix->ndecls + 1, 1);
        b.kidx = xcalloc(ix->ndecls + 1, sizeof *b.kidx);
        b.kstamp = xcalloc(ix->ndecls + 1, sizeof *b.kstamp);
        for (i = 0; i < ix->nfiles; i++) {
            SrcFile *f = cindex_srcfile(&tu.sm, ix->files[i].path);
            b.to_sm[i] = f && f->kind == SF_USER ? f->id : -1;
        }
        collect(&b, &tu);
    }
    rec_of = xcalloc(nsm + 1, sizeof *rec_of);   /* 1 + the record of a file */
    for (i = 0; !over && i < nsm; i++) {
        SrcFile *f = srcmgr_file(&tu.sm, i);
        FileAcc *a = &b.acc[i];
        CRecBlob blob;
        if (f->kind == SF_USER && (a->d.len || a->r.len || a->c.len || a->m.len)) {
            build_record(&b, f, a, build_id, &blob);
            vec_push(&recs, blob);
            rec_of[i] = (uint32_t)recs.len;
        }
        vec_free(&a->d);
        vec_free(&a->r);
        vec_free(&a->c);
        vec_free(&a->m);
    }
    for (i = 0; i < nsm; i++) {              /* every file the unit read */
        SrcFile *f = srcmgr_file(&tu.sm, i);
        FilRow r;
        if (f->kind != SF_USER && f->kind != SF_SYSTEM)
            continue;
        r.path = f->path;
        r.size = f->size;
        r.flags = CFIL_USER;
        r.mtime_ns = f->mtime_ns;
        r.hash = cindex_hash(f->buf, f->size);
        r.digest = rec_of[i] ? recs.data[rec_of[i] - 1].digest : 0;
        bytes += f->size;
        vec_push(f->kind == SF_USER ? &fils : &sys, r);
    }
    for (k = 0; cfg && k < cfg->len; k++) {  /* ... and to build its options */
        FilRow r;
        memset(&r, 0, sizeof r);
        r.path = cfg->data[k].path;
        r.size = cfg->data[k].size;
        r.mtime_ns = cfg->data[k].mtime_ns;
        r.hash = cfg->data[k].hash;
        r.flags = CFIL_CONFIG | (cfg->data[k].absent ? CFIL_ABSENT : 0);
        vec_push(&fils, r);
    }
    if (over && tu.sm.over_path) {           /* so that changing it retries the unit */
        FilRow r;
        memset(&r, 0, sizeof r);
        r.path = tu.sm.over_path;
        r.flags = CFIL_USER;
        if (!crec_stat_hash(r.path, &r.size, &r.mtime_ns, &r.hash))
            r.flags |= CFIL_ABSENT;
        vec_push(&fils, r);
    }
    if (fils.len > 1)
        qsort(fils.data, fils.len, sizeof *fils.data, filrow_cmp);
    if (sys.len > 1)
        qsort(sys.data, sys.len, sizeof *sys.data, filrow_cmp);
    build_chunks(sys.data, sys.len, build_id, &chunks);
    nmiss = over ? 0 : ml.v.len;             /* the file that overran is no miss */
    if (nmiss > 1)
        qsort(ml.v.data, nmiss, sizeof *ml.v.data, miss_cmp);
    memset(&w, 0, sizeof w);
    memset(&t, 0, sizeof t);
    t.flag_hash = flag_hash;
    t.started_ns = started;
    t.written_ns = now_ns(CLOCK_REALTIME);
    t.main_path = st_add(&w.st, path);
    t.errors = over ? 0 : (uint32_t)tu.diag.nerrors;
    t.status = over ? CTU_OVER_LIMIT : t.errors ? CTU_PARTIAL : CTU_OK;
    t.bytes_read = (uint32_t)bytes;
    t.index_ms = (uint32_t)((now_ns(CLOCK_MONOTONIC) - t0) / 1000000u);
    sec = wr_sec(&w, T_TU, sizeof t);
    sec_put(sec, &t);
    sec = wr_sec(&w, T_FILS, sizeof(CRecFil));
    for (k = 0; k < fils.len; k++) {
        CRecFil e;
        memset(&e, 0, sizeof e);
        e.path = st_add(&w.st, fils.data[k].path);
        e.size = fils.data[k].size;
        e.mtime_ns = fils.data[k].mtime_ns;
        e.content_hash = fils.data[k].hash;
        e.record_digest = fils.data[k].digest;
        e.flags = fils.data[k].flags;
        sec_put(sec, &e);
    }
    if (chunks.len) {
        sec = wr_sec(&w, T_SYSC, sizeof(uint64_t));
        for (k = 0; k < chunks.len; k++)
            sec_put(sec, &chunks.data[k].digest);
    }
    if (nmiss) {
        sec = wr_sec(&w, T_MISS, sizeof(CRecMiss));
        for (k = 0; k < nmiss; k++) {
            CRecMiss e;
            if (k && !miss_cmp(&ml.v.data[k - 1], &ml.v.data[k]))
                continue;
            e.spelled = st_add(&w.st, ml.v.data[k].spelled);
            e.from_path = st_add(&w.st, ml.v.data[k].from);
            e.first_dir = ml.v.data[k].first_dir;
            e.how = ml.v.data[k].how;
            sec_put(sec, &e);
        }
    }
    wr_finish(&w, CREC_MANIFEST, build_id, &out->manifest);
    wr_free(&w);
    out->recs = recs.data;
    out->nrecs = recs.len;
    out->chunks = chunks.data;
    out->nchunks = chunks.len;
    out->main = xstrdup(path);
    out->status = (int)t.status;
    out->tukey = crec_tukey(path, flag_hash, build_id);
    vec_free(&fils);
    vec_free(&sys);
    free(rec_of);
    free(b.acc);
    free(b.to_sm);
    free(b.exported);
    free(b.kc);
    free(b.kc_ok);
    free(b.kidx);
    free(b.kstamp);
    vec_free(&ml.v);
    arena_free(&ar);
    cindex_free(ix);
    tu_free(&tu);
    return CREC_DONE;
}

void crec_free(CRecSet *s)
{
    size_t i;
    for (i = 0; i < s->nrecs; i++) {
        free(s->recs[i].data);
        free(s->recs[i].path);
    }
    free(s->recs);
    for (i = 0; i < s->nchunks; i++)
        free(s->chunks[i].data);
    free(s->chunks);
    free(s->manifest.data);
    free(s->main);
    memset(s, 0, sizeof *s);
}

/* ---- the store ------------------------------------------------------------- */

char *crec_path(const char *dir, const char *sub, uint64_t name, const char *ext)
{
    char *p = xmalloc(strlen(dir) + strlen(sub) + strlen(ext) + 32);
    sprintf(p, "%s/%s/%02x/%016llx.%s", dir, sub, (unsigned)(name >> 56),
            (unsigned long long)name, ext);
    return p;
}

static bool put_file(const char *dir, const char *sub, uint64_t name, const char *ext,
                     const CRecBlob *b, char *err, size_t errn)
{
    char *path = crec_path(dir, sub, name, ext), *slash;
    bool ok = true;
    struct stat st;
    if (stat(path, &st) != 0 || !strcmp(ext, "tu")) {   /* a record's name is its content */
        slash = strrchr(path, '/');
        *slash = 0;
        ok = dir_create_all(path, err, errn);
        *slash = '/';
        ok = ok && file_write_atomic(path, b->data, b->len, false, err, errn);
    }
    free(path);
    return ok;
}

bool crec_write(const char *dir, const CRecSet *s, char *err, size_t errn)
{
    size_t i;
    for (i = 0; i < s->nrecs; i++)
        if (!put_file(dir, "rec", s->recs[i].digest, "rec", &s->recs[i], err, errn))
            return false;
    for (i = 0; i < s->nchunks; i++)
        if (!put_file(dir, "sys", s->chunks[i].digest, "sys", &s->chunks[i], err, errn))
            return false;
    return put_file(dir, "tu", s->tukey, "tu", &s->manifest, err, errn);
}

/* ---- the dump -------------------------------------------------------------- */

typedef struct Dump {
    FILE *out;
    const char *root;
    size_t rootn;
    const CRecView *v;           /* the record or manifest being printed */
    char *path, *text;           /* the last file read, for lines */
    size_t len;
} Dump;

static const char *rel(const Dump *d, const char *p)
{
    if (d->root && !strncmp(p, d->root, d->rootn) && p[d->rootn] == '/')
        return p + d->rootn + 1;
    return p;
}

static const char *S(const Dump *d, uint32_t off)
{
    return d->v->strs + off;
}

static void line_col(Dump *d, const char *path, uint32_t off, uint32_t *l, uint32_t *c)
{
    size_t i, ls = 0;
    if (!d->path || strcmp(d->path, path)) {
        free(d->path);
        free(d->text);
        d->path = xstrdup(path);
        d->text = read_file_nobom(path, &d->len, NULL);
    }
    *l = *c = 0;
    if (!d->text)
        return;
    for (i = 0, *l = 1; i < d->len && i < off; i++)
        if (d->text[i] == '\n') {
            (*l)++;
            ls = i + 1;
        }
    *c = off - (uint32_t)ls + 1;
}

static void put_key(Dump *d, uint32_t ki)
{
    const CRecKey *k = &d->v->keys[ki];
    fprintf(d->out, "%s %s", cindex_kind_name(k->kind), S(d, k->name));
    if (k->linkage == 2 && k->path == 0)
        fputs(" ext", d->out);
    else {
        uint32_t l, c;
        line_col(d, S(d, k->path), k->off, &l, &c);
        fprintf(d->out, " @%s:%u:%u", rel(d, S(d, k->path)), l, c);
    }
}

static void put_site(Dump *d, const char *path, uint32_t off, unsigned len)
{
    uint32_t l, c;
    line_col(d, path, off, &l, &c);
    fprintf(d->out, "%u:%u +%u len %u ", l, c, off, len);
}

static void dump_record(Dump *d, const CRecBlob *b)
{
    CRecView v;
    const char *err = crec_parse(b->data, b->len, CREC_RECORD, ((CRecHeader *)b->data)->build_id, &v);
    const char *path;
    uint32_t i;
    if (err) {
        fprintf(d->out, "record %s: INVALID: %s\n", rel(d, b->path), err);
        return;
    }
    d->v = &v;
    path = S(d, v.file->path);
    fprintf(d->out, "record %s size %u hash %016llx\n", rel(d, path), v.file->size,
            (unsigned long long)v.file->content_hash);
    for (i = 0; i < v.ndecls; i++) {
        const CRecDecl *e = &v.decls[i];
        fputs("  decl ", d->out);
        put_site(d, path, e->off, e->len);
        fprintf(d->out, "%s ", e->role == CIX_DEF ? "DEF" : "DECL");
        put_key(d, e->key);
        if (*S(d, e->container))
            fprintf(d->out, " in %s", S(d, e->container));
        fprintf(d->out, " range +%u-%u flags %u\n", e->ext_begin, e->ext_end, e->flags);
    }
    for (i = 0; i < v.nrefs; i++) {
        const CRecRef *e = &v.refs[i];
        fputs("  ref ", d->out);
        put_site(d, path, e->off, e->len);
        put_key(d, e->key);
        cindex_print_flags(d->out, e->flags);
        fputc('\n', d->out);
    }
    for (i = 0; i < v.ncalls; i++) {
        const CRecCall *e = &v.calls[i];
        fputs("  call ", d->out);
        put_site(d, path, e->off, e->len);
        fputs("from ", d->out);
        put_key(d, e->caller);
        fputs(" to ", d->out);
        put_key(d, e->callee);
        cindex_print_flags(d->out, e->flags);
        fputc('\n', d->out);
    }
    for (i = 0; i < v.nmacros; i++) {
        const CRecMacro *e = &v.macros[i];
        fputs("  macro ", d->out);
        put_site(d, path, e->off, e->len);
        fprintf(d->out, "%s%s\n", S(d, e->name), e->funclike ? "()" : "");
    }
}

typedef struct DumpRow {         /* one "file" line */
    const char *path;
    uint32_t size;
    uint64_t hash;
    const char *kind;
    bool record;
} DumpRow;

static int row_cmp(const void *pa, const void *pb)
{
    return strcmp(((const DumpRow *)pa)->path, ((const DumpRow *)pb)->path);
}

static int blob_path_cmp(const void *pa, const void *pb)
{
    return strcmp(((const CRecBlob *)pa)->path, ((const CRecBlob *)pb)->path);
}

void crec_dump(const CRecSet *s, const char *root, FILE *out)
{
    static const char *const status[] = {"?", "OK", "PARTIAL", "OVER_LIMIT", "FAILED"};
    Dump d;
    CRecView v;
    CRecBlob *sorted = xmalloc((s->nrecs + 1) * sizeof *sorted);
    const char *err = crec_parse(s->manifest.data, s->manifest.len, CREC_MANIFEST,
                                 ((CRecHeader *)s->manifest.data)->build_id, &v);
    uint32_t i;
    size_t k;
    memset(&d, 0, sizeof d);
    d.out = out;
    d.root = root;
    d.rootn = root ? strlen(root) : 0;
    d.v = &v;
    if (err) {
        fprintf(out, "manifest: INVALID: %s\n", err);
        free(sorted);
        return;
    }
    fprintf(out, "unit %s status %s errors %u bytes %u\n", rel(&d, S(&d, v.tu->main_path)),
            status[v.tu->status < 5 ? v.tu->status : 0], v.tu->errors,
            v.tu->bytes_read);
    /* the files in path order, the chunks' system files merged back in */
    {
        VEC(DumpRow) rows = {0};
        VEC(CRecView) cvs = {0};
        size_t nrow;
        for (i = 0; i < v.nfils; i++) {
            const CRecFil *e = &v.fils[i];
            DumpRow r = {S(&d, e->path), e->size, e->content_hash,
                         e->flags & CFIL_ABSENT ? "absent"
                         : e->flags & CFIL_CONFIG ? "config" : "user",
                         e->record_digest != 0};
            vec_push(&rows, r);
        }
        for (k = 0; k < s->nchunks; k++) {
            CRecView cv;
            if (crec_parse(s->chunks[k].data, s->chunks[k].len, CREC_CHUNK,
                           ((CRecHeader *)s->chunks[k].data)->build_id, &cv)) {
                fprintf(out, "  chunk %zu: INVALID\n", k);
                continue;
            }
            vec_push(&cvs, cv);
            for (i = 0; i < cv.nsyst; i++) {
                DumpRow r = {cv.strs + cv.syst[i].path, cv.syst[i].size,
                             cv.syst[i].content_hash, "system", false};
                vec_push(&rows, r);
            }
        }
        nrow = rows.len;
        if (nrow > 1)
            qsort(rows.data, nrow, sizeof *rows.data, row_cmp);
        for (k = 0; k < nrow; k++)
            fprintf(out, "  file %s size %u hash %016llx %s%s\n", rel(&d, rows.data[k].path),
                    rows.data[k].size, (unsigned long long)rows.data[k].hash,
                    rows.data[k].kind, rows.data[k].record ? " record" : "");
        vec_free(&rows);
        vec_free(&cvs);
    }
    for (i = 0; i < v.nmiss; i++)
        fprintf(out, "  miss %c%s%c from %s\n", v.miss[i].how & CMISS_ANGLED ? '<' : '"',
                S(&d, v.miss[i].spelled), v.miss[i].how & CMISS_ANGLED ? '>' : '"',
                rel(&d, S(&d, v.miss[i].from_path)));
    memcpy(sorted, s->recs, s->nrecs * sizeof *sorted);
    if (s->nrecs > 1)
        qsort(sorted, s->nrecs, sizeof *sorted, blob_path_cmp);
    for (k = 0; k < s->nrecs; k++)
        dump_record(&d, &sorted[k]);
    free(sorted);
    free(d.path);
    free(d.text);
}
