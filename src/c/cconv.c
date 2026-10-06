/* cconv.c - -Wconversion and friends: gcc 13 unsafe_conversion_p and
 * conversion_warning (c-family/c-warn.cc), -Wdouble-promotion, decimal/binary
 * float mixing.  Split out of cexpr.c; shares its helpers through
 * cexpr_int.h. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- -Wconversion: gcc's unsafe_conversion_p and conversion_warning ------- */

enum { UC_SAFE, UC_OTHER, UC_SIGN, UC_REAL, UC_IMAG };

unsigned bf_width(Checker *c, uint32_t n);

static unsigned uc_bw;    /* the width of the bit-field being assigned, or 0 */
static const char *uc_whole;   /* a conditional that folds to this constant */

/* The target type as gcc prints it: a bit-field's own type is 'signed char:1'. */
static const char *tgt_name(Checker *c, TypeId lt, char *buf)
{
    bool tu = !is_signed(c, lt);
    unsigned w = uc_bw;
    if (!w)
        return type_q(TT, lt);
    snprintf(buf, 64, "'%s:%u'", w <= 8 ? (tu ? "unsigned char" : "signed char")
             : w <= 16 ? (tu ? "short unsigned int" : "short int")
             : w <= 32 ? (tu ? "unsigned int" : "int")
                       : (tu ? "long unsigned int" : "long int"), w);
    return buf;
}

static unsigned tgt_bits(Checker *c, TypeId lt)
{
    return uc_bw ? uc_bw : int_bits(c, lt);
}

static bool tgt_fits(Checker *c, uint64_t v, TypeId et, TypeId lt)
{
    bool neg;
    uint64_t mask;
    if (!uc_bw)
        return cexpr_fits(c, v, et, lt);
    neg = is_signed(c, et) && (int64_t)v < 0;
    mask = ((uint64_t)1 << uc_bw) - 1;
    if (!is_signed(c, lt))
        return !neg && v <= mask;
    if (neg)
        return (int64_t)v >= -(int64_t)(mask >> 1) - 1;
    return v <= (mask >> 1);
}

static uint64_t tgt_trunc(Checker *c, TypeId lt, uint64_t v)
{
    uint64_t mask, r;
    if (!uc_bw)
        return cexpr_trunc(c, lt, v);
    mask = ((uint64_t)1 << uc_bw) - 1;
    r = v & mask;
    if (is_signed(c, lt) && (r >> (uc_bw - 1)))
        r |= ~mask;
    return r;
}

/* gcc's REAL_TYPE (decimal floats and complex types excluded). */
bool gcc_real(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_FLOAT16 && k <= TY_IBM128;
}

/* TYPE_PRECISION for ordering integer or real types. */
unsigned uc_prec(Checker *c, TypeId t)
{
    return is_int(c, t) ? int_bits(c, t) : (unsigned)float_prec(c, tkind(c, t));
}

/* A REAL_CST as gcc prints it in a warning: enough digits to round-trip the
 * type, a suffix for float and long double. */
static const char *real_cst_str(Checker *c, char *out, long double f, TypeId t)
{
    char b[160], *e, *p;
    int ex, dig = (int)(uc_prec(c, t) * 0.30103 + 1.0) + 1;
    TypeKind k = tkind(c, t);
    const char *suf = k == TY_FLOAT || k == TY_FLOAT32 ? "f"
                      : k == TY_LDOUBLE ? "l" : "";
    if (f != f) {
        snprintf(out, 160, "+QNaN%s", suf);
        return out;
    }
    if (f > 1.0e4900L || f < -1.0e4900L || f == (long double)HUGE_VALL ||
        f == -(long double)HUGE_VALL) {
        snprintf(out, 160, "%cInf%s", f < 0 ? '-' : '+', suf);
        return out;
    }
    if (f == 0) {
        snprintf(out, 160, "%s0.0%s", signbit(f) ? "-" : "", suf);
        return out;
    }
    snprintf(b, sizeof b, "%.*Le", dig - 1, f);
    e = strchr(b, 'e');
    if (!e) {
        snprintf(out, 160, "%s%s", b, suf);
        return out;
    }
    ex = atoi(e + 1);
    *e = 0;
    p = b + strlen(b);
    while (p > b && p[-1] == '0')
        *--p = 0;
    if (p > b && p[-1] == '.')
        *p++ = '0', *p = 0;
    snprintf(out, 160, "%se%+d%s", b, ex, suf);
    return out;
}

/* f rounded to the real type t (an overflow gives infinity). */
static long double real_round(Checker *c, long double f, TypeId t)
{
    TypeKind k = tkind(c, t);
    if (k == TY_FLOAT || k == TY_FLOAT32)
        return (float)f;
    if (k == TY_DOUBLE || k == TY_FLOAT64 || k == TY_FLOAT32X ||
        (k == TY_LDOUBLE && c->tgt->long_double == LD_IEEE64))
        return (double)f;
    return f;
}

/* An integer part as gcc prints it. */
static const char *part_istr(Checker *c, char *out, TypeId t, long double v)
{
    if (is_signed(c, t))
        snprintf(out, 160, "%lld", (long long)(int64_t)v);
    else
        snprintf(out, 160, "%llu", (unsigned long long)(uint64_t)v);
    return out;
}

/* A complex constant as gcc prints it: (_Complex double){1.0e+0, 0.0}. */
static const char *cplx_str(Checker *c, char *out, long double re,
                            long double im, TypeId ct)
{
    char a[160], b[160], tn[80];
    const char *q = type_q(TT, ct);
    snprintf(tn, sizeof tn, "%.*s", (int)strlen(q) - 2, q + 1);
    if (is_flt(c, ct)) {
        real_cst_str(c, a, re, ct);
        real_cst_str(c, b, im, ct);
    } else {
        part_istr(c, a, ct, re);
        part_istr(c, b, ct, im);
    }
    snprintf(out, 400, "(_Complex %s){%.140s, %.140s}", tn, a, b);
    return out;
}

