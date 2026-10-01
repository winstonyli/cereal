/* cdecl.c - the checker's declarations: specifiers, declarators, structs,
 * unions, enums, functions (a port of gcc 13's c-decl.cc and the parts of
 * c-parser.cc that decide where its diagnostics point).  Expressions are
 * cexpr.c's; the walk and the scopes are check.c's (check_int.h).
 *
 * The walk visits the nodes of a unit in post-order.  What each visit does:
 *   SPECS        builds the declaration specifiers (gcc's c_declspecs) and
 *                pushes them on Checker.specs; the consumer (DECL, FUNC_DEF,
 *                PARAM, TYPE_NAME, MEMBER_DECL) pops them
 *   STRUCT/ENUM  the tag type (start_struct/finish_struct at OPEN/here)
 *   FUNC         get_parm_info: checks on the parameter list
 *   DECLARED     start_decl: the declarator is analysed (grokdeclarator) and
 *                the declared name enters its scope
 *   INIT_DECL    finish_decl: the initializer completes an array type, ...
 *   MEMBER       grokfield
 *   TYPE_NAME    groktypename (result in Checker.ty[node])
 *
 * Side arrays (Checker.ty/cv/cb/ef) are used per tag:
 *   STRUCT/ENUM  ty: the tag's type; cv: TypeSpecKind;
 *                ef bit 1: the tag was first declared in a prototype scope
 *   TYPE_NAME    ty: the type
 *   PARAM        ty: the type in the function type (adjusted; error type if
 *                erroneous); cb: symbol ref + 1 of the named parameter;
 *                cv: bit 0 named, bit 1 register
 *   FUNC         cv: bit 0 (void) only, bit 1 error in the list
 *   DECLARED     ty: the declared type; cb: symbol ref + 1
 *
 * Symbols: one CSym per entity.  Besides the CSF_* of check_int.h, flags use
 * two private bits: CSF_DECL_EXTERNAL and CSF_TREE_STATIC (gcc's
 * DECL_EXTERNAL/TREE_STATIC); TREE_PUBLIC is linkage == LK_EXTERNAL.
 * Symbols with external linkage (and declarations of externals at block
 * scope) are persistent (gsyms); the rest live for the unit (lsyms).
 *
 * --dump-types prints, per line:
 *   typedef NAME = TYPE
 *   var NAME: TYPE [static|extern|tentative]
 *   func NAME: TYPE [static|extern|inline|defined]
 *   enumconst NAME = VALUE (TYPE)
 *   record layouts, as type_dump_record prints them, when a struct or union
 *   is completed.
 * Names of block-scope entities are printed with the function's name and
 * a colon before them ("f:x"). */
#include "c/check_int.h"

#include <inttypes.h>
#include <string.h>

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)

enum { DC_NORMAL, DC_FIELD, DC_PARM, DC_TYPENAME };

/* ---- small helpers ------------------------------------------------------ */

static unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static const Tok *tokp(const Checker *c, uint32_t tok)
{
    return &c->u->toks[tok].t;
}

static int tpunct(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return P_NONE;
    t = tokp(c, tok);
    return t->kind == TK_PUNCT ? t->punct : P_NONE;
}

static int tckw(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return CK_NONE;
    t = tokp(c, tok);
    if (t->kind != TK_IDENT)
        return CK_NONE;
    return ident_by_id(c->in, t->aux)->ckw & 0xFF;
}

static const char *tstr(const Checker *c, uint32_t tok)
{
    const Tok *t = tokp(c, tok);
    if (t->kind == TK_IDENT)
        return ident_by_id(c->in, t->aux)->str;
    return "";
}

static bool tfrom_macro(const Checker *c, uint32_t tok)
{
    return c->u->toks[tok].exp != 0;
}

static SrcLoc tloc(const Checker *c, uint32_t tok)
{
    if (tok >= c->u->ntoks)
        return c->last_bol;
    return ctok_loc(c, tok);
}

typedef struct Kids {
    uint32_t buf[24];
    uint32_t *p;
    uint32_t n;
} Kids;

static void kids_get(const Checker *c, uint32_t i, Kids *k)
{
    uint32_t n = node_children(c->nodes, i, k->buf, 24);
    k->p = k->buf;
    if (n > 24) {
        k->p = xmalloc(n * sizeof *k->p);
        node_children(c->nodes, i, k->p, n);
    }
    k->n = n;
}

static void kids_free(Kids *k)
{
    if (k->p != k->buf)
        free(k->p);
}

/* The last child of i (NO_NODE if none). */
static uint32_t last_child(const Checker *c, uint32_t i)
{
    return c->nodes[i].size > 1 ? i - 1 : NO_NODE;
}

/* The first child of i. */
static uint32_t first_child(const Checker *c, uint32_t i)
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
static uint32_t inner_decl(const Checker *c, uint32_t i)
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

/* First / last token of a subtree. */
static uint32_t first_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = c->nodes[i].tok;
    for (k = cfirst(c, i); k < i; k++)
        if (c->nodes[k].tok < m)
            m = c->nodes[k].tok;
    return m;
}

static uint32_t last_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = 0;
    for (k = cfirst(c, i); k <= i; k++) {
        uint32_t t = c->nodes[k].tok;
        if (c->nodes[k].tag == N_STRING && c->nodes[k].aux)
            t += c->nodes[k].aux - 1u;
        if (t > m)
            m = t;
    }
    return m;
}

/* The token after subtree i, closing brackets included. */
static uint32_t after_tok(const Checker *c, uint32_t i)
{
    uint32_t f = first_tok(c, i), l = last_tok(c, i), k;
    int depth = 0;
    for (k = f; k <= l && k < c->u->ntoks; k++)
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE: depth++; break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE: depth--; break;
        default: break;
        }
    k = l + 1;
    while (depth > 0 && k < c->u->ntoks) {
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE: depth++; break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE: depth--; break;
        default: break;
        }
        k++;
    }
    return k;
}

/* ---- gcc's input_location ---------------------------------------------- */

/* The parser set input_location to token tok itself (a tag name, an
 * enumerator, a parse error). */
static void iloc_event(Checker *c, uint32_t tok)
{
    uint64_t a = c->u->first_tok + tok + 1;
    if (a > c->iloc_tok)
        c->iloc_tok = a;
}

/* input_location while the parser's lookahead is token L. */
static SrcLoc iloc(Checker *c, uint32_t L)
{
    uint32_t b;
    if (!c->u->ntoks)
        return c->last_bol;
    if (L >= c->u->ntoks)
        L = c->u->ntoks - 1;
    for (b = L + 1; b-- > 0;)
        if (c->u->toks[b].t.flags & TF_BOL)
            break;
    if (c->iloc_tok > c->u->first_tok) {
        uint64_t t = c->iloc_tok - 1 - c->u->first_tok;
        if (t <= L && (b == UINT32_MAX || t >= b))
            return ctok_loc(c, (uint32_t)t);
    }
    return cinput_loc(c, L);
}

SrcLoc cdecl_iloc(Checker *c, uint32_t tok)
{
    return iloc(c, tok);
}

/* A pedantic pedwarn about node/token tok, off under __extension__. */
static bool in_extension(Checker *c, uint32_t node)
{
    return node != NO_NODE && cexpr_in_extension(c, node);
}

/* ---- attributes ---------------------------------------------------------- */

/* The attribute's name without the leading and trailing "__". */
static void attr_norm(const char *s, char *out, size_t n)
{
    size_t len = strlen(s);
    if (len > 4 && s[0] == '_' && s[1] == '_' && s[len - 1] == '_' &&
        s[len - 2] == '_') {
        s += 2;
        len -= 4;
    }
    if (len >= n)
        len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}

static bool is_pow2(uint64_t v)
{
    return v && !(v & (v - 1));
}

/* check_user_alignment for the constant expression node e: the alignment
 * in bytes (0: none, silently ignored or already diagnosed).  loc: where
 * gcc's error() points; objfile: the attribute form (gcc's caller passes
 * true), which words the upper bound as the object file maximum. */
static uint32_t check_user_alignment(Checker *c, uint32_t e, SrcLoc loc,
                                     bool objfile)
{
    TypeId t;
    int64_t v;
    if (c->ck[e] == K_ERR)
        return 0;
    t = c->ty[e];
    if (!(c->ck[e] == K_ICE || (c->ck[e] == K_FOLD && (c->ef[e] & EF_CST))) ||
        !type_is_integer(TT, t)) {
        cerror(c, loc, "requested alignment is not an integer constant");
        return 0;
    }
    v = cexpr_sval(c, e);
    if (v == 0)
        return 0;
    if (v < 0 || !is_pow2((uint64_t)v)) {
        cerror(c, loc, "requested alignment '%lld' is not a positive power "
               "of 2", (long long)v);
        return 0;
    }
    if (objfile && v > ((int64_t)1 << 28)) {
        cerror(c, loc, "requested alignment '%lld' exceeds object file "
               "maximum %u", (long long)v, 1U << 28);
        return 0;
    }
    if (v >= ((int64_t)1 << 29)) {
        cerror(c, loc, "requested alignment '%lld' exceeds maximum %u",
               (long long)v, 1U << 28);
        return 0;
    }
    return (uint32_t)v;
}

/* handle_*_attribute argument counts: "wrong number of arguments", at
 * input_location. */
static void gnu_attr_argc(Checker *c, uint32_t attr)
{
    static const struct { const char *n; uint32_t lo, hi; } t[] = {
        {"alloc_align", 1, 1}, {"assume_aligned", 1, 2}, {"copy", 1, 1}};
    Kids k;
    uint32_t j;
    if (tokp(c, cnode(c, attr)->tok)->kind == TK_PUNCT)
        return;
    uint32_t at = c->nodes[attr].tok;
    Kids sib;
    uint32_t up = attr, lv;
    for (lv = 0; lv < 3 && up != NO_NODE && at == c->nodes[attr].tok; lv++) {
        up = c->par[up];
        if (up == NO_NODE)
            break;
        kids_get(c, up, &sib);     /* a leading attribute: applied at the declarator */
        for (j = 0; j < sib.n; j++)
            if (sib.p[j] > attr && (ntag(c, sib.p[j]) == N_DECLARED ||
                                    ntag(c, sib.p[j]) == N_INIT_DECL)) {
                at = c->nodes[cfirst(c, sib.p[j])].tok;
                break;
            }
        kids_free(&sib);
    }
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        char name[48];
        Kids ak;
        size_t n;
        if (ntag(c, k.p[j]) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[k.p[j]].tok), name, sizeof name);
        kids_get(c, k.p[j], &ak);
        for (n = 0; n < sizeof t / sizeof *t; n++)
            if (!strcmp(name, t[n].n) && (ak.n < t[n].lo || ak.n > t[n].hi))
                cerror(c, cinput_loc(c, at),
                       "wrong number of arguments specified for '%s' "
                       "attribute", name);
        kids_free(&ak);
    }
    kids_free(&k);
}

/* gcc's c_parser_std_attribute: a name without a namespace that is not one
 * of the standard attributes is pedwarned and dropped, at input_location
 * with the lookahead just past the name and its arguments.  (Before C2X
 * 'ns::name' is not parsed: 'ns' is such a name.) */
static void std_attr_unknown(Checker *c, uint32_t attr)
{
    static const char *const known[] = {"deprecated", "fallthrough",
        "maybe_unused", "nodiscard", "noreturn", "_Noreturn"};
    Kids k;
    uint32_t j;
    if (tokp(c, cnode(c, attr)->tok)->kind != TK_PUNCT)
        return;
    if (c->par[attr] != NO_NODE && ntag(c, c->par[attr]) == N_ENUMERATOR)
        iloc_event(c, cnode(c, c->par[attr])->tok);
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        char name[48];
        size_t n;
        if (ntag(c, k.p[j]) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[k.p[j]].tok), name, sizeof name);
        for (n = 0; n < sizeof known / sizeof *known; n++)
            if (!strcmp(name, known[n]))
                break;
        if (n < sizeof known / sizeof *known)
            continue;
        cwarn(c, iloc(c, last_tok(c, k.p[j]) + 1), "attributes",
              "'%s' attribute ignored", name);
    }
}

/* Whether declaration node d itself carries a deprecated/unavailable
 * attribute (not one on a nested parameter): its uses of such typedefs
 * are not reported. */
static bool decl_has_dep_attr(Checker *c, uint32_t d)
{
    uint32_t k;
    if (d == NO_NODE)
        return false;
    for (k = cfirst(c, d); k < d; k++) {
        char name[48];
        uint32_t at, owner;
        if (ntag(c, k) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[k].tok), name, sizeof name);
        if (strcmp(name, "deprecated") && strcmp(name, "unavailable"))
            continue;
        at = c->par[k];
        owner = at == NO_NODE ? NO_NODE : c->par[at];
        if (owner == d || (owner != NO_NODE && c->par[owner] == d &&
                           (ntag(c, owner) == N_INIT_DECL ||
                            ntag(c, owner) == N_MEMBER)))
            return true;
    }
    return false;
}

/* Collects the type-affecting attributes of one ATTRIBUTE node. */
static void attr_collect(Checker *c, uint32_t attr, Attrs *a)
{
    Kids k;
    uint32_t j;
    kids_get(c, attr, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t item = k.p[j], arg;
        char name[48];
        Kids ak;
        if (ntag(c, item) != N_ATTR_ITEM)
            continue;
        attr_norm(tstr(c, c->nodes[item].tok), name, sizeof name);
        kids_get(c, item, &ak);
        arg = ak.n ? ak.p[0] : NO_NODE;
        if (!strcmp(name, "aligned")) {
            uint32_t v;
            if (arg == NO_NODE)
                v = c->tgt->default_aligned;
            else
                v = check_user_alignment(c, arg, iloc(c, after_tok(c, attr)),
                                         true);
            if (v > a->aligned)
                a->aligned = v;
        } else if (!strcmp(name, "packed")) {
            a->packed = true;
        } else if (!strcmp(name, "transparent_union")) {
            a->transparent_union = true;
        } else if (!strcmp(name, "noreturn") || !strcmp(name, "__noreturn__")) {
            a->noreturn = true;
        } else if (!strcmp(name, "deprecated") ||
                   !strcmp(name, "unavailable")) {
            if (name[0] == 'u')
                a->unavailable = true;
            else
                a->deprecated = true;
            if (arg != NO_NODE && ntag(c, arg) == N_STRING)
                a->dep_msg = cdep_msg(c, arg);
        } else if (!strcmp(name, "unused")) {
            a->unused = true;
        } else if (!strcmp(name, "weak") || !strcmp(name, "__weak__")) {
            a->weak = true;
        } else if (!strcmp(name, "vector_size") && arg != NO_NODE) {
            if ((c->ck[arg] == K_ICE || c->ck[arg] == K_FOLD) &&
                type_is_integer(TT, c->ty[arg]))
                a->vector_size = (uint64_t)cexpr_sval(c, arg);
        } else if (!strcmp(name, "mode") && arg != NO_NODE &&
                   ntag(c, arg) == N_IDENT) {
            char m[16];
            attr_norm(tstr(c, c->nodes[arg].tok), m, sizeof m);
            a->has_mode = true;
            a->mode_bytes = 0;
            a->mode_float = 0;
            if (!strcmp(m, "QI") || !strcmp(m, "byte"))
                a->mode_bytes = 1;
            else if (!strcmp(m, "HI"))
                a->mode_bytes = 2;
            else if (!strcmp(m, "SI"))
                a->mode_bytes = 4;
            else if (!strcmp(m, "DI"))
                a->mode_bytes = 8;
            else if (!strcmp(m, "TI"))
                a->mode_bytes = 16;
            else if (!strcmp(m, "word") || !strcmp(m, "pointer") ||
                     !strcmp(m, "unwind_word"))
                a->mode_bytes = c->tgt->ptr_size;
            else if (!strcmp(m, "SF"))
                a->mode_float = 1;
            else if (!strcmp(m, "DF"))
                a->mode_float = 2;
            else if (!strcmp(m, "XF"))
                a->mode_float = 3;
            else if (!strcmp(m, "TF"))
                a->mode_float = 4;
            else
                a->has_mode = false;
        }
        kids_free(&ak);
    }
    kids_free(&k);
}

/* The integer type of the given size and signedness. */
static TypeId int_of_size(Checker *c, unsigned bytes, bool uns)
{
    static const int sk[] = {TY_SCHAR, TY_SHORT, TY_INT, TY_LONG, TY_LLONG,
                             TY_INT128};
    static const int uk[] = {TY_UCHAR, TY_USHORT, TY_UINT, TY_ULONG,
                             TY_ULLONG, TY_UINT128};
    size_t k;
    for (k = 0; k < sizeof sk / sizeof *sk; k++)
        if (c->tgt->size[sk[k]] == bytes)
            return TYPE_MK(uns ? uk[k] : sk[k], 0);
    return ERRT;
}

/* The mode and vector_size attributes applied to a base type. */
static TypeId attr_apply_type(Checker *c, TypeId t, const Attrs *a)
{
    if (a->has_mode && !(type_ckind(TT, t) == TY_ERROR)) {
        unsigned q = TYPE_QUALS(t);
        TypeId n = ERRT;
        if (a->mode_bytes && type_is_integer(TT, t)) {
            n = int_of_size(c, a->mode_bytes, !type_is_signed(TT, t));
            if (a->mode_bytes == 1 && type_kind(TT, TYPE_UNQUAL(type_canon(TT, t))) == TY_BOOL)
                n = TYPE_B(BOOL);
        } else if (a->mode_bytes && type_is_float(TT, t))
            n = ERRT;
        else if (a->mode_float && (type_is_float(TT, t) || type_is_integer(TT, t))) {
            static const int fk[] = {0, TY_FLOAT, TY_DOUBLE, TY_LDOUBLE,
                                     TY_FLOAT128};
            n = TYPE_MK(fk[a->mode_float], 0);
        }
        if (type_ckind(TT, n) != TY_ERROR)
            t = n | q;
    }
    if (a->vector_size) {
        TypeId el = type_canon(TT, t);
        unsigned q = TYPE_QUALS(t);
        if (type_is_arith(TT, el))
            t = type_vector(TT, TYPE_UNQUAL(el), a->vector_size) | q;
    }
    return t;
}

static void attrs_merge(Attrs *to, const Attrs *from)
{
    if (from->aligned > to->aligned)
        to->aligned = from->aligned;
    to->packed |= from->packed;
    to->transparent_union |= from->transparent_union;
    if (from->has_mode) {
        to->has_mode = true;
        to->mode_bytes = from->mode_bytes;
        to->mode_float = from->mode_float;
    }
    if (from->vector_size)
        to->vector_size = from->vector_size;
    to->deprecated |= from->deprecated;
    to->unavailable |= from->unavailable;
    if (from->dep_msg)
        to->dep_msg = from->dep_msg;
    to->unused |= from->unused;
    to->noreturn |= from->noreturn;
}

/* The attributes of node i's ATTRIBUTE children (direct). */
static void attrs_of_children(Checker *c, uint32_t i, Attrs *a)
{
    Kids k;
    uint32_t j;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ATTRIBUTE)
            attr_collect(c, k.p[j], a);
    kids_free(&k);
}

/* A storage class or function specifier (declspecs_add_scspec). */
static void add_scspec(Checker *c, Spec *s, uint32_t tok)
{
    int kw = tckw(c, tok);
    const char *sp = tstr(c, tok);
    SrcLoc loc = tloc(c, tok);
    SrcLoc il = iloc(c, tok);
    unsigned n = SC_NONE;
    bool dupe = false;

    if (s->non_sc_seen)
        cwarn(c, il, "old-style-declaration",
              "'%s' is not at beginning of declaration", sp);
    switch (kw) {
    case CK_INLINE:
        s->is_inline = true;
        s->inline_loc = loc;
        break;
    case CK_NORETURN:
        s->is_noreturn = true;
        s->noreturn_loc = loc;
        break;
    case CK_THREAD_LOCAL:
        dupe = s->thread;
        if (s->sc == SC_AUTO)
            cerror(c, il, "'%s' used with 'auto'", sp);
        else if (s->sc == SC_REGISTER)
            cerror(c, il, "'%s' used with 'register'", sp);
        else if (s->sc == SC_TYPEDEF)
            cerror(c, il, "'%s' used with 'typedef'", sp);
        else {
            s->thread = true;
            s->thread_gnu = !strcmp(sp, "__thread");
            if (!s->thread_gnu)
                cpedantic(c, loc, "ISO C99 does not support '%s'", sp);
            s->thread_loc = loc;
        }
        break;
    case CK_AUTO:
        n = SC_AUTO;
        break;
    case CK_EXTERN:
        n = SC_EXTERN;
        if (s->thread && s->thread_gnu)
            cerror(c, il, "'__thread' before 'extern'");
        break;
    case CK_REGISTER:
        n = SC_REGISTER;
        break;
    case CK_STATIC:
        n = SC_STATIC;
        if (s->thread && s->thread_gnu)
            cerror(c, il, "'__thread' before 'static'");
        break;
    case CK_TYPEDEF:
        n = SC_TYPEDEF;
        break;
    default:
        return;
    }
    if (n != SC_NONE && n == s->sc)
        dupe = true;
    if (dupe) {
        if (kw == CK_THREAD_LOCAL)
            cerror(c, il, "duplicate '_Thread_local' or '__thread'");
        else
            cerror(c, il, "duplicate '%s'", sp);
    }
    if (n != SC_NONE) {
        if (s->sc != SC_NONE && n != s->sc)
            cerror(c, il, "multiple storage classes in declaration "
                   "specifiers");
        else {
            s->sc = (uint8_t)n;
            s->sc_loc = loc;
            if (n != SC_EXTERN && n != SC_STATIC && s->thread) {
                cerror(c, il, "'%s' used with '%s'",
                       s->thread_gnu ? "__thread" : "_Thread_local", sp);
                s->thread = false;
            }
        }
    }
}

static void add_qual(Checker *c, Spec *s, uint32_t tok)
{
    int kw = tckw(c, tok);
    const char *sp = tstr(c, tok);
    SrcLoc loc = tloc(c, tok);
    unsigned q = 0;
    unsigned idx = 0;
    bool dupe;
    uint32_t prev;
    s->non_sc_seen = true;
    switch (kw) {
    case CK_CONST: q = TQ_CONST; idx = 0; break;
    case CK_VOLATILE: q = TQ_VOLATILE; idx = 1; break;
    case CK_RESTRICT: q = TQ_RESTRICT; idx = 2; break;
    case CK_ATOMIC: q = TQ_ATOMIC; idx = 3; break;
    default: return;
    }
    if (q == TQ_ATOMIC)
        cpedantic(c, loc, "ISO C99 does not support the '_Atomic' qualifier");
    dupe = (s->quals & q) != 0;
    prev = s->qual_tok[idx];
    s->quals |= q;
    s->qual_tok[idx] = tok + 1;
    if (!s->qual_loc)
        s->qual_loc = loc;
    if (q == TQ_RESTRICT)
        s->restrict_q = true;
    if (dupe && prev && !tfrom_macro(c, prev - 1) && !tfrom_macro(c, tok))
        cwarn(c, loc, "duplicate-decl-specifier",
              "duplicate '%s' declaration specifier", sp);
}

/* The name of the word currently in s, for "both 'X' and 'Y'". */
static const char *word_name(const Spec *s)
{
    switch (s->word) {
    case TW_VOID: return "void";
    case TW_BOOL: return "_Bool";
    case TW_CHAR: return "char";
    case TW_INT: return "int";
    case TW_FLOAT: return "float";
    case TW_DOUBLE: return "double";
    case TW_INT128: return "__int128";
    case TW_FLOATN:
    case TW_DECIMAL: return s->wname;
    case TW_AUTO_TYPE: return "__auto_type";
    default: return "";
    }
}

static unsigned wbit(unsigned w)
{
    return 1u << w;
}

/* A modifier (long, short, signed, unsigned, complex) against the word. */
static bool mod_conflict(Checker *c, const Spec *s, const char *mod,
                         unsigned words, SrcLoc loc)
{
    if (s->word != TW_NONE && (words & wbit(s->word))) {
        cerror(c, loc, "both '%s' and '%s' in declaration specifiers", mod,
               word_name(s));
        return true;
    }
    return false;
}

#define W_ALLBUT_INTDBL (wbit(TW_AUTO_TYPE) | wbit(TW_VOID) | wbit(TW_BOOL) | \
                         wbit(TW_CHAR) | wbit(TW_FLOAT) | wbit(TW_INT128) |    \
                         wbit(TW_FLOATN) | wbit(TW_DECIMAL))

