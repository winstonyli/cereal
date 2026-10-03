/* type.c - the type table: hash-consing, sizes, layout, compatibility and
 * gcc-compatible printing.  See type.h and docs/TYPES.md. */
#include "c/type.h"

#include <ctype.h>
#include <string.h>

/* ---- the table ------------------------------------------------------- */

static uint32_t mix(uint32_t h, uint64_t v)
{
    h ^= (uint32_t)v + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= (uint32_t)(v >> 32) * 0x85ebca6bu;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 15);
}

static uint32_t ent_hash(const TypeTable *tt, const TypeEnt *e)
{
    uint32_t h = mix(e->kind | (uint32_t)e->flags << 8, e->base);
    h = mix(h, e->n);
    if (e->kind == TY_FUNC) {
        const TypeId *p = tt->params.data + e->extra;
        for (uint64_t i = 0; i < e->n; i++)
            h = mix(h, p[i]);
    } else {
        h = mix(h, e->extra);
    }
    return h;
}

static bool ent_eq(const TypeTable *tt, const TypeEnt *a, const TypeEnt *b)
{
    if (a->kind != b->kind || a->flags != b->flags || a->base != b->base ||
        a->n != b->n)
        return false;
    if (a->kind != TY_FUNC)
        return a->extra == b->extra;
    return a->n == 0 ||
           !memcmp(tt->params.data + a->extra, tt->params.data + b->extra,
                   a->n * sizeof(TypeId));
}

static void rehash(TypeTable *tt)
{
    uint32_t cap = tt->slot_mask ? (tt->slot_mask + 1) * 2 : 256;
    free(tt->slots);
    tt->slots = xcalloc(cap, sizeof *tt->slots);
    tt->slot_mask = cap - 1;
    for (uint32_t i = TY_NBUILTIN; i < tt->ents.len; i++) {
        const TypeEnt *e = &tt->ents.data[i];
        if (e->kind == TY_TYPEDEF || e->kind == TY_STRUCT ||
            e->kind == TY_UNION || e->kind == TY_ENUM)
            continue;
        uint32_t s = ent_hash(tt, e) & tt->slot_mask;
        while (tt->slots[s])
            s = (s + 1) & tt->slot_mask;
        tt->slots[s] = i;
    }
}

/* Finds or adds e.  canon: the canonical type, 0 if e is itself canonical.
 * For functions e->extra indexes parameters already pushed to the pool;
 * they are popped again if the type exists. */
static TypeId hashcons(TypeTable *tt, TypeEnt e, TypeId canon)
{
    if ((tt->nhashed + 1) * 4 > (tt->slot_mask + 1) * 3)
        rehash(tt);
    uint32_t s = ent_hash(tt, &e) & tt->slot_mask;
    for (uint32_t i; (i = tt->slots[s]); s = (s + 1) & tt->slot_mask) {
        if (ent_eq(tt, &tt->ents.data[i], &e)) {
            if (e.kind == TY_FUNC)
                tt->params.len = e.extra;
            return TYPE_MK(i, 0);
        }
    }
    uint32_t idx = (uint32_t)tt->ents.len;
    e.canon = canon ? canon : TYPE_MK(idx, 0);
    vec_push(&tt->ents, e);
    tt->slots[s] = idx;
    tt->nhashed++;
    return TYPE_MK(idx, 0);
}

static TypeId add_ent(TypeTable *tt, TypeEnt e)
{
    uint32_t idx = (uint32_t)tt->ents.len;
    if (!e.canon)
        e.canon = TYPE_MK(idx, 0);
    vec_push(&tt->ents, e);
    return TYPE_MK(idx, 0);
}

static void init_va_list(TypeTable *tt)
{
    uint32_t name = intern_cstr(tt->in, "__builtin_va_list")->id;
    TypeId vp = type_ptr(tt, TYPE_B(VOID));
    switch (tt->tgt->va_list) {
    case VA_CHAR_PTR:
        tt->va_list = type_typedef(tt, name, type_ptr(tt, TYPE_B(CHAR)));
        return;
    case VA_X86_64: {
        TypeId r = type_new_record(tt, intern_cstr(tt->in, "__va_list_tag")->id,
                                   false, 0);
        FieldIn f[4] = {
            {intern_cstr(tt->in, "gp_offset")->id, TYPE_B(UINT), -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "fp_offset")->id, TYPE_B(UINT), -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "overflow_arg_area")->id, vp, -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "reg_save_area")->id, vp, -1, 0, false, 0, 0, 0, 0},
        };
        type_complete_record(tt, r, f, 4, 0, 0, false, 0);
        type_record(tt, r)->flags |= RF_NOKEYWORD;
        tt->va_list = type_typedef(tt, name, type_array(tt, r, 1));
        return;
    }
    case VA_AARCH64: {
        TypeId r = type_new_record(tt, intern_cstr(tt->in, "__va_list")->id,
                                   false, 0);
        FieldIn f[5] = {
            {intern_cstr(tt->in, "__stack")->id, vp, -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "__gr_top")->id, vp, -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "__vr_top")->id, vp, -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "__gr_offs")->id, TYPE_B(INT), -1, 0, false, 0, 0, 0, 0},
            {intern_cstr(tt->in, "__vr_offs")->id, TYPE_B(INT), -1, 0, false, 0, 0, 0, 0},
        };
        type_complete_record(tt, r, f, 5, 0, 0, false, 0);
        type_record(tt, r)->flags |= RF_NOKEYWORD;
        tt->va_list = type_typedef(tt, name, r);
        return;
    }
    }
}

void types_init(TypeTable *tt, const Target *tgt, Interner *in)
{
    memset(tt, 0, sizeof *tt);
    tt->tgt = tgt;
    tt->in = in;
    for (uint32_t k = 0; k < TY_NBUILTIN; k++) {
        TypeEnt e = {0};
        e.kind = (uint8_t)k;
        e.canon = TYPE_MK(k, 0);
        vec_push(&tt->ents, e);
    }
    vec_push(&tt->recs, (Record){0});   /* 0: none */
    vec_push(&tt->enums, (Enum){0});
    rehash(tt);
    init_va_list(tt);
}

