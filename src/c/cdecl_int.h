/* cdecl_int.h - what cdecl.c, cattr.c, cmerge.c and crecord.c share: the
 * node and token readers, the Kids list, the type and symbol predicates, the
 * GDecl record, and the prototypes that cross between those files. */
#ifndef CEREAL_CDECL_INT_H
#define CEREAL_CDECL_INT_H

#include "c/check_int.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)

enum { DC_NORMAL, DC_FIELD, DC_PARM, DC_TYPENAME };

/* ---- declarator analysis -------------------------------------------------- */

enum { GD_NONE, GD_VAR, GD_PARM, GD_FIELD, GD_FUNC, GD_TYPEDEF, GD_TYPENAME };

typedef struct GDecl {
    int what;
    uint32_t name;           /* ident, 0: none */
    uint32_t name_node;
    SrcLoc loc;              /* the declaration's own location */
    TypeId ty;
    CSym s;
    bool array_param;
    TypeId pre;              /* DC_PARM: the array type before it decayed */
    int width;               /* fields: bits, -1: not a bit-field */
    bool default_int;
    bool funcdef_ok;
} GDecl;

/* The minimum precision of v in a type of signedness uns (gcc's
 * tree_int_cst_min_precision). */
static inline unsigned min_prec(uint64_t v, bool neg, bool uns)
{
    unsigned n = 0;
    if (neg) {
        uint64_t m = ~v;
        while (m) {
            n++;
            m >>= 1;
        }
        return n + 1;
    }
    if (v == 0)
        return 1;
    while (v) {
        n++;
        v >>= 1;
    }
    return n + (uns ? 0 : 1);
}

static inline bool is_incomplete_array(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ARRAY &&
           (type_ent(TT, type_canon(TT, t))->flags & TF_INCOMPLETE);
}

/* ---- symbol predicates ---------------------------------------------------- */

static inline bool sym_public(const CSym *s)
{
    return s->linkage == LK_EXTERNAL;
}

static inline bool sym_external(const CSym *s)
{
    return (s->flags & (CSF_DECL_EXTERNAL | CSF_IMPLICIT)) != 0;
}

static inline bool sym_defined(const CSym *s)
{
    return (s->flags & CSF_DEFINED) != 0;
}

/* DECL_FILE_SCOPE_P: the symbol is a persistent one. */
static inline bool ref_file_scope(uint32_t ref)
{
    return !(ref & SYM_LOCAL);
}

static inline bool extern_inline(const CSym *s)
{
    return (s->flags & CSF_INLINE) && sym_external(s);
}

static inline const char *sname(Checker *c, const CSym *s)
{
    return cident(c, s->name);
}

/* A pedantic pedwarn about node/token tok, off under __extension__. */
static inline bool in_extension(Checker *c, uint32_t node)
{
    return node != NO_NODE && cexpr_in_extension(c, node);
}

static inline unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static inline const Tok *tokp(const Checker *c, uint32_t tok)
{
    return &c->u->toks[tok].t;
}

static inline int tckw(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return CK_NONE;
    t = tokp(c, tok);
    if (t->kind != TK_IDENT)
        return CK_NONE;
    return ident_by_id(c->in, t->aux)->ckw & 0xFF;
}

static inline const char *tstr(const Checker *c, uint32_t tok)
{
    const Tok *t = tokp(c, tok);
    if (t->kind == TK_IDENT)
        return ident_by_id(c->in, t->aux)->str;
    return "";
}

typedef struct Kids {
    uint32_t buf[24];
    uint32_t *p;
    uint32_t n;
} Kids;

static inline void kids_get(const Checker *c, uint32_t i, Kids *k)
{
    uint32_t n = node_children(c->nodes, i, k->buf, 24);
    k->p = k->buf;
    if (n > 24) {
        k->p = xmalloc(n * sizeof *k->p);
        node_children(c->nodes, i, k->p, n);
    }
    k->n = n;
}