/* A type specifier keyword (declspecs_add_type, keyword part). */
static void add_type_kw(Checker *c, Spec *s, uint32_t tok)
{
    int kw = tckw(c, tok);
    const char *sp = tstr(c, tok);
    SrcLoc loc = tloc(c, tok);
    bool dupe = false;

    s->non_sc_seen = true;
    if (!s->type_loc)
        s->type_loc = loc;
    if (s->has_type) {
        cerror(c, loc, "two or more data types in declaration specifiers");
        return;
    }
    switch (kw) {
    case CK_LONG:
        if (s->long_long) {
            cerror(c, loc, "'long long long' is too long for GCC");
            return;
        }
        if (s->is_long) {
            if (s->word == TW_DOUBLE) {
                cerror(c, loc, "both 'long long' and 'double' in declaration "
                       "specifiers");
                return;
            }
            s->long_long = true;
            return;
        }
        if (s->is_short)
            cerror(c, loc, "both 'long' and 'short' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "long", W_ALLBUT_INTDBL, loc))
            s->is_long = true;
        return;
    case CK_SHORT:
        dupe = s->is_short;
        if (s->is_long)
            cerror(c, loc, "both 'long' and 'short' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "short",
                               W_ALLBUT_INTDBL | wbit(TW_DOUBLE), loc))
            s->is_short = true;
        break;
    case CK_SIGNED:
        dupe = s->is_signed;
        if (s->is_unsigned)
            cerror(c, loc, "both 'signed' and 'unsigned' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "signed",
                               wbit(TW_AUTO_TYPE) | wbit(TW_VOID) |
                               wbit(TW_BOOL) | wbit(TW_FLOAT) |
                               wbit(TW_DOUBLE) | wbit(TW_FLOATN) |
                               wbit(TW_DECIMAL), loc))
            s->is_signed = true;
        break;
    case CK_UNSIGNED:
        dupe = s->is_unsigned;
        if (s->is_signed)
            cerror(c, loc, "both 'signed' and 'unsigned' in declaration "
                   "specifiers");
        else if (!mod_conflict(c, s, "unsigned",
                               wbit(TW_AUTO_TYPE) | wbit(TW_VOID) |
                               wbit(TW_BOOL) | wbit(TW_FLOAT) |
                               wbit(TW_DOUBLE) | wbit(TW_FLOATN) |
                               wbit(TW_DECIMAL), loc))
            s->is_unsigned = true;
        break;
    case CK_COMPLEX:
        dupe = s->is_complex;
        if (!mod_conflict(c, s, "complex",
                          wbit(TW_AUTO_TYPE) | wbit(TW_VOID) | wbit(TW_BOOL) |
                          wbit(TW_DECIMAL), loc)) {
            s->is_complex = true;
            s->complex_loc = loc;
        }
        break;
    case CK_IMAGINARY:
        /* an extension gcc rejects; reported as an unknown type name */
        return;
    default: {
        /* the words */
        unsigned nw = TW_NONE;
        if (s->word != TW_NONE) {
            cerror(c, loc, "two or more data types in declaration "
                   "specifiers");
            return;
        }
        switch (kw) {
        case CK_AUTO_TYPE:
            if (!mod_conflict(c, s, s->is_long ? "long" : s->is_short ? "short"
                              : s->is_signed ? "signed" : s->is_unsigned
                              ? "unsigned" : "complex", ~0u, loc) &&
                (s->is_long || s->is_short || s->is_signed || s->is_unsigned ||
                 s->is_complex)) {
                cerror(c, loc, "both '%s' and '__auto_type' in declaration "
                       "specifiers",
                       s->is_long ? "long" : s->is_short ? "short"
                       : s->is_signed ? "signed" : s->is_unsigned
                       ? "unsigned" : "complex");
                return;
            }
            if (s->is_long || s->is_short || s->is_signed ||
                s->is_unsigned || s->is_complex)
                return;
            nw = TW_AUTO_TYPE;
            break;
        case CK_VOID:
        case CK_BOOL: {
            const char *w = kw == CK_VOID ? "void" : "_Bool";
            const char *m = s->is_long ? "long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : s->is_complex ? "complex" : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and '%s' in declaration specifiers",
                       m, w);
                return;
            }
            nw = kw == CK_VOID ? TW_VOID : TW_BOOL;
            break;
        }
        case CK_CHAR: {
            const char *m = s->is_long ? "long" : s->is_short ? "short" : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and 'char' in declaration "
                       "specifiers", m);
                return;
            }
            nw = TW_CHAR;
            break;
        }
        case CK_INT:
            nw = TW_INT;
            break;
        case CK_FLOAT: {
            const char *m = s->is_long ? "long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and 'float' in declaration "
                       "specifiers", m);
                return;
            }
            nw = TW_FLOAT;
            break;
        }
        case CK_DOUBLE: {
            const char *m = s->long_long ? "long long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and 'double' in declaration "
                       "specifiers", m);
                return;
            }
            nw = TW_DOUBLE;
            break;
        }
        case CK_INT128: {
            size_t l = strlen(sp);
            if (!(l >= 2 && sp[l - 1] == '_' && sp[l - 2] == '_'))
                cpedantic(c, loc, "ISO C does not support '__int128' types");
            if (s->is_long)
                cerror(c, loc, "both '__int128' and 'long' in declaration "
                       "specifiers");
            else if (s->is_short)
                cerror(c, loc, "both '__int128' and 'short' in declaration "
                       "specifiers");
            else {
                s->word = TW_INT128;
                s->type_loc = loc;
            }
            return;
        }
        case CK_FLOATN: {
            const char *m;
            static const struct { const char *sp; int ty; const char *nm; }
                fl[] = {{"_Float16", TY_FLOAT16, "_Float16"},
                        {"__bf16", TY_BF16, "__bf16"},
                        {"_Float32", TY_FLOAT32, "_Float32"},
                        {"_Float64", TY_FLOAT64, "_Float64"},
                        {"_Float128", TY_FLOAT128, "_Float128"},
                        {"__float128", TY_FLOAT128, "_Float128"},
                        {"_Float32x", TY_FLOAT32X, "_Float32x"},
                        {"_Float64x", TY_FLOAT64X, "_Float64x"},
                        {"_Float128x", 0, "_Float128x"},
                        {"__float80", TY_LDOUBLE, "__float80"},
                        {"__ibm128", TY_IBM128, "__ibm128"}};
            size_t k;
            s->nty = 0;
            s->wname = sp;
            for (k = 0; k < sizeof fl / sizeof *fl; k++)
                if (!strcmp(fl[k].sp, sp)) {
                    s->nty = (uint32_t)fl[k].ty;
                    s->wname = fl[k].nm;
                }
            if (strcmp(sp, "__bf16") && strcmp(sp, "__float80") &&
                strcmp(sp, "__ibm128") && strcmp(sp, "__float128"))
                cpedantic(c, loc, "ISO C does not support the '%s' type",
                          s->wname);
            m = s->is_long ? "long" : s->is_short ? "short"
                : s->is_signed ? "signed" : s->is_unsigned ? "unsigned"
                : NULL;
            if (m) {
                cerror(c, loc, "both '%s' and '%s' in declaration specifiers",
                       m, s->wname);
                return;
            }
            if (!s->nty || !c->tgt->size[s->nty]) {
                cerror(c, loc, "'%s' is not supported on this target",
                       s->wname);
                s->nty = TY_INT;
            }
            nw = TW_FLOATN;
            break;
        }
        case CK_DECIMAL: {
            const char *m = s->long_long ? "long long" : s->is_long ? "long"
                : s->is_short ? "short" : s->is_signed ? "signed"
                : s->is_unsigned ? "unsigned" : s->is_complex ? "complex"
                : NULL;
            s->wname = sp;
            s->nty = !strcmp(sp, "_Decimal32") ? TY_DEC32
                     : !strcmp(sp, "_Decimal64") ? TY_DEC64 : TY_DEC128;
            if (m)
                cerror(c, loc, "both '%s' and '%s' in declaration specifiers",
                       m, sp);
            else
                s->word = TW_DECIMAL;
            cpedantic(c, loc, "ISO C does not support decimal floating-point "
                      "before C2X");
            if (!m)
                s->type_loc = loc;
            return;
        }
        default:
            return;
        }
        s->word = (uint8_t)nw;
        s->type_loc = loc;
        return;
    }
    }
    if (dupe)
        cerror(c, loc, "duplicate '%s'", sp);
}

/* A whole type (typedef name, tag, typeof, _Atomic(type)). */
static void add_type_whole(Checker *c, Spec *s, TypeId t, unsigned kind,
                           uint32_t tok)
{
    SrcLoc loc = tloc(c, tok);
    s->non_sc_seen = true;
    s->kind = (uint8_t)kind;
    if (s->has_type || s->word != TW_NONE || s->is_long || s->is_short ||
        s->is_signed || s->is_unsigned || s->is_complex) {
        cerror(c, loc, "two or more data types in declaration specifiers");
        return;
    }
    s->ty = t;
    s->has_type = true;
    s->type_loc = loc;
    if (type_ckind(TT, t) == TY_ERROR)
        s->error = true;
}

/* finish_declspecs: the type the words and modifiers name. */
static void finish_declspecs(Checker *c, Spec *s)
{
    TypeId t = ERRT;
    if (s->has_type) {
        if (s->error)
            s->ty = TYPE_B(INT);
        return;
    }
    if (s->word == TW_NONE) {
        if (s->is_long || s->is_short || s->is_signed || s->is_unsigned)
            s->word = TW_INT;
        else if (s->is_complex) {
            s->word = TW_DOUBLE;
            cpedantic(c, s->complex_loc, "ISO C does not support plain "
                      "'complex' meaning 'double complex'");
        } else {
            s->word = TW_INT;
            s->default_int = true;
        }
    }
    switch (s->word) {
    case TW_AUTO_TYPE:
        t = TYPE_B(INT);
        break;
    case TW_VOID:
        t = TYPE_B(VOID);
        break;
    case TW_BOOL:
        t = TYPE_B(BOOL);
        break;
    case TW_CHAR:
        t = s->is_signed ? TYPE_B(SCHAR) : s->is_unsigned ? TYPE_B(UCHAR)
                                                         : TYPE_B(CHAR);
        goto cplx;
    case TW_INT128:
        t = s->is_unsigned ? TYPE_B(UINT128) : TYPE_B(INT128);
        if (!c->tgt->size[TY_INT128])
            t = TYPE_B(INT);
        goto cplx;
    case TW_INT:
        if (s->long_long)
            t = s->is_unsigned ? TYPE_B(ULLONG) : TYPE_B(LLONG);
        else if (s->is_long)
            t = s->is_unsigned ? TYPE_B(ULONG) : TYPE_B(LONG);
        else if (s->is_short)
            t = s->is_unsigned ? TYPE_B(USHORT) : TYPE_B(SHORT);
        else
            t = s->is_unsigned ? TYPE_B(UINT) : TYPE_B(INT);
    cplx:
        if (s->is_complex) {
            cpedantic(c, s->complex_loc, "ISO C does not support complex "
                      "integer types");
            t = type_complex(TT, t);
        }
        break;
    case TW_FLOAT:
        t = s->is_complex ? type_complex(TT, TYPE_B(FLOAT)) : TYPE_B(FLOAT);
        break;
    case TW_DOUBLE:
        if (s->is_long)
            t = s->is_complex ? type_complex(TT, TYPE_B(LDOUBLE))
                              : TYPE_B(LDOUBLE);
        else
            t = s->is_complex ? type_complex(TT, TYPE_B(DOUBLE))
                              : TYPE_B(DOUBLE);
        break;
    case TW_FLOATN:
        t = TYPE_MK(s->nty, 0);
        if (s->is_complex)
            t = type_complex(TT, t);
        break;
    case TW_DECIMAL:
        t = TYPE_MK(s->nty, 0);
        break;
    default:
        break;
    }
    s->ty = t;
}

/* The location gcc's shadow_tag uses for "here": the SPECS node's tokens. */

static void specs_visit(Checker *c, uint32_t i)
{
    Spec s;
    Kids k;
    uint32_t j;
    memset(&s, 0, sizeof s);
    s.node = i;
    s.tag_node = NO_NODE;
    s.kind = TSK_NONE;
    s.tok0 = c->nodes[i].tok;
    s.loc = tloc(c, s.tok0);
    {
        /* a declaration as the body of a label (C2X) */
        uint32_t d = c->par[i], l = d == NO_NODE ? NO_NODE : c->par[d];
        if (l != NO_NODE && ntag(c, d) == N_DECL && d == l - 1 &&
            cfirst(c, d) == cfirst(c, i) &&
            (ntag(c, l) == N_LABEL || ntag(c, l) == N_CASE ||
             ntag(c, l) == N_DEFAULT))
            cpedantic(c, s.loc, "a label can only be part of a statement "
                      "and a declaration is not a statement");
    }
    s.tok1 = s.tok0;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t n = k.p[j];
        const Node *nd = cnode(c, n);
        switch (nd->tag) {
        case N_STORAGE:
        case N_FUNCSPEC:
            add_scspec(c, &s, nd->tok);
            break;
        case N_QUAL:
            add_qual(c, &s, nd->tok);
            break;
        case N_TYPESPEC:
            add_type_kw(c, &s, nd->tok);
            break;
        case N_TYPEDEF_NAME: {
            uint32_t ref = lookup_ord(c, cnode_ident(c, n));
            TypeId t = ERRT;
            if (nd->flags & NF_ERROR) /* an unknown type name, diagnosed */
                ref = SYM_NONE;
            else if (ref != SYM_NONE && csym(c, ref)->kind == CS_TYPEDEF)
                t = csym(c, ref)->ty;
            else if (ref == SYM_NONE || csym(c, ref)->kind != CS_TYPEDEF)
                cerror(c, iloc(c, nd->tok), "'%s' fails to be a typedef or "
                       "built in type", tstr(c, nd->tok));
            add_type_whole(c, &s, t, TSK_TYPEDEF, nd->tok);
            if (ref != SYM_NONE) {
                csym(c, ref)->flags |= CSF_USED;
                if ((csym(c, ref)->flags & (CSF_DEPRECATED | CSF_UNAVAILABLE)) &&
                    !decl_has_dep_attr(c, c->par[c->par[n]]))
                {
                    /* gcc names the tagged type, not a plain typedef */
                    const CSym *ts = csym(c, ref);
                    TypeId ct = type_canon(TT, ts->ty);
                    SrcLoc nl = 0;
                    bool have = false;
                    if (type_ckind(TT, ct) == TY_STRUCT || type_ckind(TT, ct) == TY_UNION) {
                        const Record *r = type_record(TT, ct);
                        if (r) {
                            nl = r->loc;
                            have = true;
                        }
                    } else if (type_ckind(TT, ct) == TY_ENUM) {
                        const Enum *en = type_enum(TT, ct);
                        if (en) {
                            nl = en->loc;
                            have = true;
                        }
                    }
                    cdep_use(c, cinput_loc(c, nd->tok), ts, have ? &nl : NULL);
                }
            }
            break;
        }
        case N_STRUCT:
        case N_ENUM:
            add_type_whole(c, &s, c->ty[n], (unsigned)(c->cv[n] & 0xff),
                           nd->tok);
            if (c->cb[n]) {
                s.xref_name = c->cb[n];
                s.xref_loc = (SrcLoc)(c->cv[n] >> 8);
            }
            if (!s.error || s.has_type)
                s.tag_node = n;
            break;
        case N_TYPEOF: {
            uint32_t a = first_child(c, n);
            TypeId t = ERRT;
            if (a != NO_NODE) {
                if (ntag(c, a) == N_TYPE_NAME)
                    t = c->ty[a];
                else
                    t = c->ty[a];
            }
            add_type_whole(c, &s, t, TSK_TYPEOF, nd->tok);
            break;
        }
        case N_ATOMIC_TYPE: {
            uint32_t a = first_child(c, n);
            TypeId t = a != NO_NODE ? c->ty[a] : ERRT;
            SrcLoc loc = tloc(c, nd->tok);
            cpedantic(c, loc, "ISO C99 does not support the '_Atomic' "
                      "qualifier");
            if (type_ckind(TT, t) != TY_ERROR) {
                if (type_ckind(TT, t) == TY_ARRAY ||
                    type_ckind(TT, t) == TY_VLA) {
                    cerror(c, loc, "'_Atomic'-qualified array type");
                    t = ERRT;
                } else if (type_ckind(TT, t) == TY_FUNC) {
                    cerror(c, loc, "'_Atomic'-qualified function type");
                    t = ERRT;
                } else if (TYPE_QUALS(t) || TYPE_QUALS(type_canon(TT, t))) {
                    cerror(c, loc, "'_Atomic' applied to a qualified type");
                    t = ERRT;
                } else
                    t |= TQ_ATOMIC;
            }
            add_type_whole(c, &s, t, TSK_TYPEOF, nd->tok);
            break;
        }
        case N_ALIGNAS: {
            uint32_t a = first_child(c, n);
            SrcLoc loc = tloc(c, nd->tok);
            uint32_t v = 0;
            s.alignas_seen = true;
            s.alignas_loc = loc;
            cpedantic(c, loc, "ISO C99 does not support '%s'",
                      tstr(c, nd->tok));
            if (a != NO_NODE) {
                if (ntag(c, a) == N_TYPE_NAME) {
                    if (type_ckind(TT, c->ty[a]) != TY_ERROR)
                        v = type_align(TT, c->ty[a]);
                } else
                    v = check_user_alignment(c, a, iloc(c, after_tok(c, n)),
                                         false);
            }
            if (v > s.align)
                s.align = v;
            break;
        }
        case N_ATTRIBUTE:
            s.has_attrs = true;
            attr_collect(c, n, &s.attrs);
            break;
        default:
            break;
        }
    }
    kids_free(&k);
    s.tok1 = k.n ? after_tok(c, i) : s.tok0;
    finish_declspecs(c, &s);
    if (s.attrs.aligned > s.align)
        s.align = s.attrs.aligned;
    if (s.attrs.has_mode || s.attrs.vector_size)
        s.ty = attr_apply_type(c, s.ty, &s.attrs);
    if (type_ckind(TT, s.ty) == TY_ERROR)
        s.error = true;
    c->ty[i] = s.ty;
    vec_push(&c->specs, s);
}

/* ---- type helpers --------------------------------------------------------- */

static TypeKind tkind(Checker *c, TypeId t)
{
    return type_ckind(TT, t);
}

static bool is_err(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ERROR;
}

static bool is_arr(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k == TY_ARRAY || k == TY_VLA;
}

static bool is_func(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_FUNC;
}

static bool is_void(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_VOID;
}

static bool is_prototype(Checker *c, TypeId t)
{
    return is_func(c, t) &&
           !(type_ent(TT, type_canon(TT, t))->flags & TF_NOPROTO);
}

static TypeId strip_arrays(Checker *c, TypeId t)
{
    while (is_arr(c, t))
        t = type_base(TT, t);
    return t;
}

/* The qualifiers a type has, whether written or through a typedef. */
static unsigned all_quals(Checker *c, TypeId t)
{
    t = strip_arrays(c, t);
    return TYPE_QUALS(t) | TYPE_QUALS(type_canon(TT, t));
}

static bool object_or_incomplete(Checker *c, TypeId t)
{
    return !is_func(c, t);
}

/* c_build_qualified_type: quals on t (an array's go to its elements);
 * restrict on a non-pointer is an error (at input_location, ltok). */
static TypeId qualify(Checker *c, TypeId t, unsigned q, uint32_t ltok)
{
    if (!q || is_err(c, t))
        return t;
    if (q & TQ_RESTRICT) {
        TypeId e = strip_arrays(c, t);
        if (tkind(c, e) != TY_PTR ||
            !object_or_incomplete(c, type_base(TT, e))) {
            cerror(c, iloc(c, ltok), "invalid use of 'restrict'");
            q &= ~(unsigned)TQ_RESTRICT;
        }
    }
    return t | q;
}

/* Is the constant expression node e an INTEGER_CST (K_ICE, or a folded
 * constant)?  If not, may it still be folded to one? */
static bool node_int_cst(Checker *c, uint32_t e)
{
    return c->ck[e] == K_ICE || (c->ck[e] == K_FOLD && (c->ef[e] & EF_CST));
}

/* ---- declarator analysis ------------------------------------------------- */

enum { GD_NONE, GD_VAR, GD_PARM, GD_FIELD, GD_FUNC, GD_TYPEDEF, GD_TYPENAME };

typedef struct GDecl {
    int what;
    uint32_t name;           /* ident, 0: none */
    uint32_t name_node;
    SrcLoc loc;              /* the declaration's own location */
    TypeId ty;
    CSym s;
    bool array_param;
    int width;               /* fields: bits, -1: not a bit-field */
    bool default_int;
    bool funcdef_ok;
} GDecl;

/* The location of an abstract declarator's (missing) name. */
static SrcLoc abstract_loc(Checker *c, uint32_t top, uint32_t after)
{
    uint32_t d = top, inner;
    if (d != NO_NODE) {
        while ((inner = inner_decl(c, d)) != NO_NODE)
            d = inner;
        if (ntag(c, d) == N_ARRAY)
            return tloc(c, cnode(c, d)->tok);
    }
    return iloc(c, after);
}

static unsigned quals_of(Checker *c, uint32_t node)
{
    Kids k;
    uint32_t j;
    unsigned q = 0;
    kids_get(c, node, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_QUAL)
            switch (tckw(c, cnode(c, k.p[j])->tok)) {
            case CK_CONST: q |= TQ_CONST; break;
            case CK_VOLATILE: q |= TQ_VOLATILE; break;
            case CK_RESTRICT: q |= TQ_RESTRICT; break;
            case CK_ATOMIC: q |= TQ_ATOMIC; break;
            default: break;
            }
    kids_free(&k);
    return q;
}

static bool has_child_attr(Checker *c, uint32_t node)
{
    Kids k;
    uint32_t j;
    bool r = false;
    kids_get(c, node, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ATTRIBUTE &&
            c->nodes[k.p[j]].tok > c->nodes[node].tok)
            r = true;   /* inside '[' ']', not before it */
    kids_free(&k);
    return r;
}

/* The size expression child of an array declarator, or NO_NODE. */
static uint32_t array_size_node(Checker *c, uint32_t a)
{
    uint32_t l = last_child(c, a);
    if (l == NO_NODE)
        return NO_NODE;
    if (cexpr_is_expr(ntag(c, l)))
        return l;
    return NO_NODE;
}

/* The parameter type list of a FUNC declarator: what grokparms and
 * get_parm_info make of it.  Returns the function's parameter types in
 * ps (malloc'd, *n of them), *flags for TF_*; ret false if the list is
 * "erroneous" (no list at all). */
typedef struct PInfo {
    TypeId *ps;
    uint32_t n;
    unsigned flags;
    bool krlist;             /* an identifier list */
    bool empty;              /* () */
} PInfo;

static void func_params(Checker *c, uint32_t f, PInfo *pi)
{
    Kids k;
    uint32_t j, np = 0;
    memset(pi, 0, sizeof *pi);
    kids_get(c, f, &k);
    pi->ps = xmalloc((k.n + 1) * sizeof *pi->ps);
    if (cnode(c, f)->flags & NF_KR)
        pi->krlist = true;
    for (j = 0; j < k.n; j++) {
        uint32_t p = k.p[j];
        if (is_real_param(c, p)) {
            bool named = c->cv[p] & 1;
            TypeId t = c->ty[p];
            /* a void parameter without a name is (void) or an error */
            if (!named && is_void(c, t) && !is_err(c, t))
                continue;
            pi->ps[np++] = t;
        } else if (ntag(c, p) == N_KR_IDENT) {
            pi->krlist = true;
        }
    }
    kids_free(&k);
    pi->n = np;
    if (cnode(c, f)->flags & NF_VARIADIC)
        pi->flags |= TF_VARIADIC;
}

/* The parameter list has exactly one parameter, unnamed and of type void. */
static bool lone_void(Checker *c, uint32_t f)
{
    Kids k;
    uint32_t j, np = 0, only = NO_NODE;
    bool r;
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++)
        if (is_real_param(c, k.p[j]) || ntag(c, k.p[j]) == N_KR_IDENT) {
            np++;
            only = k.p[j];
        }
    kids_free(&k);
    r = np == 1 && is_real_param(c, only) && !(c->cv[only] & 1) &&
        is_void(c, c->ty[only]);
    return r;
}

static bool func_has_params(Checker *c, uint32_t f)
{
    Kids k;
    uint32_t j;
    bool r = false;
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++)
        if (is_real_param(c, k.p[j]) || ntag(c, k.p[j]) == N_KR_IDENT)
            r = true;
    kids_free(&k);
    return r;
}

static bool func_has_vla_unspec(Checker *c, uint32_t f)
{
    uint32_t k;
    for (k = cfirst(c, f); k < f; k++)
        if (ntag(c, k) == N_ARRAY && (cnode(c, k)->flags & NF_STAR))
            return true;
    return false;
}

static void warn_if_shadowing(Checker *c, const CSym *x);

/* grokparms: checks on the parameter list, at input_location. */
static void grokparms(Checker *c, uint32_t f, bool funcdef, uint32_t ltok,
                      const PInfo *pi, bool *incomplete_def)
{
    Kids k;
    uint32_t j, parmno = 0;
    SrcLoc il = iloc(c, ltok);
    bool empty = !func_has_params(c, f);
    *incomplete_def = false;
    if (funcdef && func_has_vla_unspec(c, f))
        cerror(c, il, "'[*]' not allowed in other than function prototype "
               "scope");
    if (empty && !funcdef)
        cwarn(c, il, "strict-prototypes", "function declaration isn't a "
              "prototype");
    if (pi->krlist) {
        if (!funcdef)
            cpedwarn(c, il, "", "parameter names (without types) in function "
                     "declaration");
        return;
    }
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t p = k.p[j];
        TypeId t;
        bool named;
        if (!is_real_param(c, p))
            continue;
        t = c->ty[p];
        named = c->cv[p] & 1;
        if (!named && !is_err(c, t) && is_void(c, t))
            continue;
        parmno++;
        if (named && !c->cb[p])
            named = false;
        if (named && (csym(c, c->cb[p] - 1)->flags & CSF_USED))
            warn_if_shadowing(c, csym(c, c->cb[p] - 1));
        if (is_err(c, t))
            continue;
        if (!type_is_complete(TT, t)) {
            /* the lone (void) is not a parameter */
            if (parmno == 1 && lone_void(c, f))
                continue;
            if (funcdef) {
                if (named)
                    cerror(c, csym(c, c->cb[p] - 1)->loc,
                           "parameter %u ('%s') has incomplete type",
                           parmno, cident(c, csym(c, c->cb[p] - 1)->name));
                else
                    cerror(c, c->cb[p] ? csym(c, c->cb[p] - 1)->loc : il,
                           "parameter %u has incomplete type", parmno);
                *incomplete_def = true;
            } else if (is_void(c, t)) {
                if (named)
                    cwarn(c, csym(c, c->cb[p] - 1)->loc, "",
                          "parameter %u ('%s') has void type",
                          parmno, cident(c, csym(c, c->cb[p] - 1)->name));
                else
                    cwarn(c, c->cb[p] ? csym(c, c->cb[p] - 1)->loc : il, "",
                          "parameter %u has void type", parmno);
            }
        }
    }
    kids_free(&k);
}

