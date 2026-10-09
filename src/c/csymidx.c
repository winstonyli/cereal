/* csymidx.c - the C symbol index: the builder the checker's hooks feed, and
 * the frozen CIndex (docs/B2_DESIGN.md). */
#include "c/csymidx.h"
#include "c/check_int.h"
#include "hash.h"
#include "pp.h"

#include <string.h>

/* ---- the builder ---------------------------------------------------------- */

/* Build-only role: a tentative definition, decided at the end. */
#define ROLE_TENT 3
/* Build-only flag: from a macro, spelled in source (body or argument). */
#define FROM_MACRO CIX_ARG

typedef struct BEv {
    SrcLoc loc;
    uint32_t decl;           /* id - 1 */
    uint8_t len, flags;
} BEv;

typedef struct BDecl {
    uint32_t name;           /* ident */
    uint8_t kind, linkage;
    uint16_t flags;
} BDecl;

typedef VEC(uint32_t) U32V;

struct SymIdxB {
    VEC(BEv) ev;
    VEC(BDecl) decls;
    /* key -> decl id (0: not seen): ordinary symbols (persistent and the
     * unit's), records and enums (type table index), fields, label slots */
    U32V gmap, lmap, recmap, enmap, fmap, labmap;
    size_t unit_ev0;         /* the unit's first event */
    const SrcFile *cf;       /* the last file asked about */
    bool csys;               /* ... in a system header, if it has no markers */
    FILE *vout;              /* --verify-symbols */
    VEC(uint8_t) seen;       /* verify: unit tokens that got an event */
    uint32_t unindexed, excused;
};

SymIdxB *csx_new(FILE *verify)
{
    SymIdxB *b = xcalloc(1, sizeof *b);
    b->vout = verify;
    return b;
}

void csx_free(SymIdxB *b)
{
    if (!b)
        return;
    vec_free(&b->ev);
    vec_free(&b->decls);
    vec_free(&b->gmap);
    vec_free(&b->lmap);
    vec_free(&b->recmap);
    vec_free(&b->enmap);
    vec_free(&b->fmap);
    vec_free(&b->labmap);
    vec_free(&b->seen);
    free(b);
}

static uint32_t *slot(U32V *m, uint32_t k)
{
    if (k >= m->len) {
        size_t n = m->cap ? m->cap : 64;
        while (n <= k)
            n *= 2;
        if (n > m->cap) {
            m->data = xrealloc(m->data, n * sizeof *m->data);
            m->cap = n;
        }
        memset(m->data + m->len, 0, (k + 1 - m->len) * sizeof *m->data);
        m->len = k + 1;
    }
    return &m->data[k];
}

static uint32_t new_decl(SymIdxB *b, uint32_t name, int kind, int linkage,
                         unsigned flags)
{
    BDecl d;
    d.name = name;
    d.kind = (uint8_t)kind;
    d.linkage = (uint8_t)linkage;
    d.flags = (uint16_t)flags;
    vec_push(&b->decls, d);
    return (uint32_t)b->decls.len;
}

static bool is_sys(SymIdxB *b, SrcMgr *sm, SrcLoc loc)
{
    const SrcFile *f = b->cf;
    if (!f || loc < f->base || loc >= f->base + f->span) {
        f = b->cf = srcmgr_file_of(sm, loc);
        if (!f)
            return false;
        b->csys = f->system_header;
    }
    return f->nsysmarks ? srcmgr_is_system((SrcFile *)f, loc) : b->csys;
}

/* Where an event at token tok goes (design section 5.3); false: nowhere
 * (presented in a system header). */
static bool where(Checker *c, uint32_t tok, BEv *e)
{
    const PTok *p;
    SrcLoc pres;
    if (tok >= c->u->ntoks)
        return false;
    p = &c->u->toks[tok];
    pres = p->exp ? p->exp : p->t.loc;
    if (is_sys(c->sx, c->sm, pres))
        return false;
    e->flags = 0;
    e->len = (uint8_t)(p->t.len > 255 ? 255 : p->t.len);
    if (!p->mloc) {
        e->loc = p->t.loc;
    } else if (p->t.flags & (TF_PASTED | TF_SYNTH)) {
        e->loc = pres;      /* no spelling: the length is the name's there */
        e->flags = CIX_AT_EXPANSION;
        e->len = 0;
    } else {
        e->loc = p->t.loc;
        e->flags = FROM_MACRO;   /* body or argument: decided at the end */
    }
    return true;
}

