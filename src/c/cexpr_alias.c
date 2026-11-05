/* cexpr_alias.c - postfix and unary operators, -Waddress-of-packed-member, scalar_storage_order, -Wstrict-aliasing
 * (split from cexpr.c; see its header comment). */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- postfix operators --------------------------------------------------------- */

void e_paren(Checker *c, uint32_t i)
{
    uint32_t k = first_child(c, i);
    if (k == NO_NODE || ntag(c, k) == N_ERROR) {
        set_err(c, i);
        return;
    }
    copy_node(c, i, k);
}

bool is_const(Checker *c, uint32_t i)
{
    return c->ck[i] == K_ICE || c->ck[i] == K_FOLD || c->ck[i] == K_FLOAT ||
           c->ck[i] == K_ADDR;
}

bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out);
bool rvalue_ok(Checker *c, uint32_t i);
bool rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc);
unsigned vec_esize(Checker *c, TypeId el);
TypeId vec_elem(Checker *c, TypeId vt);

/* ---- -Waddress-of-packed-member ----------------------------------------------
 * c-family/c-warn.cc warn_for_address_or_pointer_of_packed_member: a pointer
 * that takes the address of a member of a packed struct, or converts a
 * pointer to a packed struct, to a type that is more strictly aligned. */

/* min_align_of_type, in bytes (1 for void, functions, incomplete types). */
static unsigned pk_align(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    TypeKind kk = tkind(c, k);
    if (kk == TY_VOID || kk == TY_FUNC || kk == TY_ERROR || !complete(c, k))
        return 1;
    return type_align(TT, t);     /* a typedef may carry aligned(N), even 1 */
}

static bool pk_packed_rec(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    return is_record(c, k) && (type_record(TT, k)->flags & RF_PACKED);
}

static void pk_defined_here(Checker *c, Diagnostic *d, TypeId t)
{
    TypeId k = type_canon(TT, t);
    if (d && is_record(c, k) && type_record(TT, k)->tag &&
        type_record(TT, k)->nfields)
        cnote(c, d, type_record(TT, k)->loc, "defined here");
}

/* check_alignment_of_packed_member for field f of record rec at offset off
 * (bits): whether the member may be misaligned for a pointer to type. */
static int pk_member(Checker *c, TypeId type, TypeId rec, const Field *f,
                     uint64_t off, bool rvalue)
{
    unsigned ta;
    bool byfield;
    /* finish_struct gives DECL_PACKED to the members of a packed record
     * whose type is aligned more than a byte */
    byfield = (f->flags & FF_PACKED) ||
              (pk_packed_rec(c, rec) && pk_align(c, f->ty) > 1);
    if (!(byfield || pk_packed_rec(c, f->ty)) ||
        (f->flags & FF_BITFIELD) || (rvalue && !is_array(c, f->ty)))
        return 0;
    ta = pk_align(c, type);
    if (!(pk_align(c, rec) < ta || (off / 8) % ta != 0))
        return 0;
    return byfield ? 1 : 2;     /* 2: only the member's type is packed */
}

/* Where gcc reports a packed-member address: the location of the folded
 * expression (&*P is P, *&P is P at the * ), and of the operand when the
 * outermost operator is a dereference. */
static SrcLoc pk_loc(Checker *c, uint32_t e)
{
    uint32_t ops[16], n = 0, b = e, i;
    SrcLoc loc, under;
    int kind = 0;               /* 0 plain, 1 addr, 2 deref */
    while (n < 16 && b != NO_NODE && ntag(c, b) == N_UNARY &&
           (npunct(c, b) == P_STAR || npunct(c, b) == P_AMP)) {
        ops[n++] = b;
        b = strip_paren(c, first_child(c, b));
    }
    if (b == NO_NODE)
        return first_loc(c, e);
    loc = under = first_loc(c, b);
    for (i = n; i-- > 0;) {
        SrcLoc ol = first_loc(c, ops[i]);
        bool amp = npunct(c, ops[i]) == P_AMP;
        if (amp && kind == 2) {
            kind = 0;
            loc = first_loc(c, b);
        } else if (!amp && kind == 1) {
            kind = 0;
            loc = ol;
        } else {
            under = loc;
            kind = amp ? 1 : 2;
            loc = ol;
        }
    }
    return kind == 2 ? under : loc;
}

/* mode 0: the conversion context (assignment, initializer, argument), which
 * looks through a cast and reports a member of a packed record at the cast;
 * 1: the cast itself, which reports only a member whose type is packed;
 * 2: the operand of a cast seen from its context.  arg: a call argument,
 * located at the member operator. */

/* gcc's build_c_cast does not look for packed members in a cast that leaves
 * the pointer type as it is; the conversion it feeds checks them instead. */
bool noop_ptr_cast(Checker *c, TypeId to, uint32_t operand)
{
    TypeId o = rvt(c, operand);
    return is_ptr(c, o) &&
           type_canon(TT, unqual(c, o)) == type_canon(TT, unqual(c, to));
}

static void packed_ptr_check(Checker *c, TypeId to, uint32_t e)
{
    packed_ptr_check_x(c, to, e, 0, 0, false);
}

void packed_ptr_check_x(Checker *c, TypeId to, uint32_t e, int mode,
                               SrcLoc castloc, bool arg)
{
    bool rvalue = true, indirect = false;
    uint32_t r, k[3];
    TypeId type;
    if (!TT->any_packed || e == NO_NODE || node_err(c, e) || !is_ptr(c, to))
        return;
    e = strip_paren(c, e);
    if (e == NO_NODE || node_err(c, e))
        return;
    if (c->ck[e] == K_ADDR && c->cb[e] == 0)
        return;                 /* folded to a constant (offsetof idiom) */
    type = pointee(c, type_canon(TT, to));
    if (mode == 0 && ntag(c, e) == N_CAST) {
        uint32_t ck[4], in;
        if (node_children(c->nodes, e, ck, 4) >= 2) {
            in = strip_paren(c, ck[1]);
            if (in != NO_NODE && noop_ptr_cast(c, c->ty[e], in))
                packed_ptr_check_x(c, to, in, 2, first_loc(c, e), arg);
        }
        return;
    }
    if (ntag(c, e) == N_COND) {
        if (nkids(c, e, k, 3) == 3) {
            packed_ptr_check_x(c, to, k[1], mode, castloc, false);
            packed_ptr_check_x(c, to, k[2], mode, castloc, false);
        }
        return;
    }
    if (ntag(c, e) == N_BINARY && npunct(c, e) == P_COMMA) {
        if (nkids(c, e, k, 2) == 2)
            packed_ptr_check_x(c, to, k[1], mode, castloc, false);
        return;
    }
    r = e;
    if (ntag(c, r) == N_UNARY && npunct(c, r) == P_STAR) {
        r = strip_paren(c, first_child(c, r));
        indirect = true;
        if (r == NO_NODE)
            return;
    }
    if (ntag(c, r) == N_UNARY && npunct(c, r) == P_AMP) {
        r = strip_paren(c, first_child(c, r));
        rvalue = indirect;
        if (r == NO_NODE)
            return;
        if (!indirect && ntag(c, r) == N_UNARY && npunct(c, r) == P_STAR) {
            /* &*p folds to p (&*&x is handled below) */
            uint32_t x = strip_paren(c, first_child(c, r));
            if (x != NO_NODE && !(ntag(c, x) == N_UNARY &&
                                  npunct(c, x) == P_AMP)) {
                packed_ptr_check_x(c, to, x, mode, castloc, arg);
                return;
            }
        }
    }
    /* *&x folds to x */
    while (r != NO_NODE && ntag(c, r) == N_UNARY && npunct(c, r) == P_STAR) {
        uint32_t u = strip_paren(c, first_child(c, r));
        if (u == NO_NODE || ntag(c, u) != N_UNARY || npunct(c, u) != P_AMP)
            break;
        r = strip_paren(c, first_child(c, u));
    }
    if (r == NO_NODE || node_err(c, r))
        return;
    {
        TypeId rt = 0;
        bool decl = false;
        if (ntag(c, r) == N_IDENT) {
            decl = true;        /* a variable or parameter: only they have a packed type */
            rt = c->ty[r];
        } else if (ntag(c, r) == N_CALL) {
            uint32_t fn = first_child(c, r);
            TypeId ft;
            if (fn == NO_NODE || node_err(c, fn))
                return;
            ft = type_canon(TT, c->ty[fn]);
            if (tkind(c, ft) == TY_PTR)
                ft = type_canon(TT, pointee(c, ft));
            if (tkind(c, ft) != TY_FUNC)
                return;
            rt = type_base(TT, ft);
            if (!is_ptr(c, rt))
                return;
            decl = true;
            rvalue = true;
        }
        if (decl) {
            if (is_err(c, rt) || mode == 2)     /* the cast reports it */
                return;
            if (rvalue && is_ptr(c, rt))
                rt = pointee(c, type_canon(TT, rt));
            while (is_array(c, rt))
                rt = elem_of(c, rt);
            if (pk_packed_rec(c, rt)) {
                unsigned ta = pk_align(c, type), ra = pk_align(c, rt);
                if (ra < ta) {
                    Diagnostic *d = cwarn_d(c, DL_WARNING,
                        cdecl_iloc(c, last_tok(c, e) + 1),
                        DW_ADDRESS_OF_PACKED_MEMBER, "converting a packed %s "
                        "pointer (alignment %u) to a %s pointer (alignment "
                        "%u) may result in an unaligned pointer value",
                        type_q(TT, rt), ra, type_q(TT, type), ta);
                    pk_defined_here(c, d, rt);
                    pk_defined_here(c, d, type);
                }
            }
            return;
        }
    }
    while (ntag(c, r) == N_MEMBER_EXPR || ntag(c, r) == N_INDEX ||
           (ntag(c, r) == N_UNARY && (tckw(c, c->nodes[r].tok) == CK_REAL ||
                                      tckw(c, c->nodes[r].tok) == CK_IMAG))) {
        uint32_t base = first_child(c, r);
        bool arrow = false;
        if (ntag(c, r) == N_UNARY) {
            if (rvalue)
                return;
            r = strip_paren(c, base);
            if (r == NO_NODE)
                return;
            continue;
        }
        if (rvalue && !is_array(c, c->ty[r]))
            return;             /* a member value, not an address */
        if (ntag(c, r) == N_MEMBER_EXPR) {
            TypeId rec;
            const Field *f;
            uint64_t off = 0;
            unsigned q = 0;
            int pk;
            arrow = (c->nodes[r].flags & NF_ARROW) != 0;
            if (base == NO_NODE || node_err(c, base))
                return;
            rec = type_canon(TT, arrow ? pointee(c, rvt(c, base)) : c->ty[base]);
            f = find_field(c, rec, cnode_ident(c, r), &off, &q);
            if (!f)
                return;
            pk = pk_member(c, type, rec, f, off, rvalue);
            if (pk && (mode == 1 ? pk == 2 : mode == 2 ? pk == 1 : true)) {
                SrcLoc wl = mode == 2 && castloc ? castloc : pk_loc(c, e);
                if (arg && mode == 0 && ntag(c, e) == N_MEMBER_EXPR && !castloc)
                    wl = expr_loc(c, e);
                cwarn(c, wl, DW_ADDRESS_OF_PACKED_MEMBER,
                      "taking address of packed member of %s may result in "
                      "an unaligned pointer value", type_q(TT, mainv(c, rec)));
                return;
            }
            if (pk)
                return;
            if (is_array(c, c->ty[r]))
                rvalue = false;
            if (rvalue || arrow)
                return;
        } else {
            uint32_t kk[2];
            if (nkids(c, r, kk, 2) < 2)
                return;
            base = strip_paren(c, kk[0]);
            if (base == NO_NODE || (!is_array(c, c->ty[base]) &&
                                    tkind(c, type_canon(TT, c->ty[base])) !=
                                        TY_VECTOR))
                return;         /* p[i] is *(p + i) */
            if (is_array(c, c->ty[r]))
                rvalue = false;
            if (rvalue)
                return;
        }
        r = strip_paren(c, base);
        if (r == NO_NODE)
            return;
    }
}