/* ---- bit-fields -------------------------------------------------------- */

/* The minimum precision of v in a type of signedness uns (gcc's
 * tree_int_cst_min_precision). */
static unsigned min_prec(uint64_t v, bool neg, bool uns)
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

/* Is a value of the enumeration type e too wide for w bits? */
static bool enum_narrower(Checker *c, TypeId e, unsigned w)
{
    uint32_t k;
    bool uns = !type_is_signed(TT, e), narrow = false, any = false;
    TypeId ce = type_canon(TT, e);
    for (k = 0; k < c->gsyms.len + c->lsyms.len; k++) {
        const CSym *s = k < c->gsyms.len ? &c->gsyms.data[k]
                                         : &c->lsyms.data[k - c->gsyms.len];
        bool neg;
        if (s->kind != CS_ENUMCONST || type_canon(TT, s->ty) != ce)
            continue;
        any = true;
        neg = !uns && (int64_t)s->val < 0;
        if (w < min_prec(s->val, neg, uns))
            narrow = true;
    }
    return any && narrow;
}

/* check_bitfield_type_and_width: the width node w of the field name (0:
 * unnamed); *ty may change (invalid types become unsigned); returns the
 * width in bits. */
static int check_bitfield(Checker *c, SrcLoc loc, TypeId *ty, uint32_t w,
                          uint32_t name, uint32_t ltok)
{
    const char *nm = name ? cident(c, name) : "<anonymous>";
    int64_t width = 1;
    unsigned max;
    TypeId t = *ty;
    bool ok = true;
    if (w != NO_NODE) {
        if (c->ck[w] == K_ERR || c->ck[w] == K_NONE || c->ck[w] == K_FLOAT ||
            c->ck[w] == K_ADDR || !type_is_integer(TT, c->ty[w])) {
            cerror(c, loc, "bit-field '%s' width not an integer constant", nm);
            ok = false;
        } else {
            if (!node_int_cst(c, w))
                cpedantic(c, loc, "bit-field '%s' width not an integer "
                          "constant expression", nm);
            if (c->ef[w] & EF_OVERFLOW)
                cpedwarn(c, loc, "overflow", "overflow in constant expression");
            width = cexpr_sval(c, w);
            if (width < 0 && !type_is_signed(TT, c->ty[w]))
                width = INT64_MAX;
            if (width < 0) {
                cerror(c, loc, "negative width in bit-field '%s'", nm);
                width = 1;
            } else if (width == 0 && name) {
                cerror(c, loc, "zero width for bit-field '%s'", nm);
                width = 1;
            }
        }
    }
    (void)ok;
    (void)ltok;
    if (is_err(c, t))
        return (int)width;
    if (!type_is_integer(TT, t)) {
        cerror(c, loc, "bit-field '%s' has invalid type", nm);
        t = TYPE_B(UINT) | TYPE_QUALS(t);
        *ty = t;
    }
    max = tkind(c, t) == TY_BOOL ? 1 : type_int_bits(TT, t);
    if (width > (int64_t)max) {
        cerror(c, loc, "width of '%s' exceeds its type", nm);
        width = max;
    }
    if (tkind(c, t) == TY_ENUM && enum_narrower(c, t, (unsigned)width))
        cwarn(c, loc, "", "'%s' is narrower than values of its type", nm);
    return (int)width;
}

/* ---- grokdeclarator ------------------------------------------------------ */

static void fill_func_type(Checker *c, uint32_t f, bool funcdef, uint32_t ltok,
                           TypeId ret, TypeId *out)
{
    PInfo pi;
    bool incomplete;
    unsigned fl;
    uint32_t j;
    func_params(c, f, &pi);
    grokparms(c, f, funcdef, ltok, &pi, &incomplete);
    for (j = 0; j < pi.n; j++)
        pi.ps[j] = TYPE_UNQUAL(pi.ps[j]);
    fl = pi.flags;
    if (!func_has_params(c, f) || pi.krlist || incomplete)
        fl |= TF_NOPROTO;
    if (fl & TF_NOPROTO) {
        if (pi.krlist || incomplete)
            pi.n = 0;
        else
            fl &= ~(unsigned)TF_VARIADIC;
    }
    *out = type_func(TT, ret, pi.ps, pi.n, fl);
    free(pi.ps);
}

/* The declarator chain from the root to (not including) the name. */
typedef struct Chain {
    uint32_t buf[32];
    uint32_t *p;
    uint32_t n;
} Chain;

static void chain_get(Checker *c, uint32_t top, Chain *ch)
{
    uint32_t d = top, cap = 32;
    ch->p = ch->buf;
    ch->n = 0;
    while (d != NO_NODE && ntag(c, d) != N_NAME) {
        if (ch->n == cap) {
            uint32_t *np = xmalloc(cap * 2 * sizeof *np);
            memcpy(np, ch->p, ch->n * sizeof *np);
            if (ch->p != ch->buf)
                free(ch->p);
            ch->p = np;
            cap *= 2;
        }
        ch->p[ch->n++] = d;
        d = inner_decl(c, d);
    }
}

static void chain_free(Chain *ch)
{
    if (ch->p != ch->buf)
        free(ch->p);
}

static bool valid_array_size(Checker *c, SrcLoc loc, TypeId elem, uint64_t n,
                             uint32_t name)
{
    bool ok;
    uint64_t es = type_size(TT, elem, &ok);
    if (!ok || !es)
        return true;
    if (n > (uint64_t)INT64_MAX / es) {
        if (name)
            cerror(c, loc, "size of array '%s' exceeds maximum object size "
                   "'9223372036854775807'", cident(c, name));
        else
            cerror(c, loc, "size of array exceeds maximum object size "
                   "'9223372036854775807'");
        return false;
    }
    return true;
}

static bool flex_struct(Checker *c, TypeId t);

/* grokdeclarator.  ltok: gcc's lookahead token at the diagnostics without a
 * location of their own (input_location); after: the token after the
 * declarator (for the location of an abstract one). */