static void push_ev(Checker *c, uint32_t tok, BEv *e, uint32_t id, int role)
{
    SymIdxB *b = c->sx;
    e->decl = id - 1;
    e->flags |= (uint8_t)role;
    vec_push(&b->ev, *e);
    if (b->vout && tok < b->seen.len)
        b->seen.data[tok] = 1;
}

/* A system declaration first seen in use: one event where it is declared. */
static void lazy_event(Checker *c, uint32_t id, SrcLoc loc, int role)
{
    SymIdxB *b = c->sx;
    BEv e;
    if (!loc || !is_sys(b, c->sm, loc)) {
        if (!loc)
            b->decls.data[id - 1].flags |= CIDF_BUILTIN;
        return;
    }
    b->decls.data[id - 1].flags |= CIDF_SYSTEM;
    e.loc = loc;
    e.decl = id - 1;
    e.len = 0;               /* measured at the end */
    e.flags = (uint8_t)(role | CIX_AT_EXPANSION);
    vec_push(&b->ev, e);
}

static uint32_t sym_id(Checker *c, uint32_t ref, bool lazy)
{
    SymIdxB *b = c->sx;
    uint32_t *p = slot(ref & SYM_LOCAL ? &b->lmap : &b->gmap, ref & ~SYM_LOCAL);
    const CSym *s;
    int kind;
    if (*p)
        return *p;
    s = csym(c, ref);
    if (lazy && (s->flags & CSF_ERROR))
        return 0;            /* an undeclared name's placeholder */
    kind = s->kind == CS_FUNC ? CIK_FUNC
         : s->kind == CS_TYPEDEF ? CIK_TYPEDEF
         : s->kind == CS_ENUMCONST ? CIK_ENUMCONST
         : (s->flags & CSF_PARAM) ? CIK_PARAM : CIK_OBJ;
    *p = new_decl(b, s->name, kind, s->linkage,
                  (s->flags & CSF_IMPLICIT) ? CIDF_IMPLICIT : 0);
    if (lazy)
        lazy_event(c, *p, s->loc,
                   kind == CIK_TYPEDEF || kind == CIK_ENUMCONST ||
                   (s->flags & CSF_DEFINED) ? CIX_DEF : CIX_DECL);
    return *p;
}

void csx_decl(Checker *c, uint32_t ref, const CSym *x, bool file, uint32_t tok)
{
    BEv e;
    int role;
    if (!where(c, tok, &e))
        return;
    if (x->flags & CSF_PARAM)
        role = CIX_DECL;     /* csx_param_def: a definition's */
    else if (x->kind == CS_TYPEDEF || x->kind == CS_ENUMCONST ||
             (x->flags & CSF_DEFINED))
        role = CIX_DEF;
    else if (x->kind == CS_OBJ && (x->flags & CSF_TENTATIVE))
        role = ROLE_TENT;
    else if (x->kind == CS_OBJ && !file && !(x->flags & CSF_DECL_EXTERNAL))
        role = CIX_DEF;      /* a block-scope object */
    else
        role = CIX_DECL;
    push_ev(c, tok, &e, sym_id(c, ref, false), role);
}

void csx_sym(Checker *c, uint32_t ref, uint32_t tok, int role)
{
    BEv e;
    uint32_t id;
    if (!where(c, tok, &e) || !(id = sym_id(c, ref, true)))
        return;
    push_ev(c, tok, &e, id, role);
}

void csx_param_def(Checker *c, uint32_t ref)
{
    SymIdxB *b = c->sx;
    uint32_t *p = slot(ref & SYM_LOCAL ? &b->lmap : &b->gmap, ref & ~SYM_LOCAL);
    size_t k;
    if (!*p)
        return;
    for (k = b->ev.len; k-- > b->unit_ev0;)
        if (b->ev.data[k].decl == *p - 1 &&
            (b->ev.data[k].flags & CIX_ROLE) == CIX_DECL) {
            b->ev.data[k].flags |= CIX_DEF;
            return;
        }
}

/* The decl id of a tag; lazy: create it for a use (with an event where a
 * system header declares it). */
