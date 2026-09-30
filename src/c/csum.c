/* csum.c - per-unit summaries and read sets for incremental checking
 * (csum.h, docs/TYPES.md "Summaries and read sets").
 *
 * Hot path: clookup calls csum_read only when a lookup resolved to a
 * file-scope binding (or to none), and csum_read de-duplicates with a
 * per-identifier stamp, so a unit costs one append per distinct file-scope
 * name it mentions.  Record and enum contents are reported by type.c's
 * accessors (rec_rd / enum_rd) for records older than the unit.  Nothing
 * runs per node. */
#include "c/check_int.h"
#include "hash.h"

#include <stdlib.h>
#include <string.h>

typedef struct Arr32 { uint32_t *p; size_t n; } Arr32;
typedef struct Arr64 { uint64_t *p; size_t n; } Arr64;

typedef struct CSum {
    CSumFast fast;                /* first: check.c reads it */
    Checker *c;
    uint32_t seq;                 /* the unit's stamp, from 1 */
    size_t nunit;                 /* units checked so far */
    UnitSummary cur;
    VEC(SumEntry) ents;
    VEC(SumRead) reads;
    VEC(uint64_t) touched;        /* ns << 32 | ident */
    Arr32 rs[3], ts[3];           /* read / touch stamps by ident */
    Arr32 recst, enst;            /* layout read stamps by record / enum */
    Arr32 ordcnt;                 /* identity ordinals by tag ident */
    Arr64 tdig;                   /* type digest memo by type index */
    Arr64 rl;                     /* layout digest memo by record */
    Arr64 rid, eid;               /* identity digest by record / enum */
    Arr64 siface;                 /* interface digest memo by global symbol */
    uint64_t *mkey;               /* identity -> record / enum */
    uint32_t *mval;               /* index | ENUM_BIT; 0 empty */
    size_t mcap, mcount;
    uint64_t key_override;
    uint64_t pack0;               /* #pragma pack state at unit entry */
    uint32_t rec0, enum0;         /* first record / enum of the unit */
    bool text;                    /* keep printed types in the entries */
    FILE *dump;
    UnitSummary *loaded;
    long nloaded;
} CSum;

#define ENUM_BIT 0x80000000u
#define H(a, b) hash64_mix((a), (b))

/* distinct seeds for the digests */
enum {
    D_BUILTIN = 0x100, D_PTR, D_ARRAY, D_VLA, D_FUNC, D_REC, D_ENUM, D_TYPEDEF,
    D_VECTOR, D_COMPLEX, D_QUAL, D_SYM, D_TAG, D_LAYOUT_REC, D_LAYOUT_ENUM,
    D_INCOMPLETE, D_IDENT, D_PACK, D_ENTRY, D_KEY, D_UNIT, D_TOKS
};

/* ---- growable arrays --------------------------------------------------- */

static uint32_t *st32(Arr32 *a, size_t i)
{
    if (i >= a->n) {
        size_t n = a->n ? a->n : 1024;
        while (n <= i)
            n *= 2;
        a->p = xrealloc(a->p, n * sizeof *a->p);
        memset(a->p + a->n, 0, (n - a->n) * sizeof *a->p);
        a->n = n;
    }
    return &a->p[i];
}

/* The read stamp of ident in namespace ns (keeps the inline copy in sync). */
static uint32_t *rstamp(CSum *cs, int ns, uint32_t ident)
{
    uint32_t *p = st32(&cs->rs[ns], ident);
    if (ns < 2) {
        cs->fast.rs[ns] = cs->rs[ns].p;
        cs->fast.rn[ns] = cs->rs[ns].n;
    }
    return p;
}

static uint64_t *st64(Arr64 *a, size_t i)
{
    if (i >= a->n) {
        size_t n = a->n ? a->n : 1024;
        while (n <= i)
            n *= 2;
        a->p = xrealloc(a->p, n * sizeof *a->p);
        memset(a->p + a->n, 0, (n - a->n) * sizeof *a->p);
        a->n = n;
    }
    return &a->p[i];
}

static uint64_t get64(const Arr64 *a, size_t i)
{
    return i < a->n ? a->p[i] : 0;
}

/* ---- identity -> record map --------------------------------------------- */

static void map_put(CSum *cs, uint64_t key, uint32_t val)
{
    size_t i;
    if ((cs->mcount + 1) * 2 > cs->mcap) {
        uint64_t *ok = cs->mkey;
        uint32_t *ov = cs->mval;
        size_t oc = cs->mcap, k;
        cs->mcap = oc ? oc * 2 : 256;
        cs->mkey = xcalloc(cs->mcap, sizeof *cs->mkey);
        cs->mval = xcalloc(cs->mcap, sizeof *cs->mval);
        cs->mcount = 0;
        for (k = 0; k < oc; k++)
            if (ov[k])
                map_put(cs, ok[k], ov[k]);
        free(ok);
        free(ov);
    }
    for (i = key & (cs->mcap - 1); cs->mval[i]; i = (i + 1) & (cs->mcap - 1))
        if (cs->mkey[i] == key) {
            cs->mval[i] = val;
            return;
        }
    cs->mkey[i] = key;
    cs->mval[i] = val;
    cs->mcount++;
}