static void grok(Checker *c, const Spec *sp, uint32_t top, int ctx,
                 bool funcdef, bool initialized, uint32_t width_node,
                 uint32_t ltok, uint32_t after, GDecl *g)
{
    TypeId type = sp->ty;
    bool threadp = sp->thread;
    unsigned sc = sp->sc;
    unsigned type_quals, array_ptr_quals = 0;
    bool array_ptr_attrs = false, array_parm_static = false;
    bool array_parm_vla_unspec = false;
    bool size_varies = false, funcdef_syntax = false;
    bool bitfield = width_node != NO_NODE;
    bool filescope = cat_file_scope(c);
    uint32_t name = 0, name_node = NO_NODE, d;
    SrcLoc loc = 0;
    unsigned first_kind = 0;
    Chain ch;
    uint32_t ci;
    int wbits = -1;
    bool default_int = sp->default_int;
    (void)size_varies;

    memset(g, 0, sizeof *g);
    g->width = -1;
    g->ty = ERRT;
    g->what = GD_NONE;
    if (is_err(c, type))
        return;
    /* the name, the location */
    d = top;
    while (d != NO_NODE) {
        unsigned tg = ntag(c, d);
        if (tg == N_NAME) {
            name_node = d;
            name = cnode_ident(c, d);
            loc = tloc(c, cnode(c, d)->tok);
            if (!first_kind)
                first_kind = N_NAME;
            break;
        }
        if (!first_kind)
            first_kind = tg;
        funcdef_syntax = tg == N_FUNC;
        d = inner_decl(c, d);
    }
    if (name_node == NO_NODE)
        loc = abstract_loc(c, top, after);
    if (funcdef && !funcdef_syntax)
        return;
    g->name = name;
    g->name_node = name_node;
    g->loc = loc;
    g->default_int = default_int;
    if ((ctx == DC_NORMAL || ctx == DC_FIELD) && filescope &&
        type_is_vm(TT, type)) {
        if (name)
            cerror(c, loc, "variably modified '%s' at file scope",
                   cident(c, name));
        else
            cerror(c, loc, "variably modified field at file scope");
        type = TYPE_B(INT);
    }
    if (default_int && !funcdef) {
        if (name)
            cpedwarn(c, loc, "implicit-int", "type defaults to 'int' in "
                     "declaration of '%s'", cident(c, name));
        else
            cpedwarn(c, loc, "implicit-int", "type defaults to 'int' in type "
                     "name");
    }
    type_quals = sp->quals | all_quals(c, type);
    type_quals &= TQ_MASK;
    if ((sp->quals & TQ_ATOMIC) && is_arr(c, type))
        cerror(c, loc, "'_Atomic'-qualified array type");
    if (funcdef && (threadp || sc == SC_AUTO || sc == SC_REGISTER ||
                    sc == SC_TYPEDEF)) {
        if (sc == SC_AUTO)
            cpedwarn(c, loc, filescope ? "" : "pedantic",
                     "function definition declared 'auto'");
        if (sc == SC_REGISTER)
            cerror(c, loc, "function definition declared 'register'");
        if (sc == SC_TYPEDEF)
            cerror(c, loc, "function definition declared 'typedef'");
        if (threadp)
            cerror(c, loc, "function definition declared '%s'",
                   sp->thread_gnu ? "__thread" : "_Thread_local");
        threadp = false;
        if (sc == SC_AUTO || sc == SC_REGISTER || sc == SC_TYPEDEF)
            sc = SC_NONE;
    } else if (ctx != DC_NORMAL && (sc != SC_NONE || threadp)) {
        if (ctx == DC_PARM && sc == SC_REGISTER) {
            /* allowed */
        } else {
            if (ctx == DC_FIELD) {
                if (name)
                    cerror(c, loc, "storage class specified for structure "
                           "field '%s'", cident(c, name));
                else
                    cerror(c, loc, "storage class specified for structure "
                           "field");
            } else if (ctx == DC_PARM) {
                if (name)
                    cerror(c, loc, "storage class specified for parameter "
                           "'%s'", cident(c, name));
                else
                    cerror(c, loc, "storage class specified for unnamed "
                           "parameter");
            } else if (c->cd_clit)
                cpedantic(c, tloc(c, c->cd_clit), "ISO C forbids storage "
                          "class specifiers in compound literals before C2X");
            else
                cerror(c, loc, "storage class specified for typename");
            sc = SC_NONE;
            threadp = false;
        }
    } else if (sc == SC_EXTERN && initialized && !funcdef) {
        if (filescope)
            cwarn(c, loc, "", "'%s' initialized and declared 'extern'",
                  cident(c, name));
        else
            cerror(c, loc, "'%s' has both 'extern' and initializer",
                   cident(c, name));
    } else if (filescope) {
        if (sc == SC_AUTO)
            cerror(c, loc, "file-scope declaration of '%s' specifies 'auto'",
                   cident(c, name));
        if (sc == SC_REGISTER)
            cpedantic(c, iloc(c, ltok), "file-scope declaration of '%s' "
                      "specifies 'register'", cident(c, name));
    } else {
        if (sc == SC_EXTERN && funcdef)
            cerror(c, loc, "nested function '%s' declared 'extern'",
                   cident(c, name));
        else if (threadp && sc == SC_NONE) {
            cerror(c, loc, "function-scope '%s' implicitly auto and declared "
                   "'%s'", cident(c, name),
                   sp->thread_gnu ? "__thread" : "_Thread_local");
            threadp = false;
        }
    }

    chain_get(c, top, &ch);
    for (ci = 0; ci < ch.n; ci++) {
        uint32_t dn = ch.p[ci];
        unsigned tg = ntag(c, dn);
        if (is_err(c, type))
            continue;
        if (array_ptr_quals || array_ptr_attrs || array_parm_static) {
            cerror(c, loc, "static or type qualifiers in non-parameter array "
                   "declarator");
            array_ptr_quals = 0;
            array_ptr_attrs = false;
            array_parm_static = false;
        }
        if (tg == N_ARRAY) {
            uint32_t sz = array_size_node(c, dn);
            bool itype_none = true, unspec;
            bool vla = false;
            uint64_t n = 0;
            const Node *an = cnode(c, dn);
            array_ptr_quals = quals_of(c, dn);
            array_ptr_attrs = has_child_attr(c, dn);
            array_parm_static = (an->flags & NF_STATIC) != 0;
            unspec = (an->flags & NF_STAR) && !(c->cv[dn] & 1);
            array_parm_vla_unspec = unspec;
            if (is_void(c, type)) {
                if (name)
                    cerror(c, loc, "declaration of '%s' as array of voids",
                           cident(c, name));
                else
                    cerror(c, loc, "declaration of type name as array of "
                           "voids");
                type = ERRT;
            }
            if (is_func(c, type)) {
                if (name)
                    cerror(c, loc, "declaration of '%s' as array of "
                           "functions", cident(c, name));
                else
                    cerror(c, loc, "declaration of type name as array of "
                           "functions");
                type = ERRT;
            }
            if (c->opt.pedantic && flex_struct(c, type))
                cpedantic(c, loc, "invalid use of structure with flexible "
                          "array member");
            if (sz != NO_NODE && c->ck[sz] == K_ERR &&
                is_err(c, c->ty[sz]))
                type = ERRT;
            if (is_err(c, type))
                continue;
            if (sz != NO_NODE) {
                bool size_int_const = node_int_cst(c, sz);
                bool folded = c->ck[sz] == K_ICE || c->ck[sz] == K_FOLD;
                bool this_varies = false;
                int64_t sv = 0;
                TypeId st = c->ty[sz];
                bool bad_size = false;
                if (c->ck[sz] == K_ERR) {
                    type = ERRT;
                    continue;
                }
                if (!type_is_integer(TT, st)) {
                    if (name)
                        cerror(c, loc, "size of array '%s' has non-integer "
                               "type", cident(c, name));
                    else
                        cerror(c, loc, "size of unnamed array has non-integer "
                               "type");
                    bad_size = true;
                } else if (!type_is_complete(TT, st)) {
                    if (name)
                        cerror(c, loc, "size of array '%s' has incomplete "
                               "type", cident(c, name));
                    else
                        cerror(c, loc, "size of unnamed array has incomplete "
                               "type");
                    bad_size = true;
                }
                if (bad_size) {
                    n = 1;
                    size_int_const = true;
                    folded = true;
                } else if (folded) {
                    sv = cexpr_sval(c, sz);
                    if (sv < 0 && !type_is_signed(TT, st)) {
                        /* too large for an unsigned constant */
                        sv = INT64_MAX;
                        n = (uint64_t)cexpr_sval(c, sz);
                    } else
                        n = (uint64_t)sv;
                }
                if (folded && !bad_size && sv == 0 && n == 0) {
                    if (name)
                        cpedantic(c, loc, "ISO C forbids zero-size array '%s'",
                                  cident(c, name));
                    else
                        cpedantic(c, loc, "ISO C forbids zero-size array");
                }
                if (folded) {
                    if (sv < 0) {
                        if (name)
                            cerror(c, loc, "size of array '%s' is negative",
                                   cident(c, name));
                        else
                            cerror(c, loc, "size of unnamed array is "
                                   "negative");
                        n = 1;
                        sv = 1;
                        size_int_const = true;
                    }
                    if (!size_int_const) {
                        if ((ctx == DC_NORMAL || ctx == DC_FIELD) && filescope)
                            cpedwarn(c, iloc(c, ltok), "", "variably modified "
                                     "'%s' at file scope",
                                     name ? cident(c, name) : "");
                        else
                            this_varies = size_varies = true;
                        if (this_varies) {
                            if (name)
                                cwarn(c, iloc(c, ltok), "vla", "variable "
                                      "length array '%s' is used",
                                      cident(c, name));
                            else
                                cwarn(c, iloc(c, ltok), "vla", "variable "
                                      "length array is used");
                        }
                    }
                } else if ((ctx == DC_NORMAL || ctx == DC_FIELD) && filescope) {
                    cerror(c, loc, "variably modified '%s' at file scope",
                           name ? cident(c, name) : "");
                    n = 1;
                    folded = true;
                } else {
                    this_varies = size_varies = true;
                    if (name)
                        cwarn(c, iloc(c, ltok), "vla", "variable length array "
                              "'%s' is used", cident(c, name));
                    else
                        cwarn(c, iloc(c, ltok), "vla", "variable length array "
                              "is used");
                }
                if (this_varies)
                    vla = true;
                if (!vla && folded && n > (uint64_t)INT64_MAX) {
                    if (name)
                        cerror(c, loc, "size of array '%s' is too large",
                               cident(c, name));
                    else
                        cerror(c, loc, "size of unnamed array is too large");
                    type = ERRT;
                    continue;
                }
                itype_none = false;
                if (!vla && !folded)
                    itype_none = true;
            } else if (ctx == DC_FIELD) {
                /* a flexible array member (or [*]) */
                if (array_parm_vla_unspec) {
                    size_varies = true;
                    vla = true;
                    itype_none = false;
                }
            } else if (ctx == DC_PARM) {
                if (array_parm_vla_unspec) {
                    vla = true;
                    size_varies = true;
                    itype_none = false;
                }
            } else if (ctx == DC_TYPENAME) {
                if (array_parm_vla_unspec) {
                    cwarn(c, iloc(c, ltok), "", "'[*]' not in a declaration");
                    vla = true;
                    size_varies = true;
                    itype_none = false;
                }
            }
            if (!type_is_complete(TT, type)) {
                Diagnostic *dd = cerror_d(c, loc, "array type has incomplete "
                                          "element type %s", type_q(TT, type));
                if (is_arr(c, type)) {
                    if (name)
                        cnote(c, dd, loc, "declaration of '%s' as "
                              "multidimensional array must have bounds for "
                              "all dimensions except the first",
                              cident(c, name));
                    else
                        cnote(c, dd, loc, "declaration of multidimensional "
                              "array must have bounds for all dimensions "
                              "except the first");
                }
                type = ERRT;
            } else {
                if (!vla && type_is_vm(TT, type))
                    vla = true;    /* an array of variably modified type */
                if (vla)
                    type = type_vla(TT, type);
                else if (sz != NO_NODE || (unspec && !vla))
                    type = type_array(TT, type, n);
                else
                    type = type_array_incomplete(TT, type);
                if (!vla && sz != NO_NODE &&
                    !valid_array_size(c, loc, type_base(TT, type), n, name))
                    type = ERRT;
            }
            (void)itype_none;
            if (ctx != DC_PARM && (array_ptr_quals || array_ptr_attrs ||
                                   array_parm_static)) {
                cerror(c, loc, "static or type qualifiers in non-parameter "
                       "array declarator");
                array_ptr_quals = 0;
                array_ptr_attrs = false;
                array_parm_static = false;
            }
        } else if (tg == N_FUNC) {
            bool really = funcdef && inner_decl(c, dn) != NO_NODE &&
                          ntag(c, inner_decl(c, dn)) == N_NAME;
            if (is_func(c, type)) {
                if (name)
                    cerror(c, loc, "'%s' declared as function returning a "
                           "function", cident(c, name));
                else
                    cerror(c, loc, "type name declared as function returning a "
                           "function");
                type = TYPE_B(INT);
            }
            if (is_arr(c, type)) {
                if (name)
                    cerror(c, loc, "'%s' declared as function returning an "
                           "array", cident(c, name));
                else
                    cerror(c, loc, "type name declared as function returning "
                           "an array");
                type = TYPE_B(INT);
            }
            if (type_quals) {
                SrcLoc sl = 0;
                unsigned k;
                for (k = 0; k < 4; k++)
                    if (sp->qual_tok[k]) {
                        SrcLoc l = tloc(c, sp->qual_tok[k] - 1);
                        if (!sl || l < sl)
                            sl = l;
                    }
                if (!sl)
                    sl = sp->kind == TSK_TYPEDEF ? sp->type_loc : 0;
                if (!sl)
                    sl = loc;
                if (is_void(c, type) && really)
                    cpedwarn(c, sl, "", "function definition has qualified "
                             "void return type");
                else
                    cwarn(c, sl, "ignored-qualifiers", "type qualifiers "
                          "ignored on function return type");
                type = qualify(c, type, type_quals, ltok);
            }
            type_quals = 0;
            {
                TypeId ft;
                fill_func_type(c, dn, really, ltok, type, &ft);
                type = ft;
            }
            size_varies = false;
        } else { /* N_PTR */
            uint32_t pn = dn;
            if ((type_quals & TQ_ATOMIC) && is_func(c, type)) {
                cerror(c, loc, "'_Atomic'-qualified function type");
                type_quals &= ~(unsigned)TQ_ATOMIC;
            } else if (is_func(c, type) && type_quals)
                cpedantic(c, loc, "ISO C forbids qualified function types");
            if (type_quals)
                type = qualify(c, type, type_quals, ltok);
            size_varies = false;
            type = type_ptr(TT, type);
            type_quals = quals_of(c, pn);
        }
    }
    chain_free(&ch);

    if (bitfield) {
        wbits = check_bitfield(c, loc, &type, width_node, name, ltok);
        if (type_quals & TQ_ATOMIC) {
            if (name)
                cerror(c, loc, "bit-field '%s' has atomic type",
                       cident(c, name));
            else
                cerror(c, loc, "bit-field has atomic type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        }
    }
    g->width = wbits;
    if (sp->alignas_seen) {
        if (sc == SC_TYPEDEF)
            cerror(c, loc, "alignment specified for typedef '%s'",
                   cident(c, name));
        else if (sc == SC_REGISTER)
            cerror(c, loc, "alignment specified for 'register' object '%s'",
                   cident(c, name));
        else if (ctx == DC_PARM) {
            if (name)
                cerror(c, loc, "alignment specified for parameter '%s'",
                       cident(c, name));
            else
                cerror(c, loc, "alignment specified for unnamed parameter");
        } else if (bitfield) {
            if (name)
                cerror(c, loc, "alignment specified for bit-field '%s'",
                       cident(c, name));
            else
                cerror(c, loc, "alignment specified for unnamed bit-field");
        } else if (is_func(c, type))
            cerror(c, loc, "alignment specified for function '%s'",
                   cident(c, name));
        else if (sp->align && !is_err(c, type)) {
            if (sp->alignas_seen && sp->align < type_align(TT, type) &&
                sp->attrs.aligned < sp->align) {
                if (name)
                    cerror(c, loc, "'_Alignas' specifiers cannot reduce "
                           "alignment of '%s'", cident(c, name));
                else
                    cerror(c, loc, "'_Alignas' specifiers cannot reduce "
                           "alignment of unnamed field");
                g->s.align = 0;
            } else
                g->s.align = (uint16_t)sp->align;
        }
    }
    if (!g->s.align && sp->align && !sp->alignas_seen)
        g->s.align = (uint16_t)sp->align;

    if (sc == SC_TYPEDEF) {
        if ((type_quals & TQ_ATOMIC) && is_func(c, type)) {
            cerror(c, loc, "'_Atomic'-qualified function type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        } else if (is_func(c, type) && type_quals)
            cpedantic(c, loc, "ISO C forbids qualified function types");
        if (type_quals)
            type = qualify(c, type, type_quals, ltok);
        if (sp->is_inline)
            cpedwarn(c, loc, "", "typedef '%s' declared 'inline'",
                     cident(c, name));
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "typedef '%s' declared '_Noreturn'",
                     cident(c, name));
        g->s.name = name;
        g->s.loc = loc;
        g->what = GD_TYPEDEF;
        g->ty = type;
        g->s.kind = CS_TYPEDEF;
        g->s.sc = SC_TYPEDEF;
        g->s.ty = type;
        return;
    }
    if (ctx == DC_TYPENAME) {
        if ((type_quals & TQ_ATOMIC) && is_func(c, type)) {
            cerror(c, loc, "'_Atomic'-qualified function type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        } else if (is_func(c, type) && type_quals)
            cpedantic(c, loc, "ISO C forbids const or volatile function "
                      "types");
        if (type_quals)
            type = qualify(c, type, type_quals, ltok);
        g->what = GD_TYPENAME;
        g->ty = type;
        return;
    }
    if (ctx == DC_FIELD && !is_err(c, type) && type_is_vm(TT, type))
        cpedantic(c, loc, "a member of a structure or union cannot have a "
                  "variably modified type");
    if (is_void(c, type) && ctx != DC_PARM &&
        !((ctx != DC_FIELD && !is_func(c, type)) &&
          (sc == SC_EXTERN ||
           (filescope && !(sc == SC_STATIC || sc == SC_REGISTER))))) {
        cerror(c, loc, "variable or field '%s' declared void",
               name ? cident(c, name) : "");
        type = TYPE_B(INT);
    }
    g->s.name = name;
    g->s.loc = loc;
    g->s.sc = (uint8_t)sc;
    if (ctx == DC_PARM) {
        bool arrp = false;
        if (is_arr(c, type)) {
            TypeId e = type_base(TT, type);
            if (type_quals)
                e = qualify(c, e, type_quals, ltok);
            type = type_ptr(TT, e);
            type_quals = array_ptr_quals;
            if (type_quals)
                type = qualify(c, type, type_quals, ltok);
            if (array_ptr_attrs)
                cwarn(c, loc, "attributes", "attributes in parameter array "
                      "declarator ignored");
            arrp = true;
        } else if (is_func(c, type)) {
            if (type_quals & TQ_ATOMIC) {
                cerror(c, loc, "'_Atomic'-qualified function type");
                type_quals &= ~(unsigned)TQ_ATOMIC;
            } else if (type_quals)
                cpedantic(c, loc, "ISO C forbids qualified function types");
            type = type_ptr(TT, type | type_quals);
            type_quals = 0;
        } else if (type_quals)
            type = qualify(c, type, type_quals, ltok);
        if (sp->is_inline)
            cpedwarn(c, loc, "", "parameter '%s' declared 'inline'",
                     name ? cident(c, name) : "");
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "parameter '%s' declared '_Noreturn'",
                     name ? cident(c, name) : "");
        g->what = GD_PARM;
        g->array_param = arrp;
        g->ty = type;
        g->s.kind = CS_OBJ;
        g->s.flags = CSF_PARAM | CSF_DEFINED | (arrp ? CSF_ARRAY_PARM : 0);
        g->s.ty = type;
        return;
    }
    if (ctx == DC_FIELD) {
        if (is_func(c, type)) {
            cerror(c, loc, "field '%s' declared as a function",
                   name ? cident(c, name) : "");
            type = type_ptr(TT, type);
        } else if (!is_err(c, type) &&
                   !(type_is_complete(TT, type) ||
                     (tkind(c, type) == TY_ARRAY &&
                      (type_ent(TT, type_canon(TT, type))->flags &
                       TF_INCOMPLETE)))) {
            if (name)
                cerror(c, loc, "field '%s' has incomplete type",
                       cident(c, name));
            else
                cerror(c, loc, "unnamed field has incomplete type");
            type = ERRT;
        }
        if (!is_err(c, type))
            type = qualify(c, type, type_quals, ltok);
        g->what = GD_FIELD;
        g->ty = type;
        g->s.kind = CS_OBJ;
        g->s.ty = type;
        return;
    }
    if (is_func(c, type)) {
        bool fs = !filescope;
        if (sc == SC_REGISTER || threadp) {
            cerror(c, loc, "invalid storage class for function '%s'",
                   cident(c, name));
        } else if (fs) {
            if (sc == SC_AUTO)
                cpedantic(c, loc, "invalid storage class for function '%s'",
                          cident(c, name));
            else if (sc == SC_STATIC) {
                cerror(c, loc, "invalid storage class for function '%s'",
                       cident(c, name));
                if (funcdef)
                    sc = SC_NONE;
                else {
                    g->what = GD_NONE;
                    return;
                }
            }
        }
        g->s.sc = (uint8_t)sc;
        if (type_quals & TQ_ATOMIC) {
            cerror(c, loc, "'_Atomic'-qualified function type");
            type_quals &= ~(unsigned)TQ_ATOMIC;
        } else if (type_quals)
            cpedantic(c, loc, "ISO C forbids qualified function types");
        if (sc == SC_AUTO && fs)
            g->s.flags &= ~(unsigned)CSF_DECL_EXTERNAL;
        else if (sp->is_inline && sc != SC_STATIC) {
            if (sc != SC_EXTERN)
                g->s.flags |= CSF_DECL_EXTERNAL;
        } else if (!initialized)
            g->s.flags |= CSF_DECL_EXTERNAL;
        g->s.linkage = (sc == SC_STATIC || sc == SC_AUTO) ? LK_INTERNAL
                                                          : LK_EXTERNAL;
        if (name && !strcmp(cident(c, name), "main")) {
            if (sp->is_inline)
                cpedwarn(c, loc, "", "cannot inline function 'main'");
            if (sp->is_noreturn)
                cpedwarn(c, loc, "", "'main' declared '_Noreturn'");
        } else {
            if (sp->is_inline)
                g->s.flags |= CSF_INLINE;
            if (sp->is_noreturn) {
                cpedantic(c, loc, "ISO C99 does not support '_Noreturn'");
                g->s.flags |= CSF_NORETURN;
            }
        }
        if (sp->attrs.noreturn)
            g->s.flags |= CSF_NORETURN;
        g->what = GD_FUNC;
        g->s.kind = CS_FUNC;
    } else {
        bool extern_ref = !initialized && sc == SC_EXTERN;
        if (!is_err(c, type))
            type = qualify(c, type, type_quals, ltok);
        if (extern_ref && !filescope) {
            uint32_t ge = name && name < c->nidents ? c->ext[name] : 0;
            uint32_t vis = lookup_ord(c, name);
            csum_read_ext(c, name);
            if (ge && ge - 1 != vis && csym(c, ge - 1)->kind == CS_OBJ &&
                csym(c, ge - 1)->linkage != LK_EXTERNAL)
                cerror(c, loc, "variable previously declared 'static' "
                       "redeclared 'extern'");
        }
        if (sp->is_inline)
            cpedwarn(c, loc, "", "variable '%s' declared 'inline'",
                     cident(c, name));
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "variable '%s' declared '_Noreturn'",
                     cident(c, name));
        if (sc == SC_EXTERN)
            g->s.flags |= CSF_DECL_EXTERNAL;
        if (filescope) {
            g->s.linkage = sc != SC_STATIC ? LK_EXTERNAL : LK_INTERNAL;
            if (!extern_ref)
                g->s.flags |= CSF_TREE_STATIC;
        } else {
            if (sc == SC_STATIC)
                g->s.flags |= CSF_TREE_STATIC;
            g->s.linkage = extern_ref ? LK_EXTERNAL : LK_NONE;
        }
        if (threadp)
            g->s.flags |= CSF_THREAD;
        g->what = GD_VAR;
        g->s.kind = CS_OBJ;
    }
    if ((sc == SC_EXTERN || (sc == SC_NONE && is_func(c, type) && !funcdef)) &&
        type_is_vm(TT, type)) {
        if (is_func(c, type))
            cerror(c, loc, "non-nested function with variably modified type");
        else
            cerror(c, loc, "object with variably modified type must have no "
                   "linkage");
    }
    g->ty = type;
    g->s.ty = type;
    (void)first_kind;
}

/* ---- symbols: duplicate declarations, pushdecl ---------------------------- */

static bool sym_public(const CSym *s)
{
    return s->linkage == LK_EXTERNAL;
}

static bool sym_external(const CSym *s)
{
    return (s->flags & (CSF_DECL_EXTERNAL | CSF_IMPLICIT)) != 0;
}

static bool sym_defined(const CSym *s)
{
    return (s->flags & CSF_DEFINED) != 0;
}

/* DECL_FILE_SCOPE_P: the symbol is a persistent one. */
static bool ref_file_scope(uint32_t ref)
{
    return !(ref & SYM_LOCAL);
}

/* gcc's tree code of a symbol. */
static int dcode(const CSym *s)
{
    return s->kind * 2 + (s->kind == CS_OBJ && (s->flags & CSF_PARAM) ? 1 : 0);
}

static bool extern_inline(const CSym *s)
{
    return (s->flags & CSF_INLINE) && sym_external(s);
}

static const char *sname(Checker *c, const CSym *s)
{
    return cident(c, s->name);
}

/* The type a typedef names (a typedef symbol's ty is its own entry). */
static TypeId typedef_under(Checker *c, TypeId t)
{
    if (type_kind(TT, t) == TY_TYPEDEF)
        return type_ent(TT, t)->base | TYPE_QUALS(t);
    return t;
}

/* comptypes: a K&R definition's parameter types only ask for a warning
 * when they disagree with a prototype (gcc's value 2), not for an error. */
static bool compat_gcc(Checker *c, TypeId a, TypeId b)
{
    TypeId ca = type_canon(TT, a), cb = type_canon(TT, b);
    if (type_kind(TT, ca) == TY_FUNC && type_kind(TT, cb) == TY_FUNC) {
        const TypeEnt *ea = type_ent(TT, ca), *eb = type_ent(TT, cb);
        bool npa = ea->flags & TF_NOPROTO, npb = eb->flags & TF_NOPROTO;
        if (npa != npb) {
            const TypeEnt *np = npa ? ea : eb;
            TypeId stripped = type_func(TT, np->base, NULL, 0, TF_NOPROTO);
            return type_compatible(TT, npa ? stripped : a,
                                   npa ? b : stripped);
        }
    }
    return type_compatible(TT, a, b);
}

/* The note of locate_old_decl. */
static void locate_old_decl(Checker *c, Diagnostic *d, const CSym *o)
{
    if (!d)
        return;
    if (sym_defined(o) || o->kind == CS_ENUMCONST)
        cnote(c, d, o->loc, "previous definition of '%s' with type %s",
              sname(c, o), type_q(TT, o->ty));
    else if (o->flags & CSF_IMPLICIT)
        cnote(c, d, o->loc, "previous implicit declaration of '%s' with type "
              "%s", sname(c, o), type_q(TT, o->ty));
    else
        cnote(c, d, o->loc, "previous declaration of '%s' with type %s",
              sname(c, o), type_q(TT, o->ty));
}

/* Would conversion of the two types differ only by an enum against an
 * integer type somewhere (gcc's enum_and_int_p)? */
static bool enum_int_pair(Checker *c, TypeId a, TypeId b)
{
    TypeKind ka, kb;
    a = type_canon(TT, a);
    b = type_canon(TT, b);
    ka = tkind(c, a);
    kb = tkind(c, b);
    if (ka == TY_ENUM && kb != TY_ENUM && type_is_integer(TT, b))
        return true;
    if (kb == TY_ENUM && ka != TY_ENUM && type_is_integer(TT, a))
        return true;
    if (ka != kb)
        return false;
    if (ka == TY_PTR || ka == TY_ARRAY)
        return enum_int_pair(c, type_base(TT, a), type_base(TT, b));
    if (ka == TY_FUNC) {
        uint32_t n = (uint32_t)type_ent(TT, a)->n, m = (uint32_t)type_ent(TT, b)->n, k;
        if (enum_int_pair(c, type_base(TT, a), type_base(TT, b)))
            return true;
        for (k = 0; k < n && k < m; k++)
            if (enum_int_pair(c, type_params(TT, a)[k], type_params(TT, b)[k]))
                return true;
    }
    return false;
}

static void arglist_conflict(Checker *c, Diagnostic *d, const CSym *nw,
                             const CSym *o, TypeId newtype, TypeId oldtype)
{
    TypeId t;
    uint32_t n, k;
    const TypeId *ps;
    if (!d || o->kind != CS_FUNC ||
        !type_compatible(TT, type_base(TT, oldtype), type_base(TT, newtype)) ||
        !((!is_prototype(c, oldtype) && !sym_defined(o)) ||
          (!is_prototype(c, newtype) && !sym_defined(nw))))
        return;
    t = is_prototype(c, oldtype) ? oldtype : newtype;
    if (!is_prototype(c, t))
        return;
    t = type_canon(TT, t);
    if (type_ent(TT, t)->flags & TF_VARIADIC) {
        cnote(c, d, iloc(c, c->cd_ltok), "a parameter list with an ellipsis "
              "cannot match an empty parameter name list declaration");
        return;
    }
    n = (uint32_t)type_ent(TT, t)->n;
    ps = type_params(TT, t);
    for (k = 0; k < n; k++)
        if (type_default_promote(TT, ps[k]) != ps[k]) {
            cnote(c, d, iloc(c, c->cd_ltok), "an argument type that has a "
                  "default promotion cannot match an empty parameter name "
                  "list declaration");
            return;
        }
}

static TypeId main_variant(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    return (TYPE_QUALS(k) & TQ_ATOMIC) ? TYPE_UNQUAL(k) | TQ_ATOMIC
                                       : TYPE_UNQUAL(k);
}

/* validate_proto_after_old_defn: *d gets the error's diagnostic. */
static bool validate_proto_after_old_defn(Checker *c, const CSym *nw,
                                          const CSym *o, Diagnostic **d)
{
    TypeId ot = type_canon(TT, o->ty), nt = type_canon(TT, nw->ty);
    uint32_t on = (uint32_t)type_ent(TT, ot)->n;
    uint32_t nn = (uint32_t)type_ent(TT, nt)->n, k;
    const TypeId *op = type_params(TT, ot), *np = type_params(TT, nt);
    *d = NULL;
    for (k = 0; k < on || k < nn; k++) {
        TypeId a, b;
        if (k >= on) {
            *d = cerror_d(c, nw->loc, "prototype for '%s' declares more "
                          "arguments than previous old-style definition",
                          sname(c, nw));
            return false;
        }
        if (k >= nn) {
            *d = cerror_d(c, nw->loc, "prototype for '%s' declares fewer "
                          "arguments than previous old-style definition",
                          sname(c, nw));
            return false;
        }
        if (is_err(c, op[k]) || is_err(c, np[k]))
            return false;
        a = main_variant(c, op[k]);
        b = main_variant(c, np[k]);
        if (!type_compatible(TT, a, b)) {
            *d = cerror_d(c, nw->loc, "prototype for '%s' declares argument "
                          "%u with incompatible type", sname(c, nw), k + 1);
            return false;
        }
    }
    cwarn(c, nw->loc, "", "prototype for '%s' follows non-prototype "
          "definition", sname(c, nw));
    return true;
}

/* diagnose_mismatched_decls: are the two consistent?  nfile/ofile:
 * DECL_FILE_SCOPE_P of the new and the old declaration. */
static bool diagnose_mismatched(Checker *c, CSym *nw, bool nfile,
                                const CSym *o, bool ofile,
                                bool new_implicit_int, TypeId *newtypep,
                                TypeId *oldtypep)
{
    TypeId newtype = nw->ty, oldtype = o->ty;
    Diagnostic *d, *wd = NULL;
    bool pedwarned = false, enum_and_int = false;
    *newtypep = newtype;
    *oldtypep = oldtype;
    if (is_err(c, oldtype) || is_err(c, newtype))
        return false;
    if (dcode(o) != dcode(nw)) {
        d = cerror_d(c, nw->loc, "'%s' redeclared as different kind of "
                     "symbol", sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    if (o->kind == CS_ENUMCONST) {
        d = cerror_d(c, nw->loc, "redeclaration of enumerator '%s'",
                     sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    {
        TypeId a = o->kind == CS_TYPEDEF ? typedef_under(c, oldtype) : oldtype;
        TypeId b = o->kind == CS_TYPEDEF ? typedef_under(c, newtype) : newtype;
        if (!compat_gcc(c, a, b)) {
            if (nw->kind == CS_FUNC && sym_defined(nw) &&
                is_void(c, type_base(TT, oldtype)) &&
                type_canon(TT, type_base(TT, newtype)) == TYPE_B(INT) &&
                new_implicit_int && !sym_defined(o)) {
                d = cpedwarn(c, iloc(c, c->cd_ltok), "", "conflicting types "
                             "for '%s'", sname(c, nw));
                pedwarned = d != NULL;
                wd = d;
                nw->ty = newtype = oldtype;
                *newtypep = newtype;
            } else if (nw->kind == CS_FUNC &&
                       is_void(c, type_base(TT, newtype)) &&
                       type_canon(TT, type_base(TT, oldtype)) == TYPE_B(INT) &&
                       (o->flags & CSF_IMPLICIT) && !sym_defined(o)) {
                d = cpedwarn(c, iloc(c, c->cd_ltok), "", "conflicting types "
                             "for '%s'; have %s", sname(c, nw),
                             type_q(TT, newtype));
                pedwarned = d != NULL;
                wd = d;
                oldtype = newtype;
                *oldtypep = oldtype;
            } else {
                if (TYPE_QUALS(b) != TYPE_QUALS(a))
                    d = cerror_d(c, nw->loc, "conflicting type qualifiers for "
                                 "'%s'", sname(c, nw));
                else
                    d = cerror_d(c, nw->loc, "conflicting types for '%s'; have "
                                 "%s", sname(c, nw), type_q(TT, newtype));
                arglist_conflict(c, d, nw, o, newtype, oldtype);
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (nw->kind != CS_TYPEDEF && enum_int_pair(c, a, b) &&
                   type_canon(TT, a) != type_canon(TT, b)) {
            enum_and_int = true;
            wd = cwarn_d(c, DL_WARNING, nw->loc, "enum-int-mismatch",
                         "conflicting types for '%s' due to enum/integer "
                         "mismatch; have %s", sname(c, nw),
                         type_q(TT, newtype));
        }
    }
    (void)enum_and_int;
    if (nw->kind == CS_TYPEDEF) {
        TypeId a = typedef_under(c, oldtype), b = typedef_under(c, newtype);
        if (type_canon(TT, a) != type_canon(TT, b)) {
            d = cerror_d(c, nw->loc, "redefinition of typedef '%s' with "
                         "different type", sname(c, nw));
            locate_old_decl(c, d, o);
            return false;
        }
        if (type_is_vm(TT, b)) {
            d = cerror_d(c, nw->loc, "redefinition of typedef '%s' with "
                         "variably modified type", sname(c, nw));
            locate_old_decl(c, d, o);
        } else {
            d = cpedantic(c, nw->loc, "redefinition of typedef '%s'",
                          sname(c, nw));
            locate_old_decl(c, d, o);
        }
        return true;
    } else if (nw->kind == CS_FUNC) {
        if (sym_defined(nw)) {
            if (sym_defined(o)) {
                d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (sym_defined(o) && !is_prototype(c, oldtype) &&
                   is_prototype(c, newtype) &&
                   type_ent(TT, type_canon(TT, oldtype))->n) {
            if (!validate_proto_after_old_defn(c, nw, o, &d)) {
                locate_old_decl(c, d, o);
                return false;
            }
        }
        if (sym_public(o) && !sym_public(nw)) {
            if (!extern_inline(o)) {
                d = cerror_d(c, nw->loc, "static declaration of '%s' follows "
                             "non-static declaration", sname(c, nw));
                locate_old_decl(c, d, o);
            }
            return false;
        } else if (sym_public(nw) && !sym_public(o)) {
            if (!ofile) {
                d = cerror_d(c, nw->loc, "non-static declaration of '%s' "
                             "follows static declaration", sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        }
    } else if (nw->kind == CS_OBJ && !(nw->flags & CSF_PARAM)) {
        if ((nw->flags & CSF_THREAD) != (o->flags & CSF_THREAD)) {
            if (nw->flags & CSF_THREAD)
                d = cerror_d(c, nw->loc, "thread-local declaration of '%s' "
                             "follows non-thread-local declaration",
                             sname(c, nw));
            else
                d = cerror_d(c, nw->loc, "non-thread-local declaration of "
                             "'%s' follows thread-local declaration",
                             sname(c, nw));
            locate_old_decl(c, d, o);
            return false;
        }
        if (sym_defined(nw) && sym_defined(o)) {
            d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
            locate_old_decl(c, d, o);
            return false;
        }
        if (nfile && sym_public(nw) != sym_public(o)) {
            if (sym_external(nw)) {
                if (!ofile) {
                    d = cerror_d(c, nw->loc, "extern declaration of '%s' "
                                 "follows declaration with no linkage",
                                 sname(c, nw));
                    locate_old_decl(c, d, o);
                    return false;
                }
            } else {
                if (sym_public(nw))
                    d = cerror_d(c, nw->loc, "non-static declaration of '%s' "
                                 "follows static declaration", sname(c, nw));
                else
                    d = cerror_d(c, nw->loc, "static declaration of '%s' "
                                 "follows non-static declaration",
                                 sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (!nfile) {
            if (sym_external(nw)) {
                /* extern with initializer at block scope: an error already */
            } else if (sym_external(o)) {
                d = cerror_d(c, nw->loc, "declaration of '%s' with no linkage "
                             "follows extern declaration", sname(c, nw));
                locate_old_decl(c, d, o);
            } else {
                d = cerror_d(c, nw->loc, "redeclaration of '%s' with no "
                             "linkage", sname(c, nw));
                locate_old_decl(c, d, o);
            }
            return false;
        }
    }
    if (nw->kind == CS_OBJ && (nw->flags & CSF_PARAM) &&
        !(o->flags & CSF_FWD)) {
        d = cerror_d(c, nw->loc, "redefinition of parameter '%s'",
                     sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    if (!wd && !((nw->flags & CSF_PARAM) && (o->flags & CSF_FWD)) &&
        !(nw->kind == CS_FUNC && sym_defined(nw) && !sym_defined(o)) &&
        !(sym_external(o) && !sym_external(nw)) &&
        !(nw->kind == CS_OBJ && sym_defined(nw) && !sym_defined(o)))
        wd = cwarn_d(c, DL_WARNING, nw->loc, "redundant-decls", "redundant "
                     "redeclaration of '%s'", sname(c, nw));
    if (wd || pedwarned)
        locate_old_decl(c, wd, o);
    return true;
}

/* merge_decls: nw is consistent with o; o becomes the merged declaration. */
static void merge_decls(Checker *c, CSym *nw, CSym *o, TypeId newtype,
                        TypeId oldtype)
{
    bool is_fn = nw->kind == CS_FUNC;
    bool new_def = is_fn && sym_defined(nw);
    bool new_proto = is_fn && is_prototype(c, nw->ty);
    bool old_proto = o->kind == CS_FUNC && is_prototype(c, o->ty);
    bool ext_new = sym_external(nw), pub, ext_final, static_final;
    bool infunc = c->func_sym != SYM_NONE;
    CSym m = *nw;
    if (nw->kind == CS_TYPEDEF)
        m.ty = o->ty;
    else
        m.ty = type_composite(TT, newtype, oldtype);
    if ((!sym_defined(nw) && sym_defined(o)) || (old_proto && !new_proto))
        m.loc = o->loc;
    m.flags |= o->flags & (CSF_DEFINED | CSF_USED | CSF_NORETURN | CSF_THREAD |
                           CSF_INLINE | CSF_BLOCK_EXTERN | CSF_TENTATIVE |
                           CSF_WEAK | CSF_ADDR_WARNED | CSF_DEPRECATED |
                           CSF_UNAVAILABLE | CSF_INNER_COMP);
    if (!m.dep_msg)
        m.dep_msg = o->dep_msg;
    if (!new_def)
        m.flags |= o->flags & (CSF_PROTO_DEF | CSF_KR_DEF);
    m.def_loc = sym_defined(nw) ? nw->loc : o->def_loc;
    if (sym_defined(nw) && !nw->def_loc)
        m.def_loc = nw->loc;
    m.flags &= ~(unsigned)CSF_IMPLICIT;
    if (o->align > m.align)
        m.align = o->align;
    if (is_fn && (nw->flags & CSF_INLINE || o->flags & CSF_INLINE) &&
        (!(nw->flags & CSF_INLINE) || !(o->flags & CSF_INLINE) ||
         !sym_external(o)) &&
        ext_new && !infunc)
        ext_new = false;
    if (new_def && ((nw->flags | o->flags) & CSF_INLINE) && !sym_public(o))
        ext_new = false;
    if (is_fn)
        pub = sym_public(nw) && sym_public(o);
    else
        pub = ext_new ? sym_public(o) : sym_public(nw);
    if (ext_new) {
        ext_final = sym_external(o);
        static_final = (o->flags & CSF_TREE_STATIC) != 0;
    } else {
        ext_final = false;
        static_final = (nw->flags & CSF_TREE_STATIC) != 0;
    }
    m.flags &= ~(unsigned)(CSF_DECL_EXTERNAL | CSF_TREE_STATIC);
    if (ext_final)
        m.flags |= CSF_DECL_EXTERNAL;
    if (static_final)
        m.flags |= CSF_TREE_STATIC;
    if (pub)
        m.linkage = LK_EXTERNAL;
    else {
        uint8_t l = ext_new ? o->linkage : nw->linkage;
        if (o->linkage == LK_INTERNAL || nw->linkage == LK_INTERNAL)
            l = LK_INTERNAL;
        m.linkage = l == LK_EXTERNAL ? LK_INTERNAL : l;
    }
    if (nw->kind == CS_TYPEDEF)
        m.linkage = LK_NONE;
    *o = m;
}

static bool duplicate_decls(Checker *c, CSym *nw, bool nfile, uint32_t oldref,
                            bool implicit_int)
{
    TypeId nt, ot;
    CSym *o = csym(c, oldref);
    if (!diagnose_mismatched(c, nw, nfile, o, ref_file_scope(oldref),
                             implicit_int, &nt, &ot))
        return false;
    merge_decls(c, nw, o, nt, ot);
    return true;
}

static bool fn_pointer_type(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_PTR && is_func(c, type_base(TT, t));
}

static void warn_if_shadowing(Checker *c, const CSym *x)
{
    uint32_t bi;
    if (x->name >= c->nidents)
        return;
    for (bi = c->top[NS_ORD][x->name]; bi; bi = c->log.data[bi - 1].prev) {
        uint32_t ref = c->log.data[bi - 1].ref;
        const CSym *old = csym(c, ref);
        Diagnostic *d = NULL;
        if (old == x)
            continue;
        if (old->flags & CSF_ERROR)
            continue;
        if (old->flags & CSF_PARAM)
            d = cwarn_d(c, DL_WARNING, x->loc, "shadow", "declaration of '%s' "
                        "shadows a parameter", sname(c, x));
        else if (ref_file_scope(ref)) {
            if (old->kind == CS_FUNC && x->kind != CS_FUNC &&
                !fn_pointer_type(c, x->ty))
                continue;
            d = cwarn_d(c, DL_WARNING, x->loc, "shadow", "declaration of '%s' "
                        "shadows a global declaration", sname(c, x));
        } else
            d = cwarn_d(c, DL_WARNING, x->loc, "shadow", "declaration of '%s' "
                        "shadows a previous local", sname(c, x));
        if (d)
            cnote(c, d, old->loc, "shadowed declaration is here");
        break;
    }
}

/* The enclosing bindings of name when a block-scope external declaration
 * of type newty arrives: the first one of a file-scope entity keeps the type
 * it has now, and an incomplete array at file scope completed here (other
 * than to one element) is diagnosed at the end of the file. */
static void outer_bindings(Checker *c, uint32_t name, TypeId newty)
{
    uint32_t bi = c->top[NS_ORD][name];
    bool saved = false;
    TypeId vt = 0;
    bool have_vt = false;
    const TypeEnt *ne = type_ent(TT, type_canon(TT, newty));
    for (; bi; bi = c->log.data[bi - 1].prev) {
        Bind *bd = &c->log.data[bi - 1];
        CSym *s = csym(c, bd->ref);
        bool var_fn = (s->kind == CS_OBJ && !(s->flags & CSF_PARAM)) ||
                      s->kind == CS_FUNC;
        if (!have_vt) {
            vt = bd->ty ? bd->ty - 1 : s->ty;
            have_vt = true;
        }
        if (!var_fn || !ref_file_scope(bd->ref))
            continue;
        if (!saved && !bd->ty) {
            bd->ty = s->ty + 1;
            saved = true;
        } else if (!saved) {
            saved = true;
        }
        if (cbind_scope(c, bi) == SCK_FILE && s->kind == CS_OBJ &&
            (s->flags & CSF_TREE_STATIC) && type_ckind(TT, vt) == TY_ARRAY &&
            !type_is_complete(TT, vt) && ne->kind == TY_ARRAY &&
            !(ne->flags & TF_INCOMPLETE) && ne->n != 1)
            s->flags |= CSF_INNER_COMP;
    }
}

/* The type the innermost binding of name gives symbol s (gcc keeps it per
 * binding in c_binding.u.type; the symbol's own type is the composite of
 * all its declarations). */
static TypeId bound_type(Checker *c, uint32_t name, const CSym *s)
{
    uint32_t t = cbind_type(c, name);
    return t ? t - 1 : s->ty;
}

/* A block-scope external declaration of type newty was merged into symbol
 * ref: the binding it made (bi, an index + 1 into the log) sees only the
 * composite of the type visible before (vt, or none) and newty. */
static void bind_this_type(Checker *c, uint32_t bi, uint32_t ref, TypeId vt,
                           bool have_vt, TypeId newty)
{
    TypeId all = csym(c, ref)->ty, t = all;
    if (!have_vt)
        t = newty;
    else if (!is_err(c, vt) && type_compatible(TT, vt, newty))
        t = type_composite(TT, vt, newty);
    if (t != all || c->log.data[bi - 1].ty)
        c->log.data[bi - 1].ty = t + 1;
}

/* pushdecl: enters x in the current scope, merging it with an earlier
 * declaration of the same entity.  Returns the symbol the name now denotes. */
static uint32_t pushdecl(Checker *c, const CSym *xin, bool implicit_int)
{
    CSym x = *xin;
    uint32_t name = x.name, b, ref;
    bool filescope = cat_file_scope(c);
    bool varfn = (x.kind == CS_OBJ && !(x.flags & CSF_PARAM)) ||
                 x.kind == CS_FUNC;
    bool nfile = c->func_sym == SYM_NONE ||
                 (varfn && sym_public(&x) && !sym_defined(&x));
    bool global = nfile && !(x.flags & CSF_PARAM);
    bool skip = false, pub = sym_public(&x);
    if (!name) {
        if (x.kind == CS_TYPEDEF)
            x.ty = type_typedef(TT, 0, x.ty);
        return csym_new(c, global, &x);
    }
    if (!filescope && varfn && pub)
        x.flags |= CSF_BLOCK_EXTERN;
    csum_decl(c, name, filescope, varfn && pub);
    b = cbound_here(c, NS_ORD, name);
    if (b) {
        uint32_t vis = c->log.data[b - 1].ref, use = vis;
        bool split = varfn && pub && sym_public(csym(c, vis)) && c->ext[name];
        TypeId vt = bound_type(c, name, csym(c, vis)), newty = x.ty;
        if (split)
            use = c->ext[name] - 1;
        if (x.kind == CS_OBJ && type_ckind(TT, newty) == TY_ARRAY &&
            type_is_complete(TT, newty))
            csym(c, vis)->flags &= ~(unsigned)CSF_INNER_COMP;
        if (duplicate_decls(c, &x, nfile, use, implicit_int)) {
            if (split && !is_err(c, newty))
                bind_this_type(c, b, use, vt, true, newty);
            return use;
        }
        skip = true;
    }
    ref = SYM_NONE;
    if (!skip) {
        if (((x.flags & CSF_DECL_EXTERNAL) || filescope) && varfn) {
            uint32_t tb = c->top[NS_ORD][name], visref = SYM_NONE;
            uint32_t e = c->ext[name];
            if (tb) {
                uint32_t r = c->log.data[tb - 1].ref;
                const CSym *vs = csym(c, r);
                if ((vs->kind == CS_FUNC ||
                     (vs->kind == CS_OBJ && !(vs->flags & CSF_PARAM))) &&
                    ref_file_scope(r))
                    visref = r;
            }
            TypeId newty = x.ty;
            TypeId vt = visref != SYM_NONE ? bound_type(c, name, csym(c, visref))
                                           : 0;
            if (!filescope)
                cwarn(c, x.loc, "nested-externs", "nested extern declaration "
                      "of '%s'", sname(c, &x));
            if (e && !filescope && !is_err(c, newty))
                outer_bindings(c, name, newty);
            if (e && duplicate_decls(c, &x, nfile, e - 1, implicit_int)) {
                cbind(c, NS_ORD, name, e - 1);
                if (!filescope && !is_err(c, newty))
                    bind_this_type(c, (uint32_t)c->log.len, e - 1, vt,
                                   visref != SYM_NONE, newty);
                return e - 1;
            } else if (pub) {
                if (visref != SYM_NONE && !e &&
                    duplicate_decls(c, &x, nfile, visref, implicit_int)) {
                    cbind(c, NS_ORD, name, visref);
                    return visref;
                }
                ref = csym_new(c, global, &x);
                c->ext[name] = (ref & ~SYM_LOCAL) + 1;
                if (ref & SYM_LOCAL)
                    c->ext[name] = 0;
            }
        }
        if (!(x.flags & CSF_PARAM))
            warn_if_shadowing(c, &x);
    }
    if (ref == SYM_NONE) {
        if (x.kind == CS_TYPEDEF) {
            SrcFile *sf = srcmgr_file_of(c->sm, x.loc);
            x.ty = type_typedef(TT, name, x.ty);
            if (sf && sf->system_header)
                TT->ents.data[TYPE_IDX(x.ty)].flags |= TF_SYSHDR;
        }
        ref = csym_new(c, global, &x);
    }
    cbind(c, NS_ORD, name, ref);
    return ref;
}

/* ---- the specifier stack, small helpers ------------------------------------ */

static int find_spec(Checker *c, uint32_t specs_node)
{
    size_t k = c->specs.len;
    while (k-- > 0)
        if (c->specs.data[k].node == specs_node)
            return (int)k;
    return -1;
}

/* Drops the specifiers of the consumer node and of what it contains. */
static void pop_specs(Checker *c, uint32_t consumer)
{
    uint32_t f = cfirst(c, consumer);
    while (c->specs.len && vec_last(&c->specs).node >= f)
        c->specs.len--;
}

/* pending_xref_error */
static void pending_xref(Checker *c, Spec *sp)
{
    if (sp->xref_name) {
        cerror(c, sp->xref_loc, "'%s' defined as wrong kind of tag",
               cident(c, sp->xref_name));
        sp->xref_name = 0;
    }
}

/* The token that ends a declarator's text for the parser's lookahead: the
 * first ',' ';' (or '=' with eq, '}' or a closing bracket at depth 0) from
 * the token tok. */
static uint32_t scan_end(Checker *c, uint32_t tok, bool eq)
{
    int depth = 0;
    uint32_t k;
    for (k = tok; k < c->u->ntoks; k++)
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE:
            depth++;
            break;
        case P_RPAREN: case P_RBRACKET:
            if (depth > 0)
                depth--;
            else if (eq)
                break;
            else
                return k;
            break;
        case P_RBRACE:
            if (depth > 0)
                depth--;
            else
                return k;
            break;
        case P_COMMA: case P_SEMI:
            if (depth == 0)
                return k;
            break;
        case P_ASSIGN:
            if (depth == 0 && eq)
                return k;
            break;
        default:
            break;
        }
    return c->u->ntoks ? c->u->ntoks - 1 : 0;
}

static bool is_incomplete_array(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ARRAY &&
           (type_ent(TT, type_canon(TT, t))->flags & TF_INCOMPLETE);
}

/* --dump-types: one line per declaration (see the header). */
static void dump_decl(Checker *c, const CSym *s)
{
    StrBuf sb;
    FILE *f = c->opt.dump;
    const char *pre = "";
    if (!f || c->quiet || !s->name || (s->flags & CSF_PARAM))
        return;
    memset(&sb, 0, sizeof sb);
    if (!cat_file_scope(c) && c->func_sym != SYM_NONE) {
        sb_printf(&sb, "%s:", cident(c, csym(c, c->func_sym)->name));
        pre = sb_cstr(&sb);
    }
    switch (s->kind) {
    case CS_TYPEDEF: {
        StrBuf t;
        memset(&t, 0, sizeof t);
        type_print(TT, &t, typedef_under(c, s->ty));
        fprintf(f, "typedef %s%s = %s\n", pre, sname(c, s), sb_cstr(&t));
        sb_free(&t);
        break;
    }
    case CS_OBJ: {
        StrBuf t;
        memset(&t, 0, sizeof t);
        type_print(TT, &t, s->ty);
        fprintf(f, "var %s%s: %s%s%s%s\n", pre, sname(c, s), sb_cstr(&t),
                s->sc == SC_STATIC ? " static" : "",
                (s->flags & CSF_DECL_EXTERNAL) ? " extern" : "",
                (s->flags & CSF_TENTATIVE) ? " tentative" : "");
        sb_free(&t);
        break;
    }
    case CS_FUNC: {
        StrBuf t;
        memset(&t, 0, sizeof t);
        type_print(TT, &t, s->ty);
        fprintf(f, "func %s%s: %s%s%s%s%s\n", pre, sname(c, s), sb_cstr(&t),
                s->sc == SC_STATIC ? " static" : "",
                s->sc == SC_EXTERN ? " extern" : "",
                (s->flags & CSF_INLINE) ? " inline" : "",
                (s->flags & CSF_DEFINED) ? " defined" : "");
        sb_free(&t);
        break;
    }
    default:
        break;
    }
    sb_free(&sb);
}

/* Which declarations must reach cdecl_finish_object: the first call does the
 * end-of-translation-unit checks, so any declaration will do as its cue. */
static void queue_object(Checker *c, uint32_t ref)
{
    size_t k;
    for (k = 0; k < c->tentative.len; k++)
        if (c->tentative.data[k] == ref)
            return;
    vec_push(&c->tentative, ref);
}

static void ensure_finish_cue(Checker *c, uint32_t ref)
{
    if (!c->tentative.len)
        vec_push(&c->tentative, ref);
}

/* ---- array sizes from initializers (complete_array_type) --------------------- */

static uint32_t strip_parens(Checker *c, uint32_t e)
{
    while (e != NO_NODE && ntag(c, e) == N_PAREN)
        e = first_child(c, e);
    return e;
}

/* complete_array_type for the initializer init (NO_NODE: none); returns
 * gcc's failure code, the completed array type in *out. */
static int complete_array(Checker *c, TypeId type, uint32_t init,
                          bool do_default, TypeId *out)
{
    TypeId cn = type_canon(TT, type), elem = type_base(TT, cn);
    int failure = 0;
    int64_t n = 0;
    *out = type;
    if (init != NO_NODE) {
        uint32_t e = strip_parens(c, init);
        if (ntag(c, init) == N_INIT_LIST) {
            /* cinit.c left the element count in cv */
            if (c->ck[init] == K_ICE) {
                n = (int64_t)c->cv[init];
                if (n == 0 && c->opt.pedantic)
                    failure = 3;
            } else
                n = 1;
        } else if (ntag(c, e) == N_STRING) {
            bool o1, o2;
            uint64_t bytes = type_size(TT, c->ty[e], &o1);
            uint64_t es = type_size(TT, elem, &o2);
            if (o1 && o2 && es)
                n = (int64_t)(bytes / es);
            else
                n = 1;
        } else if (c->ck[init] != K_ERR && !is_err(c, c->ty[init]) &&
                   ntag(c, e) == N_COMPOUND_LIT &&
                   tkind(c, c->ty[e]) == TY_ARRAY &&
                   !is_incomplete_array(c, c->ty[e])) {
            /* the literal is replaced by its constructor */
            n = (int64_t)type_ent(TT, type_canon(TT, c->ty[e]))->n;
        } else {
            if (!(c->ck[init] == K_ERR || is_err(c, c->ty[init])))
                failure = 1;
            n = 1;
        }
    } else {
        failure = 2;
        if (!do_default)
            return failure;
        n = 1;
    }
    *out = type_array(TT, elem, (uint64_t)n);
    return failure;
}

/* ---- shadow_tag: declarations without declarators ---------------------------- */

static void shadow_tag(Checker *c, Spec *sp, int warned, uint32_t ltok)
{
    bool file = cat_file_scope(c);
    bool tagged = sp->kind == TSK_TAGDEF || sp->kind == TSK_TAGFIRSTREF;
    bool anyq = sp->quals != 0;
    SrcLoc il = iloc(c, ltok);
    TypeKind tk = tkind(c, sp->ty);
    if (!sp->default_int && sp->kind != TSK_TYPEDEF) {
        if (!sp->error && (tk == TY_STRUCT || tk == TY_UNION || tk == TY_ENUM)) {
            uint32_t name;
            if (tk == TY_ENUM)
                name = type_enum(TT, sp->ty)->tag;
            else
                name = type_record(TT, sp->ty)->tag;
            if (sp->restrict_q) {
                cerror(c, il, "invalid use of 'restrict'");
                warned = 1;
            }
            if (!name) {
                if (warned != 1 && tk != TY_ENUM) {
                    cpedwarn(c, il, "", "unnamed struct/union that defines no "
                             "instances");
                    warned = 1;
                }
            } else if (!tagged && sp->sc != SC_NONE) {
                if (warned != 1)
                    cpedwarn(c, il, "", "empty declaration with storage class "
                             "specifier does not redeclare tag");
                warned = 1;
                pending_xref(c, sp);
            } else if (!tagged && anyq) {
                if (warned != 1)
                    cpedwarn(c, il, "", "empty declaration with type qualifier "
                             "does not redeclare tag");
                warned = 1;
                pending_xref(c, sp);
            } else if (!tagged && sp->alignas_seen) {
                if (warned != 1)
                    cpedwarn(c, il, "", "empty declaration with '_Alignas' "
                             "does not redeclare tag");
                warned = 1;
                pending_xref(c, sp);
            } else if (!tagged && tk == TY_ENUM) {
                if (warned != 1 &&
                    cpedantic(c, il, "empty declaration of 'enum' type does "
                              "not redeclare tag"))
                    warned = 1;
                pending_xref(c, sp);
            } else {
                sp->xref_name = 0;
                if (!cbound_here(c, NS_TAG, name)) {
                    TypeId t = tk == TY_ENUM
                        ? type_new_enum(TT, name, il)
                        : type_new_record(TT, name, tk == TY_UNION, il);
                    cbind(c, NS_TAG, name, t);
                }
            }
        } else if (warned != 1) {
            cpedwarn(c, il, "", "useless type name in empty declaration");
            warned = 1;
        }
    } else if (warned != 1 && sp->kind == TSK_TYPEDEF) {
        cpedwarn(c, il, "", "useless type name in empty declaration");
        warned = 1;
    }
    sp->xref_name = 0;
    if (sp->is_inline) {
        cerror(c, il, "'inline' in empty declaration");
        warned = 1;
    }
    if (sp->is_noreturn) {
        cerror(c, il, "'_Noreturn' in empty declaration");
        warned = 1;
    }
    if (file && sp->sc == SC_AUTO) {
        cerror(c, il, "'auto' in file-scope empty declaration");
        warned = 1;
    }
    if (file && sp->sc == SC_REGISTER) {
        cerror(c, il, "'register' in file-scope empty declaration");
        warned = 1;
    }
    if (!warned && sp->sc != SC_NONE) {
        cwarn(c, il, "", "useless storage class specifier in empty "
              "declaration");
        warned = 2;
    }
    if (!warned && sp->thread) {
        cwarn(c, il, "", "useless '%s' in empty declaration",
              sp->thread_gnu ? "__thread" : "_Thread_local");
        warned = 2;
    }
    if (!warned && anyq) {
        cwarn(c, il, "", "useless type qualifier in empty declaration");
        warned = 2;
    }
    if (!warned && sp->alignas_seen) {
        cwarn(c, il, "", "useless '_Alignas' in empty declaration");
        warned = 2;
    }

}

/* ---- DECL, DECLARED, INIT_DECL ----------------------------------------------- */

static uint32_t find_declared(Checker *c, uint32_t idecl)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, idecl, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_DECLARED)
            r = k.p[j];
    kids_free(&k);
    return r;
}

static void funcdef_declared(Checker *c, uint32_t declared);
static uint32_t find_child(Checker *c, uint32_t i, unsigned tag);

static void decl_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i);
    if (sn != NO_NODE && ntag(c, sn) == N_SPECS &&
        c->nodes[i].size == c->nodes[sn].size + 1) {
        int si = find_spec(c, sn);
        if (si >= 0) {
            Spec sp = c->specs.data[si];
            shadow_tag(c, &sp, 0, sp.tok1);
        }
    }
    pop_specs(c, i);
}

/* Attributes given after the declarator. */
static void decl_attrs(Checker *c, uint32_t idecl, Attrs *a)
{
    memset(a, 0, sizeof *a);
    attrs_of_children(c, idecl, a);
}

static void declared_visit(Checker *c, uint32_t i)
{
    uint32_t idecl = c->par[i], decl, sn, top, name_tok, end, ltok, ref;
    int si;
    Spec sp;
    bool initialized, kr, file, incomp_init = false;
    GDecl g;
    CSym s;
    Attrs a;
    SrcLoc il;
    if (ntag(c, idecl) == N_FUNC_DEF) {
        funcdef_declared(c, i);
        return;
    }
    decl = c->par[idecl];
    sn = first_child(c, decl);
    top = first_child(c, idecl);
    kr = c->par[decl] != NO_NODE && ntag(c, c->par[decl]) == N_FUNC_DEF;
    si = find_spec(c, sn);
    if (si < 0 || ntag(c, sn) != N_SPECS)
        return;
    if (c->nodes[sn].size == 1 && cfirst(c, idecl) == sn + 1)
        cpedwarn(c, tloc(c, c->specs.data[si].tok0), "", "data definition has "
                 "no type or storage class");
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    name_tok = cnode(c, idecl)->tok;
    end = scan_end(c, name_tok, true);
    initialized = tpunct(c, end) == P_ASSIGN;
    ltok = initialized ? end + 1 : end;
    il = iloc(c, ltok);
    file = cat_file_scope(c);
    c->cd_ltok = ltok;
    grok(c, &sp, top, kr ? DC_PARM : DC_NORMAL, false, initialized, NO_NODE,
         ltok, ltok, &g);
    if (g.what == GD_NONE)
        return;
    s = g.s;
    decl_attrs(c, idecl, &a);
    if (a.has_mode || a.vector_size) {
        s.ty = attr_apply_type(c, s.ty, &a);
        g.ty = s.ty;
    }
    attrs_merge(&a, &sp.attrs);
    if (a.transparent_union && g.what == GD_TYPEDEF &&
        type_ckind(TT, s.ty) == TY_UNION) {
        /* handle_transparent_union_attribute on a typedef: the union type
         * itself becomes transparent when its first member fits */
        Record *r = type_record(TT, s.ty);
        if (r && (r->flags & RF_COMPLETE) && r->nfields) {
            const Field *fl = &TT->fields.data[r->fields];
            bool sok;
            uint64_t sz = type_size(TT, fl->ty, &sok);
            if (sok && sz == r->size && !(fl->flags & FF_BITFIELD))
                r->flags |= RF_TRANSPARENT;
            else
                cwarn(c, s.loc, "attributes", "'transparent_union' attribute "
                      "ignored");
        } else if (r && !(r->flags & RF_COMPLETE))
            r->flags |= RF_TRANSPARENT;
    }
    if (a.aligned > s.align)
        s.align = (uint16_t)a.aligned;
    if (a.unused)
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;
    if (a.deprecated || a.unavailable) {
        s.flags |= a.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
        s.dep_msg = a.dep_msg;
    }
    if (a.weak)
        s.flags |= CSF_WEAK;
    if (a.noreturn && s.kind == CS_FUNC)
        s.flags |= CSF_NORETURN;
    if (file && g.what == GD_VAR && s.sc == SC_REGISTER &&
        find_child(c, idecl, N_ASM_LABEL) != NO_NODE)
        s.flags |= CSF_REGISTER_NAMED;
    if (g.what == GD_VAR && g.name &&
        !strcmp(cident(c, g.name), "main") && sym_public(&s))
        cwarn(c, s.loc, "main", "'main' is usually a function");
    if (initialized) {
        switch (g.what) {
        case GD_TYPEDEF:
            cerror(c, il, "typedef '%s' is initialized (use '__typeof__' "
                   "instead)", cident(c, g.name));
            initialized = false;
            break;
        case GD_FUNC:
            cerror(c, il, "function '%s' is initialized like a variable",
                   cident(c, g.name));
            initialized = false;
            break;
        case GD_PARM:
            cerror(c, il, "parameter '%s' is initialized", cident(c, g.name));
            initialized = false;
            break;
        default:
            if (is_err(c, s.ty))
                initialized = false;
            else if (type_is_complete(TT, s.ty)) {
                /* fine */
            } else if (!is_arr(c, s.ty)) {
                cerror(c, il, "variable '%s' has initializer but incomplete "
                       "type", cident(c, g.name));
                initialized = false;
                incomp_init = true;   /* gcc still digests the initializer */
            }
            break;
        }
    }
    if (initialized && g.what == GD_VAR && sp.word == TW_AUTO_TYPE)
        s.flags |= CSF_AUTO_TYPE;
    if (initialized) {
        s.flags |= CSF_DEFINED;
        s.def_loc = s.loc;
        if (file)
            s.flags |= CSF_TREE_STATIC;
    }
    if (g.what == GD_VAR && file && !initialized &&
        !(s.flags & CSF_DECL_EXTERNAL))
        s.flags |= CSF_TENTATIVE;
    if (g.what == GD_TYPEDEF) {
        uint32_t nents = (uint32_t)TT->ents.len;
        ref = pushdecl(c, &s, false);
        if (s.align && TT->ents.len > nents &&
            type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF) {
            TypeEnt *te = &TT->ents.data[TYPE_IDX(csym(c, ref)->ty)];
            te->align = s.align;
            te->flags |= TF_ALIGNED;
        }
    } else
        ref = pushdecl(c, &s, false);
    {
        CSym *t = csym(c, ref);
        if (initialized && (t->flags & CSF_DECL_EXTERNAL)) {
            t->flags &= ~(unsigned)CSF_DECL_EXTERNAL;
            t->flags |= CSF_TREE_STATIC;
        }
        c->ty[i] = t->ty;
    }
    c->cb[i] = ref + 1;
    c->cv[i] = initialized ? 1 : incomp_init ? 2 : 0;
    if (g.what == GD_FUNC)
        cexpr_record_params(c, i, ref, false);
    if (g.what == GD_VAR || g.what == GD_FUNC)
        ensure_finish_cue(c, ref);
}

static void init_decl_visit(Checker *c, uint32_t idecl)
{
    uint32_t declared = find_declared(c, idecl), ref;
    bool init_ok, file = cat_file_scope(c);
    uint32_t init = NO_NODE;
    CSym *s;
    TypeId type;
    if (declared == NO_NODE || !c->cb[declared])
        return;
    ref = c->cb[declared] - 1;
    s = csym(c, ref);
    init_ok = c->cv[declared] & 1;
    if (declared != idecl - 1 && init_ok)
        init = idecl - 1;
    cinit_decl_done(c, idecl);
    type = s->ty;
    if ((s->flags & CSF_AUTO_TYPE) && init != NO_NODE) {
        /* __auto_type: the initializer's type after lvalue conversion */
        TypeId it = cexpr_rvalue_type(c, init);
        if (!is_err(c, it) && type_ckind(TT, it) != TY_ERROR) {
            s->ty = type = it | TYPE_QUALS(s->ty);
        }
    }
    if (s->kind == CS_OBJ && !(s->flags & CSF_PARAM)) {
        if (is_incomplete_array(c, type) &&
            !(sym_public(s) && !file)) {
            bool tstatic = (s->flags & CSF_TREE_STATIC) != 0;
            bool do_default = tstatic ? (c->opt.pedantic && !sym_public(s))
                                      : !(s->flags & CSF_DECL_EXTERNAL);
            TypeId nt;
            int failure = complete_array(c, type, init, do_default, &nt);
            switch (failure) {
            case 1:
                cerror(c, s->loc, "initializer fails to determine size of "
                       "'%s'", sname(c, s));
                break;
            case 2:
                if (do_default)
                    cerror(c, s->loc, "array size missing in '%s'",
                           sname(c, s));
                break;
            case 3:
                cerror(c, s->loc, "zero or negative size array '%s'",
                       sname(c, s));
                break;
            default:
                break;
            }
            if (failure != 2 || do_default) {
                s->ty = nt;
                type = nt;
            }
        }
        if (!is_err(c, type) && !type_is_complete(TT, type) &&
            ((s->flags & CSF_TREE_STATIC)
                 ? (sym_defined(s) || !ref_file_scope(ref))
                 : !(s->flags & CSF_DECL_EXTERNAL))) {
            cerror(c, s->loc, "storage size of '%s' isn't known", sname(c, s));
            s->ty = type = ERRT;
        }
        {
            TypeKind k = tkind(c, type);
            if ((k == TY_STRUCT || k == TY_UNION || k == TY_ENUM) &&
                !type_is_complete(TT, type) && (s->flags & CSF_TREE_STATIC))
                queue_object(c, ref);
        }
        if ((s->flags & (CSF_TREE_STATIC | CSF_DECL_EXTERNAL)) &&
            !is_err(c, type) && tkind(c, type) == TY_VLA) {
            cerror(c, s->loc, "storage size of '%s' isn't constant",
                   sname(c, s));
            s->ty = ERRT;
        }
    }
    dump_decl(c, csym(c, ref));
}

/* ---- structs, unions, enums ------------------------------------------------- */

static uint32_t find_child(Checker *c, uint32_t i, unsigned tag)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == tag) {
            r = k.p[j];
            break;
        }
    kids_free(&k);
    return r;
}

/* The tag bound in the innermost scope (a TypeId, 0: none). */
static TypeId tag_here(Checker *c, uint32_t name)
{
    uint32_t b = cbound_here(c, NS_TAG, name);
    return b ? c->log.data[b - 1].ref : 0;
}

static const char *tag_kw(int want)
{
    return want == TY_UNION ? "union" : want == TY_ENUM ? "enum" : "struct";
}

/* pushtag of a new forward reference. */
static TypeId new_tag(Checker *c, int want, uint32_t name, SrcLoc loc)
{
    TypeId t = want == TY_ENUM
        ? type_new_enum(TT, name, loc)
        : type_new_record(TT, name, want == TY_UNION, loc);
    cbind(c, NS_TAG, name, t);
    return t;
}

/* The unit's stray ';' in the struct body being defined, up to token upto
 * (gcc's parser diagnoses them as it goes). */
static void struct_semis(Checker *c, uint32_t upto)
{
    RecDef *rd;
    uint32_t k;
    int depth = 0;
    if (!c->recs.len)
        return;
    rd = &vec_last(&c->recs);
    if (rd->is_enum)
        return;
    for (k = (uint32_t)rd->first_ec; k < upto && k < c->u->ntoks; k++) {
        switch (tpunct(c, k)) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE:
            depth++;
            break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE:
            depth--;
            break;
        case P_SEMI:
            if (depth == 0 && k > 0 &&
                (tpunct(c, k - 1) == P_SEMI || tpunct(c, k - 1) == P_LBRACE))
                cpedantic(c, tloc(c, k), "extra semicolon in struct or union "
                          "specified");
            break;
        default:
            break;
        }
    }
    if (upto > rd->first_ec)
        rd->first_ec = upto;
}

static bool being_defined(Checker *c, TypeId t)
{
    size_t k;
    for (k = 0; k < c->recs.len; k++)
        if (type_canon(TT, c->recs.data[k].ty) == type_canon(TT, t))
            return true;
    return false;
}

static void tag_visit(Checker *c, uint32_t i)
{
    iloc_event(c, cnode(c, i)->tok);
}

/* start_struct / start_enum, at the '{'. */
static void open_visit(Checker *c, uint32_t i)
{
    uint32_t st = c->par[i], tagn = find_child(c, st, N_TAG), open_tok;
    int want = cnode(c, i)->aux == 2 ? TY_ENUM
             : cnode(c, i)->aux == 1 ? TY_UNION : TY_STRUCT;
    uint32_t name = tagn != NO_NODE ? cnode_ident(c, tagn) : 0;
    SrcLoc loc;
    TypeId t, ref = 0;
    RecDef rd;
    bool created = false;
    open_tok = cnode(c, i)->tok;
    if (tagn == NO_NODE)
        iloc_event(c, open_tok);
    if (name)
        loc = tloc(c, cnode(c, tagn)->tok);
    else if (want == TY_ENUM)
        loc = tloc(c, open_tok);
    else
        loc = tloc(c, cnode(c, st)->tok);
    if (name && cat_file_scope(c))
        csum_touch(c, SUM_TAG, name);     /* defines or completes the tag */
    if (name)
        ref = tag_here(c, name);
    if (ref && (int)tkind(c, ref) != want) {
        cerror(c, iloc(c, open_tok), "'%s' defined as wrong kind of tag",
               cident(c, name));
        ref = 0;
    }
    memset(&rd, 0, sizeof rd);
    if (want == TY_ENUM) {
        Enum *e;
        SrcLoc oldloc = 0;
        if (!ref) {
            t = new_tag(c, want, name, loc);
            created = true;
        } else
            t = ref;
        e = type_enum(TT, t);
        oldloc = e->loc;
        e->loc = loc;
        if (being_defined(c, t))
            cerror(c, loc, "nested redefinition of 'enum %s'",
                   name ? cident(c, name) : "");
        if (e->complete) {
            Diagnostic *d = cerror_d(c, loc, "redeclaration of 'enum %s'",
                                     name ? cident(c, name) : "");
            if (d && oldloc)
                cnote(c, d, oldloc, "originally defined here");
            e->complete = false;
        }
        rd.is_enum = true;
        rd.first_ec = (uint32_t)c->ecs.len;
        rd.next_ty = TYPE_B(INT);
    } else {
        Record *r = ref ? type_record(TT, ref) : NULL;
        if (r) {
            if (r->flags & RF_COMPLETE) {
                Diagnostic *d = cerror_d(c, loc, "redefinition of '%s %s'",
                                         tag_kw(want), cident(c, name));
                if (d && r->loc)
                    cnote(c, d, r->loc, "originally defined here");
                ref = 0;
            } else if (r->flags & RF_DEFINING) {
                cerror(c, loc, "nested redefinition of '%s %s'", tag_kw(want),
                       cident(c, name));
                ref = 0;
            }
        }
        if (!ref) {
            t = new_tag(c, want, name, loc);
            created = true;
        } else
            t = ref;
        r = type_record(TT, t);
        r->loc = loc;
        r->flags |= RF_DEFINING;
        rd.first_ec = open_tok + 1;
    }
    rd.ty = t;
    rd.first = (uint32_t)c->fields.len;
    vec_push(&c->recs, rd);
    c->ty[i] = t;
    if (created && cscope_kind(c) == SCK_PROTO)
        c->ef[i] |= 2;
}

/* parser_xref_tag: 'struct S' without a body. */
static void xref_visit(Checker *c, uint32_t i, int want)
{
    uint32_t tagn = find_child(c, i, N_TAG), name;
    SrcLoc loc, xloc = 0;
    TypeId t, ref;
    unsigned kind = TSK_TAGREF;
    uint32_t tt_tok;
    if (tagn == NO_NODE) {
        c->ty[i] = ERRT;
        c->cv[i] = TSK_TAGREF;
        return;
    }
    tt_tok = cnode(c, tagn)->tok;
    name = cnode_ident(c, tagn);
    loc = tloc(c, tt_tok);
    ref = clookup(c, NS_TAG, name);
    if (ref && (int)tkind(c, ref) != want) {
        SrcLoc il = iloc(c, tt_tok + 1);
        if (cbound_here(c, NS_TAG, name))
            cerror(c, il, "'%s' defined as wrong kind of tag",
                   cident(c, name));
        else {
            c->cb[i] = name;
            xloc = il;
        }
        ref = 0;
    }
    if (ref) {
        t = ref;
        /* straight from the table: a name-only use is not a layout read */
        uint32_t x = type_ent(TT, type_canon(TT, t))->extra;
        if (want == TY_ENUM) {
            const Enum *e = &TT->enums.data[x];
            cdep_report(c, loc, name, e->dep, e->dmsg, &e->loc);
        } else {
            const Record *r = &TT->recs.data[x];
            cdep_report(c, loc, name, r->dep, r->dmsg, &r->loc);
        }
    } else {
        t = new_tag(c, want, name, loc);
        kind = TSK_TAGFIRSTREF;
        if (cscope_kind(c) == SCK_PROTO)
            c->ef[i] |= 2;
    }
    if (want == TY_ENUM && find_child(c, i, N_TYPE_NAME) != NO_NODE) {
        /* enum e : T;  (C2X) fixes the underlying type: complete */
        Enum *e = type_enum(TT, t);
        TypeId u = c->ty[find_child(c, i, N_TYPE_NAME)];
        if (!is_err(c, u) && type_is_integer(TT, u)) {
            e->underlying = u;
            e->complete = true;
        }
    } else if (want == TY_ENUM && c->opt.pedantic &&
               !type_enum(TT, t)->complete)
        cpedantic(c, loc, "ISO C forbids forward references to 'enum' types");
    c->ty[i] = t;
    c->cv[i] = kind | ((uint64_t)xloc << 8);
}

/* ---- finish_struct ----------------------------------------------------------- */

static bool flex_struct(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    Record *r;
    if (k != TY_STRUCT && k != TY_UNION)
        return false;
    r = type_record(TT, type_canon(TT, t));
    return r && (r->flags & RF_FLEXIBLE);
}

static void dup_add(Checker *c, uint32_t **seen, size_t *ns, size_t *cap,
                    uint32_t name, SrcLoc loc, bool *dup)
{
    size_t k;
    *dup = false;
    for (k = 0; k < *ns; k++)
        if ((*seen)[k] == name) {
            cerror(c, loc, "duplicate member '%s'", cident(c, name));
            *dup = true;
            return;
        }
    if (*ns == *cap) {
        *cap = *cap ? *cap * 2 : 16;
        *seen = xrealloc(*seen, *cap * sizeof **seen);
    }
    (*seen)[(*ns)++] = name;
}

static void dup_nested(Checker *c, TypeId t, uint32_t **seen, size_t *ns,
                       size_t *cap, int depth)
{
    Record *r = type_record(TT, type_canon(TT, t));
    uint32_t k;
    if (!r || !(r->flags & RF_COMPLETE) || depth > 16)
        return;
    for (k = 0; k < r->nfields; k++) {
        const Field *fl = &TT->fields.data[r->fields + k];
        bool dup;
        if (fl->name)
            dup_add(c, seen, ns, cap, fl->name, fl->loc, &dup);
        else if (tkind(c, fl->ty) == TY_STRUCT ||
                 tkind(c, fl->ty) == TY_UNION)
            dup_nested(c, fl->ty, seen, ns, cap, depth + 1);
    }
}

static bool is_rec(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_STRUCT || tkind(c, t) == TY_UNION;
}

static void struct_finish(Checker *c, uint32_t i, uint32_t open, int want)
{
    TypeId t = c->ty[open];
    RecDef rd;
    FieldIn *f;
    uint32_t n, k, m = 0, close_tok, last;
    Attrs a;
    SrcLoc loc;
    bool named = false, saw_named = false;
    uint32_t *seen = NULL;
    size_t ns = 0, cap = 0;
    Record *r;
    int depth = 0;
    Kids kk;
    if (!c->recs.len)
        return;
    rd = vec_last(&c->recs);
    r = type_record(TT, t);
    loc = r ? r->loc : tloc(c, cnode(c, i)->tok);
    /* the closing brace, the stray semicolons, a missing one */
    close_tok = cnode(c, open)->tok;
    for (k = close_tok; k < c->u->ntoks; k++) {
        int p = tpunct(c, k);
        if (p == P_LBRACE)
            depth++;
        else if (p == P_RBRACE && --depth == 0) {
            close_tok = k;
            break;
        }
    }
    struct_semis(c, close_tok);
    last = NO_NODE;
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++) {
        unsigned tg = ntag(c, kk.p[k]);
        if (tg == N_MEMBER_DECL || tg == N_STATIC_ASSERT || tg == N_PRAGMA)
            last = kk.p[k];
    }
    kids_free(&kk);
    if (last != NO_NODE && ntag(c, last) != N_PRAGMA && close_tok > 0 &&
        tpunct(c, close_tok - 1) != P_SEMI &&
        tpunct(c, close_tok - 1) != P_LBRACE)
        cpedwarn(c, tloc(c, close_tok), "", "no semicolon at end of struct or "
                 "union");
    memset(&a, 0, sizeof a);
    attrs_of_children(c, i, &a);
    n = (uint32_t)c->fields.len - rd.first;
    f = c->fields.data + rd.first;
    if (c->opt.pedantic) {
        for (k = 0; k < n; k++)
            if (f[k].name)
                named = true;
        if (!named) {
            if (want == TY_UNION)
                cpedantic(c, loc, n ? "union has no named members"
                                    : "union has no members");
            else
                cpedantic(c, loc, n ? "struct has no named members"
                                    : "struct has no members");
        }
    }
    for (k = 0; k < n; k++) {
        bool is_last = k == n - 1 || want == TY_UNION;
        if (is_err(c, f[k].ty))
            continue;
        if (is_incomplete_array(c, f[k].ty)) {
            if (want == TY_UNION) {
                cerror(c, f[k].loc, "flexible array member in union");
                f[k].ty = ERRT;
            } else if (!is_last) {
                cerror(c, f[k].loc, "flexible array member not at end of "
                       "struct");
                f[k].ty = ERRT;
            } else if (!saw_named) {
                cerror(c, f[k].loc, "flexible array member in a struct with "
                       "no named members");
                f[k].ty = ERRT;
            }
        }
        if (c->opt.pedantic && want == TY_STRUCT && flex_struct(c, f[k].ty))
            cpedantic(c, f[k].loc, "invalid use of structure with flexible "
                      "array member");
        if (f[k].name || is_rec(c, f[k].ty))
            saw_named = true;
    }
    for (k = 0; k < n; k++) {
        bool dup;
        if (f[k].name) {
            dup_add(c, &seen, &ns, &cap, f[k].name, f[k].loc, &dup);
            if (dup)
                f[k].name = 0;
        } else if (is_rec(c, f[k].ty) && f[k].width < 0)
            dup_nested(c, f[k].ty, &seen, &ns, &cap, 0);
    }
    free(seen);
    for (k = 0; k < n; k++)
        if (!is_err(c, f[k].ty))
            f[m++] = f[k];
    csum_read_pack(c);
    type_complete_record(TT, t, f, m, c->pack, a.aligned, a.packed);
    r = type_record(TT, t);
    r->dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    r->dmsg = a.dep_msg;
    if (a.transparent_union && want == TY_UNION) {
        bool ok = r->nfields > 0;
        if (ok) {
            const Field *fl = &TT->fields.data[r->fields];
            bool sok;
            uint64_t sz = type_size(TT, fl->ty, &sok);
            ok = sok && sz == r->size && !(fl->flags & FF_BITFIELD);
        }
        if (ok)
            r->flags |= RF_TRANSPARENT;
        else
            cwarn(c, loc, "", "union cannot be made transparent");
    }
    if (c->opt.dump && !c->quiet) {
        StrBuf sb;
        memset(&sb, 0, sizeof sb);
        type_dump_record(TT, &sb, t);
        fputs(sb_cstr(&sb), c->opt.dump);
        sb_free(&sb);
    }
    c->fields.len = rd.first;
    c->recs.len--;
    c->ty[i] = t;
    c->cv[i] = TSK_TAGDEF;
    if (c->ef[open] & 2)
        c->ef[i] |= 2;
}

/* ---- enumerators ------------------------------------------------------------- */

static bool val_neg(Checker *c, uint64_t v, TypeId ty)
{
    return type_is_signed(TT, ty) && (int64_t)v < 0;
}

/* a < b, as mathematical values of types ta and tb. */
static bool val_lt(Checker *c, uint64_t a, TypeId ta, uint64_t b, TypeId tb)
{
    bool na = val_neg(c, a, ta), nb = val_neg(c, b, tb);
    if (na != nb)
        return na;
    if (na)
        return (int64_t)a < (int64_t)b;
    return a < b;
}

static void enumerator_visit(Checker *c, uint32_t i)
{
    uint32_t name = cnode_ident(c, i), vn = NO_NODE, k;
    RecDef *rd;
    SrcLoc nloc = tloc(c, cnode(c, i)->tok), vloc = nloc;
    TypeId vt = TYPE_B(INT);
    uint64_t v = 0;
    bool have = false, wide = false;
    Kids kk;
    CSym s;
    uint32_t ref;
    if (!c->recs.len || !vec_last(&c->recs).is_enum)
        return;
    rd = &vec_last(&c->recs);
    iloc_event(c, cnode(c, i)->tok);
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++)
        if (cexpr_is_expr(ntag(c, kk.p[k])))
            vn = kk.p[k];
    kids_free(&kk);
    if (vn != NO_NODE) {
        TypeId ty = c->ty[vn];
        vloc = tloc(c, first_tok(c, vn));
        if (c->ck[vn] == K_ERR && is_err(c, ty)) {
            /* ignored: the default */
        } else if (!type_is_integer(TT, ty)) {
            cerror(c, vloc, "enumerator value for '%s' is not an integer "
                   "constant", cident(c, name));
        } else {
            bool ok = c->ck[vn] == K_ICE;
            if (!ok && c->ck[vn] == K_FOLD) {
                cpedantic(c, vloc, "enumerator value for '%s' is not an "
                          "integer constant expression", cident(c, name));
                ok = true;
            }
            if (!ok)
                cerror(c, iloc(c, after_tok(c, vn)), "enumerator value for "
                       "'%s' is not an integer constant", cident(c, name));
            else {
                if (c->ef[vn] & EF_OVERFLOW)
                    cpedwarn(c, nloc, "overflow",
                             "overflow in constant expression");
                vt = type_int_promote(TT, ty);
                v = cexpr_trunc(c, vt, (uint64_t)cexpr_sval(c, vn));
                have = true;
            }
        }
    }
    if (!have) {
        wide = rd->next_ty == TYPE_B(UINT128) || rd->next_ty == TYPE_B(INT128);
        v = rd->next;
        vt = rd->next_ty;
        if (rd->next_overflow)
            cerror(c, vloc, "overflow in enumeration values");
    }
    if (wide)
        cpedantic(c, vloc, "enumerator value outside the range of '%s'",
                  type_is_signed(TT, vt) ? "intmax_t" : "uintmax_t");
    if (wide) {
        /* wider than intmax_t: the value itself is not representable */
    } else if (!cexpr_fits(c, v, vt, TYPE_B(INT))) {
        cpedantic(c, vloc, "ISO C restricts enumerator values to range of "
                  "'int' before C2X");
    } else {
        vt = TYPE_B(INT);
        v = cexpr_trunc(c, vt, v);
    }
    /* the next value */
    {
        uint64_t nv = cexpr_trunc(c, vt, v + 1);
        bool ovf = val_lt(c, nv, vt, v, vt);
        TypeId nt = vt;
        if (ovf) {
            unsigned prec = type_int_bits(TT, vt) + 1;
            bool uns = !type_is_signed(TT, vt);
            TypeId nw = uns ? TYPE_B(ULONG) : TYPE_B(LONG);
            if (prec > type_int_bits(TT, nw))
                nw = uns ? TYPE_B(ULLONG) : TYPE_B(LLONG);
            if (prec > type_int_bits(TT, nw) && c->tgt->size[TY_INT128])
                nw = uns ? TYPE_B(UINT128) : TYPE_B(INT128);
            if (prec <= type_int_bits(TT, nw)) {
                ovf = false;
                nt = nw;
                nv = nw == TYPE_B(UINT128) || nw == TYPE_B(INT128)
                         ? 0 : cexpr_trunc(c, nw, v + 1);
            }
        }
        rd->next = nv;
        rd->next_ty = nt;
        rd->next_overflow = ovf;
    }
    memset(&s, 0, sizeof s);
    s.name = name;
    s.kind = CS_ENUMCONST;
    s.ty = vt;      /* the value's type until the enum is finished */
    s.loc = nloc;
    s.val = v;
    s.vty = vt;
    ref = pushdecl(c, &s, false);
    vec_push(&c->ecs, ref);
}

/* c_common_type_for_size */
static TypeId type_for_bits(Checker *c, unsigned bits, bool uns)
{
    if (bits <= 8)
        return uns ? TYPE_B(UCHAR) : TYPE_B(SCHAR);
    if (bits <= 16)
        return uns ? TYPE_B(USHORT) : TYPE_B(SHORT);
    if (bits <= 32)
        return uns ? TYPE_B(UINT) : TYPE_B(INT);
    if (bits <= 64) {
        if (type_int_bits(TT, TYPE_B(LONG)) == 64)
            return uns ? TYPE_B(ULONG) : TYPE_B(LONG);
        return uns ? TYPE_B(ULLONG) : TYPE_B(LLONG);
    }
    if (bits <= 128 && c->tgt->size[TY_INT128])
        return uns ? TYPE_B(UINT128) : TYPE_B(INT128);
    return ERRT;
}

static void enum_finish(Checker *c, uint32_t i, uint32_t open)
{
    TypeId t = c->ty[open];
    RecDef rd;
    Enum *e;
    Attrs a;
    uint32_t k, ne;
    uint64_t mn = 0, mx = 0;
    TypeId mnt = TYPE_B(INT), mxt = TYPE_B(INT);
    bool uns, wider;
    unsigned prec, p2;
    TypeId tem;
    if (!c->recs.len)
        return;
    rd = vec_last(&c->recs);
    ne = (uint32_t)c->ecs.len - rd.first_ec;
    memset(&a, 0, sizeof a);
    attrs_of_children(c, i, &a);
    e = type_enum(TT, t);
    if (a.packed)
        e->packed = true;
    e->dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    e->dmsg = a.dep_msg;
    for (k = 0; k < ne; k++) {
        const CSym *s = csym(c, c->ecs.data[rd.first_ec + k]);
        if (k == 0) {
            mn = mx = s->val;
            mnt = mxt = s->vty;
            continue;
        }
        if (val_lt(c, mx, mxt, s->val, s->vty)) {
            mx = s->val;
            mxt = s->vty;
        }
        if (val_lt(c, s->val, s->vty, mn, mnt)) {
            mn = s->val;
            mnt = s->vty;
        }
    }
    uns = !val_neg(c, mn, mnt);
    p2 = min_prec(mn, !uns, uns);
    prec = min_prec(mx, val_neg(c, mx, mxt), uns);
    if (p2 > prec)
        prec = p2;
    wider = !cexpr_fits(c, mn, mnt, TYPE_B(INT)) ||
            !cexpr_fits(c, mx, mxt, TYPE_B(INT));
    if (e->packed || prec > 32) {
        tem = type_for_bits(c, prec, uns);
        if (is_err(c, tem)) {
            cpedwarn(c, iloc(c, after_tok(c, i)), "", "enumeration values "
                     "exceed range of largest integer");
            tem = TYPE_B(LLONG);
        }
    } else
        tem = uns ? TYPE_B(UINT) : TYPE_B(INT);
    {
        uint32_t utn = find_child(c, i, N_TYPE_NAME);
        if (utn != NO_NODE && !is_err(c, c->ty[utn]) &&
            type_is_integer(TT, c->ty[utn])) {
            tem = c->ty[utn];       /* C2X fixed underlying type */
            wider = false;
        }
    }
    e->underlying = tem;
    e->complete = true;
    for (k = 0; k < ne; k++) {
        CSym *s = csym(c, c->ecs.data[rd.first_ec + k]);
        s->ty = t;
        if (wider)
            s->vty = t;
    }
    if (c->opt.dump && !c->quiet) {
        for (k = 0; k < ne; k++) {
            const CSym *s = csym(c, c->ecs.data[rd.first_ec + k]);
            StrBuf sb;
            memset(&sb, 0, sizeof sb);
            type_print(TT, &sb, s->vty);
            if (val_neg(c, s->val, s->vty) || type_is_signed(TT, s->vty))
                fprintf(c->opt.dump, "enumconst %s = %" PRId64 " (%s)\n",
                        sname(c, s), (int64_t)s->val, sb_cstr(&sb));
            else
                fprintf(c->opt.dump, "enumconst %s = %" PRIu64 " (%s)\n",
                        sname(c, s), s->val, sb_cstr(&sb));
            sb_free(&sb);
        }
    }
    c->ecs.len = rd.first_ec;
    c->fields.len = rd.first;
    c->recs.len--;
    c->ty[i] = t;
    c->cv[i] = TSK_TAGDEF;
    if (c->ef[open] & 2)
        c->ef[i] |= 2;
}

static void struct_visit(Checker *c, uint32_t i)
{
    uint32_t open = find_child(c, i, N_OPEN);
    int want;
    if (cnode(c, i)->tag == N_ENUM)
        want = TY_ENUM;
    else
        want = tckw(c, cnode(c, i)->tok) == CK_UNION ? TY_UNION : TY_STRUCT;
    if (want == TY_ENUM && find_child(c, i, N_TYPE_NAME) != NO_NODE) {
        uint32_t tn = find_child(c, i, N_TYPE_NAME);
        uint32_t tg_ = find_child(c, i, N_TAG);
        cpedantic(c, tloc(c, tg_ != NO_NODE ? cnode(c, tg_)->tok
                                              : first_tok(c, tn) - 1),
                  "ISO C does not support specifying 'enum' underlying types "
                  "before C2X");
    }
    if (open == NO_NODE)
        xref_visit(c, i, want);
    else if (want == TY_ENUM)
        enum_finish(c, i, open);
    else
        struct_finish(c, i, open, want);
}

/* ---- members ------------------------------------------------------------------ */

/* The delimiter (',' ';' '}') ending the j-th declarator of a member
 * declaration whose specifiers end before token start. */
static uint32_t member_delim(Checker *c, uint32_t start, uint32_t j)
{
    uint32_t k = start;
    for (;;) {
        k = scan_end(c, k, false);
        if (j == 0 || tpunct(c, k) != P_COMMA)
            return k;
        j--;
        k++;
    }
}

static void member_visit(Checker *c, uint32_t i)
{
    uint32_t md = c->par[i], sn, top = NO_NODE, w = NO_NODE, k, idx = 0, ltok;
    Kids kk;
    Spec sp;
    int si;
    GDecl g;
    Attrs a;
    FieldIn fi;
    if (md == NO_NODE || ntag(c, md) != N_MEMBER_DECL)
        return;
    sn = first_child(c, md);
    si = find_spec(c, sn);
    if (si < 0)
        return;
    struct_semis(c, first_tok(c, md));
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++) {
        if (is_declarator_tag(ntag(c, kk.p[k])))
            top = kk.p[k];
        else if (cexpr_is_expr(ntag(c, kk.p[k])) &&
                 (cnode(c, i)->flags & NF_BITFIELD))
            w = kk.p[k];
    }
    kids_free(&kk);
    kids_get(c, md, &kk);
    for (k = 0; k < kk.n && kk.p[k] != i; k++)
        if (ntag(c, kk.p[k]) == N_MEMBER)
            idx++;
    kids_free(&kk);
    ltok = member_delim(c, sp.tok1, idx);
    c->cd_ltok = ltok;
    grok(c, &sp, top, DC_FIELD, false, false, w, ltok, ltok, &g);
    if (g.what == GD_NONE)
        return;
    memset(&a, 0, sizeof a);
    attrs_of_children(c, i, &a);
    if (a.has_mode || a.vector_size)
        g.ty = attr_apply_type(c, g.ty, &a);
    attrs_merge(&a, &sp.attrs);
    memset(&fi, 0, sizeof fi);
    fi.name = g.name;
    fi.ty = g.ty;
    fi.width = g.width;
    fi.align = g.s.align > a.aligned ? g.s.align : (uint16_t)a.aligned;
    fi.packed = a.packed;
    fi.loc = g.loc;
    fi.dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    fi.dmsg = a.dep_msg;
    vec_push(&c->fields, fi);
}

static void member_decl_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i);
    int si = sn != NO_NODE ? find_spec(c, sn) : -1;
    if (si >= 0 && c->nodes[i].size == c->nodes[sn].size + 1) {
        Spec sp = c->specs.data[si];
        uint32_t ltok = sp.tok1;
        struct_semis(c, first_tok(c, i));
        if (sp.kind == TSK_NONE && !sp.has_type && sp.word == TW_NONE &&
            !sp.is_long && !sp.is_short && !sp.is_signed && !sp.is_unsigned &&
            !sp.is_complex) {
            cpedantic(c, tloc(c, sp.tok0), "ISO C forbids member declarations "
                      "with no members");
            shadow_tag(c, &sp, c->opt.pedantic ? 1 : 0, ltok);
        } else if (!sp.error) {
            TypeKind tk = tkind(c, sp.ty);
            bool ok = false;
            if ((tk == TY_STRUCT || tk == TY_UNION) && sp.kind != TSK_TYPEDEF)
                ok = type_record(TT, type_canon(TT, sp.ty))->tag == 0;
            if (!ok)
                cpedwarn(c, tloc(c, ltok), "", "declaration does not declare "
                         "anything");
            else {
                GDecl g;
                cpedantic(c, tloc(c, ltok), "ISO C99 doesn't support unnamed "
                          "structs/unions");
                grok(c, &sp, NO_NODE, DC_FIELD, false, false, NO_NODE, ltok,
                     ltok, &g);
                if (g.what != GD_NONE) {
                    FieldIn fi;
                    memset(&fi, 0, sizeof fi);
                    fi.ty = g.ty;
                    fi.width = -1;
                    fi.align = g.s.align > sp.attrs.aligned
                        ? g.s.align : (uint16_t)sp.attrs.aligned;
                    fi.packed = sp.attrs.packed;
                    fi.loc = tloc(c, ltok);
                    {
                        /* gcc locates an anonymous member at its '{' */
                        uint32_t q;
                        for (q = cfirst(c, sn); q <= sn; q++)
                            if (ntag(c, q) == N_OPEN) {
                                fi.loc = tloc(c, cnode(c, q)->tok);
                                break;
                            }
                    }
                    vec_push(&c->fields, fi);
                }
            }
        }
    }
    pop_specs(c, i);
}

/* ---- _Static_assert, #pragma pack ---------------------------------------------- */

static void static_assert_visit(Checker *c, uint32_t i)
{
    uint32_t e = first_child(c, i), s = NO_NODE, k;
    SrcLoc aloc = tloc(c, cnode(c, i)->tok), vloc;
    Kids kk;
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        struct_semis(c, cnode(c, i)->tok);
    if (!in_extension(c, i))
        cpedantic(c, aloc, "ISO C99 does not support '_Static_assert'");
    if (e == NO_NODE)
        return;
    kids_get(c, i, &kk);
    for (k = 0; k < kk.n; k++)
        if (ntag(c, kk.p[k]) == N_STRING)
            s = kk.p[k];
    kids_free(&kk);
    if (s == e)
        return;
    vloc = cnode_loc(c, e);
    if (!type_is_integer(TT, c->ty[e])) {
        cerror(c, vloc, "expression in static assertion is not an integer");
        return;
    }
    if (c->ck[e] != K_ICE) {
        if (c->ck[e] == K_FOLD)
            cpedantic(c, vloc, "expression in static assertion is not an "
                      "integer constant expression");
        else {
            if (c->ck[e] != K_ERR)
                cerror(c, vloc, "expression in static assertion is not "
                       "constant");
            return;
        }
    }
    if (cexpr_sval(c, e) == 0) {
        if (s != NO_NODE) {
            StrBuf sb;
            uint32_t p, np = cnode(c, s)->aux ? cnode(c, s)->aux : 1;
            memset(&sb, 0, sizeof sb);
            sb_putc(&sb, '"');
            for (p = 0; p < np; p++) {
                const Tok *t = tokp(c, cnode(c, s)->tok + p);
                const char *tx = tok_text_raw(c->sm, c->in, t);
                size_t n = t->len, off = 0;
                while (off < n && tx[off] != '"')
                    off++;
                if (off < n)
                    off++;
                if (n > off && tx[n - 1] == '"')
                    n--;
                if (n > off)
                    sb_putn(&sb, tx + off, n - off);
            }
            sb_putc(&sb, '"');
            cerror(c, aloc, "static assertion failed: %s", sb_cstr(&sb));
            sb_free(&sb);
        } else
            cerror(c, aloc, "static assertion failed");
    }
}

static void pragma_visit(Checker *c, uint32_t i)
{
    const Tok *t = tokp(c, cnode(c, i)->tok);
    const char *s = tok_text_raw(c->sm, c->in, t), *end;
    size_t n = t->len;
    int action = 0;   /* 0 set, 1 push, 2 pop */
    long val = -1;
    bool have = false;
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        struct_semis(c, cnode(c, i)->tok);
    end = s + n;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (end - s >= 6 && !strncmp(s, "pragma", 6))
        s += 6;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (end - s < 4 || strncmp(s, "pack", 4))
        return;
    s += 4;
    while (s < end && (*s == ' ' || *s == '\t'))
        s++;
    if (s >= end || *s != '(')
        return;
    s++;
    for (;;) {
        while (s < end && (*s == ' ' || *s == '\t' || *s == ','))
            s++;
        if (s >= end || *s == ')')
            break;
        if (*s >= '0' && *s <= '9') {
            val = 0;
            have = true;
            while (s < end && *s >= '0' && *s <= '9')
                val = val * 10 + (*s++ - '0');
        } else if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                   *s == '_') {
            const char *b = s;
            while (s < end && ((*s >= 'a' && *s <= 'z') ||
                               (*s >= 'A' && *s <= 'Z') || *s == '_' ||
                               (*s >= '0' && *s <= '9')))
                s++;
            if (s - b == 4 && !strncmp(b, "push", 4))
                action = 1;
            else if (s - b == 3 && !strncmp(b, "pop", 3))
                action = 2;
        } else
            return;
    }
    if (have && !(val == 0 || val == 1 || val == 2 || val == 4 || val == 8 ||
                  val == 16))
        return;
    if (action == 1) {
        vec_push(&c->pack_stack, c->pack);
        if (have)
            c->pack = (unsigned)val;
    } else if (action == 2) {
        if (c->pack_stack.len) {
            c->pack = vec_last(&c->pack_stack);
            c->pack_stack.len--;
        }
    } else
        c->pack = have ? (unsigned)val : 0;
}

/* ---- parameters, array declarators, function declarators --------------------- */

static void param_visit(Checker *c, uint32_t p)
{
    uint32_t sn = first_child(c, p), top = NO_NODE, after = after_tok(c, p);
    uint32_t l = last_child(c, p), ref;
    int si;
    Spec sp;
    GDecl g;
    CSym s;
    Attrs a;
    while (l != NO_NODE && ntag(c, l) == N_ATTRIBUTE) {
        /* trailing attributes: the declarator is the node before them */
        Kids k;
        uint32_t j, prev = NO_NODE;
        kids_get(c, p, &k);
        for (j = 0; j < k.n && k.p[j] != l; j++)
            prev = k.p[j];
        kids_free(&k);
        l = prev;
    }
    if (l != NO_NODE && is_declarator_tag(ntag(c, l)))
        top = l;
    si = sn != NO_NODE && ntag(c, sn) == N_SPECS ? find_spec(c, sn) : -1;
    if (si < 0) {
        c->ty[p] = ERRT;
        pop_specs(c, p);
        return;
    }
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    c->cd_ltok = after;
    grok(c, &sp, top, DC_PARM, false, false, NO_NODE, after, after, &g);
    if (g.what == GD_NONE) {
        c->ty[p] = ERRT;
        pop_specs(c, p);
        return;
    }
    s = g.s;
    if (!g.name) {
        uint32_t d = top, inner;
        bool arr = false;
        if (d != NO_NODE) {
            while ((inner = inner_decl(c, d)) != NO_NODE)
                d = inner;
            arr = ntag(c, d) == N_ARRAY;
        }
        if (!arr)
            s.loc = tloc(c, first_tok(c, p));
    }
    memset(&a, 0, sizeof a);
    attrs_of_children(c, p, &a);
    attrs_merge(&a, &sp.attrs);
    if (a.unused)
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;
    if (a.deprecated || a.unavailable) {
        s.flags |= a.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
        s.dep_msg = a.dep_msg;
    }
    if ((cnode(c, p)->flags & NF_SEMI) && c->fwd_warned != c->par[p] + 1) {
        /* mark_forward_parm_decls: once per parameter scope */
        c->fwd_warned = c->par[p] + 1;
        cpedantic(c, iloc(c, after + 1),
                  "ISO C forbids forward parameter declarations");
    }
    ref = pushdecl(c, &s, false);
    if (cnode(c, p)->flags & NF_FWD)
        csym(c, ref)->flags |= CSF_FWD;
    else
        csym(c, ref)->flags &= ~(unsigned)CSF_FWD;
    c->ty[p] = g.ty;
    c->cb[p] = ref + 1;
    c->cv[p] = (g.name ? 1 : 0) | (sp.sc == SC_REGISTER ? 2 : 0);
    pop_specs(c, p);
}

static void array_visit(Checker *c, uint32_t i)
{
    if (!(cnode(c, i)->flags & NF_STAR))
        return;
    switch (cscope_kind(c)) {
    case SCK_PROTO:
        break;
    case SCK_FUNC:
        if (c->cur_func_node != NO_NODE)
            c->ef[c->cur_func_node] |= 1;
        break;
    default:
        cerror(c, tloc(c, cnode(c, i)->tok), "'[*]' not allowed in other than "
               "function prototype scope");
        c->cv[i] |= 1;
        break;
    }
}

/* The location of a parameter's symbol. */
static SrcLoc param_loc(Checker *c, uint32_t p)
{
    if (c->cb[p])
        return csym(c, c->cb[p] - 1)->loc;
    return tloc(c, cnode(c, p)->tok);
}

/* GNU forward parameter declarations (mark_forward_parm_decls, and
 * get_parm_info's check that each was declared again). */
static void fwd_params(Checker *c, uint32_t f)
{
    Kids k;
    uint32_t j, m;
    c->fwd_warned = 0;
    kids_get(c, f, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t p = k.p[j];
        bool redone = false;
        if (ntag(c, p) != N_PARAM || !(cnode(c, p)->flags & NF_FWD))
            continue;
        for (m = 0; m < k.n && !redone; m++)
            redone = c->cb[p] && is_real_param(c, k.p[m]) &&
                     c->cb[k.p[m]] == c->cb[p];
        if (!redone)
            cerror(c, param_loc(c, p), "parameter '%s' has just a forward "
                   "declaration", c->cb[p] && csym(c, c->cb[p] - 1)->name
                       ? cident(c, csym(c, c->cb[p] - 1)->name)
                       : "({anonymous})");
    }
    kids_free(&k);
}

/* get_parm_info, at the FUNC node. */
static void func_visit(Checker *c, uint32_t f)
{
    unsigned fl = cnode(c, f)->flags;
    bool var = (fl & NF_VARIADIC) != 0, has = func_has_params(c, f);
    uint32_t k, first = cfirst(c, f), nparm = 0, ntags = 0, j;
    bool gave = false;
    if (var && !has)
        cpedantic(c, tloc(c, cnode(c, f)->tok + 1), "ISO C requires a named "
                  "argument before '...' before C2X");
    if (!has || (fl & NF_KR))
        return;
    fwd_params(c, f);
    c->stack.len = 0;
    for (k = f; k-- > first;) {
        unsigned t = ntag(c, k);
        if (t == N_SCOPE && c->par[k] == f)
            break;
        if (t == N_FUNC) {
            k = cfirst(c, k);
            continue;
        }
        if (t == N_PARAM && !(cnode(c, k)->flags & NF_FWD)) {
            nparm++;
            vec_push(&c->stack, k);
        } else if ((t == N_STRUCT || t == N_ENUM) && (c->ef[k] & 2)) {
            ntags++;
            vec_push(&c->stack, k);
        }
    }
    if (nparm == 1 && ntags == 0 && lone_void(c, f)) {
        uint32_t p = c->stack.data[0];
        SrcLoc loc = param_loc(c, p);
        if (TYPE_QUALS(c->ty[p]) || (c->cv[p] & 2))
            cerror(c, loc, "'void' as only parameter may not be qualified");
        if (var)
            cerror(c, loc, "'void' must be the only parameter");
        return;
    }
    for (j = 0; j < c->stack.len; j++) {
        uint32_t e = c->stack.data[j];
        if (is_real_param(c, e)) {
            if (!(c->cv[e] & 1) && is_void(c, c->ty[e]) && !is_err(c, c->ty[e])
                && !gave) {
                cerror(c, param_loc(c, e), "'void' must be the only "
                       "parameter");
                gave = true;
            }
        } else {
            TypeId t = c->ty[e];
            TypeKind tk = tkind(c, t);
            uint32_t name = 0;
            SrcLoc loc = 0;
            const char *kw = tk == TY_ENUM ? "enum"
                                           : tk == TY_UNION ? "union" : "struct";
            if (tk == TY_ENUM) {
                Enum *en = type_enum(TT, t);
                if (en) {
                    name = en->tag;
                    loc = en->loc;
                }
            } else if (tk == TY_STRUCT || tk == TY_UNION) {
                Record *r = type_record(TT, t);
                if (r) {
                    name = r->tag;
                    loc = r->loc;
                }
            } else
                continue;
            if (tk == TY_UNION && !name)
                continue;
            if (name)
                cwarn(c, loc, "", "'%s %s' declared inside parameter list will "
                      "not be visible outside of this definition or "
                      "declaration", kw, cident(c, name));
            else
                cwarn(c, loc, "", "anonymous %s declared inside parameter list "
                      "will not be visible outside of this definition or "
                      "declaration", kw);
        }
    }
}

/* ---- type names ---------------------------------------------------------------- */

static void typename_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i), top = NO_NODE, l = last_child(c, i);
    uint32_t after = after_tok(c, i);
    int si;
    Spec sp;
    GDecl g;
    if (l != NO_NODE && is_declarator_tag(ntag(c, l)))
        top = l;
    si = sn != NO_NODE && ntag(c, sn) == N_SPECS ? find_spec(c, sn) : -1;
    if (si < 0) {
        c->ty[i] = ERRT;
        pop_specs(c, i);
        return;
    }
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    c->cd_ltok = after;
    c->cd_clit = c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_COMPOUND_LIT
                     ? after + 1 : 0;
    grok(c, &sp, top, DC_TYPENAME, false, false, NO_NODE, after, after, &g);
    c->cd_clit = 0;
    c->ty[i] = g.what == GD_NONE || sp.error ? ERRT : g.ty;
    pop_specs(c, i);
}

TypeId type_of_typename(Checker *c, uint32_t i)
{
    return c->ty[i];
}

/* ---- function definitions ------------------------------------------------------- */

typedef struct FdParts {
    uint32_t specs, declared, scope, compound, scope_end;
} FdParts;

static void fd_parts(Checker *c, uint32_t fd, FdParts *p)
{
    Kids k;
    uint32_t j;
    p->specs = p->declared = p->scope = p->compound = p->scope_end = NO_NODE;
    kids_get(c, fd, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t n = k.p[j];
        switch (ntag(c, n)) {
        case N_SPECS:
            if (p->specs == NO_NODE)
                p->specs = n;
            break;
        case N_DECLARED:
            if (p->declared == NO_NODE)
                p->declared = n;
            break;
        case N_SCOPE:
            if (p->scope == NO_NODE)
                p->scope = n;
            break;
        case N_COMPOUND:
            p->compound = n;
            break;
        case N_SCOPE_END:
            p->scope_end = n;
            break;
        default:
            break;
        }
    }
    kids_free(&k);
}

/* The FUNC declarator directly around the name. */
static uint32_t funcdef_fnode(Checker *c, uint32_t top)
{
    uint32_t d = top;
    while (d != NO_NODE && ntag(c, d) != N_NAME) {
        uint32_t in = inner_decl(c, d);
        if (in == NO_NODE)
            return NO_NODE;
        if (ntag(c, d) == N_FUNC && ntag(c, in) == N_NAME)
            return d;
        d = in;
    }
    return NO_NODE;
}

static TypeId plain_type(Checker *c, TypeId t)
{
    return TYPE_UNQUAL(type_canon(TT, t));
}

/* check_main_parameter_types */
static void check_main_params(Checker *c, SrcLoc loc, TypeId ft)
{
    const TypeEnt *e = type_ent(TT, type_canon(TT, ft));
    uint32_t n = e->n, j, argct = 0;
    const char *nm = "main";
    for (j = 0; j < n; j++) {
        TypeId t = type_params(TT, type_canon(TT, ft))[j];
        TypeId b;
        if (is_err(c, t))
            break;
        argct++;
        b = tkind(c, t) == TY_PTR ? type_base(TT, t) : ERRT;
        switch (argct) {
        case 1:
            if (plain_type(c, t) != TYPE_B(INT))
                cpedwarn(c, loc, "main", "first argument of '%s' should be "
                         "'int'", nm);
            break;
        case 2:
        case 3:
            if (tkind(c, t) != TY_PTR || tkind(c, b) != TY_PTR ||
                plain_type(c, type_base(TT, b)) != TYPE_B(CHAR)) {
                if (argct == 2)
                    cpedwarn(c, loc, "main", "second argument of '%s' should "
                             "be 'char **'", nm);
                else
                    cpedwarn(c, loc, "main", "third argument of '%s' should "
                             "probably be 'char **'", nm);
            }
            break;
        default:
            break;
        }
    }
    if (argct > 0 && (argct < 2 || argct > 3))
        cpedwarn(c, loc, "main", "'%s' takes only zero or two arguments", nm);
    if (type_ent(TT, type_canon(TT, ft))->flags & TF_VARIADIC)
        cpedwarn(c, loc, "main", "'%s' declared as variadic function", nm);
}

static bool isnt_proto(Checker *c, const CSym *old)
{
    return !old || !is_prototype(c, old->ty);
}

/* start_function, at the DECLARED node of a FUNC_DEF. */
static void funcdef_declared(Checker *c, uint32_t declared)
{
    uint32_t fd = c->par[declared], top = declared - 1, ltok, fnode, ref, b;
    FdParts fp;
    int si;
    Spec sp;
    GDecl g;
    CSym s, oldc;
    bool have_old = false, nested = c->func_sym != SYM_NONE, pub, is_main, inl;
    SrcLoc loc;
    const char *name;
    TypeId rt;
    fd_parts(c, fd, &fp);
    if (fp.specs == NO_NODE)
        return;
    si = find_spec(c, fp.specs);
    if (si < 0)
        return;
    if (nested && !in_extension(c, fd))
        cpedantic(c, tloc(c, cnode(c, fd)->tok), "ISO C forbids nested "
                  "functions");
    pending_xref(c, &c->specs.data[si]);
    sp = c->specs.data[si];
    ltok = after_tok(c, top);
    c->cd_ltok = ltok;
    grok(c, &sp, top, DC_NORMAL, true, true, NO_NODE, ltok, ltok, &g);
    if (g.what != GD_FUNC || !is_func(c, g.s.ty))
        return;
    s = g.s;
    loc = s.loc;
    name = cident(c, s.name);
    if (nested)
        s.linkage = LK_INTERNAL;
    /* the return type */
    rt = type_base(TT, s.ty);
    if (!is_err(c, rt) && !is_void(c, rt) && !type_is_complete(TT, rt)) {
        const TypeEnt *e = type_ent(TT, type_canon(TT, s.ty));
        uint32_t n = e->n, flags = e->flags & (TF_VARIADIC | TF_NOPROTO);
        TypeId *ps = xmalloc((n + 1) * sizeof *ps);
        if (n)
            memcpy(ps, type_params(TT, type_canon(TT, s.ty)), n * sizeof *ps);
        cerror(c, loc, "return type is an incomplete type");
        s.ty = type_func(TT, TYPE_B(VOID), ps, n, flags);
        free(ps);
    }
    if (g.default_int)
        cpedwarn(c, loc, "implicit-int", "return type defaults to 'int'");
    b = cbound_here(c, NS_ORD, s.name);
    if (b && csym(c, c->log.data[b - 1].ref)->kind == CS_FUNC) {
        oldc = *csym(c, c->log.data[b - 1].ref);
        have_old = true;
    }
    c->cd_have_proto = false;
    c->cd_proto = 0;
    c->cd_proto_loc = 0;
    if (!is_prototype(c, s.ty)) {
        TypeId oldt = have_old ? oldc.ty : s.ty;
        if (have_old && is_func(c, oldt) &&
            type_compatible(TT, type_base(TT, oldt), type_base(TT, s.ty))) {
            if (type_ent(TT, type_canon(TT, oldt))->flags & TF_VARIADIC) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "", "'%s' defined "
                                        "as variadic function without "
                                        "prototype", name);
                locate_old_decl(c, d, &oldc);
            }
            s.ty = type_composite(TT, oldt, s.ty);
        }
        if (sym_public(&s)) {
            uint32_t e = s.name < c->nidents ? c->ext[s.name] : 0;
            csum_read_ext(c, s.name);
            if (e) {
                const CSym *es = csym(c, e - 1);
                if (is_func(c, es->ty) &&
                    type_compatible(TT, type_base(TT, s.ty),
                                    type_base(TT, es->ty))) {
                    c->cd_proto_loc = es->loc;
                    if (is_prototype(c, es->ty)) {
                        c->cd_have_proto = true;
                        c->cd_proto = es->ty;
                    }
                }
            }
        }
    }
    pub = sym_public(&s);
    is_main = !strcmp(name, "main");
    inl = (s.flags & CSF_INLINE) != 0;
    if (diag_enabled(c->diag, "strict-prototypes") && !is_prototype(c, s.ty) &&
        isnt_proto(c, have_old ? &oldc : NULL))
        cwarn(c, loc, "strict-prototypes", "function declaration isn't a "
              "prototype");
    else if (diag_enabled(c->diag, "missing-prototypes") && pub && !is_main &&
             isnt_proto(c, have_old ? &oldc : NULL) && !inl)
        cwarn(c, loc, "missing-prototypes", "no previous prototype for '%s'",
              name);
    else if (diag_enabled(c->diag, "missing-prototypes") && have_old &&
             (oldc.flags & CSF_USED) && !is_prototype(c, oldc.ty))
        cwarn(c, loc, "missing-prototypes", "'%s' was used with no prototype "
              "before its definition", name);
    else if (diag_enabled(c->diag, "missing-declarations") && pub &&
             !have_old && !is_main && !inl)
        cwarn(c, loc, "missing-declarations", "no previous declaration for "
              "'%s'", name);
    else if (diag_enabled(c->diag, "missing-declarations") && have_old &&
             (oldc.flags & CSF_USED) && (oldc.flags & CSF_IMPLICIT))
        cwarn(c, loc, "missing-declarations", "'%s' was used with no "
              "declaration before its definition", name);
    s.flags |= CSF_DEFINED | CSF_TREE_STATIC;
    s.def_loc = loc;
    fnode = funcdef_fnode(c, top);
    if (fnode != NO_NODE) {
        unsigned ff = cnode(c, fnode)->flags;
        if (!(ff & NF_KR) && (func_has_params(c, fnode) || (ff & NF_VARIADIC)))
            s.flags |= CSF_PROTO_DEF;
        else
            s.flags |= CSF_KR_DEF;
    }
    if (is_main) {
        TypeId r = type_base(TT, s.ty);
        if (plain_type(c, r) != TYPE_B(INT))
            cpedwarn(c, loc, "main", "return type of 'main' is not 'int'");
        else if (TYPE_QUALS(type_canon(TT, r)) & TQ_ATOMIC)
            cpedwarn(c, loc, "main", "'_Atomic'-qualified return type of "
                     "'main'");
        check_main_params(c, loc, s.ty);
        if (!pub)
            cpedwarn(c, loc, "main", "'main' is normally a non-static "
                     "function");
    }
    ref = pushdecl(c, &s, false);
    {
        CSym *t = csym(c, ref);
        t->flags |= CSF_DEFINED | CSF_TREE_STATIC |
                    (s.flags & (CSF_PROTO_DEF | CSF_KR_DEF));
        t->def_loc = loc;
    }
    dump_decl(c, csym(c, ref));
    c->func_sym = ref;
    c->cur_func_node = fd;
    c->ef[fd] = 0;
    c->func_node = fnode;
    c->ty[declared] = csym(c, ref)->ty;
    c->cb[declared] = ref + 1;
    cexpr_record_params(c, declared, ref, true);
    c->cv[declared] = 1;
    ensure_finish_cue(c, ref);
}