/* ---- scalar_storage_order ------------------------------------------------ */

/* A record laid out in the byte order opposite to the target's.  Its scalar
 * members, and the arrays of them (but not of bytes), are "reverse". */
static bool sso_rec(Checker *c, TypeId t)
{
    TypeKind k;
    t = type_canon(TT, t);
    k = tkind(c, t);
    return (k == TY_STRUCT || k == TY_UNION) &&
           (type_record(TT, t)->flags & RF_SSO);
}

static bool sso_agg(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, type_canon(TT, t));
    return k == TY_STRUCT || k == TY_UNION;
}

static bool sso_arr(Checker *c, uint32_t e);

/* Whether the member or element expression e lies in reverse storage. */
static bool sso_container(Checker *c, uint32_t e)
{
    uint32_t k[2];
    if (ntag(c, e) == N_MEMBER_EXPR) {
        uint32_t base = first_child(c, e);
        TypeId t;
        if (base == NO_NODE || node_err(c, base))
            return false;
        if (c->nodes[e].flags & NF_ARROW) {
            t = rvt(c, base);
            if (!is_ptr(c, t))
                return false;
            t = pointee(c, type_canon(TT, t));
        } else
            t = c->ty[base];
        return sso_rec(c, t);
    }
    if (nkids(c, e, k, 2) < 2)
        return false;
    k[0] = strip_paren(c, k[0]);
    return k[0] != NO_NODE && is_array(c, c->ty[k[0]]) && sso_arr(c, k[0]);
}

/* An array expression of reverse order: the elements are scalars wider than
 * a byte (TYPE_REVERSE_STORAGE_ORDER on the array type). */
static bool sso_arr(Checker *c, uint32_t e)
{
    TypeId el;
    bool ov;
    e = strip_paren(c, e);
    if (e == NO_NODE || node_err(c, e) || !is_array(c, c->ty[e]) ||
        (ntag(c, e) != N_MEMBER_EXPR && ntag(c, e) != N_INDEX))
        return false;
    for (el = c->ty[e]; is_array(c, el);)
        el = elem_of(c, el);
    if (sso_agg(c, el) || !complete(c, el) || type_size(TT, el, &ov) <= 1)
        return false;
    return sso_container(c, e);
}

/* A scalar (not aggregate, pointer or vector) member or element of reverse
 * storage: its address cannot be taken. */
static bool sso_scalar_ref(Checker *c, uint32_t e)
{
    TypeId t;
    e = strip_paren(c, e);
    if (e == NO_NODE || node_err(c, e) ||
        (ntag(c, e) != N_MEMBER_EXPR && ntag(c, e) != N_INDEX))
        return false;
    t = c->ty[e];
    if (sso_agg(c, t) || is_array(c, t) || is_ptr(c, t) ||
        tkind(c, type_canon(TT, t)) == TY_VECTOR)
        return false;
    return sso_container(c, e);
}

/* The pointer conversion of e takes a reverse record or array: a pointer to
 * it, or a decaying array of reverse arrays. */
static bool sso_ptr_src(Checker *c, uint32_t e)
{
    e = strip_paren(c, e);
    if (e == NO_NODE || node_err(c, e))
        return false;
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_AMP)
        return sso_arr(c, first_child(c, e));
    return is_array(c, c->ty[e]) && is_array(c, elem_of(c, c->ty[e])) &&
           sso_arr(c, e);
}

/* A call of an allocator (the malloc attribute, or a built-in one): gcc
 * does not take its result for a pointer of another storage order. */
static bool sso_alloc_call(Checker *c, uint32_t e)
{
    static const char *const alloc[] = {"malloc", "calloc", "alloca",
        "aligned_alloc", "__builtin_malloc", "__builtin_calloc",
        "__builtin_alloca", "__builtin_alloca_with_align",
        "__builtin_aligned_alloc"};
    uint32_t fn, sets[3];
    unsigned n, k;
    e = strip_paren(c, e);
    if (e == NO_NODE || ntag(c, e) != N_CALL)
        return false;
    fn = strip_paren(c, first_child(c, e));
    if (fn == NO_NODE)
        return false;
    if (ntag(c, fn) == N_IDENT)
        for (k = 0; k < sizeof alloc / sizeof *alloc; k++)
            if (!strcmp(cident(c, cnode_ident(c, fn)), alloc[k]))
                return true;
    n = cexpr_asets(c, fn, false, sets);
    for (k = 0; k < n; k++)
        if (cdecl_aset_has(c, sets[k], "malloc", NULL))
            return true;
    return false;
}

/* An array decays to a pointer unless it is the operand of &, sizeof,
 * alignof, typeof or a subscript. */
void sso_decay(Checker *c, uint32_t i)
{
    uint32_t p = i, k[2];
    if (c->ck[i] == K_ERR || !is_array(c, c->ty[i]) || !sso_arr(c, i))
        return;
    do {
        i = p;
        p = c->par[i];
    } while (p != NO_NODE && ntag(c, p) == N_PAREN);
    if (p != NO_NODE) {
        switch (ntag(c, p)) {
        case N_SIZEOF_EXPR: case N_ALIGNOF_EXPR: case N_TYPEOF:
            return;
        case N_UNARY:
            if (npunct(c, p) == P_AMP)
                return;
            break;
        case N_INDEX:
            if (nkids(c, p, k, 2) == 2 && k[0] == i)
                return;
            break;
        default:
            break;
        }
    }
    cwarn(c, first_loc(c, i), DW_SCALAR_STORAGE_ORDER, "address of array with "
          "reverse scalar storage order requested");
}