static uint32_t map_get(const CSum *cs, uint64_t key)
{
    size_t i;
    if (!cs->mcap)
        return 0;
    for (i = key & (cs->mcap - 1); cs->mval[i]; i = (i + 1) & (cs->mcap - 1))
        if (cs->mkey[i] == key)
            return cs->mval[i];
    return 0;
}

/* ---- digests of types ---------------------------------------------------- */

static uint64_t ident_dig(const CSum *cs, uint32_t id)
{
    return id ? ident_by_id(cs->c->in, id)->digest : 0;
}

static const char *ident_str(const CSum *cs, uint32_t id)
{
    return id ? ident_by_id(cs->c->in, id)->str : "-";
}

static uint64_t tdigest(CSum *cs, TypeId t);

static uint64_t tbase(CSum *cs, uint32_t idx)
{
    TypeTable *tt = &cs->c->tt;
    TypeEnt e;
    uint64_t d, m = get64(&cs->tdig, idx);
    uint32_t k;
    if (m)
        return m;
    e = tt->ents.data[idx];
    switch (e.kind) {
    case TY_PTR:
        d = H(D_PTR, tdigest(cs, e.base));
        break;
    case TY_ARRAY:
        d = H(H(D_ARRAY, e.flags), H(e.n, tdigest(cs, e.base)));
        break;
    case TY_VLA:
        d = H(D_VLA, tdigest(cs, e.base));
        break;
    case TY_FUNC:
        d = H(H(D_FUNC, e.flags), H(e.n, tdigest(cs, e.base)));
        for (k = 0; k < e.n; k++)
            d = H(d, tdigest(cs, tt->params.data[e.extra + k]));
        break;
    case TY_STRUCT: case TY_UNION:
        d = H(H(D_REC, e.kind), get64(&cs->rid, e.extra));
        break;
    case TY_ENUM:
        d = H(D_ENUM, get64(&cs->eid, e.extra));
        break;
    case TY_TYPEDEF:
        d = H(H(D_TYPEDEF, ident_dig(cs, e.extra)),
              H(e.align | (uint64_t)e.flags << 16, tdigest(cs, e.base)));
        break;
    case TY_VECTOR:
        d = H(H(D_VECTOR, e.n), tdigest(cs, e.base));
        break;
    case TY_COMPLEX:
        d = H(D_COMPLEX, tdigest(cs, e.base));
        break;
    default:
        d = H(D_BUILTIN, e.kind);
        break;
    }
    if (!d)
        d = 1;
    *st64(&cs->tdig, idx) = d;
    return d;
}

static uint64_t tdigest(CSum *cs, TypeId t)
{
    uint64_t d = tbase(cs, TYPE_IDX(t));
    return TYPE_QUALS(t) ? H(H(D_QUAL, TYPE_QUALS(t)), d) : d;
}

static uint64_t rec_layout(CSum *cs, uint32_t idx)
{
    TypeTable *tt = &cs->c->tt;
    const Record *r = &tt->recs.data[idx];
    uint64_t d = get64(&cs->rl, idx);
    uint32_t k;
    if (d)
        return d;
    if (!(r->flags & RF_COMPLETE))
        return H(D_INCOMPLETE, r->flags & RF_UNION);
    d = H(H(D_LAYOUT_REC, r->flags), H(r->size, r->align));
    d = H(d, r->nfields);
    for (k = 0; k < r->nfields; k++) {
        Field f = tt->fields.data[r->fields + k];
        d = H(d, H(ident_dig(cs, f.name), tdigest(cs, f.ty)));
        d = H(d, H(f.off_bits, f.width | (uint64_t)f.flags << 32));
        d = H(d, f.align);
    }
    if (!d)
        d = 1;
    *st64(&cs->rl, idx) = d;
    return d;
}

static uint64_t enum_layout(CSum *cs, uint32_t idx)
{
    const Enum *e = &cs->c->tt.enums.data[idx];
    uint64_t d;
    if (!e->complete)
        return H(D_INCOMPLETE, 2);
    d = H(H(D_LAYOUT_ENUM, e->packed), tdigest(cs, e->underlying));
    return d ? d : 1;
}

/* ---- symbols and tags ----------------------------------------------------- */

static const CSym *sym_of(Checker *c, uint32_t ref)
{
    if (ref == SYM_NONE)
        return NULL;
    if (ref & SYM_LOCAL)
        return (ref & ~SYM_LOCAL) < c->lsyms.len ? csym(c, ref) : NULL;
    return ref < c->gsyms.len ? csym(c, ref) : NULL;
}