static bool in_seen(const uint32_t *seen, uint32_t n, uint32_t ref)
{
    uint32_t j;
    for (j = 0; j < n; j++)
        if (seen[j] == ref)
            return true;
    return false;
}

static void unbind_to(Checker *c, uint32_t mark)
{
    while (c->log.len > mark) {
        Bind *b = &c->log.data[--c->log.len];
        c->top[b->ns][b->ident] = b->prev;
    }
}

/* store_parm_decls, at the BODY leaf. */
static void body_visit(Checker *c, uint32_t i)
{
    uint32_t comp = c->par[i], fd, f, se, j, mark, inner, declared;
    FdParts fp;
    bool krf, proto;
    SrcLoc il, fnloc;
    Kids k;
    if (comp == NO_NODE || ntag(c, comp) != N_COMPOUND)
        return;
    fd = c->par[comp];
    if (fd == NO_NODE || ntag(c, fd) != N_FUNC_DEF || c->cur_func_node != fd ||
        c->func_node == NO_NODE)
        return;
    fd_parts(c, fd, &fp);
    declared = fp.declared;
    f = c->func_node;
    il = iloc(c, cnode(c, i)->tok);
    inner = inner_decl(c, f);
    fnloc = inner != NO_NODE ? tloc(c, cnode(c, inner)->tok)
                             : csym(c, c->func_sym)->loc;
    krf = (cnode(c, f)->flags & NF_KR) != 0;
    proto = !krf && (func_has_params(c, f) ||
                     (cnode(c, f)->flags & NF_VARIADIC));
    se = last_child(c, f);
    mark = vec_last(&c->scopes).log;
    if (proto) {
        if (c->log.len > mark) {
            cerror(c, fnloc, "old-style parameter declarations in prototyped "
                   "function definition");
            unbind_to(c, mark);
        }
        kids_get(c, f, &k);
        for (j = 0; j < k.n; j++) {
            uint32_t p = k.p[j];
            CSym *s;
            if (!is_real_param(c, p) || !c->cb[p])
                continue;
            if (!(c->cv[p] & 1) && is_void(c, c->ty[p]) &&
                !is_err(c, c->ty[p]))
                continue;
            s = csym(c, c->cb[p] - 1);
            if (s->name) {
                cbind(c, NS_ORD, s->name, c->cb[p] - 1);
                if (!(s->flags & CSF_USED))
                    warn_if_shadowing(c, s);
            } else
                cpedantic(c, s->loc, "ISO C does not support omitting "
                          "parameter names in function definitions before "
                          "C2X");
        }
        kids_free(&k);
        if (se != NO_NODE && ntag(c, se) == N_SCOPE_END) {
            uint32_t q;
            for (q = 0; q < c->cb[se]; q++) {
                const Bind *bd = &c->saved.data[c->cv[se] + q];
                if (!bd->ident)
                    continue;
                if (bd->ns == NS_ORD) {
                    const CSym *s = csym(c, bd->ref);
                    if (s->kind == CS_OBJ && (s->flags & CSF_PARAM))
                        continue;
                }
                cbind(c, bd->ns, bd->ident, bd->ref);
            }
        }
        return;
    }
    /* old style */
    {
        uint32_t *pl, *seen, np = 0, ns = 0, nk;
        uint32_t cap;
        cwarn(c, fnloc, "old-style-definition", "old-style function "
              "definition");
        if (c->ef[fd] & 1)
            cerror(c, il, "'[*]' not allowed in other than function "
                   "prototype scope");
        kids_get(c, f, &k);
        nk = k.n;
        cap = nk + (uint32_t)(c->log.len - mark) + 2;
        pl = xmalloc(cap * sizeof *pl);
        seen = xmalloc(cap * sizeof *seen);
        for (j = 0; j < nk; j++) {
            uint32_t kn = k.p[j], name, bi, ref;
            CSym *s;
            if (ntag(c, kn) != N_KR_IDENT)
                continue;
            name = cnode_ident(c, kn);
            bi = cbound_here(c, NS_ORD, name);
            if (bi) {
                ref = c->log.data[bi - 1].ref;
                s = csym(c, ref);
                if (s->flags & CSF_ERROR)
                    continue;
                if (!(s->kind == CS_OBJ && (s->flags & CSF_PARAM))) {
                    cerror(c, s->loc, "'%s' declared as a non-parameter",
                           cident(c, name));
                    continue;
                }
                if (in_seen(seen, ns, ref)) {
                    cerror(c, s->loc, "multiple parameters named '%s'",
                           cident(c, name));
                    continue;
                }
                if (is_void(c, s->ty)) {
                    cerror(c, s->loc, "parameter '%s' declared with void type",
                           cident(c, name));
                    s->ty = TYPE_B(INT);
                }
                warn_if_shadowing(c, s);
            } else {
                CSym n;
                memset(&n, 0, sizeof n);
                n.name = name;
                n.kind = CS_OBJ;
                n.flags = CSF_PARAM | CSF_DEFINED;
                n.ty = TYPE_B(INT);
                n.loc = fnloc;
                ref = pushdecl(c, &n, false);
                warn_if_shadowing(c, csym(c, ref));
                cpedwarn(c, fnloc, "implicit-int", "type of '%s' defaults to "
                         "'int'", cident(c, name));
            }
            seen[ns++] = ref;
            pl[np++] = ref;
        }
        kids_free(&k);
        for (j = (uint32_t)c->log.len; j-- > mark;) {
            const Bind *bd = &c->log.data[j];
            CSym *s;
            if (bd->ns != NS_ORD)
                continue;
            s = csym(c, bd->ref);
            if (!(s->kind == CS_OBJ && (s->flags & CSF_PARAM)))
                continue;
            if (!is_err(c, s->ty) && !type_is_complete(TT, s->ty)) {
                cerror(c, s->loc, "parameter '%s' has incomplete type",
                       sname(c, s));
                s->ty = ERRT;
            }
            if (!in_seen(seen, ns, bd->ref)) {
                cerror(c, s->loc, "declaration for parameter '%s' but no "
                       "such parameter", sname(c, s));
                if (np < cap)
                    pl[np++] = bd->ref;
            }
        }
        if (c->cd_have_proto) {
            TypeId pt = type_canon(TT, c->cd_proto);
            uint32_t n = type_ent(TT, pt)->n, li, pi;
            bool variadic = (type_ent(TT, pt)->flags & TF_VARIADIC) != 0;
            TypeId *pp = xmalloc((n + 1) * sizeof *pp);
            if (n)
                memcpy(pp, type_params(TT, pt), n * sizeof *pp);
            for (li = 0, pi = 0;; li++, pi++) {
                bool parm = li < np, tyvalid = pi < n;
                CSym *s;
                TypeId at, vt;
                if (!(parm || (tyvalid && !is_err(c, pp[pi]))))
                    break;
                (void)variadic;
                if (!parm || !tyvalid) {
                    cerror(c, il, "number of arguments doesn't match "
                           "prototype");
                    cerror(c, c->cd_proto_loc, "prototype declaration");
                    break;
                }
                s = csym(c, pl[li]);
                vt = pp[pi];
                if (is_err(c, s->ty) || is_err(c, vt))
                    continue;
                at = type_default_promote(TT, s->ty);
                if (!type_compatible(TT, plain_type(c, at),
                                     plain_type(c, vt)) ||
                    ((TYPE_QUALS(type_canon(TT, at)) & TQ_ATOMIC) !=
                     (TYPE_QUALS(type_canon(TT, vt)) & TQ_ATOMIC))) {
                    if ((TYPE_QUALS(type_canon(TT, at)) & TQ_ATOMIC) ==
                            (TYPE_QUALS(type_canon(TT, vt)) & TQ_ATOMIC) &&
                        plain_type(c, s->ty) == plain_type(c, vt)) {
                        cpedantic(c, s->loc, "promoted argument '%s' doesn't "
                                  "match prototype", sname(c, s));
                        cpedantic(c, c->cd_proto_loc, "prototype "
                                  "declaration");
                    } else {
                        cerror(c, s->loc, "argument '%s' doesn't match "
                               "prototype", sname(c, s));
                        cerror(c, c->cd_proto_loc, "prototype declaration");
                    }
                }
            }
            free(pp);
        } else if (np > 0) {
            CSym *fs = csym(c, c->func_sym);
            if (is_func(c, fs->ty) && !is_prototype(c, fs->ty)) {
                TypeId *ps = xmalloc(np * sizeof *ps), nt;
                for (j = 0; j < np; j++)
                    ps[j] = TYPE_UNQUAL(type_default_promote(
                        TT, csym(c, pl[j])->ty));
                nt = type_func(TT, type_base(TT, fs->ty), ps, np, TF_NOPROTO);
                free(ps);
                fs = csym(c, c->func_sym);
                fs->ty = nt;
                if (declared != NO_NODE)
                    c->ty[declared] = nt;
            }
        }
        free(pl);
        free(seen);
    }
}