void types_free(TypeTable *tt)
{
    vec_free(&tt->ents);
    free(tt->slots);
    vec_free(&tt->params);
    vec_free(&tt->recs);
    vec_free(&tt->fields);
    vec_free(&tt->enums);
    for (unsigned i = 0; i < ARRAY_LEN(tt->qbuf); i++)
        sb_free(&tt->qbuf[i]);
}

TypeId type_canon(TypeTable *tt, TypeId t)
{
    TypeId c = tt->ents.data[TYPE_IDX(t)].canon | TYPE_QUALS(t);
    if (!TYPE_QUALS(c))
        return c;
    const TypeEnt *e = &tt->ents.data[TYPE_IDX(c)];
    if (e->kind == TY_ARRAY || e->kind == TY_VLA) {
        uint8_t ek = e->kind, ef = e->flags;   /* ents may grow below */
        uint64_t en = e->n;
        TypeId el = type_canon(tt, e->base | TYPE_QUALS(c));
        if (ek == TY_VLA)
            return type_vla(tt, el);
        if (ef & TF_INCOMPLETE)
            return type_array_incomplete(tt, el);
        return type_array(tt, el, en);
    }
    return c;
}

/* ---- construction ---------------------------------------------------- */

TypeId type_ptr(TypeTable *tt, TypeId to)
{
    TypeId c = type_canon(tt, to);
    TypeId canon = c == to ? 0 : type_ptr(tt, c);
    return hashcons(tt, (TypeEnt){.kind = TY_PTR, .base = to}, canon);
}

static TypeId mk_array(TypeTable *tt, uint8_t kind, uint8_t flags,
                       TypeId elem, uint64_t n)
{
    TypeId c = type_canon(tt, elem);
    TypeId canon = c == elem ? 0 : mk_array(tt, kind, flags, c, n);
    return hashcons(tt, (TypeEnt){.kind = kind, .flags = flags, .base = elem,
                                  .n = n}, canon);
}

TypeId type_array(TypeTable *tt, TypeId elem, uint64_t n)
{
    return mk_array(tt, TY_ARRAY, 0, elem, n);
}

TypeId type_array_incomplete(TypeTable *tt, TypeId elem)
{
    return mk_array(tt, TY_ARRAY, TF_INCOMPLETE, elem, 0);
}

TypeId type_vla(TypeTable *tt, TypeId elem)
{
    return mk_array(tt, TY_VLA, 0, elem, 0);
}

TypeId type_func(TypeTable *tt, TypeId ret, const TypeId *params,
                 uint32_t n, unsigned flags)
{
    bool canonical = type_canon(tt, ret) == ret;
    for (uint32_t i = 0; i < n && canonical; i++)
        canonical = type_canon(tt, params[i]) == params[i];
    TypeId canon = 0;
    if (!canonical) {
        TypeId *cp = xmalloc((n ? n : 1) * sizeof *cp);
        for (uint32_t i = 0; i < n; i++)
            cp[i] = type_canon(tt, params[i]);
        canon = type_func(tt, type_canon(tt, ret), cp, n, flags);
        free(cp);
    }
    uint32_t off = (uint32_t)tt->params.len;
    for (uint32_t i = 0; i < n; i++)
        vec_push(&tt->params, params[i]);
    return hashcons(tt, (TypeEnt){.kind = TY_FUNC, .flags = (uint8_t)flags,
                                  .base = ret, .extra = off, .n = n}, canon);
}

TypeId type_vector(TypeTable *tt, TypeId elem, uint64_t bytes)
{
    TypeId c = type_canon(tt, elem);
    TypeId canon = c == elem ? 0 : type_vector(tt, c, bytes);
    return hashcons(tt, (TypeEnt){.kind = TY_VECTOR, .base = elem,
                                  .n = bytes}, canon);
}

TypeId type_complex(TypeTable *tt, TypeId real)
{
    TypeId c = type_canon(tt, real);
    TypeId canon = c == real ? 0 : type_complex(tt, c);
    return hashcons(tt, (TypeEnt){.kind = TY_COMPLEX, .base = real}, canon);
}

TypeId type_typedef(TypeTable *tt, uint32_t name, TypeId to)
{
    return add_ent(tt, (TypeEnt){.kind = TY_TYPEDEF, .base = to,
                                 .extra = name,
                                 .canon = type_canon(tt, to)});
}

TypeId type_new_record(TypeTable *tt, uint32_t tag, bool is_union,
                       SrcLoc loc)
{
    Record r = {0};
    r.tag = tag;
    r.loc = loc;
    r.flags = is_union ? RF_UNION : 0;
    r.align = 1;
    uint32_t ri = (uint32_t)tt->recs.len;
    r.ty = add_ent(tt, (TypeEnt){.kind = is_union ? TY_UNION : TY_STRUCT,
                                 .extra = ri});
    vec_push(&tt->recs, r);
    return r.ty;
}

TypeId type_clone_record(TypeTable *tt, TypeId t)
{
    TypeId ct = type_canon(tt, t);
    Record r = *type_record(tt, ct);
    TypeEnt e = *type_ent(tt, ct);
    if (!(r.flags & RF_COMPLETE) || (r.flags & RF_SSO))
        return t;
    e.extra = (uint32_t)tt->recs.len;
    e.canon = 0;
    r.ty = add_ent(tt, e);
    r.flags |= RF_SSO;
    vec_push(&tt->recs, r);
    return r.ty | TYPE_QUALS(t);
}

TypeId type_new_enum(TypeTable *tt, uint32_t tag, SrcLoc loc)
{
    Enum en = {0};
    en.tag = tag;
    en.loc = loc;
    en.underlying = TYPE_B(UINT);
    uint32_t ei = (uint32_t)tt->enums.len;
    en.ty = add_ent(tt, (TypeEnt){.kind = TY_ENUM, .extra = ei});
    vec_push(&tt->enums, en);
    return en.ty;
}

/* Reads of a record or enum's contents (layout, completeness, underlying
 * type): records older than the unit being checked are reported to the
 * summary hook (csum.c), which keeps the unit's read set. */
static inline Record *rec_rd(TypeTable *tt, uint32_t i)
{
    if (i < tt->unit_rec0)
        tt->rd_hook(tt->rd_ctx, i, false);
    return &tt->recs.data[i];
}