static uint32_t sflags(const CSym *s)
{
    uint32_t f = 0;
    if (s->flags & CSF_DEFINED) f |= SF_DEFINED;
    if (s->flags & CSF_TENTATIVE) f |= SF_TENTATIVE;
    if (s->flags & CSF_INLINE) f |= SF_INLINE;
    if (s->flags & CSF_THREAD) f |= SF_THREAD;
    if (s->flags & CSF_NORETURN) f |= SF_NORETURN;
    if (s->flags & CSF_WEAK) f |= SF_WEAK;
    if (s->flags & CSF_IMPLICIT) f |= SF_IMPLICIT;
    if (s->flags & CSF_ERROR) f |= SF_ERROR;
    if (s->flags & CSF_PROTO_DEF) f |= SF_PROTO_DEF;
    if (s->flags & CSF_KR_DEF) f |= SF_KR_DEF;
    if (s->flags & CSF_CONST_INIT) f |= SF_CONST_INIT;
    if (s->flags & CSF_DECL_EXTERNAL) f |= SF_DECL_EXTERNAL;
    if (s->flags & CSF_REGISTER_NAMED) f |= SF_REGISTER;
    return f;
}

/* What readers of the symbol see, and what only a redeclaration sees. */
enum { IFACE_MASK = SF_INLINE | SF_THREAD | SF_NORETURN | SF_WEAK |
                    SF_IMPLICIT | SF_ERROR | SF_PROTO_DEF | SF_KR_DEF |
                    SF_CONST_INIT | SF_REGISTER };

static void sym_fill(CSum *cs, const CSym *s, SumEntry *e)
{
    uint32_t f = sflags(s);
    uint64_t d;
    e->kind = s->kind;
    e->linkage = s->linkage;
    e->sc = s->sc;
    e->flags = f;
    e->type_dig = tdigest(cs, s->ty);
    e->value = 0;
    d = H(H(D_SYM, s->kind), H(s->linkage | (uint32_t)s->sc << 8,
                               f & IFACE_MASK));
    d = H(d, H(s->align, e->type_dig));
    if (s->kind == CS_ENUMCONST) {
        e->value = s->val;
        d = H(d, H(s->val, tdigest(cs, s->vty)));
    } else if (s->flags & CSF_CONST_INIT)
        d = H(d, s->val);
    if (!d)
        d = 1;
    e->iface = d;
    e->full = H(d, f & ~(uint32_t)IFACE_MASK);
}

static int tag_kind(const CSum *cs, TypeId t)
{
    TypeKind k = type_ckind((TypeTable *)&cs->c->tt, t);
    return k == TY_ENUM ? 2 : k == TY_UNION ? 1 : 0;
}

/* The index of a tag's record / enum. */
static uint32_t tag_index(const CSum *cs, TypeId t)
{
    const TypeTable *tt = &cs->c->tt;
    return tt->ents.data[TYPE_IDX(type_canon((TypeTable *)tt, t))].extra;
}

static uint64_t tag_ident(const CSum *cs, TypeId t)
{
    return tag_kind(cs, t) == 2 ? get64(&cs->eid, tag_index(cs, t))
                                : get64(&cs->rid, tag_index(cs, t));
}

static void tag_fill(CSum *cs, TypeId t, SumEntry *e)
{
    int k = tag_kind(cs, t);
    uint32_t idx = tag_index(cs, t);
    uint64_t id = tag_ident(cs, t);
    bool complete;
    e->kind = (uint8_t)k;
    e->linkage = 0;
    e->sc = 0;
    e->value = 0;
    e->type_dig = H(D_TAG, id);
    if (k == 2) {
        complete = cs->c->tt.enums.data[idx].complete;
        e->layout_dig = enum_layout(cs, idx);
    } else {
        complete = (cs->c->tt.recs.data[idx].flags & RF_COMPLETE) != 0;
        e->layout_dig = rec_layout(cs, idx);
    }
    e->flags = complete ? SF_COMPLETE : 0;
    e->iface = H(H(D_TAG, k), id);
    e->full = H(e->iface, complete);
}

/* The file-scope binding of ident (log index + 1), 0: none. */
static uint32_t file_binding(Checker *c, int ns, uint32_t ident)
{
    uint32_t b, fe;
    if (ident >= c->nidents)
        return 0;
    b = c->top[ns][ident];
    fe = c->scopes.len > 1 ? c->scopes.data[1].log : UINT32_MAX;
    while (b && b - 1 >= fe)
        b = c->log.data[b - 1].prev;
    return b;
}

static uint32_t file_ref(Checker *c, uint32_t ident)
{
    uint32_t b = file_binding(c, NS_ORD, ident);
    return b ? c->log.data[b - 1].ref : SYM_NONE;
}