/* unsafe_conversion_p for one part v (of component type ct) converted to lt. */
static int uc_part(Checker *c, TypeId lt, long double v, TypeId ct,
                   bool check_sign)
{
    if (is_int(c, ct)) {
        uint64_t u = cplx_u(v);
        if (!gcc_integer(c, ct) || int_bits(c, ct) > 64)
            return UC_SAFE;
        if (gcc_integer(c, lt) && int_bits(c, lt) <= 64) {
            if (tgt_fits(c, u, ct, lt))
                return UC_SAFE;
            if (!is_signed(c, lt) && is_signed(c, ct) && (int64_t)u < 0)
                return check_sign ? UC_SIGN : UC_SAFE;
            if (is_signed(c, lt) && !is_signed(c, ct))
                return check_sign ? UC_SIGN : UC_SAFE;
            return UC_OTHER;
        }
        if (gcc_real(c, lt))
            return real_round(c, v, lt) == v ? UC_SAFE : UC_REAL;
        return UC_SAFE;
    }
    if (gcc_real(c, ct)) {
        if (gcc_integer(c, lt))
            return v == truncl(v) ? UC_SAFE : UC_REAL;
        if (gcc_real(c, lt) && uc_prec(c, lt) < uc_prec(c, ct))
            return real_round(c, v, lt) == v ? UC_SAFE : UC_REAL;
    }
    return UC_SAFE;
}

/* Does float f overflow integer type lt?  r: the saturated result. */
bool float_ovf(Checker *c, long double f, TypeId lt, uint64_t *r)
{
    unsigned bits = tgt_bits(c, lt), k;
    long double hi = 1.0L;
    bool ovf;
    if (f != f)
        return false;
    for (k = 0; k < bits - (is_signed(c, lt) ? 1u : 0u); k++)
        hi *= 2.0L;
    /* trunc(f) >= hi, or trunc(f) < lo */
    if (is_signed(c, lt))
        ovf = f >= hi || f <= -hi - 1.0L;
    else
        ovf = f >= hi || f <= -1.0L;
    return ovf && float_to_int_bits(c, f, lt, bits, r);
}

static uint32_t uc_cast_operand(Checker *c, uint32_t e)
{
    uint32_t k[3], n = nkids(c, e, k, 3);
    return n ? k[n - 1] : NO_NODE;
}

/* gcc's get_unwidened (e, 0): the expression and type e had before widening
 * conversions (explicit casts only; the promotions are not nodes here). */
static uint32_t unwidened(Checker *c, uint32_t e, TypeId *ty)
{
    unsigned final = int_bits(c, rvt(c, e));
    uint32_t win = e, op = strip_paren(c, e);
    bool uns = false;
    while (op != NO_NODE && ntag(c, op) == N_CAST) {
        uint32_t in = strip_paren(c, uc_cast_operand(c, op));
        TypeId ot = rvt(c, op), it;
        int bc;
        if (in == NO_NODE)
            break;
        it = rvt(c, in);
        if (!is_int(c, it) || !is_int(c, ot))
            break;
        bc = (int)int_bits(c, ot) - (int)int_bits(c, it);
        if (bc < 0 && final > int_bits(c, ot))
            break;
        op = in;
        if (bc > 0) {
            if (!uns || final <= int_bits(c, it))
                win = in;
            if ((uns || ntag(c, op) == N_CAST) && !is_signed(c, it)) {
                uns = true;
                win = in;
            }
        }
    }
    *ty = rvt(c, win);
    return win;
}

static bool bitwise_op(int p)
{
    return p == P_AMP || p == P_PIPE || p == P_CARET;
}

/* Does node n, an integer constant converted to type as, fit type t? */
bool const_fits(Checker *c, uint32_t n, TypeId as, TypeId t)
{
    return has_ival(c, n) && int_bits(c, rvt(c, n)) <= 64 &&
           int_bits(c, as) <= 64 &&
           cexpr_fits(c, cexpr_trunc(c, as, c->cv[n]), as, t);
}

/* A binary operation, or a compound assignment: gcc builds the same
 * operation tree for both. */
static bool is_binop(Checker *c, uint32_t n)
{
    return ntag(c, n) == N_BINARY ||
           (ntag(c, n) == N_ASSIGN && npunct(c, n) != P_ASSIGN);
}

/* The operator of a binary operation or compound assignment. */
static int binop_p(Checker *c, uint32_t n)
{
    return assign_binop(npunct(c, n));
}

static bool bool_valued(Checker *c, uint32_t s);

/* build_binary_op narrows a division, a modulus or a right shift to its
 * operands' narrower type: wt is that type when it did. */
static bool shorten_divshift(Checker *c, uint32_t ws, TypeId et, TypeId *wt)
{
    uint32_t k[2], s1;
    TypeId t0, t1;
    unsigned p = binop_p(c, ws);
    bool div = p == P_SLASH || p == P_PERCENT, cst;
    if ((!div && p != P_SHR) || nkids(c, ws, k, 2) != 2)
        return false;
    unwidened(c, k[0], &t0);
    s1 = strip_paren(c, unwidened(c, k[1], &t1));
    /* a compound assignment has its target's type: the operation is in et */
    if (int_bits(c, t0) >= int_bits(c, ntag(c, ws) == N_ASSIGN ? et : rvt(c, ws)))
        return false;
    cst = has_ival(c, s1);
    if (div) {
        if (cst && is_signed(c, rvt(c, s1)) && (int64_t)c->cv[s1] == -1)
            cst = false, t1 = 0;
        if (!(!is_signed(c, rvt(c, k[0])) || cst))
            return false;
        if (cst ? !const_fits(c, s1, rvt(c, s1), t0)
                : !t1 || mainv(c, t0) != mainv(c, t1))
            return false;
    } else if (!cst || (is_signed(c, rvt(c, s1)) && (int64_t)c->cv[s1] < 0) ||
               c->cv[s1] >= int_bits(c, t0)) {
        return false;
    }
    *wt = t0;
    return true;
}

