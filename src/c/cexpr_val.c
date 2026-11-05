/* cexpr_val.c - value helpers: types, integer, floating and complex constants; constexpr objects
 * (split from cexpr.c; see its header comment). */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- small helpers ----------------------------------------------------- */

bool cexpr_is_extension(Checker *c, uint32_t i)
{
    return ntag(c, i) == N_UNARY && tckw(c, c->nodes[i].tok) == CK_EXTENSION;
}

/* ---- types --------------------------------------------------------------- */

TypeId elem_of(Checker *c, TypeId t);

TypeId cexpr_rvalue_type(Checker *c, uint32_t i)
{
    TypeId t = c->ty[i];
    switch (tkind(c, t)) {
    case TY_ARRAY: case TY_VLA:
        return type_ptr(TT, elem_of(c, t));
    case TY_FUNC:
        return type_ptr(TT, t);
    default:
        return unqual(c, t);
    }
}

/* Preference among floating types of one precision. */
static int fl_pref(TypeKind k)
{
    if (k == TY_FLOAT32X || k == TY_FLOAT64X)
        return 0;
    if (k == TY_FLOAT || k == TY_DOUBLE || k == TY_LDOUBLE)
        return 1;
    return 2;
}

/* The usual arithmetic conversions of two promoted arithmetic types. */
TypeId common_type(Checker *c, TypeId a, TypeId b)
{
    TypeKind ka, kb;
    a = mainv(c, a);
    b = mainv(c, b);
    if (a == b)
        return a;
    if (is_complex(c, a) || is_complex(c, b)) {
        TypeId ra = is_complex(c, a) ? mainv(c, type_base(TT, a)) : a;
        TypeId rb = is_complex(c, b) ? mainv(c, type_base(TT, b)) : b;
        TypeId r = common_type(c, ra, rb);
        if (is_complex(c, a) && mainv(c, type_base(TT, a)) == r)
            return a;
        if (is_complex(c, b) && mainv(c, type_base(TT, b)) == r)
            return b;
        return type_complex(TT, r);
    }
    ka = tkind(c, a);
    kb = tkind(c, b);
    if (is_flt(c, a) || is_flt(c, b)) {
        if (!is_flt(c, b))
            return a;
        if (!is_flt(c, a))
            return b;
        if (float_prec(c, ka) != float_prec(c, kb))
            return float_prec(c, ka) > float_prec(c, kb) ? a : b;
        /* same precision: prefer _FloatN, then long double, double, and
         * last _FloatNx */
        if (fl_pref(ka) != fl_pref(kb))
            return fl_pref(ka) > fl_pref(kb) ? a : b;
        if (ka >= TY_FLOAT32 && ka <= TY_FLOAT64X)
            return a;
        if (ka == TY_LDOUBLE || kb == TY_LDOUBLE)
            return TYPE_B(LDOUBLE);
        if (ka == TY_DOUBLE || kb == TY_DOUBLE)
            return TYPE_B(DOUBLE);
        return a;
    }
    if (int_bits(c, a) != int_bits(c, b))
        return int_bits(c, a) > int_bits(c, b) ? a : b;
    if (ka == TY_ULLONG || kb == TY_ULLONG)
        return TYPE_B(ULLONG);
    if (ka == TY_LLONG || kb == TY_LLONG)
        return !is_signed(c, a) || !is_signed(c, b) ? TYPE_B(ULLONG)
                                                    : TYPE_B(LLONG);
    if (ka == TY_ULONG || kb == TY_ULONG)
        return TYPE_B(ULONG);
    if (ka == TY_LONG || kb == TY_LONG)
        return !is_signed(c, a) || !is_signed(c, b) ? TYPE_B(ULONG)
                                                    : TYPE_B(LONG);
    return !is_signed(c, a) ? a : b;
}

/* ---- integer values ------------------------------------------------------ */

IaTy cx_ia_ty(Checker *c, TypeId t)
{
    unsigned bits = int_bits(c, t);
    return (IaTy){(uint8_t)(bits == 0 || bits > 64 ? 64 : bits), is_signed(c, t),
                  tkind(c, t) == TY_BOOL};
}

uint64_t cexpr_trunc(Checker *c, TypeId t, uint64_t v)
{
    if (tkind(c, t) != TY_BOOL && !is_int(c, t) && !is_ptr(c, t))
        return v;
    return ia_canon(cx_ia_ty(c, t), v);
}

int64_t cexpr_sval(Checker *c, uint32_t i)
{
    return (int64_t)c->cv[i];
}

