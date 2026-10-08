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
#include "c/cdecl_int.h"


/* ---- gcc's input_location ---------------------------------------------- */

/* The parser set input_location to token tok itself (a tag name, an
 * enumerator, a parse error). */
void iloc_event(Checker *c, uint32_t tok)
{
    uint64_t a = c->u->first_tok + tok + 1;
    if (a > c->iloc_tok)
        c->iloc_tok = a;
}

/* input_location while the parser's lookahead is token L. */
SrcLoc iloc(Checker *c, uint32_t L)
{
    uint32_t b;
    if (!c->u->ntoks)
        return c->last_bol;
    if (L >= c->u->ntoks)
        L = c->u->ntoks - 1;
    b = cbol_tok(c, L) - 1;
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


/* ---- type helpers --------------------------------------------------------- */

static TypeKind tkind(Checker *c, TypeId t)
{
    return type_ckind(TT, t);
}

static bool is_err(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ERROR;
}

/* Does function type t have a parameter of erroneous type? */
static bool func_err_param(Checker *c, TypeId t)
{
    const TypeEnt *e;
    uint32_t i;
    t = type_canon(TT, t);
    if (type_ckind(TT, t) != TY_FUNC)
        return false;
    e = type_ent(TT, t);
    for (i = 0; i < e->n; i++)
        if (is_err(c, type_params(TT, t)[i]))
            return true;
    return false;
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

/* A comma operator outside a sizeof/alignof operand: the expression is not an
 * integer constant expression, though gcc folds it (an array size of (2, 2)
 * makes a VLA). */
static bool evaluated_comma(Checker *c, uint32_t e)
{
    Kids k;
    uint32_t j;
    bool r = false;
    if (ntag(c, e) == N_SIZEOF_EXPR || ntag(c, e) == N_SIZEOF_TYPE ||
        ntag(c, e) == N_ALIGNOF_EXPR || ntag(c, e) == N_ALIGNOF_TYPE ||
        ntag(c, e) == N_HAS_ATTR)       /* unevaluated operands */
        return false;
    if (ntag(c, e) == N_BINARY && tpunct(c, c->nodes[e].tok) == P_COMMA)
        return true;
    kids_get(c, e, &k);
    for (j = 0; j < k.n && !r; j++)
        r = evaluated_comma(c, k.p[j]);
    kids_free(&k);
    return r;
}

/* Is the constant expression node e an INTEGER_CST (K_ICE, or a folded
 * constant)?  If not, may it still be folded to one? */
static bool node_int_cst(Checker *c, uint32_t e)
{
    return (c->ck[e] == K_ICE || (c->ck[e] == K_FOLD && (c->ef[e] & EF_CST))) &&
           !evaluated_comma(c, e);
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
    TypeId pre;              /* DC_PARM: the array type before it decayed */
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

static unsigned quals_of_warn(Checker *c, uint32_t node)
{
    unsigned q = quals_of(c, node);
    if (q) {
        Kids k;
        uint32_t j;
        unsigned seen = 0;
        uint32_t prev[4] = {0, 0, 0, 0};
        kids_get(c, node, &k);
        for (j = 0; j < k.n; j++) {
            uint32_t tk;
            unsigned bit, ix;
            const char *nm;
            if (ntag(c, k.p[j]) != N_QUAL)
                continue;
            tk = cnode(c, k.p[j])->tok;
            switch (tckw(c, tk)) {
            case CK_CONST: bit = TQ_CONST; ix = 0; nm = "const"; break;
            case CK_VOLATILE: bit = TQ_VOLATILE; ix = 1; nm = "volatile"; break;
            case CK_RESTRICT: bit = TQ_RESTRICT; ix = 2; nm = "restrict"; break;
            default:
                bit = TQ_ATOMIC; ix = 3; nm = "_Atomic";
                cped11(c, tloc(c, tk),
                       "ISO C99 does not support the '_Atomic' qualifier");
            }
            /* gcc: a repeated pointer qualifier (not from a macro) */
            if ((seen & bit) && !tfrom_macro(c, tk) && !tfrom_macro(c, prev[ix]))
                cwarn(c, tloc(c, tk), cc90_id(c, NULL) ? cc90_id(c, NULL)
                      : "duplicate-decl-specifier",
                      "duplicate '%s' declaration specifier", nm);
            seen |= bit;
            prev[ix] = tk;
        }
        kids_free(&k);
    }
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
    uint32_t k = f, lo = cfirst(c, f);
    /* a function declarator inside a parameter has a prototype scope of its own */
    while (k > lo) {
        k--;
        if (ntag(c, k) == N_FUNC) {
            k = cfirst(c, k);
            continue;
        }
        if (ntag(c, k) == N_ARRAY && (cnode(c, k)->flags & NF_STAR))
            return true;
    }
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
        if (!funcdef) {
            bool q = c->quiet;
            uint32_t last = NO_NODE;
            DiagOrd o0 = diag_ord(c->diag, q ? ORD_LATE : ORD_NORMAL);
            c->quiet = false;     /* gcc issues this in the declarator parse */
            kids_get(c, f, &k);
            for (j = 0; j < k.n; j++)
                if (ntag(c, k.p[j]) == N_KR_IDENT)
                    last = cnode(c, k.p[j])->tok;
            kids_free(&k);
            /* `(a,)`: gcc has already asked for the identifier at the ) */
            if (last != NO_NODE && tpunct(c, last + 1) == P_COMMA &&
                tpunct(c, last + 2) == P_RPAREN)
                il = ctok_loc(c, last + 2);
            cpedwarn(c, il, "", "parameter names (without types) in function "
                     "declaration");
            c->quiet = q;
            diag_ord(c->diag, o0);
        }
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
    /* gcc: a forward-referenced enum is wider than any bit-field of it */
    if (!type_is_complete(TT, e))
        return true;
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
                cconst_overflow(c, loc);
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
    if (TYPE_UNQUAL(type_canon(TT, t)) != TYPE_B(INT) &&
        TYPE_UNQUAL(type_canon(TT, t)) != TYPE_B(UINT) &&
        TYPE_UNQUAL(type_canon(TT, t)) != TYPE_B(BOOL))
        cc90(c, loc, NULL, "type of bit-field '%s' is a GCC extension", nm);
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

/* -Wlarger-than=N: an object declaration (layout_decl) of more than N bytes. */
static void larger_than(Checker *c, SrcLoc loc, uint32_t name, TypeId ty)
{
    uint64_t lim, sz;
    bool ok;
    if (!name || !diag_enabled(c->diag, "larger-than="))
        return;
    /* gcc: by default, the largest valid object */
    if (!(lim = diag_option_size(c->diag, "larger-than=")))
        lim = INT64_MAX;
    sz = type_size(TT, ty, &ok);
    if (ok && sz > lim)
        cwarn(c, loc, "larger-than=", "size of '%s' %llu bytes exceeds "
              "maximum object size %llu", cident(c, name),
              (unsigned long long)sz, (unsigned long long)lim);
}

static bool valid_array_size(Checker *c, SrcLoc loc, TypeId elem, uint64_t n,
                             uint32_t name)
{
    bool ok;
    uint64_t es = type_size(TT, elem, &ok);
    if (!ok || !es)
        return true;
    if (n > (uint64_t)INT64_MAX / es) {
        /* gcc prints the size only when it is a representable constant */
        if (n <= UINT64_MAX / es) {
            unsigned long long sz = (unsigned long long)(n * es);
            if (name)
                cerror(c, loc, "size '%llu' of array '%s' exceeds maximum "
                       "object size '9223372036854775807'", sz,
                       cident(c, name));
            else
                cerror(c, loc, "size '%llu' of array exceeds maximum object "
                       "size '9223372036854775807'", sz);
        } else if (name)
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
        if (filescope) {
            /* -Wc++-compat lets extern const pass (a C++ idiom for
             * internal linkage) */
            if (!(type_quals & TQ_CONST) || !diag_enabled(c->diag, "c++-compat"))
                cwarn(c, loc, "", "'%s' initialized and declared 'extern'",
                      cident(c, name));
        }
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
            bool vla = false, sized = false, this_varies = false;
            uint64_t n = 0;
            const Node *an = cnode(c, dn);
            array_ptr_quals = quals_of_warn(c, dn);
            array_ptr_attrs = has_child_attr(c, dn);
            array_parm_static = (an->flags & NF_STATIC) != 0;
            if ((an->flags & NF_STAR) && (c->cv[dn] & 1)) {
                type = ERRT;        /* gcc drops the declaration */
                continue;
            }
            unspec = (an->flags & NF_STAR);
            array_parm_vla_unspec = unspec;
            if (unspec)
                cc90(c, cnode_loc(c, dn), NULL, "ISO C90 does not support "
                     "'[*]' array declarators");
            if (array_parm_static || array_ptr_quals)
                cc90(c, cnode_loc(c, dn), NULL, "ISO C90 does not support "
                     "'static' or type qualifiers in parameter array "
                     "declarators");
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
                this_varies = false;
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
                } else if (!cexpr_rvalue_ok_at(c, sz,
                                               cnode_loc(c, dn))) {
                    /* require_complete_type, at the '[' */
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
                                cc90(c, iloc(c, ltok), "vla", "ISO C90 "
                                     "forbids variable length array '%s'",
                                     cident(c, name));
                            else
                                cc90(c, iloc(c, ltok), "vla", "ISO C90 "
                                     "forbids variable length array");
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
                        cc90(c, iloc(c, ltok), "vla", "ISO C90 forbids "
                             "variable length array '%s'", cident(c, name));
                    else
                        cc90(c, iloc(c, ltok), "vla", "ISO C90 forbids "
                             "variable length array");
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
                if (!array_parm_vla_unspec)
                    cc90(c, loc, NULL, "ISO C90 does not support flexible "
                         "array members");
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
                if (sp->node != NO_NODE) {
                    /* layout_type: a user alignment the element size does not
                     * respect */
                    bool ok;
                    uint64_t esz = type_size(TT, type, &ok);
                    unsigned al = type_align(TT, type);
                    uint32_t up = c->par[sp->node];
                    while (up != NO_NODE && ntag(c, up) != N_HAS_ATTR &&
                           ntag(c, up) != N_DECL)
                        up = c->par[up];
                    if (ok && al && esz % al)
                        cerror(c, up != NO_NODE && ntag(c, up) == N_HAS_ATTR
                                  ? cdecl_line_start_loc(c, first_tok(c, sp->node))
                                  : iloc(c, ltok),
                               esz < al ? "alignment of array elements is "
                               "greater than element size" :
                               "size of array element is not a multiple of "
                               "its alignment");
                }
                if (!vla) {      /* an array of variable-size type */
                    TypeId et = type_canon(TT, type);
                    while (tkind(c, et) == TY_ARRAY)
                        et = type_canon(TT, type_base(TT, et));
                    if (tkind(c, et) == TY_VLA ||
                        ((tkind(c, et) == TY_STRUCT || tkind(c, et) == TY_UNION) &&
                         (type_record(TT, et)->flags & RF_VLA)))
                    {
                        vla = true;
                        sized = sz != NO_NODE;
                    }
                }
                if (vla) {
                    char *tx = this_varies ? cparm_dim_text(c, dn) : NULL;
                    type = type_vla_x(TT, type, n, sized,
                                      tx ? type_vla_text(TT, tx) : 0);
                    free(tx);
                }
                else if (sz != NO_NODE || (unspec && !vla))
                    type = type_array(TT, type, n);
                else if (ctx == DC_FIELD)
                    type = type_array_flex(TT, type);
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
            type_quals = quals_of_warn(c, pn);
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
                g->s.align = sp->align;
        }
    }
    if (!g->s.align && sp->align && !sp->alignas_seen)
        g->s.align = sp->align;

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
            g->pre = type;
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
                     name && *cident(c, name) ? cident(c, name)
                                              : "({anonymous})");
        if (sp->is_noreturn)
            cpedwarn(c, loc, "", "parameter '%s' declared '_Noreturn'",
                     name && *cident(c, name) ? cident(c, name)
                                              : "({anonymous})");
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
            type = qualify(c, qualify(c, type, type_quals, ltok), type_quals,
                           ltok);     /* gcc qualifies twice (a 2nd error) */
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
        if (type_quals && !is_err(c, type))
            type = qualify(c, type, type_quals, ltok);
        if (sc == SC_AUTO && fs)
            g->s.flags &= ~(unsigned)CSF_DECL_EXTERNAL;
        else if (sp->is_inline && sc != SC_STATIC) {
            /* DECL_EXTERNAL = (extern) == gnu89 semantics */
            if ((sc == SC_EXTERN) == sp->attrs.gnu_inline)
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
            if (sp->attrs.gnu_inline)
                g->s.flags |= CSF_GNU_INLINE;
            if (sp->is_noreturn) {
                cped11(c, loc, "ISO C99 does not support '_Noreturn'");
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
            type = qualify(c, qualify(c, type, type_quals, ltok), type_quals,
                           ltok);     /* gcc qualifies twice (a 2nd error) */
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

/* Two function types whose return types differ only by volatile (gcc
 * accepts them, with a pedwarn: the old way to write noreturn). */
static bool volatile_ret_only(Checker *c, TypeId a, TypeId b)
{
    TypeId ca = type_canon(TT, a), cb = type_canon(TT, b), ra, rb;
    const TypeEnt *ea, *eb;
    if (type_kind(TT, ca) != TY_FUNC || type_kind(TT, cb) != TY_FUNC)
        return false;
    ea = type_ent(TT, ca);
    eb = type_ent(TT, cb);
    ra = type_base(TT, ca);
    rb = type_base(TT, cb);
    if ((TYPE_QUALS(ra) ^ TYPE_QUALS(rb)) != TQ_VOLATILE)
        return false;
    ra = TYPE_UNQUAL(ra) | (TYPE_QUALS(ra) & ~TQ_VOLATILE);
    rb = TYPE_UNQUAL(rb) | (TYPE_QUALS(rb) & ~TQ_VOLATILE);
    return compat_gcc(c, type_func(TT, ra, type_params(TT, ca), ea->n,
                                   ea->flags),
                      type_func(TT, rb, type_params(TT, cb), eb->n,
                                eb->flags));
}

/* The note of locate_old_decl. */
static void locate_old_decl(Checker *c, Diagnostic *d, const CSym *o)
{
    if (!d)
        return;
    if (sym_defined(o) || o->kind == CS_ENUMCONST)
        cnote(c, d, o->loc, "previous definition of '%s' with type %s",
              sname(c, o), type_q_decl(TT, o->ty));
    else if (o->flags & CSF_IMPLICIT)
        cnote(c, d, o->loc, "previous implicit declaration of '%s' with type "
              "%s", sname(c, o), type_q_decl(TT, o->ty));
    else
        cnote(c, d, o->loc, "previous declaration of '%s' with type %s",
              sname(c, o), type_q_decl(TT, o->ty));
}

/* -Wtraditional: a file-scope declaration without 'static' after a static one. */
static void nonstatic_follows_static(Checker *c, CSym *nw, const CSym *o)
{
    Diagnostic *d;
    if (cin_system(c, nw->loc))
        return;
    d = cwarn_d(c, DL_WARNING, nw->loc, "traditional", "non-static declaration "
                "of '%s' follows static declaration", sname(c, nw));
    if (d)
        locate_old_decl(c, d, o);
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

static bool has_err_param(Checker *c, TypeId t)
{
    TypeId ct = type_canon(TT, t);
    uint32_t k;
    for (k = 0; k < (uint32_t)type_ent(TT, ct)->n; k++)
        if (is_err(c, type_params(TT, ct)[k]))
            return true;
    return false;
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
    bool pedwarned = false, enum_and_int = false, note_new = false;
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
        /* gcc's noreturn is a volatile function type: 'volatile' written on
         * one declaration agrees with 'noreturn' on the other */
        if (nw->kind == CS_FUNC && type_ckind(TT, a) == TY_FUNC &&
            type_ckind(TT, b) == TY_FUNC) {
            if (!(TYPE_QUALS(a) & TQ_VOLATILE) && (TYPE_QUALS(b) & TQ_VOLATILE) &&
                (o->flags & CSF_NORETURN))
                a |= TQ_VOLATILE;
            else if (!(TYPE_QUALS(b) & TQ_VOLATILE) &&
                     (TYPE_QUALS(a) & TQ_VOLATILE) && (nw->flags & CSF_NORETURN))
                b |= TQ_VOLATILE;
            /* a qualifier written on a function (a typedef of function type)
             * is a flag of its declaration, not part of the type */
            if ((TYPE_QUALS(a) ^ TYPE_QUALS(b)) & (TQ_CONST | TQ_VOLATILE)) {
                a &= ~(TypeId)(TQ_CONST | TQ_VOLATILE);
                b &= ~(TypeId)(TQ_CONST | TQ_VOLATILE);
            }
        }
        if (!compat_gcc(c, a, b)) {
            if (nw->kind == CS_FUNC && sym_defined(nw) &&
                is_void(c, type_base(TT, oldtype)) &&
                type_canon(TT, type_base(TT, newtype)) == TYPE_B(INT) &&
                new_implicit_int && !sym_defined(o)) {
                d = cpedwarn(c, nw->loc, "", "conflicting types "
                             "for '%s'", sname(c, nw));
                pedwarned = d != NULL;
                wd = d;
                nw->ty = newtype = oldtype;
                *newtypep = newtype;
            } else if (nw->kind == CS_FUNC &&
                       is_void(c, type_base(TT, newtype)) &&
                       type_canon(TT, type_base(TT, oldtype)) == TYPE_B(INT) &&
                       (o->flags & CSF_IMPLICIT) && !sym_defined(o)) {
                d = cpedwarn(c, sym_defined(nw) ? nw->loc : iloc(c, c->cd_ltok),
                             "", "conflicting types for '%s'; have %s",
                             sname(c, nw), type_q(TT, newtype));
                pedwarned = d != NULL;
                wd = d;
                note_new = true; /* gcc retypes the implicit decl first */
                oldtype = newtype;
                *oldtypep = oldtype;
            } else if (nw->kind == CS_FUNC && func_err_param(c, newtype)) {
                /* a parameter that was already diagnosed matches anything */
                nw->ty = newtype = oldtype;
                *newtypep = newtype;
            } else if (nw->kind == CS_FUNC && volatile_ret_only(c, a, b)) {
                int k;
                /* comptypes runs three times (twice for a block-scope one) */
                for (k = 0; k < (cat_file_scope(c) ? 3 : 2); k++)
                    cpedwarn(c, cinput_loc(c, c->cd_ltok), "", "function "
                             "return types not compatible due to 'volatile'");
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
        } else if (nw->kind == CS_FUNC && type_tu_mixed(TT, a, b)) {
            int k;
            for (k = 0; k < 2; k++)
                cpedwarn(c, cinput_loc(c, c->cd_ltok), "pedantic", "function types "
                         "not truly compatible in ISO C");
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
            d = cped11(c, nw->loc, "redefinition of typedef '%s'",
                          sname(c, nw));
            locate_old_decl(c, d, o);
        }
        return true;
    } else if (nw->kind == CS_FUNC) {
        if (sym_defined(nw)) {
            /* an extern inline (gnu_inline) definition may be overridden */
            if (sym_defined(o) &&
                (!extern_inline(o) || extern_inline(nw) ||
                 !(o->flags & CSF_GNU_INLINE))) {
                d = cerror_d(c, nw->loc, "redefinition of '%s'", sname(c, nw));
                locate_old_decl(c, d, o);
                return false;
            }
        } else if (sym_defined(o) && !is_prototype(c, oldtype) &&
                   !o->olddef_merged &&
                   is_prototype(c, newtype) &&
                   (type_ent(TT, type_canon(TT, oldtype))->n ||
                    !has_err_param(c, newtype))) {
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
            nonstatic_follows_static(c, nw, o);
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
                if (sym_public(nw))
                    nonstatic_follows_static(c, nw, o);
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
    if (nw->kind == CS_OBJ && nfile && ofile && !sym_external(nw) &&
        !sym_external(o) && !cin_system(c, nw->loc)) {
        d = cwarn_d(c, DL_WARNING, nw->loc, "c++-compat", "duplicate "
                    "declaration of '%s' is invalid in C++", sname(c, nw));
        locate_old_decl(c, d, o);
    }
    if (nw->kind == CS_OBJ && (nw->flags & CSF_PARAM) &&
        !(o->flags & CSF_FWD)) {
        d = cerror_d(c, nw->loc, "redefinition of parameter '%s'",
                     sname(c, nw));
        locate_old_decl(c, d, o);
        return false;
    }
    if (nw->kind == CS_FUNC && nw->parms && o->parms)
        cparm_compare(c, nw->parms, o->parms);
    if (!wd && !((nw->flags & CSF_PARAM) && (o->flags & CSF_FWD)) &&
        !(nw->kind == CS_FUNC && sym_defined(nw) && !sym_defined(o)) &&
        !(sym_external(o) && !sym_external(nw)) &&
        !(nw->kind == CS_OBJ && sym_defined(nw) && !sym_defined(o)))
        wd = cwarn_d(c, DL_WARNING, nw->loc, "redundant-decls", "redundant "
                     "redeclaration of '%s'", sname(c, nw));
    if (wd || pedwarned)
    {
        CSym tmp = *o;
        tmp.ty = newtype;
        locate_old_decl(c, wd, note_new ? &tmp : o);
    }
    return true;
}

/* handle_weak_attribute + declare_weak for a declaration of an object or
 * function: an inline function ignores it, a symbol without external linkage
 * cannot be weak. */
static void weak_apply(Checker *c, CSym *s, bool is_inline)
{
    if (s->kind == CS_FUNC && is_inline) {
        cwarn(c, s->loc, "attributes", "inline function '%s' declared weak",
              sname(c, s));
        return;
    }
    if (!sym_public(s)) {
        cerror(c, s->loc, "weak declaration of '%s' must be public",
               sname(c, s));
        return;
    }
    s->flags |= CSF_WEAK;
}

/* Whether the specifiers sn or the declarator idecl give the function
 * noinline (noipa implies it), unless the attribute was ignored. */
static bool given_noinline(Checker *c, uint32_t sn, uint32_t idecl)
{
    unsigned k;
    bool r = false;
    static const char *const names[] = {"noinline", "noipa"};
    for (k = 0; k < 2; k++)
        r = r || (sn != NO_NODE && attrs_item_named(c, sn, names[k])) ||
            attrs_item_named(c, idecl, names[k]);
    for (k = 0; k < c->nign; k++)
        if (!strcmp(c->ign[k], "noinline"))
            r = false;
    return r;
}

/* The deallocator named by a malloc attribute `holder` writes (into out). */
static bool attrs_malloc_dealloc(Checker *c, uint32_t holder, char *out,
                                 size_t n)
{
    Kids k;
    uint32_t j;
    bool found = false;
    if (holder == NO_NODE)
        return false;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n && !found; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n && !found; q++) {
            char an[32];
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (!strcmp(an, "malloc")) {
                char *cm;
                cdecl_attr_args(c, it.p[q], out, n);
                cm = strchr(out, ',');
                if (cm)
                    *cm = 0;
                /* gcc's own deallocators imply no noinline */
                found = out[0] && !isdigit((unsigned char)out[0]) &&
                        strncmp(out, "__builtin_", 10) &&
                        strcmp(out, "free") && strcmp(out, "realloc");
            }
        }
        kids_free(&it);
    }
    kids_free(&k);
    return found;
}

/* The first token of loc's line, where gcc's input_location is. */
static SrcLoc bol_loc(Checker *c, SrcLoc loc)
{
    uint32_t line, col = 1, len;
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    const char *t;
    if (!f)
        return loc;
    srcmgr_linecol(f, loc, &line, &col);
    t = srcmgr_line_text(f, line, &len);
    for (col = 1; col <= len && (t[col - 1] == ' ' || t[col - 1] == '	'); col++)
        ;
    return srcmgr_loc_of(f, line, col);
}

/* An inline function given the noinline attribute (malloc with a
 * deallocator implies it; with optimization it is ignored instead). */
static void inline_given(Checker *c, const CSym *s, bool is_inline,
                         uint32_t sn, uint32_t idecl)
{
    char dn[64];
    if (s->kind == CS_FUNC && is_inline &&
        (attrs_malloc_dealloc(c, sn, dn, sizeof dn) ||
         attrs_malloc_dealloc(c, idecl, dn, sizeof dn))) {
        if (c->opt.optimize)
            cwarn(c, bol_loc(c, s->loc), "attributes", "'malloc (%s)' "
                  "attribute ignored on functions declared 'inline'", dn);
        else
            cwarn(c, s->loc, "attributes", "inline function '%s' given "
                  "attribute 'noinline'", sname(c, s));
        return;
    }
    if (s->kind == CS_FUNC && is_inline && given_noinline(c, sn, idecl))
        cwarn(c, s->loc, "attributes", "inline function '%s' given attribute "
              "'noinline'", sname(c, s));
}

/* diagnose_mismatched_decls: an inline declaration after one with
 * noinline, or noinline after an inline one. */
/* Append the options of one optimize("...") argument string (quotes and all
 * as cdecl_attr_args writes it) to acc, comma separated. */
static void opt_acc(char *acc, size_t n, const char *arg)
{
    size_t l = strlen(acc);
    for (; *arg && l + 2 < n; arg++) {
        if (*arg == '"')
            continue;
        acc[l++] = *arg == ' ' ? ',' : *arg;
    }
    if (l + 2 < n)
        acc[l++] = ',';
    acc[l] = 0;
}

static int opt_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The accumulated options in a form that is equal exactly when gcc's
 * optimization nodes are: an -O level makes the order significant (it only
 * defaults flags not set so far), otherwise the order does not matter. */
static void opt_canon(char *acc)
{
    char *tok[64], *s, *e;
    size_t nt = 0, k;
    bool lvl = false;
    for (s = acc; *s && nt < 64; s = e) {
        e = strchr(s, ',');
        if (!e)
            break;
        *e++ = 0;
        if (*s) {
            tok[nt++] = s;
            lvl = lvl || *s == 'O' || (*s >= '0' && *s <= '9');
        }
    }
    if (!lvl)
        qsort(tok, nt, sizeof *tok, opt_cmp);
    {
        char out[512];
        size_t l = 0;
        for (k = 0; k < nt && l + 2 < sizeof out; k++)
            l += (size_t)snprintf(out + l, sizeof out - l, "%s,", tok[k]);
        out[l] = 0;
        memcpy(acc, out, l + 1);
    }
}

static void attrs_opt_args(Checker *c, uint32_t holder, char *acc, size_t n)
{
    Kids k;
    uint32_t j;
    kids_get(c, holder, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[32], arg[256];
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (strcmp(an, "optimize"))
                continue;
            cdecl_attr_args(c, it.p[q], arg, sizeof arg);
            opt_acc(acc, n, arg);
        }
        kids_free(&it);
    }
    kids_free(&k);
}

static void aset_opt_args(const Checker *c, uint32_t set, char *acc, size_t n)
{
    size_t k, ord[32], m = 0;
    if (!set)
        return;
    for (k = c->ahead.data[set - 1]; k && m < 32; k = c->anames.data[k - 1].prev)
        if (!strcmp(c->anames.data[k - 1].name, "optimize"))
            ord[m++] = k;
    while (m--)
        opt_acc(acc, n, c->anames.data[ord[m] - 1].arg);
}

/* Whether the optimize options written on the new declaration differ from
 * the definition's (none at all counts as different). */
static bool opt_mismatch(Checker *c, const CSym *o, uint32_t sn, uint32_t idecl)
{
    char a[512] = "", b[512] = "";
    aset_opt_args(c, o->aset, a, sizeof a);
    if (!*a)
        return true;
    if (sn != NO_NODE)
        attrs_opt_args(c, sn, b, sizeof b);
    attrs_opt_args(c, idecl, b, sizeof b);
    opt_canon(a);
    opt_canon(b);
    return strcmp(a, b) != 0;
}

static void inline_follows(Checker *c, const CSym *nw, uint32_t ltok,
                           uint32_t sn, uint32_t idecl)
{
    uint32_t ref = lookup_ord(c, nw->name);
    const CSym *o;
    Diagnostic *d = NULL;
    bool nw_noinline;
    if (ref == SYM_NONE || csym(c, ref)->kind != CS_FUNC)
        return;
    o = csym(c, ref);
    nw_noinline = given_noinline(c, sn, idecl);
    if ((nw->flags & CSF_INLINE) && !(o->flags & CSF_INLINE) &&
        cdecl_aset_has(c, o->aset, "noinline", NULL))
        d = cwarn_d(c, DL_WARNING, iloc(c, ltok), "attributes", "inline declaration "
                    "of '%s' follows declaration with attribute 'noinline'",
                    sname(c, nw));
    else if ((o->flags & CSF_INLINE) && nw_noinline)
        d = cwarn_d(c, DL_WARNING, nw->loc, "attributes", "declaration of "
                    "'%s' with attribute 'noinline' follows inline "
                    "declaration", sname(c, nw));
    else if ((o->flags & CSF_DEFINED) &&
             ((sn != NO_NODE && attrs_item_named(c, sn, "optimize")) ||
              attrs_item_named(c, idecl, "optimize")) &&
             opt_mismatch(c, o, sn, idecl))
        d = cwarn_d(c, DL_WARNING, iloc(c, ltok), "attributes", "optimization "
                    "attribute on '%s' follows definition but the attribute "
                    "doesn't match", sname(c, nw));
    locate_old_decl(c, d, o);
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
    if (nw->kind == CS_TYPEDEF) {
        m.ty = o->ty;
        if (infunc)
            m.flags |= CSF_USED;   /* gcc: a redeclared local typedef is used */
    } else
        m.ty = type_composite(TT, newtype, oldtype);
    if ((!sym_defined(nw) && sym_defined(o)) || (old_proto && !new_proto))
        m.loc = o->loc;
    m.flags |= o->flags & (CSF_DEFINED | CSF_USED | CSF_CUSED | CSF_NORETURN | CSF_THREAD |
                           CSF_INLINE | CSF_BLOCK_EXTERN | CSF_TENTATIVE |
                           CSF_WEAK | CSF_WEAKREF | CSF_ADDR_WARNED | CSF_DEPRECATED |
                           CSF_UNAVAILABLE | CSF_INNER_COMP | CSF_GNU_INLINE | CSF_PURE | CSF_CONSTFN);
    m.olddef_merged = o->olddef_merged ||
                      (new_def && !new_proto && !sym_defined(o));
    /* merge_weak: PR 49899, a static function cannot become weak and public */
    if ((nw->flags & CSF_WEAK) && !(o->flags & CSF_WEAK) && !sym_public(o) &&
        sym_public(nw))
        cerror(c, m.loc, "weak declaration of '%s' being applied to a already "
               "existing, static definition", sname(c, nw));
    if (!m.dep_msg)
        m.dep_msg = o->dep_msg;
    if (o->sect && nw->sect &&
        strcmp(c->dep_msgs.data[o->sect - 1], c->dep_msgs.data[nw->sect - 1]))
        cwarn(c, iloc(c, c->cd_ltok), "attributes", "ignoring attribute "
              "'section (\"%s\")' because it conflicts with previous "
              "'section (\"%s\")'", c->dep_msgs.data[nw->sect - 1],
              c->dep_msgs.data[o->sect - 1]);
    if (o->sect)
        m.sect = o->sect;
    m.nonnull |= o->nonnull;
    if (o->fmt)
        m.fmt = o->fmt;
    if (o->fmtarg)
        m.fmtarg = o->fmtarg;
    if (o->aset)
        m.aset = o->aset;
    if (o->parms) {
        m.parms = o->parms;
        if (nw->parms != o->parms)
            cparm_release(c, nw->parms);
    }
    if (!new_def)
        m.flags |= o->flags & (CSF_PROTO_DEF | CSF_KR_DEF);
    m.def_loc = sym_defined(nw) ? nw->loc : o->def_loc;
    if (sym_defined(nw) && !nw->def_loc)
        m.def_loc = nw->loc;
    m.flags &= ~(unsigned)CSF_IMPLICIT;
    if (o->align > m.align)
        m.align = o->align;
    if (o->ualign > m.ualign)
        m.ualign = o->ualign;
    if (is_fn && (nw->flags & CSF_INLINE || o->flags & CSF_INLINE) &&
        !((nw->flags | o->flags) & CSF_GNU_INLINE) &&
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
                             implicit_int, &nt, &ot)) {
        c->redecl_failed = true;
        return false;
    }
    c->vis_old = *o;
    c->vis_old_ok = true;
    merge_decls(c, nw, o, nt, ot);
    return true;
}

static bool fn_pointer_type(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_PTR && is_func(c, type_base(TT, t));
}

/* The option of a parameter/local shadow warning: -Wshadow itself, else
 * -Wshadow=compatible-local when the types are compatible, else
 * -Wshadow=local. */
static const char *shadow_id(Checker *c, const CSym *old, const CSym *x)
{
    if (diag_enabled(c->diag, "shadow"))
        return "shadow";
    return type_compatible(TT, old->ty, x->ty) ? "shadow=compatible-local"
                                               : "shadow=local";
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
            d = cwarn_d(c, DL_WARNING, x->loc, shadow_id(c, old, x),
                        "declaration of '%s' shadows a parameter",
                        sname(c, x));
        else if (ref_file_scope(ref)) {
            if (old->kind == CS_FUNC && x->kind != CS_FUNC &&
                !fn_pointer_type(c, x->ty))
                continue;
            d = cwarn_d(c, DL_WARNING, x->loc, "shadow", "declaration of '%s' "
                        "shadows a global declaration", sname(c, x));
        } else
            d = cwarn_d(c, DL_WARNING, x->loc, shadow_id(c, old, x),
                        "declaration of '%s' shadows a previous local",
                        sname(c, x));
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

static void typedef_tag_clash(Checker *c, uint32_t name, SrcLoc at,
                              SrcLoc old);

/* -Wc++-compat: a typedef name used inside a struct body; a field of an open
 * struct with that name would hide it in C++. */
void cxx_typedef_in_struct(Checker *c, uint32_t ident, uint32_t tok)
{
    size_t k = c->fields.len;
    if (!diag_enabled(c->diag, "c++-compat"))
        return;
    vec_push(&c->tdseen, ident);
    while (k-- > 0)
        if (c->fields.data[k].name == ident) {
            if (!cin_system(c, tloc(c, tok)))
                cwarn(c, tloc(c, tok), "c++-compat", "C++ lookup of '%s' "
                      "would return a field, not a type", cident(c, ident));
            return;
        }
}

/* gcc's C_TYPE_FIELDS_VOLATILE: a struct or union with a volatile member,
 * at any depth. */
static bool fields_volatile(Checker *c, TypeId t)
{
    const Record *r;
    uint32_t k;
    while (type_ckind(TT, t) == TY_ARRAY)
        t = type_base(TT, type_canon(TT, t));
    if (type_ckind(TT, t) != TY_STRUCT && type_ckind(TT, t) != TY_UNION)
        return false;
    r = type_record(TT, type_canon(TT, t));
    for (k = 0; k < r->nfields; k++) {
        TypeId ft = c->tt.fields.data[r->fields + k].ty;
        if ((TYPE_QUALS(type_canon(TT, ft)) & TQ_VOLATILE) || fields_volatile(c, ft))
            return true;
    }
    return false;
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
    if (x.kind == CS_TYPEDEF) {
        uint32_t tb = cbound_here(c, NS_TAG, name);
        if (tb) {
            TypeId tt = c->log.data[tb - 1].ref;
            if (type_canon(TT, x.ty & ~(TypeId)TQ_MASK) != type_canon(TT, tt))
                typedef_tag_clash(c, name, x.loc,
                                  type_ckind(TT, tt) == TY_ENUM
                                  ? type_enum(TT, tt)->loc
                                  : type_record(TT, tt)->loc);
        }
    }
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
        if (x.kind == CS_TYPEDEF && csym(c, vis)->kind == CS_OBJ)
            return vis;     /* gcc keeps the variable: the name stays bound to it */
        skip = true;
    }
    ref = SYM_NONE;
    if (!skip) {
        /* a block-scope function declaration names the external one, also
         * `extern inline` (not DECL_EXTERNAL here until it is defined) */
        if (((x.flags & CSF_DECL_EXTERNAL) || filescope ||
             (x.kind == CS_FUNC && pub)) && varfn) {
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
                /* PR c/102759: a file-scope `f ()` after only block-scope
                 * declarations of f does not inherit their prototype */
                if (filescope && visref == SYM_NONE && x.kind == CS_FUNC &&
                    type_ckind(TT, newty) == TY_FUNC &&
                    (type_ent(TT, type_canon(TT, newty))->flags & TF_NOPROTO) &&
                    !is_err(c, newty))
                    csym(c, e - 1)->ty = newty;
                cbind(c, NS_ORD, name, e - 1);
                if (!filescope && !is_err(c, newty))
                    bind_this_type(c, (uint32_t)c->log.len, e - 1, vt,
                                   visref != SYM_NONE, newty);
                return e - 1;
            } else if (pub) {
                if (visref != SYM_NONE && !e && !filescope &&
                    !is_err(c, newty))
                    outer_bindings(c, name, newty);
                if (visref != SYM_NONE && !e &&
                    duplicate_decls(c, &x, nfile, visref, implicit_int)) {
                    cbind(c, NS_ORD, name, visref);
                    if (!filescope && !is_err(c, newty))
                        bind_this_type(c, (uint32_t)c->log.len, visref, vt,
                                       true, newty);
                    return visref;
                }
                if (x.kind == CS_FUNC && !e)
                    cexpr_builtin_decl(c, &x);
                else if (x.kind == CS_OBJ && !e)
                    cexpr_builtin_nonfn(c, &x);
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
        while (e != NO_NODE && cexpr_is_extension(c, e))
            e = strip_parens(c, first_child(c, e));
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

/* An attribute-only statement: c_parser_declaration_or_fndef's checks. */
static void attr_only_check(Checker *c, uint32_t i, uint32_t tok, bool top);

static void attr_stmt_visit(Checker *c, uint32_t i)
{
    uint32_t par = c->par[i];
    if (tokp(c, c->nodes[i].tok)->kind == TK_PUNCT)
        return;                 /* [[...]] */
    if (par != NO_NODE && ntag(c, par) == N_LABEL)
        return;                 /* attributes of the label (N_ATTRIBUTE case) */
    if (par != NO_NODE && (ntag(c, par) == N_CASE || ntag(c, par) == N_DEFAULT) &&
        par == i + 1)
        cpedantic(c, tloc(c, c->nodes[i].tok), "a label can only be part of a "
                  "statement and a declaration is not a statement");
    attr_only_check(c, i, c->nodes[i].tok, false);
}

/* c_parser_declaration_or_fndef for a list of attributes alone (holder's
 * N_ATTRIBUTE kids), the first at token tok. */
static void attr_only_check(Checker *c, uint32_t i, uint32_t tok, bool top)
{
    Kids k;
    uint32_t j, n, nft = 0, other[16], nother = 0;
    bool param = false;
    SrcLoc loc;
    char name[48];
    loc = cdecl_line_start_loc(c, tok);
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        Kids ak;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &ak);
        for (n = 0; n < ak.n; n++) {
            Kids args;
            if (ntag(c, ak.p[n]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[ak.p[n]].tok), name, sizeof name);
            if (!strcmp(name, "fallthrough")) {
                nft++;
                kids_get(c, ak.p[n], &args);
                param |= args.n != 0;
                kids_free(&args);
            } else if (nother < 16)
                other[nother++] = ak.p[n];
        }
        kids_free(&ak);
    }
    if (!nft)
        cpedwarn(c, loc, "", "empty declaration");
    else if (top)
        cwarn(c, loc, "attributes", "'fallthrough' attribute at top level");
    else {
        if (nft > 1)
            cwarn(c, loc, "attributes", "attribute 'fallthrough' specified "
                  "multiple times");
        if (param)
            cwarn(c, loc, "attributes", "'fallthrough' attribute specified "
                  "with a parameter");
        for (j = 0; j < nother; j++) {
            attr_norm(tstr(c, c->nodes[other[j]].tok), name, sizeof name);
            cwarn(c, loc, "attributes", "'%s' attribute ignored", name);
        }
    }
    kids_free(&k);
}

static void shadow_tag(Checker *c, Spec *sp, int warned, uint32_t ltok)
{
    bool file = cat_file_scope(c);
    bool tagged = sp->kind == TSK_TAGDEF || sp->kind == TSK_TAGFIRSTREF;
    bool anyq = sp->quals != 0;
    SrcLoc il = iloc(c, ltok);
    TypeKind tk = tkind(c, sp->ty);
    if (sp->word == TW_AUTO_TYPE) {
        cerror(c, il, "'__auto_type' in empty declaration");
        warned = 1;
    } else if (!sp->default_int && sp->kind != TSK_TYPEDEF) {
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
            /* gcc: a struct whose member was a pedwarned variably modified
             * type comes back as an error type, so nothing is declared */
            if ((c->func_sym == SYM_NONE || c->in_head) && tagged && tk != TY_ENUM && !warned) {
                const Record *r = type_record(TT, sp->ty);
                uint32_t f;
                for (f = 0; f < r->nfields; f++)
                    if (type_is_vm(TT, TT->fields.data[r->fields + f].ty)) {
                        cpedwarn(c, cdecl_line_start_loc(c, ltok), "", "empty declaration");
                        warned = 1;
                        break;
                    }
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
    if (warned == 2 && sp->default_int)   /* no type specifier at all */
        cpedwarn(c, cdecl_line_start_loc(c, ltok), "", "empty declaration");

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
static uint32_t funcdef_fnode(Checker *c, uint32_t top);

static void decl_visit(Checker *c, uint32_t i)
{
    uint32_t sn = first_child(c, i);
    if ((cnode(c, i)->flags & (NF_NESTED | NF_ERROR)) == (NF_NESTED | NF_ERROR) &&
        !in_extension(c, i))
        cpedantic(c, tloc(c, cnode(c, i)->tok), "ISO C forbids nested "
                  "functions");
    /* a declaration whose declarator failed to parse is not an empty one */
    if (sn != NO_NODE && ntag(c, sn) == N_SPECS &&
        !(cnode(c, i)->flags & NF_ERROR) &&
        c->nodes[i].size == c->nodes[sn].size + 1) {
        int si = find_spec(c, sn);
        if (si >= 0) {
            Spec sp = c->specs.data[si];
            uint32_t at = NO_NODE;
            Kids sk;
            if (sp.default_int && !sp.has_type && sp.kind == TSK_NONE &&
                sp.sc == SC_NONE && !sp.quals) {
                kids_get(c, sn, &sk);
                if (sk.n && ntag(c, sk.p[0]) == N_ATTRIBUTE &&
                    tokp(c, c->nodes[sk.p[0]].tok)->kind != TK_PUNCT)
                    at = sk.p[0];
                kids_free(&sk);
            }
            if (at != NO_NODE)
                attr_only_check(c, sn, c->nodes[at].tok, cat_file_scope(c));
            else
                shadow_tag(c, &sp, 0, sp.tok1);
        }
    }
    pop_specs(c, i);
}

/* Attributes given after the declarator. */
static void decl_attrs(Checker *c, uint32_t idecl, Attrs *a)
{
    uint32_t d = first_child(c, idecl);
    memset(a, 0, sizeof *a);
    attrs_of_children(c, idecl, a);
    /* attributes among a pointer's qualifiers belong to the declaration */
    while (d != NO_NODE && ntag(c, d) == N_PTR) {
        attrs_of_children(c, d, a);
        d = first_child(c, d);
    }
}

static uint32_t last_enumerator(Checker *c, uint32_t n)
{
    Kids k;
    uint32_t j, r = NO_NODE;
    kids_get(c, n, &k);
    for (j = 0; j < k.n; j++)
        if (ntag(c, k.p[j]) == N_ENUMERATOR)
            r = k.p[j];
    kids_free(&k);
    return r;
}

/* A declarator whose specifiers define a deprecated/unavailable tag uses the
 * type: gcc warns at the tag (anonymous struct: its '{', enum: its first
 * enumerator) once per declarator. */
static void dep_spec_use(Checker *c, const Spec *sp)
{
    uint32_t n = sp->tag_node, tag, tok;
    TypeId t;
    SrcLoc loc, note;
    uint32_t dep, dmsg;
    bool isenum;
    if (n == NO_NODE || (ntag(c, n) != N_STRUCT && ntag(c, n) != N_ENUM) ||
        !(cnode(c, n)->flags & NF_BODY))
        return;
    t = type_canon(TT, c->ty[n]);
    isenum = type_ckind(TT, t) == TY_ENUM;
    if (isenum) {
        const Enum *e = type_enum(TT, t);
        if (!e)
            return;
        dep = e->dep;
        dmsg = e->dmsg;
    } else {
        const Record *r = type_record(TT, t);
        if (!r)
            return;
        dep = r->dep;
        dmsg = r->dmsg;
    }
    if (!(dep & (CSF_DEPRECATED | CSF_UNAVAILABLE)))
        return;
    tag = find_child(c, n, N_TAG);
    for (tok = cnode(c, n)->tok; tpunct(c, tok) != P_LBRACE; tok++)
        ;
    if (tag != NO_NODE) {
        loc = note = tloc(c, cnode(c, tag)->tok);
        if (isenum) {
            uint32_t en = last_enumerator(c, n);
            if (en != NO_NODE)
                loc = tloc(c, cnode(c, en)->tok);
        }
    } else if (isenum) {
        uint32_t en = last_enumerator(c, n);
        note = tloc(c, tok);
        loc = en != NO_NODE ? tloc(c, cnode(c, en)->tok) : note;
    } else {
        loc = tloc(c, tok);
        note = tloc(c, cnode(c, n)->tok);
    }
    cdep_named(c, loc, tag != NO_NODE ? cident(c, cnode_ident(c, tag)) : NULL,
               dep, dmsg, &note);
}

/* nocf_check and transaction_unsafe belong to the function type: written on
 * the declaration (or after a '*'), they reach the first function type under
 * the pointers of ty. */
static TypeId fn_attr_type(Checker *c, TypeId ty, unsigned bits)
{
    unsigned q = TYPE_QUALS(ty);
    TypeId cn = type_canon(TT, ty);
    if (type_ckind(TT, cn) == TY_PTR && type_kind(TT, ty) != TY_TYPEDEF) {
        TypeId b = type_base(TT, ty), nb = fn_attr_type(c, b, bits);
        return nb == b ? ty : type_ptr(TT, nb) | q;
    }
    if (type_kind(TT, ty) == TY_FUNC) {
        const TypeEnt *e = type_ent(TT, ty);
        return type_func(TT, type_base(TT, ty), type_params(TT, ty),
                         (uint32_t)e->n, e->flags | bits) | q;
    }
    return ty;
}

static unsigned fn_attr_walk(Checker *c, uint32_t h, int depth)
{
    unsigned bits = 0;
    Kids hk;
    uint32_t m;
    if (depth > 16)
        return 0;
    kids_get(c, h, &hk);
    for (m = 0; m < hk.n; m++) {
        int t = ntag(c, hk.p[m]);
        if (t == N_ATTRIBUTE) {
            Kids it;
            uint32_t q;
            kids_get(c, hk.p[m], &it);
            for (q = 0; q < it.n; q++) {
                char an[32];
                if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                    continue;
                attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
                if (!strcmp(an, "nocf_check"))
                    bits |= TF_NOCF;
                else if (!strcmp(an, "transaction_unsafe"))
                    bits |= TF_TXUNSAFE;
            }
            kids_free(&it);
        } else if (t == N_PTR || t == N_FUNC || t == N_ARRAY)
            bits |= fn_attr_walk(c, hk.p[m], depth + 1);
    }
    kids_free(&hk);
    return bits;
}

/* The bits a declaration puts into its type.  nocf_check is ignored (with a
 * warning) without -fcf-protection. */
static unsigned nocf_ignored(Checker *c, unsigned bits, SrcLoc loc)
{
    if ((bits & TF_NOCF) && c->opt.cf_nobranch) {
        cwarn(c, loc, "attributes", "'nocf_check' attribute ignored. Use "
              "'-fcf-protection' option to enable it");
        bits &= ~(unsigned)TF_NOCF;
    }
    return bits;
}

static unsigned fn_attr_bits(Checker *c, uint32_t sn, uint32_t idecl, bool isfunc)
{
    unsigned bits = fn_attr_walk(c, idecl, 0);
    if (isfunc && sn != NO_NODE)
        bits |= fn_attr_walk(c, sn, 0);
    return bits;
}

static void declared_visit(Checker *c, uint32_t i)
{
    uint32_t idecl = c->par[i], decl, sn, top, name_tok, end, ltok, ref;
    int si;
    Spec sp;
    bool initialized, kr, file, incomp_init = false, fn_inv = false;
    GDecl g;
    CSym s;
    Attrs a;
    SrcLoc il;
    if (ntag(c, idecl) == N_FUNC_DEF) {
        funcdef_declared(c, i);
        return;
    }
    decl = c->par[idecl];
    c->vis_old_ok = false;
    c->redecl_failed = false;
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
    dep_spec_use(c, &sp);
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
    {
        unsigned fb = fn_attr_bits(c, sn, idecl, s.kind == CS_FUNC);
        if (fb) {
            fb = nocf_ignored(c, fb, tloc(c, c->specs.data[si].tok0));
            if (fb)
                s.ty = fn_attr_type(c, s.ty, fb);
        }
    }
    if (s.kind == CS_FUNC)
        cpragma_optimize_repeat(c, il);
    if (s.kind == CS_OBJ)
        larger_than(c, s.loc, s.name, s.ty);
    c->attr_fty = s.kind == CS_FUNC ? type_canon(TT, s.ty) : 0;
    decl_attrs(c, idecl, &a);
    c->attr_fty = 0;
    if (a.has_mode || a.vs_seen) {
        s.ty = attr_apply_type(c, s.ty, &a);
        g.ty = s.ty;
    }
    /* noreturn on a pointer to function qualifies the function type
     * (handle_noreturn_attribute) */
    if ((a.noreturn || sp.attrs.noreturn) && s.kind != CS_FUNC &&
        type_kind(TT, s.ty) == TY_PTR) {
        TypeId fn = type_base(TT, s.ty);
        if (type_ckind(TT, fn) == TY_FUNC && !(TYPE_QUALS(fn) & TQ_VOLATILE)) {
            s.ty = type_ptr(TT, fn | TQ_VOLATILE) | TYPE_QUALS(s.ty);
            g.ty = s.ty;
        }
    }
    attrs_unknown_emit(c, &sp.attrs, ltok);
    if (g.what == GD_FUNC && s.kind == CS_FUNC) {
        /* the declared type keeps the typedef names of the parameters */
        TypeId aft = type_kind(TT, s.ty) == TY_FUNC ? s.ty : type_canon(TT, s.ty);
        acc_start(s.name, s.loc);
        imp_name = alloc_name;
        if (!kr) {
            uint32_t pt = cparm_make(c, funcdef_fnode(c, top));
            imp_n = cparm_implied(c, pt, imp_l, 16);
            cparm_release(c, pt);
        }
        attrs_alloc_check(c, sn, aft, ltok);
        attrs_alloc_check(c, idecl, aft, ltok);
        alloc_name = 0;
    }
    if (g.what == GD_TYPEDEF && type_ckind(TT, s.ty) == TY_FUNC) {
        alloc_name = s.name;
        attrs_alloc_check(c, sn, s.ty, ltok);
        attrs_alloc_check(c, idecl, s.ty, ltok);
        alloc_name = 0;
    }
    if (s.kind == CS_FUNC)
        cexpr_builtin_noproto_fmt(c, &s, tloc(c, sp.tok0));
    if (s.kind == CS_OBJ || s.kind == CS_TYPEDEF) {
        unsigned ac = s.kind == CS_TYPEDEF ? AC_T : file ? AC_G :
                      s.sc == SC_STATIC ? AC_S : AC_L;
        if (sp.thread)
            ac |= AC_TLS;
        if (s.kind == CS_OBJ && s.sc != SC_STATIC && (file || s.sc == SC_EXTERN))
            ac |= AC_PUB;
        ctx_vname = sname(c, &s);
        attrs_ctx_check(c, sn, s.ty, ltok, ac);
        attrs_ctx_check(c, idecl, s.ty, ltok, ac);
    }
    if (s.kind == CS_OBJ && type_ckind(TT, s.ty) == TY_PTR &&
        type_ckind(TT, type_base(TT, s.ty)) == TY_FUNC) {
        /* a pointer to function: the attributes describe the function */
        alloc_via_ptr = true;
        attrs_alloc_check(c, sn, type_base(TT, s.ty), ltok);
        attrs_alloc_check(c, idecl, type_base(TT, s.ty), ltok);
        alloc_via_ptr = false;
    }
    if (s.kind == CS_OBJ) {
        strict_flex_check(c, sn, false, s.ty, s.name, s.loc, sp.tok0);
        strict_flex_check(c, idecl, false, s.ty, s.name, s.loc, NO_NODE);
    }
    if (s.kind == CS_OBJ && !(type_ckind(TT, s.ty) == TY_PTR &&
        type_ckind(TT, type_base(TT, s.ty)) == TY_FUNC) &&
        (attrs_item_named(c, sn, "assume_aligned") ||
         attrs_item_named(c, idecl, "assume_aligned"))) {
        cwarn(c, iloc(c, ltok), "attributes", "'assume_aligned' attribute only "
              "applies to function types");
    }
    bool ign_packed = false, ign_aligned = false;
    if (s.kind == CS_FUNC || s.kind == CS_OBJ) {
        AttrState st = {0};
        uint32_t h = idecl;
        attrs_copy_check(c, sn, s.kind, s.name, ltok, &st);
        for (;;) {       /* attributes after a '*' belong to the declaration */
            Kids hk;
            uint32_t m, nx = NO_NODE;
            kids_get(c, h, &hk);
            for (m = 0; m < hk.n && nx == NO_NODE; m++)
                if (ntag(c, hk.p[m]) == N_PTR)
                    nx = hk.p[m];
            kids_free(&hk);
            if (nx == NO_NODE)
                break;
            attrs_copy_check(c, nx, s.kind, s.name, ltok, &st);
            h = nx;
        }
        attrs_copy_check(c, idecl, s.kind, s.name, ltok, &st);
        c->last_ualign = st.calign;
        ign_packed = st.ign_packed;
        ign_aligned = st.ign_aligned;
        if (s.kind == CS_FUNC) {
            s.flags |= (st.pure ? CSF_PURE : 0) | (st.cnst ? CSF_CONSTFN : 0);
            a.nonnull |= st.nonnull;
        }
    }
    attrs_merge(&a, &sp.attrs);
    if (ign_packed)
        a.packed = false;
    if (ign_aligned)
        a.aligned = 0;
    if (a.desig && !(g.what == GD_TYPEDEF && type_ckind(TT, s.ty) == TY_STRUCT))
        cerror(c, iloc(c, ltok), "'designated_init' attribute is only valid on "
               "'struct' type");
    attrs_misapplied(c, &a, kr ? 'p' : g.what == GD_TYPEDEF ? 't' :
                     g.what == GD_FUNC ? 'f' :
                     file ? 'g' : s.sc == SC_STATIC ? 's' :
                     s.sc == SC_EXTERN ? 'g' : 'a', !file, 0, ltok);
    attrs_section_check(c, &a, kr ? 'p' : g.what == GD_TYPEDEF ? 't' :
                        g.what == GD_FUNC ? 'f' : 'g',
                        g.what == GD_VAR && !file && s.sc != SC_STATIC,
                        s.name, s.loc);
    attrs_zcur_check(c, &a, g.what == GD_FUNC && !kr, s.loc);
    attrs_wina_check(c, &a, kr ? 'p' : g.what == GD_TYPEDEF ? 't' : 'g', false,
                     s.name, s.loc);
    s.sect = a.sec;
    if (s.kind == CS_OBJ && !file && (s.flags & CSF_TREE_STATIC)) {
        TypeId et = s.ty;
        while (is_arr(c, et))
            et = type_base(TT, type_canon(TT, et));
        if (!(TYPE_QUALS(et) & TQ_CONST))
            cdecl_record_inline_static(c, s.loc, s.name, true);
    }
    if (a.cleanup && a.cleanup_arg && s.kind == CS_OBJ && !file &&
        s.sc != SC_STATIC && s.sc != SC_EXTERN && !kr) {
        /* handle_cleanup_attribute: a function taking the variable's address;
         * it counts as a use of the variable */
        uint32_t ca = a.cleanup_arg, fr = SYM_NONE;
        if (ntag(c, ca) == N_IDENT)
            fr = lookup_ord(c, cnode_ident(c, ca));
        if (ntag(c, ca) != N_IDENT)
            cerror(c, il, "cleanup argument not an identifier");
        else if (fr == SYM_NONE || csym(c, fr)->kind != CS_FUNC)
            cerror(c, il, "cleanup argument not a function");
        else {
            csym(c, fr)->flags |= CSF_USED | CSF_CUSED;
            cexpr_cleanup_call(c, fr, s.ty, s.loc, il);
        }
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;   /* never warned about */
    }
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
        s.align = a.aligned;
    if (a.unused)
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;
    if (a.used && g.what == GD_TYPEDEF)
        s.flags |= CSF_USED;         /* TREE_USED: not an unused local typedef */
    if (a.deprecated || a.unavailable) {
        s.flags |= a.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
        s.dep_msg = a.dep_msg;
    }
    if (a.e_wi)
        cerror(c, s.loc, "weak '%s' cannot be defined 'ifunc'", sname(c, &s));
    if (a.e_iw)
        cerror(c, s.loc, "indirect function '%s' cannot be declared weak",
               sname(c, &s));
    if (a.weakref && (s.kind == CS_OBJ || s.kind == CS_FUNC) && sym_public(&s))
        cerror(c, s.loc, "'weakref' symbol '%s' must have static linkage",
               sname(c, &s));
    if (a.weak && !kr && (s.kind == CS_OBJ || s.kind == CS_FUNC))
        weak_apply(c, &s, sp.is_inline);
    if (a.weakref && (s.kind == CS_OBJ || s.kind == CS_FUNC))
        s.flags |= CSF_WEAK | CSF_WEAKREF;   /* a weakref is weak too */
    /* handle_alias_ifunc_attributes: a function alias has an initial value, so
     * a later definition (or an earlier one) is a redefinition */
    if (a.defn && s.kind == CS_FUNC && g.what == GD_FUNC)
        s.flags |= CSF_DEFINED;
    if ((a.noreturn || (type_ckind(TT, s.ty) == TY_FUNC &&
                        (TYPE_QUALS(type_canon(TT, s.ty)) & TQ_VOLATILE))) &&
        s.kind == CS_FUNC)           /* a volatile function is noreturn */
        s.flags |= CSF_NORETURN;
    if (a.nonnull && (s.kind == CS_FUNC || (s.kind == CS_OBJ && file)))
        s.nonnull = a.nonnull;     /* an object: a function pointer */
    if (a.fmt && s.kind == CS_FUNC)
        s.fmt = a.fmt;
    if (a.fmtarg && s.kind == CS_FUNC)
        s.fmtarg = a.fmtarg;
    if (g.what == GD_FUNC && s.kind == CS_FUNC && !kr)
        s.parms = cparm_make(c, funcdef_fnode(c, top));
    if (g.what == GD_VAR && s.sc == SC_REGISTER &&
        find_child(c, idecl, N_ASM_LABEL) != NO_NODE) {
        if (file)
            s.flags |= CSF_REGISTER_NAMED;
        if (fields_volatile(c, s.ty))
            cerror(c, tloc(c, sp.tok1 - 1), "cannot put object with volatile field into "
                   "register");
    }
    if (g.what == GD_VAR && g.name &&
        !strcmp(cident(c, g.name), "main") && sym_public(&s))
        cwarn(c, s.loc, "main", "'main' is usually a function");
    if (initialized) {
        switch (g.what) {
        case GD_TYPEDEF:
            cerror(c, il, "typedef '%s' is initialized (use '__typeof__' "
                   "instead)", cident(c, g.name));
            initialized = false;
            incomp_init = true;   /* the initializer is still parsed */
            break;
        case GD_FUNC:
            cerror(c, il, "function '%s' is initialized like a variable",
                   cident(c, g.name));
            {   /* a redeclaration of a defined function: its initial value
                 * is already set, and the initializer is invalid too
                 * (after the merge's own diagnostics) */
                uint32_t b = cbound_here(c, NS_ORD, g.name);
                fn_inv = b && sym_defined(csym(c, c->log.data[b - 1].ref));
            }
            initialized = false;
            incomp_init = true;   /* the initializer is still parsed */
            break;
        case GD_PARM:
            cerror(c, il, "parameter '%s' is initialized", cident(c, g.name));
            initialized = false;
            incomp_init = true;   /* the initializer is still parsed */
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
        if (a.sso == 1 && is_rec(c, type_canon(TT, s.ty)))
            s.ty = type_clone_record(TT, s.ty);   /* a distinct variant */
        ref = pushdecl(c, &s, false);
        if (a.may_alias && type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF)
            TT->ents.data[TYPE_IDX(csym(c, ref)->ty)].flags |= TF_MAYALIAS;
        if ((a.wina_al || sp.attrs.wina_al) &&
            type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF) {
            WinaEnt we = {TYPE_IDX(csym(c, ref)->ty),
                          a.wina_al > sp.attrs.wina_al ? a.wina_al
                                                       : sp.attrs.wina_al};
            vec_push(&c->wina_td, we);
        }
        /* a redeclaration can only raise the alignment */
        if (s.align && type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF) {
            TypeEnt *te = &TT->ents.data[TYPE_IDX(csym(c, ref)->ty)];
            uint16_t enc = (uint16_t)(__builtin_ctz(s.align) + 1);
            if (TT->ents.len > nents || enc > te->align) {
                te->align = enc;
                te->flags |= TF_ALIGNED;
            }
        }
    } else {
        if (g.what == GD_FUNC) {
            inline_given(c, &s, sp.is_inline, sn, idecl);
            inline_follows(c, &s, ltok, sn, idecl);
        }
        ref = pushdecl(c, &s, false);
    }
    {
        CSym *t = csym(c, ref);
        if (initialized && (t->flags & CSF_DECL_EXTERNAL)) {
            t->flags &= ~(unsigned)CSF_DECL_EXTERNAL;
            t->flags |= CSF_TREE_STATIC;
        }
        c->ty[i] = t->ty;
    }
    if (a.nonnull && !file && csym(c, ref)->kind == CS_OBJ && s.name &&
        c->top[NS_ORD][s.name])
        c->log.data[c->top[NS_ORD][s.name] - 1].nn = a.nonnull;
    if (csym(c, ref)->kind == CS_TYPEDEF)
        cparm_typedef(c, top, csym(c, ref)->ty);
    if (g.what == GD_FUNC && csym(c, ref)->kind == CS_FUNC)
        acc_implied(c, ref, csym(c, ref)->parms != s.parms, false);
    if (fn_inv && c->vis_old_ok)
        cerror(c, tloc(c, ltok), "invalid initializer");
    {   /* merge_decls: a different explicit visibility is not applied */
        char was[32], now[32];
        uint32_t tmp = 0;
        bool had = cdecl_aset_first_arg(c, csym(c, ref)->aset, "visibility",
                                        was, sizeof was);
        if (had) {
            attrs_names(c, sn, &tmp);
            attrs_names(c, idecl, &tmp);
        }
        if (had && (csym(c, ref)->kind == CS_OBJ ||
                    csym(c, ref)->kind == CS_FUNC) &&
            cdecl_aset_first_arg(c, tmp, "visibility", now, sizeof now) &&
            strcmp(was, now)) {
            Diagnostic *vd = cwarn_d(c, DL_WARNING, s.loc, "", "redeclaration "
                                     "of '%s' with different visibility (old "
                                     "visibility preserved)", sname(c, &s));
            locate_old_decl(c, vd, c->vis_old_ok ? &c->vis_old : csym(c, ref));
        }
        c->vis_old_ok = false;
    }
    attrs_names(c, sn, &csym(c, ref)->aset);
    attrs_names_ptrs(c, idecl, &csym(c, ref)->aset);
    attrs_names(c, idecl, &csym(c, ref)->aset);
    if (g.what == GD_FUNC && csym(c, ref)->kind == CS_FUNC)
        acc_chain_implied(c, ref);
    if (a.packed)               /* ignored (attrs_misapplied), so not kept */
        aset_drop(c, csym(c, ref)->aset, "packed");
    if (csym(c, ref)->kind == CS_TYPEDEF && csym(c, ref)->aset &&
        type_kind(TT, csym(c, ref)->ty) == TY_TYPEDEF) {
        uint32_t p[2] = {TYPE_IDX(csym(c, ref)->ty), csym(c, ref)->aset};
        aset_keep_max_aligned(c, p[1]);
        vec_push(&c->tdas, p[0]);
        vec_push(&c->tdas, p[1]);
    }
    if (c->last_ualign > csym(c, ref)->ualign)
        csym(c, ref)->ualign = c->last_ualign;
    c->last_ualign = 0;
    c->nign = 0;
    if (sp.is_noreturn)
        aset_add(c, &csym(c, ref)->aset, "noreturn", "");
    if (csym(c, ref)->kind == CS_FUNC && csym(c, ref)->linkage == LK_INTERNAL) {
        /* leaf only means something for external functions */
        if (attrs_item_named(c, sn, "leaf") || attrs_item_named(c, idecl, "leaf"))
            cwarn(c, iloc(c, ltok), "attributes", "'leaf' attribute has no "
                  "effect on unit local functions");
        aset_drop(c, csym(c, ref)->aset, "leaf");
    }
    c->cb[i] = ref + 1;
    c->cv[i] = initialized ? 1 : incomp_init ? 2 : 0;
    if (g.what == GD_FUNC)
        cexpr_record_params(c, i, ref, false);
    if (g.what == GD_VAR || g.what == GD_FUNC)
        ensure_finish_cue(c, ref);
}

/* The token after the last struct/union keyword in [first, stop) (the tag,
 * or the '{' of an anonymous one); 0 if none. */
uint32_t ctrad_tag(Checker *c, uint32_t first, uint32_t stop)
{
    uint32_t k, tag = 0;
    for (k = first; k < stop; k++) {
        int kw = tckw(c, k);
        if (kw == CK_STRUCT || kw == CK_UNION)
            tag = k + 1;
    }
    return tag;
}

SrcLoc ctrad_loc(Checker *c, uint32_t tag, uint32_t init_tok)
{
    if (tag && cinput_loc(c, tag) == cinput_loc(c, init_tok))
        return ctok_loc(c, tag);
    return cinput_loc(c, init_tok);
}

static uint32_t decl_tag(Checker *c, uint32_t declared)
{
    uint32_t d = c->par[declared], stop = cnode(c, declared)->tok;
    while (d != NO_NODE && ntag(c, d) != N_DECL)
        d = c->par[d];
    return ctrad_tag(c, d == NO_NODE ? stop : first_tok(c, d), stop);
}

SrcLoc ctrad_decl_loc(Checker *c, uint32_t declared, uint32_t init_tok)
{
    return ctrad_loc(c, decl_tag(c, declared), init_tok);
}

/* Where gcc is when it finishes the initializer: at the declaration's last
 * struct/union tag, 0 if it has none. */
SrcLoc cdecl_tag_loc(Checker *c, uint32_t declared)
{
    uint32_t tag = decl_tag(c, declared);
    return tag ? ctok_loc(c, tag) : 0;
}

/* -Wtraditional: an automatic aggregate with an initializer (start_init). */
static void trad_aggr_init(Checker *c, uint32_t idecl, uint32_t declared,
                           uint32_t init)
{
    CSym *s;
    TypeKind k;
    SrcLoc loc;
    if (!diag_enabled(c->diag, "traditional") || init == NO_NODE ||
        !c->cb[declared])
        return;
    s = csym(c, c->cb[declared] - 1);
    if (s->kind != CS_OBJ || cat_file_scope(c) || (s->flags & CSF_TREE_STATIC))
        return;
    k = type_ckind(TT, s->ty);
    if (k != TY_STRUCT && k != TY_UNION && k != TY_ARRAY && k != TY_VLA)
        return;
    loc = ctrad_decl_loc(c, declared, first_tok(c, init));
    if (!cin_system(c, loc))
        cwarn(c, loc, "traditional", "traditional C rejects automatic "
              "aggregate initialization");
    (void)idecl;
}

/* -Wc++-compat: gcc's diagnose_uninitialized_cst_member, one warning and note
 * per const member found (nested records included). */
static void cxx_uninit_members(Checker *c, SrcLoc loc, TypeId top, TypeId t)
{
    const Record *r;
    uint32_t k;
    TypeKind tk;
    while (is_arr(c, t))
        t = type_base(TT, t);
    tk = type_ckind(TT, t);
    if (tk != TY_STRUCT && tk != TY_UNION)
        return;
    r = type_record(TT, type_canon(TT, t));
    for (k = 0; k < r->nfields; k++) {
        const Field *fl = &TT->fields.data[r->fields + k];
        TypeId ft = fl->ty;
        while (is_arr(c, ft))
            ft = type_base(TT, type_canon(TT, ft));
        if (TYPE_QUALS(ft) & TQ_CONST) {
            Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "c++-compat",
                                    "uninitialized const member in %s is "
                                    "invalid in C++", type_q(TT, top));
            if (d && fl->name)
                cnote(c, d, fl->loc, "'%s' should be initialized",
                      cident(c, fl->name));
        } else
            cxx_uninit_members(c, loc, top, ft);
    }
}

/* -Wc++-compat: a file-scope definition whose type is an unnamed struct,
 * union or enum (not one a typedef names). */
static void cxx_anon_type(Checker *c, const CSym *s)
{
    const TypeEnt *e;
    if (s->kind != CS_OBJ || !cat_file_scope(c) || s->sc == SC_STATIC ||
        s->sc == SC_EXTERN || !diag_enabled(c->diag, "c++-compat") ||
        cin_system(c, s->loc))
        return;
    e = type_ent(TT, s->ty & ~(TypeId)TQ_MASK);
    if (e->kind == TY_STRUCT || e->kind == TY_UNION) {
        if (type_record(TT, type_canon(TT, s->ty))->tag)
            return;
    } else if (e->kind == TY_ENUM) {
        if (type_enum(TT, type_canon(TT, s->ty))->tag)
            return;
    } else
        return;
    cwarn(c, s->loc, "c++-compat", "non-local variable '%s' with anonymous "
          "type is questionable in C++", sname(c, s));
}

/* -Wc++-compat: a const object, or one with const members, without initializer. */
static void cxx_uninit_const(Checker *c, const CSym *s)
{
    TypeId t = s->ty;
    if (s->kind != CS_OBJ || s->sc == SC_EXTERN || (s->flags & CSF_PARAM) ||
        !diag_enabled(c->diag, "c++-compat") || cin_system(c, s->loc))
        return;
    while (is_arr(c, t))
        t = type_base(TT, t);
    if (TYPE_QUALS(t) & TQ_CONST)
        cwarn(c, s->loc, "c++-compat", "uninitialized 'const %s' is invalid "
              "in C++", sname(c, s));
    else
        cxx_uninit_members(c, s->loc, t, t);
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
    trad_aggr_init(c, idecl, declared, init);
    cxx_anon_type(c, s);
    if (init == NO_NODE)
        cxx_uninit_const(c, s);
    type = s->ty;
    if ((s->flags & CSF_AUTO_TYPE) && init != NO_NODE) {
        /* __auto_type: the initializer's type after lvalue conversion */
        TypeId it = cexpr_rvalue_type(c, init);
        if ((c->ef[init] & EF_BITFIELD) && !is_err(c, it)) {
            uint32_t d = c->par[idecl];
            while (d != NO_NODE && ntag(c, d) != N_DECL)
                d = c->par[d];
            cerror(c, d == NO_NODE ? s->loc : tloc(c, cnode(c, d)->tok),
                   "'__auto_type' used with a bit-field initializer");
        } else if (!is_err(c, it) && type_ckind(TT, it) != TY_ERROR) {
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
                /* a conflicting redeclaration left the old declaration */
                if (!c->redecl_failed)
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

uint32_t find_child(Checker *c, uint32_t i, unsigned tag)
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

/* -Wc++-compat: a typedef and a tag of the same name in one scope that are
 * not the same type; the earlier one is at old. */
static void typedef_tag_clash(Checker *c, uint32_t name, SrcLoc at, SrcLoc old)
{
    Diagnostic *d;
    if (cin_system(c, at))
        return;
    d = cwarn_d(c, DL_WARNING, at, "c++-compat", "using '%s' as both a typedef "
                "and a tag is invalid in C++", cident(c, name));
    if (d && old)
        cnote(c, d, old, "originally defined here");
}

/* pushtag of a new forward reference. */
static TypeId new_tag(Checker *c, int want, uint32_t name, SrcLoc loc)
{
    TypeId t = want == TY_ENUM
        ? type_new_enum(TT, name, loc)
        : type_new_record(TT, name, want == TY_UNION, loc);
    if (name) {
        uint32_t ob = cbound_here(c, NS_ORD, name);
        if (ob) {
            const CSym *o = csym(c, c->log.data[ob - 1].ref);
            if (o->kind == CS_TYPEDEF &&
                type_canon(TT, o->ty & ~(TypeId)TQ_MASK) != type_canon(TT, t))
                typedef_tag_clash(c, name, loc, o->loc);
        }
    }
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
        if (!c->recs.data[k].cleared &&
            type_canon(TT, c->recs.data[k].ty) == type_canon(TT, t))
            return true;
    return false;
}

static void tag_visit(Checker *c, uint32_t i)
{
    iloc_event(c, cnode(c, i)->tok);
}

/* c_cast_expr / the compound literal: the type name itself defines the tag
 * (ctsk_tagdef, or names it for the first time). */
static void cxx_defining_cast(Checker *c, uint32_t st, bool lit_only)
{
    if (cexpr_cxx_compat(c, st)) {
        uint32_t a = c->par[st], prev = st;
        for (; a != NO_NODE; prev = a, a = c->par[a]) {
            unsigned tg = ntag(c, a);
            if (tg == N_CAST || tg == N_COMPOUND_LIT) {
                if (first_child(c, a) == prev &&
                    (!lit_only || tg == N_COMPOUND_LIT))
                    cwarn(c, tloc(c, cnode(c, a)->tok), "c++-compat",
                          "defining a type in a %s is invalid in C++",
                          tg == N_CAST ? "cast" : "compound literal");
                break;
            }
            if (tg == N_STRUCT || tg == N_ENUM || tg == N_COMPOUND ||
                tg == N_DECL || tg == N_FUNC_DEF)
                break;
        }
    }
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
        if (being_defined(c, t)) {
            /* gcc diagnoses it in start_enum, before any later syntax error
             * in the unit (an empty inner enum), then clears the flag */
            bool q = c->quiet;
            size_t k;
            c->quiet = false;
            cerror(c, loc, "nested redefinition of 'enum %s'",
                   name ? cident(c, name) : "");
            c->quiet = q;
            for (k = 0; k < c->recs.len; k++)
                if (type_canon(TT, c->recs.data[k].ty) == type_canon(TT, t))
                    c->recs.data[k].cleared = true;
        }
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
    if (cexpr_cxx_compat(c, st)) {
        /* in_sizeof / in_typeof / in_alignof: the parser is inside one */
        bool in_sz = false, in_ty = false, in_al = false;
        uint32_t a;
        for (a = c->par[st]; a != NO_NODE; a = c->par[a])
            switch (ntag(c, a)) {
            case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: in_sz = true; break;
            case N_TYPEOF: case N_HAS_ATTR: in_ty = true; break;
            case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE: in_al = true; break;
            default: break;
            }
        if (in_sz || in_ty || in_al)
            cwarn(c, loc, "c++-compat", "defining type in '%s' expression is "
                  "invalid in C++", in_sz ? "sizeof" : in_ty ? "typeof"
                                                              : "alignof");
    }
    cxx_defining_cast(c, st, false);
    if (c->recs.len && !vec_last(&c->recs).is_enum &&
        diag_enabled(c->diag, "c++-compat")) {
        if (want == TY_ENUM)
            type_enum(TT, t)->in_struct = true;
        else
            type_record(TT, t)->flags |= RF_IN_STRUCT;
    }
    rd.ty = t;
    rd.first = (uint32_t)c->fields.len;
    rd.first_td = (uint32_t)c->tdseen.len;
    vec_push(&c->recs, rd);
    c->ty[i] = t;
    if (created && cscope_kind(c) == SCK_PROTO)
        c->ef[i] |= 2;
}

/* -Wc++-compat: a type or enumerator defined inside a struct is used outside. */
void cxx_in_struct_use(Checker *c, SrcLoc at, const char *what,
                       const char *noted, SrcLoc def)
{
    Diagnostic *d;
    if (cin_system(c, at))
        return;
    d = cwarn_d(c, DL_WARNING, at, "c++-compat", "%s defined in struct or "
                "union is not visible in C++", what);
    if (d && def)
        cnote(c, d, def, "%s defined here", noted);
}

/* parser_xref_tag: 'struct S' without a body. */
static void xref_visit(Checker *c, uint32_t i, int want)
{
    uint32_t tagn = find_child(c, i, N_TAG), name;
    SrcLoc loc, xloc = 0;
    TypeId t, ref;
    unsigned kind = TSK_TAGREF;
    bool wrong = false;
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
        wrong = true;
    }
    if (ref) {
        t = ref;
        /* straight from the table: a name-only use is not a layout read */
        uint32_t x = type_ent(TT, type_canon(TT, t))->extra;
        if (want == TY_ENUM) {
            const Enum *e = &TT->enums.data[x];
            cdep_report(c, loc, name, e->dep, e->dmsg, &e->loc);
            if (e->in_struct && !c->recs.len)
                cxx_in_struct_use(c, loc, "enum type", "enum type", e->loc);
        } else {
            const Record *r = &TT->recs.data[x];
            cdep_report(c, loc, name, r->dep, r->dmsg, &r->loc);
            if ((r->flags & RF_IN_STRUCT) && !c->recs.len)
                cxx_in_struct_use(c, loc, tag_kw(want), tag_kw(want), r->loc);
        }
    } else {
        t = new_tag(c, want, name, loc);
        /* a tag of the wrong kind is a reference, not a first one */
        kind = wrong ? TSK_TAGREF : TSK_TAGFIRSTREF;
        cxx_defining_cast(c, i, true);
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

bool is_rec(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_STRUCT || tkind(c, t) == TY_UNION;
}

/* -Wpacked at gcc's input_location when finish_struct runs: the last
 * struct/union/enum tag (or the record's own tag or '{') lexed, unless a
 * later token, up to the one after the attributes, starts a line. */
static SrcLoc finish_loc(Checker *c, uint32_t open, uint32_t close_tok)
{
    uint32_t k, tag, peek = close_tok + 1, last;
    int depth;
    for (open = cnode(c, open)->tok; open < close_tok; open++)
        if (tpunct(c, open) == P_LBRACE)
            break;
    tag = open;
    if (open > 0 && c->u->toks[open - 1].t.kind == TK_IDENT &&
        tckw(c, open - 1) == CK_NONE)
        tag = open - 1;
    for (k = open + 1; k < close_tok; k++) {
        int kw = tckw(c, k);
        if ((kw == CK_STRUCT || kw == CK_UNION || kw == CK_ENUM) &&
            k + 1 < close_tok)
            tag = k + 1;
    }
    /* trailing attributes */
    while (peek < c->u->ntoks && tckw(c, peek) == CK_ATTRIBUTE) {
        peek++;
        for (depth = 0; peek < c->u->ntoks; peek++) {
            int p = tpunct(c, peek);
            if (p == P_LPAREN)
                depth++;
            else if (p == P_RPAREN && --depth == 0) {
                peek++;
                break;
            }
        }
    }
    last = tag;
    for (k = tag + 1; k <= peek && k < c->u->ntoks; k++)
        if (c->u->toks[k].t.flags & TF_BOL)
            last = 0;
    return last ? tloc(c, tag) : cinput_loc(c, peek);
}

static void packed_unnecessary(Checker *c, TypeId t, uint32_t open,
                               uint32_t close_tok)
{
    const Record *r = type_record(TT, t);
    SrcLoc loc = finish_loc(c, open, close_tok);
    if (r->tag)
        cwarn(c, loc, "packed", "packed attribute is unnecessary for '%s'",
              cident(c, r->tag));
    else
        cwarn(c, loc, "packed", "packed attribute is unnecessary");
}

/* place_field and finalize_record_size: -Wpadded.  A field placed past the
 * end of the one before it, and the size rounded up to the alignment. */
static void padded_check(Checker *c, TypeId t, bool is_union, uint32_t open,
                         uint32_t close_tok)
{
    const Record *r = type_record(TT, t);
    uint64_t pos = 0, end = 0;
    uint32_t k;
    for (k = 0; k < r->nfields; k++) {
        const Field *fl = &TT->fields.data[r->fields + k];
        bool bf = (fl->flags & FF_BITFIELD) != 0;
        uint64_t bits = bf ? fl->width : type_size(TT, fl->ty, NULL) * 8;
        if (!is_union && !bf && fl->off_bits > pos)
            cwarn(c, fl->loc, "padded", "padding struct to align '%s'",
                  fl->name ? cident(c, fl->name) : "({anonymous})");
        if (is_union ? bits > end : fl->off_bits + bits > end)
            end = is_union ? bits : fl->off_bits + bits;
        pos = fl->off_bits + bits;
    }
    end = (end + 7) / 8;
    if (r->size > end)
        cwarn(c, finish_loc(c, open, close_tok), "padded", "padding struct "
              "size to alignment boundary with %llu bytes",
              (unsigned long long)(r->size - end));
}

/* TYPE_WARN_IF_NOT_ALIGN: the warn_if_not_aligned of a typedef (or one it
 * names), of a record, or of an array's element. */
static unsigned type_wina(Checker *c, TypeId t)
{
    for (;;) {
        const TypeEnt *e = type_ent(TT, t);
        size_t k;
        if (e->kind == TY_TYPEDEF) {
            for (k = c->wina_td.len; k-- > 0;)
                if (c->wina_td.data[k].key == TYPE_IDX(t))
                    return c->wina_td.data[k].wina;
            t = e->base;
        } else if (e->kind == TY_ARRAY) {
            t = e->base;
        } else if (e->kind == TY_STRUCT || e->kind == TY_UNION) {
            for (k = c->wina_rec.len; k-- > 0;)
                if (c->wina_rec.data[k].key == e->extra)
                    return c->wina_rec.data[k].wina;
            return 0;
        } else
            return 0;
    }
}

/* Does the type carry an aligned attribute (TYPE_ATTRIBUTES)? */
static bool type_user_aligned(Checker *c, TypeId t)
{
    for (;;) {
        const TypeEnt *e = type_ent(TT, t);
        (void)c;
        if (e->kind == TY_TYPEDEF) {
            if (e->flags & TF_ALIGNED)
                return true;
            t = e->base;
        } else if (e->kind == TY_STRUCT || e->kind == TY_UNION)
            return (TT->recs.data[e->extra].flags & RF_USER_ALIGN) != 0;
        else
            return false;
    }
}

/* place_field and finalize_record_size: -Wif-not-aligned and
 * -Wpacked-not-aligned. */
static void wina_check(Checker *c, TypeId t, const FieldIn *f, uint32_t m,
                       unsigned attr_wina, SrcLoc end)
{
    Record *r = type_record(TT, t);
    const Field *fl;
    unsigned rw = attr_wina, rp = 0;
    uint32_t k;
    bool on_w = diag_enabled(c->diag, "if-not-aligned"),
         on_p = diag_enabled(c->diag, "packed-not-aligned");
    if (!r || m != r->nfields)
        return;
    fl = TT->fields.data + r->fields;
    for (k = 0; k < m; k++) {
        unsigned w, p = 0;
        if (f[k].width >= 0)
            continue;
        w = f[k].wina > type_wina(c, f[k].ty) ? f[k].wina
                                              : type_wina(c, f[k].ty);
        if (!w && type_user_aligned(c, f[k].ty))
            p = type_align(TT, f[k].ty);
        if (w > rw)
            rw = w;
        if (p > rp)
            rp = p;
    }
    if (rw) {
        WinaEnt we = {(uint32_t)(r - TT->recs.data), (uint16_t)rw};
        vec_push(&c->wina_rec, we);
    }
    if (rw && !on_w)
        rw = 0;
    if (rp && !on_p)
        rp = 0;
    if (rw ? r->align < rw : rp && r->align < rp) {
        unsigned need = rw ? rw : rp;
        cwarn(c, end, rw ? "if-not-aligned" : "packed-not-aligned",
              "alignment %u of '%s %s' is less than %u", r->align,
              r->flags & RF_UNION ? "union" : "struct",
              r->tag ? cident(c, r->tag) : "", need);
    }
    for (k = 0; k < m; k++) {
        unsigned w, p = 0, v;
        if (f[k].width >= 0 || !f[k].name)
            continue;
        w = f[k].wina > type_wina(c, f[k].ty) ? f[k].wina
                                              : type_wina(c, f[k].ty);
        if (!w && type_user_aligned(c, f[k].ty))
            p = type_align(TT, f[k].ty);
        v = w ? w : p;
        if (!v || !(w ? on_w : on_p))
            continue;
        if ((fl[k].off_bits / 8) % v)
            cwarn(c, f[k].loc, w ? "if-not-aligned" : "packed-not-aligned",
                  "'%s' offset %llu in '%s %s' isn't aligned to %u",
                  cident(c, f[k].name),
                  (unsigned long long)(fl[k].off_bits / 8),
                  r->flags & RF_UNION ? "union" : "struct",
                  r->tag ? cident(c, r->tag) : "", v);
    }
}

static void struct_finish(Checker *c, uint32_t i, uint32_t open, int want)
{
    TypeId t = c->ty[open];
    RecDef rd;
    FieldIn *f;
    uint32_t n, k, m = 0, close_tok;
    Attrs a;
    SrcLoc loc;
    bool named = false, saw_named = false;
    int keep_err = -1;
    uint32_t *seen = NULL;
    size_t ns = 0, cap = 0;
    Record *r;
    int depth = 0;
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
    memset(&a, 0, sizeof a);
    {
        Attrs ma;
        memset(&ma, 0, sizeof ma);
        c->attr_quiet = true;
        attrs_of_children(c, i, &ma);
        c->attr_quiet = false;
        if (ma.has_mode)
            cerror(c, tloc(c, close_tok), "mode '%s' applied to inappropriate "
                   "type", ma.mode_name);
    }
    c->attr_at = loc;               /* gcc reports a tag attribute at the tag name */
    c->attr_at_set = true;
    attrs_of_children(c, i, &a);
    c->attr_at_set = false;
    if (a.noinline)
        cwarn(c, iloc(c, close_tok), "attributes",
              "'noinline' attribute does not apply to types");
    if (a.used)
        cwarn(c, iloc(c, close_tok), "attributes",
              "'used' attribute does not apply to types");
    if (a.nonstring)
        cwarn(c, iloc(c, close_tok), "attributes",
              "'nonstring' attribute does not apply to types");
    if (a.desig && want == TY_UNION)
        cerror(c, iloc(c, close_tok), "'designated_init' attribute is only "
               "valid on 'struct' type");
    n = (uint32_t)c->fields.len - rd.first;
    f = c->fields.data + rd.first;
    if (c->opt.pedantic) {
        for (k = 0; k < n; k++)
            if (f[k].name)
                named = true;
        if (!named) {
            DiagOrd o0 = diag_ord(c->diag, n > 0 ? ORD_LATE : ORD_NORMAL);
            /* finish_struct: after a missing semicolon */
            if (want == TY_UNION)
                cpedantic(c, loc, n ? "union has no named members"
                                    : "union has no members");
            else
                cpedantic(c, loc, n ? "struct has no named members"
                                    : "struct has no members");
            diag_ord(c->diag, o0);
        }
    }
    for (k = 0; k < n; k++) {
        size_t q;
        for (q = rd.first_td; q < c->tdseen.len && f[k].name; q++)
            if (c->tdseen.data[q] == f[k].name) {
                if (!cin_system(c, f[k].loc))
                    cwarn(c, f[k].loc, "c++-compat", "using '%s' as both "
                          "field and typedef name is invalid in C++",
                          cident(c, f[k].name));
                break;
            }
    }
    c->tdseen.len = rd.first_td;
    if (n == 0 && !cin_system(c, loc))
        cwarn(c, loc, "c++-compat", "empty %s has size 0 in C, size 1 in C++",
              want == TY_UNION ? "union" : "struct");
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
                /* the member stays, erroneous (uses are silent) */
                f[k].ty = ERRT;
                keep_err = k;
            } else if (!(type_ent(TT, type_canon(TT, f[k].ty))->flags &
                         TF_FLEX)) {
                /* finish_struct gives a typedef'd `T[]` member its domain too */
                TypeId ct = type_canon(TT, f[k].ty);
                f[k].ty = type_array_flex(TT, type_base(TT, ct)) |
                          TYPE_QUALS(f[k].ty);
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
        if (!is_err(c, f[k].ty) || (int)k == keep_err)
            f[m++] = f[k];
    csum_read_pack(c);
    type_complete_record(TT, t, f, m, c->pack, a.aligned, a.packed,
                         a.ms);
    if (type_record(TT, t)->size > (uint64_t)INT64_MAX)
        cerror(c, loc, "type %s is too large", type_q(TT, t));
    /* gcc reports at the closing brace when it starts its line, else at the tag */
    wina_check(c, t, f, m, a.wina_al,
               cbol_tok(c, close_tok) == close_tok + 1 ? cinput_loc(c, close_tok)
                                                       : loc);
    if (diag_enabled(c->diag, "padded"))
        padded_check(c, t, want == TY_UNION, open, close_tok);
    if (a.packed && want != TY_UNION && diag_enabled(c->diag, "packed") &&
        type_packed_unnecessary(TT, t, f, m, c->pack, a.aligned, a.ms, -1))
        packed_unnecessary(c, t, open, close_tok);
    if (!a.packed && want != TY_UNION && diag_enabled(c->diag, "packed"))
        for (k = 0; k < m; k++)
            if (f[k].packed && !f[k].align && f[k].name &&
                type_align(TT, f[k].ty) != 1 &&
                type_packed_unnecessary(TT, t, f, m, c->pack, a.aligned, a.ms,
                                        (long)k))
                cwarn(c, f[k].loc, "attributes", "packed attribute is "
                      "unnecessary for '%s'", cident(c, f[k].name));
    if (want == TY_UNION)       /* a member of the other storage order */
        for (k = 0; k < m; k++) {
            TypeId ft = type_canon(TT, f[k].ty);
            while (tkind(c, ft) == TY_ARRAY || tkind(c, ft) == TY_VLA)
                ft = type_canon(TT, type_base(TT, ft));
            if (is_rec(c, ft) &&
                ((type_record(TT, ft)->flags & RF_SSO) != 0) != (a.sso == 1))
                cwarn(c, f[k].loc, "scalar-storage-order", "type punning "
                      "toggles scalar storage order");
        }
    if (a.desig && want != TY_UNION)
        type_record(TT, t)->flags |= RF_DESIGNATED;
    if (a.sso == 1)     /* the target is little-endian */
        type_record(TT, t)->flags |= RF_SSO;
    r = type_record(TT, t);
    {
        uint32_t as = r->aset;
        attrs_names(c, i, &as);
        r->aset = as;
    }
    r->dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    r->dmsg = a.dep_msg;
    if (a.may_alias)
        r->flags |= RF_MAYALIAS;
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

/* Attributes written on an enumerator (a CONST_DECL): decl_attributes finds
 * a handler that does not take it.  Probed on gcc 13. */
static void enumerator_attrs(Checker *c, uint32_t i, SrcLoc loc, uint32_t name,
                             uint32_t ref)
{
    static const char *const ignored[] = {
        "alias", "always_inline", "artificial", "assume", "cleanup", "cold",
        "common", "const", "constructor", "destructor", "error",
        "externally_visible", "fallthrough", "fentry_name", "fentry_section",
        "flatten", "gcc_struct", "gnu_inline", "hot", "ifunc", "mode",
        "ms_struct", "no_address_safety_analysis", "no_icf",
        "no_profile_instrument_function", "no_sanitize",
        "no_sanitize_address", "no_sanitize_coverage", "no_sanitize_thread",
        "no_sanitize_undefined", "no_stack_protector", "noclone", "nocommon",
        "nodirect_extern_access", "noinline", "noipa", "nonstring",
        "noreturn", "nothrow", "optimize", "packed", "pure", "retain",
        "returns_twice", "scalar_storage_order", "signed_bool_precision",
        "simd", "stack_protect", "target", "target_clones",
        "transaction_callable", "transaction_may_cancel_outer",
        "transaction_pure", "transaction_safe", "transaction_safe_dynamic",
        "transaction_unsafe", "transaction_wrap", "transparent_union", "used",
        "vector_mask", "visibility", "volatile", "warn_unused", "warning",
        "weak", "weakref"};
    static const char *const fnonly[] = {
        "access", "alloc_align", "alloc_size", "assume_aligned",
        "callee_pop_aggregate_return", "cdecl", "fastcall", "fd_arg",
        "fd_arg_read", "fd_arg_write", "force_align_arg_pointer",
        "format_arg", "indirect_return", "interrupt", "ms_abi",
        "no_caller_saved_registers", "nocf_check", "nonnull", "regparm",
        "returns_nonnull", "sentinel", "sseregparm", "stdcall", "sysv_abi",
        "thiscall", "warn_unused_result"};
    Kids k;
    uint32_t j;
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[48];
            size_t f;
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (!strcmp(an, "gnu") || attr_scope_of(c, c->nodes[it.p[q]].tok))
                continue;
            if (!strcmp(an, "unused") && ref != SYM_NONE)
                csym(c, ref)->flags |= CSF_ATTR_UNUSED;
            if (!attr_known(an) || !strcmp(an, "maybe_unused") ||
                !strcmp(an, "nodiscard")) {
                cwarn(c, loc, "attributes", "'%s' attribute directive "
                      "ignored", an);
                continue;
            }
            if (!strcmp(an, "malloc")) {
                cwarn(c, loc, "attributes", "'malloc' attribute ignored; "
                      "valid only for functions");
                continue;
            }
            if (!strcmp(an, "aligned")) {
                cerror(c, loc, "alignment may not be specified for '%s'",
                       cident(c, name));
                continue;
            }
            if (!strcmp(an, "section")) {
                cerror(c, loc, "section attribute not allowed for '%s'",
                       cident(c, name));
                continue;
            }
            for (f = 0; f < sizeof fnonly / sizeof *fnonly; f++)
                if (!strcmp(an, fnonly[f])) {
                    cwarn(c, loc, "attributes", "'%s' attribute only applies "
                          "to function types", an);
                    break;
                }
            if (f < sizeof fnonly / sizeof *fnonly)
                continue;
            for (f = 0; f < sizeof ignored / sizeof *ignored; f++)
                if (!strcmp(an, ignored[f])) {
                    cwarn(c, loc, "attributes", "'%s' attribute ignored", an);
                    break;
                }
        }
        kids_free(&it);
    }
    kids_free(&k);
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
                    cconst_overflow(c, nloc);
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
    if (type_enum(TT, rd->ty)->in_struct)
        s.flags |= CSF_IN_STRUCT;
    {
        Attrs ea;
        memset(&ea, 0, sizeof ea);
        c->attr_quiet = true;       /* enumerator_attrs reports them */
        attrs_of_children(c, i, &ea);
        c->attr_quiet = false;
        if (ea.deprecated || ea.unavailable) {
            s.flags |= ea.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
            s.dep_msg = ea.dep_msg;
        }
    }
    ref = pushdecl(c, &s, false);
    vec_push(&c->ecs, ref);
    enumerator_attrs(c, i, nloc, name, ref);
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
    if (a.desig)
        cerror(c, ne ? csym(c, c->ecs.data[rd.first_ec + ne - 1])->loc :
               tloc(c, cnode(c, i)->tok), "'designated_init' attribute is "
               "only valid on 'struct' type");
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
    if (e->packed || c->opt.short_enums || prec > 32) {
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
    if (a.has_mode && a.mode_bytes) {
        TypeId mt = int_of_size(c, a.mode_bytes, uns);
        if (!is_err(c, mt)) {
            if (prec > a.mode_bytes * 8u && ne)
                cerror(c, csym(c, c->ecs.data[rd.first_ec])->loc,
                       "specified mode too small for enumerated values");
            else
                tem = mt;
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

/* handle_visibility_attribute on a tagged definition: C has no class types. */
static void tag_visibility(Checker *c, uint32_t i)
{
    uint32_t tg = find_child(c, i, N_TAG), j, etok = 0;
    Kids k;
    if (tg == NO_NODE)
        return;
    if (cnode(c, i)->tag == N_ENUM) {   /* gcc's input_location: the first enumerator */
        for (etok = cnode(c, i)->tok; tpunct(c, etok) != P_LBRACE; etok++)
            ;
        etok++;
    }
    kids_get(c, i, &k);
    for (j = 0; j < k.n; j++) {
        Kids it;
        uint32_t q;
        if (ntag(c, k.p[j]) != N_ATTRIBUTE)
            continue;
        kids_get(c, k.p[j], &it);
        for (q = 0; q < it.n; q++) {
            char an[48];
            if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                continue;
            attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
            if (!strcmp(an, "visibility") &&
                !attr_scope_of(c, c->nodes[it.p[q]].tok))
                cwarn(c, tloc(c, etok ? etok : cnode(c, tg)->tok),
                      "attributes", "'visibility' attribute ignored on types");
        }
        kids_free(&it);
    }
    kids_free(&k);
}

static void struct_visit(Checker *c, uint32_t i)
{
    uint32_t open = find_child(c, i, N_OPEN);
    int want;
    if (cnode(c, i)->tag == N_ENUM)
        want = TY_ENUM;
    else
        want = tckw(c, cnode(c, i)->tok) == CK_UNION ? TY_UNION : TY_STRUCT;
    if (want == TY_ENUM && open != NO_NODE) {   /* a trailing comma */
        Kids ek;
        uint32_t j, en = NO_NODE;
        kids_get(c, i, &ek);
        for (j = 0; j < ek.n; j++)
            if (ntag(c, ek.p[j]) == N_ENUMERATOR)
                en = ek.p[j];
        kids_free(&ek);
        if (en != NO_NODE && tpunct(c, last_tok(c, en) + 1) == P_COMMA)
            cc90(c, tloc(c, last_tok(c, en) + 1), NULL, "comma at end of "
                 "enumerator list");
    }
    if (want == TY_ENUM && find_child(c, i, N_TYPE_NAME) != NO_NODE) {
        uint32_t tn = find_child(c, i, N_TYPE_NAME);
        uint32_t tg_ = find_child(c, i, N_TAG);
        cpedantic(c, tloc(c, tg_ != NO_NODE ? cnode(c, tg_)->tok
                                              : first_tok(c, tn) - 1),
                  "ISO C does not support specifying 'enum' underlying types "
                  "before C2X");
    }
    if (open != NO_NODE)
        tag_visibility(c, i);
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

/* Attributes written after a '*' in a declarator belong to the pointer
 * type: aligned sets its alignment, packed is dropped with a warning (and
 * conflicts with an aligned before it in the same list). */
static void ptr_type_attrs(Checker *c, uint32_t d, TypeId ty, uint32_t tok,
                           Attrs *out)
{
    Kids k;
    uint32_t j;
    kids_get(c, d, &k);
    for (j = 0; j < k.n; j++) {
        uint32_t x = k.p[j];
        if (is_declarator_tag(ntag(c, x))) {
            ptr_type_attrs(c, x, ty, tok, out);
        } else if (ntag(c, d) == N_PTR && ntag(c, x) == N_ATTRIBUTE) {
            Attrs t;
            Kids it;
            uint32_t q;
            bool saw_aligned = false;
            memset(&t, 0, sizeof t);
            attr_collect(c, x, &t);
            if (t.aligned > out->aligned)
                out->aligned = t.aligned;
            kids_get(c, x, &it);
            for (q = 0; q < it.n; q++) {
                char an[48];
                if (ntag(c, it.p[q]) != N_ATTR_ITEM)
                    continue;
                attr_norm(tstr(c, c->nodes[it.p[q]].tok), an, sizeof an);
                if (!strcmp(an, "aligned"))
                    saw_aligned = true;
                else if (!strcmp(an, "packed")) {
                    if (saw_aligned)
                        cwarn(c, iloc(c, tok), "attributes", "ignoring "
                              "attribute 'packed' because it conflicts with "
                              "attribute 'aligned'");
                    else
                        cwarn(c, iloc(c, tok), "attributes", "'packed' "
                              "attribute ignored for type %s",
                              type_q(TT, ty));
                }
            }
            kids_free(&it);
        }
    }
    kids_free(&k);
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
    dep_spec_use(c, &sp);
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
    if (a.has_mode || a.vs_seen)
        g.ty = attr_apply_type(c, g.ty, &a);
    attrs_unknown_emit(c, &sp.attrs, ltok);
    if (top != NO_NODE)
        ptr_type_attrs(c, top, g.ty, ltok, &a);
    attrs_merge(&a, &sp.attrs);
    attrs_misapplied(c, &a, 'm', false, w == NO_NODE ? g.ty : 0, ltok);
    attrs_ctx_check(c, sp.node, g.ty, ltok, AC_F);
    attrs_ctx_check(c, i, g.ty, ltok, AC_F);
    attrs_section_check(c, &a, 'm', false, g.name, g.loc);
    strict_flex_check(c, sp.node, true, g.ty, g.name, g.loc, sp.tok0);
    strict_flex_check(c, i, true, g.ty, g.name, g.loc, NO_NODE);
    attrs_zcur_check(c, &a, false, g.loc);
    attrs_wina_check(c, &a, 'm', g.width >= 0, g.name, g.loc);
    if (g.width >= 0 && g.name && type_wina(c, g.ty))
        cerror(c, g.loc, "cannot declare bit-field '%s' with "
               "'warn_if_not_aligned' type", cident(c, g.name));
    memset(&fi, 0, sizeof fi);
    fi.wina = a.wina_al;
    fi.name = g.name;
    fi.ty = g.ty;
    fi.width = g.width;
    fi.align = g.s.align > a.aligned ? g.s.align : a.aligned;
    fi.packed = a.packed;
    fi.loc = g.loc;
    fi.dep = (a.deprecated ? CSF_DEPRECATED : 0) |
             (a.unavailable ? CSF_UNAVAILABLE : 0);
    fi.dmsg = a.dep_msg;
    attrs_names(c, sp.node, &fi.aset);
    attrs_names(c, i, &fi.aset);
    if (!fi.packed && cdecl_aset_has(c, fi.aset, "packed", NULL))
        fi.packed = true;       /* copied from another declaration */
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
        if (sp.kind == TSK_NONE && !sp.has_type && sp.default_int) {
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
                cped11(c, tloc(c, ltok), "ISO C99 doesn't support unnamed "
                          "structs/unions");
                grok(c, &sp, NO_NODE, DC_FIELD, false, false, NO_NODE, ltok,
                     ltok, &g);
                if (g.what != GD_NONE) {
                    FieldIn fi;
                    memset(&fi, 0, sizeof fi);
                    fi.ty = g.ty;
                    fi.width = -1;
                    fi.align = g.s.align > sp.attrs.aligned
                        ? g.s.align : sp.attrs.aligned;
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
    if (!in_extension(c, i)) {
        size_t n0 = c->diag->all.len;
        cped11(c, aloc, "ISO C99 does not support '_Static_assert'");
        choist(c, i, n0);
    }
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
        /* an erroneous value has no location of its own: the first token */
        cerror(c, is_err(c, c->ty[e]) ? tloc(c, first_tok(c, e)) : vloc,
               "expression in static assertion is not an integer");
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
            uint32_t p, np = node_pieces(c, s);
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
    if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_STRUCT)
        struct_semis(c, cnode(c, i)->tok);
    cpragma_apply(c, cnode(c, i)->tok);
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
    {   /* attributes among any pointer's qualifiers belong to the parameter */
        uint32_t d = top;
        while (d != NO_NODE) {
            if (ntag(c, d) == N_PTR)
                attrs_of_children(c, d, &a);
            d = inner_decl(c, d);
        }
    }
    {
        unsigned fb = fn_attr_walk(c, p, 0);
        if (fb) {
            uint32_t fn = c->par[p];
            fb = nocf_ignored(c, fb, tloc(c, first_tok(c, fn)));
            if (fb)
                s.ty = fn_attr_type(c, s.ty, fb);
        }
    }
    larger_than(c, s.loc, s.name, s.ty);
    attrs_unknown_emit(c, &sp.attrs, first_tok(c, p));
    attrs_merge(&a, &sp.attrs);
    attrs_misapplied(c, &a, 'p', false, 0, first_tok(c, p));
    attrs_ctx_check(c, sp.node, s.ty, first_tok(c, p), AC_P);
    attrs_ctx_check(c, p, s.ty, first_tok(c, p), AC_P);
    attrs_zcur_check(c, &a, false, s.loc);
    attrs_section_check(c, &a, 'p', false, s.name, s.loc);
    if (a.unused)
        s.flags |= CSF_USED | CSF_ATTR_UNUSED;
    if (a.deprecated || a.unavailable) {
        s.flags |= a.unavailable ? CSF_UNAVAILABLE : CSF_DEPRECATED;
        s.dep_msg = a.dep_msg;
    }
    if ((cnode(c, p)->flags & NF_SEMI) && c->fwd_warned != c->par[p] + 1) {
        /* mark_forward_parm_decls: once per parameter scope */
        c->fwd_warned = c->par[p] + 1;
        cpedantic(c, cdecl_line_start_loc(c, after),
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
    if (g.array_param)
        c->cv[p] |= (uint64_t)g.pre << 32;
    pop_specs(c, p);
}

static void array_visit(Checker *c, uint32_t i)
{
    if (!(cnode(c, i)->flags & NF_STAR))
        return;
    switch (cscope_kind(c)) {
    case SCK_PROTO:
        break;
    case SCK_FUNC: {
        uint32_t a;
        for (a = c->par[i]; a != NO_NODE && ntag(c, a) != N_FUNC_DEF &&
                            ntag(c, a) != N_COMPOUND; a = c->par[a])
            ;
        if (a != NO_NODE && ntag(c, a) == N_COMPOUND) {   /* in the body */
            cerror(c, tloc(c, cnode(c, i)->tok), "'[*]' not allowed in other "
                   "than function prototype scope");
            c->cv[i] |= 1;
        } else if (c->cur_func_node != NO_NODE)
            c->ef[c->cur_func_node] |= 1;
        break;
    }
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

uint32_t cdecl_inner_decl(const Checker *c, uint32_t i)
{
    return inner_decl(c, i);
}

uint32_t cdecl_array_size_node(Checker *c, uint32_t a)
{
    return array_size_node(c, a);
}

unsigned cdecl_quals_of(Checker *c, uint32_t node)
{
    return quals_of(c, node);
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
    bool iso_def;
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
    attrs_unknown_emit(c, &sp.attrs, ltok);
    cpragma_optimize_repeat(c, cinput_loc(c, ltok));
    acc_start(g.s.name, g.s.loc);
    imp_name = alloc_name;
    {
        uint32_t pt = cparm_make(c, funcdef_fnode(c, top));
        imp_n = cparm_implied(c, pt, imp_l, 16);
        cparm_release(c, pt);
    }
    attrs_alloc_check(c, fp.specs, type_kind(TT, g.s.ty) == TY_FUNC ? g.s.ty :
                      type_canon(TT, g.s.ty), ltok);
    alloc_name = 0;
    {
        AttrState st = {0};
        attrs_copy_check(c, fp.specs, CS_FUNC, g.s.name, ltok, &st);
        g.s.flags |= (st.pure ? CSF_PURE : 0) | (st.cnst ? CSF_CONSTFN : 0);
        if (st.calign > g.s.ualign)
            g.s.ualign = st.calign;
        g.s.nonnull |= st.nonnull | sp.attrs.nonnull;
        if (sp.attrs.fmt)
            g.s.fmt = sp.attrs.fmt;
        if (sp.attrs.fmtarg)
            g.s.fmtarg = sp.attrs.fmtarg;
    }
    attrs_section_check(c, &sp.attrs, 'f', false, g.s.name, g.s.loc);
    attrs_zcur_check(c, &sp.attrs, true, g.s.loc);
    attrs_wina_check(c, &sp.attrs, 'f', false, g.s.name, g.s.loc);
    g.s.sect = sp.attrs.sec;
    s = g.s;
    {
        unsigned fb = fn_attr_bits(c, fp.specs, fd, true);
        if (fb) {
            fb = nocf_ignored(c, fb, tloc(c, c->specs.data[si].tok0));
            if (fb)
                s.ty = fn_attr_type(c, s.ty, fb);
        }
    }
    loc = s.loc;
    name = cident(c, s.name);
    if (nested)
        s.linkage = LK_INTERNAL;
    if (sp.attrs.weak)
        weak_apply(c, &s, sp.is_inline);
    inline_given(c, &s, sp.is_inline, fp.specs, fd);
    /* the return type */
    rt = type_base(TT, s.ty);
    if (!is_err(c, rt) && !is_void(c, rt) && !type_is_complete(TT, rt)) {
        const TypeEnt *e = type_ent(TT, type_canon(TT, s.ty));
        uint32_t n = e->n, flags = e->flags & (TF_VARIADIC | TF_NOPROTO | TF_NOCF | TF_TXUNSAFE);
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
    c->cd_builtin = false;
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
        /* an old-style definition of a library built-in is checked against
         * the built-in's prototype (current_function_prototype_built_in) */
        if (!c->cd_have_proto && !have_old && sym_public(&s)) {
            TypeId bp = ccall_builtin_ptype(c, name);
            if (bp && is_func(c, bp) &&
                type_compatible(TT, type_base(TT, s.ty),
                                type_base(TT, bp))) {
                c->cd_have_proto = c->cd_builtin = true;
                c->cd_proto = bp;
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
    s.parms = cparm_make(c, funcdef_fnode(c, top));
    iso_def = !nested && (s.flags & CSF_PROTO_DEF);
    ref = pushdecl(c, &s, g.default_int);
    if (iso_def && !cin_system(c, loc))
        cwarn(c, loc, "traditional", "traditional C rejects ISO C style "
              "function definitions");
    {
        CSym *t = csym(c, ref);
        t->flags |= CSF_DEFINED | CSF_TREE_STATIC |
                    (s.flags & (CSF_PROTO_DEF | CSF_KR_DEF));
        t->def_loc = loc;
    }
    acc_implied(c, ref, false, true);
    attrs_names(c, fp.specs, &csym(c, ref)->aset);
    acc_chain_implied(c, ref);
    if (sp.is_noreturn)
        aset_add(c, &csym(c, ref)->aset, "noreturn", "");
    dump_decl(c, csym(c, ref));
    c->func_sym = ref;
    c->kr_decls = (csym(c, ref)->flags & CSF_KR_DEF) != 0;
    c->in_head = true;
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
    c->kr_decls = false;
    c->in_head = false;
    if (comp == NO_NODE)
        return;
    if (ntag(c, comp) == N_FUNC_DEF)    /* no '{' followed the declarations */
        fd = comp;
    else if (ntag(c, comp) == N_COMPOUND)
        fd = c->par[comp];
    else
        return;
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
            larger_than(c, s->loc, s->name, s->ty);   /* declared again in the body */
            if (s->name) {
                cbind(c, NS_ORD, s->name, c->cb[p] - 1);
                if (!(s->flags & CSF_USED))
                    warn_if_shadowing(c, s);
            } else
                cpedantic(c, tloc(c, c->nodes[p + 1 - c->nodes[p].size].tok),
                          "ISO C does not support omitting "
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
                DiagOrd o0 = diag_ord(c->diag, ORD_LATE);
                if (!cexpr_undeclared_here(c, name))    /* gcc bound it to an error */
                    cpedwarn(c, fnloc, "implicit-int", "type of '%s' "
                             "defaults to 'int'", cident(c, name));
                diag_ord(c->diag, o0);
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
                DiagOrd o0 = diag_ord(c->diag, ORD_LATE);
                cerror(c, s->loc, "declaration for parameter '%s' but no "
                       "such parameter", sname(c, s));
                diag_ord(c->diag, o0);
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
                    if (c->cd_builtin)
                        cwarn(c, fnloc, "", "number of arguments doesn't match "
                              "built-in prototype");
                    else {
                        cerror(c, il, "number of arguments doesn't match "
                               "prototype");
                        cerror(c, c->cd_proto_loc, "prototype declaration");
                    }
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
                        if (!c->cd_builtin)
                            cpedantic(c, c->cd_proto_loc, "prototype "
                                      "declaration");
                    } else if (c->cd_builtin) {
                        cwarn(c, s->loc, "", "argument '%s' doesn't match "
                              "built-in prototype", sname(c, s));
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
        case N_UNARY:               /* __real__ x = ..., __imag__ x = ... */
            if (tckw(c, c->nodes[e].tok) == CK_REAL ||
                tckw(c, c->nodes[e].tok) == CK_IMAG) {
                e = first_child(c, e);
                continue;
            }
            return;
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
static void st_mark_value(Checker *c, uint32_t e);

static void st_mark(Checker *c, uint32_t e)
{
    while (ntag(c, e) == N_PAREN)
        e = first_child(c, e);
    if (ntag(c, e) == N_ASSIGN && tpunct(c, cnode(c, e)->tok) == P_ASSIGN) {
        wr_mark(c, first_child(c, e));
        if (last_child(c, e) != NO_NODE)
            st_mark_value(c, last_child(c, e));
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
/* st_mark for the last statement of a statement expression: its value is
 * used, only the left operands of commas are not. */
static void st_mark_value(Checker *c, uint32_t e)
{
    for (;;) {
        Kids k;
        bool comma;
        while (ntag(c, e) == N_PAREN)
            e = first_child(c, e);
        if (ntag(c, e) != N_BINARY || tpunct(c, cnode(c, e)->tok) != P_COMMA)
            return;
        kids_get(c, e, &k);
        comma = k.n == 2;
        if (comma) {
            st_mark(c, k.p[0]);
            e = k.p[1];
        }
        kids_free(&k);
        if (!comma)
            return;
    }
}

static void read_scan(Checker *c, uint32_t first, uint32_t last,
                      const uint32_t *names, uint8_t *read, uint32_t n)
{
    uint32_t k, j;
    c->stack.len = 0;
    for (k = first; k <= last; k++) {
        if (ntag(c, k) == N_EXPR_STMT) {
            uint32_t e = first_child(c, k);
            if (e == NO_NODE)
                ;
            else if (k + 1 < c->nn && ntag(c, k + 1) == N_SCOPE_END &&
                     c->par[k + 1] != NO_NODE && c->par[c->par[k + 1]] != NO_NODE &&
                     ntag(c, c->par[c->par[k + 1]]) == N_STMT_EXPR)
                st_mark_value(c, e);
            else
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
                DiagOrd o0 = diag_ord(c->diag, ORD_LATE); /* reported when the scope closes */
                cwarn(c, s->loc, "unused-variable", "unused variable '%s'",
                      sname(c, s));
                diag_ord(c->diag, o0);
                if (sym_public(s))
                    s->flags |= CSF_USED;
            } else if (!read[j] && !sym_public(s) && s->sc != SC_EXTERN &&
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
    if (fp.scope != NO_NODE) {
        unused_scan(c, fp.scope, se - 1);
        cstmt_dup_branches(c, fp.scope, se - 1);
        cstmt_dup_cond(c, fp.scope, se - 1);
    }
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

void cdecl_record_inline_static(Checker *c, SrcLoc loc, uint32_t name,
                                bool modifiable)
{
    InlStatic is;
    if (c->func_sym == SYM_NONE || !extern_inline(csym(c, c->func_sym)))
        return;
    is.loc = loc;
    is.fref = c->func_sym;
    is.name = name;
    is.modifiable = modifiable;
    vec_push(&c->inl_statics, is);
}

/* check_inline_statics: newest first, and only the functions that are still
 * inline definitions. */
void cdecl_check_inline_statics(Checker *c)
{
    size_t k;
    for (k = c->inl_statics.len; k-- > 0;) {
        const InlStatic *is = &c->inl_statics.data[k];
        const CSym *f = csym(c, is->fref);
        if (!extern_inline(f))
            continue;
        cpedwarn(c, is->loc, "", is->modifiable
                 ? "'%s' is static but declared in inline function '%s' which "
                   "is not static"
                 : "'%s' is static but used in inline function '%s' which is "
                   "not static", cident(c, is->name), sname(c, f));
    }
}

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
            (s->flags & CSF_DECL_EXTERNAL) && s->name &&
            !cdecl_aset_has(c, s->aset, "weakref", NULL)) {
            if (s->flags & CSF_CUSED)
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
    case N_DECLARED: {
        /* gcc declares the name before it parses the initializer: a
         * conflict is reported even if the initializer has a syntax error */
        bool quiet = c->quiet;
        c->quiet = quiet && i + 1 < c->nn && ntag(c, i + 1) == N_INIT_DECL;
        DiagOrd o0 = diag_ord(c->diag, ORD_EARLY);
        /* an error inside the declarator itself: gcc finishes the
         * declaration (implicit int) after reporting it */
        uint32_t nt = cnode(c, i)->tok;
        if (quiet && c->first_err_params && nt < c->first_err_tok &&
            c->first_err_tok <= scan_end(c, nt, true)) {
            c->quiet = false;
            diag_ord(c->diag, ORD_LATE);
        }
        declared_visit(c, i);
        diag_ord(c->diag, o0);
        c->quiet = quiet;
        cinit_declared(c, i);
        break;
    }
    case N_INIT_DECL:
        init_decl_visit(c, i);
        break;
    case N_ATTR_STMT:
        attr_stmt_visit(c, i);
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
        sso_check(c, i);
        if (c->par[i] != NO_NODE && ntag(c, c->par[i]) == N_LABEL &&
            tokp(c, cnode(c, i)->tok)->kind != TK_PUNCT) {
            Kids ak;
            uint32_t n;
            kids_get(c, i, &ak);
            for (n = 0; n < ak.n; n++) {
                char nm[48];
                if (ntag(c, ak.p[n]) != N_ATTR_ITEM)
                    continue;
                attr_norm(tstr(c, c->nodes[ak.p[n]].tok), nm, sizeof nm);
                if (!strcmp(nm, "fallthrough"))
                    cwarn(c, cdecl_line_start_loc(c, cnode(c, i)->tok),
                          "attributes", "'fallthrough' attribute ignored");
            }
            kids_free(&ak);
        }
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