/* The entity ident denotes in namespace ns as the file scope has it now;
 * false if there is none. */
static bool entity_now(CSum *cs, int ns, uint32_t ident, SumEntry *e)
{
    Checker *c = cs->c;
    memset(e, 0, sizeof *e);
    e->ns = (uint8_t)ns;
    if (ns == SUM_TAG) {
        uint32_t b = file_binding(c, NS_TAG, ident);
        if (!b)
            return false;
        tag_fill(cs, c->log.data[b - 1].ref, e);
        return true;
    } else {
        const CSym *s;
        uint32_t ref;
        if (ns == SUM_EXT) {
            if (ident >= c->nidents || !c->ext[ident])
                return false;
            ref = c->ext[ident] - 1;
        } else {
            uint32_t b = file_binding(c, NS_ORD, ident);
            if (!b)
                return false;
            ref = c->log.data[b - 1].ref;
        }
        s = sym_of(c, ref);
        if (!s)
            return false;
        sym_fill(cs, s, e);
        return true;
    }
}

static uint64_t nz(uint64_t d)
{
    return d ? d : 1;
}

/* The digest readers see of a symbol, memoized for the persistent symbols;
 * csum_touch / the unit's end drop the memo of what a unit changes. */
static uint64_t sym_iface(CSum *cs, uint32_t ref)
{
    const CSym *s = sym_of(cs->c, ref);
    SumEntry e;
    if (!s)
        return 1;
    if (!(ref & SYM_LOCAL)) {
        uint64_t m = get64(&cs->siface, ref);
        if (m)
            return m;
    }
    sym_fill(cs, s, &e);
    if (!(ref & SYM_LOCAL))
        *st64(&cs->siface, ref) = nz(e.iface);
    return nz(e.iface);
}

static void sym_forget(CSum *cs, uint32_t ref)
{
    if (ref != SYM_NONE && !(ref & SYM_LOCAL) && ref < cs->siface.n)
        cs->siface.p[ref] = 0;
}

/* ---- the read set ---------------------------------------------------------- */

static void add_read(CSum *cs, const char *name, int ns, int mode,
                     uint64_t key, uint64_t digest)
{
    SumRead r;
    r.name = name;
    r.ns = (uint8_t)ns;
    r.mode = (uint8_t)mode;
    r.key = key;
    r.digest = digest;
    vec_push(&cs->reads, r);
}

void csum_read(Checker *c, int ns, uint32_t ident, uint32_t b)
{
    CSum *cs = c->cs;
    uint32_t *st;
    uint64_t d = 0;
    if (!ident)
        return;
    st = rstamp(cs, ns, ident);
    if (*st == cs->seq)
        return;
    *st = cs->seq;
    if (b) {
        uint32_t ref = c->log.data[b - 1].ref;
        if (ns == SUM_TAG)
            d = nz(H(H(D_TAG, tag_kind(cs, ref)),
                     tag_ident(cs, ref)));
        else
            d = sym_iface(cs, ref);
    }
    add_read(cs, ident_str(cs, ident), ns, RD_NAME, 0, d);
}

void csum_read_ext(Checker *c, uint32_t ident)
{
    CSum *cs = c->cs;
    uint32_t *st;
    SumEntry e;
    if (!cs || !ident)
        return;
    st = rstamp(cs, SUM_EXT, ident);
    if (*st == cs->seq)
        return;
    *st = cs->seq;
    add_read(cs, ident_str(cs, ident), SUM_EXT, RD_NAME, 0,
             entity_now(cs, SUM_EXT, ident, &e) ? nz(e.iface) : 0);
}

void csum_touch(Checker *c, int ns, uint32_t ident)
{
    CSum *cs = c->cs;
    uint32_t *st;
    SumEntry e;
    bool have;
    if (!cs || !ident)
        return;
    st = st32(&cs->ts[ns], ident);
    if (*st == cs->seq)
        return;
    *st = cs->seq;
    *rstamp(cs, ns, ident) = cs->seq;      /* later reads are its own */
    have = entity_now(cs, ns, ident, &e);
    add_read(cs, ident_str(cs, ident), ns, RD_FULL, 0, have ? nz(e.full) : 0);
    if (ns != SUM_TAG)
        sym_forget(cs, ns == SUM_EXT ? (ident < c->nidents && c->ext[ident]
                                            ? c->ext[ident] - 1 : SYM_NONE)
                       : file_ref(c, ident));
    vec_push(&cs->touched, (uint64_t)ns << 32 | ident);
    if (have && ns == SUM_TAG) {
        /* completing or defining the tag: its contents are the unit's */
        uint32_t idx = tag_index(cs, c->log.data[file_binding(c, NS_TAG,
                                                              ident) - 1].ref);
        *st32(e.kind == 2 ? &cs->enst : &cs->recst, idx) = cs->seq;
    }
}