static bool assign_check(Checker *c, uint32_t expr, TypeId lhs,
                         const ConvInfo *ci);

void cexpr_packed_check(Checker *c, uint32_t expr, TypeId to)
{
    packed_ptr_check(c, to, expr);
}

bool cexpr_assign_check(Checker *c, uint32_t expr, TypeId lhs,
                        const ConvInfo *ci)
{
    bool r = assign_check(c, expr, lhs, ci);
    if (expr != NO_NODE && !is_err(c, lhs))
        packed_ptr_check_x(c, lhs, expr, 0, 0, ci && ci->context == CONV_ARG);
    return r;
}

/* -Wtraditional-conversion: an argument whose prototype conversion differs
 * from the default promotions an unprototyped call would apply. */
static bool trad_integral(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return gcc_integer(c, t) || k == TY_ENUM || k == TY_BOOL;
}

static void trad_conv(Checker *c, uint32_t e, TypeId lt, TypeId rt, SrcLoc l,
                      const char *fn, int pn)
{
    TypeId cl = type_canon(TT, lt), cr = type_canon(TT, rt);
    bool li = trad_integral(c, cl), ri = trad_integral(c, cr);
    bool lr = gcc_real(c, cl), rr = gcc_real(c, cr);
    bool lc = is_complex(c, cl), rc = is_complex(c, cr);
    const char *w = NULL;
    if (li && rr)
        w = "integer rather than floating";
    else if (li && rc)
        w = "integer rather than complex";
    else if (lc && rr)
        w = "complex rather than floating";
    else if (lc && ri)
        w = "complex rather than integer";
    else if (lr && ri)
        w = "floating rather than integer";
    else if (lr && rc)
        w = "floating rather than complex";
    else if (lr && rr) {
        if (uc_prec(c, cl) == (unsigned)float_prec(c, TY_FLOAT) &&
            diag_enabled(c->diag, DW_TRADITIONAL_CONVERSION))
            cwarn(c, l, DW_NONE, "passing argument %d of '%s' as 'float' rather "
                  "than 'double' due to prototype", pn, fn);
        return;
    } else if (li && ri) {
        TypeId t1 = promoted(c, e);
        bool lu = !is_signed(c, cl), u1 = !is_signed(c, t1);
        if (tkind(c, cl) == TY_ENUM && mainv(c, cl) == mainv(c, cr))
            return;
        if (int_bits(c, cl) != int_bits(c, t1))
            w = "with different width";
        else if (lu == u1 || tkind(c, cl) == TY_ENUM)
            return;
        else {
            if (const_fits(c, e, t1, cl))
                return;
            w = lu ? "as unsigned" : "as signed";
        }
        if (diag_enabled(c->diag, DW_TRADITIONAL_CONVERSION))
            cwarn(c, l, DW_TRADITIONAL_CONVERSION, "passing argument %d of "
                  "'%s' %s due to prototype", pn, fn, w);
        return;
    }
    if (w && diag_enabled(c->diag, DW_TRADITIONAL_CONVERSION))
        cwarn(c, l, DW_TRADITIONAL_CONVERSION, "passing argument %d of '%s' as "
              "%s due to prototype", pn, fn, w);
}

/* The built-in function expression e names (see ccall_builtin_ref); a
 * replaced function type turns *pt (the decayed pointer type) into a pointer
 * to it. */
const char *fnref_builtin(Checker *c, uint32_t e, TypeId *pt, char *buf,
                                 size_t n)
{
    uint32_t ref;
    const CSym *s;
    TypeId fty = 0;
    const char *nm;
    e = strip_paren(c, e);
    if (e != NO_NODE && !node_err(c, e) && ntag(c, e) == N_UNARY &&
        npunct(c, e) == P_AMP)
        e = strip_paren(c, first_child(c, e));
    if (e == NO_NODE || node_err(c, e) || ntag(c, e) != N_IDENT)
        return NULL;
    ref = lookup_ord(c, cnode_ident(c, e));
    if (ref == SYM_NONE) {      /* an undeclared __builtin_X */
        nm = cident(c, cnode_ident(c, e));
        return !strncmp(nm, "__builtin_", 10) && ccall_is_builtin(c, nm)
               ? nm : NULL;
    }
    s = csym(c, ref);
    if (s->kind != CS_FUNC)
        return NULL;
    nm = ccall_builtin_ref(c, s, &fty, buf, n);
    if (nm && fty)
        *pt = type_ptr(TT, fty);
    return nm;
}

/* The type e has as an initializer or operand, where gcc keeps a built-in
 * redeclared without a prototype at the built-in's own type. */
TypeId cexpr_builtin_ptr_type(Checker *c, uint32_t e, TypeId t)
{
    char buf[48];
    (void)fnref_builtin(c, e, &t, buf, sizeof buf);
    return t;
}

