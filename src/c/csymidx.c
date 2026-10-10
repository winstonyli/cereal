/* csymidx.c - the C symbol index: the builder the checker's hooks feed, and
 * the frozen CIndex (docs/B2_DESIGN.md). */
#include "c/csymidx.h"
#include "c/cdecl_int.h"
#include "hash.h"
#include "pp.h"
#include "thread.h"

#include <stdio.h>
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
    uint32_t fn;             /* a block-scope symbol's function (ident), 0 */
    uint32_t hover;          /* offset in SymIdxB.hs, 0: none yet */
    uint32_t scope;          /* 1 + index in SymIdxB.scopes, 0: none */
    uint32_t ty;             /* 1 + its TypeId (an enumerator: its enum's), 0 */
    uint32_t type, parent;   /* CIdxDecl's, set at the end */
    uint8_t kind, linkage;
    uint16_t flags;
} BDecl;

typedef struct BScope {      /* presentation points */
    SrcLoc begin, end;
    uint32_t parent;         /* 1 + index, 0: a root */
} BScope;

typedef VEC(uint32_t) U32V;

struct SymIdxB {
    VEC(BEv) ev;
    VEC(BDecl) decls;
    /* key -> decl id (0: not seen): ordinary symbols (persistent and the
     * unit's), records and enums (type table index), fields, label slots,
     * typedefs (index of their TY_TYPEDEF entry) */
    U32V gmap, lmap, recmap, enmap, fmap, labmap, tdmap;
    U32V gec, lec;           /* enumerator symbol -> 1 + its enum's TypeId */
    U32V enhover;            /* enum index -> hover offset (csx_enum) */
    VEC(BScope) scopes;      /* every unit's root and the checker's scopes */
    U32V open;               /* the open scopes (ids), the unit's root first */
    StrBuf hs;               /* hover texts, deduplicated: the final string
                                pool's start (offset 0 is "") */
    uint32_t *hset, hcap, hcount;   /* hash set of hs offsets + 1 */
    StrBuf tmp;
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
    vec_free(&b->tdmap);
    vec_free(&b->gec);
    vec_free(&b->lec);
    vec_free(&b->enhover);
    vec_free(&b->scopes);
    vec_free(&b->open);
    vec_free(&b->seen);
    sb_free(&b->hs);
    sb_free(&b->tmp);
    free(b->hset);
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
    memset(&d, 0, sizeof d);
    d.name = name;
    d.kind = (uint8_t)kind;
    d.linkage = (uint8_t)linkage;
    d.flags = (uint16_t)flags;
    vec_push(&b->decls, d);
    return (uint32_t)b->decls.len;
}

/* ---- hover texts (copied out of the checker while its tables live) ------- */

static uint32_t hash_str(const char *s, size_t n)
{
    return (uint32_t)hash64(s, n, 0x686f76u);
}

/* The offset of text s (cut to CIX_HOVER_MAX bytes) in b->hs, added if new;
 * 0 for an empty text.  The two spaces type_print puts after a parameter
 * ending in a word (gcc's diagnostic text) read as one here. */
static uint32_t hover_add(SymIdxB *b, StrBuf *s)
{
    uint32_t i, off;
    size_t n = 0, j;
    for (j = 0; j < s->len; j++)
        if (!(n >= 2 && s->data[j] == ' ' && s->data[n - 1] == ' ' &&
              s->data[n - 2] == ','))
            s->data[n++] = s->data[j];
    s->len = n;
    while (n && s->data[n - 1] == '\n')
        n--;
    if (!n)
        return 0;
    if (n > CIX_HOVER_MAX) {     /* at a character boundary */
        n = CIX_HOVER_MAX;
        while (n && ((unsigned char)s->data[n] & 0xC0) == 0x80)
            n--;
        s->len = n;
        sb_puts(s, "...");
        n = s->len;
    }
    s->len = n;
    if (!b->hs.len)
        sb_putc(&b->hs, 0);
    if (2 * (b->hcount + 1) > b->hcap) {        /* grow and rehash */
        uint32_t cap = b->hcap ? 2 * b->hcap : 1024, k, *set;
        set = xcalloc(cap, sizeof *set);
        for (k = 0; k < b->hcap; k++)
            if (b->hset[k]) {
                const char *t = b->hs.data + b->hset[k] - 1;
                for (i = hash_str(t, strlen(t)) & (cap - 1); set[i];
                     i = (i + 1) & (cap - 1))
                    ;
                set[i] = b->hset[k];
            }
        free(b->hset);
        b->hset = set;
        b->hcap = cap;
    }
    for (i = hash_str(s->data, n) & (b->hcap - 1); b->hset[i];
         i = (i + 1) & (b->hcap - 1)) {
        off = b->hset[i] - 1;
        if (!strncmp(b->hs.data + off, s->data, n) && !b->hs.data[off + n])
            return off;
    }
    off = (uint32_t)b->hs.len;
    sb_putn(&b->hs, s->data, n);
    sb_putc(&b->hs, 0);
    b->hset[i] = off + 1;
    b->hcount++;
    return off;
}

/* Whether type t is const, arrays stripped (`const int a[3]`, through
 * typedefs). */
static bool is_readonly(Checker *c, TypeId t)
{
    int n;
    t = type_canon(&c->tt, t);
    for (n = 0; n < 64 && (type_kind(&c->tt, t) == TY_ARRAY ||
                           type_kind(&c->tt, t) == TY_VLA); n++)
        t = type_canon(&c->tt, type_ent(&c->tt, t)->base);
    return TYPE_QUALS(t) & TQ_CONST;
}

/* An ordinary symbol's text (its --dump-types line), type and flags, taken
 * while the symbol lives. */
static void sym_hover(Checker *c, uint32_t ref, uint32_t id)
{
    SymIdxB *b = c->sx;
    const CSym *s = csym(c, ref);
    BDecl *d = &b->decls.data[id - 1];
    b->tmp.len = 0;
    cdecl_decl_line(c, csym(c, ref), d->fn, &b->tmp);
    d->hover = hover_add(b, &b->tmp);
    if (s->linkage == 1 || s->sc == SC_STATIC)
        d->flags |= CIDF_STATIC;
    if (d->kind == CIK_ENUMCONST) {
        U32V *m = ref & SYM_LOCAL ? &b->lec : &b->gec;
        d->ty = (ref & ~SYM_LOCAL) < m->len ? m->data[ref & ~SYM_LOCAL] : 0;
        d->flags |= CIDF_READONLY;
        return;
    }
    d->ty = s->ty + 1;
    if ((d->kind == CIK_OBJ || d->kind == CIK_PARAM) && is_readonly(c, s->ty))
        d->flags |= CIDF_READONLY;
    if (d->kind == CIK_TYPEDEF && type_kind(&c->tt, s->ty) == TY_TYPEDEF)
        *slot(&b->tdmap, TYPE_IDX(s->ty)) = id;
}