/* ---- unused things --------------------------------------------------------- */

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

/* The identifier e names, as a target of a plain assignment: not a read. */
static void wr_mark(Checker *c, uint32_t e)
{
    for (;;) {
        switch (ntag(c, e)) {
        case N_IDENT:
            vec_push(&c->stack, e);
            return;
        case N_PAREN:
            e = first_child(c, e);
            continue;
        case N_MEMBER_EXPR:
            if (!(cnode(c, e)->flags & NF_ARROW)) {
                e = first_child(c, e);
                continue;
            }
            return;
        case N_INDEX: {
            uint32_t base = first_child(c, e);
            if (base != NO_NODE && is_arr(c, c->ty[base])) {
                e = base;
                continue;
            }
            return;
        }
        default:
            return;
        }
    }
}

/* An expression whose value is not used. */
static void st_mark(Checker *c, uint32_t e)
{
    while (ntag(c, e) == N_PAREN)
        e = first_child(c, e);
    if (ntag(c, e) == N_ASSIGN && tpunct(c, cnode(c, e)->tok) == P_ASSIGN) {
        wr_mark(c, first_child(c, e));
    } else if (ntag(c, e) == N_BINARY &&
               tpunct(c, cnode(c, e)->tok) == P_COMMA) {
        Kids k;
        kids_get(c, e, &k);
        if (k.n == 2) {
            st_mark(c, k.p[0]);
            st_mark(c, k.p[1]);
        }
        kids_free(&k);
    }
}