static uint32_t tag_id(Checker *c, uint32_t t, bool lazy)
{
    SymIdxB *b = c->sx;
    const TypeEnt *te = type_ent(&c->tt, type_ent(&c->tt, t)->canon);
    uint32_t *p, name;
    SrcLoc loc;
    int kind, role;
    if (te->kind == TY_ENUM) {
        const Enum *en = &c->tt.enums.data[te->extra];
        p = slot(&b->enmap, te->extra);
        kind = CIK_ENUM;
        name = en->tag;
        loc = en->loc;
        role = en->complete ? CIX_DEF : CIX_DECL;
    } else if (te->kind == TY_STRUCT || te->kind == TY_UNION) {
        const Record *r = &c->tt.recs.data[te->extra];
        p = slot(&b->recmap, te->extra);
        kind = te->kind == TY_UNION ? CIK_UNION : CIK_STRUCT;
        name = r->tag;
        loc = r->loc;
        role = (r->flags & RF_COMPLETE) ? CIX_DEF : CIX_DECL;
    } else {
        return 0;
    }
    if (!*p) {
        *p = new_decl(b, name, kind, 0, 0);
        if (lazy)
            lazy_event(c, *p, loc, role);
    }
    return *p;
}

void csx_tag(Checker *c, uint32_t t, uint32_t tok, int role)
{
    BEv e;
    uint32_t id;
    if (!where(c, tok, &e) || !(id = tag_id(c, t, role == CIX_REF)))
        return;
    push_ev(c, tok, &e, id, role);
}

void csx_tag_redecl(Checker *c, uint32_t t, uint32_t tok)
{
    SymIdxB *b = c->sx;
    BEv e;
    uint32_t id;
    size_t k;
    if (!where(c, tok, &e) || !(id = tag_id(c, t, false)))
        return;
    for (k = b->ev.len; k-- > b->unit_ev0;)
        if (b->ev.data[k].loc == e.loc) {
            b->ev.data[k].decl = id - 1;
            b->ev.data[k].flags = (uint8_t)((b->ev.data[k].flags & ~CIX_ROLE) |
                                            CIX_DECL);
            return;
        }
    push_ev(c, tok, &e, id, CIX_DECL);
}

void csx_field(Checker *c, uint32_t field, uint32_t tok, int role)
{
    SymIdxB *b = c->sx;
    uint32_t *p;
    BEv e;
    if (!where(c, tok, &e))
        return;
    p = slot(&b->fmap, field);
    if (!*p) {
        const Field *f = &c->tt.fields.data[field];
        *p = new_decl(b, f->name, CIK_FIELD, 0, 0);
        if (role == CIX_REF)
            lazy_event(c, *p, f->loc, CIX_DEF);
    }
    push_ev(c, tok, &e, *p, role);
}

void csx_skip(Checker *c, uint32_t tok)
{
    SymIdxB *b = c->sx;
    if (b->vout && tok < b->seen.len)
        b->seen.data[tok] = 1;
}

void csx_label_new(Checker *c, uint32_t slot_, uint32_t name, SrcLoc loc)
{
    (void)loc;
    (void)name;
    *slot(&c->sx->labmap, slot_) = 0;   /* the decl is made at its first event */
}

void csx_label(Checker *c, uint32_t slot_, uint32_t tok, int role)
{
    BEv e;
    uint32_t *id;
    if (!where(c, tok, &e))
        return;              /* a system header's labels have no decl */
    id = slot(&c->sx->labmap, slot_);
    if (!*id)
        *id = new_decl(c->sx, c->u->toks[tok].t.aux, CIK_LABEL, 0, 0);
    push_ev(c, tok, &e, *id, role);
}

/* ---- the unit ------------------------------------------------------------------ */

void csx_unit_begin(Checker *c)
{
    SymIdxB *b = c->sx;
    b->lmap.len = 0;
    b->unit_ev0 = b->ev.len;
    if (b->vout) {
        b->seen.len = 0;
        while (b->seen.len < c->u->ntoks)
            vec_push(&b->seen, 0);
    }
}

/* --verify-symbols: a name the parser placed in a naming node that got no
 * event, unless there is a reason. */
static bool names_entity(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    switch (c->nodes[i].tag) {
    case N_NAME:
        /* asm operand names are not C entities */
        return p == NO_NODE || (c->nodes[p].tag != N_ASM_OPERAND &&
                                c->nodes[p].tag != N_ASM_SECTION);
    case N_TYPEDEF_NAME: case N_TAG: case N_ENUMERATOR: case N_KR_IDENT:
    case N_DESIG_FIELD: case N_LABEL: case N_GOTO: case N_ADDR_LABEL:
    case N_IDENT: case N_MEMBER_EXPR:
        return true;
    default:
        return false;
    }
}