/* The lines of b->tmp (each ending in '\n') after a heading and
 * CIX_HOVER_MEMBERS members give way to a count. */
static void cut_members(SymIdxB *b)
{
    size_t k, lines = 0, cut = 0, more = 0;
    for (k = 0; k < b->tmp.len; k++)
        if (b->tmp.data[k] == '\n' && ++lines == CIX_HOVER_MEMBERS + 1)
            cut = k + 1;
        else if (b->tmp.data[k] == '\n' && lines > CIX_HOVER_MEMBERS + 1)
            more++;
    if (more) {
        b->tmp.len = cut;
        sb_printf(&b->tmp, "  ... %zu more\n", more);
    }
}

/* A struct or union: its layout (type_dump_record), members bounded. */
static uint32_t record_hover(Checker *c, const Record *r)
{
    SymIdxB *b = c->sx;
    b->tmp.len = 0;
    type_dump_record(&c->tt, &b->tmp, r->ty);
    cut_members(b);
    return hover_add(b, &b->tmp);
}

/* A field: name and type, then its record and offset. */
static uint32_t field_hover(Checker *c, const Record *r, const Field *f)
{
    SymIdxB *b = c->sx;
    b->tmp.len = 0;
    sb_printf(&b->tmp, "field %s: ", ident_by_id(c->in, f->name)->str);
    type_print(&c->tt, &b->tmp, f->ty);
    sb_puts(&b->tmp, " (");
    type_print(&c->tt, &b->tmp, r->ty);
    sb_printf(&b->tmp, ", offset %llu", (unsigned long long)(f->off_bits / 8));
    if (f->flags & FF_BITFIELD)
        sb_printf(&b->tmp, " bit %u, width %u", (unsigned)(f->off_bits % 8),
                  f->width);
    sb_putc(&b->tmp, ')');
    return hover_add(b, &b->tmp);
}

void csx_enum(Checker *c, uint32_t t, const uint32_t *ecs, uint32_t n)
{
    SymIdxB *b = c->sx;
    const TypeEnt *te = type_ent(&c->tt, type_ent(&c->tt, t)->canon);
    const Enum *en;
    uint32_t k;
    if (te->kind != TY_ENUM)
        return;
    en = &c->tt.enums.data[te->extra];
    b->tmp.len = 0;
    type_print(&c->tt, &b->tmp, t);
    sb_puts(&b->tmp, " (underlying ");
    type_print(&c->tt, &b->tmp, en->underlying);
    sb_puts(&b->tmp, ")\n");
    for (k = 0; k < n; k++) {
        const CSym *s = csym(c, ecs[k]);
        *slot(ecs[k] & SYM_LOCAL ? &b->lec : &b->gec, ecs[k] & ~SYM_LOCAL) = t + 1;
        if (type_is_signed(&c->tt, s->vty))
            sb_printf(&b->tmp, "  %s = %" PRId64 "\n", cident(c, s->name),
                      (int64_t)s->val);
        else
            sb_printf(&b->tmp, "  %s = %" PRIu64 "\n", cident(c, s->name), s->val);
    }
    cut_members(b);
    *slot(&b->enhover, te->extra) = hover_add(b, &b->tmp);
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
    if ((ref & SYM_LOCAL) && kind != CIK_PARAM && !cat_file_scope(c) &&
        c->func_sym != SYM_NONE)
        b->decls.data[*p - 1].fn = csym(c, c->func_sym)->name;  /* "f:x" */
    if ((ref & SYM_LOCAL) && !s->linkage && b->open.len)
        b->decls.data[*p - 1].scope = vec_last(&b->open);
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
    if (b->decls.data[*p - 1].scope && b->open.len)
        b->decls.data[*p - 1].scope = vec_last(&b->open);   /* the body's */
    for (k = b->ev.len; k-- > b->unit_ev0;)
        if (b->ev.data[k].decl == *p - 1 &&
            (b->ev.data[k].flags & CIX_ROLE) == CIX_DECL) {
            b->ev.data[k].flags |= CIX_DEF;
            return;
        }
}

/* The decl id of a tag, created if new; lazy: for a use (with an event where
 * a system header declares it); 2: tag_link's rule. */
static uint32_t tag_id(Checker *c, uint32_t t, int lazy)
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
    if (!*p && lazy == 2 && name && !is_sys(b, c->sm, loc))
        return 0;            /* tag_link: a user tag with no events */
    if (!*p) {
        *p = new_decl(b, name, kind, 0, 0);
        if (lazy && name)
            lazy_event(c, *p, loc, role);
    }
    return *p;
}

/* A tag as the type or parent of another decl (B3_DESIGN.md 3.3): its decl;
 * created for an anonymous tag (nameless, no events) or a system one (with
 * its declaration event), else only looked up. */
static uint32_t tag_link(Checker *c, uint32_t t)
{
    return tag_id(c, t, 2);
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
    if (!*id) {
        SymIdxB *b = c->sx;
        uint32_t name = c->u->toks[tok].t.aux;
        *id = new_decl(b, name, CIK_LABEL, 0, 0);
        b->decls.data[*id - 1].scope = b->open.len ? b->open.data[0] : 0;
        b->tmp.len = 0;
        sb_puts(&b->tmp, "label ");
        if (c->func_sym != SYM_NONE)
            sb_printf(&b->tmp, "%s:", cident(c, csym(c, c->func_sym)->name));
        sb_puts(&b->tmp, cident(c, name));
        b->decls.data[*id - 1].hover = hover_add(b, &b->tmp);
    }
    push_ev(c, tok, &e, *id, role);
}

/* ---- the unit ------------------------------------------------------------------ */

/* A token's presentation point, and just past it. */
static SrcLoc tok_begin(const PTok *p)
{
    return p->exp ? p->exp : p->t.loc;
}

static SrcLoc tok_end(const PTok *p)
{
    return p->exp ? p->exp + 1 : p->t.loc + (p->t.len ? p->t.len : 1);
}

void csx_unit_begin(Checker *c)
{
    SymIdxB *b = c->sx;
    BScope root = {0, 0, 0};
    b->lmap.len = b->lec.len = 0;
    b->unit_ev0 = b->ev.len;
    if (c->u->ntoks) {       /* the external declaration: the scopes' root */
        root.begin = tok_begin(&c->u->toks[0]);
        root.end = tok_end(&c->u->toks[c->u->ntoks - 1]);
    }
    vec_push(&b->scopes, root);
    b->open.len = 0;
    vec_push(&b->open, (uint32_t)b->scopes.len);
    if (b->vout) {
        b->seen.len = 0;
        while (b->seen.len < c->u->ntoks)
            vec_push(&b->seen, 0);
    }
}