/* The narrowed type of a bitwise node's operands (shorten_binary_op) and
 * gcc's BIT_AND special cases: true when the result surely fits lt. */
static bool shorten_bitwise(Checker *c, uint32_t ws, TypeId lt, TypeId et,
                            TypeId *wt)
{
    uint32_t k[2], o0, o1, s0, s1;
    TypeId t0, t1;
    bool c0, c1;
    if (nkids(c, ws, k, 2) != 2)
        return false;
    o0 = unwidened(c, k[0], &t0);
    o1 = unwidened(c, k[1], &t1);
    s0 = strip_paren(c, o0);
    s1 = strip_paren(c, o1);
    c0 = has_ival(c, s0);
    c1 = has_ival(c, s1);
    if (!c0 && !c1) {
        unsigned b0 = int_bits(c, t0), b1 = int_bits(c, t1);
        /* a truth value is a one-bit unsigned (fold: (int) (f != 0)) */
        bool z0 = bool_valued(c, s0), z1 = bool_valued(c, s1);
        if (z0 != z1) {
            *wt = z0 ? t1 : t0;
        } else {
            *wt = b0 > b1 ? t0 : b1 > b0 ? t1
                  : is_signed(c, t0) ? t0 : t1;
        }
        if (int_bits(c, *wt) > int_bits(c, et))
            *wt = et;
    } else if (c0 != c1) {
        TypeId vt = c0 ? t1 : t0;
        uint32_t ks = c0 ? s0 : s1;
        unsigned tp = tgt_bits(c, lt);
        uint64_t m = tp >= 64 ? ~0ull : (1ull << tp) - 1;
        /* a constant that does not fit lt (and is not a full mask) keeps
         * the operation at its own width */
        if (gcc_integer(c, lt) && tp < 64 && int_bits(c, vt) <= tp &&
            (int64_t)c->cv[ks] > 0 && c->cv[ks] >> tp &&
            ntag(c, c0 ? s1 : s0) != N_BINARY &&
            !(binop_p(c, ws) == P_AMP && (c->cv[ks] & m) == m)) {
            *wt = et;
            return false;
        }
        if (int_bits(c, vt) < int_bits(c, *wt))
            *wt = vt;
    }
    if (binop_p(c, ws) == P_AMP) {
        TypeId sg, us, d;
        sign_map(c, lt, &us, &d);
        sign_map(c, lt, &d, &sg);
        TypeId ot = rvt(c, ws);        /* the operands' converted type */
        if ((c0 && const_fits(c, s0, ot, sg) && const_fits(c, s0, ot, us)) ||
            (c1 && const_fits(c, s1, ot, sg) && const_fits(c, s1, ot, us)))
            return true;
        if ((c0 && !is_signed(c, ot) && const_fits(c, s0, ot, lt)) ||
            (c1 && !is_signed(c, ot) && const_fits(c, s1, ot, lt)))
            return true;
    }
    return false;
}

/* unsafe_conversion_p: e (of type et) converted to lt. */
static int unsafe_conv_t(Checker *c, TypeId lt, uint32_t e, TypeId et,
                         bool check_sign)
{
    uint32_t s = strip_paren(c, e);
    if (s == NO_NODE)
        return UC_SAFE;
    if (has_ival(c, s) && int_bits(c, et) <= 64) {
        if (!gcc_integer(c, et))
            return UC_SAFE;
        if (gcc_integer(c, lt) && int_bits(c, lt) <= 64) {
            uint64_t v = c->cv[s];
            if (tgt_fits(c, v, et, lt))
                return UC_SAFE;
            if (!is_signed(c, lt) && is_signed(c, et) && (int64_t)v < 0)
                return check_sign ? UC_SIGN : UC_SAFE;
            if (is_signed(c, lt) && !is_signed(c, et))
                return check_sign ? UC_SIGN : UC_SAFE;
            return UC_OTHER;
        }
        if (gcc_real(c, lt)) {
            long double f = is_signed(c, et) ? (long double)(int64_t)c->cv[s]
                                             : (long double)c->cv[s];
            return real_round(c, f, lt) == f ? UC_SAFE : UC_REAL;
        }
        return UC_SAFE;
    }
    if (c->ck[s] == K_FLOAT && gcc_real(c, et)) {
        long double f = c->fv.data[c->cv[s]];
        if (gcc_integer(c, lt))
            return f == truncl(f) ? UC_SAFE : UC_REAL;
        if (gcc_real(c, lt) && uc_prec(c, lt) < uc_prec(c, et))
            return real_round(c, f, lt) == f ? UC_SAFE : UC_REAL;
        return UC_SAFE;
    }
    if (is_complex(c, et)) {
        TypeId ef = mainv(c, type_base(TT, type_canon(TT, et))), tf;
        if (!is_complex(c, lt))
            return UC_IMAG;
        tf = mainv(c, type_base(TT, type_canon(TT, lt)));
        if (gcc_real(c, ef) && gcc_integer(c, tf))
            return UC_REAL;
        if (gcc_real(c, ef) && gcc_real(c, tf))
            return uc_prec(c, tf) < uc_prec(c, ef) ? UC_REAL : UC_SAFE;
        if (gcc_integer(c, ef) && gcc_integer(c, tf)) {
            bool eu = !is_signed(c, ef), tu = !is_signed(c, tf);
            if (int_bits(c, tf) < int_bits(c, ef))
                return UC_OTHER;
            if (check_sign && ((int_bits(c, tf) == int_bits(c, ef) && eu != tu) ||
                               (tu && !eu)))
                return UC_SIGN;
            return UC_SAFE;
        }
        if (gcc_integer(c, ef) && gcc_real(c, tf))
            return int_bits(c, ef) - (is_signed(c, ef) ? 1u : 0u) <=
                   uc_prec(c, tf) ? UC_SAFE : UC_OTHER;
        return UC_SAFE;
    }
    if (gcc_real(c, et) && gcc_integer(c, lt))
        return UC_REAL;
    if (gcc_integer(c, et) && gcc_integer(c, lt)) {
        TypeId wt;
        uint32_t w = unwidened(c, s, &wt), ws = strip_paren(c, w);
        unsigned tp = tgt_bits(c, lt), wb;
        bool eu, tu = !is_signed(c, lt);
        if (ws != NO_NODE && is_binop(c, ws) &&
            bitwise_op(binop_p(c, ws)) &&
            shorten_bitwise(c, ws, lt, et, &wt))
            return UC_SAFE;
        if (ws != NO_NODE && is_binop(c, ws))
            shorten_divshift(c, ws, et, &wt);
        unsigned bw = ws != NO_NODE ? bf_width(c, ws) : 0;
        wb = int_bits(c, wt);
        if (bw && bw < wb)
            wb = bw;            /* a bit-field has its own narrow type */
        eu = !is_signed(c, wt);
        if (tp < wb)
            return UC_OTHER;
        if (check_sign && ((tp == wb && eu != tu) || (tu && !eu)))
            return UC_SIGN;
        return UC_SAFE;
    }
    if (gcc_integer(c, et) && gcc_real(c, lt)) {
        TypeId wt;
        unsigned fp;
        uint32_t w = unwidened(c, s, &wt), bw = bf_width(c, w);
        if (w != NO_NODE && is_binop(c, strip_paren(c, w)))
            shorten_divshift(c, strip_paren(c, w), et, &wt);
        fp = int_bits(c, wt);
        if (bw && bw < fp)
            fp = bw;
        fp -= is_signed(c, wt) ? 1u : 0u;
        return fp <= uc_prec(c, lt) ? UC_SAFE : UC_OTHER;
    }
    if (gcc_real(c, et) && gcc_real(c, lt) && uc_prec(c, lt) < uc_prec(c, et))
        return UC_REAL;
    return UC_SAFE;
}