bool cexpr_fits(Checker *c, uint64_t v, TypeId from, TypeId to)
{
    /* a 128-bit value reaching here is an unsigned-range literal */
    bool neg = is_int(c, from) && is_signed(c, from) && (int64_t)v < 0 &&
               int_bits(c, from) <= 64;
    unsigned bits = int_bits(c, to);
    if (tkind(c, to) == TY_BOOL)
        return v <= 1 && !neg;
    if (bits == 0 || bits > 64)
        bits = 64;
    if (is_int(c, to) && is_signed(c, to)) {
        int64_t lo, hi;
        if (bits == 64) {
            /* an unsigned value above INT64_MAX does not fit */
            return neg || (int64_t)v >= 0;
        }
        hi = (int64_t)((UINT64_C(1) << (bits - 1)) - 1);
        lo = -hi - 1;
        if (!neg && v > (uint64_t)hi)
            return false;
        return (int64_t)v >= lo;
    }
    if (neg)
        return false;
    return bits == 64 || v < (UINT64_C(1) << bits);
}

/* Does v (per its type t) have the value zero / is it negative? */
bool ival_neg(Checker *c, TypeId t, uint64_t v)
{
    return is_int(c, t) && is_signed(c, t) && (int64_t)v < 0;
}

/* ---- floating values ----------------------------------------------------- */

FpTy fp_ty(Checker *c, TypeId t)
{
    switch (tkind(c, t)) {
    case TY_FLOAT: case TY_FLOAT32:
        return FP_F32;
    case TY_DOUBLE: case TY_FLOAT64: case TY_FLOAT32X:
        return FP_F64;
    case TY_LDOUBLE:
        return c->tgt->long_double == LD_IEEE64 ? FP_F64 : FP_LD;
    default:
        return FP_LD;
    }
}

long double fround(Checker *c, TypeId t, long double v)
{
    switch (fp_ty(c, t)) {
    case FP_F32:
        return (float)v;
    case FP_F64:
        return (double)v;
    default:
        return v;
    }
}

/* The value of node i as a long double, if it is an arithmetic constant. */
bool fval(Checker *c, uint32_t i, long double *out)
{
    switch (c->ck[i]) {
    case K_FLOAT:
        *out = c->fv.data[c->cv[i]];
        return true;
    case K_ICE: case K_FOLD:
        if (!is_int(c, rvt(c, i)))
            return false;
        *out = ival_neg(c, rvt(c, i), c->cv[i]) ? (long double)(int64_t)c->cv[i]
                                               : (long double)c->cv[i];
        return true;
    default:
        return false;
    }
}

/* Does node i have a known integer value? */
bool has_ival(Checker *c, uint32_t i)
{
    return (c->ck[i] == K_ICE || c->ck[i] == K_FOLD) && is_int(c, rvt(c, i));
}

/* ---- complex constants ----------------------------------------------------
 * A complex constant has EF_CPLXCST (its ck stays K_NONE); the real and
 * imaginary parts, rounded to the component type, are fv[cv] and fv[cv + 1].
 * Integer parts are held as exact long doubles. */
bool is_decimal_flt(Checker *c, TypeId t);

TypeId cplx_comp(Checker *c, TypeId t)
{
    return mainv(c, type_base(TT, type_canon(TT, t)));
}

void cplx_set(Checker *c, uint32_t i, long double re, long double im)
{
    c->cv[i] = fpush(c, re);
    fpush(c, im);
    c->ef[i] |= EF_CPLXCST;
}

uint64_t cplx_u(long double v)
{
    return v < 0 ? (uint64_t)(int64_t)v : (uint64_t)v;
}

long double cplx_ld(Checker *c, TypeId t, uint64_t u)
{
    return ival_neg(c, t, u) ? (long double)(int64_t)u : (long double)u;
}

/* Part v of component type from, converted to component type to. */
bool part_conv(Checker *c, TypeId from, TypeId to, long double v,
                      long double *out)
{
    uint64_t u;
    if (is_decimal_flt(c, from) || is_decimal_flt(c, to))
        return false;
    if (is_flt(c, to)) {
        *out = fround(c, to, v);
        return true;
    }
    if (!is_int(c, to) || int_bits(c, to) > 64 || tkind(c, to) == TY_BOOL)
        return false;
    if (is_int(c, from)) {
        if (int_bits(c, from) > 64)
            return false;
        u = cexpr_trunc(c, to, cplx_u(v));
    } else if (!float_to_int(c, v, to, &u)) {
        return false;
    }
    *out = cplx_ld(c, to, u);
    return true;
}