void csx_scope(Checker *c, uint32_t tok, bool open)
{
    SymIdxB *b = c->sx;
    if (tok >= c->u->ntoks || !b->open.len)
        return;
    if (open) {
        BScope s;
        s.begin = tok_begin(&c->u->toks[tok]);
        s.end = 0;
        s.parent = vec_last(&b->open);
        vec_push(&b->scopes, s);
        vec_push(&b->open, (uint32_t)b->scopes.len);
    } else if (b->open.len > 1) {
        b->scopes.data[vec_pop(&b->open) - 1].end = tok_end(&c->u->toks[tok]);
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
    /* the unit's symbols are reset when the next unit starts */
    for (i = 0; i < b->lmap.len; i++)
        if (b->lmap.data[i])
            sym_hover(c, i | SYM_LOCAL, b->lmap.data[i]);
    while (b->open.len > 1)  /* left open by error recovery: to the unit's end */
        b->scopes.data[vec_pop(&b->open) - 1].end =
            b->scopes.data[b->open.data[0] - 1].end;
    b->open.len = 0;
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

/* The decl of type t (B3_DESIGN.md 3.3): through pointers, arrays, function
 * types (to the return type), vectors and complex to the first typedef with
 * a decl, or a tag. */
static uint32_t type_decl(Checker *c, TypeId t)
{
    SymIdxB *b = c->sx;
    int n;
    for (n = 0; n < 64; n++) {
        const TypeEnt *te = type_ent(&c->tt, t);
        switch (te->kind) {
        case TY_TYPEDEF:
            if (TYPE_IDX(t) < b->tdmap.len && b->tdmap.data[TYPE_IDX(t)])
                return b->tdmap.data[TYPE_IDX(t)];
            /* fall through */
        case TY_PTR: case TY_ARRAY: case TY_VLA: case TY_FUNC:
        case TY_VECTOR: case TY_COMPLEX:
            t = te->base;
            break;
        case TY_STRUCT: case TY_UNION: case TY_ENUM:
            return tag_link(c, t);
        default:
            return 0;
        }
    }
    return 0;
}

/* Types, parents and the system declarations the unit never named, while
 * the checker's tables live (B3_DESIGN.md 3.3, 3.4). */
static void link_decls(Checker *c)
{
    SymIdxB *b = c->sx;
    uint32_t i, *outer;
    size_t k;
    for (i = 1; i < c->nidents; i++) {   /* system declarations up front */
        uint32_t bind = c->top[NS_ORD][i], ref;
        const CSym *s;
        if (!bind || ((ref = c->log.data[bind - 1].ref) & SYM_LOCAL) ||
            (ref < b->gmap.len && b->gmap.data[ref]))
            continue;
        s = csym(c, ref);
        if (!(s->flags & (CSF_ERROR | CSF_IMPLICIT)) && s->loc &&
            is_sys(b, c->sm, s->loc))
            sym_id(c, ref, true);
    }
    for (i = 0; i < b->gmap.len; i++)
        if (b->gmap.data[i])
            sym_hover(c, i, b->gmap.data[i]);
    /* fields: the outermost record through anonymous members is the parent */
    outer = xcalloc(c->tt.recs.len + 1, sizeof *outer);
    for (k = 0; k < c->tt.recs.len; k++) {
        const Record *r = &c->tt.recs.data[k];
        for (i = r->fields; i < r->fields + r->nfields; i++) {
            const Field *f = &c->tt.fields.data[i];
            const TypeEnt *te = type_ent(&c->tt, type_canon(&c->tt, f->ty));
            if (!f->name && (te->kind == TY_STRUCT || te->kind == TY_UNION))
                outer[te->extra] = (uint32_t)k + 1;
        }
    }
    for (k = 0; k < c->tt.recs.len; k++) {
        const Record *r = &c->tt.recs.data[k];
        size_t o = k;
        uint32_t par = 0;
        int n;
        for (n = 0; n < 64 && !c->tt.recs.data[o].tag && outer[o]; n++)
            o = outer[o] - 1;
        for (i = r->fields; i < r->fields + r->nfields && i < b->fmap.len; i++) {
            const Field *f = &c->tt.fields.data[i];
            BDecl *d;
            if (!b->fmap.data[i])
                continue;
            if (!par)
                par = tag_link(c, c->tt.recs.data[o].ty);
            d = &b->decls.data[b->fmap.data[i] - 1];
            d->parent = par;
            d->ty = f->ty + 1;
            if (is_readonly(c, f->ty))
                d->flags |= CIDF_READONLY;
        }
    }
    free(outer);
    for (i = 0; i < b->decls.len; i++) {  /* grows by tags tag_link makes */
        BDecl *d = &b->decls.data[i];
        TypeId t = d->ty - 1;
        uint32_t ty = 0;
        if (d->kind == CIK_STRUCT || d->kind == CIK_UNION || d->kind == CIK_ENUM) {
            d->type = i + 1;
            continue;
        }
        if (!d->ty || d->kind == CIK_LABEL)
            continue;
        if (d->kind == CIK_ENUMCONST) {
            ty = type_ckind(&c->tt, t) == TY_ENUM ? tag_link(c, t) : 0;
            b->decls.data[i].parent = ty;
        } else if (d->kind == CIK_TYPEDEF && type_kind(&c->tt, t) == TY_TYPEDEF) {
            ty = type_decl(c, type_ent(&c->tt, t)->base);   /* one step */
        } else {
            ty = type_decl(c, t);
        }
        b->decls.data[i].type = ty;
    }
}

typedef struct SortScope {
    CIdxScope s;
    uint32_t ord;            /* creation order */
} SortScope;

/* (file, begin, end descending): enclosing scopes first */
static int scope_cmp_ix(const CIdxScope *a, const CIdxScope *b)
{
    if (a->file != b->file)
        return a->file < b->file ? -1 : 1;
    if (a->begin != b->begin)
        return a->begin < b->begin ? -1 : 1;
    return a->end > b->end ? -1 : a->end < b->end;
}

static int scope_cmp(const void *pa, const void *pb)
{
    const SortScope *a = pa, *b = pb;
    int r = scope_cmp_ix(&a->s, &b->s);
    return r ? r : a->ord < b->ord ? -1 : a->ord > b->ord;
}

/* The builder's scopes into ix->scopes (B3_DESIGN.md 3.2): kept if in one
 * user file with events (ix->files, paths in strings + path_off) and in the
 * file of its nearest kept ancestor, overlapping it (then clamped to it); a
 * dropped root drops its blocks.  Returns
 * builder id - 1 -> 1 + final index, or the nearest kept ancestor's, or 0. */
static uint32_t *freeze_scopes(Checker *c, CIndex *ix, const char *strings,
                               const uint32_t *path_off)
{
    SymIdxB *b = c->sx;
    size_t n = b->scopes.len, k, nk = 0;
    uint32_t *map = xcalloc(n + 1, sizeof *map), *pos;
    uint8_t *dead = xcalloc(n + 1, 1);
    SortScope *v = xmalloc((n + 1) * sizeof *v);
    const SrcFile *f = NULL;
    uint32_t fi = 0;
    for (k = 0; k < n; k++) {
        const BScope *s = &b->scopes.data[k];
        uint32_t pk = s->parent ? map[s->parent - 1] : 0;
        SortScope *o = &v[nk];
        if (s->parent && dead[s->parent - 1]) {
            dead[k] = 1;
            continue;
        }
        map[k] = pk;
        if (!f || s->begin < f->base || s->begin >= f->base + f->span) {
            f = srcmgr_file_of(c->sm, s->begin);
            for (fi = 0; f && fi < ix->nfiles &&
                         strcmp(strings + path_off[fi], f->path); fi++)
                ;
        }
        /* a parent in another file, or in another inclusion of this one
         * (a file including itself): no overlap */
        if (!f || f->kind != SF_USER || fi >= ix->nfiles || s->end <= s->begin ||
            s->begin - f->base > f->size || s->end - 1 - f->base > f->size ||
            is_sys(b, c->sm, s->begin) ||
            (pk && (v[pk - 1].s.file != fi ||
                    s->begin - f->base >= v[pk - 1].s.end ||
                    s->end - f->base <= v[pk - 1].s.begin))) {
            dead[k] = !s->parent;
            continue;
        }
        o->s.file = fi;
        o->s.begin = s->begin - f->base;
        o->s.end = s->end - f->base;
        o->s.parent = pk;
        o->ord = (uint32_t)nk;
        if (pk) {            /* inside the parent, should recovery disagree */
            const CIdxScope *p = &v[pk - 1].s;
            o->s.begin = o->s.begin < p->begin ? p->begin : o->s.begin;
            o->s.end = o->s.end > p->end ? p->end : o->s.end;
            o->s.end = o->s.end < o->s.begin ? o->s.begin : o->s.end;
        }
        map[k] = (uint32_t)++nk;
    }
    free(dead);
    qsort(v, nk, sizeof *v, scope_cmp);
    pos = xmalloc((nk + 1) * sizeof *pos);   /* creation order -> 1 + final */
    for (k = 0; k < nk; k++)
        pos[v[k].ord] = (uint32_t)k + 1;
    ix->nscopes = (uint32_t)nk;
    ix->scopes = xmalloc((nk + 1) * sizeof *ix->scopes);
    for (k = 0; k < nk; k++) {
        ix->scopes[k] = v[k].s;
        if (v[k].s.parent)
            ix->scopes[k].parent = pos[v[k].s.parent - 1];
    }
    for (k = 0; k < n; k++)
        if (map[k])
            map[k] = pos[map[k] - 1];
    free(pos);
    free(v);
    return map;
}

static uint32_t serial_counter;

static uint32_t next_serial(void)
{
    return atomic_add_u32(&serial_counter, 1) + 1;
}

/* The events by decl (CSR: by_decl_start, by_decl) from ev, nev, ndecls. */
static void build_csr(CIndex *ix)
{
    uint32_t i, nd = ix->ndecls, ne = ix->nev;
    uint32_t *fill;
    ix->by_decl_start = xcalloc(nd + 1, sizeof *ix->by_decl_start);
    ix->by_decl = xmalloc((ne + 1) * sizeof *ix->by_decl);
    for (i = 0; i < ne; i++)
        ix->by_decl_start[ix->ev[i].decl + 1]++;
    for (i = 0; i < nd; i++)
        ix->by_decl_start[i + 1] += ix->by_decl_start[i];
    fill = xmalloc((nd + 1) * sizeof *fill);
    memcpy(fill, ix->by_decl_start, (nd + 1) * sizeof *fill);
    for (i = 0; i < ne; i++)
        ix->by_decl[fill[ix->ev[i].decl]++] = i;
    free(fill);
}

CIndex *csx_finish(Checker *c)
{
    SymIdxB *b = c->sx;
    CIndex *ix;
    uint32_t *has_def, i, nd, ne = 0, nf = 0;
    uint32_t *path_off = NULL, *smap;
    size_t k, nr;
    Range *rg;
    Pool pool;
    const SrcFile *cur = NULL;
    if (!b)
        return NULL;
    link_decls(c);
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
    /* hover texts of tags and fields (persistent symbols': link_decls) */
    for (i = 0; i < b->recmap.len && i < c->tt.recs.len; i++)
        if (b->recmap.data[i]) {
            const Record *r = &c->tt.recs.data[i];
            b->decls.data[b->recmap.data[i] - 1].hover = record_hover(c, r);
        }
    for (i = 0; i < b->enmap.len; i++)
        if (b->enmap.data[i])
            b->decls.data[b->enmap.data[i] - 1].hover =
                i < b->enhover.len ? b->enhover.data[i] : 0;
    for (k = 0; k < c->tt.recs.len; k++) {
        const Record *r = &c->tt.recs.data[k];
        for (i = r->fields; i < r->fields + r->nfields && i < b->fmap.len; i++)
            if (b->fmap.data[i])
                b->decls.data[b->fmap.data[i] - 1].hover =
                    field_hover(c, r, &c->tt.fields.data[i]);
    }
    for (i = 0; i < nd; i++) {   /* else "KIND NAME" (an incomplete enum) */
        BDecl *d = &b->decls.data[i];
        if (d->hover)
            continue;
        b->tmp.len = 0;
        sb_printf(&b->tmp, "%s %s", cindex_kind_name(d->kind),
                  d->name && d->name < interner_count(c->in)
                      ? ident_by_id(c->in, d->name)->str : "<anonymous>");
        d->hover = hover_add(b, &b->tmp);
    }

    ix = xcalloc(1, sizeof *ix);
    ix->unindexed = b->unindexed;
    ix->excused = b->excused;
    memset(&pool, 0, sizeof pool);
    pool.by_ident = xcalloc(interner_count(c->in) + 1, sizeof *pool.by_ident);
    pool.sb = b->hs;         /* the hover texts start the pool */
    memset(&b->hs, 0, sizeof b->hs);
    free(b->hset);
    b->hset = NULL;
    b->hcap = b->hcount = 0;
    b->enhover.len = 0;
    if (!pool.sb.len)
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
                ix->files[nf].edited = false;
                ix->files[nf].dmg_begin = ix->files[nf].dmg_end = 0;
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
                   cindex_ident_char(cur->buf[o->off + n]))
                n++;
            o->len = (uint8_t)n;
        }
        if (is_sys(b, c->sm, e->loc))
            o->flags |= CIX_SYSTEM;
    }
    ix->nev = ne;
    ix->nfiles = nf;
    smap = freeze_scopes(c, ix, pool.sb.data, path_off);
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
        o->hover = d->hover;
        o->kind = d->kind;
        o->linkage = d->linkage;
        o->flags = d->flags;
        o->type = d->type;
        o->parent = d->parent;
        o->scope = d->scope ? smap[d->scope - 1] : 0;
    }
    free(smap);
    b->scopes.len = 0;
    free(pool.by_ident);
    ix->nstrings = pool.sb.len;
    ix->strings = pool.sb.data ? xrealloc(pool.sb.data, pool.sb.len) : NULL;
    for (i = 0; i < nf; i++)
        ix->files[i].path = ix->strings + path_off[i];
    free(path_off);
    build_csr(ix);
    ix->ev = xrealloc(ix->ev, (ne + 1) * sizeof *ix->ev);
    b->ev.len = b->decls.len = 0;
    ix->refs = 1;
    ix->serial = next_serial();
    return ix;
}