static int unsafe_conv(Checker *c, TypeId lt, uint32_t e, bool check_sign)
{
    uint32_t s = strip_paren(c, e);
    return s == NO_NODE ? UC_SAFE
                        : unsafe_conv_t(c, lt, s, rvt(c, s), check_sign);
}

/* The operands of an arithmetic node as conversion_warning treats them
 * (arith_ops); 0 for other nodes. */
static int arith_operands(Checker *c, uint32_t s, uint32_t *k)
{
    int p;
    switch (ntag(c, s)) {
    case N_BINARY:
        p = npunct(c, s);
        if ((p == P_PLUS || p == P_MINUS || p == P_STAR || p == P_SLASH ||
             p == P_PERCENT || bitwise_op(p)) && nkids(c, s, k, 2) == 2)
            return 2;
        /* a shift count does not take part in the conversion */
        if ((p == P_SHL || p == P_SHR) && nkids(c, s, k, 2) == 2)
            return 1;
        return 0;
    case N_UNARY:
        p = npunct(c, s);
        if ((p == P_MINUS || p == P_TILDE) && nkids(c, s, k, 1) == 1)
            return 1;
        return 0;
    case N_ASSIGN:        /* a compound assignment converts a op b */
        p = npunct(c, s);
        if (p == P_ASSIGN)
            return 0;
        p = assign_binop(p);
        if ((p == P_SHL || p == P_SHR) && nkids(c, s, k, 2) == 2)
            return 1;
        return nkids(c, s, k, 2) == 2 ? 2 : 0;
    default:
        return 0;
    }
}

/* A comparison or logical operator: its value is a boolean. */
static bool bool_valued(Checker *c, uint32_t s)
{
    int p;
    if (ntag(c, s) == N_UNARY)
        return npunct(c, s) == P_BANG;
    if (ntag(c, s) == N_COND) {        /* fold turns c ? 1 : 0 into (int)c */
        uint32_t k[3], a0, a1;
        if (nkids(c, s, k, 3) != 3)
            return false;
        a0 = strip_paren(c, k[1]);
        a1 = strip_paren(c, k[2]);
        return has_ival(c, a0) && has_ival(c, a1) &&
               ((c->cv[a0] == 1 && c->cv[a1] == 0) ||
                (c->cv[a0] == 0 && c->cv[a1] == 1)) &&
               !has_ival(c, s);
    }
    if (ntag(c, s) != N_BINARY)
        return false;
    p = npunct(c, s);
    return p == P_EQEQ || p == P_NE || p == P_LT || p == P_GT || p == P_LE ||
           p == P_GE || p == P_ANDAND || p == P_OROR;
}

/* An expression built only from numeric literals (a complex constant). */
bool opeq(Checker *c, uint32_t x, uint32_t y);

/* gcc folds x - x and x ^ x (also as -= and ^=) of integers to 0. */
static bool folds_to_zero(Checker *c, uint32_t n)
{
    uint32_t k[3];
    unsigned p;
    if ((ntag(c, n) != N_BINARY && ntag(c, n) != N_ASSIGN) ||
        !gcc_integer(c, rvt(c, n)))
        return false;
    p = npunct(c, n);
    return (p == P_MINUS || p == P_CARET || p == P_SUB_ASSIGN ||
            p == P_XOR_ASSIGN) &&
           nkids(c, n, k, 3) == 2 && opeq(c, k[0], k[1]);
}