/* Whether a warning or error was reported on loc's line: the name is in
 * code gcc rejects or questions (a dropped member, a designator into an
 * incomplete type, a label outside a function), which the index may skip. */
static bool diagnosed_line(Checker *c, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    uint32_t line, col, l2, c2;
    size_t k;
    if (!f)
        return false;
    srcmgr_linecol(f, loc, &line, &col);
    for (k = 0; k < c->diag->all.len; k++) {
        const Diagnostic *d = c->diag->all.data[k];
        if (d->level < DL_WARNING || d->loc < f->base ||
            d->loc >= f->base + f->span)
            continue;
        srcmgr_linecol(f, d->loc, &l2, &c2);
        if (l2 == line)
            return true;
    }
    return false;
}

static bool excused(Checker *c, uint32_t i, const char *name, SrcLoc loc)
{
    uint32_t a;
    if (!strncmp(name, "__builtin_", 10) || !strcmp(name, "__func__") ||
        !strcmp(name, "__FUNCTION__") || !strcmp(name, "__PRETTY_FUNCTION__"))
        return true;
    if ((c->nodes[i].tag == N_IDENT || c->nodes[i].tag == N_MEMBER_EXPR) &&
        c->ty[i] == TYPE_B(ERROR))
        return true;         /* undeclared, no such member: diagnosed */
    for (a = i; a != NO_NODE; a = c->par[a])
        if ((c->nodes[a].flags & NF_ERROR) || c->nodes[a].tag == N_ERROR ||
            c->nodes[a].tag == N_ATTRIBUTE)
            return true;
    return diagnosed_line(c, loc);
}

void csx_unit_end(Checker *c)
{
    SymIdxB *b = c->sx;
    uint32_t i;
    if (!b->vout || c->quiet)
        return;
    for (i = 0; i < c->nn; i++) {
        uint32_t tok = c->nodes[i].tok;
        const Tok *t;
        BEv e;
        const char *name;
        if (!names_entity(c, i) || tok >= c->u->ntoks || b->seen.data[tok])
            continue;
        t = &c->u->toks[tok].t;
        if (t->kind != TK_IDENT || !where(c, tok, &e))
            continue;
        name = ident_by_id(c->in, t->aux)->str;
        if (excused(c, i, name, e.loc)) {
            b->seen.data[tok] = 1;
            b->excused++;
            continue;
        }
        b->seen.data[tok] = 1;   /* reported once */
        b->unindexed++;
        {
            SrcFile *f = srcmgr_file_of(c->sm, e.loc);
            uint32_t line = 0, col = 0;
            if (f)
                srcmgr_linecol(f, e.loc, &line, &col);
            fprintf(b->vout, "%s:%u:%u: unindexed '%s' (%s)\n",
                    f ? f->name : "?", line, col, name,
                    node_names[c->nodes[i].tag]);
        }
    }
}

/* ---- freezing -------------------------------------------------------------- */

static int ev_cmp(const void *pa, const void *pb)
{
    const BEv *a = pa, *b = pb;
    if (a->loc != b->loc)
        return a->loc < b->loc ? -1 : 1;
    if (a->decl != b->decl)
        return a->decl < b->decl ? -1 : 1;
    return (int)(a->flags & CIX_ROLE) - (int)(b->flags & CIX_ROLE);
}

typedef struct Range { SrcLoc b, e; } Range;

static int range_cmp(const void *pa, const void *pb)
{
    const Range *a = pa, *b = pb;
    return a->b < b->b ? -1 : a->b > b->b;
}

/* #define directive ranges of the check's preprocessor, sorted. */
static Range *define_ranges(Checker *c, size_t *n)
{
    PP *pp = c->opt.macro_ctx;
    Range *r;
    size_t i, k = 0;
    *n = 0;
    if (!pp || !pp->macros.len)
        return NULL;
    r = xmalloc(pp->macros.len * sizeof *r);
    for (i = 0; i < pp->macros.len; i++) {
        const Macro *m = pp->macros.data[i];
        if (m->predefined || !m->name_loc || m->end_loc < m->name_loc)
            continue;
        r[k].b = m->name_loc;
        r[k].e = m->end_loc;
        k++;
    }
    qsort(r, k, sizeof *r, range_cmp);
    *n = k;
    return r;
}