/* Which of the names are read (any use but assigning with '=') in nodes
 * [first, last]. */
static void read_scan(Checker *c, uint32_t first, uint32_t last,
                      const uint32_t *names, uint8_t *read, uint32_t n)
{
    uint32_t k, j;
    c->stack.len = 0;
    for (k = first; k <= last; k++) {
        if (ntag(c, k) == N_EXPR_STMT) {
            uint32_t e = first_child(c, k);
            if (e != NO_NODE)
                st_mark(c, e);
        } else if (ntag(c, k) == N_FOR) {
            Kids kk;
            kids_get(c, k, &kk);
            if (kk.n >= 4) {
                if (cexpr_is_expr(ntag(c, kk.p[1])))
                    st_mark(c, kk.p[1]);
                if (cexpr_is_expr(ntag(c, kk.p[3])))
                    st_mark(c, kk.p[3]);
            }
            kids_free(&kk);
        }
    }
    if (c->stack.len)
        qsort(c->stack.data, c->stack.len, sizeof(uint32_t), cmp_u32);
    for (k = first; k <= last; k++) {
        uint32_t id;
        if (ntag(c, k) != N_IDENT)
            continue;
        if (c->stack.len &&
            bsearch(&k, c->stack.data, c->stack.len, sizeof(uint32_t),
                    cmp_u32))
            continue;
        id = cnode_ident(c, k);
        for (j = 0; j < n; j++)
            if (names[j] == id)
                read[j] = 1;
    }
    c->stack.len = 0;
}