void csum_decl(Checker *c, uint32_t ident, bool filescope, bool external)
{
    if (!c->cs || !ident)
        return;
    if (filescope) {
        csum_touch(c, SUM_ORD, ident);
        if (external)
            csum_touch(c, SUM_EXT, ident);
    } else if (external) {
        csum_touch(c, SUM_EXT, ident);
        if (file_binding(c, NS_ORD, ident))
            csum_touch(c, SUM_ORD, ident);
    }
}

static uint64_t pack_digest(const Checker *c)
{
    uint64_t d = H(D_PACK, c->pack);
    size_t k;
    for (k = 0; k < c->pack_stack.len; k++)
        d = H(d, c->pack_stack.data[k]);
    return nz(d);
}

void csum_read_pack(Checker *c)
{
    CSum *cs = c->cs;
    uint32_t *st;
    if (!cs)
        return;
    st = rstamp(cs, 0, 0);                    /* ident 0 is never a name */
    if (*st == cs->seq)
        return;
    *st = cs->seq;
    add_read(cs, "pack", SUM_STATE, RD_NAME, 0, cs->pack0);
}

/* A record's or enum's contents were read (type.c). */
static void rd_hook(void *ctx, uint32_t idx, bool is_enum)
{
    CSum *cs = ctx;
    uint32_t *st = st32(is_enum ? &cs->enst : &cs->recst, idx);
    uint32_t tag;
    uint64_t id;
    if (*st == cs->seq)
        return;
    *st = cs->seq;
    if (is_enum) {
        tag = cs->c->tt.enums.data[idx].tag;
        id = get64(&cs->eid, idx);
        add_read(cs, ident_str(cs, tag), SUM_LAYOUT, RD_LAYOUT, id,
                 nz(enum_layout(cs, idx)));
    } else {
        tag = cs->c->tt.recs.data[idx].tag;
        id = get64(&cs->rid, idx);
        add_read(cs, ident_str(cs, tag), SUM_LAYOUT, RD_LAYOUT, id,
                 nz(rec_layout(cs, idx)));
    }
}

/* ---- units ------------------------------------------------------------------- */

static void free_entries(CSum *cs)
{
    size_t k;
    for (k = 0; k < cs->ents.len; k++)
        free(cs->ents.data[k].type_text);
    cs->ents.len = 0;
}

static const char *const ns_name[] = { "ord", "tag", "ext", "layout", "state" };
static const char *const mode_name[] = { "name", "full", "layout" };

static int cmp_entry(const void *a, const void *b)
{
    const SumEntry *x = a, *y = b;
    if (x->ns != y->ns)
        return x->ns < y->ns ? -1 : 1;
    return strcmp(x->name, y->name);
}

static int cmp_read(const void *a, const void *b)
{
    const SumRead *x = a, *y = b;
    int r;
    if (x->ns != y->ns)
        return x->ns < y->ns ? -1 : 1;
    if (x->mode != y->mode)
        return x->mode < y->mode ? -1 : 1;
    r = strcmp(x->name, y->name);
    if (r)
        return r;
    return x->key < y->key ? -1 : x->key > y->key;
}

/* Gives the records and enums of the unit their identity digests: the
 * unit's key, the tag (or "anonymous"), and the ordinal among the unit's
 * with the same tag. */
static void assign_ids(CSum *cs, uint64_t key)
{
    TypeTable *tt = &cs->c->tt;
    uint32_t i, anon = 0;
    size_t k;
    VEC(uint32_t) used = {0};
    for (int pass = 0; pass < 2; pass++) {
        uint32_t lo = pass ? cs->enum0 : cs->rec0;
        uint32_t hi = pass ? (uint32_t)tt->enums.len : (uint32_t)tt->recs.len;
        anon = 0;
        for (i = lo; i < hi; i++) {
            uint32_t tag = pass ? tt->enums.data[i].tag
                                : tt->recs.data[i].tag;
            uint32_t ord;
            uint64_t id;
            if (tag) {
                uint32_t *cnt = st32(&cs->ordcnt, tag);
                ord = (*cnt)++;
                vec_push(&used, tag);
            } else
                ord = anon++;
            id = H(H(D_KEY, key), H(H(pass, ident_dig(cs, tag)), ord));
            if (!pass && (tt->recs.data[i].flags & RF_UNION))
                id = H(id, 1);
            id = nz(id);
            *st64(pass ? &cs->eid : &cs->rid, i) = id;
            map_put(cs, id, i | (pass ? ENUM_BIT : 0));
        }
        for (k = 0; k < used.len; k++)
            cs->ordcnt.p[used.data[k]] = 0;
        used.len = 0;
    }
    vec_free(&used);
}