static bool cplx_const(Checker *c, uint32_t n)
{
    uint32_t k[3], m, i;
    n = strip_paren(c, n);
    if (n == NO_NODE)
        return false;
    switch (ntag(c, n)) {
    case N_NUMBER:
        return true;
    case N_UNARY: case N_BINARY:
        m = nkids(c, n, k, 3);
        for (i = 0; i < m; i++)
            if (!cplx_const(c, k[i]))
                return false;
        return m > 0;
    default:
        return false;
    }
}

/* conversion_warning for a complex constant s (of type et) converted to lt. */
static void cplx_conv_warn(Checker *c, SrcLoc l, TypeId lt, uint32_t s,
                           TypeId et, bool top)
{
    long double re, im, ore, oim;
    TypeId ct, tc;
    int kind;
    const char *opt, *tn, *w, *toS;
    char fb[400], to[400], tb[64];
    if (!cplx_get(c, s, &re, &im, &ct))
        return;
    if (is_complex(c, lt)) {
        tc = cplx_comp(c, lt);
        if (!part_conv(c, ct, tc, re, &ore) || !part_conv(c, ct, tc, im, &oim))
            return;
        kind = uc_part(c, tc, re, ct, true);
        if (kind == UC_SAFE)
            kind = uc_part(c, tc, im, ct, true);
        cplx_str(c, to, ore, oim, tc);
        tn = type_q(TT, lt);
    } else {
        tc = lt;
        if (!part_conv(c, ct, lt, re, &ore))
            return;
        kind = im != 0 ? UC_OTHER : uc_part(c, lt, re, ct, true);
        if (gcc_integer(c, lt))
            snprintf(to, sizeof to, "%s",
                     vstr(c, lt, tgt_trunc(c, lt, cplx_u(ore))));
        else
            real_cst_str(c, to, ore, lt);
        tn = tgt_name(c, lt, tb);
    }
    if (kind == UC_SAFE)
        return;
    opt = kind == UC_REAL ? "float-conversion"
          : kind == UC_SIGN ? "sign-conversion" : "conversion";
    if (!diag_enabled(c->diag, opt))
        return;
    cplx_str(c, fb, re, im, ct);
    toS = uc_whole ? uc_whole : to;
    w = is_signed(c, tc) ? "signed" : "unsigned";
    if (kind == UC_SIGN) {
        if (top || uc_whole)
            cwarn(c, l, opt, "%s conversion from %s to %s changes value from "
                  "'%s' to '%s'", w, type_q(TT, et), tn, fb, toS);
        else
            cwarn(c, l, opt, "%s conversion from %s to %s changes the value "
                  "of '%s'", w, type_q(TT, et), tn, fb);
    } else if (top || uc_whole) {
        cwarn(c, l, opt, "conversion from %s to %s changes value from '%s' to "
              "'%s'", type_q(TT, et), tn, fb, toS);
    } else {
        cwarn(c, l, opt, "conversion from %s to %s changes the value of '%s'",
              type_q(TT, et), tn, fb);
    }
}

/* conversion_warning: e converted to lt.  top: e is the converted expression,
 * not an arm of a conditional. */