static bool assign_check(Checker *c, uint32_t expr, TypeId lhs,
                         const ConvInfo *ci)
{
    Conv x;
    char m[4][640], q[48];
    const char *T, *R, *fn = ci->fname ? ci->fname : "";
    int ctx = ci->context, pn = ci->parmnum;
    TypeId lt, rt, cl, cr;
    TypeKind kl, kr;
    uint32_t p;
    char bnbuf[48];
    const char *bname;
    if (expr == NO_NODE || node_err(c, expr) || is_err(c, lhs))
        return false;
    lt = unqual(c, lhs);
    rt = rvt(c, expr);
    if (is_err(c, rt))
        return false;
    bname = fnref_builtin(c, expr, &rt, bnbuf, sizeof bnbuf);
    x.c = c;
    x.ci = ci;
    x.expr = expr;
    x.type = lt;
    x.rhstype = rt;
    x.npc = is_npc(c, expr);
    p = c->par[expr];
    switch (ctx) {
    case CONV_ASSIGN:
        x.loc = ci->loc ? ci->loc
                        : p != NO_NODE && ntag(c, p) == N_ASSIGN
                              ? cnode_loc(c, p) : expr_loc(c, expr);
        x.eloc = ci->eloc ? ci->eloc : first_loc(c, expr);
        break;
    case CONV_ARG:
        x.loc = x.eloc = ci->loc ? ci->loc : expr_loc(c, expr);
        break;
    default:
        x.loc = x.eloc = ci->loc ? ci->loc : expr_loc(c, expr);
        break;
    }
    if (ctx == CONV_ARG && diag_enabled(c->diag, DW_TRADITIONAL_CONVERSION) &&
        strncmp(fn, "__builtin_", 10)) {
        SrcLoc tl = has_ival(c, strip_paren(c, expr))
                    ? (c->u->toks[first_tok(c, expr)].exp
                       ? c->u->toks[first_tok(c, expr)].exp
                       : first_loc(c, expr)) : x.loc;
        if (!cin_system(c, tl))
            trad_conv(c, expr, lt, rt, tl, fn, pn);
    }
    cl = type_canon(TT, lt);
    cr = type_canon(TT, rt);
    kl = tkind(c, cl);
    kr = tkind(c, cr);

    if (cexpr_cxx_compat(c, expr) && kl == TY_ENUM &&
        mainv(c, orig_type(c, expr)) != mainv(c, lt)) {
        sp(m[CONV_ARG], "enum conversion when passing argument %d of '%s' is "
           "invalid in C++", pn, fn);
        if (ci->lhs_bitfield) {
            sp(m[CONV_ASSIGN], "enum conversion in assignment is invalid in "
               "C++");
            sp(m[CONV_INIT], "enum conversion in initialization is invalid in "
               "C++");
        } else {
            sp(m[CONV_ASSIGN], "enum conversion from %s to %s in assignment "
               "is invalid in C++", cmp_tstr(c, expr), type_q(TT, lt));
            sp(m[CONV_INIT], "enum conversion from %s to %s in initialization "
               "is invalid in C++", cmp_tstr(c, expr), type_q(TT, lt));
        }
        sp(m[CONV_RETURN], "enum conversion from %s to %s in return is "
           "invalid in C++", cmp_tstr(c, expr), type_q(TT, lt));
        conv_diag(&x, RK_PED, DW_CPP_COMPAT, m, true);
    }
    if (diag_enabled(c->diag, DW_ENUM_CONVERSION)) {
        TypeId ot = orig_type(c, expr);
        if (tkind(c, ot) == TY_ENUM && kl == TY_ENUM &&
            mainv(c, ot) != mainv(c, lt))
            cwarn(c, x.loc, DW_ENUM_CONVERSION, "implicit conversion from %s "
                  "to %s", type_q(TT, ot), type_q(TT, lt));
    }
    if (mainv(c, lt) == mainv(c, rt)) {
        if (ci->lhs_bits && gcc_integer(c, lt))
            conv_arith(&x);        /* a bit-field is narrower than its type */
        return true;
    }
    if (kl == TY_BOOL && expr != NO_NODE && (gcc_integer(c, cr) || is_flt(c, cr) || is_complex(c, cr)))
        cexpr_truth_warn(c, expr, cinput_loc(c, c->nodes[expr].tok));
    if (kr == TY_VOID) {
        if (ci->warnopt)
            cwarn(c, x.loc, ci->warnopt, "void value not ignored as it ought "
                  "to be");
        else
            cerror(c, x.loc, ci->context == CONV_ARG
                   ? "invalid use of void expression"
                   : "void value not ignored as it ought to be");
        return false;
    }
    if (!complete(c, rt)) {
        incomplete_error(c, x.loc, expr, rt);
        return false;
    }
    if (type_is_arith(TT, cl) && type_is_arith(TT, cr) &&
        kl != TY_VECTOR && kr != TY_VECTOR) {
        if (ci->warnopt && ctx == CONV_ARG)
            builtin_noproto_arg(&x);
        conv_arith(&x);
        return true;
    }
    if (kl == TY_VECTOR && kr == TY_VECTOR) {
        /* vector_types_convertible_p */
        bool same_size, el_int, er_int, lax_ok, ok_a, ok_b;
        if (type_compatible(TT, mvt(c, cl), mvt(c, cr)))
            return true;
        if (expr != NO_NODE && vector_truth_node(c, expr) &&
            type_size(TT, cl, &ok_a) == type_size(TT, cr, &ok_b))
            return true;        /* a comparison yields an opaque vector */
        same_size = type_size(TT, cl, &ok_a) == type_size(TT, cr, &ok_b);
        el_int = type_is_integer(TT, type_base(TT, cl));
        er_int = type_is_integer(TT, type_base(TT, cr));
        lax_ok = same_size && el_int == er_int &&
                 (type_is_integer(TT, type_base(TT, cl)) ||
                  type_ent(TT, cl)->n == type_ent(TT, cr)->n);
        if (c->opt.lax_vector && lax_ok)
            return true;
        if (lax_ok && !c->lax_noted) {
            c->lax_noted = true;
            diag_report(c->diag, DL_NOTE, DW_NONE, cinput_loc(c, c->nodes[expr].tok),
                        "use '-flax-vector-conversions' to permit conversions "
                        "between vectors with differing element types or "
                        "numbers of subparts");
        }
        goto incompatible;
    }
    if (kl == TY_UNION && kr != TY_UNION &&
        (type_record(TT, cl)->flags & RF_TRANSPARENT)) {
        /* gcc: a transparent union accepts any of its members' types
         * (pointer members also take void * and null pointer constants) */
        const Record *tr = type_record(TT, cl);
        for (uint32_t k = 0; k < tr->nfields; k++) {
            TypeId mt = c->tt.fields.data[tr->fields + k].ty;
            TypeId mc = type_canon(TT, mt);
            bool ok = type_compatible(TT, mvt(c, mt), mvt(c, rt));
            if (!ok && tkind(c, mc) == TY_PTR) {
                if (kr == TY_PTR) {
                    TypeId ml = type_canon(TT, type_base(TT, mc));
                    TypeId mr = type_canon(TT, type_base(TT, cr));
                    ok = tkind(c, ml) == TY_VOID || tkind(c, mr) == TY_VOID ||
                         type_compatible(TT, mvt(c, ml), mvt(c, mr));
                } else if (x.npc) {
                    ok = true;
                }
            }
            if (!ok)
                continue;
            if (c->opt.pedantic) {
                SrcFile *sf = ci->fsym ? srcmgr_file_of(c->sm,
                                   csym(c, ci->fsym - 1)->loc) : NULL;
                if (!(sf && sf->system_header))
                    cpedantic(c, x.loc, "ISO C prohibits argument conversion "
                              "to union type");
            }
            return true;
        }
    }
    if ((kl == TY_STRUCT || kl == TY_UNION) && kl == kr &&
        type_compatible(TT, mainv(c, lt), mainv(c, rt)))
        return true;

    if (kl == TY_PTR && kr == TY_PTR) {
        TypeId ttl = type_canon(TT, type_base(TT, cl));
        TypeId ttr = type_canon(TT, type_base(TT, cr));
        TypeId mvl = mvt(c, ttl), mvr = mvt(c, ttr);
        TypeId ul, sl, ur, sr;
        bool opaque = tkind(c, ttl) == TY_VECTOR && tkind(c, ttr) == TY_VECTOR &&
                      type_ent(TT, ttl)->n == type_ent(TT, ttr)->n;
        int target_cmp = 0;
        bool lvoid = is_void(c, ttl) && !(TYPE_QUALS(ttl) & TQ_ATOMIC);
        bool rvoid = is_void(c, ttr) && !(TYPE_QUALS(ttr) & TQ_ATOMIC);
        bool ok = lvoid || rvoid;
        T = type_q(TT, lt);
        R = type_q(TT, rt);
        if (sso_rec(c, ttl) != (sso_rec(c, ttr) || sso_ptr_src(c, expr)) &&
            !(ctx == CONV_ARG && ci->fname && ccall_is_builtin(c, ci->fname)) &&
            !sso_alloc_call(c, expr)) {
            sp(m[CONV_ARG], "passing argument %d of '%s' from incompatible "
               "scalar storage order", pn, fn);
            sp(m[CONV_ASSIGN], "assignment to %s from pointer type %s with "
               "incompatible scalar storage order", T, R);
            sp(m[CONV_INIT], "initialization of %s from pointer type %s with "
               "incompatible scalar storage order", T, R);
            sp(m[CONV_RETURN], "returning %s from pointer type with "
               "incompatible scalar storage order %s", R, T);
            conv_diag(&x, RK_WARN, DW_SCALAR_STORAGE_ORDER, m, true);
        }
        if (!ok)
            ok = (target_cmp = comp_target(&x, cl, cr)) != 0 || opaque;
        if (!ok) {
            sign_map(c, mvl, &ul, &sl);
            sign_map(c, mvr, &ur, &sr);
            ok = ul == ur && sl == sr &&
                 (TYPE_QUALS(mvl) & TQ_ATOMIC) == (TYPE_QUALS(mvr) & TQ_ATOMIC);
        }
        if (ok) {
            unsigned qnoat = TQ_CONST | TQ_VOLATILE | TQ_RESTRICT;
            if (is_array(c, ttr)) {
                TypeId sr2 = strip_arr(c, ttr), sl2 = strip_arr(c, ttl);
                unsigned miss = TYPE_QUALS(sr2) & ~TYPE_QUALS(sl2);
                if (miss & qnoat) {
                    const char *qs = qual_str(q, miss);
                    sp(m[CONV_ARG], "passing argument %d of '%s' discards '%s' "
                       "qualifier from pointer target type", pn, fn, qs);
                    sp(m[CONV_ASSIGN], "assignment discards '%s' qualifier from "
                       "pointer target type", qs);
                    sp(m[CONV_INIT], "initialization discards '%s' qualifier "
                       "from pointer target type", qs);
                    sp(m[CONV_RETURN], "return discards '%s' qualifier from "
                       "pointer target type", qs);
                    conv_diag(&x, RK_WARN, DW_DISCARDED_ARRAY_QUALIFIERS, m,
                              false);
                }
            } else if (c->opt.pedantic &&
                       ((is_void(c, ttl) && is_func(c, ttr)) ||
                        (is_void(c, ttr) && !x.npc && is_func(c, ttl)))) {
                sp(m[CONV_ARG], "ISO C forbids passing argument %d of '%s' "
                   "between function pointer and 'void *'", pn, fn);
                sp(m[CONV_ASSIGN], "ISO C forbids assignment between function "
                   "pointer and 'void *'");
                sp(m[CONV_INIT], "ISO C forbids initialization between "
                   "function pointer and 'void *'");
                sp(m[CONV_RETURN], "ISO C forbids return between function "
                   "pointer and 'void *'");
                conv_diag(&x, RK_PED, DW_PEDANTIC, m, true);
            } else if (!is_func(c, ttr) && !is_func(c, ttl)) {
                unsigned warn_ped = TYPE_QUALS(ttr) & qnoat & ~TYPE_QUALS(ttl);
                unsigned warn_q = TYPE_QUALS(ttr) & qnoat &
                                  ~gq(c, ttl);
                /* the qualifiers of an array target live on its elements */
                unsigned miss = gq(c, ttr) & ~gq(c, ttl);
                if (is_array(c, ttl))
                    warn_ped = gq(c, ttr) & qnoat & ~gq(c, ttl);
                /* gcc compares a void source with the array type's own
                 * qualifiers: const void * -> const T (*)[N] discards */
                if (rvoid && is_array(c, ttl) && !lvoid) {
                    miss = TYPE_QUALS(ttr);
                    warn_q = warn_ped = miss & qnoat;
                }
                if (warn_q || (warn_ped && c->opt.pedantic)) {
                    const char *qs = qual_str(q, miss);
                    sp(m[CONV_ARG], "passing argument %d of '%s' discards '%s' "
                       "qualifier from pointer target type", pn, fn, qs);
                    sp(m[CONV_ASSIGN], "assignment discards '%s' qualifier from "
                       "pointer target type", qs);
                    sp(m[CONV_INIT], "initialization discards '%s' qualifier "
                       "from pointer target type", qs);
                    sp(m[CONV_RETURN], "return discards '%s' qualifier from "
                       "pointer target type", qs);
                    conv_diag(&x, RK_PED, DW_DISCARDED_QUALIFIERS, m, true);
                } else if (warn_ped) {
                    /* pedwarn_c11 before C2X: only when pedantic (above) */
                } else if (is_void(c, ttl) || is_void(c, ttr) || target_cmp) {
                    /* a mismatch in signedness is ignored */
                } else if (diag_enabled(c->diag, DW_POINTER_SIGN)) {
                    sp(m[CONV_ARG], "pointer targets in passing argument %d of "
                       "'%s' differ in signedness", pn, fn);
                    sp(m[CONV_ASSIGN], "pointer targets in assignment from %s to "
                       "%s differ in signedness", R, T);
                    sp(m[CONV_INIT], "pointer targets in initialization of %s "
                       "from %s differ in signedness", T, R);
                    sp(m[CONV_RETURN], "pointer targets in returning %s from a "
                       "function with return type %s differ in signedness", R, T);
                    conv_diag(&x, RK_PED, DW_POINTER_SIGN, m, true);
                }
            } else if (is_func(c, ttl) && is_func(c, ttr)) {
                unsigned lq = TYPE_QUALS(ttl) & qnoat, rq = TYPE_QUALS(ttr) & qnoat;
                if (lq & ~rq) {
                    const char *qs = fqual_str(q, lq & ~rq);
                    sp(m[CONV_ARG], "passing argument %d of '%s' makes '%s' "
                       "qualified function pointer from unqualified", pn, fn, qs);
                    sp(m[CONV_ASSIGN], "assignment makes '%s' qualified "
                       "function pointer from unqualified", qs);
                    sp(m[CONV_INIT], "initialization makes '%s' qualified "
                       "function pointer from unqualified", qs);
                    sp(m[CONV_RETURN], "return makes '%s' qualified function "
                       "pointer from unqualified", qs);
                    conv_diag(&x, RK_PED, DW_DISCARDED_QUALIFIERS, m, true);
                }
            }
            /* C++ has no implicit void * -> T *; a null pointer constant
             * (NULL is usually (void *) 0) is tolerated */
            if (rvoid && !x.npc && !lvoid && cexpr_cxx_compat(c, expr))
                cwarn(c, ctx == CONV_ASSIGN ? x.loc
                      : c->u->toks[first_tok(c, expr)].exp
                      ? c->u->toks[first_tok(c, expr)].exp : x.loc,
                      DW_CPP_COMPAT,
                      "request for implicit conversion "
                      "from %s to %s not permitted in C++", type_q(TT, rt),
                      type_q(TT, lt));
        } else {
            T = type_q(TT, lt);
            R = type_q(TT, rt);
            sp(m[CONV_ARG], "passing argument %d of '%s' from incompatible "
               "pointer type", pn, fn);
            if (bname) {
                sp(m[CONV_ASSIGN], "assignment to %s from pointer to '%s' with "
                   "incompatible type %s", T, bname, R);
                sp(m[CONV_INIT], "initialization of %s from pointer to '%s' "
                   "with incompatible type %s", T, bname, R);
                sp(m[CONV_RETURN], "returning pointer to '%s' of type %s from "
                   "a function with incompatible type %s", bname, R, T);
            } else {
                sp(m[CONV_ASSIGN], "assignment to %s from incompatible pointer "
                   "type %s", T, R);
                sp(m[CONV_INIT], "initialization of %s from incompatible "
                   "pointer type %s", T, R);
                sp(m[CONV_RETURN], "returning %s from a function with "
                   "incompatible return type %s", R, T);
            }
            conv_diag(&x, RK_PED, DW_INCOMPATIBLE_POINTER_TYPES, m, true);
        }
        return true;
    }
    if (kl == TY_PTR && gcc_integer(c, cr)) {
        if (!x.npc) {
            T = type_q(TT, lt);
            R = type_q(TT, rt);
            sp(m[CONV_ARG], "passing argument %d of '%s' makes pointer from "
               "integer without a cast", pn, fn);
            sp(m[CONV_ASSIGN], "assignment to %s from %s makes pointer from "
               "integer without a cast", T, R);
            sp(m[CONV_INIT], "initialization of %s from %s makes pointer from "
               "integer without a cast", T, R);
            sp(m[CONV_RETURN], "returning %s from a function with return type "
               "%s makes pointer from integer without a cast", R, T);
            conv_diag(&x, RK_PED, DW_INT_CONVERSION, m, true);
        }
        return true;
    }
    if (gcc_integer(c, cl) && kr == TY_PTR) {
        T = ctx == CONV_ASSIGN ? bf_tstr(c, lt, ci->lhs_bits) : type_q(TT, lt);
        R = type_q(TT, rt);
        sp(m[CONV_ARG], "passing argument %d of '%s' makes integer from "
           "pointer without a cast", pn, fn);
        sp(m[CONV_ASSIGN], "assignment to %s from %s makes integer from "
           "pointer without a cast", T, R);
        sp(m[CONV_INIT], "initialization of %s from %s makes integer from "
           "pointer without a cast", T, R);
        sp(m[CONV_RETURN], "returning %s from a function with return type %s "
           "makes integer from pointer without a cast", R, T);
        conv_diag(&x, RK_PED, DW_INT_CONVERSION, m, true);
        return true;
    }
    if (kl == TY_BOOL && kr == TY_PTR) {
        /* convert_for_assignment: c_objc_common_truthvalue_conversion */
        cexpr_truth_warn(c, expr, cinput_loc(c, c->nodes[expr].tok));
        return true;
    }

    /* C23 nullptr_t: any value converts to a pointer or bool, only a null
     * pointer constant converts to it */
    if (kr == TY_NULLPTR && (kl == TY_PTR || kl == TY_BOOL))
        return true;
    if (kl == TY_NULLPTR && x.npc)
        return true;

incompatible:
    T = type_q(TT, lt);
    R = type_q(TT, rt);
    switch (ctx) {
    case CONV_ARG: {
        Diagnostic *d;
        if (ci->warnopt)
            d = cwarn_d(c, DL_WARNING, x.eloc, ci->warnopt, "incompatible type "
                        "for argument %d of '%s'", pn, fn);
        else
            d = cerror_d(c, x.eloc, "incompatible type for argument %d of '%s'",
                         pn, fn);
        cnote(c, d, conv_note_loc(&x), "expected %s but argument is of type %s",
              type_q(TT, lt), type_q(TT, rt));
        break;
    }
    case CONV_ASSIGN:
        if (ci->warnopt)
            cwarn(c, x.eloc, DW_NONE, "incompatible types when assigning to type %s "
                  "from type %s", T, R);
        else
            cerror(c, x.eloc, "incompatible types when assigning to type %s "
                   "from type %s", T, R);
        break;
    case CONV_INIT:
        if (ci->warnopt)
            cwarn(c, x.loc, DW_NONE, "incompatible types when initializing type %s "
                  "using type %s", T, R);
        else
            cerror(c, x.loc, "incompatible types when initializing type %s "
                   "using type %s", T, R);
        break;
    default:
        if (ci->warnopt)
            cwarn(c, x.loc, DW_NONE, "incompatible types when returning type %s but "
                  "%s was expected", R, T);
        else
            cerror(c, x.loc, "incompatible types when returning type %s but "
                   "%s was expected", R, T);
        break;
    }
    return false;
}