/* The key of a unit that declares no names: its tokens. */
static uint64_t token_key(const Checker *c)
{
    uint64_t d = D_TOKS;
    uint32_t i;
    for (i = 0; i < c->u->ntoks; i++) {
        const Tok *t = &c->u->toks[i].t;
        d = H(d, (uint64_t)t->kind | (uint64_t)t->punct << 8 |
                     (uint64_t)t->len << 16);
        if (t->kind == TK_IDENT)
            d = H(d, ident_dig(c->cs, t->aux));
    }
    return nz(d);
}

static void show_type(CSum *cs, SumEntry *e, TypeId t)
{
    Checker *c = cs->c;
    c->sb.len = 0;
    type_print(&c->tt, &c->sb, t);
    e->type_text = xstrdup(sb_cstr(&c->sb));
}

void csum_unit_begin(Checker *c)
{
    CSum *cs = c->cs;
    cs->seq++;
    cs->fast.seq = cs->seq;
    free_entries(cs);
    cs->reads.len = 0;
    cs->touched.len = 0;
    cs->rec0 = (uint32_t)c->tt.recs.len;
    cs->enum0 = (uint32_t)c->tt.enums.len;
    c->tt.unit_rec0 = cs->rec0;
    c->tt.unit_enum0 = cs->enum0;
    c->tt.rd_hook = rd_hook;
    c->tt.rd_ctx = cs;
    cs->pack0 = pack_digest(c);
    if (cs->loaded && cs->dump) {
        size_t n = cs->nunit;
        if (n >= (size_t)cs->nloaded)
            fprintf(cs->dump, "validate unit %zu: no summary\n", n);
        else {
            const SumRead *bad = NULL;
            if (summary_valid(&cs->loaded[n], checker_file_digest, c, &bad))
                fprintf(cs->dump, "validate unit %zu: valid\n", n);
            else
                fprintf(cs->dump, "validate unit %zu: invalid (%s %s %s)\n", n,
                        ns_name[bad->ns], mode_name[bad->mode], bad->name);
        }
    }
}

void csum_unit_end(Checker *c)
{
    CSum *cs = c->cs;
    UnitSummary *u = &cs->cur;
    size_t k;
    uint64_t key, d, sig;
    c->tt.unit_rec0 = c->tt.unit_enum0 = 0;
    /* the unit's key: the names it declares, unless the caller gave one */
    if (cs->key_override)
        key = cs->key_override;
    else if (cs->touched.len) {
        VEC(uint64_t) v = {0};
        for (k = 0; k < cs->touched.len; k++) {
            uint64_t t = cs->touched.data[k];
            vec_push(&v, H(t >> 32, ident_dig(cs, (uint32_t)t)));
        }
        /* order independent: the declarations' order is text, but a
         * redeclaration order must not matter */
        d = D_KEY;
        {
            /* sort for a canonical order */
            size_t i, j;
            for (i = 1; i < v.len; i++) {
                uint64_t x = v.data[i];
                for (j = i; j && v.data[j - 1] > x; j--)
                    v.data[j] = v.data[j - 1];
                v.data[j] = x;
            }
        }
        for (k = 0; k < v.len; k++)
            d = H(d, v.data[k]);
        vec_free(&v);
        key = nz(d);
    } else
        key = token_key(c);
    cs->key_override = 0;
    assign_ids(cs, key);
    for (k = 0; k < cs->touched.len; k++) {
        uint64_t t = cs->touched.data[k];
        uint32_t ident = (uint32_t)t;
        if ((int)(t >> 32) == SUM_TAG)
            continue;
        sym_forget(cs, (int)(t >> 32) == SUM_EXT
                           ? (ident < c->nidents && c->ext[ident]
                                  ? c->ext[ident] - 1 : SYM_NONE)
                           : file_ref(c, ident));
    }
    /* entries */
    for (k = 0; k < cs->touched.len; k++) {
        uint64_t t = cs->touched.data[k];
        int ns = (int)(t >> 32);
        uint32_t ident = (uint32_t)t;
        SumEntry e;
        if (!entity_now(cs, ns, ident, &e))
            continue;
        if (ns == SUM_EXT) {
            /* the same symbol as the ordinary entry: one entry */
            uint32_t fr = file_ref(c, ident);
            if (fr != SYM_NONE && c->ext[ident] && fr == c->ext[ident] - 1)
                continue;
        }
        e.name = ident_str(cs, ident);
        if (cs->text && ns != SUM_TAG) {
            uint32_t ref = SYM_NONE;
            if (ns == SUM_EXT)
                ref = c->ext[ident] - 1;
            else
                ref = c->log.data[file_binding(c, NS_ORD, ident) - 1].ref;
            show_type(cs, &e, sym_of(c, ref)->ty);
        }
        e.digest = H(H(D_ENTRY, e.ns), H(ident_dig(cs, ident), e.full));
        if (ns == SUM_TAG)
            e.digest = H(e.digest, e.layout_dig);
        vec_push(&cs->ents, e);
    }
    if (pack_digest(c) != cs->pack0) {
        SumEntry e;
        memset(&e, 0, sizeof e);
        e.ns = SUM_STATE;
        e.name = "pack";
        e.iface = e.full = pack_digest(c);
        e.digest = H(H(D_ENTRY, SUM_STATE), e.full);
        vec_push(&cs->ents, e);
        add_read(cs, "pack", SUM_STATE, RD_NAME, 0, cs->pack0);
    }
    if (cs->ents.len > 1)
        qsort(cs->ents.data, cs->ents.len, sizeof *cs->ents.data, cmp_entry);
    if (cs->reads.len > 1)
        qsort(cs->reads.data, cs->reads.len, sizeof *cs->reads.data, cmp_read);
    d = sig = D_UNIT;
    for (k = 0; k < cs->ents.len; k++) {
        d = H(d, cs->ents.data[k].digest);
        if (!(cs->ents.data[k].flags & SF_IMPLICIT))
            sig = H(sig, cs->ents.data[k].digest);
    }
    u->key = key;
    u->digest = d;
    u->sig = sig;
    u->errors = c->quiet;
    u->entries = cs->ents.data;
    u->nentries = cs->ents.len;
    u->reads = cs->reads.data;
    u->nreads = cs->reads.len;
    if (cs->dump)
        summary_write(cs->dump, u, cs->nunit);
    cs->nunit++;
}