static inline Enum *enum_rd(TypeTable *tt, uint32_t i)
{
    if (i < tt->unit_enum0)
        tt->rd_hook(tt->rd_ctx, i, true);
    return &tt->enums.data[i];
}

Record *type_record(TypeTable *tt, TypeId t)
{
    const TypeEnt *e = type_ent(tt, tt->ents.data[TYPE_IDX(t)].canon);
    if (e->kind != TY_STRUCT && e->kind != TY_UNION)
        return NULL;
    return rec_rd(tt, e->extra);
}

Enum *type_enum(TypeTable *tt, TypeId t)
{
    const TypeEnt *e = type_ent(tt, tt->ents.data[TYPE_IDX(t)].canon);
    return e->kind == TY_ENUM ? enum_rd(tt, e->extra) : NULL;
}

/* ---- queries --------------------------------------------------------- */

static const TypeEnt *cent(const TypeTable *tt, TypeId t)
{
    return &tt->ents.data[TYPE_IDX(tt->ents.data[TYPE_IDX(t)].canon)];
}

bool type_is_complete(TypeTable *tt, TypeId t)
{
    bool ok;
    const TypeEnt *e = cent(tt, t);
    if (e->kind == TY_VLA)
        return true;
    type_size(tt, t, &ok);
    return ok;
}

bool type_is_integer(TypeTable *tt, TypeId t)
{
    TypeKind k = type_ckind(tt, t);
    return (k >= TY_BOOL && k <= TY_UINT128) || k == TY_ENUM;
}

bool type_is_signed(TypeTable *tt, TypeId t)
{
    const TypeEnt *e = cent(tt, t);
    switch (e->kind) {
    case TY_CHAR: return tt->tgt->char_signed;
    case TY_SCHAR: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
    case TY_INT128:
        return true;
    case TY_ENUM:
        return type_is_signed(tt, enum_rd(tt, e->extra)->underlying);
    default:
        return e->kind >= TY_FLOAT16 && e->kind < TY_NBUILTIN;
    }
}

bool type_is_float(TypeTable *tt, TypeId t)
{
    TypeKind k = type_ckind(tt, t);
    return k >= TY_FLOAT16 && k < TY_NBUILTIN;
}

bool type_is_arith(TypeTable *tt, TypeId t)
{
    return type_is_integer(tt, t) || type_is_float(tt, t) ||
           type_ckind(tt, t) == TY_COMPLEX;
}

bool type_is_scalar(TypeTable *tt, TypeId t)
{
    return type_is_arith(tt, t) || type_ckind(tt, t) == TY_PTR;
}

bool type_is_object(TypeTable *tt, TypeId t)
{
    return type_ckind(tt, t) != TY_FUNC;
}

bool type_is_void(TypeTable *tt, TypeId t)
{
    return type_ckind(tt, t) == TY_VOID;
}

bool type_is_record(TypeTable *tt, TypeId t)
{
    TypeKind k = type_ckind(tt, t);
    return k == TY_STRUCT || k == TY_UNION;
}

bool type_is_vm(TypeTable *tt, TypeId t)
{
    for (;;) {
        const TypeEnt *e = cent(tt, t);
        switch (e->kind) {
        case TY_VLA: return true;
        case TY_STRUCT: case TY_UNION:
            return (type_record(tt, t)->flags & RF_VMOD) != 0;
        case TY_PTR: case TY_ARRAY: t = e->base; break;
        case TY_FUNC: t = e->base; break;
        default: return false;
        }
    }
}

TypeId type_base(TypeTable *tt, TypeId t)
{
    TypeId c = type_canon(tt, t);
    (void)c;
    /* Keep the typedef-bearing spelling: walk typedefs by hand. */
    unsigned q = TYPE_QUALS(t);
    const TypeEnt *e = type_ent(tt, t);
    while (e->kind == TY_TYPEDEF) {
        q |= TYPE_QUALS(e->base);
        t = e->base;
        e = type_ent(tt, t);
    }
    switch (e->kind) {
    case TY_ARRAY: case TY_VLA:
        return e->base | q;          /* the array's qualifiers are its element's */
    case TY_PTR: case TY_FUNC: case TY_VECTOR: case TY_COMPLEX:
        return e->base;
    default:
        return TYPE_B(ERROR);
    }
}

uint64_t type_size(TypeTable *tt, TypeId t, bool *ok)
{
    const TypeEnt *e = cent(tt, t);
    const Target *tg = tt->tgt;
    bool dummy;
    if (!ok)
        ok = &dummy;
    *ok = true;
    switch (e->kind) {
    case TY_ERROR: *ok = false; return 1;
    case TY_VOID: *ok = false; return 1;        /* GNU: sizeof(void) == 1 */
    case TY_PTR: return tg->ptr_size;
    case TY_ARRAY:
        if (e->flags & TF_INCOMPLETE) {
            *ok = false;
            return 0;
        }
        return e->n * type_size(tt, e->base, ok);
    case TY_VLA: *ok = false; return 0;
    case TY_FUNC: *ok = false; return 1;        /* GNU: sizeof(f) == 1 */
    case TY_STRUCT: case TY_UNION: {
        const Record *r = rec_rd(tt, e->extra);
        if (!(r->flags & RF_COMPLETE)) {
            *ok = false;
            return 0;
        }
        return r->size;
    }
    case TY_ENUM: {
        const Enum *en = enum_rd(tt, e->extra);
        *ok = en->complete;
        return tg->size[type_ckind(tt, en->underlying)];
    }
    case TY_VECTOR: return e->n;
    case TY_COMPLEX: return 2 * type_size(tt, e->base, ok);
    default:
        if (e->kind >= sizeof tg->size / sizeof tg->size[0] ||
            !tg->size[e->kind]) {
            *ok = false;
            return 0;
        }
        return tg->size[e->kind];
    }
}