/* Node n as a constant pair: a complex constant, or a real one (imaginary
 * part 0).  ct: the component type. */
bool cplx_get(Checker *c, uint32_t n, long double *re, long double *im,
                     TypeId *ct)
{
    TypeId t;
    long double f;
    if (n == NO_NODE)
        return false;
    t = rvt(c, n);
    if (is_complex(c, t)) {
        if (!(c->ef[n] & EF_CPLXCST))
            return false;
        *re = c->fv.data[c->cv[n]];
        *im = c->fv.data[c->cv[n] + 1];
        *ct = cplx_comp(c, t);
        return true;
    }
    if ((!is_int(c, t) && !is_flt(c, t)) || (is_int(c, t) && int_bits(c, t) > 64) ||
        !fval(c, n, &f))
        return false;
    *re = f;
    *im = 0;
    *ct = mainv(c, t);
    return true;
}

/* Node a converted to complex type to. */
bool cplx_conv(Checker *c, uint32_t a, TypeId to, long double *re,
                      long double *im)
{
    long double r, m;
    TypeId ct, tc = cplx_comp(c, to);
    return cplx_get(c, a, &r, &m, &ct) && part_conv(c, ct, tc, r, re) &&
           part_conv(c, ct, tc, m, im);
}

/* (a + bi) op (c + di) in component type tc. */
bool cplx_arith(Checker *c, int op, TypeId tc, long double ar,
                       long double ai, long double br, long double bi,
                       long double *rr, long double *ri)
{
    long double x, y;
#define CR(v) fround(c, tc, (v))
    if (is_int(c, tc)) {
        uint64_t p = cplx_u(ar), q = cplx_u(ai), r = cplx_u(br),
                 s = cplx_u(bi), u, w;
        switch (op) {
        case P_PLUS: u = p + r; w = q + s; break;
        case P_MINUS: u = p - r; w = q - s; break;
        case P_STAR: u = p * r - q * s; w = p * s + q * r; break;
        default: return false;
        }
        *rr = cplx_ld(c, tc, cexpr_trunc(c, tc, u));
        *ri = cplx_ld(c, tc, cexpr_trunc(c, tc, w));
        return true;
    }
    if (!is_flt(c, tc) || is_decimal_flt(c, tc))
        return false;
    switch (op) {
    case P_PLUS: x = CR(ar + br); y = CR(ai + bi); break;
    case P_MINUS: x = CR(ar - br); y = CR(ai - bi); break;
    case P_STAR:
        x = CR(CR(ar * br) - CR(ai * bi));
        y = CR(CR(ar * bi) + CR(ai * br));
        break;
    default: {
        long double d = CR(CR(br * br) + CR(bi * bi));
        if (d == 0)
            return false;
        x = CR(CR(CR(ar * br) + CR(ai * bi)) / d);
        y = CR(CR(CR(ai * br) - CR(ar * bi)) / d);
    }
    }
#undef CR
    if (!isfinite(x) || !isfinite(y))
        return false;
    *rr = x;
    *ri = y;
    return true;
}

/* gcc: an INTEGER_CST once C_MAYBE_CONST_EXPRs are removed. */
bool is_intcst(Checker *c, uint32_t i)
{
    return c->ck[i] == K_ICE || (c->ck[i] == K_FOLD && (c->ef[i] & EF_CST));
}

/* Does the expression read a const object that -O folded to its value?  Such
 * an expression is no integer constant expression, hence no null pointer
 * constant (the tree is postorder: the subtree ends at i). */
bool constvar_in(Checker *c, uint32_t i)
{
    uint32_t j;
    for (j = i + 1 - c->nodes[i].size; j <= i; j++)
        if (c->nodes[j].tag == N_IDENT && c->ck[j] == K_FOLD &&
            (c->ef[j] & EF_CST))
            return true;
    return false;
}

/* EXPR_INT_CONST_OPERANDS. */
bool intops(Checker *c, uint32_t i)
{
    return is_int(c, rvt(c, i)) &&
           (c->ck[i] == K_ICE || (c->ef[i] & EF_INTOPS));
}

/* The truth value of node i if known: 1 true, 0 false, -1 unknown.  fold:
 * as c_fully_fold sees it (folded values too), else as the parser does. */