/* gcc's location of a call: EXPR_LOC_OR_LOC of the primary expression the
 * postfix operators start from. */
SrcLoc call_loc(Checker *c, uint32_t f)
{
    for (;;) {
        unsigned tg = ntag(c, f);
        uint32_t k;
        if (tg != N_CALL && tg != N_INDEX && tg != N_MEMBER_EXPR &&
            tg != N_POSTFIX)
            break;
        k = first_child(c, f);
        if (k == NO_NODE)
            break;
        f = k;
    }
    if (ntag(c, f) == N_PAREN) {
        uint32_t in = strip_paren(c, f);
        if (in != NO_NODE && ntag(c, in) != N_IDENT && ntag(c, in) != N_NUMBER &&
            ntag(c, in) != N_CHAR && ntag(c, in) != N_STRING)
            return expr_loc(c, in);
        return cnode_loc(c, f);
    }
    return cnode_loc(c, f);
}

/* A call (maybe parenthesized) of an implicitly declared function. */
static bool implicit_call(Checker *c, uint32_t e)
{
    uint32_t f, ref;
    while (e != NO_NODE && ntag(c, e) == N_PAREN)
        e = strip_paren(c, e);
    if (e == NO_NODE || ntag(c, e) != N_CALL)
        return false;
    f = first_child(c, e);
    if (f == NO_NODE || ntag(c, f) != N_IDENT)
        return false;
    if (!c->cb[f])           /* the symbol the identifier resolved to */
        return false;
    ref = c->cb[f] - 1;
    return csym(c, ref)->flags & CSF_IMPLICIT;
}