/* ---- the frozen index ---------------------------------------------------------- */

void cindex_free(CIndex *ix)
{
    if (!ix || atomic_add_u32(&ix->refs, (uint32_t)-1) != 1)
        return;
    free(ix->files);
    free(ix->ev);
    free(ix->by_decl);
    free(ix->by_decl_start);
    free(ix->scopes);
    if (ix->core) {          /* decls and strings are the core's */
        cindex_free(ix->core);
    } else {
        free(ix->decls);
        free(ix->strings);
    }
    free(ix);
}

CIndex *cindex_ref(CIndex *ix)
{
    if (ix)
        atomic_add_u32(&ix->refs, 1);
    return ix;
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
           (ix->ndecls + 1) * sizeof *ix->by_decl_start +
           ix->nscopes * sizeof *ix->scopes +
           (ix->core ? 0 : (ix->ndecls + 1) * sizeof *ix->decls + ix->nstrings);
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

uint32_t cindex_lower(const CIndex *ix, uint32_t fi, uint32_t off)
{
    uint32_t lo = 0, hi = ix->nev;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const CIdxEvent *e = &ix->ev[mid];
        if (e->file < fi || (e->file == fi && e->off < off))
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

size_t cindex_types(const CIndex *ix, const uint32_t *decls, size_t nd,
                    uint32_t *out)
{
    size_t i, j, n = 0;
    for (i = 0; i < nd; i++) {
        uint32_t t = ix->decls[decls[i]].type;
        for (j = 0; j < n && out[j] != t - 1; j++)
            ;
        if (t && j == n)
            out[n++] = t - 1;
    }
    return n;
}

uint32_t cindex_scope_at(const CIndex *ix, uint32_t fi, uint32_t off)
{
    uint32_t lo = 0, hi = ix->nscopes, s;
    while (lo < hi) {               /* the first scope starting after off */
        uint32_t mid = lo + (hi - lo) / 2;
        const CIdxScope *x = &ix->scopes[mid];
        if (x->file < fi || (x->file == fi && x->begin <= off))
            lo = mid + 1;
        else
            hi = mid;
    }
    /* every scope containing off is an ancestor of the last one starting at
     * or before it (proper nesting) */
    for (s = lo; s && ix->scopes[s - 1].file == fi; s = ix->scopes[s - 1].parent)
        if (off < ix->scopes[s - 1].end)
            return s;
    return 0;
}

uint32_t cindex_scope_root(const CIndex *ix, uint32_t s)
{
    while (s && ix->scopes[s - 1].parent)
        s = ix->scopes[s - 1].parent;
    return s;
}

typedef struct Vis {
    uint32_t decl, rank;     /* rank: the scope's index + 1, 0 file level */
    const char *name;
} Vis;

static int vis_cmp(const void *pa, const void *pb)
{
    const Vis *a = pa, *b = pb;
    int r = strcmp(a->name, b->name);
    if (r)
        return r;
    if (a->rank != b->rank)
        return a->rank > b->rank ? -1 : 1;
    return a->decl < b->decl ? -1 : a->decl > b->decl;
}

size_t cindex_visible(const CIndex *ix, const char *path, uint32_t off,
                      uint32_t **out)
{
    int fi = ix ? cindex_file(ix, path) : -1;
    VEC(Vis) v = {0};
    uint32_t d, j;
    size_t k, n = 0;
    *out = NULL;
    if (fi < 0 || ix->files[fi].stale)
        return 0;
    for (d = 0; d < ix->ndecls; d++) {
        const CIdxDecl *x = &ix->decls[d];
        Vis best = {0, 0, NULL};
        if (!x->name || (x->kind != CIK_FUNC && x->kind != CIK_OBJ &&
                         x->kind != CIK_PARAM && x->kind != CIK_TYPEDEF &&
                         x->kind != CIK_ENUMCONST))
            continue;
        for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++) {
            const CIdxEvent *e = &ix->ev[ix->by_decl[j]];
            uint32_t s;
            if ((e->flags & CIX_ROLE) == CIX_REF || ix->files[e->file].stale ||
                ((int)e->file == fi && e->off > off))
                continue;    /* not a declaration, or not before off */
            s = x->scope;
            if (!s && (s = cindex_scope_at(ix, e->file, e->off)) &&
                !ix->scopes[s - 1].parent)
                s = 0;       /* an external declaration: file level */
            if (s && ((int)ix->scopes[s - 1].file != fi ||
                      off < ix->scopes[s - 1].begin || off >= ix->scopes[s - 1].end))
                continue;    /* its scope does not contain off */
            if (!best.name || s > best.rank) {
                best.decl = d;
                best.rank = s;
                best.name = ix->strings + x->name;
            }
        }
        if (best.name)
            vec_push(&v, best);
    }
    if (v.len > 1)
        qsort(v.data, v.len, sizeof *v.data, vis_cmp);
    *out = xmalloc((v.len + 1) * sizeof **out);
    for (k = 0; k < v.len; k++)     /* the innermost of each name */
        if (!k || strcmp(v.data[k].name, v.data[k - 1].name))
            (*out)[n++] = v.data[k].decl;
    vec_free(&v);
    return n;
}

void cindex_hover(const CIndex *ix, const uint32_t *decls, size_t nd, bool md,
                  StrBuf *out)
{
    size_t i, j, shown = 0, more = 0;
    for (i = 0; i < nd; i++) {
        uint32_t h = ix->decls[decls[i]].hover;
        for (j = 0; j < i && ix->decls[decls[j]].hover != h; j++)
            ;
        if (j < i)
            continue;        /* equal texts share their offset */
        if (shown == 5) {
            more++;
            continue;
        }
        if (md)
            sb_printf(out, "%s```c\n%s\n```", shown ? "\n---\n" : "",
                      ix->strings + h);
        else
            sb_printf(out, "%s%s", shown ? "\n" : "", ix->strings + h);
        shown++;
    }
    if (more)
        sb_printf(out, "\nand %zu more", more);
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

bool cindex_is_identifier(const char *s)
{
    if (!*s || (*s >= '0' && *s <= '9'))
        return false;
    for (; *s; s++)
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '_'))
            return false;
    return true;
}