int truth(Checker *c, uint32_t i, bool fold)
{
    long double f;
    switch (c->ck[i]) {
    case K_ICE:
        return c->cv[i] != 0;
    case K_FOLD:
        if (!fold && !(c->ef[i] & EF_CST))
            return -1;
        return c->cv[i] != 0;
    case K_FLOAT:
        if (!fold && !(c->ef[i] & EF_REALCST))
            return -1;
        f = c->fv.data[c->cv[i]];
        return f != 0;
    case K_ADDR:
        if (c->cb[i])
            return 1;
        return fold ? c->cv[i] != 0 : -1;
    default:
        return -1;
    }
}

/* ---- constexpr objects (C23 6.7.2) ---------------------------------------
 * A constexpr object is a const one whose value is a constant of the kind
 * its type has: an integer constant expression, an arithmetic constant, a
 * null pointer.  cinit_decl_done checks the initializer and records the value
 * in the symbol; a read of the object (not &x, x++, x = ..) is that constant. */

/* Is the identifier at i read as a value? */
bool value_use(Checker *c, uint32_t i)
{
    uint32_t t = i, p = c->par[i];
    while (p != NO_NODE && ntag(c, p) == N_PAREN) {
        t = p;
        p = c->par[t];
    }
    if (p == NO_NODE)
        return true;
    switch (ntag(c, p)) {
    case N_UNARY:
        return npunct(c, p) != P_AMP && npunct(c, p) != P_INC &&
               npunct(c, p) != P_DEC;
    case N_POSTFIX:
        return false;
    case N_ASSIGN:
        return t == p - 1;
    default:
        return true;
    }
}

/* Node i reads a constexpr value of scalar type t (a null pointer: val 0). */
static void cx_set(Checker *c, uint32_t i, TypeId t, uint64_t val, long double fv)
{
    c->cb[i] = 0;
    if (is_flt(c, t)) {
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, fv);
        c->ef[i] = EF_REALCST;
    } else if (is_ptr(c, t)) {
        c->ck[i] = K_ADDR;
        c->cv[i] = 0;
        c->ef[i] = 0;
    } else {
        c->ck[i] = K_ICE;
        c->cv[i] = val;
        c->ef[i] = EF_INTOPS;
    }
}

void constexpr_read(Checker *c, uint32_t i, const CSym *s)
{
    cx_set(c, i, unqual(c, s->ty), s->val, s->fval);
}

/* The constant of scalar type t at byte off of a constexpr aggregate's bytes;
 * false if it is not one (a bit-field, long double, a non-null pointer). */
static bool cx_load(Checker *c, const CxImg *im, uint64_t off, TypeId t, uint64_t *val, long double *fv)
{
    bool ok = false;
    uint64_t sz = type_size(TT, t, &ok), raw = 0;
    unsigned k;
    if (!ok || !sz || sz > 8 || off > im->n || sz > im->n - off)
        return false;
    for (k = 0; im->b && k < sz; k++)
        raw |= (uint64_t)im->b[off + k] << (8 * k);
    if (is_flt(c, t)) {
        if (sz == 4) {
            float f;
            uint32_t u = (uint32_t)raw;
            memcpy(&f, &u, 4);
            *fv = f;
        } else if (sz == 8) {
            double d;
            memcpy(&d, &raw, 8);
            *fv = d;
        } else
            return false;
    } else if (is_ptr(c, t)) {
        if (raw)
            return false;
        *val = 0;
    } else if (is_int(c, t))
        *val = cexpr_trunc(c, t, raw);
    else
        return false;
    return true;
}

/* The constexpr aggregate lvalue n names (the object and its members) and the
 * byte offset into it. */
static bool cx_lv(Checker *c, uint32_t n, uint32_t *ref, uint64_t *off)
{
    n = strip_paren(c, n);
    if (n == NO_NODE)
        return false;
    switch (ntag(c, n)) {
    case N_IDENT: {
        uint32_t r = lookup_ord(c, cnode_ident(c, n));
        if (r == SYM_NONE || !csym(c, r)->cximg)
            return false;
        *ref = r;
        *off = 0;
        return true;
    }
    case N_MEMBER_EXPR: {
        uint32_t d = first_child(c, n);
        const Field *f;
        uint64_t fo = 0;
        unsigned q = 0;
        if ((c->nodes[n].flags & NF_ARROW) || d == NO_NODE || !cx_lv(c, d, ref, off) ||
            !is_record(c, c->ty[d]) || !cnode_ident(c, n))
            return false;
        f = find_field(c, c->ty[d], cnode_ident(c, n), &fo, &q);
        if (!f || (f->flags & FF_BITFIELD))
            return false;
        *off += fo / 8;
        return true;
    }
    default:                    /* gcc does not fold a subscript of a constexpr array */
        return false;
    }
}