static void conversion_warning(Checker *c, SrcLoc l, TypeId lt, uint32_t e,
                               TypeId et, bool top)
{
    uint32_t s = strip_paren(c, e), k[3];
    int kind;
    const char *opt;
    bool cst;
    if (s == NO_NODE || node_err(c, s) || folds_to_zero(c, s))
        return;
    cst = has_ival(c, s) || c->ck[s] == K_FLOAT;
    if (ntag(c, s) == N_COND && !cst && !bool_valued(c, s)) {
        uint32_t n = nkids(c, s, k, 3);
        if (n >= 2) {
            /* the arms were converted to the conditional's type */
            TypeId ct = rvt(c, s);
            int j;
            char wb[40];
            const char *saved = uc_whole;
            size_t first_seen = c->diag->all.len;
            uc_whole = NULL;
            /* arms that convert to one value make the whole a constant */
            if (gcc_integer(c, lt) && int_bits(c, lt) <= 64 &&
                has_ival(c, strip_paren(c, k[n - 2])) &&
                has_ival(c, strip_paren(c, k[n - 1]))) {
                uint64_t r0 = tgt_trunc(c, lt, c->cv[strip_paren(c, k[n - 2])]),
                         r1 = tgt_trunc(c, lt, c->cv[strip_paren(c, k[n - 1])]);
                if (r0 == r1) {
                    snprintf(wb, sizeof wb, "%s", vstr(c, lt, r0));
                    uc_whole = wb;
                }
            }
            for (j = n - 2; j < (int)n; j++) {
                bool ar = is_arith(c, ct) && !is_complex(c, ct);
                size_t seen = c->diag->all.len;
                if (j == (int)n - 1 && seen != first_seen)
                    break;      /* gcc stops at the first arm that warns */
                /* gcc warned about an arm's conversion to ct already */
                if (ar && mainv(c, promoted(c, k[j])) != mainv(c, ct) &&
                    unsafe_conv_t(c, ct, k[j], promoted(c, k[j]), true) !=
                    UC_SAFE)
                    continue;
                /* a signed bit-field's true arm that does not fit: gcc folds
                 * the conditional and names it in an overflow warning */
                if (uc_bw && j == (int)n - 2 && is_signed(c, lt) &&
                    gcc_integer(c, ct) && int_bits(c, ct) <= 64 &&
                    has_ival(c, strip_paren(c, k[j])) &&
                    !tgt_fits(c, c->cv[strip_paren(c, k[j])], ct, lt)) {
                    char tb[64];
                    if (diag_enabled(c->diag, "overflow"))
                        cwarn(c, l, "overflow", "overflow in conversion from "
                              "%s to %s changes value from '%s' to '%s'",
                              type_q(TT, ct), tgt_name(c, lt, tb),
                              cexpr_str_plain(c, s),
                              vstr(c, lt, tgt_trunc(c, lt,
                                   c->cv[strip_paren(c, k[j])])));
                    continue;
                }
                conversion_warning(c, l, lt, k[j], ar ? ct : rvt(c, k[j]),
                                   false);
            }
            uc_whole = saved;
        }
        return;
    }
    if (!(gcc_integer(c, et) || gcc_real(c, et) || is_complex(c, et)) ||
        !(gcc_integer(c, lt) || gcc_real(c, lt) || is_complex(c, lt)))
        return;
    if (is_complex(c, et) && (c->ef[s] & EF_CPLXCST)) {
        cplx_conv_warn(c, l, lt, s, et, top);
        return;
    }
    if (is_complex(c, et) && cplx_const(c, s))
        return;      /* a constant we could not evaluate */
    if (!cst && bool_valued(c, s)) {
        char tb[64];
        if (uc_bw == 1 && is_signed(c, lt) && diag_enabled(c->diag, "conversion"))
            cwarn(c, l, "conversion", "conversion to %s from boolean "
                  "expression", tgt_name(c, lt, tb));
        return;
    }
    if (top && !cst && is_binop(c, s) && bitwise_op(binop_p(c, s)) &&
        gcc_integer(c, lt) && gcc_integer(c, et) && int_bits(c, lt) < 64 &&
        nkids(c, s, k, 2) == 2) {
        /* a constant beyond lt's width: fold turns `x & K` into 0 when K has
         * no bit in lt's width; otherwise the constant itself is the culprit */
        uint32_t a = strip_paren(c, k[0]), b = strip_paren(c, k[1]);
        bool ca = has_ival(c, a);
        uint32_t ks = ca ? a : b, os = ca ? b : a;
        unsigned tp = tgt_bits(c, lt);
        uint64_t m = (1ull << tp) - 1;
        TypeId ot;
        uint32_t ow = unwidened(c, os, &ot);
        if (ca != has_ival(c, b) && (int64_t)c->cv[ks] > 0 &&
            c->cv[ks] >> tp && ntag(c, strip_paren(c, ow)) != N_BINARY) {
            char sb[200];
            if ((c->cv[ks] & m) == 0 && binop_p(c, s) == P_AMP) {
                if (diag_enabled(c->diag, "overflow")) {
                    const char *tq = type_q(TT, rvt(c, s));
                    char pn[80] = "";
                    if (rvt(c, k[ca]) != rvt(c, s))
                        snprintf(pn, sizeof pn, "(%.*s)", (int)strlen(tq) - 2,
                                 tq + 1);
                    snprintf(sb, sizeof sb, "%s%s & %s", pn,
                             cexpr_str(c, k[ca]), vstr(c, rvt(c, ks), c->cv[ks]));
                    cwarn(c, l, "overflow", "overflow in conversion from %s "
                          "to %s changes value from '%s' to '0'",
                          type_q(TT, et), type_q(TT, lt), sb);
                }
                return;
            }
            if (int_bits(c, ot) <= tp &&
                !(binop_p(c, s) == P_AMP && (c->cv[ks] & m) == m)) {
                if (diag_enabled(c->diag, "conversion"))
                    cwarn(c, l, "conversion", "conversion from %s to %s "
                          "changes the value of '%s'", type_q(TT, et),
                          type_q(TT, lt), vstr(c, rvt(c, ks), c->cv[ks]));
                return;
            }
        }
    }
    kind = unsafe_conv_t(c, lt, s, et, true);
    if (kind == UC_SAFE)
        return;
    opt = kind == UC_REAL ? "float-conversion"
          : kind == UC_SIGN ? "sign-conversion" : "conversion";
    if (kind == UC_IMAG) {
        if (diag_enabled(c->diag, opt)) {
            char tb[64];
            cwarn(c, l, opt, "conversion from %s to %s discards imaginary "
                  "component", type_q(TT, et), tgt_name(c, lt, tb));
        }
        return;
    }
    if (kind != UC_REAL && !cst) {
        int i, n = arith_operands(c, s, k);
        bool safe = n > 0;
        for (i = 0; i < n && safe; i++) {
            bool cs = true;
            uint32_t o = strip_paren(c, k[i]);
            if (i == 1 && npunct(c, s) == P_PLUS && !is_signed(c, lt) &&
                has_ival(c, o) && is_signed(c, rvt(c, o)) &&
                (int64_t)c->cv[o] < 0)
                cs = false;
            if (unsafe_conv(c, lt, k[i], cs) != UC_SAFE)
                safe = false;
        }
        if (safe)
            opt = "arith-conversion";        /* off by default */
    }
    if (!diag_enabled(c->diag, opt))
        return;
    if (cst) {
        char a[160], b[160], tb[64];
        const char *from, *to, *tn = tgt_name(c, lt, tb);
        if (has_ival(c, s)) {
            uint64_t v = c->cv[s];
            from = vstr(c, et, v);
            if (gcc_integer(c, lt)) {
                to = vstr(c, lt, tgt_trunc(c, lt, v));
            } else {
                long double f = is_signed(c, et) ? (long double)(int64_t)v
                                                 : (long double)v;
                to = real_cst_str(c, b, real_round(c, f, lt), lt);
            }
        } else {
            long double f = c->fv.data[c->cv[s]];
            uint64_t r;
            from = real_cst_str(c, a, f, et);
            if (gcc_integer(c, lt)) {
                if (!float_to_int_bits(c, f, lt, tgt_bits(c, lt), &r))
                    return;
                to = vstr(c, lt, r);
            } else
                to = real_cst_str(c, b, real_round(c, f, lt), lt);
        }
        if (kind == UC_SIGN) {
            const char *w = is_signed(c, lt) ? "signed" : "unsigned";
            if (top || uc_whole)
                cwarn(c, l, opt, "%s conversion from %s to %s changes value "
                      "from '%s' to '%s'", w, type_q(TT, et), tn, from,
                      uc_whole ? uc_whole : to);
            else
                cwarn(c, l, opt, "%s conversion from %s to %s changes the "
                      "value of '%s'", w, type_q(TT, et), tn, from);
        } else if (top || uc_whole) {
            cwarn(c, l, opt, "conversion from %s to %s changes value from "
                  "'%s' to '%s'", type_q(TT, et), tn, from,
                  uc_whole ? uc_whole : to);
        } else {
            cwarn(c, l, opt, "conversion from %s to %s changes the value of "
                  "'%s'", type_q(TT, et), tn, from);
        }
        return;
    }
    {
        char tb[64];
        if (kind == UC_SIGN)
            cwarn(c, l, opt, "conversion to %s from %s may change the sign of "
                  "the result", tgt_name(c, lt, tb), type_q(TT, et));
        else
            cwarn(c, l, opt, "conversion from %s to %s may change value",
                  type_q(TT, et), tgt_name(c, lt, tb));
    }
}