static unsigned align_of(TypeTable *tt, TypeId t, bool member)
{
    const TypeEnt *e = type_ent(tt, t);
    while (e->kind == TY_TYPEDEF) {
        if (e->flags & TF_ALIGNED)
            return e->align;
        e = type_ent(tt, e->base);
    }
    const Target *tg = tt->tgt;
    switch (e->kind) {
    case TY_ERROR: case TY_VOID: case TY_FUNC: return 1;
    case TY_PTR: return tg->ptr_align;
    case TY_ARRAY: case TY_VLA: case TY_COMPLEX:
        return align_of(tt, e->base, member);
    case TY_STRUCT: case TY_UNION: return rec_rd(tt, e->extra)->align;
    case TY_ENUM: return align_of(tt, enum_rd(tt, e->extra)->underlying, member);
    case TY_VECTOR: return (unsigned)e->n;
    default: {
        unsigned a = member ? tg->member_align[e->kind] : tg->align[e->kind];
        return a ? a : 1;
    }
    }
}

unsigned type_align(TypeTable *tt, TypeId t) { return align_of(tt, t, false); }
unsigned type_member_align(TypeTable *tt, TypeId t) { return align_of(tt, t, true); }

unsigned type_int_bits(TypeTable *tt, TypeId t)
{
    const TypeEnt *e = cent(tt, t);
    if (e->kind == TY_BOOL)
        return 1;
    if (e->kind == TY_ENUM)
        return type_int_bits(tt, enum_rd(tt, e->extra)->underlying);
    return tt->tgt->size[e->kind] * 8u;
}

int type_int_rank(TypeTable *tt, TypeId t)
{
    const TypeEnt *e = cent(tt, t);
    switch (e->kind) {
    case TY_BOOL: return 1;
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 2;
    case TY_SHORT: case TY_USHORT: return 3;
    case TY_INT: case TY_UINT: return 4;
    case TY_LONG: case TY_ULONG: return 5;
    case TY_LLONG: case TY_ULLONG: return 6;
    case TY_INT128: case TY_UINT128: return 7;
    case TY_ENUM: return type_int_rank(tt, enum_rd(tt, e->extra)->underlying);
    default: return 0;
    }
}

TypeId type_to_unsigned(TypeTable *tt, TypeId t)
{
    const TypeEnt *e = cent(tt, t);
    switch (e->kind) {
    case TY_CHAR: case TY_SCHAR: return TYPE_B(UCHAR);
    case TY_SHORT: return TYPE_B(USHORT);
    case TY_INT: return TYPE_B(UINT);
    case TY_LONG: return TYPE_B(ULONG);
    case TY_LLONG: return TYPE_B(ULLONG);
    case TY_INT128: return TYPE_B(UINT128);
    case TY_ENUM: return type_to_unsigned(tt, enum_rd(tt, e->extra)->underlying);
    default: return TYPE_UNQUAL(tt->ents.data[TYPE_IDX(t)].canon);
    }
}

TypeId type_int_promote(TypeTable *tt, TypeId t)
{
    if (!type_is_integer(tt, t))
        return t;
    if (type_int_rank(tt, t) < 4) {
        uint64_t sz = type_size(tt, t, NULL);
        if (sz < tt->tgt->size[TY_INT] || type_is_signed(tt, t))
            return TYPE_B(INT);
        return TYPE_B(UINT);
    }
    if (type_ckind(tt, t) == TY_ENUM)
        return type_canon(tt, type_enum(tt, t)->underlying);
    return TYPE_UNQUAL(type_canon(tt, t));
}

TypeId type_default_promote(TypeTable *tt, TypeId t)
{
    TypeKind k = type_ckind(tt, t);
    if (k == TY_FLOAT)
        return TYPE_B(DOUBLE);
    return type_int_promote(tt, t);
}

TypeId type_param_adjust(TypeTable *tt, TypeId t)
{
    TypeKind k = type_ckind(tt, t);
    if (k == TY_ARRAY || k == TY_VLA)
        return type_ptr(tt, type_base(tt, t));
    if (k == TY_FUNC)
        return type_ptr(tt, t);
    return t;
}

/* ---- compatibility --------------------------------------------------- */

static bool is_array_kind(TypeKind k) { return k == TY_ARRAY || k == TY_VLA; }

static bool proto_vs_noproto(TypeTable *tt, const TypeEnt *p, const TypeEnt *np)
{
    const TypeId *pp = tt->params.data + p->extra;
    if (np->n) {    /* a K&R definition: the promoted parameter types */
        if (np->n != p->n || (p->flags & TF_VARIADIC))
            return false;
        const TypeId *kp = tt->params.data + np->extra;
        for (uint64_t i = 0; i < p->n; i++)
            if (!type_compatible(tt, pp[i], type_default_promote(tt, kp[i])))
                return false;
        return true;
    }
    if (p->flags & TF_VARIADIC)
        return false;
    for (uint64_t i = 0; i < p->n; i++)
        if (!type_compatible(tt, pp[i], type_default_promote(tt, pp[i])))
            return false;
    return true;
}

/* type_lists_compatible_p: a parameter of transparent union type matches
 * the type of any of its members. */
static bool tu_param_match(TypeTable *tt, TypeId u, TypeId o)
{
    u = TYPE_UNQUAL(type_canon(tt, u));
    const TypeEnt *e = type_ent(tt, u);
    if (e->kind != TY_UNION)
        return false;
    const Record *r = &tt->recs.data[e->extra];
    if (!(r->flags & RF_TRANSPARENT))
        return false;
    for (uint32_t i = 0; i < r->nfields; i++)
        if (type_compatible(tt, TYPE_UNQUAL(tt->fields.data[r->fields + i].ty),
                            TYPE_UNQUAL(o)))
            return true;
    return false;
}

/* Compatible function types where some parameter is a transparent union on
 * one side only: gcc pedwarns that they are not truly compatible. */
bool type_tu_mixed(TypeTable *tt, TypeId a, TypeId b)
{
    a = type_canon(tt, a);
    b = type_canon(tt, b);
    if (type_kind(tt, a) != TY_FUNC || type_kind(tt, b) != TY_FUNC)
        return false;
    const TypeEnt *ea = type_ent(tt, a), *eb = type_ent(tt, b);
    if ((ea->flags | eb->flags) & TF_NOPROTO || ea->n != eb->n)
        return false;
    for (uint32_t i = 0; i < ea->n; i++) {
        TypeId x = tt->params.data[ea->extra + i], y = tt->params.data[eb->extra + i];
        bool ux = type_ckind(tt, x) == TY_UNION &&
                  (type_record(tt, type_canon(tt, x))->flags & RF_TRANSPARENT);
        bool uy = type_ckind(tt, y) == TY_UNION &&
                  (type_record(tt, type_canon(tt, y))->flags & RF_TRANSPARENT);
        if (ux != uy)
            return true;
    }
    return false;
}