/* A read of a member of a constexpr aggregate is a constant. */
void cexpr_cx_fold(Checker *c, uint32_t i)
{
    TypeId t = unqual(c, c->ty[i]);
    uint32_t ref;
    uint64_t off, val = 0;
    long double fv = 0;
    if ((!is_int(c, t) && !is_flt(c, t) && !is_ptr(c, t)) || (c->ef[i] & EF_BITFIELD) ||
        !value_use(c, i) || !cx_lv(c, i, &ref, &off) ||
        !cx_load(c, &c->cximg.data[csym(c, ref)->cximg - 1], off, t, &val, &fv))
        return;
    cx_set(c, i, t, val, fv);
}

/* The initializer of a constexpr scalar (gcc's check_for_...constexpr): its
 * value in *val (*fval for a floating type), false after a diagnostic. */
static bool cx_scalar(Checker *c, TypeId t, uint32_t init, uint64_t *val, long double *fval)
{
    TypeId it = c->ty[init];
    SrcLoc l = tloc(c, first_tok(c, init));
    bool ok = false;
    if (is_err(c, t) || is_err(c, it))
        return false;
    if (is_int(c, t)) {
        /* integer constant operands, not only an ICE: 1 << 31 passes (a
         * -O folded const variable does not) */
        if (!intops(c, init))
            cerror(c, l, "'constexpr' integer initializer is not an integer "
                   "constant expression");
        else if ((c->ef[init] & EF_OVERFLOW) ||
                 !cexpr_fits(c, c->cv[init], it, t))
            cerror(c, l, "'constexpr' initializer not representable in type "
                   "of object");
        else {
            *val = cexpr_trunc(c, t, c->cv[init]);
            ok = true;
        }
    } else if (is_flt(c, t)) {
        long double v = 0;
        bool have = true;
        if (c->ck[init] == K_FLOAT)
            v = c->fv.data[c->cv[init]];
        else if (c->ck[init] == K_ICE && is_int(c, it))
            v = is_signed(c, it) ? (long double)(int64_t)c->cv[init]
                                 : (long double)c->cv[init];
        else
            have = false;
        if (!have)
            cerror(c, l, "'constexpr' initializer is not a constant "
                   "expression");
        else if (v == v && fround(c, t, v) != v)
            cerror(c, l, "'constexpr' initializer not representable in type "
                   "of object");
        else {
            *fval = v;
            ok = true;
        }
    } else if (is_ptr(c, t)) {
        bool null = (c->ck[init] == K_ADDR && !c->cb[init] && !c->cv[init]) ||
                    (c->ck[init] == K_ICE && is_int(c, it) && !c->cv[init]);
        if (!null)
            cerror(c, l, "'constexpr' pointer initializer is not null");
        else {
            *val = 0;
            ok = true;
        }
    }
    return ok;
}

/* An aggregate's initializer element (tast_image): a constant of its type. */
static bool cx_elem(Checker *c, const TIRec *r)
{
    uint64_t v;
    long double f;
    if (r->width)                       /* bit-fields: parked */
        return true;
    return cx_scalar(c, unqual(c, r->ty), r->node, &v, &f);
}

/* A constexpr aggregate (or a braced scalar): every element is checked, and
 * the evaluator builds the bytes, which reads of its members and elements
 * fold from (cexpr_cx_fold). */
void cexpr_constexpr_agg(Checker *c, CSym *s, uint32_t ref, uint32_t init)
{
    CxImg im;
    TypeId t = unqual(c, s->ty);
    bool agg = is_record(c, t) || is_array(c, t);
    if (c->diag->nerrors != c->unit_nerr0 || is_err(c, t) || s->cx != 1)
        return;
    if (ntag(c, init) != N_INIT_LIST &&
        !(is_array(c, t) && ntag(c, strip_paren(c, init)) == N_STRING))
        return;
    memset(&im, 0, sizeof im);
    if (!tast_image(c, ref, init, cx_elem, &im))
        return;
    vec_push(&c->cximg, im);
    s->cximg = (uint32_t)c->cximg.len;
    if (!agg && cx_load(c, &im, 0, t, &s->val, &s->fval))
        s->cx = 2;
}

void cexpr_constexpr_init(Checker *c, CSym *s, uint32_t init, TypeId type)
{
    if (cx_scalar(c, unqual(c, type), init, &s->val, &s->fval))
        s->cx = 2;
}