/* convert_and_check of arithmetic types: -Woverflow for a constant.  True
 * when gcc does not go on to conversion_warning. */
static SrcLoc conv_loc(Conv *x)
{
    Checker *c = x->c;
    SrcLoc l = x->eloc ? x->eloc : x->loc;
    if (x->ci->context == CONV_ARG && !x->ci->loc) {    /* the argument's start,
                                                        * at the macro use in a
                                                        * system header */
        uint32_t t = first_tok(c, x->expr);
        SrcFile *sf;
        l = ctok_loc(c, t);
        sf = srcmgr_file_of(c->sm, l);
        if (sf && sf->system_header && c->u->toks[t].exp)
            l = c->u->toks[t].exp;
        {
            /* a folded real or complex constant is located at its operator */
            uint32_t s = strip_paren(c, x->expr);
            if (s != NO_NODE && ntag(c, s) == N_BINARY &&
                (c->ck[s] == K_FLOAT || (c->ef[s] & EF_CPLXCST)))
                l = cnode_loc(c, s);
        }
    } else {                /* a macro of a system header: where it is used */
        SrcFile *sf = srcmgr_file_of(c->sm, l);
        uint32_t t = first_tok(c, x->expr);
        if (sf && sf->system_header && c->u->toks[t].exp)
            l = c->u->toks[t].exp;
    }
    return l;
}

bool is_cmp_op(int op);

/* gcc builds the result of a vector comparison or ! as an opaque vector
 * type, convertible to any vector of the same size. */
bool vector_truth_node(Checker *c, uint32_t e)
{
    e = strip_paren(c, e);
    if (e == NO_NODE)
        return false;
    if (ntag(c, e) == N_BINARY)
        return is_cmp_op(npunct(c, e));
    return ntag(c, e) == N_UNARY && npunct(c, e) == P_BANG;
}

static bool conv_overflow(Conv *x)
{
    Checker *c = x->c;
    uint32_t e = x->expr;
    TypeId lt = x->type, rt = x->rhstype;
    SrcLoc l = conv_loc(x);
    if (inhibited(c, e, false) || (c->ef[e] & EF_OVERFLOW))
        return true;
    if (!is_int(c, lt) || tkind(c, lt) == TY_BOOL)
        return false;
    /* a __int128 literal (an unsuffixed decimal above LLONG_MAX) keeps its
     * value in cv; computed 128-bit values are not tracked */
    if (has_ival(c, e) && int_bits(c, lt) <= 64 &&
        (int_bits(c, rt) <= 64 ||
         (int_bits(c, rt) == 128 && ntag(c, strip_paren(c, e)) == N_NUMBER))) {
        uint64_t v = c->cv[e], r = cexpr_trunc(c, lt, v);
        TypeId us, sg, dummy;
        bool warn = false;
        if (cexpr_fits(c, v, rt, lt))
            return false;
        sign_map(c, lt, &us, &dummy);
        sign_map(c, lt, &dummy, &sg);
        if (!is_signed(c, lt)) {
            if (!cexpr_fits(c, v, rt, sg)) {
                cwarn(c, l, "overflow", "%sconversion from %s to %s "
                      "changes value from '%s' to '%s'",
                      is_signed(c, rt) ? "unsigned " : "", type_q(TT, rt),
                      type_q(TT, lt), vstr(c, rt, v), vstr(c, lt, r));
                return true;
            }
            return false;
        }
        if (!cexpr_fits(c, v, rt, us))
            warn = true;
        else if (c->opt.pedantic &&
                 (!gcc_integer(c, rt) || int_bits(c, rt) != int_bits(c, lt)))
            warn = true;
        if (warn)
            cwarn(c, l, "overflow", "overflow in conversion from %s to %s "
                  "changes value from '%s' to '%s'", type_q(TT, rt),
                  type_q(TT, lt), vstr(c, rt, v), vstr(c, lt, r));
        return warn;
    }
    if (c->ck[e] == K_FLOAT && is_flt(c, rt) && int_bits(c, lt) <= 64) {
        long double f = c->fv.data[c->cv[e]];
        uint64_t r;
        char rb[160];
        if (!float_ovf(c, f, lt, &r))
            return false;
        char tb[64];
        cwarn(c, l, "overflow", "overflow in conversion from %s to %s changes "
              "value from '%s' to '%s'", type_q(TT, rt), tgt_name(c, lt, tb),
              real_cst_str(c, rb, f, rt), vstr(c, lt, r));
        return true;
    }
    if ((c->ef[e] & EF_CPLXCST) && is_complex(c, rt) && int_bits(c, lt) <= 64) {
        /* the real part is converted; a signed target overflows when an
         * integer part does not fit, any target when a float part saturates */
        long double re, im;
        TypeId ct;
        uint64_t r = 0;
        char fb[400];
        bool ovf;
        if (!cplx_get(c, e, &re, &im, &ct))
            return false;
        if (is_flt(c, ct))
            ovf = float_ovf(c, re, lt, &r);
        else if ((ovf = is_int(c, ct) && int_bits(c, ct) <= 64 &&
                        is_signed(c, lt) && !cexpr_fits(c, cplx_u(re), ct, lt)))
            r = cexpr_trunc(c, lt, cplx_u(re));
        if (ovf)
            cwarn(c, l, "overflow", "overflow in conversion from %s to %s "
                  "changes value from '%s' to '%s'", type_q(TT, rt),
                  type_q(TT, lt), cplx_str(c, fb, re, im, ct), vstr(c, lt, r));
        return ovf;
    }
    return false;
}