bool type_compatible(TypeTable *tt, TypeId a, TypeId b)
{
    a = type_canon(tt, a);
    b = type_canon(tt, b);
    if (a == b)
        return true;
    if (TYPE_QUALS(a) != TYPE_QUALS(b))
        return false;
    const TypeEnt *ea = type_ent(tt, a), *eb = type_ent(tt, b);
    if (ea->kind == TY_ENUM && eb->kind != TY_ENUM) /* no underlying type before the '{' */
        return enum_rd(tt, ea->extra)->complete &&
               TYPE_UNQUAL(type_canon(tt, enum_rd(tt, ea->extra)->underlying))
               == TYPE_UNQUAL(b);
    if (eb->kind == TY_ENUM && ea->kind != TY_ENUM)
        return type_compatible(tt, b, a);
    if (is_array_kind(ea->kind) && is_array_kind(eb->kind)) {
        if (!type_compatible(tt, ea->base, eb->base))
            return false;
        if (ea->kind == TY_VLA || eb->kind == TY_VLA ||
            (ea->flags & TF_INCOMPLETE) || (eb->flags & TF_INCOMPLETE))
            return true;
        return ea->n == eb->n;
    }
    if (ea->kind != eb->kind)
        return false;
    switch (ea->kind) {
    case TY_PTR:
        return type_compatible(tt, ea->base, eb->base);
    case TY_FUNC: {
        if (!type_compatible(tt, ea->base, eb->base))
            return false;
        bool npa = ea->flags & TF_NOPROTO, npb = eb->flags & TF_NOPROTO;
        if (npa && npb)
            return true;
        if (npa)
            return proto_vs_noproto(tt, eb, ea);
        if (npb)
            return proto_vs_noproto(tt, ea, eb);
        if (ea->n != eb->n ||
            (ea->flags & TF_VARIADIC) != (eb->flags & TF_VARIADIC))
            return false;
        for (uint64_t i = 0; i < ea->n; i++)
        {
            TypeId pa = tt->params.data[ea->extra + i];
            TypeId pb = tt->params.data[eb->extra + i];
            if (!type_compatible(tt, pa, pb) && !tu_param_match(tt, pa, pb) &&
                !tu_param_match(tt, pb, pa))
                return false;
        }
        return true;
    }
    default:
        return false;
    }
}

TypeId type_composite(TypeTable *tt, TypeId a, TypeId b)
{
    if (type_canon(tt, a) == type_canon(tt, b) || !type_compatible(tt, a, b))
        return a;
    TypeId ca = type_canon(tt, a), cb = type_canon(tt, b);
    unsigned q = TYPE_QUALS(ca);
    /* the entities as written, so that the parts keep their typedef names
     * (gcc's composite_type recurses on TREE_TYPE of the originals) */
    TypeId pa = a, pb = b;
    while (type_ent(tt, pa)->kind == TY_TYPEDEF)
        pa = type_ent(tt, pa)->base | TYPE_QUALS(pa);
    while (type_ent(tt, pb)->kind == TY_TYPEDEF)
        pb = type_ent(tt, pb)->base | TYPE_QUALS(pb);
    if (TYPE_QUALS(pa) || TYPE_QUALS(pb) ||
        type_ent(tt, pa)->kind != type_ent(tt, ca)->kind ||
        type_ent(tt, pb)->kind != type_ent(tt, cb)->kind)
        pa = ca, pb = cb;
    TypeEnt xa = *type_ent(tt, pa), xb = *type_ent(tt, pb);
    const TypeEnt *ea = &xa, *eb = &xb;   /* ents may grow below */
    if (is_array_kind(ea->kind)) {
        TypeId el = type_composite(tt, ea->base, eb->base);
        if (ea->kind == TY_ARRAY && !(ea->flags & TF_INCOMPLETE))
            return type_array(tt, el, ea->n);
        if (eb->kind == TY_ARRAY && !(eb->flags & TF_INCOMPLETE))
            return type_array(tt, el, eb->n);
        if (ea->kind == TY_VLA || eb->kind == TY_VLA)
            return type_vla(tt, el);
        return type_array_incomplete(tt, el);
    }
    switch (ea->kind) {
    case TY_PTR:
        return type_ptr(tt, type_composite(tt, ea->base, eb->base)) | q;
    case TY_FUNC: {
        TypeId ret = type_composite(tt, ea->base, eb->base);
        bool npa = ea->flags & TF_NOPROTO, npb = eb->flags & TF_NOPROTO;
        const TypeEnt *p = npa ? eb : ea;
        if (npa && npb)
            return type_func(tt, ret, NULL, 0, TF_NOPROTO);
        uint32_t n = (uint32_t)p->n;
        TypeId *ps = xmalloc((n ? n : 1) * sizeof *ps);
        for (uint32_t i = 0; i < n; i++) {
            TypeId x = tt->params.data[ea->extra + i];
            if (npa || npb) {
                ps[i] = tt->params.data[p->extra + i];
                continue;
            }
            TypeId y = tt->params.data[eb->extra + i];
            /* gcc: of a transparent union and one of its member types the
             * member type is kept, whichever came first */
            bool ua = type_ckind(tt, x) == TY_UNION &&
                      (type_record(tt, type_canon(tt, x))->flags & RF_TRANSPARENT);
            bool ub = type_ckind(tt, y) == TY_UNION &&
                      (type_record(tt, type_canon(tt, y))->flags & RF_TRANSPARENT);
            ps[i] = ua && !ub ? y : ub && !ua ? x : type_composite(tt, x, y);
        }
        TypeId r = type_func(tt, ret, ps, n, p->flags & TF_VARIADIC);
        free(ps);
        return r;
    }
    default:
        return a;
    }
}