const UnitSummary *checker_summary(const Checker *c)
{
    return c->cs ? &c->cs->cur : NULL;
}

void checker_set_unit_key(Checker *c, uint64_t key)
{
    if (c->cs)
        c->cs->key_override = key;
}

CSum *csum_new(Checker *c)
{
    CSum *cs = xcalloc(1, sizeof *cs);
    cs->c = c;
    c->csf = &cs->fast;
    cs->text = c->opt.dump_summaries != NULL;
    cs->dump = c->opt.dump_summaries;
    /* records made before any unit (__va_list_tag) */
    cs->rec0 = 0;
    cs->enum0 = 0;
    assign_ids(cs, 0);
    if (c->opt.validate_summaries) {
        FILE *f = fopen(c->opt.validate_summaries, "r");
        if (f) {
            cs->nloaded = summary_load(f, &cs->loaded);
            fclose(f);
        }
        if (cs->nloaded < 0) {
            cs->nloaded = 0;
            fprintf(stderr, "cereal: bad summaries file '%s'\n",
                    c->opt.validate_summaries);
        }
    }
    return cs;
}

void csum_free(Checker *c)
{
    CSum *cs = c->cs;
    int k;
    if (!cs)
        return;
    free_entries(cs);
    vec_free(&cs->ents);
    vec_free(&cs->reads);
    vec_free(&cs->touched);
    for (k = 0; k < 3; k++) {
        free(cs->rs[k].p);
        free(cs->ts[k].p);
    }
    free(cs->recst.p);
    free(cs->enst.p);
    free(cs->ordcnt.p);
    free(cs->tdig.p);
    free(cs->rl.p);
    free(cs->rid.p);
    free(cs->eid.p);
    free(cs->siface.p);
    free(cs->mkey);
    free(cs->mval);
    if (cs->loaded)
        summary_free(cs->loaded, (size_t)cs->nloaded);
    free(cs);
    c->cs = NULL;
    c->csf = NULL;
}

/* ---- validity ----------------------------------------------------------------- */

uint64_t checker_file_digest(void *checker, int ns, const char *name,
                             uint64_t key, int mode)
{
    Checker *c = checker;
    CSum *cs = c->cs;
    SumEntry e;
    const Ident *id;
    if (!cs)
        return 0;
    if (ns == SUM_STATE)
        return pack_digest(c);
    if (ns == SUM_LAYOUT) {
        uint32_t v = map_get(cs, key);
        if (!v)
            return 0;
        return nz(v & ENUM_BIT ? enum_layout(cs, v & ~ENUM_BIT)
                               : rec_layout(cs, v));
    }
    id = intern_find(c->in, name, strlen(name));
    if (!id || !entity_now(cs, ns, id->id, &e))
        return 0;
    return nz(mode == RD_FULL ? e.full : e.iface);
}

bool summary_valid(const UnitSummary *s, SumLookup lookup, void *ctx,
                   const SumRead **bad)
{
    size_t k;
    for (k = 0; k < s->nreads; k++) {
        const SumRead *r = &s->reads[k];
        if (r->digest != lookup(ctx, r->ns, r->name, r->key, r->mode)) {
            if (bad)
                *bad = r;
            return false;
        }
    }
    return true;
}

/* ---- text form ------------------------------------------------------------------ */

static const char *const kind_name[] = { "obj", "func", "typedef",
                                         "enumconst" };
static const char *const tag_name[] = { "struct", "union", "enum" };
static const char *const link_name[] = { "none", "internal", "external" };
static const char *const sc_name[] = { "none", "typedef", "extern", "static",
                                       "auto", "register" };