/* Where gcc reports a bad callee: a compound literal or statement
 * expression is located at its opening brace, other callees as call_loc. */
SrcLoc callee_err_loc(Checker *c, uint32_t f)
{
    uint32_t g = f, k;
    while (g != NO_NODE && ntag(c, g) == N_PAREN)
        g = strip_paren(c, g);
    if (g != NO_NODE && ntag(c, g) == N_COMPOUND_LIT) {
        uint32_t b = g - 1;
        if (c->nodes[g].size > 1 && ntag(c, b) == N_INIT_LIST)
            return cnode_loc(c, b);
    } else if (g != NO_NODE && ntag(c, g) == N_STMT_EXPR) {
        k = first_child(c, g);
        if (k != NO_NODE) {
            /* a lone expression or break/continue keeps its own location */
            uint32_t e = stmt_expr_value(c, g), it = g >= 3 ? g - 3 : NO_NODE;
            if (e != NO_NODE && cfirst(c, e) == cfirst(c, k) + 1 &&
                !implicit_call(c, e))
                return ntag(c, e) == N_PAREN ? cnode_loc(c, e)
                       : ntag(c, e) == N_CALL ? call_loc(c, e)
                       : expr_loc(c, e);
            if (it != NO_NODE && cfirst(c, it) == cfirst(c, k) + 1 &&
                (ntag(c, it) == N_BREAK || ntag(c, it) == N_CONTINUE))
                return cnode_loc(c, it);
            return cnode_loc(c, k);
        }
    }
    return call_loc(c, f);
}

/* ---- unary operators ------------------------------------------------------------ */

/* convert_lvalue_to_rvalue's check: an operand used for its value must not
 * have an incomplete (non-void) type.  Reported at its first token. */
bool rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc)
{
    TypeId t = rvt(c, i);
    if (is_void(c, t) || complete(c, t))
        return true;
    incomplete_error(c, loc, i, t);
    return false;
}

bool rvalue_ok(Checker *c, uint32_t i)
{
    return rvalue_ok_at(c, i, first_loc(c, i));
}

bool cexpr_rvalue_ok(Checker *c, uint32_t i)
{
    return rvalue_ok(c, i);
}

bool cexpr_rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc)
{
    return rvalue_ok_at(c, i, loc);
}

/* Value flags of a unary result from its operand a: see the file
 * comment. */
static void unary_value(Checker *c, uint32_t i, uint32_t a, uint64_t v,
                        bool ovf)
{
    TypeId t = c->ty[i];
    c->cv[i] = cexpr_trunc(c, t, v);
    if (c->ck[a] == K_ICE) {
        c->ck[i] = K_ICE;
        c->ef[i] |= EF_INTOPS;
        if (ovf || (c->ef[a] & EF_OVERFLOW))
            c->ef[i] |= EF_OVERFLOW;
        if (ovf && !(c->ef[a] & EF_OVERFLOW) && !inhibited(c, i, false))
            cwarn(c, cnode_loc(c, i), DW_OVERFLOW, "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, a),
                  type_q(TT, t), vstr(c, t, c->cv[i]));
        return;
    }
    c->ck[i] = K_FOLD;
    if (c->ef[a] & (EF_CST | EF_NOPCST | EF_REALCST))
        c->ef[i] |= EF_NOPCST;
    else if (c->ef[a] & EF_INTOPS)
        c->ef[i] |= EF_INTOPS;
    if (ovf && !(c->ef[a] & EF_OVERFLOW)) {
        c->ef[i] |= EF_OVERFLOW | EF_FOLDWARN;
        c->fold_pending++;
    }
}

/* gcc's readonly_error for lvalue a: the diagnostic (use 0: assignment, 1:
 * increment, 2: decrement, 3: an asm output) when a is read-only; true if
 * one was given. */
bool readonly_check(Checker *c, uint32_t a, SrcLoc loc, int use)
{
    static const char *const verb[] = { "assignment", "increment", "decrement",
                                        "" };
    /* member in a read-only object, read-only member, parameter, variable,
     * location */
    static const char *const fmt[2][5] = {
        { "%s of member '%s' in read-only object", "%s of read-only member '%s'",
          "%s of read-only parameter '%s'", "%s of read-only variable '%s'",
          "%s of read-only location '%s'" },
        { "%smember '%s' in read-only object used as 'asm' output",
          "%sread-only member '%s' used as 'asm' output",
          "%sread-only parameter '%s' use as 'asm' output",
          "%sread-only variable '%s' used as 'asm' output",
          "%sread-only location '%s' used as 'asm' output" } };
    const char *const *f = fmt[use == 3];
    TypeId t = c->ty[a];
    uint32_t s = strip_paren(c, a);
    bool ro = (tquals(c, t) & TQ_CONST) != 0;
    if (!ro && is_record(c, t)) {
        const Record *r = type_record(TT, t);
        ro = r && (r->flags & RF_CONST_MEMBER);
    }
    if (!ro && use < 3 && s != NO_NODE && ntag(c, s) == N_INDEX) {
        /* an element of a string literal: a warning, not an error (PR 27676) */
        uint32_t k[3], b;
        if (nkids(c, s, k, 3) >= 2 && (b = strip_paren(c, k[0])) != NO_NODE &&
            ntag(c, b) == N_STRING)
        {
            /* at the literal, or at a prefix operator before it */
            SrcLoc fl = first_loc(c, a);
            cwarn(c, loc < fl ? loc : fl, DW_NONE, "%s of read-only location '%s'",
                  verb[use], estr(c, a));
        }
    }
    if (!ro)
        return false;
    if (s != NO_NODE && ntag(c, s) == N_MEMBER_EXPR) {
        uint32_t base = first_child(c, s), mid = cnode_ident(c, s);
        TypeId bt = base == NO_NODE ? ERRT : c->ty[base];
        bool arrow = (c->nodes[s].flags & NF_ARROW) != 0;
        if (base != NO_NODE && arrow && is_ptr(c, rvt(c, base)))
            bt = pointee(c, rvt(c, base));
        if (mid) {
            /* the object holding the member: through anonymous members,
             * their qualifiers count (gcc's nested COMPONENT_REFs) */
            uint64_t off = 0;
            unsigned q = tquals(c, bt);
            find_field(c, bt, mid, &off, &q);
            cerror(c, loc, f[q & TQ_CONST ? 0 : 1], verb[use], cident(c, mid));
            return true;
        }
    }
    if (s != NO_NODE && ntag(c, s) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        if (ref != SYM_NONE && csym(c, ref)->kind != CS_FUNC) {
            cerror(c, loc, f[csym(c, ref)->flags & CSF_PARAM ? 2 : 3],
                   verb[use], cident(c, cnode_ident(c, s)));
            return true;
        }
    }
    cerror(c, loc, f[4], verb[use], estr(c, a));
    return true;
}