static inline void kids_free(Kids *k)
{
    if (k->p != k->buf)
        free(k->p);
}

/* The last child of i (NO_NODE if none). */
static inline uint32_t last_child(const Checker *c, uint32_t i)
{
    return c->nodes[i].size > 1 ? i - 1 : NO_NODE;
}

/* The first child of i. */
static inline uint32_t first_child(const Checker *c, uint32_t i)
{
    uint32_t k, f = cfirst(c, i);
    if (c->nodes[i].size <= 1)
        return NO_NODE;
    k = i - 1;
    while (cfirst(c, k) > f)
        k = cfirst(c, k) - 1;
    return k;
}

/* The declarator inside a PTR/ARRAY/FUNC node (NO_NODE: abstract). */
static inline uint32_t inner_decl(const Checker *c, uint32_t i)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (is_declarator_tag(ntag(c, k.p[j]))) {
            r = k.p[j];
            break;
        }
    kids_free(&k);
    return r;
}


/* ---- cattr.c ---- */

enum { AC_T = 1, AC_G = 2, AC_L = 4, AC_S = 8, AC_P = 16, AC_F = 32, AC_ALL = 63,
       AC_TLS = 64, AC_PUB = 128 };    /* ... / is externally visible */    /* the object has thread storage duration */

typedef struct AttrState {
    bool pure, cnst;
    bool pure_from, cnst_from;
    SrcLoc pure_loc, cnst_loc;
    uint64_t nonnull;
    bool inited;             /* the declared name looked up (a summary read) */
    uint32_t pset;           /* the previous declaration's attribute names */
    bool hasprev;
    SrcLoc prevloc;
    char cur[24][24];        /* the attributes this declaration kept so far */
    unsigned ncur;
    bool ign_packed, ign_aligned;
    bool copy_set;           /* the attribute being checked comes from copy */
    SrcLoc copy_loc;
    uint32_t calign, palign;
} AttrState;

void acc_chain_implied(Checker *c, uint32_t ref);
void acc_implied(Checker *c, uint32_t ref, bool redecl, bool def);
void acc_start(uint32_t name, SrcLoc loc);
void aset_add(Checker *c, uint32_t *set, const char *name,
    const char *arg);
void aset_drop(Checker *c, uint32_t set, const char *name);
void aset_keep_max_aligned(Checker *c, uint32_t set);
TypeId attr_apply_type(Checker *c, TypeId t, const Attrs *a);
void attr_collect(Checker *c, uint32_t attr, Attrs *a);
bool attr_known(const char *name);
void attr_norm(const char *s, char *out, size_t n);
bool attr_scope_of(Checker *c, uint32_t tok);
void attrs_alloc_check(Checker *c, uint32_t holder, TypeId fty,
    uint32_t tok);
void attrs_copy_check(Checker *c, uint32_t holder, uint32_t kind,
    uint32_t name, uint32_t tok, AttrState *st);
void attrs_ctx_check(Checker *c, uint32_t holder, TypeId ty, uint32_t tok,
    unsigned ctx);
bool attrs_item_named(Checker *c, uint32_t holder, const char *want);
void attrs_merge(Attrs *to, const Attrs *from);
void attrs_misapplied(Checker *c, const Attrs *a, char where, bool local,
    TypeId fty, uint32_t tok);
void attrs_names(Checker *c, uint32_t holder, uint32_t *set);
void attrs_names_ptrs(Checker *c, uint32_t h, uint32_t *set);
void attrs_of_children(Checker *c, uint32_t i, Attrs *a);
void attrs_section_check(Checker *c, const Attrs *a, char where,
    bool localvar, uint32_t name, SrcLoc nloc);
void attrs_unknown_emit(Checker *c, const Attrs *sa, uint32_t tok);
void attrs_wina_check(Checker *c, const Attrs *a, char where,
    bool bitfield, uint32_t name, SrcLoc nloc);