/* The warnings of pop_scope for the block of nodes [first, last] (a SCOPE
 * to its SCOPE_END, or to the end of a function's body). */
static void unused_scan(Checker *c, uint32_t first, uint32_t last)
{
    uint32_t k, nc = 0, cap = 0, j;
    uint32_t *cand = NULL, *names = NULL;
    uint8_t *read = NULL;
    int depth = 0;
    bool need_read = false;
    for (k = first; k <= last; k++) {
        unsigned t = ntag(c, k);
        if (t == N_SCOPE)
            depth++;
        else if (t == N_SCOPE_END)
            depth--;
        else if (t == N_DECLARED && depth == 1 && c->cb[k]) {
            if (nc == cap) {
                cap = cap ? cap * 2 : 8;
                cand = xrealloc(cand, cap * sizeof *cand);
            }
            cand[nc++] = k;
        }
    }
    if (!nc) {
        cstmt_emit_labels(c, first, -1);
        return;
    }
    names = xmalloc(nc * sizeof *names);
    read = xcalloc(nc, 1);
    for (j = 0; j < nc; j++) {
        const CSym *s = csym(c, c->cb[cand[j]] - 1);
        names[j] = s->name;
        if (s->kind == CS_OBJ && !(s->flags & CSF_PARAM) && s->name &&
            (s->flags & CSF_USED) && !sym_public(s))
            need_read = true;
    }
    if (need_read && diag_enabled(c->diag, "unused-but-set-variable"))
        read_scan(c, first, last, names, read, nc);
    else
        memset(read, 1, nc);
    for (j = nc; j-- > 0;) {
        CSym *s = csym(c, c->cb[cand[j]] - 1);
        cstmt_emit_labels(c, first, (int64_t)cand[j]);
        if (s->kind == CS_OBJ && !(s->flags & CSF_PARAM) && s->name) {
            if (!(s->flags & CSF_USED)) {
                cwarn(c, s->loc, "unused-variable", "unused variable '%s'",
                      sname(c, s));
                if (sym_public(s))
                    s->flags |= CSF_USED;
            } else if (!read[j] && !sym_public(s) &&
                       !(s->flags & CSF_ATTR_UNUSED))
                cwarn(c, s->loc, "unused-but-set-variable", "variable '%s' set "
                      "but not used", sname(c, s));
        } else if (s->kind == CS_FUNC && !sym_public(s) && !sym_defined(s) &&
                   s->name && !ref_file_scope(c->cb[cand[j]] - 1))
            cerror(c, s->loc, "nested function '%s' declared but never "
                   "defined", sname(c, s));
    }
    cstmt_emit_labels(c, first, -1);
    free(cand);
    free(names);
    free(read);
}

/* A compound statement or a for statement closed its scope. */
static void block_scope_end(Checker *c, uint32_t i)
{
    uint32_t f = cfirst(c, i);
    if (ntag(c, f) != N_SCOPE)
        return;
    unused_scan(c, f, i - 1);
}

/* finish_function, at the SCOPE_END of a function's scope. */
void cdecl_func_end(Checker *c, uint32_t se)
{
    uint32_t fd = c->par[se], f, j, np = 0, nk, p;
    FdParts fp;
    uint32_t *pl, *names;
    uint8_t *read;
    Kids k;
    if (fd == NO_NODE || ntag(c, fd) != N_FUNC_DEF || c->cur_func_node != fd ||
        c->func_node == NO_NODE)
        return;
    fd_parts(c, fd, &fp);
    f = c->func_node;
    if (fp.scope != NO_NODE)
        unused_scan(c, fp.scope, se - 1);
    /* the parameters, in order */
    kids_get(c, f, &k);
    nk = k.n;
    pl = xmalloc((nk + 1) * sizeof *pl);
    for (j = 0; j < nk; j++) {
        uint32_t kn = k.p[j];
        if (is_real_param(c, kn)) {
            if (c->cb[kn] && (c->cv[kn] & 1) &&
                !in_seen(pl, np, c->cb[kn] - 1))
                pl[np++] = c->cb[kn] - 1;
        } else if (ntag(c, kn) == N_KR_IDENT) {
            uint32_t bi = cbound_here(c, NS_ORD, cnode_ident(c, kn));
            if (bi) {
                uint32_t ref = c->log.data[bi - 1].ref;
                const CSym *s = csym(c, ref);
                if (s->kind == CS_OBJ && (s->flags & CSF_PARAM) &&
                    !in_seen(pl, np, ref))
                    pl[np++] = ref;
            }
        }
    }
    kids_free(&k);
    names = xmalloc((np + 1) * sizeof *names);
    read = xcalloc(np + 1, 1);
    for (j = 0; j < np; j++)
        names[j] = csym(c, pl[j])->name;
    if (np && fp.scope != NO_NODE &&
        diag_enabled(c->diag, "unused-but-set-parameter"))
        read_scan(c, fp.scope, se - 1, names, read, np);
    else
        memset(read, 1, np);
    for (j = 0; j < np; j++) {
        const CSym *s = csym(c, pl[j]);
        if ((s->flags & CSF_USED) && !read[j] &&
            !(s->flags & CSF_ATTR_UNUSED))
            cwarn(c, s->loc, "unused-but-set-parameter", "parameter '%s' set "
                  "but not used", sname(c, s));
    }
    /* typedefs made in the function and never used */
    if (fp.scope != NO_NODE && c->diag->nerrors == 0 &&
        diag_enabled(c->diag, "unused-local-typedefs")) {
        uint32_t q;
        for (q = fp.scope; q < se; q++)
            if (ntag(c, q) == N_DECLARED && c->cb[q]) {
                CSym *s = csym(c, c->cb[q] - 1);
                if (s->kind == CS_TYPEDEF && s->name && !(s->flags & CSF_USED)) {
                    cwarn(c, s->loc, "unused-local-typedefs", "typedef '%s' "
                          "locally defined but not used", sname(c, s));
                    s->flags |= CSF_USED;
                }
            }
    }
    for (j = 0; j < np; j++) {
        const CSym *s = csym(c, pl[j]);
        if (s->name && !(s->flags & CSF_USED))
            cwarn(c, s->loc, "unused-parameter", "unused parameter '%s'",
                  sname(c, s));
    }
    free(pl);
    free(names);
    free(read);
    /* back to the enclosing function, if any */
    p = c->par[fd];
    while (p != NO_NODE && ntag(c, p) != N_FUNC_DEF)
        p = c->par[p];
    c->func_sym = SYM_NONE;
    c->cur_func_node = NO_NODE;
    c->func_node = NO_NODE;
    if (p != NO_NODE) {
        FdParts op;
        fd_parts(c, p, &op);
        if (op.declared != NO_NODE && c->cb[op.declared]) {
            c->func_sym = c->cb[op.declared] - 1;
            c->cur_func_node = p;
            c->func_node = funcdef_fnode(c, op.declared - 1);
        }
    }
}

void cdecl_body_scope(Checker *c, uint32_t scope)
{
    (void)c;
    (void)scope;
}

/* ---- the end of the translation unit ----------------------------------------- */

void cdecl_finish_object(Checker *c, uint32_t ref)
{
    size_t k;
    if (!c->tentative.len || ref != c->tentative.data[0])
        return;
    /* incomplete types of objects that are never completed */
    for (k = 0; k < c->tentative.len; k++) {
        CSym *s = csym(c, c->tentative.data[k]);
        if (s->kind == CS_OBJ && (s->flags & CSF_TREE_STATIC) &&
            !is_err(c, s->ty) && !is_void(c, s->ty) && !type_is_complete(TT, s->ty) &&
            !is_incomplete_array(c, s->ty)) {
            cerror(c, s->loc, "storage size of '%s' isn't known", sname(c, s));
            s->ty = ERRT;
        }
    }
    /* the external scope: inline functions never defined */
    for (k = c->gsyms.len; k-- > 0;) {
        const CSym *s = &c->gsyms.data[k];
        if (s->kind == CS_FUNC && (s->flags & CSF_INLINE) && sym_public(s) &&
            !sym_defined(s) && s->name)
            cpedwarn(c, s->loc, "", "inline function '%s' declared but never "
                     "defined", sname(c, s));
    }
    /* the file scope */
    for (k = 0; k < c->gsyms.len; k++) {
        const CSym *s = &c->gsyms.data[k];
        if (s->kind == CS_OBJ && (s->flags & CSF_INNER_COMP) && s->name)
            cerror(c, s->loc, "type of array '%s' completed incompatibly with "
                   "implicit initialization", sname(c, s));
        if (s->kind == CS_FUNC && !sym_public(s) && !sym_defined(s) &&
            (s->flags & CSF_DECL_EXTERNAL) && s->name) {
            if (s->flags & CSF_USED)
                cpedwarn(c, s->loc, "", "'%s' used but never defined",
                         sname(c, s));
            else
                cwarn(c, s->loc, "unused-function", "'%s' declared 'static' "
                      "but never defined", sname(c, s));
        }
    }
    {
        int pass;
        for (pass = 0; pass < 2; pass++)
            for (k = 0; k < c->gsyms.len; k++) {
                CSym *s = &c->gsyms.data[k];
                if (s->kind == CS_OBJ && (s->flags & CSF_TENTATIVE) &&
                    !(s->flags & CSF_DECL_EXTERNAL) && s->name &&
                    sym_public(s) == (pass == 1) &&
                    is_incomplete_array(c, s->ty)) {
                    cwarn(c, s->loc, "", "array '%s' assumed to have one "
                          "element", sname(c, s));
                    s->ty = type_array(TT, type_base(TT, s->ty), 1);
                }
            }
    }
    for (k = 0; k < c->gsyms.len; k++) {
        const CSym *s = &c->gsyms.data[k];
        if (s->kind == CS_OBJ && s->sc == SC_REGISTER &&
            !(s->flags & (CSF_REGISTER_NAMED | CSF_PARAM)) && s->name &&
            !is_err(c, s->ty))
            cerror(c, s->loc, "register name not specified for '%s'",
                   sname(c, s));
    }
}

/* ---- the dispatch ------------------------------------------------------------- */

void cdecl_node(Checker *c, uint32_t i)
{
    switch (ntag(c, i)) {
    case N_SPECS:
        specs_visit(c, i);
        break;
    case N_EMPTY:
        if (cat_file_scope(c))
            cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C does not allow "
                      "extra ';' outside of a function");
        break;
    case N_FUNC_DEF:
        pop_specs(c, i);
        break;
    case N_DECL:
        decl_visit(c, i);
        break;
    case N_GOTO_EXPR:
        cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C forbids 'goto *expr;'");
        break;
    case N_DECLARED:
        declared_visit(c, i);
        cinit_declared(c, i);
        break;
    case N_INIT_DECL:
        init_decl_visit(c, i);
        break;
    case N_ATTRIBUTE: {
        /* parser diagnostics: gcc gives them in units with errors too */
        bool quiet = c->quiet;
        c->quiet = false;
        if (tokp(c, cnode(c, i)->tok)->kind == TK_PUNCT)
            cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C does not support "
                      "'[[]]' attributes before C2X");
        std_attr_unknown(c, i);
        gnu_attr_argc(c, i);
        c->quiet = quiet;
        break;
    }
    case N_STRUCT:
    case N_ENUM:
        struct_visit(c, i);
        break;
    case N_TAG:
        tag_visit(c, i);
        break;
    case N_OPEN:
        open_visit(c, i);
        break;
    case N_ENUMERATOR:
        enumerator_visit(c, i);
        break;
    case N_MEMBER:
        member_visit(c, i);
        break;
    case N_MEMBER_DECL:
        member_decl_visit(c, i);
        break;
    case N_STATIC_ASSERT:
        static_assert_visit(c, i);
        break;
    case N_PRAGMA:
        pragma_visit(c, i);
        break;
    case N_PARAM:
        param_visit(c, i);
        break;
    case N_FUNC:
        func_visit(c, i);
        break;
    case N_ARRAY:
        array_visit(c, i);
        break;
    case N_TYPE_NAME:
        typename_visit(c, i);
        break;
    case N_BODY:
        body_visit(c, i);
        break;
    case N_COMPOUND:
    case N_FOR:
        block_scope_end(c, i);
        break;
    default:
        break;
    }
}