static void incdec(Checker *c, uint32_t i, uint32_t a, bool inc)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId t;
    if (node_err(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (!is_void(c, t) && !complete(c, t)) {
        incomplete_error(c, loc, a, t);
        set_err(c, i);
        return;
    }
    if (is_void(c, t)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (!(c->ef[a] & EF_LVALUE) || is_array(c, c->ty[a]) ||
        is_func(c, c->ty[a])) {
        cerror(c, loc, inc ? "lvalue required as increment operand"
                           : "lvalue required as decrement operand");
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ENUM && cexpr_cxx_compat(c, i))
        cwarn(c, loc, DW_CPP_COMPAT, inc ? "increment of enumeration value is "
              "invalid in C++" : "decrement of enumeration value is invalid in "
              "C++");
    if (tkind(c, t) == TY_BOOL)
        cwarn(c, loc, DW_BOOL_OPERATION, inc ? "increment of a boolean "
              "expression" : "decrement of a boolean expression");
    if (is_complex(c, t))
        ped(c, i, loc, "ISO C does not support '++' and '--' on complex types");
    else if (!is_ptr(c, t) && !is_int(c, t) && !is_flt(c, t) &&
             tkind(c, t) != TY_VECTOR) {
        cerror(c, loc, inc ? "wrong type argument to increment"
                           : "wrong type argument to decrement");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, t)) {
        TypeId b = pointee(c, t);
        if (!is_void(c, b) && !is_func(c, b) && !complete(c, b))
            cerror(c, loc, inc ? "increment of pointer to an incomplete type "
                   "%s" : "decrement of pointer to an incomplete type %s",
                   type_q(TT, b));
        else if ((is_func(c, b) || is_void(c, b)) &&
                 !cexpr_in_extension(c, i))
            cpedwarn(c, loc, DW_POINTER_ARITH, inc ? "wrong type argument to "
                     "increment" : "wrong type argument to decrement");
        /* gcc lowers an atomic ++/-- to a pointer + 1 sum */
        if (tquals(c, c->ty[a]) & TQ_ATOMIC)
            ptr_arith_ok(c, i, loc, t);
    }
    if (readonly_check(c, a, loc, inc ? 1 : 2)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = EF_SIDE | (c->ef[strip_paren(c, a)] & EF_BFPROMOTE);   /* a narrow bit-field's value promotes */
}

static void addr_of(Checker *c, uint32_t i, uint32_t a)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t s = strip_paren(c, a);
    TypeId t = c->ty[a];
    if (node_err(c, a)) {
        set_err(c, i);
        return;
    }
    if (is_void(c, t) && !tquals(c, t) &&
        !(ntag(c, s) == N_UNARY && npunct(c, s) == P_STAR))
        cpedwarn(c, loc, DW_NONE, "taking address of expression of type 'void'");
    if (ntag(c, s) == N_UNARY && npunct(c, s) == P_STAR) {
        uint32_t p = first_child(c, s);
        c->ty[i] = rvt(c, p);
        c->ck[i] = c->ck[p];
        c->cv[i] = c->cv[p];
        c->cb[i] = c->cb[p];
        c->ef[i] = c->ef[p] & (EF_SIDE | EF_INTOPS | EF_NOPCST | EF_CST);
        return;
    }
    if (!is_func(c, t) && !(c->ef[a] & EF_LVALUE)) {
        cerror(c, loc, "lvalue required as unary '&' operand");
        set_err(c, i);
        return;
    }
    if (c->ef[a] & EF_BITFIELD) {
        uint32_t m = s;
        while (m != NO_NODE && ntag(c, m) == N_UNARY &&
               tckw(c, c->nodes[m].tok) == CK_EXTENSION)
            m = strip_paren(c, first_child(c, m));
        cerror(c, loc, "cannot take address of bit-field '%s'",
               m != NO_NODE && ntag(c, m) == N_MEMBER_EXPR
                   ? cident(c, cnode_ident(c, m)) : "");
        set_err(c, i);
        return;
    }
    if (sso_scalar_ref(c, s)) {
        cerror(c, loc, "cannot take address of scalar with reverse storage "
               "order");
        set_err(c, i);
        return;
    }
    if (sso_arr(c, s))
        cwarn(c, loc, DW_SCALAR_STORAGE_ORDER, "address of array with reverse "
              "scalar storage order requested");
    if (c->ef[a] & EF_REGISTER) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        bool global = ref != SYM_NONE && !(ref & SYM_LOCAL);
        cerror(c, cinput_loc(c, last_tok(c, i) + 1),
               global ? "address of global register variable '%s' requested"
                      : "address of register variable '%s' requested",
               cident(c, cnode_ident(c, s)));
        set_err(c, i);
        return;
    }
    c->ty[i] = type_ptr(TT, t);
    c->ef[i] = c->ef[a] & EF_PROP;
    if (c->ef[a] & EF_ADDRLV) {
        c->ck[i] = K_ADDR;
        c->cb[i] = c->cb[a];
        c->cv[i] = c->cv[a];
    }
}

/* ---- -Wstrict-aliasing (gcc's c-family strict_aliasing_warning) ---------- */

static bool strict_alias_on(Checker *c)
{
    char o = c->opt.opt_level;
    if (c->opt.strict_alias)
        return c->opt.strict_alias == 1;
    return o == '2' || o == '3' || o == 's' || o == 'z' || o == 'f';
}

/* Looks through parentheses and pointer conversions for the address of an
 * object (&decl, &a.b, &a[i], &p->m) or an array that decays: the node whose
 * address it is, with the object's type in *ot and the outermost conversion
 * in *first.  &*p and &p[i] are not: gcc folds them to p. */
static uint32_t alias_base(Checker *c, uint32_t e, TypeId *ot, uint32_t *first)
{
    uint32_t k[2], x;
    bool ok;
    *first = NO_NODE;
    for (;;) {
        e = strip_paren(c, e);
        if (e == NO_NODE || node_err(c, e))
            return NO_NODE;
        if (ntag(c, e) == N_CAST && nkids(c, e, k, 2) == 2 &&
            is_ptr(c, c->ty[e]) &&
            (is_ptr(c, c->ty[k[1]]) || is_array(c, c->ty[k[1]]))) {
            if (*first == NO_NODE)
                *first = e;
            e = k[1];
            continue;
        }
        break;
    }
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_AMP) {
        x = first_child(c, e);
        x = x == NO_NODE ? x : strip_paren(c, x);
    } else if (is_array(c, c->ty[e])) {
        x = e;
    } else {
        return NO_NODE;
    }
    if (x == NO_NODE || node_err(c, x))
        return NO_NODE;
    switch (ntag(c, x)) {
    case N_IDENT:
        ok = (c->ef[x] & EF_LVALUE) && !is_func(c, c->ty[x]);
        break;
    case N_MEMBER_EXPR:
        ok = true;
        break;
    case N_UNARY: {             /* __real__ x, __imag__ x */
        uint32_t b = first_child(c, x);
        ok = (tckw(c, c->nodes[x].tok) == CK_REAL ||
              tckw(c, c->nodes[x].tok) == CK_IMAG) && b != NO_NODE &&
             ntag(c, strip_paren(c, b)) == N_IDENT;
        break;
    }
    case N_INDEX: {
        uint32_t b = first_child(c, x);
        ok = b != NO_NODE && is_array(c, c->ty[b]);
        break;
    }
    default:
        ok = false;
        break;
    }
    if (!ok || (is_record(c, c->ty[x]) && !complete(c, c->ty[x])))
        return NO_NODE;
    *ot = c->ty[x];
    return e;
}

/* pt: the pointer type, obj: the type of the object it points into.
 * Level 2 warns at the cast, level 3 at the dereference. */
static void alias_warn(Checker *c, SrcLoc loc, TypeId obj, TypeId pt,
                       bool at_cast)
{
    TypeId tg = pointee(c, pt);
    int r;
    if (is_void(c, tg) || type_ptr_may_alias(TT, pt))
        return;
    if (!complete(c, tg)) {
        if (at_cast)
            cwarn(c, loc, DW_STRICT_ALIASING_V, "type-punning to incomplete "
                  "type might break strict-aliasing rules");
        return;
    }
    r = type_alias_rel(TT, obj, tg);
    if (r == AL_DISJOINT)
        cwarn(c, loc, DW_STRICT_ALIASING_V, "dereferencing type-punned "
              "pointer will break strict-aliasing rules");
    else if (r == AL_MAY && at_cast)
        cwarn(c, loc, DW_STRICT_ALIASING_V, "dereferencing type-punned "
              "pointer might break strict-aliasing rules");
}

static bool alias_wanted(Checker *c, int lvl)
{
    return strict_alias_on(c) && diag_enabled(c->diag, DW_STRICT_ALIASING_V) &&
           diag_option_level(c->diag, DW_STRICT_ALIASING_V, 3) == lvl;
}

/* *p, p->m, p[0] with p = (T *)&obj (level 3); loc: where gcc reports a
 * subscript (else at the conversion). */
void alias_deref(Checker *c, uint32_t p, bool use_loc, SrcLoc loc)
{
    TypeId ot;
    uint32_t first, b;
    if (!alias_wanted(c, 3))
        return;
    b = alias_base(c, p, &ot, &first);
    if (b == NO_NODE)
        return;
    alias_warn(c, !use_loc && first != NO_NODE ? cnode_loc(c, first) : loc, ot,
               rvt(c, p), false);
}

/* (T *)&obj itself (level 2). */
void alias_cast(Checker *c, uint32_t a, TypeId pt)
{
    TypeId ot;
    uint32_t first, b;
    if (!alias_wanted(c, 2))
        return;
    b = alias_base(c, a, &ot, &first);
    if (b == NO_NODE)
        return;
    alias_warn(c, cnode_loc(c, strip_paren(c, a)), ot, pt, true);
}