void attrs_zcur_check(Checker *c, const Attrs *a, bool fn, SrcLoc nloc);
uint32_t check_user_alignment(Checker *c, uint32_t e, SrcLoc loc,
    bool objfile);
bool decl_has_dep_attr(Checker *c, uint32_t d);
void gnu_attr_argc(Checker *c, uint32_t attr);
TypeId int_of_size(Checker *c, unsigned bytes, bool uns);
void sso_check(Checker *c, uint32_t attr);
uint8_t sso_of_tag(Checker *c, uint32_t n);
void std_attr_unknown(Checker *c, uint32_t attr);
void strict_flex_check(Checker *c, uint32_t holder, bool field,
    TypeId ty, uint32_t name, SrcLoc loc,
    uint32_t tok0);

/* Set around the attribute checks of one declaration (cdecl.c sets them,
 * cattr.c reads them). */
extern bool alloc_via_ptr;
extern uint32_t alloc_name;
extern const char *ctx_vname;
extern CImplied imp_l[16];
extern unsigned imp_n;
extern uint32_t imp_name;

/* cspec.c */
void specs_visit(Checker *c, uint32_t i);
void cxx_typedef_in_struct(Checker *c, uint32_t ident, uint32_t tok);

bool is_rec(Checker *c, TypeId t);
uint32_t find_child(Checker *c, uint32_t i, unsigned tag);

SrcLoc iloc(Checker *c, uint32_t L);
void iloc_event(Checker *c, uint32_t tok);

/* cmerge.c: duplicate declarations. */
bool cdecl_duplicate_decls(Checker *c, CSym *nw, bool nfile, uint32_t oldref,
                           bool implicit_int);
void cdecl_inline_given(Checker *c, const CSym *s, bool is_inline,
                        uint32_t sn, uint32_t idecl);
void cdecl_inline_follows(Checker *c, const CSym *nw, uint32_t ltok,
                          uint32_t sn, uint32_t idecl);
void cdecl_locate_old_decl(Checker *c, Diagnostic *d, const CSym *o);
void cdecl_weak_apply(Checker *c, CSym *s, bool is_inline);
TypeId cdecl_typedef_under(Checker *c, TypeId t);

/* cdecl.c: the declaration core the tag and member code calls. */
void cdecl_grok(Checker *c, const Spec *sp, uint32_t top, int ctx,
                bool funcdef, bool initialized, uint32_t width_node,
                uint32_t ltok, uint32_t after, GDecl *g);
uint32_t cdecl_pushdecl(Checker *c, const CSym *xin, bool implicit_int);
int cdecl_find_spec(Checker *c, uint32_t specs_node);
void cdecl_pop_specs(Checker *c, uint32_t consumer);
void cdecl_pending_xref(Checker *c, Spec *sp);
void cdecl_shadow_tag(Checker *c, Spec *sp, int warned, uint32_t ltok);
void cdecl_dep_spec_use(Checker *c, const Spec *sp);
uint32_t cdecl_scan_end(Checker *c, uint32_t tok, bool eq);

/* crecord.c: structs, unions, enums, members. */
void cdecl_tag_visit(Checker *c, uint32_t i);
void cdecl_open_visit(Checker *c, uint32_t i);
void cdecl_struct_visit(Checker *c, uint32_t i);
void cdecl_enumerator_visit(Checker *c, uint32_t i);
void cdecl_member_visit(Checker *c, uint32_t i);
void cdecl_member_decl_visit(Checker *c, uint32_t i);
void cdecl_static_assert_visit(Checker *c, uint32_t i);
void cdecl_pragma_visit(Checker *c, uint32_t i);
bool cdecl_flex_struct(Checker *c, TypeId t);
void cdecl_struct_semis(Checker *c, uint32_t upto);
void cdecl_typedef_tag_clash(Checker *c, uint32_t name, SrcLoc at, SrcLoc old);

#endif