/* ---- layout ---------------------------------------------------------- */

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return a ? (v + a - 1) / a * a : v;
}

typedef struct {
    uint64_t size, align;    /* bits */
    bool is_union;
    uint64_t pack;           /* bits; 0 none */
    bool packed;
    /* MS: the bit-field being filled */
    bool ongoing;
    uint64_t ong_size, ong_unused;
} Layout;

static void sysv_field(TypeTable *tt, Layout *L, const FieldIn *f,
                       uint64_t tsize, uint64_t tyalign, Field *out)
{
    const Target *tg = tt->tgt;
    bool packed = L->packed || f->packed;
    if (f->width < 0) {
        uint64_t fa = packed ? 8 : tyalign;
        if (f->align)
            fa = MAX(fa, (uint64_t)f->align * 8);
        if (L->pack)
            fa = MIN(fa, L->pack);
        uint64_t off = L->is_union ? 0 : align_up(L->size, fa);
        out->off_bits = off;
        L->size = MAX(L->size, off + tsize);
        L->align = MAX(L->align, fa);
        return;
    }
    uint64_t w = (uint64_t)f->width;
    uint64_t tfa = tyalign;
    if (w > 0) {
        if (tg->ignore_nonzero_bitfield_align)
            tfa = 1;
    } else {
        if (tg->ignore_zero_bitfield_align)
            tfa = 1;
        else if (tg->min_zero_bitfield_align)
            tfa = MAX(tfa, (uint64_t)tg->min_zero_bitfield_align * 8);
    }
    uint64_t anno = f->align ? (uint64_t)f->align * 8 : 1;
    uint64_t first = L->is_union ? 0 : L->size;
    uint64_t fa;
    if (w == 0) {
        fa = MAX(tfa, anno);
    } else {
        fa = anno;
        if (L->pack)
            fa = MIN(fa, L->pack);
        if (!packed) {
            uint64_t ta = L->pack ? MIN(tfa, L->pack) : tfa;
            uint64_t start = align_up(first, fa);
            if (ta > tsize || start % ta + w > tsize)
                fa = MAX(fa, ta);
        }
    }
    uint64_t off = align_up(first, fa);
    out->off_bits = off;
    L->size = MAX(L->size, off + w);
    if (f->name || tg->unnamed_field_affects_align) {
        uint64_t ra;
        if (w == 0)
            ra = MAX(tfa, anno);
        else if (L->pack)
            ra = MIN(MAX(tfa, anno), L->pack);
        else if (packed)
            ra = anno;
        else
            ra = MAX(tfa, anno);
        L->align = MAX(L->align, ra);
    }
}

static void ms_field(Layout *L, const FieldIn *f, uint64_t tsize,
                     uint64_t tyalign, Field *out)
{
    bool packed = L->packed || f->packed;
    bool bitf = f->width >= 0;
    uint64_t w = bitf ? (uint64_t)f->width : 0;
    uint64_t anno = f->align ? (uint64_t)f->align * 8 : 8;
    bool ignore = packed ||
                  (bitf && L->ongoing && L->ong_size == tsize) ||
                  (bitf && w == 0 && !L->ongoing);
    uint64_t fa = anno;
    if (!ignore)
        fa = MAX(fa, tyalign);
    if (L->pack)
        fa = MIN(fa, L->pack);
    if (!bitf || (w == 0 && L->ongoing) || (w != 0 && !packed)) {
        uint64_t ta = packed && !(bitf && w == 0) ? 8 : tyalign;
        ta = MAX(ta, anno);
        if (L->pack)
            ta = MIN(ta, L->pack);
        L->align = MAX(L->align, ta);
    }
    if (!bitf) {
        L->ongoing = false;
        uint64_t off = L->is_union ? 0 : align_up(L->size, fa);
        out->off_bits = off;
        L->size = MAX(L->size, off + tsize);
        return;
    }
    if (L->is_union) {
        out->off_bits = 0;
        L->size = MAX(L->size, w);
        return;
    }
    if (w == 0) {
        L->ongoing = false;
    } else {
        if (L->ongoing && L->ong_size == tsize && L->ong_unused >= w) {
            out->off_bits = L->size - L->ong_unused;
            L->ong_unused -= w;
            return;
        }
        L->ongoing = true;
        L->ong_size = tsize;
        L->ong_unused = tsize - w;
    }
    uint64_t off = align_up(L->size, fa);
    out->off_bits = off;
    L->size = w == 0 ? off : off + tsize;
}

/* -Wpacked: would the record t, just completed from f[0..n) with the packed
 * attribute, lay out the same (every offset and the size) without it?  With
 * only >= 0: does field `only`, whose own packed attribute is the question,
 * sit at the same offset without it? */
bool type_packed_unnecessary(TypeTable *tt, TypeId t, const FieldIn *f,
                             uint32_t n, unsigned pack, unsigned align, int ms,
                             long only)
{
    const Record *r = type_record(tt, t);
    Layout L = {0};
    uint32_t i;
    L.align = 8;
    L.is_union = r->flags & RF_UNION;
    L.pack = (uint64_t)pack * 8;
    for (i = 0; i < n; i++) {
        Field out = {0};
        FieldIn fi = f[i];
        bool ok;
        uint64_t tsize = type_size(tt, f[i].ty, &ok) * 8;
        uint64_t tyalign = (uint64_t)type_member_align(tt, f[i].ty) * 8;
        if (!ok)
            tsize = 0;
        if (only >= 0)
            fi.packed = f[i].packed && i != (uint32_t)only;
        if (ms > 0 || (!ms && tt->tgt->ms_bitfields))
            ms_field(&L, &fi, tsize, tyalign, &out);
        else
            sysv_field(tt, &L, &fi, tsize, tyalign, &out);
        if (only >= 0 ? i == (uint32_t)only : 1)
            if (out.off_bits != tt->fields.data[r->fields + i].off_bits)
                return false;
        if (only >= 0 && i == (uint32_t)only)
            return true;
    }
    if (align)
        L.align = MAX(L.align, (uint64_t)align * 8);
    return align_up(L.size, L.align) / 8 == r->size;
}