static void deref(Checker *c, uint32_t i, uint32_t a)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t s = strip_paren(c, a);
    TypeId t, b;
    if (node_err(c, a) || !rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (!is_ptr(c, t)) {
        cerror(c, loc, "invalid type argument of unary '*' (have %s)",
               type_q(TT, t));
        set_err(c, i);
        return;
    }
    b = pointee(c, t);
    if (ntag(c, s) == N_UNARY && npunct(c, s) == P_AMP) {
        uint32_t x = first_child(c, s);
        if (x != NO_NODE && c->ty[x] == b) {
            copy_node(c, i, x);
            return;
        }
    }
    /* *&&label folds to the label (a void) */
    if (is_void(c, b) && !inhibited(c, i, false) &&
        ntag(c, s) != N_ADDR_LABEL)
        cwarn(c, loc, DW_NONE, "dereferencing 'void *' pointer");
    alias_deref(c, a, false, loc);
    c->ty[i] = b;
    c->ef[i] = EF_LVALUE | (c->ef[a] & EF_PROP);
    if (tquals(c, b) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (c->ck[a] == K_ADDR) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = c->cb[a];
        c->cv[i] = c->cv[a];
        addr_rvalue(c, i);
    }
}

/* warn_about_bool_operation: a bool or truth-valued operand (through the
 * right operand of commas). */
static bool bool_operand(Checker *c, uint32_t n)
{
    uint32_t k[3];
    for (n = strip_paren(c, n); ntag(c, n) == N_BINARY &&
         npunct(c, n) == P_COMMA && nkids(c, n, k, 3) == 2;
         n = strip_paren(c, k[1]))
        ;
    return is_boolish(c, n);
}

static void arith_unary(Checker *c, uint32_t i, uint32_t a, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId t;
    if (op == P_PLUS && !cin_system(c, loc))
        cwarn(c, loc, DW_TRADITIONAL, "traditional C rejects the unary plus "
              "operator");
    if (node_err(c, a) || !rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (is_void(c, t)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_VECTOR && op == P_TILDE &&
        is_flt(c, vec_elem(c, t))) {
        cerror(c, loc, "wrong type argument to bit-complement");
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_VECTOR && op != P_BANG) {
        /* vector operands: the result has the vector's type */
        c->ty[i] = TYPE_UNQUAL(type_canon(TT, t));
        c->ef[i] = c->ef[a] & EF_PROP;
        return;
    }
    switch (op) {
    case P_PLUS: case P_MINUS:
        if (!is_arith(c, t)) {
            cerror(c, loc, op == P_PLUS ? "wrong type argument to unary plus"
                                        : "wrong type argument to unary minus");
            set_err(c, i);
            return;
        }
        break;
    case P_TILDE:
        if (is_complex(c, t)) {
            ped(c, i, loc, "ISO C does not support '~' for complex conjugation");
        } else if (!is_int(c, t)) {
            cerror(c, loc, "wrong type argument to bit-complement");
            set_err(c, i);
            return;
        } else if (bool_operand(c, a)) {
            /* -Wbool-operation (-Wall) */
            Diagnostic *d = cwarn_d(c, DL_WARNING, loc, DW_BOOL_OPERATION,
                                    "'~' on a boolean expression");
            cnote(c, d, loc, "did you mean to use logical not?");
        }
        break;
    default: /* '!' */
        if (!is_scalar(c, t)) {
            cerror(c, loc, "wrong type argument to unary exclamation mark");
            set_err(c, i);
            return;
        }
        cexpr_truth_warn(c, a, loc);
        c->ty[i] = TYPE_B(INT);
        c->ef[i] = c->ef[a] & EF_PROP;
        {
            int tv = truth(c, a, true);
            if (tv < 0) {
                if (c->ef[a] & EF_INTOPS)
                    c->ef[i] |= EF_INTOPS;
                return;
            }
            /* gcc's nullptr is an INTEGER_CST */
            if (c->ck[a] == K_ICE || (c->ck[a] == K_ADDR &&
                                      tkind(c, rvt(c, a)) == TY_NULLPTR)) {
                c->ck[i] = K_ICE;
                c->cv[i] = !tv;
                c->ef[i] |= EF_INTOPS;
                if (c->ef[a] & EF_OVERFLOW)
                    c->ef[i] |= EF_OVERFLOW;
                return;
            }
            c->ck[i] = K_FOLD;
            c->cv[i] = !tv;
            if (truth(c, a, false) >= 0)
                c->ef[i] |= EF_NOPCST;
            else if (c->ef[a] & EF_INTOPS)
                c->ef[i] |= EF_INTOPS;
        }
        return;
    }
    c->ty[i] = promoted(c, a);
    c->ef[i] = c->ef[a] & EF_PROP;
    if ((c->ef[a] & EF_CPLXCST) && is_complex(c, c->ty[i]) &&
        (op == P_MINUS || op == P_PLUS || op == P_TILDE)) {
        TypeId tc = cplx_comp(c, c->ty[i]);
        long double re = c->fv.data[c->cv[a]], im = c->fv.data[c->cv[a] + 1],
                    ore, oim;
        if (op == P_MINUS) {
            re = -re;
            im = -im;
        } else if (op == P_TILDE) {
            im = -im;
        }
        if (part_conv(c, tc, tc, re, &ore) && part_conv(c, tc, tc, im, &oim))
            cplx_set(c, i, ore, oim);
        return;
    }
    if (c->ck[a] == K_FLOAT) {
        long double f = c->fv.data[c->cv[a]];
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, op == P_MINUS ? -f : f);
        return;
    }
    if (has_ival(c, a)) {
        uint64_t v = c->cv[a];
        bool ovf = false;
        TypeId pt = c->ty[i];
        if (op == P_MINUS || op == P_TILDE) {
            unsigned fl;
            v = ia_unop(op, cx_ia_ty(c, pt), cexpr_trunc(c, pt, v), &fl);
            ovf = (fl & IA_OVF) && int_bits(c, pt) <= 64;
        }
        if (op == P_PLUS) {
            /* no operation: the operand's value (not an lvalue) */
            c->ck[i] = c->ck[a];
            c->cv[i] = cexpr_trunc(c, pt, v);
            c->ef[i] |= c->ef[a] & (EF_INTOPS | EF_CST | EF_NOPCST |
                                    EF_OVERFLOW);
            return;
        }
        unary_value(c, i, a, v, ovf);
        return;
    }
    if (c->ef[a] & EF_INTOPS)
        c->ef[i] |= EF_INTOPS;
}

static void real_imag(Checker *c, uint32_t i, uint32_t a, bool real)
{
    TypeId t;
    if (node_err(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (is_complex(c, t)) {
        c->ty[i] = type_qual(type_base(TT, type_canon(TT, t)),
                             tquals(c, c->ty[a]));
        c->ef[i] = c->ef[a] & (EF_LVALUE | EF_SIDE);
        if (c->ef[a] & EF_CPLXCST) {
            long double v = c->fv.data[c->cv[a] + (real ? 0 : 1)];
            TypeId tc = cplx_comp(c, t);
            if (is_flt(c, tc)) {
                c->ck[i] = K_FLOAT;
                c->cv[i] = fpush(c, v);
            } else if (is_int(c, tc) && int_bits(c, tc) <= 64) {
                c->ck[i] = K_FOLD;
                c->cv[i] = cexpr_trunc(c, tc, cplx_u(v));
                c->ef[i] |= EF_CST;
            }
        }
        return;
    }
    if (!is_int(c, t) && !is_flt(c, t)) {
        cerror(c, cnode_loc(c, i), "wrong type argument to %s",
               real ? "__real" : "__imag");
        set_err(c, i);
        return;
    }
    if (real) {
        copy_node(c, i, a);
        c->ty[i] = t;
        c->ef[i] &= ~(EF_LVALUE | EF_ADDRLV | EF_BITFIELD);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = c->ef[a] & EF_PROP;
    if (is_flt(c, t)) {
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, 0);
    } else {
        c->cv[i] = 0;
        if (c->ef[a] & EF_SIDE) {   /* gcc: a discarded side effect is not constant */
            c->ck[i] = K_NONE;
            return;
        }
        c->ck[i] = c->ck[a] == K_ICE ? K_ICE : K_FOLD;
        c->ef[i] |= c->ck[a] == K_ICE ? EF_INTOPS : EF_NOPCST;
    }
}

void e_unary(Checker *c, uint32_t i)
{
    uint32_t a = first_child(c, i);
    int op = npunct(c, i);
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    switch (op) {
    case P_INC: case P_DEC:
        incdec(c, i, a, op == P_INC);
        return;
    case P_AMP:
        addr_of(c, i, a);
        return;
    case P_STAR:
        deref(c, i, a);
        return;
    case P_PLUS: case P_MINUS: case P_TILDE: case P_BANG:
        arith_unary(c, i, a, op);
        return;
    default:
        break;
    }
    switch (tckw(c, c->nodes[i].tok)) {
    case CK_EXTENSION:
        copy_node(c, i, a);
        return;
    case CK_REAL:
        real_imag(c, i, a, true);
        return;
    case CK_IMAG:
        real_imag(c, i, a, false);
        return;
    default:
        set_err(c, i);
        return;
    }
}

void e_postfix(Checker *c, uint32_t i)
{
    uint32_t a = first_child(c, i);
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    incdec(c, i, a, npunct(c, i) == P_INC);
}