const char *cindex_place(SrcMgr *sm, const char *path, uint32_t off, char *buf,
                         size_t bufsz)
{
    SrcFile *f = cindex_srcfile(sm, path);
    uint32_t l = 0, c = 0;
    if (f)
        srcmgr_linecol(f, f->base + off, &l, &c);
    snprintf(buf, bufsz, "%s:%u:%u", f ? f->name : path, l, c);
    return buf;
}

const char *cindex_rename_plan(const CIndex *ix, SrcMgr *sm, const char *path,
                               uint32_t off, const char *main, uint32_t **ev,
                               size_t *n, char *msg, size_t msgsz)
{
    uint32_t decls[2], d, j;
    int mf = cindex_file(ix, main);
    bool declared = false;
    char at[512];
    size_t nd = cindex_decls_at(ix, path, off, decls, 2);
    const CIdxDecl *x;
    *ev = NULL;
    *n = 0;
    if (!nd)
        return "no C name here";
    if (nd > 1)
        return "several C entities are named here (a #define body, or a "
               "header read twice); renaming one would rename the others";
    d = decls[0];
    x = &ix->decls[d];
    if (x->flags & (CIDF_BUILTIN | CIDF_SYSTEM)) {
        snprintf(msg, msgsz, "'%s' is predeclared or declared in a system header",
                 cindex_name(ix, d));
        return msg;
    }
    for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++) {
        const CIdxEvent *e = &ix->ev[ix->by_decl[j]];
        const char *p = ix->files[e->file].path;
        if ((e->flags & CIX_ROLE) != CIX_REF)
            declared = true;
        if (ix->files[e->file].stale)
            return "the C symbol index is not ready; retry";
        if (e->flags & (CIX_MACRO_BODY | CIX_AT_EXPANSION)) {
            snprintf(msg, msgsz, "'%s' at %s is %s; renaming would change the "
                     "macro", cindex_name(ix, d),
                     cindex_place(sm, p, e->off, at, sizeof at),
                     e->flags & CIX_MACRO_BODY ? "spelled in a #define body"
                                               : "formed by ## or a macro");
            return msg;
        }
        if ((int)e->file != mf) {
            snprintf(msg, msgsz, "'%s' is declared or used in %s, which other "
                     "units may include; renaming it needs the project index",
                     cindex_name(ix, d), cindex_place(sm, p, e->off, at, sizeof at));
            return msg;
        }
    }
    if ((x->flags & CIDF_IMPLICIT) && !declared) {
        snprintf(msg, msgsz, "'%s' is only declared implicitly", cindex_name(ix, d));
        return msg;
    }
    *n = cindex_select(ix, &d, 1, CIQ_REFS, mf, ev);
    return NULL;
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
        const char *h;
        fprintf(out, "#%u %s %s%s%s%s%s%s%s%s", i, kind_names[d->kind],
                d->name ? cindex_name(ix, i) : "<anonymous>",
                d->linkage == 1 ? " internal" : d->linkage == 2 ? " external" : "",
                d->flags & CIDF_BUILTIN ? " builtin" : "",
                d->flags & CIDF_IMPLICIT ? " implicit" : "",
                d->flags & CIDF_SYSTEM ? " system" : "",
                d->flags & CIDF_TENTATIVE ? " tentative" : "",
                d->flags & CIDF_READONLY ? " readonly" : "",
                d->flags & CIDF_STATIC ? " static" : "");
        if (d->type)
            fprintf(out, " type #%u", d->type - 1);
        if (d->parent)
            fprintf(out, " parent #%u", d->parent - 1);
        if (d->scope) {          /* lines of the scope's file */
            const CIdxScope *s = &ix->scopes[d->scope - 1];
            uint32_t l0 = 0, l1 = 0, c0;
            if ((f = cindex_srcfile(sm, ix->files[s->file].path)) != NULL) {
                srcmgr_linecol(f, f->base + s->begin, &l0, &c0);
                srcmgr_linecol(f, f->base + s->end - 1, &l1, &c0);
            }
            fprintf(out, " scope %u-%u", l0, l1);
        }
        fputs(" :: ", out);
        for (h = ix->strings + d->hover; *h; h++)  /* the hover text, one line */
            if (*h == '\n')
                fputs("\\n", out);
            else
                fputc(*h, out);
        fputc('\n', out);
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
        if (!declared && !ix->core && ix->decls[i].name &&
            !(ix->decls[i].flags & (CIDF_BUILTIN | CIDF_IMPLICIT | CIDF_SYSTEM))) {
            fprintf(out, "verify: #%u %s '%s' has no declaration event\n", i,
                    kind_names[ix->decls[i].kind], cindex_name(ix, i));
            bad++;
        }
        if (ix->decls[i].type > ix->ndecls || ix->decls[i].parent > ix->ndecls ||
            ix->decls[i].scope > ix->nscopes) {
            fprintf(out, "verify: #%u: type, parent or scope out of range\n", i);
            bad++;
        }
    }
    /* scopes: sorted, each inside its parent (an earlier one in its file),
     * none overlapping the scopes open before it only in part */
    for (i = 0; i < ix->nscopes; i++) {
        const CIdxScope *s = &ix->scopes[i], *p;
        uint32_t o = i;
        if (i && scope_cmp_ix(&ix->scopes[i - 1], s) > 0) {
            fprintf(out, "verify: scopes %u and %u out of order\n", i - 1, i);
            bad++;
        }
        if (s->parent && (s->parent > i || (p = &ix->scopes[s->parent - 1])->file !=
                          s->file || p->begin > s->begin || p->end < s->end)) {
            fprintf(out, "verify: scope %u is not inside its parent\n", i);
            bad++;
        }
        while (o && ix->scopes[o - 1].file == s->file &&
               ix->scopes[o - 1].end <= s->begin)
            o = ix->scopes[o - 1].parent;   /* the open scopes before it */
        if (o && ix->scopes[o - 1].file == s->file && ix->scopes[o - 1].end < s->end) {
            fprintf(out, "verify: scope %u overlaps scope %u\n", i, o - 1);
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
    fprintf(out, "symbols: %u events, %u decls, %u scopes, %u files, %zu bytes, "
            "%u unindexed, %u excused\n", ix->nev, ix->ndecls, ix->nscopes,
            ix->nfiles, cindex_bytes(ix), ix->unindexed, ix->excused);
    return bad + ix->unindexed;
}

/* ---- carrying an index across an edit (B3_DESIGN.md 12) --------------------- */

void cindex_text_edit(const char *a, size_t na, const char *b, size_t nb,
                      CIdxEdit *out)
{
    size_t m = na < nb ? na : nb, pre = 0, suf = 0, oe, ne;
    while (pre < m && a[pre] == b[pre])
        pre++;
    if (pre == na && na == nb) {
        out->pre = out->old_end = out->new_end = (uint32_t)na;
        out->same = true;
        return;
    }
    while (suf < m - pre && a[na - 1 - suf] == b[nb - 1 - suf])
        suf++;
    oe = na - suf;
    ne = nb - suf;
    /* whole identifiers: one that continues across a boundary, in either
     * text, is inside the span (a[oe..] and b[ne..] are the same bytes) */
    if ((pre < na && cindex_ident_char(a[pre])) ||
        (pre < nb && cindex_ident_char(b[pre])))
        while (pre > 0 && cindex_ident_char(a[pre - 1]))
            pre--;
    if ((oe > 0 && cindex_ident_char(a[oe - 1])) ||
        (ne > 0 && cindex_ident_char(b[ne - 1])))
        while (oe < na && cindex_ident_char(a[oe])) {
            oe++;
            ne++;
        }
    out->pre = (uint32_t)pre;
    out->old_end = (uint32_t)oe;
    out->new_end = (uint32_t)ne;
    out->same = false;
}

/* The map of an old position (a point between bytes): monotone. */
uint32_t cindex_edit_map(const CIdxEdit *e, uint32_t x)
{
    if (x <= e->pre)
        return x;
    return x >= e->old_end ? x - e->old_end + e->new_end : e->pre;
}

CIndex *cindex_carry(CIndex *from, SrcMgr *sm_old, SrcMgr *sm_new)
{
    uint32_t nf = from->nfiles, i, k, ne = 0;
    CIdxEdit *ed = xcalloc(nf + 1, sizeof *ed);   /* zeros: the identity */
    SrcFile **nt = xcalloc(nf + 1, sizeof *nt);   /* the new text, or NULL */
    bool changed = false;
    CIndex *c;
    for (i = 0; i < nf; i++) {
        const CIdxFile *cf = &from->files[i];
        SrcFile *fo, *fn;
        ed[i].same = true;
        if (cf->stale)
            continue;
        fo = cindex_srcfile(sm_old, cf->path);
        fn = cindex_srcfile(sm_new, cf->path);
        if (!fo || !fn) {
            changed = true;                       /* becomes stale */
            continue;
        }
        cindex_text_edit(fo->buf, fo->size, fn->buf, fn->size, &ed[i]);
        nt[i] = fn;
        changed |= !ed[i].same;
    }
    if (!changed) {
        free(ed);
        free(nt);
        return cindex_ref(from);
    }
    c = xcalloc(1, sizeof *c);
    c->refs = 1;
    c->serial = next_serial();
    c->core = cindex_ref(from->core ? from->core : from);
    c->decls = from->decls;
    c->ndecls = from->ndecls;
    c->strings = from->strings;
    c->nstrings = from->nstrings;
    c->unindexed = from->unindexed;
    c->excused = from->excused;
    c->nfiles = nf;
    c->files = xmalloc((nf + 1) * sizeof *c->files);
    memcpy(c->files, from->files, nf * sizeof *c->files);
    for (i = 0; i < nf; i++) {
        CIdxFile *cf = &c->files[i];
        const CIdxEdit *e = &ed[i];
        if (cf->stale)
            continue;
        if (!nt[i]) {
            cf->stale = true;
            continue;
        }
        if (!e->same) {
            uint32_t b = e->pre, en = e->new_end;
            if (cf->edited) {                     /* hull with the old damage */
                uint32_t mb = cindex_edit_map(e, cf->dmg_begin);
                uint32_t me = cindex_edit_map(e, cf->dmg_end);
                b = mb < b ? mb : b;
                en = me > en ? me : en;
            }
            cf->size = nt[i]->size;
            cf->hash = cindex_hash(nt[i]->buf, nt[i]->size);
            cf->edited = true;
            cf->dmg_begin = b;
            cf->dmg_end = en;
        }
    }
    c->ev = xmalloc((from->nev + 1) * sizeof *c->ev);
    for (k = 0; k < from->nev; k++) {
        CIdxEvent o = from->ev[k];
        const CIdxEdit *e = &ed[o.file];
        if (o.off >= e->old_end)
            o.off = o.off - e->old_end + e->new_end;
        else if (o.off + o.len > e->pre)
            continue;                             /* overlaps the edit */
        c->ev[ne++] = o;
    }
    c->nev = ne;
    c->nscopes = from->nscopes;
    c->scopes = xmalloc((from->nscopes + 1) * sizeof *c->scopes);
    for (i = 0; i < from->nscopes; i++) {
        const CIdxEdit *e = &ed[from->scopes[i].file];
        c->scopes[i] = from->scopes[i];
        c->scopes[i].begin = cindex_edit_map(e, from->scopes[i].begin);
        c->scopes[i].end = cindex_edit_map(e, from->scopes[i].end);
    }
    build_csr(c);
    free(ed);
    free(nt);
    return c;
}

bool cindex_damaged(const CIndex *ix, int fi, uint32_t off)
{
    const CIdxFile *cf;
    if (!ix || fi < 0 || (uint32_t)fi >= ix->nfiles)
        return false;
    cf = &ix->files[fi];
    return cf->edited && off >= cf->dmg_begin && off <= cf->dmg_end;
}

/* How many DECL and DEF events decl d has in ix. */
static uint32_t decl_events(const CIndex *ix, uint32_t d)
{
    uint32_t j, n = 0;
    for (j = ix->by_decl_start[d]; j < ix->by_decl_start[d + 1]; j++)
        if ((ix->ev[ix->by_decl[j]].flags & CIX_ROLE) != CIX_REF)
            n++;
    return n;
}

bool cindex_touched(const CIndex *ix, uint32_t d)
{
    return ix->core && d < ix->ndecls &&
           decl_events(ix, d) < decl_events(ix->core, d);
}

/* ---- --verify-carry ---------------------------------------------------------- */

typedef struct VEv {
    uint32_t off;
    uint32_t role;
    uint32_t kind;
    const char *name;
} VEv;

static int vev_cmp(const VEv *a, const VEv *b)
{
    if (a->off != b->off)
        return a->off < b->off ? -1 : 1;
    if (a->role != b->role)
        return a->role < b->role ? -1 : 1;
    if (a->kind != b->kind)
        return a->kind < b->kind ? -1 : 1;
    return strcmp(a->name, b->name);
}

static int vev_qcmp(const void *a, const void *b)
{
    return vev_cmp(a, b);
}

/* The events of file fi of ix that lie outside [db, de] (the damage; none
 * excluded if !edited), sorted by (off, role, kind, name). */
static VEv *vev_gather(const CIndex *ix, int fi, bool edited, uint32_t db,
                       uint32_t de, size_t *n)
{
    VEv *v;
    size_t cnt = 0;
    uint32_t k;
    v = xmalloc((ix->nev + 1) * sizeof *v);
    for (k = fi < 0 ? ix->nev : cindex_lower(ix, (uint32_t)fi, 0);
         k < ix->nev && ix->ev[k].file == fi; k++) {
        const CIdxEvent *e = &ix->ev[k];
        if (edited && !(e->off + e->len <= db || e->off >= de))
            continue;
        v[cnt].off = e->off;
        v[cnt].role = e->flags & CIX_ROLE;
        v[cnt].kind = ix->decls[e->decl].kind;
        v[cnt].name = cindex_name(ix, e->decl);
        cnt++;
    }
    qsort(v, cnt, sizeof *v, vev_qcmp);
    *n = cnt;
    return v;
}

typedef struct VSc {
    uint32_t b, e;
} VSc;

/* (begin ascending, end descending), as the index sorts scopes */
static int vsc_cmp(const VSc *x, const VSc *y)
{
    if (x->b != y->b)
        return x->b < y->b ? -1 : 1;
    return x->e > y->e ? -1 : x->e < y->e;
}

/* The scopes of file fi, in index order, with neither end in the damage. */
static VSc *vsc_gather(const CIndex *ix, int fi, bool edited, uint32_t db,
                       uint32_t de, size_t *n)
{
    VSc *v = xmalloc((ix->nscopes + 1) * sizeof *v);
    size_t cnt = 0;
    uint32_t k;
    for (k = 0; fi >= 0 && k < ix->nscopes; k++) {
        const CIdxScope *s = &ix->scopes[k];
        if (s->file != (uint32_t)fi ||
            (edited && ((s->begin >= db && s->begin <= de) ||
                        (s->end >= db && s->end <= de))))
            continue;
        v[cnt].b = s->begin;
        v[cnt].e = s->end;
        cnt++;
    }
    *n = cnt;
    return v;
}

/* The "L:C" of offset off, without the file name. */
static const char *place_lc(SrcMgr *sm, const char *path, uint32_t off, char *buf,
                            size_t bufsz)
{
    char *p;
    int colons = 0;
    cindex_place(sm, path, off, buf, bufsz);
    for (p = buf + strlen(buf); p > buf; p--)
        if (p[-1] == ':' && ++colons == 2)
            return p;
    return buf;
}

size_t cindex_verify_carry(const CIndex *ix1, const CIndex *c,
                           const CIndex *fresh, SrcMgr *sm2, FILE *out)
{
    char *vb = NULL, *line;
    size_t vl = 0, bad, ndiff = 0, nedited = 0;
    FILE *m = open_memstream(&vb, &vl);
    char dmg[1200] = "";
    uint32_t i, d;
    if (!m)
        fatal("out of memory");
    bad = cindex_verify(c, sm2, m);
    fclose(m);
    for (line = vb; line && *line;) {      /* the problems, not the summary */
        char *nl = strchr(line, '\n');
        if (strncmp(line, "symbols: ", 9))
            fwrite(line, 1, nl ? (size_t)(nl - line) + 1 : strlen(line), out);
        line = nl ? nl + 1 : NULL;
    }
    free(vb);
    for (d = 0; d < c->ndecls; d++)        /* touched: fewer DECL/DEF events */
        if (cindex_touched(c, d) != (decl_events(c, d) < decl_events(ix1, d))) {
            fprintf(out, "verify: #%u: touched disagrees with the events\n", d);
            bad++;
        }
    for (i = 0; i < c->nfiles + fresh->nfiles; i++) {
        /* the files of the carried index, then those only fresh has */
        const char *path;
        int ci, fi;
        bool ed;
        uint32_t db = 1, de = 0;
        size_t nc, nf, a, b;
        VEv *vc, *vf;
        VSc *sc, *sf;
        char p1[512], p2[512];
        if (i < c->nfiles) {
            ci = (int)i;
            path = c->files[i].path;
            fi = cindex_file(fresh, path);
            if (c->files[i].stale)
                continue;
        } else {
            path = fresh->files[i - c->nfiles].path;
            if (cindex_file(c, path) >= 0)
                continue;
            ci = -1;
            fi = (int)(i - c->nfiles);
        }
        ed = ci >= 0 && c->files[ci].edited;
        if (ed) {
            db = c->files[ci].dmg_begin;
            de = c->files[ci].dmg_end;
            if (!cindex_damaged(c, ci, db) || !cindex_damaged(c, ci, de) ||
                (db && cindex_damaged(c, ci, db - 1)) ||
                cindex_damaged(c, ci, de + 1)) {
                fprintf(out, "verify: %s: damage query disagrees\n", path);
                bad++;
            }
            snprintf(dmg + strlen(dmg), sizeof dmg - strlen(dmg), "%s%s-%s",
                     nedited ? " " : "", place_lc(sm2, path, db, p1, sizeof p1),
                     place_lc(sm2, path, de, p2, sizeof p2));
            nedited++;
        }
        vc = vev_gather(c, ci, ed, db, de, &nc);
        vf = vev_gather(fresh, fi, ed, db, de, &nf);
        for (a = b = 0; a < nc || b < nf;) {
            int r = a == nc ? 1 : b == nf ? -1 : vev_cmp(&vc[a], &vf[b]);
            const VEv *e = r <= 0 ? &vc[a] : &vf[b];
            if (!r) {
                a++, b++;
                continue;
            }
            if (ndiff++ < 20)
                fprintf(out, "diff: %s %s %s %s at %s\n", r < 0 ? "carried" : "fresh",
                        cindex_role_name(e->role), cindex_kind_name(e->kind),
                        e->name, cindex_place(sm2, path, e->off, p1, sizeof p1));
            r < 0 ? a++ : b++;
        }
        free(vc);
        free(vf);
        /* scopes outside the damage: the same (begin, end) in the same order */
        sc = vsc_gather(c, ci, ed, db, de, &nc);
        sf = vsc_gather(fresh, fi, ed, db, de, &nf);
        for (a = b = 0; a < nc || b < nf;) {
            int r = a == nc ? 1 : b == nf ? -1 : vsc_cmp(&sc[a], &sf[b]);
            const VSc *s = r <= 0 ? &sc[a] : &sf[b];
            if (!r) {
                a++, b++;
                continue;
            }
            if (ndiff++ < 20)
                fprintf(out, "diff: %s scope %s-%s\n", r < 0 ? "carried" : "fresh",
                        place_lc(sm2, path, s->b, p1, sizeof p1),
                        place_lc(sm2, path, s->e, p2, sizeof p2));
            r < 0 ? a++ : b++;
        }
        free(sc);
        free(sf);
    }
    if (ndiff > 20)
        fprintf(out, "diff: ... and %zu more\n", ndiff - 20);
    fprintf(out, "carry: kept %u of %u events, damage %s, %zu differences\n",
            c->nev, ix1->nev, nedited ? dmg : "none", ndiff);
    return bad;
}