void conv_arith(Conv *x)
{
    Checker *c = x->c;
    unsigned w = x->ci->lhs_bits;
    if (w && gcc_integer(c, x->type) && w < int_bits(c, x->type) &&
        gcc_real(c, x->rhstype) && !inhibited(c, x->expr, false) &&
        !(c->ef[x->expr] & EF_OVERFLOW)) {
        uc_bw = w;
        if (!conv_overflow(x))
            conversion_warning(c, conv_loc(x), x->type, x->expr, x->rhstype,
                               true);
        uc_bw = 0;
        return;
    }
    if (w && gcc_integer(c, x->type) && w < int_bits(c, x->type) &&
        gcc_integer(c, x->rhstype) && !inhibited(c, x->expr, false) &&
        !(c->ef[x->expr] & EF_OVERFLOW)) {
        /* a bit-field is converted to its own narrow type */
        uint32_t s = strip_paren(c, x->expr);
        bool done = s != NO_NODE && has_ival(c, s) &&
                    int_bits(c, rvt(c, s)) <= 64 &&
                    cexpr_bf_overflow(c, conv_loc(x), s, x->type,
                                      x->rhstype, w);
        if (!done) {
            uc_bw = w;
            conversion_warning(c, conv_loc(x), x->type, x->expr, x->rhstype,
                               true);
            uc_bw = 0;
        }
        return;
    }
    if (!conv_overflow(x))
        conversion_warning(x->c, conv_loc(x), x->type, x->expr, x->rhstype,
                           true);
}

/* convert_and_check of an operand to the type of its operation (build_binary_op,
 * build_conditional_expr); prom: the operand was promoted first. */
/* build_binary_op: decimal floating operands do not mix with the other
 * floating or complex types. */
bool dec_mix(Checker *c, uint32_t i, TypeId ta, TypeId tb)
{
    TypeKind ka = tkind(c, ta), kb = tkind(c, tb);
    bool da = ka == TY_DEC32 || ka == TY_DEC64 || ka == TY_DEC128;
    bool db = kb == TY_DEC32 || kb == TY_DEC64 || kb == TY_DEC128;
    if (da == db || !is_arith(c, ta) || !is_arith(c, tb) || is_int(c, ta) ||
        is_int(c, tb))
        return false;
    cerror(c, cinput_loc(c, last_tok(c, i) + 1), "cannot mix operands of decimal "
           "floating and %s types", is_complex(c, ta) || is_complex(c, tb)
           ? "complex" : "other floating");
    set_err(c, i);
    return true;
}

/* -Wdouble-promotion (do_warn_double_promotion): a float (or complex float)
 * operand implicitly converted to double (long double does not warn). */
void double_promo(Checker *c, uint32_t at, SrcLoc loc, TypeId from,
                         TypeId to, const char *what)
{
    TypeId f = mainv(c, from), t = mainv(c, to);
    if (!diag_enabled(c->diag, "double-promotion"))
        return;
    if (is_complex(c, f) != is_complex(c, t))
        return;
    if (is_complex(c, f)) {
        f = cplx_comp(c, f);
        t = cplx_comp(c, t);
    }
    if (tkind(c, f) != TY_FLOAT ||
        tkind(c, t) != TY_DOUBLE ||
        inhibited(c, at, false))
        return;
    cwarn(c, loc, "double-promotion", "implicit conversion from %s to %s %s",
          type_q(TT, mainv(c, from)), type_q(TT, mainv(c, to)), what);
}

void conv_operand(Checker *c, SrcLoc l, TypeId lt, uint32_t a, bool prom)
{
    ConvInfo ci;
    Conv x;
    TypeId at = prom ? promoted(c, a) : rvt(c, a);
    if (!is_arith(c, at) || is_complex(c, at) || mainv(c, at) == mainv(c, lt))
        return;
    memset(&ci, 0, sizeof ci);
    x.c = c;
    x.ci = &ci;
    x.expr = a;
    x.loc = x.eloc = l;
    x.type = lt;
    x.rhstype = at;
    x.npc = false;
    conv_arith(&x);
}

/* The declared enum type of an enumerator, else the value's type (gcc's
 * original type of the expression). */
TypeId orig_type(Checker *c, uint32_t e)
{
    uint32_t s = strip_paren(c, e);
    if (s != NO_NODE && ntag(c, s) == N_COND) {
        /* c_parser_conditional_expression: both arms' original types, when
         * they agree */
        uint32_t k[3], n = nkids(c, s, k, 3);
        if (n >= 2) {
            TypeId t1 = orig_type(c, k[n - 2]), t2 = orig_type(c, k[n - 1]);
            if (mainv(c, t1) == mainv(c, t2))
                return t1;
        }
        return rvt(c, s);
    }
    if (s != NO_NODE && ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA) {
        uint32_t k[2];
        if (nkids(c, s, k, 2) == 2)
            return orig_type(c, k[1]);      /* the right operand's */
    }
    if (s != NO_NODE && ntag(c, s) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        if (ref != SYM_NONE && csym(c, ref)->kind == CS_ENUMCONST)
            return csym(c, ref)->ty;
    }
    return rvt(c, e);
}

