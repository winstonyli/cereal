/* alias.c - alias sets for -Wstrict-aliasing.  gcc's alias.c keeps a graph of
 * sets; the warning only asks whether two types' sets conflict, so a set is
 * the canonical type that names it and "holds" is a walk over the members.
 * Probed against gcc-13 (docs/STATUS.md, Round 24). */
#include "c/type.h"

/* The type that names t's set; 0 for the character types (set 0, which
 * conflicts with every set).  Signedness, qualifiers, typedefs and array or
 * vector wrappers do not change a set; an enum shares its integer's; every
 * pointer to a chain ending in void is void *. */
static TypeId strip_agg(TypeTable *tt, TypeId t)
{
    for (;;) {
        TypeKind k;
        t = TYPE_UNQUAL(type_canon(tt, t));
        k = type_kind(tt, t);
        if (k != TY_ARRAY && k != TY_VLA && k != TY_VECTOR)
            return t;
        t = type_base(tt, t);
    }
}

static TypeId ptr_key(TypeTable *tt, TypeId t)
{
    TypeId p = strip_agg(tt, type_base(tt, t));
    TypeKind k = type_kind(tt, p);
    if (k == TY_PTR)
        p = ptr_key(tt, p);
    else if (k == TY_CHAR || k == TY_SCHAR || k == TY_UCHAR)
        p = TYPE_B(CHAR);
    else if ((k >= TY_SHORT && k <= TY_UINT128) || k == TY_ENUM)
        p = type_to_unsigned(tt, p);
    return type_ptr(tt, p);
}

/* int ** ends in int; void ** in void. */
static bool ends_void(TypeTable *tt, TypeId t)
{
    while (type_kind(tt, t = strip_agg(tt, t)) == TY_PTR)
        t = type_base(tt, t);
    return type_kind(tt, t) == TY_VOID;
}

static TypeId set_key(TypeTable *tt, TypeId t)
{
    TypeKind k;
    t = strip_agg(tt, t);
    k = type_kind(tt, t);
    switch (k) {
    case TY_VOID: case TY_CHAR: case TY_SCHAR: case TY_UCHAR:
        return 0;
    case TY_ENUM: case TY_SHORT: case TY_INT: case TY_LONG: case TY_LLONG:
    case TY_INT128:
        return type_to_unsigned(tt, t);
    case TY_PTR:
        return ends_void(tt, t) ? type_ptr(tt, TYPE_B(VOID)) : ptr_key(tt, t);
    default:
        return t;
    }
}

static bool holds(TypeTable *tt, TypeId outer, TypeId key, int depth)
{
    TypeKind k;
    outer = strip_agg(tt, outer);
    k = type_kind(tt, outer);
    if (set_key(tt, outer) == key)
        return true;
    if (k == TY_COMPLEX)
        return set_key(tt, type_base(tt, outer)) == key;
    if (k == TY_PTR)       /* void * holds every pointer set */
        return set_key(tt, outer) == type_ptr(tt, TYPE_B(VOID)) &&
               type_kind(tt, key) == TY_PTR;
    if ((k == TY_STRUCT || k == TY_UNION) && depth < 16) {
        const Record *r = type_record(tt, outer);
        uint32_t f;
        if (!r || !(r->flags & RF_COMPLETE))
            return false;
        for (f = 0; f < r->nfields; f++)
            if (holds(tt, tt->fields.data[r->fields + f].ty, key, depth + 1))
                return true;
    }
    return false;
}

/* A record with a character member has set 0 as a child: as the object it
 * conflicts with everything (as the target it does not). */
static bool zero_child(TypeTable *tt, TypeId t, int depth)
{
    TypeKind k;
    t = strip_agg(tt, t);
    k = type_kind(tt, t);
    if ((k == TY_STRUCT || k == TY_UNION) && depth < 16) {
        const Record *r = type_record(tt, t);
        uint32_t f;
        if (!r || !(r->flags & RF_COMPLETE))
            return false;
        for (f = 0; f < r->nfields; f++) {
            TypeId ft = tt->fields.data[r->fields + f].ty;
            if (set_key(tt, ft) == 0)
                return true;
            if (zero_child(tt, ft, depth + 1))
                return true;
        }
    }
    return false;
}

int type_alias_rel(TypeTable *tt, TypeId a, TypeId b)
{
    TypeId ka = set_key(tt, a), kb = set_key(tt, b);
    if (ka == kb || !ka || !kb)
        return AL_SAME;
    if (zero_child(tt, a, 0) || holds(tt, a, kb, 0) || holds(tt, b, ka, 0))
        return AL_MAY;
    return AL_DISJOINT;
}

bool type_ptr_may_alias(TypeTable *tt, TypeId p)
{
    TypeId t;
    /* the pointer as written: its target keeps its typedef */
    while (type_kind(tt, p) == TY_TYPEDEF)
        p = type_ent(tt, p)->base;
    if (type_kind(tt, p) != TY_PTR)
        return false;
    t = type_base(tt, p);
    for (;;) {
        const TypeEnt *e = type_ent(tt, t);
        if (e->kind == TY_TYPEDEF) {
            if (e->flags & TF_MAYALIAS)
                return true;
            t = e->base;
            continue;
        }
        if (e->kind == TY_STRUCT || e->kind == TY_UNION) {
            const Record *r = type_record(tt, t);
            return r && (r->flags & RF_MAYALIAS);
        }
        return false;
    }
}