static const char *const flag_name[] = {
    "defined", "tentative", "inline", "thread", "noreturn", "weak",
    "implicit", "error", "proto_def", "kr_def", "const_init", "decl_external",
    "complete", "register"
};

static void put_flags(FILE *out, uint32_t f)
{
    size_t k;
    bool first = true;
    for (k = 0; k < sizeof flag_name / sizeof *flag_name; k++)
        if (f >> k & 1) {
            fprintf(out, "%s%s", first ? "" : ",", flag_name[k]);
            first = false;
        }
    if (first)
        fputc('-', out);
}

void summary_write(FILE *out, const UnitSummary *s, size_t index)
{
    size_t k;
    fprintf(out, "unit %zu key=%016llx digest=%016llx sig=%016llx%s\n", index,
            (unsigned long long)s->key, (unsigned long long)s->digest,
            (unsigned long long)s->sig, s->errors ? " errors" : "");
    for (k = 0; k < s->nentries; k++) {
        const SumEntry *e = &s->entries[k];
        fprintf(out, "  %s ", ns_name[e->ns]);
        if (e->ns == SUM_TAG)
            fprintf(out, "%s %s flags=", tag_name[e->kind], e->name);
        else if (e->ns == SUM_STATE)
            fprintf(out, "%s flags=", e->name);
        else
            fprintf(out, "%s %s%s%s%s linkage=%s storage=%s flags=",
                    kind_name[e->kind], e->name,
                    e->type_text ? " type=\"" : "",
                    e->type_text ? e->type_text : "",
                    e->type_text ? "\"" : "", link_name[e->linkage],
                    sc_name[e->sc]);
        put_flags(out, e->flags);
        if (e->ns == SUM_ORD || e->ns == SUM_EXT) {
            if (e->kind == CS_ENUMCONST)
                fprintf(out, " value=%lld", (long long)e->value);
            fprintf(out, " type_dig=%016llx", (unsigned long long)e->type_dig);
        }
        if (e->ns == SUM_TAG)
            fprintf(out, " layout=%016llx", (unsigned long long)e->layout_dig);
        fprintf(out, " digest=%016llx\n", (unsigned long long)e->digest);
    }
    for (k = 0; k < s->nreads; k++) {
        const SumRead *r = &s->reads[k];
        fprintf(out, "  read %s %s %016llx %016llx %s\n", ns_name[r->ns],
                mode_name[r->mode], (unsigned long long)r->key,
                (unsigned long long)r->digest, r->name);
    }
}

static int find_name(const char *const *tab, size_t n, const char *s)
{
    size_t k;
    for (k = 0; k < n; k++)
        if (!strcmp(tab[k], s))
            return (int)k;
    return -1;
}

long summary_load(FILE *in, UnitSummary **out)
{
    char line[4096];
    UnitSummary *v = NULL;
    size_t n = 0, cap = 0;
    *out = NULL;
    while (fgets(line, sizeof line, in)) {
        if (!strncmp(line, "unit ", 5)) {
            unsigned long long key, dig, sig;
            size_t idx;
            UnitSummary u;
            if (sscanf(line, "unit %zu key=%llx digest=%llx sig=%llx", &idx,
                       &key, &dig, &sig) != 4)
                goto bad;
            memset(&u, 0, sizeof u);
            u.key = key;
            u.digest = dig;
            u.sig = sig;
            u.owned = true;
            u.errors = strstr(line, " errors") != NULL;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                v = xrealloc(v, cap * sizeof *v);
            }
            v[n++] = u;
        } else if (!strncmp(line, "  read ", 7)) {
            char ns[16], mode[16], name[2048];
            unsigned long long key, dig;
            UnitSummary *u;
            SumRead r;
            int a, b;
            if (!n || sscanf(line, "  read %15s %15s %llx %llx %2047s", ns,
                             mode, &key, &dig, name) != 5)
                goto bad;
            a = find_name(ns_name, 5, ns);
            b = find_name(mode_name, 3, mode);
            if (a < 0 || b < 0)
                goto bad;
            u = &v[n - 1];
            r.name = xstrdup(name);
            r.ns = (uint8_t)a;
            r.mode = (uint8_t)b;
            r.key = key;
            r.digest = dig;
            u->reads = xrealloc(u->reads, (u->nreads + 1) * sizeof *u->reads);
            u->reads[u->nreads++] = r;
        }
    }
    *out = v;
    return (long)n;
bad:
    summary_free(v, n);
    return -1;
}

void summary_free(UnitSummary *s, size_t n)
{
    size_t i, k;
    for (i = 0; i < n; i++) {
        if (!s[i].owned)
            continue;
        for (k = 0; k < s[i].nreads; k++)
            free((char *)s[i].reads[k].name);
        free(s[i].reads);
    }
    free(s);
}