void type_complete_record(TypeTable *tt, TypeId t, const FieldIn *f,
                          uint32_t n, unsigned pack, unsigned align,
                          bool packed, int ms)
{
    Record *r = type_record(tt, t);
    Layout L = {0};
    L.align = 8;
    L.is_union = r->flags & RF_UNION;
    L.pack = (uint64_t)pack * 8;
    L.packed = packed;
    uint32_t first = (uint32_t)tt->fields.len;
    uint16_t flags = r->flags & (RF_UNION | RF_NOKEYWORD | RF_TRANSPARENT);
    if (packed)
        flags |= RF_PACKED;
    for (uint32_t i = 0; i < n; i++)
        if (f[i].packed)
            packed = true;
    if (packed)
        tt->any_packed = true;
    if (align)
        flags |= RF_USER_ALIGN;
    for (uint32_t i = 0; i < n; i++) {
        Field out = {0};
        out.name = f[i].name;
        out.ty = f[i].ty;
        out.width = f[i].width < 0 ? 0 : (uint32_t)f[i].width;
        out.flags = (uint16_t)((f[i].width >= 0 ? FF_BITFIELD : 0) |
                               (f[i].packed ? FF_PACKED : 0));
        out.align = f[i].align;
        out.loc = f[i].loc;
        out.dep = f[i].dep;
        out.dmsg = f[i].dmsg;
        out.aset = f[i].aset;
        bool ok;
        uint64_t tsize = type_size(tt, f[i].ty, &ok) * 8;
        if (!ok) {
            tsize = 0;
            if (type_ckind(tt, f[i].ty) == TY_ARRAY)
                flags |= RF_FLEXIBLE;
            else if (type_ckind(tt, f[i].ty) == TY_VLA)
                flags |= RF_VLA;
        }
        if (ok && L.is_union && type_is_record(tt, f[i].ty) &&
            (type_record(tt, type_canon(tt, f[i].ty))->flags & RF_FLEXIBLE))
            flags |= RF_FLEXIBLE;   /* a union includes a flexible array */
        if (type_is_vm(tt, f[i].ty))
            flags |= RF_VMOD;
        uint64_t tyalign = (uint64_t)type_member_align(tt, f[i].ty) * 8;
        if (ms > 0 || (!ms && tt->tgt->ms_bitfields))
            ms_field(&L, &f[i], tsize, tyalign, &out);
        else
            sysv_field(tt, &L, &f[i], tsize, tyalign, &out);
        TypeId c = type_canon(tt, f[i].ty);
        const TypeEnt *e = type_ent(tt, c);
        while (e->kind == TY_ARRAY) {
            c = type_canon(tt, e->base);
            e = type_ent(tt, c);
        }
        if (TYPE_QUALS(c) & TQ_CONST)
            flags |= RF_CONST_MEMBER;
        if (e->kind == TY_STRUCT || e->kind == TY_UNION)
            flags |= tt->recs.data[e->extra].flags & RF_CONST_MEMBER;
        vec_push(&tt->fields, out);
    }
    if (align)
        L.align = MAX(L.align, (uint64_t)align * 8);
    r = type_record(tt, t);
    r->fields = first;
    r->nfields = n;
    r->size = align_up(L.size, L.align) / 8;
    r->align = (uint32_t)(L.align / 8);
    r->flags = (uint16_t)(flags | RF_COMPLETE);
}

/* ---- printing -------------------------------------------------------- */

static void put_ident(TypeTable *tt, StrBuf *sb, uint32_t id)
{
    const Ident *i = ident_by_id(tt->in, id);
    sb_putn(sb, i->str, i->len);
}

static void spec_quals(StrBuf *sb, unsigned q)
{
    if (q & TQ_CONST) sb_puts(sb, "const ");
    if (q & TQ_VOLATILE) sb_puts(sb, "volatile ");
    if (q & TQ_RESTRICT) sb_puts(sb, "restrict ");
    if (q & TQ_ATOMIC) sb_puts(sb, "_Atomic ");
}

static const char *const builtin_names[TY_NBUILTIN] = {
#define X(n, s) s,
    BUILTIN_TYPES(X)
#undef X
};

static void print_spec(TypeTable *tt, StrBuf *sb, TypeId t)
{
    const TypeEnt *e = type_ent(tt, t);
    spec_quals(sb, TYPE_QUALS(t));
    switch (e->kind) {
    case TY_STRUCT: case TY_UNION: case TY_ENUM: {
        uint32_t tag;
        if (e->kind == TY_ENUM) {
            tag = tt->enums.data[e->extra].tag;
            sb_puts(sb, "enum ");
        } else {
            const Record *r = &tt->recs.data[e->extra];
            tag = r->tag;
            if (!(r->flags & RF_NOKEYWORD))
                sb_puts(sb, e->kind == TY_UNION ? "union " : "struct ");
        }
        if (tag)
            put_ident(tt, sb, tag);
        else
            sb_puts(sb, "<anonymous>");
        break;
    }
    case TY_TYPEDEF:
        put_ident(tt, sb, e->extra);
        break;
    case TY_COMPLEX:
        {   /* gcc names only the signed complex types; unsigned ones print
             * with the keyword */
            const TypeEnt *b = type_ent(tt, e->base);
            bool u = b->kind < TY_NBUILTIN &&
                     strstr(builtin_names[b->kind], "unsigned");
            sb_puts(sb, u ? "_Complex " : "complex ");
        }
        print_spec(tt, sb, e->base);
        break;
    case TY_VECTOR: {
        uint64_t es = type_size(tt, e->base, NULL);
        sb_printf(sb, "__vector(%llu) ",
                  (unsigned long long)(es ? e->n / es : 0));
        type_print(tt, sb, e->base);
        break;
    }
    default:
        sb_puts(sb, e->kind < TY_NBUILTIN ? builtin_names[e->kind] : "?");
        break;
    }
}

enum { D_EMPTY, D_PTR, D_PAREN, D_SUFFIX };

/* gcc's {aka ...} strips typedefs but stops at `typedef struct S S;` and
 * `typedef struct {...} S;`, which keep their name. */