static bool in_ranges(const Range *r, size_t n, SrcLoc loc)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {               /* the first range starting after loc */
        size_t mid = (lo + hi) / 2;
        if (r[mid].b <= loc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo > 0 && loc <= r[lo - 1].e;
}

static bool ident_char(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '$' ||
           (unsigned char)ch >= 0x80;
}

typedef struct Pool {
    StrBuf sb;
    uint32_t *by_ident;      /* ident -> offset + 1 */
} Pool;

static uint32_t pool_add(Pool *p, const char *s)
{
    uint32_t off = (uint32_t)p->sb.len;
    sb_putn(&p->sb, s, strlen(s) + 1);
    return off;
}

CIndex *csx_finish(Checker *c)
{
    SymIdxB *b = c->sx;
    CIndex *ix;
    uint32_t *has_def, i, nd, ne = 0, nf = 0;
    uint32_t *path_off = NULL;
    size_t k, nr;
    Range *rg;
    Pool pool;
    const SrcFile *cur = NULL;
    if (!b)
        return NULL;
    nd = (uint32_t)b->decls.len;
    /* tentative definitions: the first defines, unless something else does */
    has_def = xcalloc(nd + 1, sizeof *has_def);
    for (k = 0; k < b->ev.len; k++)
        if ((b->ev.data[k].flags & CIX_ROLE) == CIX_DEF)
            has_def[b->ev.data[k].decl] = 1;
    for (k = 0; k < b->ev.len; k++) {
        BEv *e = &b->ev.data[k];
        if ((e->flags & CIX_ROLE) != ROLE_TENT)
            continue;
        e->flags &= (uint8_t)~CIX_ROLE;
        if (!has_def[e->decl]) {
            has_def[e->decl] = 1;
            e->flags |= CIX_DEF;
            b->decls.data[e->decl].flags |= CIDF_TENTATIVE;
        }
    }
    free(has_def);
    /* macro tokens: in a #define body, else a macro argument */
    rg = define_ranges(c, &nr);
    for (k = 0; k < b->ev.len; k++) {
        BEv *e = &b->ev.data[k];
        if ((e->flags & FROM_MACRO) && in_ranges(rg, nr, e->loc))
            e->flags = (uint8_t)((e->flags & ~FROM_MACRO) | CIX_MACRO_BODY);
    }
    free(rg);
    qsort(b->ev.data, b->ev.len, sizeof *b->ev.data, ev_cmp);

    ix = xcalloc(1, sizeof *ix);
    ix->unindexed = b->unindexed;
    ix->excused = b->excused;
    memset(&pool, 0, sizeof pool);
    pool.by_ident = xcalloc(interner_count(c->in) + 1, sizeof *pool.by_ident);
    sb_putc(&pool.sb, 0);
    ix->ev = xmalloc((b->ev.len + 1) * sizeof *ix->ev);
    ix->files = NULL;
    for (k = 0; k < b->ev.len; k++) {
        const BEv *e = &b->ev.data[k];
        CIdxEvent *o;
        if (k && !ev_cmp(e, e - 1))
            continue;        /* the same event again */
        if (!cur || e->loc < cur->base || e->loc >= cur->base + cur->span) {
            cur = srcmgr_file_of(c->sm, e->loc);
            if (cur && (cur->kind == SF_USER || cur->kind == SF_SYSTEM)) {
                ix->files = xrealloc(ix->files, (nf + 1) * sizeof *ix->files);
                path_off = xrealloc(path_off, (nf + 1) * sizeof *path_off);
                path_off[nf] = pool_add(&pool, cur->path);
                ix->files[nf].size = cur->size;
                ix->files[nf].hash = cindex_hash(cur->buf, cur->size);
                ix->files[nf].stale = false;
                nf++;
            }
        }
        if (!cur || (cur->kind != SF_USER && cur->kind != SF_SYSTEM) ||
            e->loc - cur->base > cur->size || nf > UINT16_MAX)
            continue;
        o = &ix->ev[ne++];
        o->off = e->loc - cur->base;
        o->decl = e->decl;
        o->file = (uint16_t)(nf - 1);
        o->flags = e->flags;
        o->len = e->len;
        if (!o->len) {       /* measure the name in the text */
            uint32_t n = 0;
            while (o->off + n < cur->size && n < 255 &&
                   ident_char(cur->buf[o->off + n]))
                n++;
            o->len = (uint8_t)n;
        }
        if (is_sys(b, c->sm, e->loc))
            o->flags |= CIX_SYSTEM;
    }
    ix->nev = ne;
    ix->nfiles = nf;
    /* decls and their names */
    ix->ndecls = nd;
    ix->decls = xcalloc(nd + 1, sizeof *ix->decls);
    for (i = 0; i < nd; i++) {
        const BDecl *d = &b->decls.data[i];
        CIdxDecl *o = &ix->decls[i];
        if (d->name && d->name < interner_count(c->in)) {
            if (!pool.by_ident[d->name])
                pool.by_ident[d->name] =
                    pool_add(&pool, ident_by_id(c->in, d->name)->str) + 1;
            o->name = pool.by_ident[d->name] - 1;
        }
        o->kind = d->kind;
        o->linkage = d->linkage;
        o->flags = d->flags;
    }
    free(pool.by_ident);
    ix->nstrings = pool.sb.len;
    ix->strings = pool.sb.data ? xrealloc(pool.sb.data, pool.sb.len) : NULL;
    for (i = 0; i < nf; i++)
        ix->files[i].path = ix->strings + path_off[i];
    free(path_off);
    /* events by decl */
    ix->by_decl_start = xcalloc(nd + 1, sizeof *ix->by_decl_start);
    ix->by_decl = xmalloc((ne + 1) * sizeof *ix->by_decl);
    for (i = 0; i < ne; i++)
        ix->by_decl_start[ix->ev[i].decl + 1]++;
    for (i = 0; i < nd; i++)
        ix->by_decl_start[i + 1] += ix->by_decl_start[i];
    {
        uint32_t *fill = xmalloc((nd + 1) * sizeof *fill);
        memcpy(fill, ix->by_decl_start, (nd + 1) * sizeof *fill);
        for (i = 0; i < ne; i++)
            ix->by_decl[fill[ix->ev[i].decl]++] = i;
        free(fill);
    }
    ix->ev = xrealloc(ix->ev, (ne + 1) * sizeof *ix->ev);
    b->ev.len = b->decls.len = 0;
    return ix;
}

/* ---- the frozen index ---------------------------------------------------------- */

void cindex_free(CIndex *ix)
{
    if (!ix)
        return;
    free(ix->files);
    free(ix->ev);
    free(ix->by_decl);
    free(ix->by_decl_start);
    free(ix->decls);
    free(ix->strings);
    free(ix);
}

uint64_t cindex_hash(const char *text, size_t n)
{
    return hash64(text, n, 0x63696478u);
}

size_t cindex_bytes(const CIndex *ix)
{
    if (!ix)
        return 0;
    return sizeof *ix + ix->nfiles * sizeof *ix->files +
           ix->nev * (sizeof *ix->ev + sizeof *ix->by_decl) +
           (ix->ndecls + 1) * (sizeof *ix->decls + sizeof *ix->by_decl_start) +
           ix->nstrings;
}

int cindex_file(const CIndex *ix, const char *path)
{
    uint32_t i;
    for (i = 0; ix && i < ix->nfiles; i++)
        if (!strcmp(ix->files[i].path, path))
            return (int)i;
    return -1;
}

size_t cindex_at(const CIndex *ix, uint32_t fi, uint32_t off, uint32_t *first)
{
    uint32_t lo = 0, hi = ix->nev, k, n = 0;
    while (lo < hi) {               /* the first event after (fi, off) */
        uint32_t mid = lo + (hi - lo) / 2;
        const CIdxEvent *e = &ix->ev[mid];
        if (e->file < fi || (e->file == fi && e->off <= off))
            lo = mid + 1;
        else
            hi = mid;
    }
    /* the events at the covering name's start, which is the last start at
     * or before off whose name reaches off */
    for (k = lo; k-- > 0;) {
        const CIdxEvent *e = &ix->ev[k];
        if (e->file != fi || off - e->off > 255)
            break;
        if (off <= e->off + e->len) {
            uint32_t at = e->off;
            *first = k;
            n = 1;
            while (*first > 0 && ix->ev[*first - 1].file == fi &&
                   ix->ev[*first - 1].off == at) {
                (*first)--;
                n++;
            }
            return n;
        }
    }
    return 0;
}

static const char *const kind_names[] = {
    "func", "obj", "param", "typedef", "enumconst", "field", "struct",
    "union", "enum", "label"};
static const char *const role_names[] = {"DECL", "DEF", "REF", "?"};

const char *cindex_role_name(unsigned flags)
{
    return role_names[flags & CIX_ROLE];
}

const char *cindex_kind_name(unsigned kind)
{
    return kind < sizeof kind_names / sizeof *kind_names ? kind_names[kind] : "?";
}

size_t cindex_decls_at(const CIndex *ix, const char *path, uint32_t off,
                       uint32_t *out, size_t max)
{
    int fi = ix ? cindex_file(ix, path) : -1;
    uint32_t first, k;
    size_t n, nd = 0, j;
    if (fi < 0 || ix->files[fi].stale)
        return 0;
    n = cindex_at(ix, (uint32_t)fi, off, &first);
    for (k = first; k < first + n && nd < max; k++) {
        for (j = 0; j < nd && out[j] != ix->ev[k].decl; j++)
            ;
        if (j == nd)
            out[nd++] = ix->ev[k].decl;
    }
    return nd;
}

static int u32_cmp(const void *pa, const void *pb)
{
    uint32_t a = *(const uint32_t *)pa, b = *(const uint32_t *)pb;
    return a < b ? -1 : a > b;
}

size_t cindex_select(const CIndex *ix, const uint32_t *decls, size_t nd,
                     CIdxQuery q, int fi, uint32_t **out)
{
    VEC(uint32_t) v = {0};
    size_t i, k, n = 0;
    for (i = 0; i < nd; i++) {
        uint32_t b = ix->by_decl_start[decls[i]], e = ix->by_decl_start[decls[i] + 1];
        uint32_t j;
        int role = -1;       /* CIQ_DEF, CIQ_DECL: the one role taken */
        if (q == CIQ_DEF || q == CIQ_DECL) {
            int want = q == CIQ_DEF ? CIX_DEF : CIX_DECL;
            role = want == CIX_DEF ? CIX_DECL : CIX_DEF;
            for (j = b; j < e; j++)
                if ((ix->ev[ix->by_decl[j]].flags & CIX_ROLE) == want) {
                    role = want;
                    break;
                }
        }
        for (j = b; j < e; j++) {
            const CIdxEvent *ev = &ix->ev[ix->by_decl[j]];
            int r = ev->flags & CIX_ROLE;
            if (ix->files[ev->file].stale || (fi >= 0 && ev->file != (uint32_t)fi))
                continue;
            if (role >= 0 ? r != role
                          : (ev->flags & CIX_MACRO_BODY) ||
                                (q == CIQ_USES && r != CIX_REF))
                continue;
            vec_push(&v, ix->by_decl[j]);
        }
    }
    /* event order is (file, offset) order: one per location */
    if (v.len > 1)
        qsort(v.data, v.len, sizeof *v.data, u32_cmp);
    for (k = 0; k < v.len; k++) {
        const CIdxEvent *e = &ix->ev[v.data[k]];
        if (n) {
            const CIdxEvent *p = &ix->ev[v.data[n - 1]];
            if (p->file == e->file && p->off == e->off) {
                if ((p->flags & CIX_ROLE) == CIX_REF)
                    v.data[n - 1] = v.data[k];
                continue;
            }
        }
        v.data[n++] = v.data[k];
    }
    *out = v.data;
    return n;
}

SrcFile *cindex_srcfile(SrcMgr *sm, const char *path)
{
    uint32_t i, n = srcmgr_nfiles(sm);
    for (i = 0; i < n; i++) {
        SrcFile *f = srcmgr_file(sm, i);
        if ((f->kind == SF_USER || f->kind == SF_SYSTEM) && !strcmp(f->path, path))
            return f;
    }
    return NULL;
}

void cindex_validate(CIndex *ix, SrcMgr *sm)
{
    uint32_t i;
    for (i = 0; ix && i < ix->nfiles; i++) {
        CIdxFile *cf = &ix->files[i];
        SrcFile *f = cindex_srcfile(sm, cf->path);
        cf->stale = !f || f->size != cf->size ||
                    cindex_hash(f->buf, f->size) != cf->hash;
    }
}

void cindex_print_flags(FILE *out, unsigned fl)
{
    if (fl & CIX_MACRO_BODY)
        fputs(" body", out);
    if (fl & CIX_AT_EXPANSION)
        fputs(" expansion", out);
    if (fl & CIX_ARG)
        fputs(" arg", out);
    if (fl & CIX_SYSTEM)
        fputs(" system", out);
}

void cindex_dump(const CIndex *ix, SrcMgr *sm, FILE *out)
{
    uint32_t i;
    SrcFile *f = NULL;
    int fi = -1;
    if (!ix)
        return;
    for (i = 0; i < ix->nev; i++) {
        const CIdxEvent *e = &ix->ev[i];
        uint32_t line = 0, col = 0;
        if (e->file != fi) {
            fi = e->file;
            f = cindex_srcfile(sm, ix->files[fi].path);
        }
        if (f)
            srcmgr_linecol(f, f->base + e->off, &line, &col);
        fprintf(out, "%s:%u:%u %s %s %s -> #%u", f ? f->name : "?", line, col,
                role_names[e->flags & CIX_ROLE],
                kind_names[ix->decls[e->decl].kind], cindex_name(ix, e->decl),
                e->decl);
        cindex_print_flags(out, e->flags);
        fputc('\n', out);
    }
    for (i = 0; i < ix->ndecls; i++) {
        const CIdxDecl *d = &ix->decls[i];
        fprintf(out, "#%u %s %s%s%s%s%s%s\n", i, kind_names[d->kind],
                cindex_name(ix, i),
                d->linkage == 1 ? " internal" : d->linkage == 2 ? " external" : "",
                d->flags & CIDF_BUILTIN ? " builtin" : "",
                d->flags & CIDF_IMPLICIT ? " implicit" : "",
                d->flags & CIDF_SYSTEM ? " system" : "",
                d->flags & CIDF_TENTATIVE ? " tentative" : "");
    }
}

size_t cindex_verify(const CIndex *ix, SrcMgr *sm, FILE *out)
{
    size_t bad = 0;
    uint32_t i, j;
    if (!ix)
        return 0;
    for (i = 1; i < ix->nev; i++) {
        const CIdxEvent *a = &ix->ev[i - 1], *b = &ix->ev[i];
        if (a->file > b->file || (a->file == b->file && a->off > b->off) ||
            (a->file == b->file && a->off == b->off && a->decl == b->decl &&
             (a->flags & CIX_ROLE) == (b->flags & CIX_ROLE))) {
            fprintf(out, "verify: events %u and %u out of order\n", i - 1, i);
            bad++;
        }
    }
    if (ix->by_decl_start[ix->ndecls] != ix->nev) {
        fprintf(out, "verify: CSR covers %u of %u events\n",
                ix->by_decl_start[ix->ndecls], ix->nev);
        bad++;
    }
    for (i = 0; i < ix->ndecls; i++) {
        bool declared = false;
        for (j = ix->by_decl_start[i]; j < ix->by_decl_start[i + 1]; j++) {
            const CIdxEvent *e = &ix->ev[ix->by_decl[j]];
            if (e->decl != i) {
                fprintf(out, "verify: CSR of #%u lists event of #%u\n", i, e->decl);
                bad++;
            }
            if ((e->flags & CIX_ROLE) != CIX_REF)
                declared = true;
        }
        if (!declared && !(ix->decls[i].flags & (CIDF_BUILTIN | CIDF_IMPLICIT |
                                                 CIDF_SYSTEM))) {
            fprintf(out, "verify: #%u %s '%s' has no declaration event\n", i,
                    kind_names[ix->decls[i].kind], cindex_name(ix, i));
            bad++;
        }
    }
    for (i = 0; i < ix->nfiles; i++) {
        SrcFile *f = cindex_srcfile(sm, ix->files[i].path);
        if (!f || f->size != ix->files[i].size ||
            cindex_hash(f->buf, f->size) != ix->files[i].hash) {
            fprintf(out, "verify: %s: hash mismatch\n", ix->files[i].path);
            bad++;
        }
    }
    fprintf(out, "symbols: %u events, %u decls, %u files, %zu bytes, %u "
            "unindexed, %u excused\n", ix->nev, ix->ndecls, ix->nfiles, cindex_bytes(ix),
            ix->unindexed, ix->excused);
    return bad + ix->unindexed;
}