static bool aka_atomic(const TypeTable *tt, const TypeEnt *e)
{
    const TypeEnt *b = type_ent(tt, e->base);
    /* a system header's typedef of a tagged type keeps its name too */
    if (b->kind == TY_STRUCT || b->kind == TY_UNION)
        return (e->flags & TF_SYSHDR) || !tt->recs.data[b->extra].tag ||
               tt->recs.data[b->extra].tag == e->extra;
    if (b->kind == TY_ENUM)
        return (e->flags & TF_SYSHDR) || !tt->enums.data[b->extra].tag ||
               tt->enums.data[b->extra].tag == e->extra;
    return false;
}

void type_print(TypeTable *tt, StrBuf *sb, TypeId t)
{
    StrBuf d = {0}, tmp = {0};
    int k = D_EMPTY;
    for (;;) {
        const TypeEnt *e = type_ent(tt, t);
        unsigned q = TYPE_QUALS(t);
        if (tt->aka && e->kind == TY_TYPEDEF && !aka_atomic(tt, e)) {
            t = e->base | q;
            continue;
        }
        if (k != D_EMPTY && e->kind == TY_TYPEDEF) {
            /* gcc names a typedef of a derived type only as the whole
             * type; inside a declarator it is spelled out */
            TypeKind bk = type_ckind(tt, e->base);
            if (bk == TY_PTR || bk == TY_ARRAY || bk == TY_VLA ||
                bk == TY_FUNC) {
                t = e->base | q;
                continue;
            }
        }
        if (e->kind == TY_PTR) {
            tmp.len = 0;
            sb_putc(&tmp, '*');
            if (q & TQ_CONST) sb_puts(&tmp, " const");
            if (q & TQ_VOLATILE) sb_puts(&tmp, " volatile");
            if (q & TQ_RESTRICT) sb_puts(&tmp, " restrict");
            if (q & TQ_ATOMIC) sb_puts(&tmp, " _Atomic");
            if (k == D_PAREN)
                sb_putc(&tmp, ' ');
            sb_putn(&tmp, d.data, d.len);
            StrBuf x = d; d = tmp; tmp = x;
            k = D_PTR;
            t = e->base;
            continue;
        }
        if (e->kind == TY_ARRAY || e->kind == TY_VLA || e->kind == TY_FUNC) {
            if (k == D_PTR) {
                tmp.len = 0;
                sb_putc(&tmp, '(');
                sb_putn(&tmp, d.data, d.len);
                sb_putc(&tmp, ')');
                StrBuf x = d; d = tmp; tmp = x;
                k = D_PAREN;
            } else if (k == D_EMPTY) {
                k = D_SUFFIX;
            }
            if (e->kind == TY_ARRAY) {
                if (e->flags & TF_INCOMPLETE)
                    sb_puts(&d, "[]");
                else
                    sb_printf(&d, "[%llu]", (unsigned long long)e->n);
                t = e->base | q;
            } else if (e->kind == TY_VLA) {
                sb_puts(&d, "[*]");
                t = e->base | q;
            } else {
                sb_putc(&d, '(');
                const TypeId *p = tt->params.data + e->extra;
                if (!(e->flags & TF_NOPROTO)) {
                    if (!e->n && !(e->flags & TF_VARIADIC))
                        sb_puts(&d, "void");
                    for (uint64_t i = 0; i < e->n; i++) {
                        /* gcc separates with two spaces after a parameter
                         * that ends in a specifier word */
                        if (i)
                            sb_puts(&d, (isalnum((unsigned char)d.data[d.len - 1]) ||
                                         d.data[d.len - 1] == '_')
                                            ? ",  " : ", ");
                        type_print(tt, &d, p[i]);
                    }
                    if (e->flags & TF_VARIADIC)
                        sb_puts(&d, e->n ? ", ..." : "...");
                }
                sb_putc(&d, ')');
                t = e->base;
            }
            continue;
        }
        break;
    }
    print_spec(tt, sb, t);
    if (k == D_PTR || k == D_PAREN)
        sb_putc(sb, ' ');
    sb_putn(sb, d.data, d.len);
    sb_free(&d);
    sb_free(&tmp);
}

void type_quote(TypeTable *tt, StrBuf *sb, TypeId t)
{
    size_t start = sb->len;
    sb_putc(sb, '\'');
    type_print(tt, sb, t);
    sb_putc(sb, '\'');
    size_t end = sb->len;
    TypeId c = tt->ents.data[TYPE_IDX(t)].canon | TYPE_QUALS(t);
    if (c == t)
        return;
    sb_puts(sb, " {aka '");
    size_t aka = sb->len;
    tt->aka = true;
    type_print(tt, sb, t);
    tt->aka = false;
    if (sb->len - aka == end - start - 2 &&
        !memcmp(sb->data + aka, sb->data + start + 1, end - start - 2))
    {
        sb->len = end;
        sb->data[end] = 0;
    } else
        sb_puts(sb, "'}");
}

const char *type_q(TypeTable *tt, TypeId t)
{
    StrBuf *sb = &tt->qbuf[tt->qnext++ % ARRAY_LEN(tt->qbuf)];
    sb->len = 0;
    type_quote(tt, sb, t);
    return sb_cstr(sb);
}

void type_dump_record(TypeTable *tt, StrBuf *sb, TypeId t)
{
    Record *r = type_record(tt, t);
    type_print(tt, sb, t);
    if (!r || !(r->flags & RF_COMPLETE)) {
        sb_puts(sb, " (incomplete)\n");
        return;
    }
    sb_printf(sb, " size=%llu align=%u%s\n", (unsigned long long)r->size,
              r->align, r->flags & RF_FLEXIBLE ? " flexible" : "");
    for (uint32_t i = 0; i < r->nfields; i++) {
        const Field *f = &tt->fields.data[r->fields + i];
        sb_printf(sb, "  %llu", (unsigned long long)(f->off_bits / 8));
        if (f->flags & FF_BITFIELD)
            sb_printf(sb, ".%u:%u", (unsigned)(f->off_bits % 8), f->width);
        sb_putc(sb, ' ');
        if (f->name)
            put_ident(tt, sb, f->name);
        else
            sb_puts(sb, "<unnamed>");
        sb_puts(sb, ": ");
        type_print(tt, sb, f->ty);
        sb_putc(sb, '\n');
    }
}
