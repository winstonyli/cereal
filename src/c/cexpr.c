/* cexpr.c - expressions: their types, lvalue-ness and constant values
 * (C99 6.5, 6.6), with gcc 13's diagnostics (c-typeck.cc build_binary_op,
 * build_unary_op, build_c_cast, build_component_ref, ...; c-parser.cc for
 * the order and locations).
 *
 * Each expression node gets, in the checker's per-node arrays:
 *   ty  its type before lvalue conversion (arrays and functions undecayed);
 *   ck  what kind of constant its value (after lvalue conversion) is, and
 *   cv / cb  that value: an integer (truncated to the type, sign-extended if
 *       signed), an index into Checker.fv, or an address (cb base, cv byte
 *       offset);
 *   ef  flags (lvalue, bit-field, null pointer constant, gcc's tree codes
 *       as far as constant folding and its warnings depend on them).
 *
 * gcc's constant kinds map to ours: an INTEGER_CST built at parse time from
 * integer constant operands is K_ICE (EF_OVERFLOW: TREE_OVERFLOW); a value
 * gcc only finds when it folds the full expression (c_fully_fold) is
 * K_FOLD; EF_INTOPS is C_MAYBE_CONST_EXPR_INT_OPERANDS. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

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
static TypeId common_type(Checker *c, TypeId a, TypeId b)
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

uint64_t cexpr_trunc(Checker *c, TypeId t, uint64_t v)
{
    unsigned bits;
    if (tkind(c, t) == TY_BOOL)
        return v != 0;
    if (!is_int(c, t) && !is_ptr(c, t))
        return v;
    bits = int_bits(c, t);
    if (bits == 0 || bits >= 64)
        return v;
    v &= (UINT64_C(1) << bits) - 1;
    if (is_int(c, t) && is_signed(c, t) && (v >> (bits - 1)) & 1)
        v |= ~((UINT64_C(1) << bits) - 1);
    return v;
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

/* Integer arithmetic in type t (promoted; at most 64 bits).  Sets *ovf on
 * signed overflow, *zdiv on division by zero. */
static uint64_t int_op(Checker *c, int op, TypeId t, uint64_t a, uint64_t b,
                       bool *ovf, bool *zdiv)
{
    bool sg = is_signed(c, t);
    unsigned bits = int_bits(c, t);
    uint64_t r = 0;
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    *ovf = false;
    *zdiv = false;
    if (bits > 64 || bits == 0)
        bits = 64;
    switch (op) {
    case P_PLUS:
        r = a + b;
        if (sg && bits == 64)
            *ovf = (int64_t)((a ^ r) & (b ^ r)) < 0;
        break;
    case P_MINUS:
        r = a - b;
        if (sg && bits == 64)
            *ovf = (int64_t)((a ^ b) & (a ^ r)) < 0;
        break;
    case P_STAR:
        if (sg && bits == 64) {
            r = a * b;
            if (sa != 0 && sb != 0) {
                if ((sa == -1 && sb == INT64_MIN) ||
                    (sb == -1 && sa == INT64_MIN))
                    *ovf = true;
                else
                    *ovf = (int64_t)r / sb != sa;
            }
        } else if (sg) {
            r = (uint64_t)(sa * sb);   /* |a|, |b| < 2^32: exact */
        } else {
            r = a * b;
        }
        break;
    case P_SLASH: case P_PERCENT:
        if (b == 0) {
            *zdiv = true;
            return 0;
        }
        if (sg) {
            int64_t min = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
            if (sa == min && sb == -1) {
                *ovf = true;
                r = op == P_SLASH ? (uint64_t)min : 0;
            } else {
                r = (uint64_t)(op == P_SLASH ? sa / sb : sa % sb);
            }
        } else {
            r = op == P_SLASH ? a / b : a % b;
        }
        break;
    case P_AMP: r = a & b; break;
    case P_PIPE: r = a | b; break;
    case P_CARET: r = a ^ b; break;
    case P_SHL:
        r = b >= 64 ? 0 : a << b;
        break;
    case P_SHR:
        if (b >= 64)
            r = sg && sa < 0 ? ~UINT64_C(0) : 0;
        else if (sg && sa < 0)
            r = ~(~a >> b);
        else
            r = a >> b;
        break;
    default:
        break;
    }
    if (bits < 64) {
        uint64_t tr = cexpr_trunc(c, t, r);
        if (sg && op != P_SHL && tr != r)
            *ovf = true;
        r = tr;
    }
    return r;
}

/* ---- floating values ----------------------------------------------------- */

static long double fround(Checker *c, TypeId t, long double v)
{
    switch (tkind(c, t)) {
    case TY_FLOAT: case TY_FLOAT32:
        return (float)v;
    case TY_DOUBLE: case TY_FLOAT64: case TY_FLOAT32X:
        return (double)v;
    case TY_LDOUBLE:
        if (c->tgt->long_double == LD_IEEE64)
            return (double)v;
        return v;
    default:
        return v;
    }
}

/* The value of node i as a long double, if it is an arithmetic constant. */
static bool fval(Checker *c, uint32_t i, long double *out)
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
bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out);
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

static long double cplx_ld(Checker *c, TypeId t, uint64_t u)
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
static bool cplx_conv(Checker *c, uint32_t a, TypeId to, long double *re,
                      long double *im)
{
    long double r, m;
    TypeId ct, tc = cplx_comp(c, to);
    return cplx_get(c, a, &r, &m, &ct) && part_conv(c, ct, tc, r, re) &&
           part_conv(c, ct, tc, m, im);
}

/* (a + bi) op (c + di) in component type tc. */
static bool cplx_arith(Checker *c, int op, TypeId tc, long double ar,
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
static bool constvar_in(Checker *c, uint32_t i)
{
    uint32_t j;
    for (j = i + 1 - c->nodes[i].size; j <= i; j++)
        if (c->nodes[j].tag == N_IDENT && c->ck[j] == K_FOLD &&
            (c->ef[j] & EF_CST))
            return true;
    return false;
}

/* EXPR_INT_CONST_OPERANDS. */
static bool intops(Checker *c, uint32_t i)
{
    return is_int(c, rvt(c, i)) &&
           (c->ck[i] == K_ICE || (c->ef[i] & EF_INTOPS));
}

/* The truth value of node i if known: 1 true, 0 false, -1 unknown.  fold:
 * as c_fully_fold sees it (folded values too), else as the parser does. */
static int truth(Checker *c, uint32_t i, bool fold)
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

/* ---- context --------------------------------------------------------------- */

/* gcc's c_inhibit_evaluation_warnings at node i: inside sizeof, alignof,
 * typeof, a _Generic selector, or an operand not evaluated because of a
 * constant condition. */
bool inhibited(Checker *c, uint32_t i, bool fold)
{
    uint32_t ch = i, p;
    for (p = c->par[i]; p != NO_NODE; ch = p, p = c->par[p]) {
        uint32_t f;
        switch (c->nodes[p].tag) {
        case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
        case N_ALIGNOF_TYPE: case N_TYPEOF:
            return true;
        case N_GENERIC:
            if (ch == first_child(c, p))
                return true;
            break;
        case N_BINARY:
            if (ch == p - 1 && (npunct(c, p) == P_ANDAND ||
                                npunct(c, p) == P_OROR)) {
                f = first_child(c, p);
                if (f != NO_NODE && f != ch &&
                    truth(c, f, fold) == (npunct(c, p) == P_ANDAND ? 0 : 1))
                    return true;
            }
            break;
        case N_COND: {
            uint32_t k[3], n = nkids(c, p, k, 3);
            int tv;
            if (n < 2 || ch == k[0])
                break;
            tv = truth(c, k[0], fold);
            if (tv < 0)
                break;
            if (ch == k[n - 1] && tv == 1)
                return true;
            if (n == 3 && ch == k[1] && tv == 0)
                return true;
            break;
        }
        default:
            break;
        }
    }
    return false;
}

bool cexpr_cxx_compat(Checker *c, uint32_t node)
{
    return diag_enabled(c->diag, "c++-compat") &&
           (node == NO_NODE || !cexpr_in_extension(c, node));
}

bool cexpr_in_extension(Checker *c, uint32_t i)
{
    uint32_t p;
    for (p = i; p != NO_NODE; p = c->par[p]) {
        const Node *n = &c->nodes[p];
        if (n->tag == N_UNARY && tckw(c, n->tok) == CK_EXTENSION)
            return true;
        if ((n->tag == N_DECL || n->tag == N_FUNC_DEF) &&
            (n->flags & NF_EXTENSION))
            return true;
    }
    return false;
}

/* A -Wpedantic pedwarn about node i (off under __extension__). */
Diagnostic *ped(Checker *c, uint32_t i, SrcLoc loc, const char *fmt,
                       ...)
{
    char buf[512];
    va_list ap;
    if (!c->opt.pedantic || cexpr_in_extension(c, i))
        return NULL;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return cpedantic(c, loc, "%s", buf);
}

/* A pedwarn under -Wpointer-arith (-pedantic enables it). */
static void ped_arith(Checker *c, uint32_t i, SrcLoc loc, const char *msg)
{
    if (cexpr_in_extension(c, i))
        return;
    cpedwarn(c, loc, "pointer-arith", "%s", msg);
}

/* ---- members ------------------------------------------------------------- */

/* The field name of record rec (anonymous members searched, depth first);
 * adds its offset to *off and the qualifiers of the anonymous members on
 * the way to *quals. */
const Field *find_field(Checker *c, TypeId rec, uint32_t name,
                               uint64_t *off, unsigned *quals)
{
    const Record *r;
    uint32_t k;
    if (!is_record(c, rec))
        return NULL;
    r = type_record(TT, rec);
    if (!(r->flags & RF_COMPLETE) && !(r->flags & RF_DEFINING))
        return NULL;
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (f->name == name) {
            *off += f->off_bits;
            return f;
        }
    }
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (!f->name && is_record(c, f->ty)) {
            uint64_t o = *off + f->off_bits;
            unsigned q = *quals | tquals(c, f->ty);
            const Field *g = find_field(c, f->ty, name, &o, &q);
            if (g) {
                *off = o;
                *quals = q;
                return g;
            }
            r = type_record(TT, rec);   /* the table may have moved */
        }
    }
    return NULL;
}

bool cexpr_find_member(Checker *c, TypeId rec, uint32_t name, TypeId *ty,
                       uint64_t *off_bits, bool *bitfield)
{
    uint64_t off = 0;
    unsigned q = 0;
    const Field *f = find_field(c, rec, name, &off, &q);
    if (!f)
        return false;
    *ty = type_qual(f->ty, q);
    *off_bits += off;
    *bitfield = (f->flags & FF_BITFIELD) != 0;
    return true;
}

/* ---- spelling suggestions (gcc's spellcheck.cc) ---------------------------- */


/* A misspelled member name: the fields of rec, anonymous members'
 * included. */
void fuzzy_fields(Checker *c, Best *b, TypeId rec)
{
    const Record *r;
    uint32_t k;
    if (!is_record(c, rec))
        return;
    r = type_record(TT, rec);
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (f->name)
            best_consider(b, cident(c, f->name));
        else
            fuzzy_fields(c, b, f->ty);
        r = type_record(TT, rec);
    }
}

/* The spelling suggestion for a misspelled member of rec (NULL: none). */
const char *cexpr_fuzzy_field(Checker *c, TypeId rec, uint32_t name)
{
    Best b;
    best_init(&b, cident(c, name), &c->fuzzy_work);
    fuzzy_fields(c, &b, rec);
    return best_get(&b);
}

/* ---- missing headers (gcc's known-headers.cc, C part) ---------------------- */

typedef struct StdName {
    const char *name, *header;
} StdName;

static const StdName std_names[] = {
    {"assert", "<assert.h>"}, {"errno", "<errno.h>"},
    {"CHAR_BIT", "<limits.h>"}, {"CHAR_MAX", "<limits.h>"},
    {"CHAR_MIN", "<limits.h>"}, {"INT_MAX", "<limits.h>"},
    {"INT_MIN", "<limits.h>"}, {"LLONG_MAX", "<limits.h>"},
    {"LLONG_MIN", "<limits.h>"}, {"LONG_MAX", "<limits.h>"},
    {"LONG_MIN", "<limits.h>"}, {"MB_LEN_MAX", "<limits.h>"},
    {"SCHAR_MAX", "<limits.h>"}, {"SCHAR_MIN", "<limits.h>"},
    {"SHRT_MAX", "<limits.h>"}, {"SHRT_MIN", "<limits.h>"},
    {"UCHAR_MAX", "<limits.h>"}, {"UINT_MAX", "<limits.h>"},
    {"ULLONG_MAX", "<limits.h>"}, {"ULONG_MAX", "<limits.h>"},
    {"USHRT_MAX", "<limits.h>"},
    {"DBL_MAX", "<float.h>"}, {"DBL_MIN", "<float.h>"},
    {"FLT_MAX", "<float.h>"}, {"FLT_MIN", "<float.h>"},
    {"LDBL_MAX", "<float.h>"}, {"LDBL_MIN", "<float.h>"},
    {"va_list", "<stdarg.h>"},
    {"NULL", "<stddef.h>"}, {"offsetof", "<stddef.h>"},
    {"size_t", "<stddef.h>"}, {"wchar_t", "<stddef.h>"},
    {"ptrdiff_t", "<stddef.h>"},
    {"BUFSIZ", "<stdio.h>"}, {"EOF", "<stdio.h>"}, {"FILE", "<stdio.h>"},
    {"FILENAME_MAX", "<stdio.h>"}, {"fopen", "<stdio.h>"},
    {"fpos_t", "<stdio.h>"}, {"getchar", "<stdio.h>"},
    {"printf", "<stdio.h>"}, {"snprintf", "<stdio.h>"},
    {"sprintf", "<stdio.h>"}, {"stderr", "<stdio.h>"},
    {"stdin", "<stdio.h>"}, {"stdout", "<stdio.h>"},
    {"EXIT_FAILURE", "<stdlib.h>"}, {"EXIT_SUCCESS", "<stdlib.h>"},
    {"abort", "<stdlib.h>"}, {"atexit", "<stdlib.h>"},
    {"calloc", "<stdlib.h>"}, {"exit", "<stdlib.h>"},
    {"free", "<stdlib.h>"}, {"getenv", "<stdlib.h>"},
    {"malloc", "<stdlib.h>"}, {"realloc", "<stdlib.h>"},
    {"memchr", "<string.h>"}, {"memcmp", "<string.h>"},
    {"memcpy", "<string.h>"}, {"memmove", "<string.h>"},
    {"memset", "<string.h>"}, {"strcat", "<string.h>"},
    {"strchr", "<string.h>"}, {"strcmp", "<string.h>"},
    {"strcpy", "<string.h>"}, {"strlen", "<string.h>"},
    {"strncat", "<string.h>"}, {"strncmp", "<string.h>"},
    {"strncpy", "<string.h>"}, {"strrchr", "<string.h>"},
    {"strspn", "<string.h>"}, {"strstr", "<string.h>"},
    {"PTRDIFF_MAX", "<stdint.h>"}, {"PTRDIFF_MIN", "<stdint.h>"},
    {"SIG_ATOMIC_MAX", "<stdint.h>"}, {"SIG_ATOMIC_MIN", "<stdint.h>"},
    {"SIZE_MAX", "<stdint.h>"}, {"WINT_MAX", "<stdint.h>"},
    {"WINT_MIN", "<stdint.h>"},
    {"INT8_MAX", "<stdint.h>"}, {"INT16_MAX", "<stdint.h>"},
    {"INT32_MAX", "<stdint.h>"}, {"INT64_MAX", "<stdint.h>"},
    {"INT8_MIN", "<stdint.h>"}, {"INT16_MIN", "<stdint.h>"},
    {"INT32_MIN", "<stdint.h>"}, {"INT64_MIN", "<stdint.h>"},
    {"UINT8_MAX", "<stdint.h>"}, {"UINT16_MAX", "<stdint.h>"},
    {"UINT32_MAX", "<stdint.h>"}, {"UINT64_MAX", "<stdint.h>"},
    {"int8_t", "<stdint.h>"}, {"int16_t", "<stdint.h>"},
    {"int32_t", "<stdint.h>"}, {"int64_t", "<stdint.h>"},
    {"intptr_t", "<stdint.h>"}, {"uint8_t", "<stdint.h>"},
    {"uint16_t", "<stdint.h>"}, {"uint32_t", "<stdint.h>"},
    {"uint64_t", "<stdint.h>"}, {"uintptr_t", "<stdint.h>"},
    {"asctime", "<time.h>"}, {"clock", "<time.h>"},
    {"clock_t", "<time.h>"}, {"ctime", "<time.h>"},
    {"difftime", "<time.h>"}, {"gmtime", "<time.h>"},
    {"localtime", "<time.h>"}, {"mktime", "<time.h>"},
    {"strftime", "<time.h>"}, {"time", "<time.h>"},
    {"time_t", "<time.h>"}, {"tm", "<time.h>"},
    {"WCHAR_MAX", "<wchar.h>"}, {"WCHAR_MIN", "<wchar.h>"},
    {"bool", "<stdbool.h>"}, {"true", "<stdbool.h>"},
    {"false", "<stdbool.h>"}
};

const char *std_header(const char *name)
{
    size_t k;
    for (k = 0; k < sizeof std_names / sizeof *std_names; k++)
        if (!strcmp(std_names[k].name, name))
            return std_names[k].header;
    return NULL;
}

/* Where gcc suggests adding an #include: the line after the last #include
 * of the main file before loc, else its start. */
static SrcLoc include_loc(SrcMgr *sm, SrcLoc loc)
{
    uint32_t k, n = srcmgr_nfiles(sm), line, nl, lastinc = 0;
    SrcFile *f = NULL;
    for (k = 0; k < n; k++) {
        SrcFile *g = srcmgr_file(sm, k);
        if (g && g->kind == SF_USER && !g->system_header) {
            f = g;
            break;
        }
    }
    if (!f)
        return loc;
    srcmgr_linecol(f, f->base + f->size, &nl, &line);
    if (srcmgr_file_of(sm, loc) == f)
        srcmgr_linecol(f, loc, &nl, &line);
    for (line = 1; line < nl; line++) {
        uint32_t len, j = 0;
        const char *s = srcmgr_line_text(f, line, &len);
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j >= len || s[j] != '#')
            continue;
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (len - j >= 7 && !memcmp(s + j, "include", 7))
            lastinc = line;
    }
    return srcmgr_loc_of(f, lastinc + 1, 1);
}

/* The location of a note suggesting header hdr: gcc puts the first one per
 * header where the #include would go (with a fix-it), later ones at the
 * use. */
SrcLoc diag_header_note_loc(DiagEngine *diag, SrcLoc loc, const char *hdr)
{
    static const char *const known[] = {
        "<assert.h>", "<complex.h>", "<ctype.h>", "<inttypes.h>", "<math.h>",
        "<stdarg.h>", "<stdbool.h>", "<stddef.h>", "<stdint.h>", "<stdio.h>",
        "<stdlib.h>", "<string.h>", "<time.h>", "<wchar.h>", "<errno.h>",
        "<limits.h>", "<float.h>", "<setjmp.h>", "<signal.h>", "<locale.h>"
    };
    size_t k;
    for (k = 0; k < sizeof known / sizeof *known; k++)
        if (!strcmp(known[k], hdr)) {
            uint64_t bit = UINT64_C(1) << k;
            bool seen = (diag->hdr_noted & bit) != 0;
            diag->hdr_noted |= bit;
            return seen ? loc : include_loc(diag->sm, loc);
        }
    return include_loc(diag->sm, loc);
}

SrcLoc header_note_loc(Checker *c, SrcLoc loc, const char *hdr)
{
    return diag_header_note_loc(c->diag, loc, hdr);
}

static void header_note(Checker *c, Diagnostic *d, SrcLoc loc,
                        const char *name, const char *hdr)
{
    cnote(c, d, header_note_loc(c, loc, hdr),
          "'%s' is defined in header '%s'; did you forget to '#include %s'?",
          name, hdr, hdr);
}

#include "cbuiltin_tab.h"


/* any: the __builtin_ spelling, which exists in every mode */
const BTab *bt_find(Checker *c, const char *name, bool any)
{
    size_t lo = 0, hi = sizeof cbuiltin_tab / sizeof *cbuiltin_tab;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, cbuiltin_tab[mid].name);
        if (!r)
            return (cbuiltin_tab[mid].gnu == 1 && !c->opt.gnu && !any) ||
                           (cbuiltin_tab[mid].gnu >= 2 && !any)
                       ? NULL : (const BTab *)&cbuiltin_tab[mid];
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* A type as gcc prints it ("const char *", "long unsigned int"). */
static TypeId bt_type(Checker *c, const char *s, size_t n)
{
    TypeId t;
    bool cst = false;
    size_t k;
    while (n && s[n - 1] == ' ')
        n--;
    while (n && *s == ' ')
        s++, n--;
    if (n > 6 && !strncmp(s, "const ", 6)) {
        cst = true;
        s += 6;
        n -= 6;
    }
    if (n && s[n - 1] == '*') {
        TypeId b = bt_type(c, s, n - 1);
        if (cst)
            b = type_qual(b, TQ_CONST);
        return type_ptr(TT, b);
    }
    {
        static const struct { const char *n; TypeKind k; } base[] = {
            {"void", TY_VOID}, {"int", TY_INT}, {"char", TY_CHAR},
            {"long int", TY_LONG}, {"long long int", TY_LLONG},
            {"long unsigned int", TY_ULONG}, {"double", TY_DOUBLE},
            {"float", TY_FLOAT}, {"long double", TY_LDOUBLE},
            {"__float128", TY_FLOAT128}
        };
        t = ERRT;
        for (k = 0; k < sizeof base / sizeof *base; k++)
            if (strlen(base[k].n) == n && !strncmp(base[k].n, s, n))
                t = TYPE_MK(base[k].k, 0);
        if (is_err(c, t)) {
            if (n > 9 && !strncmp(s, "_Complex ", 9))
                t = type_complex(TT, bt_type(c, s + 9, n - 9));
            else if (n == 13 && !strncmp(s, "__va_list_tag", 13))
                t = type_base(TT, type_canon(TT, TT->va_list));
        }
    }
    if (cst)
        t = type_qual(t, TQ_CONST);
    return t;
}

/* The function type of a table entry. */
TypeId bt_func_type(Checker *c, const BTab *b)
{
    const char *p = strchr(b->sig, '|'), *q;
    TypeId ps[8], ret;
    uint32_t n = 0, flags = 0;
    if (!p)
        return ERRT;
    ret = bt_type(c, b->sig, (size_t)(p - b->sig));
    p++;
    if (!strcmp(p, "?"))
        return type_func(TT, ret, NULL, 0, TF_NOPROTO);
    while (*p && n < 8) {
        q = strchr(p, '|');
        if (!q)
            q = p + strlen(p);
        if (q - p == 3 && !strncmp(p, "...", 3))
            flags |= TF_VARIADIC;
        else
            ps[n++] = bt_type(c, p, (size_t)(q - p));
        p = *q ? q + 1 : q;
    }
    return type_func(TT, ret, ps, n, flags);
}

/* __builtin_{s,u}{add,sub,mul}{,l,ll}_overflow: _Bool (T, T, T *). */
static TypeId overflow_func_type(Checker *c, const char *b)
{
    static const char *const op[] = {"add", "sub", "mul"};
    bool sg = *b == 's';
    TypeKind k;
    TypeId t, ps[3];
    size_t k2;
    int found = 0;
    if (*b != 's' && *b != 'u')
        return ERRT;
    for (k2 = 0; k2 < 3; k2++)
        if (!strncmp(b + 1, op[k2], 3))
            found = 1;
    if (!found)
        return ERRT;
    b += 4;
    if (!strncmp(b, "ll_overflow", 12))
        k = sg ? TY_LLONG : TY_ULLONG;
    else if (!strncmp(b, "l_overflow", 11))
        k = sg ? TY_LONG : TY_ULONG;
    else if (!strncmp(b, "_overflow", 10))
        k = sg ? TY_INT : TY_UINT;
    else
        return ERRT;
    t = TYPE_MK(k, 0);
    ps[0] = ps[1] = t;
    ps[2] = type_ptr(TT, t);
    return type_func(TT, TYPE_B(BOOL), ps, 3, 0);
}

/* gcc's builtin type declarations, candidates for misspelled names. */
static const char *const builtin_type_names[] = {
    "int", "char", "long int", "unsigned int", "long unsigned int",
    "long long int", "long long unsigned int", "short int",
    "short unsigned int", "signed char", "unsigned char", "__int128",
    "__int128 unsigned", "float", "double", "long double", "_Float16",
    "_Float32", "_Float64", "_Float128", "_Float32x", "_Float64x",
    "complex int", "complex float", "complex double", "complex long double",
    "void", "_Bool", "__bf16"
};

/* lookup_name_fuzzy: a visible name close to goal.  functions: only
 * functions and pointers to functions (an implicit declaration). */
static const char *fuzzy_name(Checker *c, const char *goal, bool functions)
{
    Best b;
    size_t k;
    bool res_ok = goal[0] == '_';
    best_init(&b, goal, &c->fuzzy_work);
    for (k = c->log.len; k-- > 0;) {
        const Bind *bd = &c->log.data[k];
        const char *s;
        if (c->top[bd->ns][bd->ident] == 0)
            continue;
        if (c->fuzzy_work > SC_BUDGET)
            return NULL;
        s = cident(c, bd->ident);
        if (!res_ok && reserved_name(s))
            continue;
        if (bd->ns == NS_ORD) {
            CSym *sym = csym(c, bd->ref);
            if (sym->flags & CSF_IMPLICIT)
                continue;
            if ((sym->flags & CSF_ERROR) && is_err(c, sym->ty))
                continue;
            if (functions) {
                if (sym->kind == CS_FUNC)
                    ;
                else if (sym->kind == CS_OBJ && is_ptr(c, sym->ty) &&
                         is_func(c, pointee(c, sym->ty)))
                    ;
                else
                    continue;
            }
        } else if (functions) {
            continue;
        }
        best_consider_n(&b, s, ident_by_id(c->in, bd->ident)->len);
    }
    if (!functions)
        for (k = sizeof builtin_type_names / sizeof *builtin_type_names;
             k-- > 0;)
            if (res_ok || !reserved_name(builtin_type_names[k]))
                best_consider(&b, builtin_type_names[k]);
    return best_get(&b);
}

/* ---- printing expressions (gcc's %E) ---------------------------------------- */

static void print_ival(Checker *c, StrBuf *sb, TypeId t, uint64_t v)
{
    if (is_int(c, t) && is_signed(c, t))
        sb_printf(sb, "%lld", (long long)(int64_t)v);
    else
        sb_printf(sb, "%llu", (unsigned long long)v);
}

static int bin_prec(int op)
{
    switch (op) {
    case P_COMMA: return PR_COMMA;
    case P_OROR: return PR_LOR;
    case P_ANDAND: return PR_LAND;
    case P_PIPE: return PR_OR;
    case P_CARET: return PR_XOR;
    case P_AMP: return PR_AND;
    case P_EQEQ: case P_NE: return PR_EQ;
    case P_LT: case P_GT: case P_LE: case P_GE: return PR_REL;
    case P_SHL: case P_SHR: return PR_SHIFT;
    case P_PLUS: case P_MINUS: return PR_ADD;
    default: return PR_MUL;
    }
}

/* Does node i print as its value (an INTEGER_CST)? */
static bool prints_value(Checker *c, uint32_t i)
{
    return has_ival(c, i) &&
           (c->ck[i] == K_ICE || (c->ef[i] & (EF_CST | EF_NOPCST)));
}

static bool pfloat(Checker *c, StrBuf *sb, const char *s, size_t len);
static void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec);
static void pcond_arm(Checker *c, StrBuf *sb, uint32_t arm, TypeId rt, int prec);
static void pcond_test(Checker *c, StrBuf *sb, uint32_t cond);

/* Operand i converted to type t (an implicit conversion prints as a
 * cast). */
static void pconv(Checker *c, StrBuf *sb, uint32_t i, TypeId t, int prec)
{
    i = strip_paren(c, i);
    if (i == NO_NODE)
        return;
    if (is_arith(c, t) && !is_err(c, t) && mainv(c, rvt(c, i)) != mainv(c, t)) {
        if (prints_value(c, i) && is_int(c, t)) {
            print_ival(c, sb, t, cexpr_trunc(c, t, c->cv[i]));
            return;
        }
        if (prec > PR_UNARY)
            sb_putc(sb, '(');
        sb_putc(sb, '(');
        type_print(TT, sb, mainv(c, t));
        sb_putc(sb, ')');
        pexpr(c, sb, i, PR_UNARY);
        if (prec > PR_UNARY)
            sb_putc(sb, ')');
        return;
    }
    pexpr(c, sb, i, prec);
}

/* gcc builds p[i] on a pointer as *(p + i * size) and prints that tree:
 * the byte offset of a constant index, (sizetype)(...) of a variable one.
 * Returns the base and index nodes when node i is such a subscript and its
 * spelling is one cereal reproduces (no folded sums of a signed index). */
static bool lowered_sub(Checker *c, uint32_t i, uint32_t *base, uint32_t *idx,
                        uint64_t *size)
{
    uint32_t k[3], b, x, sx;
    TypeId bt, pt;
    bool ov;
    if (ntag(c, i) != N_INDEX || nkids(c, i, k, 3) < 2)
        return false;
    b = strip_paren(c, k[0]);
    x = strip_paren(c, k[1]);
    if (b == NO_NODE || x == NO_NODE)
        return false;
    bt = type_canon(TT, c->ty[b]);
    if (type_ckind(TT, bt) != TY_PTR) {
        uint32_t t = b;
        b = x;
        x = t;
        bt = type_canon(TT, c->ty[b]);
        if (type_ckind(TT, bt) != TY_PTR)
            return false;
    }
    if (!is_int(c, rvt(c, x)))
        return false;
    pt = type_canon(TT, type_base(TT, bt));
    *size = type_size(TT, pt, &ov);
    if (!ov || !*size || tkind(c, pt) == TY_VLA)
        return false;
    if (c->ck[x] != K_ICE) {
        sx = x;
        /* sums and differences of a signed index are folded by gcc */
        if (ntag(c, sx) == N_BINARY && (npunct(c, sx) == P_PLUS ||
                                        npunct(c, sx) == P_MINUS) &&
            is_signed(c, rvt(c, sx))) {
            uint32_t sk[3];
            if (nkids(c, sx, sk, 3) >= 2 &&
                c->ck[strip_paren(c, sk[1])] == K_ICE &&
                (npunct(c, sx) != P_PLUS ||
                 cexpr_sval(c, strip_paren(c, sk[1])) <= 0 ||
                 c->ck[strip_paren(c, sk[0])] == K_ICE ||
                 type_size(TT, rvt(c, sx), &ov) != 4))
                return false;
        }
        if (ntag(c, sx) == N_COND) {
            /* gcc folds a conditional with a constant arm */
            uint32_t ck[3];
            if (nkids(c, sx, ck, 3) < 3 ||
                c->ck[strip_paren(c, ck[1])] == K_ICE ||
                c->ck[strip_paren(c, ck[2])] == K_ICE)
                return false;
        }
        if (has_ival(c, sx))
            return false;
    }
    *base = b;
    *idx = x;
    return true;
}

static void plowered(Checker *c, StrBuf *sb, uint32_t b, uint32_t x,
                     uint64_t size)
{
    char num[32];
    sb_putc(sb, '*');
    if (c->ck[x] == K_ICE) {
        int64_t off = cexpr_sval(c, x) * (int64_t)size;
        if (off == 0) {
            pexpr(c, sb, b, PR_UNARY);
            return;
        }
        sb_putc(sb, '(');
        pexpr(c, sb, b, PR_ADD);
        snprintf(num, sizeof num, " + %lld)", (long long)off);
        sb_puts(sb, num);
        return;
    }
    sb_putc(sb, '(');
    pexpr(c, sb, b, PR_ADD);
    sb_puts(sb, " + ");
    {
        uint32_t sk[3], sx = strip_paren(c, x);
        if (ntag(c, sx) == N_BINARY && npunct(c, sx) == P_PLUS &&
            is_signed(c, rvt(c, sx)) && nkids(c, sx, sk, 3) >= 2 &&
            c->ck[strip_paren(c, sk[1])] == K_ICE) {
            /* gcc distributes: ((sizetype)i + 1) * size */
            snprintf(num, sizeof num, " + %lld)", (long long)
                     cexpr_sval(c, strip_paren(c, sk[1])));
            sb_puts(sb, "((sizetype)");
            pexpr(c, sb, sk[0], PR_UNARY);
            sb_puts(sb, num);
            if (size != 1) {
                snprintf(num, sizeof num, " * %llu", (unsigned long long)size);
                sb_puts(sb, num);
            }
            sb_putc(sb, ')');
            return;
        }
    }
    sb_puts(sb, "(sizetype)");
    {
        TypeId xt = type_canon(TT, rvt(c, x));
        bool wide_u = type_size(TT, xt, &(bool){0}) == 8 && !is_signed(c, xt);
        if (size == 1) {
            pexpr(c, sb, x, PR_UNARY);
        } else if (wide_u) {
            sb_putc(sb, '(');
            pexpr(c, sb, x, PR_MUL);
            snprintf(num, sizeof num, " * %llu)", (unsigned long long)size);
            sb_puts(sb, num);
        } else {
            sb_puts(sb, "((long unsigned int)");
            pexpr(c, sb, x, PR_UNARY);
            snprintf(num, sizeof num, " * %llu)", (unsigned long long)size);
            sb_puts(sb, num);
        }
    }
    sb_putc(sb, ')');
}

/* Whether the lvalue at node i is const through its own type or through a
 * const-qualified enclosing object (gcc's tree gives such members const
 * types). */
static bool const_through(Checker *c, uint32_t i)
{
    uint32_t b;
    while (i != NO_NODE && ntag(c, i) == N_PAREN)
        i = first_child(c, i);
    if (i == NO_NODE)
        return false;
    if (tquals(c, c->ty[i]) & TQ_CONST)
        return true;
    if (ntag(c, i) == N_MEMBER_EXPR) {
        b = first_child(c, i);
        if (b == NO_NODE)
            return false;
        if ((c->nodes[i].flags & NF_ARROW) && is_ptr(c, rvt(c, b)))
            return tquals(c, pointee(c, rvt(c, b))) & TQ_CONST;
        return const_through(c, b);
    }
    if (ntag(c, i) == N_INDEX) {
        b = first_child(c, i);
        return b != NO_NODE && is_array(c, c->ty[b]) && const_through(c, b);
    }
    return false;
}

/* The condition of `?:` as gcc prints it after truth-value conversion:
 * `(c) != 0`, `(d) != (0.0)`, `(a) < (b)`, `(c) == 0` for `!c`. */
static bool pcond_plain;   /* print a ?: test as written, not folded */

static void pcond_test(Checker *c, StrBuf *sb, uint32_t cond)
{
    uint32_t e = strip_paren(c, cond), k[3];
    TypeId t;
    int op;
    if (e == NO_NODE || has_ival(c, e) || prints_value(c, e) ||
        c->ck[e] == K_ERR || is_err(c, c->ty[e])) {
        pexpr(c, sb, cond, PR_LOR);
        return;
    }
    t = rvt(c, e);
    if (pcond_plain) {
        bool lg = (ntag(c, e) == N_BINARY && (op = npunct(c, e), op == P_ANDAND ||
                   op == P_OROR || bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL)) ||
                  (ntag(c, e) == N_UNARY && npunct(c, e) == P_BANG);
        pexpr(c, sb, e, lg ? PR_LOR : PR_EQ);
        if (!lg)
            sb_puts(sb, " != 0");
        return;
    }
    if (ntag(c, e) == N_BINARY && nkids(c, e, k, 3) == 2) {
        op = npunct(c, e);
        if (op == P_ANDAND || op == P_OROR) {
            pcond_test(c, sb, k[0]);
            sb_puts(sb, op == P_ANDAND ? " && " : " || ");
            pcond_test(c, sb, k[1]);
            return;
        }
        if (bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL) {
            bool ar = is_arith(c, rvt(c, k[0])) && is_arith(c, rvt(c, k[1]));
            if (ar) {
                sb_putc(sb, '(');
                pexpr(c, sb, k[0], PR_COMMA);
                sb_putc(sb, ')');
            } else {
                pexpr(c, sb, k[0], bin_prec(op));
            }
            sb_printf(sb, " %s ", punct_spelling[op]);
            if (ar) {
                sb_putc(sb, '(');
                pexpr(c, sb, k[1], PR_COMMA);
                sb_putc(sb, ')');
            } else {
                pexpr(c, sb, k[1], bin_prec(op) + 1);
            }
            return;
        }
    }
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_BANG &&
        nkids(c, e, k, 3) == 1 && is_arith(c, rvt(c, k[0]))) {
        sb_putc(sb, '(');
        pexpr(c, sb, k[0], PR_COMMA);
        sb_puts(sb, ") == 0");
        return;
    }
    if (is_ptr(c, t)) {
        pexpr(c, sb, e, PR_EQ);
        sb_puts(sb, " != 0");
    } else if (is_arith(c, t)) {
        sb_putc(sb, '(');
        if (tkind(c, t) == TY_CHAR || tkind(c, t) == TY_SCHAR) {
            sb_puts(sb, "(signed char)");
            pexpr(c, sb, e, PR_UNARY);
        } else if (tkind(c, t) == TY_UCHAR) {
            sb_puts(sb, "(unsigned char)");
            pexpr(c, sb, e, PR_UNARY);
        } else {
            pexpr(c, sb, e, PR_COMMA);
        }
        sb_puts(sb, is_flt(c, t) ? ") != (0.0)" : ") != 0");
    } else {
        pexpr(c, sb, cond, PR_LOR);
    }
}

/* An arm of `?:` with a pointer result: gcc prints it converted to the
 * result type (an array arm as the address of the whole array). */
static void pcond_arm(Checker *c, StrBuf *sb, uint32_t arm, TypeId rt, int prec)
{
    TypeId at = c->ty[arm];
    bool arr = is_array(c, at);
    if (is_ptr(c, rt) && (arr || is_ptr(c, at)) && (arr || at != rt)) {
        sb_putc(sb, '(');
        type_print(TT, sb, rt);
        sb_putc(sb, ')');
        if (arr)
            sb_putc(sb, '&');
        pexpr(c, sb, arm, PR_UNARY);
        return;
    }
    pexpr(c, sb, arm, prec);
}

static void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec)
{
    uint32_t k[3], n, lb, lx;
    uint64_t lsz;
    int my = PR_PRIMARY, op;
    size_t len;
    const char *s;
    i = strip_paren(c, i);
    if (i == NO_NODE)
        return;
    if (prints_value(c, i)) {
        print_ival(c, sb, rvt(c, i), c->cv[i]);
        return;
    }
    n = nkids(c, i, k, 3);
    switch (ntag(c, i)) {
    case N_BINARY: my = bin_prec(npunct(c, i)); break;
    case N_ASSIGN: my = PR_ASSIGN; break;
    case N_COND: my = PR_COND; break;
    case N_CAST: case N_UNARY: case N_SIZEOF_EXPR: case N_SIZEOF_TYPE:
    case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE:
        my = PR_UNARY;
        break;
    case N_INDEX: case N_CALL: case N_MEMBER_EXPR: case N_POSTFIX:
        my = ntag(c, i) == N_INDEX && lowered_sub(c, i, &lb, &lx, &lsz)
                 ? PR_UNARY : PR_POSTFIX;
        break;
    default: break;
    }
    if (my < prec)
        sb_putc(sb, '(');
    switch (ntag(c, i)) {
    case N_IDENT: case N_NUMBER: case N_CHAR:
        s = ttext(c, c->nodes[i].tok, &len);
        if (ntag(c, i) == N_NUMBER && pfloat(c, sb, s, len))
            break;
        sb_putn(sb, s, len);
        break;
    case N_STRING: {
        uint32_t t;
        for (t = 0; t < node_pieces(c, i); t++) {
            if (t)
                sb_putc(sb, ' ');
            s = ttext(c, c->nodes[i].tok + t, &len);
            sb_putn(sb, s, len);
        }
        break;
    }
    case N_BINARY:
        if (n < 2)
            break;
        op = npunct(c, i);
        if (op == P_COMMA || op == P_ANDAND || op == P_OROR) {
            pexpr(c, sb, k[0], my);
            sb_puts(sb, op == P_COMMA ? ", " : op == P_ANDAND ? " && "
                                                              : " || ");
            pexpr(c, sb, k[1], my + 1);
        } else {
            TypeId t0 = rvt(c, i), t1 = rvt(c, i);
            if (op == P_SHL || op == P_SHR) {
                t0 = promoted(c, k[0]);
                t1 = promoted(c, k[1]);
            } else if (bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL) {
                t0 = t1 = TYPE_B(ERROR);
                if (is_arith(c, rvt(c, k[0])) && is_arith(c, rvt(c, k[1])))
                    t0 = t1 = common_type(c, promoted(c, k[0]),
                                          promoted(c, k[1]));
            } else if (!is_arith(c, t0)) {
                t0 = t1 = TYPE_B(ERROR);
            }
            pconv(c, sb, k[0], t0, my);
            sb_printf(sb, " %s ", punct_spelling[op]);
            pconv(c, sb, k[1], t1, my + 1);
        }
        break;
    case N_ASSIGN:
        if (n < 2)
            break;
        pexpr(c, sb, k[0], PR_UNARY);
        sb_printf(sb, " %s ", punct_spelling[npunct(c, i)]);
        pexpr(c, sb, k[1], PR_ASSIGN);
        break;
    case N_COND:
        if (n < 2)
            break;
        pcond_test(c, sb, k[0]);
        sb_puts(sb, " ? ");
        if (n == 3)
            pcond_arm(c, sb, k[1], c->ty[i], PR_COMMA);
        sb_puts(sb, " : ");
        pcond_arm(c, sb, k[n - 1], c->ty[i], PR_COND);
        break;
    case N_CAST:
        if (n < 2)
            break;
        if (mainv(c, c->ty[i]) == mainv(c, rvt(c, k[1]))) {
            pexpr(c, sb, k[1], PR_UNARY);
            break;
        }
        sb_putc(sb, '(');
        type_print(TT, sb, c->ty[i]);
        sb_putc(sb, ')');
        if (c->ty[k[1]] != ERRT && tkind(c, c->ty[k[1]]) == TY_FUNC) {
            /* a function designator has decayed: gcc prints its address */
            uint32_t x = strip_paren(c, k[1]), q[3];
            while (x != NO_NODE && ntag(c, x) == N_UNARY &&
                   npunct(c, x) == P_STAR && nkids(c, x, q, 3) == 1 &&
                   tkind(c, c->ty[q[0]]) == TY_FUNC)
                x = strip_paren(c, q[0]);
            sb_putc(sb, '&');
            pexpr(c, sb, x, PR_UNARY);
            break;
        }
        pexpr(c, sb, k[1], PR_UNARY);
        break;
    case N_UNARY:
        if (n < 1)
            break;
        op = npunct(c, i);
        if (op == P_STAR && c->ty[k[0]] != ERRT && is_array(c, c->ty[k[0]])) {
            /* gcc prints a deref of an array as *(T *)&array */
            uint32_t x = k[0];
            TypeId et = elem_of(c, c->ty[x]);
            while (ntag(c, x) == N_UNARY && npunct(c, x) == P_STAR &&
                   nkids(c, x, k, 3) >= 1 && is_array(c, c->ty[k[0]]))
                x = k[0];
            while (is_array(c, et))
                et = elem_of(c, et);
            if (const_through(c, x))
                et = type_qual(et, TQ_CONST);
            sb_puts(sb, "*(");
            type_print(TT, sb, et);
            sb_puts(sb, " *)&");
            pexpr(c, sb, x, PR_UNARY);
            break;
        }
        if (op != P_NONE) {
            sb_puts(sb, punct_spelling[op]);
        } else {
            s = ttext(c, c->nodes[i].tok, &len);
            sb_putn(sb, s, len);
            sb_putc(sb, ' ');
        }
        if ((op == P_MINUS || op == P_PLUS || op == P_TILDE) &&
            is_arith(c, rvt(c, i)))
            pconv(c, sb, k[0], rvt(c, i), PR_UNARY);
        else
            pexpr(c, sb, k[0], PR_UNARY);
        break;
    case N_POSTFIX:
        if (n < 1)
            break;
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_puts(sb, punct_spelling[npunct(c, i)]);
        break;
    case N_SIZEOF_EXPR: case N_ALIGNOF_EXPR:
        s = ttext(c, c->nodes[i].tok, &len);
        sb_putn(sb, s, len);
        sb_putc(sb, ' ');
        if (n)
            pexpr(c, sb, k[0], PR_UNARY);
        break;
    case N_INDEX:
        if (n < 2)
            break;
        if (lowered_sub(c, i, &lb, &lx, &lsz)) {
            plowered(c, sb, lb, lx, lsz);
            break;
        }
        if (c->ty[k[0]] != ERRT && tkind(c, c->ty[k[0]]) == TY_VECTOR) {
            /* gcc subscripts a vector through an array view of it */
            TypeId vt = c->ty[k[0]];
            uint64_t esz = type_size(TT, type_base(TT, vt), &(bool){0});
            sb_puts(sb, "((");
            type_print(TT, sb, type_qual(type_base(TT, vt), TYPE_QUALS(vt)));
            sb_printf(sb, "[%u])", (unsigned)(type_size(TT, vt, &(bool){0}) / (esz ? esz : 1)));
            pexpr(c, sb, k[0], PR_UNARY);
            sb_putc(sb, ')');
        } else {
            pexpr(c, sb, k[0], PR_POSTFIX);
        }
        sb_putc(sb, '[');
        pexpr(c, sb, k[1], PR_COMMA);
        sb_putc(sb, ']');
        break;
    case N_MEMBER_EXPR:
        if (n < 1)
            break;
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_puts(sb, c->nodes[i].flags & NF_ARROW ? "->" : ".");
        if (!(c->nodes[i].flags & NF_ERROR)) {
            s = ttext(c, c->nodes[i].tok, &len);
            sb_putn(sb, s, len);
        }
        break;
    case N_CALL: {
        uint32_t a[64], m = nkids(c, i, a, 64), j;
        if (m == 0)
            break;
        pexpr(c, sb, a[0], PR_POSTFIX);
        sb_puts(sb, " (");
        for (j = 1; j < m && j < 64; j++) {
            if (j > 1)
                sb_puts(sb, ", ");
            pexpr(c, sb, a[j], PR_ASSIGN);
        }
        sb_putc(sb, ')');
        break;
    }
    case N_CONVERTVECTOR: {
        uint32_t a[2];
        if (nkids(c, i, a, 2) < 1)
            break;
        sb_puts(sb, "VEC_CONVERT(");
        pexpr(c, sb, a[0], PR_ASSIGN);
        sb_putc(sb, ')');
        break;
    }
    default:
        sb_puts(sb, "...");
        break;
    }
    if (my < prec)
        sb_putc(sb, ')');
}

/* A floating literal as gcc prints it (real_to_decimal at the type's full
 * digit count, trailing zeros dropped: 'd.ddde+X', and the f/l suffix);
 * false for anything else. */
static bool pfloat(Checker *c, StrBuf *sb, const char *s, size_t len)
{
    char t[96], o[64], *e, *ep, *z;
    long double v;
    int ex;
    char sfx = 0;
    bool hex = len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    (void)c;
    if (len >= sizeof t || len == 0)
        return false;
    memcpy(t, s, len);
    t[len] = 0;
    if (t[len - 1] == 'f' || t[len - 1] == 'F' || t[len - 1] == 'l' ||
        t[len - 1] == 'L') {
        if (!hex || strpbrk(t, "pP")) {
            sfx = (char)tolower((unsigned char)t[len - 1]);
            t[--len] = 0;
        }
    }
    if (!strpbrk(t, hex ? ".pP" : ".eE"))
        return false;
    v = strtold(t, &e);
    if (*e)
        return false;
    if (sfx == 'f')
        snprintf(o, sizeof o, "%.8e", (double)(float)v);
    else if (sfx == 'l')
        snprintf(o, sizeof o, "%.20Le", v);
    else
        snprintf(o, sizeof o, "%.16e", (double)v);
    ep = strchr(o, 'e');
    ex = atoi(ep + 1);
    *ep = 0;
    for (z = ep - 1; z > o && *z == '0' && z[-1] != '.'; z--)
        *z = 0;
    sb_puts(sb, o);
    sb_printf(sb, "e%c%d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
    if (sfx)
        sb_putc(sb, sfx);
    return true;
}

/* %E of node i, in a buffer valid until the next call (two rotate). */
const char *estr(Checker *c, uint32_t i)
{
    StrBuf *sb = &c->esb[c->enext++ & 1];
    sb->len = 0;
    pexpr(c, sb, i, PR_COMMA);
    return sb_cstr(sb);
}

const char *cexpr_str(Checker *c, uint32_t i)
{
    return estr(c, i);
}

/* cexpr_str as written (gcc prints the unfolded expression in some messages) */
const char *cexpr_str_plain(Checker *c, uint32_t i)
{
    const char *r;
    pcond_plain = true;
    r = estr(c, i);
    pcond_plain = false;
    return r;
}

const char *vstr(Checker *c, TypeId t, uint64_t v)
{
    char *b = c->vbuf[c->vnext++ & 1];
    if (is_int(c, t) && is_signed(c, t) && int_bits(c, t) <= 64)
        snprintf(b, sizeof c->vbuf[0], "%lld", (long long)(int64_t)v);
    else
        snprintf(b, sizeof c->vbuf[0], "%llu", (unsigned long long)v);
    return b;
}

/* ---- shared checks ----------------------------------------------------------- */

/* The element type of an array type, with the array's qualifiers. */
TypeId elem_of(Checker *c, TypeId t)
{
    const TypeEnt *e = type_ent(TT, t);
    unsigned q = TYPE_QUALS(t);
    while (e->kind == TY_TYPEDEF) {
        q |= TYPE_QUALS(e->base);
        e = type_ent(TT, e->base);
    }
    return type_qual(type_base(TT, t), q);
}

/* gcc's EXPR_LOCATION of an expression, as far as it matters. */
SrcLoc expr_loc(Checker *c, uint32_t i)
{
    switch (ntag(c, i)) {
    case N_PAREN:
        return i > 0 && c->nodes[i].size > 1 ? expr_loc(c, i - 1)
                                             : cnode_loc(c, i);
    case N_CALL:
        return first_loc(c, i);
    case N_MEMBER_EXPR: case N_ADDR_LABEL:
        return ctok_loc(c, c->nodes[i].tok - 1);
    case N_UNARY:   /* __extension__ makes no node of its own in gcc */
        if (cexpr_is_extension(c, i) && i > 0)
            return expr_loc(c, i - 1);
        return cnode_loc(c, i);
    default:
        return cnode_loc(c, i);
    }
}

/* c_incomplete_type_error (value: a variable or parameter, else
 * NO_NODE). */
void incomplete_error(Checker *c, SrcLoc loc, uint32_t value, TypeId t)
{
    TypeId ct;
    if (is_err(c, t))
        return;
    if (value != NO_NODE && ntag(c, strip_paren(c, value)) == N_IDENT) {
        uint32_t v = strip_paren(c, value);
        uint32_t ref = lookup_ord(c, cnode_ident(c, v));
        if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ) {
            cerror(c, loc, "'%s' has an incomplete type %s",
                   cident(c, cnode_ident(c, v)), type_q(TT, t));
            return;
        }
    }
    for (;;) {
        ct = type_canon(TT, t);
        switch (tkind(c, ct)) {
        case TY_VOID:
            cerror(c, loc, "invalid use of void expression");
            return;
        case TY_ARRAY:
            if (type_ent(TT, ct)->flags & TF_INCOMPLETE) {
                cerror(c, loc, (type_ent(TT, ct)->flags & TF_FLEX) ?
                       "invalid use of flexible array member" :
                       "invalid use of array with unspecified bounds");
                return;
            }
            t = type_base(TT, ct);
            continue;
        default:
            break;
        }
        break;
    }
    if (type_ent(TT, TYPE_UNQUAL(t))->kind == TY_TYPEDEF)
        cerror(c, loc, "invalid use of incomplete typedef %s", type_q(TT, t));
    else
        cerror(c, loc, "invalid use of undefined type %s", type_q(TT, t));
}

bool complete(Checker *c, TypeId t)
{
    return type_is_complete(TT, t);
}

/* The size in bytes of what a pointer of type pt points to, for
 * arithmetic (void and functions: 1). */
uint64_t elem_size(Checker *c, TypeId pt)
{
    bool ok;
    TypeId b = pointee(c, pt);
    uint64_t n;
    if (is_void(c, b) || is_func(c, b))
        return 1;
    n = type_size(TT, b, &ok);
    return ok ? n : 1;
}

/* pointer_int_sum's checks of the pointer operand (loc: the operator);
 * false after an error. */
bool ptr_arith_ok(Checker *c, uint32_t i, SrcLoc loc, TypeId pt)
{
    TypeId b = pointee(c, pt);
    if (is_void(c, b)) {
        ped_arith(c, i, loc, "pointer of type 'void *' used in arithmetic");
        return true;
    }
    if (is_func(c, b)) {
        ped_arith(c, i, loc, "pointer to a function used in arithmetic");
        return true;
    }
    if (!complete(c, b)) {
        incomplete_error(c, loc, NO_NODE, b);
        return false;
    }
    return true;
}

/* An lvalue at a constant address: an rvalue constant too if it is an
 * array or a function (it decays to its address). */
void addr_rvalue(Checker *c, uint32_t i)
{
    if ((c->ef[i] & EF_ADDRLV) && (is_array(c, c->ty[i]) ||
                                    is_func(c, c->ty[i])) &&
        tkind(c, c->ty[i]) != TY_VLA)
        c->ck[i] = K_ADDR;
}

/* ---- literals -------------------------------------------------------------- */

static void lit_report(Checker *c, uint32_t i, const Lit *l)
{
    SrcLoc loc = cnode_loc(c, i);
    if (!l->msg[0])
        return;
    switch (l->level) {
    case 2: {
        size_t k = c->diag->all.len, lim = k > 64 ? k - 64 : 0;
        if (l->id && !strcmp(l->id, "inputloc"))
            loc = cinput_loc(c, c->nodes[i].tok);
        for (; k-- > lim;)      /* the parser reported it when it skipped the token */
            if (c->diag->all.data[k]->loc == loc &&
                !strcmp(c->diag->all.data[k]->msg, l->msg))
                return;
        cerror(c, loc, "%s", l->msg);
        break;
    }
    case 1:
        if (l->flags & (LIT_TOO_LARGE | LIT_UNSIGNED_WARN))
            cpedwarn(c, loc, l->id, "%s", l->msg);
        else
            cwarn(c, loc, l->id, "%s", l->msg);
        break;
    default:
        /* libcpp's pedwarns carry no option tag; interpret_float's
         * non-standard suffix pedwarn is c-family's: [-Wpedantic] at
         * input_location */
        if (c->opt.pedantic && !cexpr_in_extension(c, i)) {
            if (l->id && !strcmp(l->id, "inputloc"))
                cpedwarn(c, cinput_loc(c, c->nodes[i].tok), "pedantic", "%s",
                         l->msg);
            else
                cpedwarn(c, loc, "", "%s", l->msg);
        }
        break;
    }
    if (l->flags & LIT_FIXED)
        cerror(c, loc, "fixed-point types not supported for this target");
    if ((l->flags & LIT_UNSIGNED_WARN) && !cin_system(c, loc))
        cwarn(c, cinput_loc(c, c->nodes[i].tok), "traditional",
              "this decimal constant would be unsigned in ISO C90");
}

/* libcpp's -Wtraditional: a floating constant's suffix, or an integer
 * constant's unsigned or imaginary one, as spelled. */
static void traditional_suffix(Checker *c, uint32_t i, const char *s,
                               size_t len, bool flt)
{
    size_t k = 0;
    bool hex = len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    char suf[16];
    if (!diag_enabled(c->diag, "traditional"))
        return;
    if (len > 1 && s[0] == '0' && (hex || s[1] == 'b' || s[1] == 'B'))
        k = 2;
    while (k < len) {
        char ch = s[k];
        if (isdigit((unsigned char)ch) || ch == '.' ||
            (hex && isxdigit((unsigned char)ch)))
            k++;
        else if (ch == (hex ? 'p' : 'e') || ch == (hex ? 'P' : 'E')) {
            k++;
            if (k < len && (s[k] == '+' || s[k] == '-'))
                k++;
        } else
            break;
    }
    if (k >= len || len - k >= sizeof suf)
        return;
    memcpy(suf, s + k, len - k);
    suf[len - k] = 0;
    if (!flt && !strpbrk(suf, "uUiIjJ"))
        return;
    if (!cin_system(c, cnode_loc(c, i)))
        cwarn(c, cnode_loc(c, i), "traditional",
              "traditional C rejects the \"%s\" suffix", suf);
}

static void e_number(Checker *c, uint32_t i)
{
    size_t len;
    const char *s = ttext(c, c->nodes[i].tok, &len);
    Lit l;
    TypeId t;
    lit_number(c->tgt, s, len, &l);
    traditional_suffix(c, i, s, len, l.flags & LIT_FLOAT);
    lit_report(c, i, &l);
    if (l.flags & LIT_BAD) {
        set_err(c, i);
        return;
    }
    t = TYPE_MK(l.ty, 0);
    if (l.flags & LIT_IMAGINARY) {
        long double im = 0;
        bool ok = false;
        c->ty[i] = type_complex(TT, t);
        if (l.flags & LIT_FLOAT) {
            ok = !(l.ty >= TY_DEC32 && l.ty <= TY_DEC128);
            im = fround(c, t, l.f);
        } else if (int_bits(c, t) <= 64) {
            ok = true;
            im = cplx_ld(c, t, cexpr_trunc(c, t, l.v));
        }
        if (ok) {
            c->ef[i] = 0;
            cplx_set(c, i, 0, im);
        }
        return;
    }
    c->ty[i] = t;
    if (l.flags & LIT_FLOAT) {
        long double v = fround(c, t, l.f);
        size_t k;
        bool nonzero = false, hex = len > 1 && s[0] == '0' &&
                                    (s[1] == 'x' || s[1] == 'X');
        if (l.ty >= TY_DEC32 && l.ty <= TY_DEC128)
            return;
        if (!hex && len && !isalpha((unsigned char)s[len - 1]))
            cwarn(c, cinput_loc(c, c->nodes[i].tok),
                  "unsuffixed-float-constants", "unsuffixed floating constant");
        for (k = hex ? 2 : 0; k < len; k++) {
            char ch = s[k];
            if ((!hex && (ch == 'e' || ch == 'E')) ||
                (hex && (ch == 'p' || ch == 'P')))
                break;
            if ((ch >= '1' && ch <= '9') ||
                (hex && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))))
                nonzero = true;
        }
        if (isinf(v))
            cwarn(c, cinput_loc(c, c->nodes[i].tok), "overflow",
                  "floating constant exceeds range of %s", type_q(TT, t));
        else if (v == 0 && nonzero)
            cwarn(c, cinput_loc(c, c->nodes[i].tok), "overflow",
                  "floating constant truncated to zero");
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, v);
        c->ef[i] = EF_REALCST;
        return;
    }
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, t, l.v);
    c->ef[i] = EF_INTOPS;
    if (len && s[0] >= '1' && s[0] <= '9')
        c->ef[i] |= EF_DECIMAL;
    else if (len == 1 || (len > 1 && s[1] != 'x' && s[1] != 'X' &&
                          s[1] != 'b' && s[1] != 'B' &&
                          !(s[1] >= '0' && s[1] <= '9')))
        c->ef[i] |= EF_DECIMAL;   /* 0, 0u */
}

/* libcpp: \e is not ISO, and \a and \x mean something else in traditional C. */
static void traditional_escapes(Checker *c, uint32_t tok, const char *s,
                                size_t len)
{
    size_t k;
    SrcLoc loc;
    loc = ctok_loc(c, tok);
    if (cin_system(c, loc))
        return;
    for (k = 0; k + 1 < len; k++)
        if (s[k] == '\\') {
            if (s[k + 1] == 'e' || s[k + 1] == 'E')
                cpedwarn(c, loc, "", "non-ISO-standard escape sequence, '\\%c'",
                          s[k + 1]);
            if (diag_enabled(c->diag, "traditional") &&
                (s[k + 1] == 'a' || s[k + 1] == 'x'))
                cwarn(c, loc, "traditional", "the meaning of '\\%c' is "
                      "different in traditional C", s[k + 1]);
            k++;
        }
}

/* libcpp's _cpp_valid_ucn for the escapes of a literal token: the -pedantic
 * notes on delimited and named forms, then the errors for a malformed or
 * invalid one.  Reported once per token, at loc (the token after a string,
 * a character constant's own start). */
static void ucn_literal(Checker *c, uint32_t tok, const char *s, size_t len,
                        SrcLoc loc)
{
    size_t k = 0, e = len ? len - 1 : 0, j;
    bool any = false, ped = c->opt.pedantic;
    for (j = 0; j + 1 < len; j++)
        if (s[j] == '\\') {
            any |= s[j + 1] == 'u' || s[j + 1] == 'U' || s[j + 1] == 'N';
            j++;
        }
    if (!any || cin_system(c, loc))
        return;
    for (j = c->ucn_seen.len; j-- > 0;)     /* the token's spelling place */
        if (c->ucn_seen.data[j] == ctok_loc(c, tok))
            return;
    vec_push(&c->ucn_seen, ctok_loc(c, tok));
    while (k < e && s[k] != '"' && s[k] != '\'')
        k++;
    for (k = 0; k < e && s[k] != '"' && s[k] != '\''; k++)
        ;
    for (k++; k < e; k++) {
        size_t b, p;
        char kind;
        uint64_t v = 0;
        unsigned nd = 0;
        if (s[k] != '\\') {
            continue;
        }
        b = k++;
        kind = k < e ? s[k] : 0;
        if (kind != 'u' && kind != 'U' && kind != 'N')
            continue;
        p = k + 1;
        if (kind == 'N') {
            size_t ns;
            if (p >= e || s[p] != '{') {
                cerror(c, loc, "'\\N' not followed by '{'");
                cerror(c, loc, "incomplete universal character name \\N");
                k = p - 1;
                continue;
            }
            ns = ++p;
            while (p < e && (isalnum((unsigned char)s[p]) || s[p] == ' ' ||
                             s[p] == '-'))
                p++;
            if (p >= e || s[p] != '}') {
                cerror(c, loc, "'\\N{' not terminated with '}' after "
                       "%.*s", (int)(p - b), s + b);
                cerror(c, loc, "%.*s is not a valid universal character",
                       (int)(p - b), s + b);
                k = p - 1;
                continue;
            }
            if (p == ns)
                cerror(c, loc, "empty named universal character escape "
                       "sequence");
            else if (ped)
                cpedwarn(c, loc, "", "named universal character escapes are "
                         "only valid in C++23");
            /* a name is not looked up: the Unicode database is not here */
            k = p;
            continue;
        }
        if (p < e && s[p] == '{') {
            size_t ds;
            ds = ++p;
            while (p < e && isxdigit((unsigned char)s[p])) {
                v = v << 4 | (uint64_t)(isdigit((unsigned char)s[p])
                                        ? s[p] - '0' : (s[p] | 32) - 'a' + 10);
                if (v > 0xFFFFFFFFu)
                    v = 0xFFFFFFFFu;
                p++;
            }
            if (p >= e || s[p] != '}') {
                cerror(c, loc, "'\\%c{' not terminated with '}' after %.*s",
                       kind, (int)(p - b), s + b);
                k = p - 1;
                continue;
            }
            if (p == ds)
                cerror(c, loc, "empty delimited escape sequence");
            else if (ped)
                cpedwarn(c, loc, "", "delimited escape sequences are only "
                         "valid in C++23");
            p++;
        } else {
            unsigned want = kind == 'u' ? 4 : 8;
            while (nd < want && p < e && isxdigit((unsigned char)s[p])) {
                v = v << 4 | (uint64_t)(isdigit((unsigned char)s[p])
                                        ? s[p] - '0' : (s[p] | 32) - 'a' + 10);
                p++;
                nd++;
            }
            if (nd < want) {
                cerror(c, loc, "incomplete universal character name %.*s",
                       (int)(p - b), s + b);
                k = p - 1;
                continue;
            }
        }
        if (v > 0x10FFFF && v < 0x80000000u)
            cpedwarn(c, loc, "", "%.*s is outside the UCS codespace",
                     (int)(p - b), s + b);
        else if ((v < 0xA0 && v != 0x24 && v != 0x40 && v != 0x60) ||
                 v >= 0x80000000u || (v >= 0xD800 && v <= 0xDFFF))
            cerror(c, loc, "%.*s is not a valid universal character",
                   (int)(p - b), s + b);
        k = p - 1;
    }
}

static void e_char(Checker *c, uint32_t i)
{
    size_t len;
    const char *s = ttext(c, c->nodes[i].tok, &len);
    Lit l;
    ucn_literal(c, c->nodes[i].tok, s, len, ctok_loc(c, c->nodes[i].tok));
    lit_char(c->tgt, s, len, &l);
    traditional_escapes(c, c->nodes[i].tok, s, len);
    lit_report(c, i, &l);
    if (l.flags & LIT_BAD) {
        set_err(c, i);
        return;
    }
    c->ty[i] = TYPE_MK(l.ty, 0);
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, c->ty[i], l.v);
    c->ef[i] = EF_INTOPS;
}

/* gcc parses the strings of an asm (template, constraints, clobbers, label)
 * apart from expressions: no -Woverlength-strings. */
static bool in_asm_string(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    return p != NO_NODE &&
           (ntag(c, p) == N_TOP_ASM || ntag(c, p) == N_ASM ||
            ntag(c, p) == N_ASM_SECTION || ntag(c, p) == N_ASM_OPERAND ||
            ntag(c, p) == N_ASM_LABEL);
}

static void e_string(Checker *c, uint32_t i)
{
    uint32_t np = node_pieces(c, i), k;
    int prefix = 0;
    uint64_t units = 0;
    unsigned width;
    TypeKind ek = TY_CHAR;
    if (np > 1 && diag_enabled(c->diag, "traditional")) {
        SrcLoc il = cinput_loc(c, c->nodes[i].tok + 1);
        if (!cin_system(c, il))
            cwarn(c, il, "traditional", "traditional C rejects string "
                  "constant concatenation");
    }
    for (k = 0; k < np; k++) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        int p = lit_str_prefix(s, len);
        traditional_escapes(c, c->nodes[i].tok + k, s, len);
        ucn_literal(c, c->nodes[i].tok + k, s, len,
                    tloc(c, c->nodes[i].tok + np));
        if (p && prefix && p != prefix) {
            /* gcc's lexer reports it twice, at the lookahead's line */
            SrcLoc il = cinput_loc(c, c->nodes[i].tok + np);
            cerror(c, il, "unsupported non-standard concatenation of string "
                   "literals");
            cerror(c, il, "unsupported non-standard concatenation of string "
                   "literals");
            set_err(c, i);
            return;
        }
        if (p)
            prefix = p;
    }
    if (prefix == '8')
        prefix = 0;
    switch (prefix) {
    case 'L': ek = c->tgt->wchar_type; break;
    case 'u': ek = c->tgt->char16_type; break;
    case 'U': ek = c->tgt->char32_type; break;
    default: break;
    }
    width = c->tgt->size[ek];
    for (k = 0; k < np; k++) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        lit_str_units(s, len, width, &units);
    }
    if (units > 4095 && !cexpr_in_extension(c, i) && !in_asm_string(c, i))
        cpedwarn(c, cinput_loc(c, c->nodes[i].tok + np), "overlength-strings",
                 "string length '%llu' is greater than the length '%d' ISO "
                 "C99 compilers are required to support",
                 (unsigned long long)units, 4095);
    /* -Wwrite-strings: the literal is an array of const char */
    c->ty[i] = type_array(TT, TYPE_MK(ek, diag_enabled(c->diag, "write-strings")
                                          ? TQ_CONST : 0), units + 1);
    c->ef[i] = EF_LVALUE | EF_STRING | EF_ADDRLV;
    c->cb[i] = CB_NODE | i;
    c->ck[i] = K_ADDR;
}

/* ---- identifiers ------------------------------------------------------------- */

static bool builtin_name(const char *s)
{
    return !strncmp(s, "__builtin_", 10) || !strncmp(s, "__sync_", 7) ||
           !strncmp(s, "__atomic_", 9);
}

/* Is node i the callee of a call (not parenthesized)? */
static bool is_callee(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    return p != NO_NODE && ntag(c, p) == N_CALL && first_child(c, p) == i;
}

static void old_decl_note(Checker *c, Diagnostic *d, const CSym *s)
{
    const char *name = cident(c, s->name);
    if (s->flags & CSF_DEFINED)
        cnote(c, d, s->def_loc ? s->def_loc : s->loc,
              "previous definition of '%s' with type %s", name,
              type_q(TT, s->ty));
    else if (s->flags & CSF_IMPLICIT)
        cnote(c, d, s->loc, "previous implicit declaration of '%s' with type %s",
              name, type_q(TT, s->ty));
    else
        cnote(c, d, s->loc, "previous declaration of '%s' with type %s", name,
              type_q(TT, s->ty));
}

/* "incompatible implicit declaration of built-in function": the declaration
 * int() of a library built-in whose type differs. */
static void builtin_mismatch(Checker *c, SrcLoc loc, const BTab *bt)
{
    Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "builtin-declaration-mismatch",
                            "incompatible implicit declaration of built-in "
                            "function '%s'", bt->name);
    if (d && bt->hdr[0])
        cnote(c, d, loc, "include '%s' or provide a declaration of '%s'",
              bt->hdr, bt->name);
}

/* An implicit function declaration (C90; a C99 pedwarn). */
static void implicit_decl(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    uint32_t ref = id < c->nidents && c->ext[id] ? c->ext[id] - 1 : SYM_NONE;
    const BTab *bt = bt_find(c, name, false);
    csum_touch(c, SUM_EXT, id);
    CSym s;
    Diagnostic *d;
    if (ref != SYM_NONE && csym(c, ref)->kind == CS_FUNC) {
        CSym *o = csym(c, ref);
        if (!(o->flags & CSF_IMPLICIT)) {
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'", name);
            old_decl_note(c, d, o);
            o->flags |= CSF_IMPLICIT;
        } else if (bt && bt->mismatch)
            builtin_mismatch(c, loc, bt);
        cbind(c, NS_ORD, id, ref);
        o = csym(c, ref);
        c->ty[i] = o->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        return;
    }
    if (bt) {
        d = cpedwarn(c, loc, "implicit-function-declaration",
                     "implicit declaration of function '%s'", name);
        if (d && bt->hdr[0])
            cnote(c, d, header_note_loc(c, loc, bt->hdr), "include '%s' or "
                  "provide a declaration of '%s'", bt->hdr, name);
        if (bt->mismatch)
            builtin_mismatch(c, loc, bt);
    } else {
        const char *hdr = std_header(name);
        const char *sug = hdr ? NULL : fuzzy_name(c, name, true);
        if (sug)
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'; did you mean "
                         "'%s'?", name, sug);
        else
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'", name);
        if (hdr)
            header_note(c, d, loc, name, hdr);
    }
    memset(&s, 0, sizeof s);
    s.name = id;
    s.kind = CS_FUNC;
    s.sc = SC_EXTERN;
    s.linkage = LK_EXTERNAL;
    s.flags = CSF_IMPLICIT | CSF_USED | CSF_CUSED;
    s.ty = bt && bt->mismatch ? bt_func_type(c, bt)
                              : type_func(TT, TYPE_B(INT), NULL, 0, TF_NOPROTO);
    s.loc = loc;
    ref = csym_new(c, true, &s);
    if (id < c->nidents && !c->ext[id])
        c->ext[id] = ref + 1;
    cbind(c, NS_ORD, id, ref);
    c->ty[i] = s.ty;
    c->ck[i] = K_ADDR;
    c->cb[i] = ref + 1;
}

static void undeclared(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    const char *hdr, *sug;
    Diagnostic *d;
    set_err(c, i);
    if (!in_function(c)) {
        CSym s;
        hdr = std_header(name);
        sug = hdr ? NULL : fuzzy_name(c, name, false);
        if (sug)
            d = cerror_d(c, loc, "'%s' undeclared here (not in a function); "
                         "did you mean '%s'?", name, sug);
        else
            d = cerror_d(c, loc, "'%s' undeclared here (not in a function)",
                         name);
        if (hdr)
            header_note(c, d, loc, name, hdr);
        memset(&s, 0, sizeof s);
        s.name = id;
        s.kind = CS_OBJ;
        s.flags = CSF_ERROR;
        s.ty = ERRT;
        s.loc = loc;
        cbind(c, NS_ORD, id, csym_new(c, cat_file_scope(c), &s));
        return;
    }
    {
        uint64_t key = ((uint64_t)c->u->first_tok * UINT64_C(0x100000001)) ^
                       c->func_sym;
        size_t k;
        if (key != c->undecl_key) {
            c->undecl_key = key;
            c->undecl.len = 0;
        }
        for (k = 0; k < c->undecl.len; k++)
            if (c->undecl.data[k] == id)
                return;
        vec_push(&c->undecl, id);
    }
    hdr = std_header(name);
    sug = hdr ? NULL : fuzzy_name(c, name, false);
    if (sug)
        d = cerror_d(c, loc, "'%s' undeclared (first use in this function); "
                     "did you mean '%s'?", name, sug);
    else
        d = cerror_d(c, loc, "'%s' undeclared (first use in this function)",
                     name);
    if (hdr)
        header_note(c, d, loc, name, hdr);
    if (d && !c->undecl_noted) {
        c->undecl_noted = true;
        cnote(c, d, loc, "each undeclared identifier is reported only once "
              "for each function it appears in");
    }
}

/* gcc's reject_gcc_builtin: a built-in function without a library fallback
 * (__builtin_trap, __sync_*, ...) may only be called, cast to void or
 * evaluated as a statement; anything that would take its address is an error.
 * The consumer of the value (through parentheses and '*') decides, and where
 * gcc reports: input_location, except an arithmetic, relational or equality
 * operand (the operand's own place for the left, the operator for the right)
 * and the right side of && and ||. */
static void reject_builtin(Checker *c, uint32_t i, const char *name)
{
    uint32_t n = i, p, k[2];
    SrcLoc loc = cinput_loc(c, c->nodes[i].tok);
    int op;
    if (!strncmp(name, "__builtin_", 10)) {
        const BTab *bt = bt_find(c, name + 10, true);
        if (bt && bt->gnu != 2)
            return;         /* has a library fallback */
    }
    for (;;) {
        p = c->par[n];
        if (p == NO_NODE)
            return;
        if (ntag(c, p) == N_PAREN ||
            (ntag(c, p) == N_UNARY && npunct(c, p) == P_STAR)) {
            n = p;
            continue;
        }
        break;
    }
    switch (ntag(c, p)) {
    case N_CALL:
        if (first_child(c, p) == n)
            return;
        break;
    case N_CAST:
        if (nkids(c, p, k, 2) < 2 || k[1] != n || is_void(c, type_of_typename(c, k[0])))
            return;
        break;
    case N_BINARY:
        op = npunct(c, p);
        if (op == P_COMMA || nkids(c, p, k, 2) < 2)
            return;
        if (k[0] != n)
            loc = cnode_loc(c, p);
        else if (op != P_ANDAND && op != P_OROR)
            loc = cnode_loc(c, n);
        break;
    case N_ASSIGN:
        if (nkids(c, p, k, 2) < 2 || k[1] != n)
            return;
        if (npunct(c, p) != P_ASSIGN)
            loc = cnode_loc(c, n);
        break;
    case N_UNARY: case N_COND: case N_RETURN: case N_IF: case N_WHILE:
    case N_DO: case N_SWITCH: case N_FOR: case N_INIT_DECL:
    case N_INIT_LIST: case N_DESIGNATED:
        break;
    default:
        return;
    }
    if (!strcmp(name, "__builtin_complex"))
        cerror(c, ctok_loc(c, last_tok(c, i) + 1), "cannot take address of '%s'", name);
    else
        cerror(c, loc, "built-in function '%s' must be directly called", name);
}

/* An operand of __builtin_has_attribute names a function without using it. */
static uint32_t sizeof_outer(const Checker *c, uint32_t i);

static bool in_has_attr(const Checker *c, uint32_t i)
{
    unsigned depth;
    for (depth = 0; depth < 8; depth++) {
        uint32_t p = c->par[i];
        if (p == NO_NODE)
            return false;
        switch (ntag(c, p)) {
        case N_HAS_ATTR:
            return true;
        case N_PAREN: case N_MEMBER_EXPR: case N_INDEX: case N_UNARY:
        case N_CAST: case N_BINARY:
            i = p;
            break;
        default:
            return false;
        }
    }
    return false;
}

static void e_ident(Checker *c, uint32_t i)
{
    uint32_t id = cnode_ident(c, i), ref = lookup_ord(c, id);
    const char *name = cident(c, id);
    CSym *s;
    if (ref == SYM_NONE) {
        uint32_t p = c->par[i];
        if (!strcmp(name, "__func__") || !strcmp(name, "__FUNCTION__") ||
            !strcmp(name, "__PRETTY_FUNCTION__")) {
            size_t n = 0;
            if (strcmp(name, "__func__"))
                cpedantic(c, cnode_loc(c, i), "ISO C does not support '%s' "
                          "predefined identifier", name);
            if (in_function(c))
                n = strlen(cident(c, csym(c, c->func_sym)->name));
            else if (!strcmp(name, "__func__"))
                cpedwarn(c, cnode_loc(c, i), "",
                         "'%s' is not defined outside of function scope",
                         name);
            c->ty[i] = type_array(TT, type_qual(TYPE_B(CHAR), TQ_CONST), n + 1);
            c->ef[i] = EF_LVALUE | EF_ADDRLV;
            c->cb[i] = CB_NODE | i;
            c->ck[i] = K_ADDR;
            return;
        }
        if (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM) {
            size_t al;
            const char *an = ttext(c, c->nodes[p].tok, &al);
            static const char *const ex[] = {
                "nonnull", "aligned", "vector_size", "warn_if_not_aligned",
                "alloc_size", "alloc_align", "assume_aligned", "malloc",
                "fallthrough"};
            size_t q;
            if (al > 4 && !strncmp(an, "__", 2) && !strncmp(an + al - 2, "__", 2)) {
                an += 2;
                al -= 4;
            }
            /* attributes whose arguments are expressions */
            for (q = 0; q < sizeof ex / sizeof *ex && strncmp(name, "__builtin_", 10); q++)
                if (strlen(ex[q]) == al && !strncmp(an, ex[q], al)) {
                    undeclared(c, i, id);
                    return;
                }
        }
        if (!strncmp(name, "__builtin_", 10)) {
            /* a built-in with a library counterpart has that function's type */
            const BTab *bt = bt_find(c, name + 10, true);
            TypeId ft = bt && (bt->gnu != 2) ? bt_func_type(c, bt)
                                           : overflow_func_type(c, name + 10);
            if (!is_err(c, ft)) {
                c->ty[i] = ft;
                c->ck[i] = K_ADDR;
                c->ef[i] = EF_ADDRLV;
                return;
            }
        }
        if (builtin_name(name) || (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM)) {
            if (builtin_name(name))
                reject_builtin(c, i, name);
            set_err(c, i);
            return;
        }
        if (is_callee(c, i)) {
            implicit_decl(c, i, id);
            return;
        }
        undeclared(c, i, id);
        return;
    }
    s = csym(c, ref);
    /* gcc marks a variable named in __builtin_has_attribute used; a function
     * stays unused ("declared static but never defined") */
    if ((s->kind != CS_FUNC || !in_has_attr(c, i)) &&
        !(s->kind == CS_OBJ && is_err(c, s->ty))) {   /* gcc: an erroneous type is no use */
        s->flags |= CSF_USED;
        if (s->kind == CS_FUNC && sizeof_outer(c, i) == NO_NODE)
            s->flags |= CSF_CUSED;
    }
    if (c->func_sym != SYM_NONE && !(ref & SYM_LOCAL) &&
        (s->kind == CS_OBJ || s->kind == CS_FUNC) && s->linkage == LK_INTERNAL)
        cdecl_record_inline_static(c, ctok_loc(c, c->nodes[i].tok), s->name,
                                   false);
    cdep_use(c, cinput_loc(c, c->nodes[i].tok), s, &s->loc);
    switch (s->kind) {
    case CS_TYPEDEF:
        set_err(c, i);
        return;
    case CS_ENUMCONST:
        if ((s->flags & CSF_IN_STRUCT) && !c->recs.len)
            cxx_in_struct_use(c, ctok_loc(c, c->nodes[i].tok),
                              "enum constant", "enum constant", s->loc);
        c->ty[i] = s->vty ? s->vty : s->ty;
        if (is_err(c, c->ty[i])) {
            set_err(c, i);
            return;
        }
        c->ck[i] = K_ICE;
        c->cv[i] = cexpr_trunc(c, c->ty[i], s->val);
        c->ef[i] = EF_INTOPS;
        return;
    case CS_FUNC:
        c->ty[i] = cbind_type(c, id) ? cbind_type(c, id) - 1 : s->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        c->ef[i] = EF_ADDRLV;
        return;
    default:
        break;
    }
    c->ty[i] = cbind_type(c, id) ? cbind_type(c, id) - 1 : s->ty;
    if (is_err(c, s->ty)) {
        set_err(c, i);
        return;
    }
    c->ef[i] = EF_LVALUE;
    if (s->sc == SC_REGISTER) {
        c->ef[i] |= EF_REGISTER;
        if (type_ckind(TT, c->ty[i]) == TY_ARRAY) {
            /* c_mark_addressable on the array-to-pointer conversion */
            uint32_t p = c->par[i];
            while (p != NO_NODE && ntag(c, p) == N_PAREN)
                p = c->par[p];
            if (p == NO_NODE ||
                !(ntag(c, p) == N_SIZEOF_EXPR || ntag(c, p) == N_ALIGNOF_EXPR ||
                  ntag(c, p) == N_TYPEOF || ntag(c, p) == N_INDEX ||
                  (ntag(c, p) == N_UNARY && npunct(c, p) == P_AMP)))
                cerror(c, cinput_loc(c, last_tok(c, i) + 1),
                       "address of register variable '%s' requested",
                       cident(c, id));
        }
    }
    if (tquals(c, s->ty) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (!(s->flags & CSF_THREAD) &&
        (!(ref & SYM_LOCAL) || s->sc == SC_STATIC || s->sc == SC_EXTERN ||
         s->linkage != LK_NONE)) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = ref + 1;
        addr_rvalue(c, i);
    }
}

/* ---- postfix operators --------------------------------------------------------- */

static void e_paren(Checker *c, uint32_t i)
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

static bool is_npc(Checker *c, uint32_t n);
bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out);
bool rvalue_ok(Checker *c, uint32_t i);
bool rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc);
unsigned vec_esize(Checker *c, TypeId el);
TypeId vec_elem(Checker *c, TypeId vt);

/* ---- implicit conversions (gcc's convert_for_assignment) ----------------------------------- */


static void sp(char *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, 640, fmt, ap);
    va_end(ap);
}

/* %qv: the qualifiers in q. */
/* a function type's const / volatile are the attributes const / noreturn */
static const char *fqual_str(char *b, unsigned q)
{
    b[0] = 0;
    if (q & TQ_CONST)
        strcat(b, "__attribute__((const))");
    if (q & TQ_VOLATILE)
        strcat(b, b[0] ? " __attribute__((noreturn))" : "__attribute__((noreturn))");
    return b;
}

static const char *qual_str(char *b, unsigned q)
{
    b[0] = 0;
    if (q & TQ_CONST)
        strcat(b, "const");
    if (q & TQ_VOLATILE)
        strcat(b, b[0] ? " volatile" : "volatile");
    if (q & TQ_RESTRICT)
        strcat(b, b[0] ? " restrict" : "restrict");
    if (q & TQ_ATOMIC)
        strcat(b, b[0] ? " _Atomic" : "_Atomic");
    return b;
}

/* gcc's strip_array_types: the element type of (nested) arrays. */
static TypeId strip_arr(Checker *c, TypeId t)
{
    t = type_canon(TT, t);
    while (is_array(c, t))
        t = type_canon(TT, type_base(TT, t));
    return t;
}

/* gcc's TYPE_QUALS (arrays carry their element's qualifiers). */
static unsigned gq(Checker *c, TypeId t)
{
    return TYPE_QUALS(strip_arr(c, t));
}

/* TYPE_MAIN_VARIANT, keeping _Atomic (and leaving the elements of arrays
 * unqualified). */
TypeId mvt(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    unsigned a = TYPE_QUALS(k) & TQ_ATOMIC;
    if (is_array(c, k)) {
        const TypeEnt *e = type_ent(TT, k);
        TypeId el = mvt(c, e->base);
        if (e->kind == TY_VLA)
            return type_vla_x(TT, el, e->n, e->flags & TF_SIZED, e->extra);
        if (e->flags & TF_INCOMPLETE)
            return type_array_incomplete(TT, el);
        return type_array(TT, el, e->n);
    }
    return TYPE_UNQUAL(k) | a;
}

/* c_common_signed_type / c_common_unsigned_type of a main variant. */
void sign_map(Checker *c, TypeId t, TypeId *uns, TypeId *sgn)
{
    TypeId m = TYPE_UNQUAL(type_canon(TT, t));
    unsigned bits;
    *uns = *sgn = m;
    switch (tkind(c, m)) {
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR:
        *uns = TYPE_B(UCHAR);
        *sgn = TYPE_B(SCHAR);
        break;
    case TY_SHORT: case TY_USHORT:
        *uns = TYPE_B(USHORT);
        *sgn = TYPE_B(SHORT);
        break;
    case TY_INT: case TY_UINT:
        *uns = TYPE_B(UINT);
        *sgn = TYPE_B(INT);
        break;
    case TY_LONG: case TY_ULONG:
        *uns = TYPE_B(ULONG);
        *sgn = TYPE_B(LONG);
        break;
    case TY_LLONG: case TY_ULLONG:
        *uns = TYPE_B(ULLONG);
        *sgn = TYPE_B(LLONG);
        break;
    case TY_INT128: case TY_UINT128:
        *uns = TYPE_B(UINT128);
        *sgn = TYPE_B(INT128);
        break;
    case TY_ENUM:
        bits = int_bits(c, m);
        if (bits == 8) {
            *uns = TYPE_B(UCHAR);
            *sgn = TYPE_B(SCHAR);
        } else if (bits == 16) {
            *uns = TYPE_B(USHORT);
            *sgn = TYPE_B(SHORT);
        } else if (bits == 32) {
            *uns = TYPE_B(UINT);
            *sgn = TYPE_B(INT);
        } else if (bits == 64) {
            *uns = TYPE_B(ULONG);
            *sgn = TYPE_B(LONG);
        }
        /* a type already of the wanted signedness maps to itself */
        if (is_signed(c, m))
            *sgn = m;
        else
            *uns = m;
        break;
    default:
        break;
    }
}

/* gcc's INTEGER_TYPE: an integer that is neither _Bool nor an enumeration. */
bool gcc_integer(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_CHAR && k <= TY_UINT128;
}

/* The parameter locations of the functions declared so far: gcc's
 * DECL_ARGUMENTS of the declaration it keeps (the definition's if seen, else
 * the first declaration's). */
typedef struct PLoc {
    uint32_t ref, n;
    bool def;
    SrcLoc *loc;
} PLoc;
typedef struct PLocVec {
    VEC(PLoc) v;
    uint32_t *idx;           /* symbol ref -> index + 1 into v */
    size_t idxcap;
} PLocVec;

void cexpr_free_params(Checker *c)
{
    PLocVec *v = c->plocs;
    size_t k;
    if (!v)
        return;
    for (k = 0; k < v->v.len; k++)
        free(v->v.data[k].loc);
    vec_free(&v->v);
    free(v->idx);
    free(v);
    c->plocs = NULL;
}

void cexpr_record_params(Checker *c, uint32_t declared, uint32_t ref, bool def)
{
    PLocVec *v = c->plocs;
    uint32_t fn = NO_NODE, kids[64], n, j, np = 0, m;
    PLoc *pl = NULL, nw;
    SrcLoc *locs;
    if (ref & SYM_LOCAL)
        return;
    if (!v) {
        v = xcalloc(1, sizeof *v);
        c->plocs = v;
    }
    if (ref < v->idxcap && v->idx[ref])
        pl = &v->v.data[v->idx[ref] - 1];
    if (pl && (pl->def || !def))
        return;
    /* the function declarator: the declared node's declarator sibling */
    for (j = cfirst(c, c->par[declared]); j < declared; j++)
        if (ntag(c, j) == N_FUNC)
            fn = j;
    if (fn == NO_NODE)
        return;
    n = nkids(c, fn, kids, 64);
    locs = xcalloc(n + 1, sizeof *locs);
    for (j = 0; j < n && j < 64; j++) {
        uint32_t q = kids[j];
        if (!is_real_param(c, q))
            continue;
        locs[np] = first_loc(c, q);
        for (m = cfirst(c, q); m <= q; m++)
            if (ntag(c, m) == N_NAME) {
                locs[np] = cnode_loc(c, m);
                break;
            }
        np++;
    }
    if (pl) {
        free(pl->loc);
        pl->loc = locs;
        pl->n = np;
        pl->def = def;
        return;
    }
    nw.ref = ref;
    nw.n = np;
    nw.def = def;
    nw.loc = locs;
    if (ref >= v->idxcap) {
        size_t nc = v->idxcap ? v->idxcap : 1024;
        while (nc <= ref)
            nc *= 2;
        v->idx = xrealloc(v->idx, nc * sizeof *v->idx);
        memset(v->idx + v->idxcap, 0, (nc - v->idxcap) * sizeof *v->idx);
        v->idxcap = nc;
    }
    vec_push(&v->v, nw);
    v->idx[ref] = (uint32_t)v->v.len;
}

SrcLoc param_loc(Checker *c, uint32_t ref, uint32_t idx, uint32_t at,
                        SrcLoc dflt)
{
    const PLocVec *v = c->plocs;
    (void)at;
    if (v && ref < v->idxcap && v->idx[ref]) {
        const PLoc *p = &v->v.data[v->idx[ref] - 1];
        return idx < p->n ? p->loc[idx] : dflt;
    }
    return dflt;
}

static SrcLoc conv_note_loc(const Conv *x)
{
    if (x->ci->note_loc)
        return x->ci->note_loc;
    if (x->ci->fsym)
        return param_loc(x->c, x->ci->fsym - 1, (uint32_t)x->ci->parmnum - 1,
                         x->expr, x->eloc);
    return x->eloc;
}

enum { RK_PED, RK_WARN };

/* expansion_point_location_if_in_system_header (PR c/67730): a system
 * header macro such as NULL still warns at its use. */
SrcLoc exp_if_system(Checker *c, SrcLoc l, uint32_t expr)
{
    SrcFile *sf = srcmgr_file_of(c->sm, l);
    if (sf && sf->system_header) {
        const PTok *t = &c->u->toks[first_tok(c, expr)];
        if (t->exp)
            return t->exp;
    }
    return l;
}

/* One diagnostic of a conversion, the text per context in m[] (indexed by
 * CONV_*); the notes gcc adds follow a reported one. */
static void conv_diag(Conv *x, int rk, const char *opt, char (*m)[640],
                      bool near)
{
    Checker *c = x->c;
    int ctx = x->ci->context;
    SrcLoc l = ctx == CONV_ARG ? x->eloc : x->loc;
    Diagnostic *d;
    l = exp_if_system(c, l, x->expr);
    if (rk == RK_PED)
        d = cpedwarn(c, l, opt, "%s", m[ctx]);
    else
        d = cwarn_d(c, DL_WARNING, l, opt, "%s", m[ctx]);
    if (!d)
        return;
    if (ctx == CONV_ARG)
        cnote(c, d, conv_note_loc(x), "expected %s but argument is of type %s",
              type_q(TT, x->type), type_q(TT, x->rhstype));
    else if (ctx == CONV_INIT && near && x->ci->near && *x->ci->near)
        cnote(c, d, l, "(near initialization for '%s')", x->ci->near);
}

/* comptypes_internal's enum_and_int_p: compatible types that differ in
 * being an enum on one side and an integer type on the other. */
bool cexpr_enum_int_mix(Checker *c, TypeId a, TypeId b, int depth)
{
    TypeKind ka, kb;
    a = type_canon(TT, a);
    b = type_canon(TT, b);
    ka = tkind(c, a);
    kb = tkind(c, b);
    if ((ka == TY_ENUM) != (kb == TY_ENUM))
        return type_is_integer(TT, a) && type_is_integer(TT, b);
    if (ka != kb || depth > 8)
        return false;
    if (ka == TY_PTR || ka == TY_ARRAY)
        return cexpr_enum_int_mix(c, type_base(TT, a), type_base(TT, b), depth + 1);
    if (ka == TY_FUNC) {
        uint32_t k, n = type_ent(TT, a)->n;
        if (n != type_ent(TT, b)->n)
            return false;
        if (cexpr_enum_int_mix(c, type_base(TT, a), type_base(TT, b), depth + 1))
            return true;
        for (k = 0; k < n; k++)
            if (cexpr_enum_int_mix(c, type_params(TT, a)[k], type_params(TT, b)[k],
                             depth + 1))
                return true;
    }
    return false;
}

/* comp_target_types of two pointer types (a pedantic note for arrays of
 * differently qualified elements). */
static int comp_target(Conv *x, TypeId lt, TypeId rt)
{
    Checker *c = x->c;
    TypeId mvl = type_base(TT, type_canon(TT, lt));
    TypeId mvr = type_base(TT, type_canon(TT, rt));
    bool val_ped = true, val;
    if (is_array(c, mvl) && is_array(c, mvr))
        val_ped = type_compatible(TT, mvl, mvr);
    val = type_compatible(TT, mvt(c, mvl), mvt(c, mvr));
    if (val && cexpr_cxx_compat(c, x->expr) &&
        cexpr_enum_int_mix(c, mvl, mvr, 0))
        cwarn(c, x->loc, "c++-compat", "pointer target types incompatible in "
              "C++");
    if (val && !val_ped)
        cpedantic(c, x->loc, "invalid use of pointers to arrays with "
                  "different qualifiers in ISO C before C2X");
    return val;
}

/* comp_target_types of two pointer targets (-, comparison, ?:): arrays of
 * differently qualified elements are compatible, with a pedantic note. */
static bool targets_compat(Checker *c, SrcLoc loc, TypeId pa, TypeId pb)
{
    TypeId a = type_canon(TT, pa), b = type_canon(TT, pb);
    bool ped = !(is_array(c, a) && is_array(c, b)) || type_compatible(TT, a, b);
    bool val = type_compatible(TT, mvt(c, a), mvt(c, b));
    if (val && !ped)
        cpedantic(c, loc, "invalid use of pointers to arrays with different "
                  "qualifiers in ISO C before C2X");
    return val;
}

/* The location of gcc's built-in declarations ("<built-in>"). */
static SrcLoc builtin_loc(Checker *c)
{
    uint32_t k, n = srcmgr_nfiles(c->sm);
    for (k = 0; k < n; k++) {
        SrcFile *f = srcmgr_file(c->sm, k);
        if (f && f->kind == SF_VIRTUAL && !strcmp(f->name, "<built-in>"))
            return f->base;
    }
    return 0;
}

/* maybe_warn_builtin_no_proto_arg: an argument of a call to a built-in
 * declared without a prototype that does not promote to the built-in's
 * parameter type. */
static void builtin_noproto_arg(Conv *x)
{
    Checker *c = x->c;
    TypeId pt = x->type, at = x->rhstype, pr = TYPE_UNQUAL(at);
    TypeKind pk = tkind(c, pt), ak = tkind(c, at);
    int pc, ac;
    Diagnostic *d;
    if (is_int(c, at)) {
        /* gcc keeps the argument's own type node (typedef and all) when the
         * promotion changes nothing; an enum as wide as int stays itself */
        TypeId pm = type_int_promote(TT, at);
        if (ak == TY_ENUM && int_bits(c, at) < int_bits(c, TYPE_B(INT)))
            pr = TYPE_B(INT);
        else if (ak != TY_ENUM && mainv(c, pm) != mainv(c, at))
            pr = mainv(c, pm);
    } else if (is_flt(c, at) && ak != TY_DOUBLE && ak != TY_LDOUBLE &&
               float_prec(c, ak) <= 53 && ak >= TY_FLOAT16 && ak <= TY_FLOAT)
        pr = TYPE_B(DOUBLE);
    pc = gcc_integer(c, pt) ? 1 : pk == TY_ENUM ? 2 : pk == TY_BOOL ? 3
         : is_flt(c, pt) ? 4 : 5;
    ac = gcc_integer(c, at) ? 1 : ak == TY_ENUM ? 2 : ak == TY_BOOL ? 3
         : is_flt(c, at) ? 4 : 5;
    if (pc == 1 && ac == 2 && int_bits(c, pt) == int_bits(c, at))
        return;
    if ((pc == ac || (pc == 1 && ac == 2)) &&
        TYPE_UNQUAL(type_canon(TT, pt)) == TYPE_UNQUAL(type_canon(TT, pr)))
        return;
    d = cwarn_d(c, DL_WARNING, x->eloc, "builtin-declaration-mismatch",
                pr == mainv(c, at)
                ? "'%s' argument %d type is %s where %s is expected in a call "
                  "to built-in function declared without prototype"
                : "'%s' argument %d promotes to %s where %s is expected in a "
                  "call to built-in function declared without prototype",
                x->ci->fname, x->ci->parmnum, type_q(TT, pr), type_q(TT, pt));
    if (d)
        cnote(c, d, builtin_loc(c), "built-in '%s' declared here",
              x->ci->fname);
}

/* -Woverflow for a constant stored into a bit-field of width w: gcc converts
 * to the bit-field's own (narrower) type, which prints as 'unsigned char:3'.
 * True when it warned. */
bool cexpr_bf_overflow(Checker *c, SrcLoc loc, uint32_t n, TypeId ft,
                       TypeId rt, unsigned w)
{
    unsigned su_bits;
    bool tu, su;
    int64_t sv;
    uint64_t uv, mask, r;
    const char *base;
    char tname[64], vs[32], rs[32];
    if (!type_is_integer(TT, rt) || type_int_bits(TT, rt) > 64)
        return false;
    su_bits = type_int_bits(TT, ft);
    if (w >= su_bits)
        return false;
    tu = !type_is_signed(TT, ft);
    su = !type_is_signed(TT, rt);
    sv = cexpr_sval(c, n);
    uv = (uint64_t)sv;
    mask = ((uint64_t)1 << w) - 1;
    {
        bool fits;
        if (su)
            fits = tu ? uv <= mask : uv <= (mask >> 1);
        else if (tu)
            fits = sv >= 0 && uv <= mask;
        else
            fits = sv >= -(int64_t)(mask >> 1) - 1 && sv <= (int64_t)(mask >> 1);
        if (fits)
            return false;
    }
    r = uv & mask;
    if (!tu && (r >> (w - 1)))
        r |= ~mask;
    base = w <= 8 ? (tu ? "unsigned char" : "signed char")
         : w <= 16 ? (tu ? "short unsigned int" : "short int")
         : w <= 32 ? (tu ? "unsigned int" : "int")
                   : (tu ? "long unsigned int" : "long int");
    snprintf(tname, sizeof tname, "'%s:%u'", base, w);
    if (su)
        snprintf(vs, sizeof vs, "%" PRIu64, uv);
    else
        snprintf(vs, sizeof vs, "%" PRId64, sv);
    if (tu)
        snprintf(rs, sizeof rs, "%" PRIu64, r);
    else
        snprintf(rs, sizeof rs, "%" PRId64, (int64_t)r);
    if (su) {
        cwarn(c, loc, "overflow", "conversion from %s to %s "
              "changes value from '%s' to '%s'", type_q(TT, rt), tname, vs, rs);
        return true;
    } else if (tu) {
        bool sfit = sv >= -(int64_t)(mask >> 1) - 1 && sv <= (int64_t)(mask >> 1);
        if (!sfit) {
            cwarn(c, loc, "overflow", "unsigned conversion from "
                  "%s to %s changes value from '%s' to '%s'", type_q(TT, rt),
                  tname, vs, rs);
            return true;
        }
    } else {
        bool ufit = sv >= 0 && uv <= mask;
        if (!ufit || c->opt.pedantic) {
            cwarn(c, loc, "overflow", "overflow in conversion "
                  "from %s to %s changes value from '%s' to '%s'",
                  type_q(TT, rt), tname, vs, rs);
            return true;
        }
    }
    return false;
}

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
static void packed_ptr_check_x(Checker *c, TypeId to, uint32_t e, int mode,
                               SrcLoc castloc, bool arg);

/* gcc's build_c_cast does not look for packed members in a cast that leaves
 * the pointer type as it is; the conversion it feeds checks them instead. */
static bool noop_ptr_cast(Checker *c, TypeId to, uint32_t operand)
{
    TypeId o = rvt(c, operand);
    return is_ptr(c, o) &&
           type_canon(TT, unqual(c, o)) == type_canon(TT, unqual(c, to));
}

static void packed_ptr_check(Checker *c, TypeId to, uint32_t e)
{
    packed_ptr_check_x(c, to, e, 0, 0, false);
}

static void packed_ptr_check_x(Checker *c, TypeId to, uint32_t e, int mode,
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
                        "address-of-packed-member", "converting a packed %s "
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
                cwarn(c, wl, "address-of-packed-member",
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
static void sso_decay(Checker *c, uint32_t i)
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
    cwarn(c, first_loc(c, i), "scalar-storage-order", "address of array with "
          "reverse scalar storage order requested");
}

static const char *cmp_tstr(Checker *c, uint32_t n);
static const char *bf_tstr(Checker *c, TypeId t, unsigned w);

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
            diag_enabled(c->diag, "traditional-conversion"))
            cwarn(c, l, "", "passing argument %d of '%s' as 'float' rather "
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
        if (diag_enabled(c->diag, "traditional-conversion"))
            cwarn(c, l, "traditional-conversion", "passing argument %d of "
                  "'%s' %s due to prototype", pn, fn, w);
        return;
    }
    if (w && diag_enabled(c->diag, "traditional-conversion"))
        cwarn(c, l, "traditional-conversion", "passing argument %d of '%s' as "
              "%s due to prototype", pn, fn, w);
}

/* The built-in function expression e names (see ccall_builtin_ref); a
 * replaced function type turns *pt (the decayed pointer type) into a pointer
 * to it. */
static const char *fnref_builtin(Checker *c, uint32_t e, TypeId *pt, char *buf,
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
    if (ctx == CONV_ARG && diag_enabled(c->diag, "traditional-conversion") &&
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
        conv_diag(&x, RK_PED, "c++-compat", m, true);
    }
    if (diag_enabled(c->diag, "enum-conversion")) {
        TypeId ot = orig_type(c, expr);
        if (tkind(c, ot) == TY_ENUM && kl == TY_ENUM &&
            mainv(c, ot) != mainv(c, lt))
            cwarn(c, x.loc, "enum-conversion", "implicit conversion from %s "
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
            diag_report(c->diag, DL_NOTE, "", cinput_loc(c, c->nodes[expr].tok),
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
            conv_diag(&x, RK_WARN, "scalar-storage-order", m, true);
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
                    conv_diag(&x, RK_WARN, "discarded-array-qualifiers", m,
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
                conv_diag(&x, RK_PED, "pedantic", m, true);
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
                    conv_diag(&x, RK_PED, "discarded-qualifiers", m, true);
                } else if (warn_ped) {
                    /* pedwarn_c11 before C2X: only when pedantic (above) */
                } else if (is_void(c, ttl) || is_void(c, ttr) || target_cmp) {
                    /* a mismatch in signedness is ignored */
                } else if (diag_enabled(c->diag, "pointer-sign")) {
                    sp(m[CONV_ARG], "pointer targets in passing argument %d of "
                       "'%s' differ in signedness", pn, fn);
                    sp(m[CONV_ASSIGN], "pointer targets in assignment from %s to "
                       "%s differ in signedness", R, T);
                    sp(m[CONV_INIT], "pointer targets in initialization of %s "
                       "from %s differ in signedness", T, R);
                    sp(m[CONV_RETURN], "pointer targets in returning %s from a "
                       "function with return type %s differ in signedness", R, T);
                    conv_diag(&x, RK_PED, "pointer-sign", m, true);
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
                    conv_diag(&x, RK_PED, "discarded-qualifiers", m, true);
                }
            }
            /* C++ has no implicit void * -> T *; a null pointer constant
             * (NULL is usually (void *) 0) is tolerated */
            if (rvoid && !x.npc && !lvoid && cexpr_cxx_compat(c, expr))
                cwarn(c, ctx == CONV_ASSIGN ? x.loc
                      : c->u->toks[first_tok(c, expr)].exp
                      ? c->u->toks[first_tok(c, expr)].exp : x.loc,
                      "c++-compat",
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
            conv_diag(&x, RK_PED, "incompatible-pointer-types", m, true);
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
            conv_diag(&x, RK_PED, "int-conversion", m, true);
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
        conv_diag(&x, RK_PED, "int-conversion", m, true);
        return true;
    }
    if (kl == TY_BOOL && kr == TY_PTR) {
        /* convert_for_assignment: c_objc_common_truthvalue_conversion */
        cexpr_truth_warn(c, expr, cinput_loc(c, c->nodes[expr].tok));
        return true;
    }

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
            cwarn(c, x.eloc, "", "incompatible types when assigning to type %s "
                  "from type %s", T, R);
        else
            cerror(c, x.eloc, "incompatible types when assigning to type %s "
                   "from type %s", T, R);
        break;
    case CONV_INIT:
        if (ci->warnopt)
            cwarn(c, x.loc, "", "incompatible types when initializing type %s "
                  "using type %s", T, R);
        else
            cerror(c, x.loc, "incompatible types when initializing type %s "
                   "using type %s", T, R);
        break;
    default:
        if (ci->warnopt)
            cwarn(c, x.loc, "", "incompatible types when returning type %s but "
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

static uint32_t stmt_expr_value(Checker *c, uint32_t i);

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
            cwarn(c, cnode_loc(c, i), "overflow", "integer overflow in "
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
 * increment, 2: decrement) when a is read-only; true if one was given. */
static bool readonly_check(Checker *c, uint32_t a, SrcLoc loc, int use)
{
    static const char *const verb[] = { "assignment", "increment", "decrement" };
    TypeId t = c->ty[a];
    uint32_t s = strip_paren(c, a);
    bool ro = (tquals(c, t) & TQ_CONST) != 0;
    if (!ro && is_record(c, t)) {
        const Record *r = type_record(TT, t);
        ro = r && (r->flags & RF_CONST_MEMBER);
    }
    if (!ro && s != NO_NODE && ntag(c, s) == N_INDEX) {
        /* an element of a string literal: a warning, not an error (PR 27676) */
        uint32_t k[3], b;
        if (nkids(c, s, k, 3) >= 2 && (b = strip_paren(c, k[0])) != NO_NODE &&
            ntag(c, b) == N_STRING)
        {
            /* at the literal, or at a prefix operator before it */
            SrcLoc fl = first_loc(c, a);
            cwarn(c, loc < fl ? loc : fl, "", "%s of read-only location '%s'",
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
            const char *nm = cident(c, mid);
            if (tquals(c, bt) & TQ_CONST)
                cerror(c, loc, "%s of member '%s' in read-only object",
                       verb[use], nm);
            else
                cerror(c, loc, "%s of read-only member '%s'", verb[use], nm);
            return true;
        }
    }
    if (s != NO_NODE && ntag(c, s) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        if (ref != SYM_NONE && csym(c, ref)->kind != CS_FUNC) {
            cerror(c, loc, "%s of read-only %s '%s'", verb[use],
                   csym(c, ref)->flags & CSF_PARAM ? "parameter" : "variable",
                   cident(c, cnode_ident(c, s)));
            return true;
        }
    }
    cerror(c, loc, "%s of read-only location '%s'", verb[use], estr(c, a));
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
        cwarn(c, loc, "c++-compat", inc ? "increment of enumeration value is "
              "invalid in C++" : "decrement of enumeration value is invalid in "
              "C++");
    if (tkind(c, t) == TY_BOOL)
        cwarn(c, loc, "bool-operation", inc ? "increment of a boolean "
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
            cpedwarn(c, loc, "pointer-arith", inc ? "wrong type argument to "
                     "increment" : "wrong type argument to decrement");
    }
    if (readonly_check(c, a, loc, inc ? 1 : 2)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = EF_SIDE;
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
        cpedwarn(c, loc, "", "taking address of expression of type 'void'");
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
        cwarn(c, loc, "scalar-storage-order", "address of array with reverse "
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
            cwarn(c, loc, "strict-aliasing=", "type-punning to incomplete "
                  "type might break strict-aliasing rules");
        return;
    }
    r = type_alias_rel(TT, obj, tg);
    if (r == AL_DISJOINT)
        cwarn(c, loc, "strict-aliasing=", "dereferencing type-punned "
              "pointer will break strict-aliasing rules");
    else if (r == AL_MAY && at_cast)
        cwarn(c, loc, "strict-aliasing=", "dereferencing type-punned "
              "pointer might break strict-aliasing rules");
}

static bool alias_wanted(Checker *c, int lvl)
{
    return strict_alias_on(c) && diag_enabled(c->diag, "strict-aliasing=") &&
           diag_option_level(c->diag, "strict-aliasing=", 3) == lvl;
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
static void alias_cast(Checker *c, uint32_t a, TypeId pt)
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
        cwarn(c, loc, "", "dereferencing 'void *' pointer");
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

static bool is_boolish(Checker *c, uint32_t n);

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
        cwarn(c, loc, "traditional", "traditional C rejects the unary plus "
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
            Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "bool-operation",
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
            if (c->ck[a] == K_ICE) {
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
        if (op == P_MINUS) {
            unsigned bits = int_bits(c, pt);
            if (is_signed(c, pt) && bits <= 64 && bits > 0 &&
                v == cexpr_trunc(c, pt, UINT64_C(1) << (bits - 1)))
                ovf = true;
            v = 0 - v;
        } else if (op == P_TILDE) {
            v = ~v;
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
        c->ck[i] = c->ck[a] == K_ICE ? K_ICE : K_FOLD;
        c->cv[i] = 0;
        c->ef[i] |= c->ck[a] == K_ICE ? EF_INTOPS : EF_NOPCST;
    }
}

static void e_unary(Checker *c, uint32_t i)
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

static void e_postfix(Checker *c, uint32_t i)
{
    uint32_t a = first_child(c, i);
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    incdec(c, i, a, npunct(c, i) == P_INC);
}

/* ---- constants through conversions ---------------------------------------------- */

bool is_decimal_flt(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_DEC32 && k <= TY_DEC128;
}

/* A floating value converted to integer type t (saturating). */
bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out)
{
    return float_to_int_bits(c, f, t, int_bits(c, t), out);
}

/* bits: the width to saturate to (a bit-field's, or t's own) */
bool float_to_int_bits(Checker *c, long double f, TypeId t, unsigned bits,
                       uint64_t *out)
{
    if (bits == 0 || bits > 64)
        return false;
    if (f != f) {
        *out = 0;
        return true;
    }
    if (tkind(c, t) == TY_BOOL) {
        *out = f != 0;
        return true;
    }
    if (is_signed(c, t)) {
        int64_t max = bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1;
        int64_t min = -max - 1;
        if (f >= (long double)max)
            *out = (uint64_t)max;
        else if (f <= (long double)min)
            *out = (uint64_t)min;
        else
            *out = (uint64_t)(int64_t)f;
    } else {
        uint64_t max = bits == 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
        if (f <= 0)
            *out = 0;
        else if (f >= (long double)max)
            *out = max;
        else
            *out = (uint64_t)f;
    }
    return true;
}

/* Node i (whose type is already set to `to`) gets the value of node a
 * converted to `to`, as a cast or the usual conversions would. */
static void conv_const(Checker *c, uint32_t i, uint32_t a, TypeId to)
{
    TypeId from = rvt(c, a);
    long double f;
    c->ck[i] = K_NONE;
    c->cv[i] = 0;
    c->cb[i] = 0;
    if (is_complex(c, to)) {
        long double re, im;
        if (cplx_conv(c, a, to, &re, &im))
            cplx_set(c, i, re, im);
        return;
    }
    if (is_complex(c, from) && (is_int(c, to) || is_flt(c, to))) {
        long double re, im, o;
        TypeId ct;
        if (!cplx_get(c, a, &re, &im, &ct) || !part_conv(c, ct, to, re, &o))
            return;
        if (is_flt(c, to)) {
            c->ck[i] = K_FLOAT;
            c->cv[i] = fpush(c, o);
        } else {
            c->ck[i] = K_FOLD;
            c->cv[i] = cexpr_trunc(c, to, cplx_u(o));
            c->ef[i] |= EF_NOPCST;
        }
        return;
    }
    if (is_int(c, to)) {
        if (has_ival(c, a) && int_bits(c, to) <= 64 && int_bits(c, from) <= 64) {
            uint64_t v = tkind(c, to) == TY_BOOL ? c->cv[a] != 0
                                                 : cexpr_trunc(c, to, c->cv[a]);
            c->cv[i] = v;
            if (c->ck[a] == K_ICE) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS | (c->ef[a] & EF_OVERFLOW);
            } else {
                c->ck[i] = K_FOLD;
                if (c->ef[a] & (EF_CST | EF_NOPCST | EF_REALCST))
                    c->ef[i] |= EF_NOPCST;
                else if (c->ef[a] & EF_INTOPS)
                    c->ef[i] |= EF_INTOPS;
                c->ef[i] |= c->ef[a] & EF_OVERFLOW;
            }
        } else if (c->ck[a] == K_FLOAT &&
                   float_to_int(c, c->fv.data[c->cv[a]], to, &c->cv[i])) {
            uint64_t ov;
            if (c->ef[a] & EF_REALCST) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS;
            } else {
                c->ck[i] = K_FOLD;
                c->ef[i] |= EF_NOPCST;
            }
            /* fold_convert_const_int_from_real: TREE_OVERFLOW */
            if (tkind(c, to) != TY_BOOL &&
                (float_ovf(c, c->fv.data[c->cv[a]], to, &ov) ||
                 c->fv.data[c->cv[a]] != c->fv.data[c->cv[a]]))
                c->ef[i] |= EF_OVERFLOW;
        } else if (c->ck[a] == K_ADDR && is_ptr(c, from)) {
            if (c->cb[a] == 0) {
                c->ck[i] = K_FOLD;
                c->cv[i] = tkind(c, to) == TY_BOOL ? c->cv[a] != 0
                                                   : cexpr_trunc(c, to, c->cv[a]);
                c->ef[i] |= EF_NOPCST;
            } else if (int_bits(c, from) == int_bits(c, to) &&
                       tkind(c, to) != TY_BOOL) {
                c->ck[i] = K_ADDR;
                c->cb[i] = c->cb[a];
                c->cv[i] = c->cv[a];
            }
        }
        return;
    }
    if (is_flt(c, to)) {
        if (!is_decimal_flt(c, to) && !is_decimal_flt(c, from) &&
            fval(c, a, &f)) {
            c->ck[i] = K_FLOAT;
            c->cv[i] = fpush(c, fround(c, to, f));
        }
        return;
    }
    if (is_ptr(c, to)) {
        if (has_ival(c, a) && int_bits(c, from) <= 64) {
            c->ck[i] = K_ADDR;
            c->cv[i] = cexpr_trunc(c, to, c->cv[a]);
        } else if (c->ck[a] == K_ADDR &&
                   (is_ptr(c, from) ||
                    (is_int(c, from) && int_bits(c, from) == int_bits(c, to)))) {
            c->ck[i] = K_ADDR;
            c->cb[i] = c->cb[a];
            c->cv[i] = c->cv[a];
        }
    }
}

/* ---- sizeof, _Alignof ------------------------------------------------------------ */

/* Is t variably modified in a way that makes its size a run-time value? */
static bool var_size(Checker *c, TypeId t)
{
    for (;;) {
        TypeId ct = type_canon(TT, t);
        TypeKind k = tkind(c, ct);
        if (k == TY_VLA)
            return true;
        if (k == TY_ARRAY) {
            t = type_base(TT, ct);
            continue;
        }
        if (k == TY_STRUCT || k == TY_UNION)
            return (type_record(TT, ct)->flags & RF_VLA) != 0;
        return false;
    }
}

static void set_ice(Checker *c, uint32_t i, TypeId t, uint64_t v)
{
    c->ty[i] = t;
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, t, v);
    c->ef[i] = EF_INTOPS;
}

static const Field *member_field_of(Checker *c, uint32_t x, TypeId *recp);

/* Whether x (parentheses ignored) names an object. */
static bool obj_ident(Checker *c, uint32_t x)
{
    uint32_t s = strip_paren(c, x), ref;
    if (s == NO_NODE || ntag(c, s) != N_IDENT)
        return false;
    ref = lookup_ord(c, cnode_ident(c, s));
    return ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ;
}

/* The outermost sizeof around node i, or NO_NODE. */
static uint32_t sizeof_outer(const Checker *c, uint32_t i)
{
    uint32_t r = NO_NODE, p;
    unsigned depth;
    for (depth = 0; depth < 256 && (p = c->par[i]) != NO_NODE; depth++) {
        if (ntag(c, p) == N_SIZEOF_EXPR || ntag(c, p) == N_SIZEOF_TYPE ||
            ntag(c, p) == N_ALIGNOF_EXPR || ntag(c, p) == N_ALIGNOF_TYPE ||
            ntag(c, p) == N_TYPEOF)
            r = p;
        i = p;
    }
    return r;
}

/* A sizeof of a variable length type is evaluated: the functions named in it
 * are used (pop_maybe_used). */
static void sizeof_marks_used(Checker *c, uint32_t sz)
{
    uint32_t k;
    for (k = cfirst(c, sz); k < sz; k++)
        if (ntag(c, k) == N_IDENT && c->cb[k]) {
            CSym *s = csym(c, c->cb[k] - 1);
            if (s->kind == CS_FUNC)
                s->flags |= CSF_CUSED;
        }
}

/* typeof of a variably modified type is evaluated like sizeof of a VLA. */
void cexpr_typeof_used(Checker *c, uint32_t n, TypeId t)
{
    if (type_is_vm(TT, t) && sizeof_outer(c, n) == NO_NODE)
        sizeof_marks_used(c, n);
}

static void e_sizeof(Checker *c, uint32_t i, bool align)
{
    uint32_t a = first_child(c, i);
    unsigned tag = ntag(c, i);
    bool is_type = tag == N_SIZEOF_TYPE || tag == N_ALIGNOF_TYPE;
    const char *op = align ? "__alignof__" : "sizeof";
    SrcLoc loc;
    TypeId t;
    uint64_t v = 1;
    bool ok;
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    if (align && !cexpr_in_extension(c, i)) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok, &len);
        if (len == 8 && !memcmp(s, "_Alignof", 8)) {
            size_t n0 = c->diag->all.len;
            cped11(c, cnode_loc(c, i), "ISO C99 does not support '_Alignof'");
            choist(c, i, n0);
            if (!is_type)
                cpedantic(c, cnode_loc(c, i),
                          "ISO C does not allow '_Alignof (expression)'");
        }
    }
    if (is_type) {
        t = type_of_typename(c, a);
        loc = first_loc(c, a);
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
    } else {
        if (node_err(c, a)) {
            /* c_alignof_expr of an erroneous operand is a constant 1 */
            if (align)
                set_ice(c, i, size_type(c), 1);
            else
                set_err(c, i);
            return;
        }
        t = c->ty[a];
        loc = align ? cnode_loc(c, i) : first_loc(c, a);
        if (c->ef[a] & EF_BITFIELD) {
            if (align) {
                cerror(c, loc, "'__alignof' applied to a bit-field");
                set_ice(c, i, size_type(c), 1);
                return;
            }
            cerror(c, loc, "'sizeof' applied to a bit-field");
        }
    }
    if (!is_type && !align) {
        uint32_t s = strip_paren(c, a);
        /* *&x folds to x (PR c/82167) */
        for (;;) {
            uint32_t k1[3], k2[3];
            uint32_t in;
            if (s == NO_NODE || ntag(c, s) != N_UNARY ||
                npunct(c, s) != P_STAR || nkids(c, s, k1, 3) != 1)
                break;
            in = strip_paren(c, k1[0]);
            if (in == NO_NODE || ntag(c, in) != N_UNARY ||
                npunct(c, in) != P_AMP || nkids(c, in, k2, 3) != 1)
                break;
            s = strip_paren(c, k2[0]);
        }
        if (s != NO_NODE && ntag(c, s) == N_IDENT) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, s));
            if (ref != SYM_NONE && (csym(c, ref)->flags & CSF_ARRAY_PARM)) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc,
                                        "sizeof-array-argument", "'sizeof' on "
                                        "array function parameter '%s' will "
                                        "return size of %s",
                                        cident(c, cnode_ident(c, s)),
                                        type_q(TT, t));
                cnote(c, d, csym(c, ref)->loc, "declared here");
            }
        }
    }
    uint32_t fref = SYM_NONE;
    if (is_func(c, t) && align && !is_type) {
        uint32_t fs = strip_paren(c, a);
        if (fs != NO_NODE && ntag(c, fs) == N_IDENT)
            fref = lookup_ord(c, cnode_ident(c, fs));
        if (fref != SYM_NONE && csym(c, fref)->kind != CS_FUNC)
            fref = SYM_NONE;
    }
    if (fref != SYM_NONE) {
        /* c_alignof_expr: a function's DECL_ALIGN_UNIT (1 unless aligned) */
        v = csym(c, fref)->ualign ? csym(c, fref)->ualign : 1;
    } else if (is_func(c, t)) {
        if (!align)
            ped_arith(c, i, loc, "invalid application of 'sizeof' to a function "
                                 "type");
        else
            ped(c, i, loc, "ISO C does not permit '_Alignof' applied to a "
                           "function type");
        v = align ? 8 : 1;
    } else if (is_void(c, t)) {
        char buf[96];
        snprintf(buf, sizeof buf, "invalid application of '%s' to a void type",
                 op);
        ped_arith(c, i, loc, buf);
    } else if (align && !is_type && !complete(c, t) && obj_ident(c, a)) {
        /* c_alignof_expr takes a variable's DECL_ALIGN: an incomplete object
         * is accepted */
        v = tkind(c, type_canon(TT, t)) == TY_ARRAY ? type_align(TT, t) : 1;
    } else if (!complete(c, t)) {
        cerror(c, loc, "invalid application of '%s' to incomplete type %s", op,
               type_q(TT, t));
        set_err(c, i);
        return;
    } else if (!align) {
        if (var_size(c, t)) {
            if (sizeof_outer(c, i) == NO_NODE)
                sizeof_marks_used(c, i);
            c->ty[i] = size_type(c);
            return;
        }
        v = type_size(TT, t, &ok);
        if (!ok)
            v = 0;
    } else {
        v = type_align(TT, t);
        if (!is_type) {
            uint32_t s = strip_paren(c, a);
            if (s != NO_NODE && ntag(c, s) == N_IDENT) {
                uint32_t ref = lookup_ord(c, cnode_ident(c, s));
                /* aligned (n) sets a variable's alignment, even below the type's */
                if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ &&
                    csym(c, ref)->align)
                    v = csym(c, ref)->align;
            } else if (s != NO_NODE && ntag(c, s) == N_MEMBER_EXPR) {
                TypeId rec = 0;
                const Field *f = member_field_of(c, s, &rec);
                if (f) {
                    bool pk = (f->flags & FF_PACKED) ||
                              (type_record(TT, rec)->flags & RF_PACKED);
                    if (pk)
                        v = f->align ? f->align : 1;
                    else if (f->align > v)
                        v = f->align;
                }
            }
        }
    }
    set_ice(c, i, size_type(c), v);
}

/* ---- casts ----------------------------------------------------------------------- */


/* gcc's TREE_CODE class of a type for -Wbad-function-cast: all integer
 * types but _Bool and enums share INTEGER_TYPE. */
static int cast_class(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    if (k == TY_BOOL || k == TY_ENUM)
        return (int)k;
    if (is_int(c, t))
        return -1;
    if (is_flt(c, t))
        return -2;
    if (is_complex(c, t))
        return -3;
    return (int)k;
}

/* gcc's handle_warn_cast_qual: a pointer cast that drops a qualifier of a
 * target type, or that is unsafe through an unqualified intermediate level. */
/* gcc's build_c_cast -Wcast-align=strict: the target is more aligned than
 * the source (-Wcast-align alone is a no-op on targets that allow unaligned
 * access). */
static void cast_align(Checker *c, SrcLoc loc, TypeId pt, TypeId po)
{
    bool ok;
    if (is_void(c, po) || is_func(c, po) || is_void(c, pt) || is_func(c, pt) ||
        !diag_enabled(c->diag, "cast-align=") ||
        diag_option_level(c->diag, "cast-align=", 1) < 2)
        return;
    type_size(TT, po, &ok);
    if (!ok)
        return;                 /* an opaque type's alignment is unknown */
    type_size(TT, pt, &ok);
    if (ok && type_align(TT, pt) > type_align(TT, po))
        cwarn(c, loc, "cast-align=", "cast increases required alignment of "
              "target type");
}

/* gcc's c_safe_arg_type_equiv_p: two parameter or return types a call
 * cannot tell apart (a void stands for the end of a prototype). */
static bool safe_arg_equiv(Checker *c, TypeId a, TypeId b)
{
    if (!a || !b)
        return !a && !b;
    a = TYPE_UNQUAL(type_canon(TT, a));
    b = TYPE_UNQUAL(type_canon(TT, b));
    if (is_ptr(c, a) && is_ptr(c, b))
        return true;
    if (is_int(c, a) && is_int(c, b) &&
        (tkind(c, a) == TY_BOOL) == (tkind(c, b) == TY_BOOL) &&
        tkind(c, a) != TY_BOOL && int_bits(c, a) == int_bits(c, b) &&
        (is_signed(c, a) == is_signed(c, b) ||
         int_bits(c, a) >= 32))
        return true;
    return type_compatible(TT, a, b);
}

/* gcc's c_safe_function_type_cast_p; fa, fb: the function types. */
static bool safe_function_cast(Checker *c, TypeId fa, TypeId fb)
{
    const TypeEnt *ea = type_ent(TT, fa), *eb = type_ent(TT, fb);
    bool pa = !(ea->flags & TF_NOPROTO), pb = !(eb->flags & TF_NOPROTO);
    uint32_t na = pa ? ea->n + !(ea->flags & TF_VARIADIC) : 0,
             nb = pb ? eb->n + !(eb->flags & TF_VARIADIC) : 0, j;
    const TypeId *qa = type_params(TT, fa), *qb = type_params(TT, fb);
    TypeId ra = type_base(TT, fa), rb = type_base(TT, fb);
    if ((is_void(c, ra) && pa && !ea->n && !(ea->flags & TF_VARIADIC)) ||
        (is_void(c, rb) && pb && !eb->n && !(eb->flags & TF_VARIADIC)))
        return true;
    if (!safe_arg_equiv(c, ra, rb))
        return false;
    for (j = 0; j < na && j < nb; j++)
        if (!safe_arg_equiv(c, j < ea->n ? qa[j] : 0, j < eb->n ? qb[j] : 0))
            return false;
    return true;
}

static void cast_function_type(Checker *c, SrcLoc loc, TypeId t, TypeId ot)
{
    TypeId pt = type_canon(TT, pointee(c, t)), po = type_canon(TT, pointee(c, ot));
    if (!is_func(c, pt) || !is_func(c, po) ||
        !diag_enabled(c->diag, "cast-function-type") ||
        safe_function_cast(c, pt, po))
        return;
    cwarn(c, loc, "cast-function-type", "cast between incompatible function "
          "types from %s to %s", type_q(TT, ot),
          type_q(TT, type_canon(TT, t)));
}

static void cast_qual(Checker *c, SrcLoc loc, TypeId t, TypeId ot)
{
    TypeId it = t, io = ot;
    unsigned discarded = 0, added = 0, qnoat = TQ_CONST | TQ_VOLATILE | TQ_RESTRICT | TQ_ATOMIC;
    bool is_const;
    char b[48];
    if (!diag_enabled(c->diag, "cast-qual"))
        return;
    do {
        it = type_canon(TT, type_base(TT, type_canon(TT, it)));
        io = type_canon(TT, type_base(TT, type_canon(TT, io)));
        if (is_func(c, it) && is_func(c, io))
            added |= tquals(c, it) & ~tquals(c, io) & qnoat;
        else
            discarded |= tquals(c, strip_arr(c, io)) & ~tquals(c, strip_arr(c, it)) & qnoat;
    } while (tkind(c, it) == TY_PTR && tkind(c, io) == TY_PTR);
    if (added)
        cwarn(c, loc, "cast-qual", "cast adds '%s%s%s' qualifier to function "
              "type", added & TQ_CONST ? "__attribute__((const))" : "",
              (added & TQ_CONST) && (added & TQ_VOLATILE) ? " " : "",
              added & TQ_VOLATILE ? "__attribute__((noreturn))" : "");
    if (discarded)
        cwarn(c, loc, "cast-qual", "cast discards '%s' qualifier from pointer "
              "target type", qual_str(b, discarded));
    if (added || discarded)
        return;
    it = t;
    io = ot;
    /* only when the types are otherwise the same */
    for (;;) {
        it = type_canon(TT, type_base(TT, type_canon(TT, it)));
        io = type_canon(TT, type_base(TT, type_canon(TT, io)));
        if (tkind(c, it) == TY_PTR && tkind(c, io) == TY_PTR)
            continue;
        if (tkind(c, it) == TY_PTR || tkind(c, io) == TY_PTR ||
            !type_compatible(TT, mainv(c, it), mainv(c, io)))
            return;
        break;
    }
    it = t;
    io = ot;
    is_const = (tquals(c, pointee(c, it)) & TQ_CONST) != 0;
    do {
        it = type_canon(TT, type_base(TT, type_canon(TT, it)));
        io = type_canon(TT, type_base(TT, type_canon(TT, io)));
        if (!is_func(c, it) && (tquals(c, it) & ~tquals(c, io) & qnoat) &&
            !is_const) {
            cwarn(c, loc, "cast-qual", "to be safe all intermediate pointers "
                  "in cast from %s to %s must be 'const' qualified",
                  type_q(TT, ot), type_q(TT, t));
            break;
        }
        if (is_const)
            is_const = (tquals(c, it) & TQ_CONST) != 0;
    } while (tkind(c, it) == TY_PTR && tkind(c, io) == TY_PTR);
}

/* gcc points at the tag of a struct/union/enum type name */
static SrcLoc cast_tag_loc(Checker *c, uint32_t i, uint32_t tn)
{
    SrcLoc el = after_loc(c, i);
    uint32_t m;
    for (m = cfirst(c, tn); m < tn; m++)
        if (ntag(c, m) == N_TAG && c->par[m] != NO_NODE &&
            (ntag(c, c->par[m]) == N_STRUCT || ntag(c, c->par[m]) == N_ENUM) &&
            c->par[c->par[m]] != NO_NODE &&
            ntag(c, c->par[c->par[m]]) == N_SPECS &&
            c->par[c->par[c->par[m]]] == tn)
            el = cnode_loc(c, m);
    return el;
}

static void e_cast(Checker *c, uint32_t i)
{
    uint32_t k[2], a;
    SrcLoc loc = cnode_loc(c, i);
    TypeId t, ot;
    TypeKind tk;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    a = k[1];
    t = type_of_typename(c, k[0]);
    if (is_err(c, t) || node_err(c, a)) {
        set_err(c, i);
        return;
    }
    tk = tkind(c, t);
    if (tk == TY_ARRAY || tk == TY_VLA) {
        cerror(c, loc, "cast specifies array type");
        set_err(c, i);
        return;
    }
    if (tk == TY_FUNC) {
        cerror(c, loc, "cast specifies function type");
        set_err(c, i);
        return;
    }
    if (tk == TY_ENUM && !complete(c, t)) {
        cerror(c, cast_tag_loc(c, i, k[0]), "conversion to incomplete type");
        set_err(c, i);
        return;
    }
    if (tk == TY_VOID) {
        if (!rvalue_ok(c, a)) {
            set_err(c, i);
            return;
        }
        c->ty[i] = unqual(c, t);
        c->ef[i] = c->ef[a] & EF_PROP;
        return;
    }
    ot = rvt(c, a);
    if (is_void(c, ot)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (!rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = unqual(c, t);
    c->ef[i] = c->ef[a] & EF_PROP;
    if (tk == TY_VECTOR) {
        /* an integer or a vector of the vector's size converts */
        bool v = tkind(c, ot) == TY_VECTOR, ok = true;
        if (!v && !(is_int(c, ot) && tkind(c, ot) != TY_BOOL))
            cerror(c, after_loc(c, i), "cannot convert value to a vector");
        else if (type_size(TT, type_canon(TT, t), &ok) !=
                 type_size(TT, type_canon(TT, ot), &ok))
            cerror(c, after_loc(c, i), "cannot convert a value of type %s to "
                   "vector type %s which has different size",
                   type_q(TT, ot), type_q(TT, type_canon(TT, t)));
        else
            return;
        set_err(c, i);
        return;
    }
    if (tkind(c, ot) == TY_VECTOR && tk != TY_UNION && !is_record(c, t)) {
        bool ok = true;
        if (is_int(c, t) && tk != TY_BOOL) {
            if (type_size(TT, type_canon(TT, t), &ok) ==
                type_size(TT, type_canon(TT, ot), &ok))
                return;
            cerror(c, after_loc(c, i), "cannot convert a vector of type %s to "
                   "type %s which has different size", type_q(TT, ot),
                   type_q(TT, t));
        } else if (tk == TY_BOOL)
            cerror(c, after_loc(c, i), "used vector type where scalar is "
                   "required");
        else if (is_flt(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where a "
                   "floating-point was expected");
        else if (is_ptr(c, t))
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        else
            cerror(c, after_loc(c, i), "aggregate value used where a complex "
                   "was expected");
        set_err(c, i);
        return;
    }
    if (is_record(c, t) || tk == TY_UNION) {
        if (mainv(c, t) == mainv(c, ot)) {
            ped(c, i, loc, "ISO C forbids casting nonscalar to the same type");
            return;
        }
        if (tk == TY_UNION) {
            const Record *r = type_record(TT, type_canon(TT, t));
            uint32_t f;
            for (f = 0; f < r->nfields; f++) {
                const Field *fl = &c->tt.fields.data[r->fields + f];
                if (type_compatible(TT, mainv(c, fl->ty), mainv(c, ot)))
                    break;
            }
            if (f == r->nfields) {
                cerror(c, loc, "cast to union type from type not present in "
                               "union");
                set_err(c, i);
                return;
            }
            ped(c, i, loc, "ISO C forbids casts to union type");
            return;
        }
        {
            cerror(c, cast_tag_loc(c, i, k[0]),
                   "conversion to non-scalar type requested");
        }
        set_err(c, i);
        return;
    }
    if (is_record(c, ot)) {
        if (tk == TY_BOOL) {
            cerror(c, after_loc(c, i), "used %s type value where scalar is "
                   "required", tkind(c, ot) == TY_UNION ? "union" : "struct");
            set_err(c, i);
            return;
        }
        if (is_ptr(c, t)) {
            /* the cast keeps its type after the error */
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
            c->ty[i] = type_canon(TT, c->ty[i]);
            return;
        }
        if (is_int(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where an integer was "
                           "expected");
        else if (is_flt(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where a floating-point was "
                           "expected");
        else if (is_ptr(c, t))
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        else
            cerror(c, after_loc(c, i), "aggregate value used where a complex was "
                           "expected");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, ot) && (is_flt(c, t) || is_complex(c, t))) {
        cerror(c, after_loc(c, i), is_flt(c, t) ? "pointer value used where a "
               "floating-point was expected" : "pointer value used where a "
               "complex was expected");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, t) && !is_ptr(c, ot) && !is_int(c, ot)) {
        cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        c->ty[i] = type_canon(TT, c->ty[i]);
        return;
    }
    if (is_ptr(c, t) && is_ptr(c, ot)) {
        TypeId pt = pointee(c, t), po = pointee(c, ot);
        cast_qual(c, loc, t, ot);
        cast_align(c, loc, pt, po);
        cast_function_type(c, loc, t, ot);
        if (is_func(c, pt) && !is_func(c, po) && !(c->ef[a] & EF_NPC))
            ped(c, i, loc, "ISO C forbids conversion of object pointer to "
                           "function pointer type");
        else if (!is_func(c, pt) && is_func(c, po))
            ped(c, i, loc, "ISO C forbids conversion of function pointer to "
                           "object pointer type");
    } else if (is_ptr(c, ot) && is_int(c, t) &&
               tk != TY_BOOL && tk != TY_ENUM &&
               int_bits(c, t) != int_bits(c, ot)) {
        cwarn(c, loc, "pointer-to-int-cast",
              "cast from pointer to integer of different size");
    }
    packed_ptr_check_x(c, t, a, noop_ptr_cast(c, t, a) ? 1 : 3, 0, false);
    if (is_ptr(c, t))
        alias_cast(c, a, c->ty[i]);
    if (tk == TY_BOOL && (is_ptr(c, ot) || is_int(c, ot) || is_flt(c, ot) || is_complex(c, ot)))
        cexpr_truth_warn(c, a, cinput_loc(c, c->nodes[i].tok));
    /* -Wbad-function-cast: a call cast to a type of another tree code */
    if (ntag(c, strip_paren(c, a)) == N_CALL && diag_enabled(c->diag, "bad-function-cast") &&
        cast_class(c, t) != cast_class(c, ot))
        cwarn(c, loc, "bad-function-cast", "cast from function call of type "
              "%s to non-matching type %s", type_q(TT, ot), type_q(TT, t));
    if (!(is_ptr(c, t) && is_ptr(c, ot)) &&
        is_ptr(c, t) && is_int(c, ot) && tkind(c, ot) != TY_BOOL &&
        tkind(c, ot) != TY_ENUM && int_bits(c, t) != int_bits(c, ot) &&
        c->ck[a] != K_ICE && c->ck[a] != K_FOLD) {
        cwarn(c, loc, "int-to-pointer-cast",
              "cast to pointer from integer of different size");
    }
    conv_const(c, i, a, c->ty[i]);
    if (is_ptr(c, t) && c->ck[i] == K_ADDR && c->cb[i] == 0 &&
        c->cv[i] == 0 && is_intcst(c, a) && !constvar_in(c, a) &&
        !(c->ef[a] & EF_OVERFLOW) &&
        !(ntag(c, strip_paren(c, a)) == N_BINARY &&
          npunct(c, strip_paren(c, a)) == P_COMMA) &&
        is_void(c, pointee(c, t)) && tquals(c, pointee(c, t)) == 0)
        c->ef[i] |= EF_NPC;
}

/* ---- compound literals, statement expressions --------------------------------------- */

static void e_complit(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[0]);
    if (is_err(c, t)) {
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ARRAY &&
        (type_ent(TT, type_canon(TT, t))->flags & TF_INCOMPLETE)) {
        /* cinit.c left the element count in the list's cv */
        uint64_t n = c->ck[k[1]] == K_ICE ? c->cv[k[1]] : 1;
        t = type_array(TT, elem_of(c, t), n);
    }
    cc90(c, ctok_loc(c, c->nodes[k[1]].tok), NULL, "ISO C90 forbids compound "
         "literals");
    if (c->ck[k[1]] == K_ERR) {
        set_err(c, i);
        return;
    }
    if (!type_is_complete(TT, t)) {
        incomplete_error(c, cnode_loc(c, k[1]), NO_NODE, t);
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = EF_LVALUE;
    /* P2c: the initializer's contents */
    if (!in_function(c) && tkind(c, t) != TY_VLA) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = CB_NODE | i;
        addr_rvalue(c, i);
    }
}

/* The node of the value of the statement expression i (NO_NODE: none). */
static uint32_t stmt_expr_value(Checker *c, uint32_t i)
{
    uint32_t k = i - 1, it;
    if (c->nodes[i].size < 4 || k < 3 || ntag(c, k - 1) != N_SCOPE_END)
        return NO_NODE;
    it = k - 2;
    while (it > 0 && (ntag(c, it) == N_LABEL || ntag(c, it) == N_CASE ||
                      ntag(c, it) == N_DEFAULT))
        it--;  /* a labeled last statement yields its own value */
    return ntag(c, it) == N_EXPR_STMT && c->nodes[it].size > 1 ? it - 1
                                                                : NO_NODE;
}

static void e_stmt_expr(Checker *c, uint32_t i)
{
    uint32_t k = i - 1, e;
    SrcLoc loc = cnode_loc(c, i);
    if (cnode(c, i)->flags & NF_ERROR) {    /* the parser reported it */
        set_err(c, i);
        return;
    }
    if (!in_function(c)) {
        cerror(c, loc, "braced-group within expression allowed only inside a "
                       "function");
        set_err(c, i);
        return;
    }
    {
        size_t n0 = c->diag->all.len;
        ped(c, i, loc, "ISO C forbids braced-groups within expressions");
        /* gcc: after the body, before the statement-with-no-effect warnings
         * it defers to the end and before its scope closes */
        if (k >= 1 && ntag(c, k - 1) == N_SCOPE_END) {
            size_t at = c->dm[k - 1], j;
            Diagnostic **dd = c->diag->all.data;
            for (j = c->dm[cfirst(c, i)]; j < at && j < n0; j++)
                if (!strcmp(dd[j]->id, "unused-value")) {
                    at = j;
                    break;
                }
            choist_at(c, n0, at);
        }
    }
    c->ty[i] = TYPE_B(VOID);
    c->ef[i] = EF_SIDE;
    if ((e = stmt_expr_value(c, i)) != NO_NODE) {
        if (node_err(c, e)) {
            set_err(c, i);
            return;
        }
        c->ty[i] = rvt(c, e);
    }
}

/* ---- __builtin_va_arg, __builtin_convertvector, offsetof, types_compatible_p ----------- */

static void e_va_arg(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t, vl = c->tt.va_list, et;
    bool ok = false;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[1]);
    if (node_err(c, k[0]) || is_err(c, t)) {
        set_err(c, i);
        return;
    }
    et = c->ty[k[0]];
    if (vl && type_compatible(TT, mainv(c, et), mainv(c, vl)))
        ok = true;
    else if (vl && is_array(c, vl) && is_ptr(c, rvt(c, k[0])) &&
             type_compatible(TT, mainv(c, rvt(c, k[0])),
                             type_ptr(TT, mainv(c, elem_of(c, vl)))))
        ok = true;
    if (!ok) {
        cerror(c, first_loc(c, k[1]), "first argument to 'va_arg' not of type "
                                     "'va_list'");
        set_err(c, i);
        return;
    }
    if (!complete(c, t)) {
        if (is_func(c, t))
            cerror(c, first_loc(c, k[1]), "second argument to 'va_arg' is a "
                   "function type %s", type_q(TT, t));
        else
            cerror(c, first_loc(c, k[1]), "second argument to 'va_arg' is of "
                   "incomplete type %s", type_q(TT, t));
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ENUM && cexpr_cxx_compat(c, i))
        cwarn(c, first_loc(c, k[1]), "c++-compat", "C++ requires promoted "
              "type, not enum type, in 'va_arg'");
    c->ty[i] = t;
    c->ef[i] = EF_SIDE;
}

static void e_convertvector(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[1]);
    if (node_err(c, k[0]) || is_err(c, t)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = c->ef[k[0]] & EF_PROP;
}

/* gcc's input_location when offsetof's member is a bit-field: a struct,
 * union or enum specifier in the type name leaves it at the tag (or '{');
 * a later token that starts a line, or a plain typedef name, leaves the
 * line change's location.  peek: the ',' after the type name. */
static SrcLoc offsetof_bf_loc(Checker *c, uint32_t ty, uint32_t peek)
{
    uint32_t t, tag = 0;
    for (t = first_tok(c, ty); t < peek; t++) {
        int kw = tckw(c, t);
        if (kw == CK_STRUCT || kw == CK_UNION || kw == CK_ENUM)
            tag = t + 1;
    }
    for (t = tag + 1; tag && t <= peek; t++)
        if (c->u->toks[t].t.flags & TF_BOL)
            tag = 0;
    return tag ? ctok_loc(c, tag) : cinput_loc(c, peek);
}

/* One step of an offsetof designator, for fold_offsetof_1's bounds check. */
typedef struct OffStep {
    bool arr, known, cons, last, ptr;
    TypeId ty;
    uint64_t n, idx;
} OffStep;

/* fold_offsetof_1: an index beyond the array's last element (one past it is
 * fine when nothing is selected from the element) warns, unless the array is
 * a "poor man's flexible array": every component on the way back to the
 * start is the last member of its struct (or in a union). */
static void offsetof_bounds(Checker *c, const OffStep *st, uint32_t ns, SrcLoc loc)
{
    uint32_t j, q;
    if (!diag_enabled(c->diag, "array-bounds="))
        return;
    for (j = 0; j < ns; j++) {
        uint64_t up;
        if (!st[j].arr || !st[j].known || !st[j].cons)
            continue;
        up = st[j].n - 1;
        if (j + 1 == ns)
            up++;                   /* the outermost reference: one past */
        if (up >= st[j].idx)
            continue;
        for (q = j; q > 0 && !st[q - 1].arr && !st[q - 1].ptr && st[q - 1].last;)
            q--;
        if (q == 0)
            continue;               /* reached the base: a trailing array */
        cwarn(c, loc, "array-bounds=", "index %llu denotes an offset greater "
              "than size of %s", (unsigned long long)st[j].idx,
              type_q(TT, st[j].ty));
    }
}

static void e_offsetof(Checker *c, uint32_t i)
{
    uint32_t k[64], n = nkids(c, i, k, 64), j;
    TypeId t, cur;
    uint64_t off = 0;
    bool konst = true, nonconst_addr = false;
    OffStep st[64] = {{0}};
    uint32_t ns = 0;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[0]);
    if (is_err(c, t)) {
        set_err(c, i);
        return;
    }
    cur = t;
    for (j = 1; j < n; j++) {
        unsigned tag = ntag(c, k[j]);
        if (tag == N_NAME || tag == N_DESIG_FIELD) {
            uint32_t name = cnode_ident(c, k[j]);
            SrcLoc loc = cnode_loc(c, k[j]);
            const Field *f;
            uint64_t o = 0;
            unsigned q = 0;
            if (tag == N_DESIG_FIELD && c->nodes[k[j]].flags && is_array(c, cur))
                cur = elem_of(c, cur);
            if (!is_record(c, cur)) {
                if (!is_err(c, cur))
                    cerror(c, loc, "request for member '%s' in something not a "
                           "structure or union", cident(c, name));
                set_err(c, i);
                return;
            }
            if (!complete(c, cur)) {
                incomplete_error(c, loc, NO_NODE, cur);
                set_err(c, i);
                return;
            }
            f = find_field(c, cur, name, &o, &q);
            if (!f) {
                cerror(c, loc, "%s has no member named '%s'", type_q(TT, cur),
                       cident(c, name));
                set_err(c, i);
                return;
            }
            if (f->flags & FF_BITFIELD) {
                cerror(c, offsetof_bf_loc(c, k[0], first_tok(c, k[1]) - 1),
                       "attempt to take address of bit-field structure member "
                       "'%s'", cident(c, name));
                set_err(c, i);
                return;
            }
            off += o / 8;
            if (ns < 64) {
                const Record *rec = type_record(TT, type_canon(TT, cur));
                st[ns].arr = false;
                st[ns].last = (rec->flags & RF_UNION) ||
                              f == &TT->fields.data[rec->fields + rec->nfields - 1];
                ns++;
            }
            cur = f->ty;
        } else if (tag == N_DESIG_INDEX) {
            uint32_t e = first_child(c, k[j]);
            if (e == NO_NODE || node_err(c, e)) {
                set_err(c, i);
                return;
            }
            if (tkind(c, cur) == TY_PTR) {   /* *(p + n): reported once the
                                              * whole designator is known */
                nonconst_addr = true;
                konst = false;
                cur = pointee(c, cur);
                if (ns < 64) {
                    st[ns].arr = false;
                    st[ns].last = false;
                    st[ns++].ptr = true;
                }
                continue;
            }
            if (!is_array(c, cur)) {
                cerror(c, cnode_loc(c, k[j]), "subscripted value is neither "
                       "array nor pointer nor vector");
                set_err(c, i);
                return;
            }
            if (!is_int(c, rvt(c, e))) {
                cerror(c, cnode_loc(c, k[j]), "array subscript is not an "
                       "integer");
                set_err(c, i);
                return;
            }
            if (has_ival(c, e) && !var_size(c, elem_of(c, cur)))
                off += (uint64_t)(int64_t)c->cv[e] * elem_size(c, type_ptr(TT,
                                                                 elem_of(c, cur)));
            else
                konst = false;
            if (ns < 64) {
                const TypeEnt *te = type_ent(TT, type_canon(TT, cur));
                st[ns].arr = true;
                st[ns].ty = cur;
                st[ns].known = tkind(c, cur) == TY_ARRAY && te->n &&
                               !(te->flags & TF_INCOMPLETE);
                st[ns].n = te->n;
                st[ns].cons = has_ival(c, e) && (int64_t)c->cv[e] >= 0;
                st[ns].idx = (uint64_t)c->cv[e];
                ns++;
            }
            cur = elem_of(c, cur);
        }
    }
    if (nonconst_addr) {
        cerror(c, offsetof_bf_loc(c, k[0], first_tok(c, k[1]) - 1),
               "cannot apply 'offsetof' to a non constant address");
        set_err(c, i);
        return;
    }
    offsetof_bounds(c, st, ns, offsetof_bf_loc(c, k[0], first_tok(c, k[1]) - 1));
    c->ty[i] = size_type(c);
    if (konst)
        set_ice(c, i, size_type(c), off);
}

static void e_types_compat(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId a, b;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    a = type_of_typename(c, k[0]);
    b = type_of_typename(c, k[1]);
    if (is_err(c, a) || is_err(c, b)) {
        set_err(c, i);
        return;
    }
    set_ice(c, i, TYPE_B(INT), type_compatible(TT, mainv(c, a), mainv(c, b)));
}

/* ---- __builtin_has_attribute ---------------------------------------------------------- */

/* The field a member-access node names (no diagnostics), and its record. */
static const Field *member_field_of(Checker *c, uint32_t x, TypeId *recp)
{
    uint32_t d = first_child(c, x);
    TypeId rt;
    uint64_t off;
    unsigned q;
    if (d == NO_NODE || node_err(c, d))
        return NULL;
    rt = c->ty[d];
    if (c->nodes[x].flags & NF_ARROW) {
        if (type_ckind(TT, rt) != TY_PTR && type_ckind(TT, rt) != TY_ARRAY)
            return NULL;
        rt = pointee(c, rt);
    }
    rt = type_canon(TT, rt);
    if (!is_record(c, rt))
        return NULL;
    if (recp)
        *recp = rt;
    return find_field(c, rt, cnode_ident(c, x), &off, &q);
}

/* The attribute sets of the typedefs t goes through, appended to out[0..n)
 * (at most max).  An array without an aligned attribute of its own takes
 * its element type's (gcc propagates the user-alignment bit). */
static unsigned typedef_asets(Checker *c, TypeId t, uint32_t *out, unsigned n,
                              unsigned max)
{
    bool al = false;
    for (;;) {
        while (type_kind(TT, t) == TY_TYPEDEF) {
            uint32_t s = cdecl_typedef_aset(c, t);
            if (s && n < max) {
                out[n++] = s;
                al |= cdecl_aset_has(c, s, "aligned", NULL);
            }
            t = type_ent(TT, t)->base;
        }
        if (al || type_kind(TT, t) != TY_ARRAY)
            return n;
        t = type_ent(TT, t)->base;
    }
}

/* The sets of attribute names that belong to the expression e: its declaration
 * (a symbol, a member), and its type's record.  strip: look through the
 * pointers and arrays of the type (what 'copy' does). */
unsigned cexpr_asets(Checker *c, uint32_t e, bool strip, uint32_t out[3])
{
    unsigned n = 0;
    uint32_t x = strip_paren(c, e);
    TypeId t;
    bool comma = false;
    if (x == NO_NODE)
        return 0;
    while (ntag(c, x) == N_BINARY && npunct(c, x) == P_COMMA) {
        uint32_t bk[3];
        if (nkids(c, x, bk, 3) < 2)
            break;
        x = strip_paren(c, bk[1]);     /* a comma expression: its value */
        comma = true;
    }
    if (ntag(c, x) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, x));
        if (ref != SYM_NONE && csym(c, ref)->aset)
            out[n++] = csym(c, ref)->aset;
    } else if (ntag(c, x) == N_MEMBER_EXPR) {
        const Field *f = member_field_of(c, x, NULL);
        if (f && f->aset)
            out[n++] = f->aset;
    }
    if (node_err(c, x))
        return n;
    t = c->ty[x];
    if (comma && type_ckind(TT, t) == TY_ARRAY)    /* C: it decays */
        t = type_ptr(TT, type_base(TT, t));
    while (strip && (type_ckind(TT, t) == TY_PTR || type_ckind(TT, t) == TY_ARRAY))
        t = type_base(TT, t);
    n = typedef_asets(c, t, out, n, 3);
    t = type_canon(TT, t);
    if (is_record(c, t) && type_record(TT, t)->aset && n < 3)
        out[n++] = type_record(TT, t)->aset;
    return n;
}

static void e_has_attr(Checker *c, uint32_t i)
{
    uint32_t k[2], sets[3], n = 0, j;
    char an[32], args[24];
    bool has = false, nonnull = false;
    if (nkids(c, i, k, 2) < 2 || ntag(c, k[1]) != N_ATTR_ITEM) {
        set_err(c, i);
        return;
    }
    {
        size_t len;
        char raw[48];
        const char *tx = ttext(c, c->nodes[k[1]].tok, &len);
        if (len >= sizeof raw)
            len = sizeof raw - 1;
        memcpy(raw, tx, len);
        raw[len] = 0;
        cdecl_attr_name(raw, an, sizeof an);
        cdecl_attr_args(c, k[1], args, sizeof args);
        nonnull = !strcmp(an, "nonnull");
        if (!cdecl_attr_known(an)) {
            cerror(c, ctok_loc(c, c->nodes[k[1]].tok), "unknown attribute "
                   "'%s'", raw);
            set_err(c, i);
            return;
        }
    }
    if (!strcmp(an, "mode")) {
        cwarn(c, ctok_loc(c, c->nodes[k[1]].tok), "attributes", "'mode' attribute "
              "not supported in '__builtin_has_attribute'");
        set_ice(c, i, TYPE_B(INT), 0);
        return;
    }
    if (!strcmp(an, "aligned") && !strcmp(args, "0"))
        cwarn(c, cnode_loc(c, i), "attributes", "requested alignment '0' is "
              "not a positive power of 2");
    if (!strcmp(an, "aligned") && args[0] && !isdigit((unsigned char)args[0])) {
        uint32_t ak[2];
        if (nkids(c, k[1], ak, 2) >= 1 && !node_err(c, ak[0]) &&
            !is_intcst(c, ak[0])) {
            cerror(c, cdecl_line_start_loc(c, c->nodes[i].tok), "requested "
                   "alignment is not an integer constant");
            set_err(c, i);
            return;
        }
    }
    if (!strcmp(an, "aligned") && args[0] >= '1' && args[0] <= '9') {
        unsigned long long av = strtoull(args, NULL, 10);
        if (av & (av - 1))
            cerror(c, cdecl_line_start_loc(c, c->nodes[i].tok), "requested "
                   "alignment '%s' is not a positive power of 2", args);
    }
    if (!strcmp(an, "vector_size")) {
        /* a property of the (vector) type itself, never of a declaration */
        TypeId t;
        if (ntag(c, k[0]) == N_TYPE_NAME)
            t = type_of_typename(c, k[0]);
        else if (node_err(c, k[0])) {
            set_err(c, i);
            return;
        } else
            t = c->ty[k[0]];
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
        t = type_canon(TT, t);
        set_ice(c, i, TYPE_B(INT),
                type_kind(TT, t) == TY_VECTOR &&
                (!args[0] || (uint64_t)atoll(args) == type_ent(TT, t)->n));
        return;
    }
    if (ntag(c, k[0]) == N_TYPE_NAME) {
        TypeId t = type_of_typename(c, k[0]);
        uint32_t q, tmp = 0;
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
        n = typedef_asets(c, t, sets, n, 2);
        t = type_canon(TT, t);
        if (is_record(c, t))
            sets[n++] = type_record(TT, t)->aset;
        /* attributes written in the type name itself */
        for (q = cfirst(c, k[0]); q < k[0]; q++)
            if (ntag(c, q) == N_ATTRIBUTE)
                cdecl_attrs_names(c, q, &tmp);
        if (tmp)
            sets[n++] = tmp;
    } else {
        if (node_err(c, k[0])) {
            set_err(c, i);
            return;
        }
        n = cexpr_asets(c, k[0], false, sets);
    }
    if ((!strcmp(an, "alloc_align") || !strcmp(an, "alloc_size")) && args[0]) {
        /* gcc applies the handler to the operand: a function that does not
         * return a pointer ignores it, one that has the attribute with other
         * arguments keeps its own */
        TypeId ft = ntag(c, k[0]) == N_TYPE_NAME ? type_of_typename(c, k[0])
                                                : c->ty[k[0]];
        if (!is_err(c, ft) && type_ckind(TT, type_canon(TT, ft)) != TY_FUNC)
            cwarn(c, cdecl_line_start_loc(c, c->nodes[i].tok), "attributes",
                  "'%s' attribute only applies to function types", an);
        else if (!is_err(c, ft) && type_ckind(TT, type_canon(TT, ft)) == TY_FUNC) {
            TypeId rt = type_base(TT, type_canon(TT, ft));
            char was[96];
            if (type_ckind(TT, type_canon(TT, rt)) != TY_PTR)
                cwarn(c, cdecl_line_start_loc(c, c->nodes[i].tok), "attributes", "'%s' attribute "
                      "ignored on a function returning %s", an, type_q(TT, rt));
            else
                for (j = 0; j < n; j++)
                    if (cdecl_aset_first_arg(c, sets[j], an, was, sizeof was)) {
                        if (strcmp(was, args))
                            cdecl_alloc_conflict(c, cdecl_line_start_loc(c, c->nodes[i].tok), an, args, was);
                        break;
                    }
        }
    }
    for (j = 0; j < n; j++)
        {
            has |= cdecl_aset_has(c, sets[j], an, args[0] || nonnull ? args : NULL);
        }
    set_ice(c, i, TYPE_B(INT), has);
}

/* ---- _Generic ------------------------------------------------------------------------ */

static void e_generic_assoc(Checker *c, uint32_t i)
{
    copy_node(c, i, i - 1);
}

static void e_generic(Checker *c, uint32_t i)
{
    uint32_t k[64], n = nkids(c, i, k, 64), j, match = NO_NODE, deflt = NO_NODE;
    TypeId sel, ts[64];
    bool bad = false;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    if (!cexpr_in_extension(c, i))
        cped11(c, cnode_loc(c, i), "ISO C99 does not support '_Generic'");
    if (node_err(c, k[0])) {
        set_err(c, i);
        return;
    }
    sel = rvt(c, k[0]);
    sel = mainv(c, sel);
    for (j = 1; j < n; j++) {
        uint32_t tn = first_child(c, k[j]);
        SrcLoc loc;
        uint32_t m;
        ts[j] = 0;
        if (tn == NO_NODE)
            continue;
        if (ntag(c, tn) != N_TYPE_NAME) {
            if (deflt != NO_NODE) {
                Diagnostic *d = cerror_d(c, cnode_loc(c, k[j]), "duplicate "
                                         "'default' case in '_Generic'");
                cnote(c, d, cnode_loc(c, deflt), "original 'default' is here");
                bad = true;
            } else {
                deflt = k[j];
            }
            continue;
        }
        loc = first_loc(c, tn);
        ts[j] = type_of_typename(c, tn);
        if (is_err(c, ts[j])) {
            ts[j] = 0;
            bad = true;
            continue;
        }
        if (is_func(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has function type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        if (!complete(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has incomplete type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        if (var_size(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has variable length type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        for (m = 1; m < j; m++)
            if (ts[m] && type_compatible(TT, ts[m], ts[j])) {
                Diagnostic *d = cerror_d(c, loc, "'_Generic' specifies two "
                                         "compatible types");
                cnote(c, d, first_loc(c, first_child(c, k[m])),
                      "compatible type is here");
                bad = true;
                break;
            }
    }
    for (j = 1; j < n; j++) {
        if (!ts[j] || !type_compatible(TT, sel, ts[j]))
            continue;
        if (match != NO_NODE) {
            Diagnostic *d = cerror_d(c, cnode_loc(c, k[j]), "'_Generic' "
                                     "selector matches multiple associations");
            cnote(c, d, cnode_loc(c, match), "other match is here");
            bad = true;
        } else {
            match = k[j];
        }
    }
    if (match == NO_NODE)
        match = deflt;
    if (match == NO_NODE) {
        cerror(c, first_loc(c, k[0]), "'_Generic' selector of type %s is not "
               "compatible with any association", type_q(TT, sel));
        set_err(c, i);
        return;
    }
    if (bad) {
        set_err(c, i);
        return;
    }
    copy_node(c, i, match);
}

static void e_addr_label(Checker *c, uint32_t i)
{
    uint32_t tok = c->nodes[i].tok;
    ped(c, i, cinput_loc(c, tok + 1), "taking the address of a "
                                                    "label is non-standard");
    if (!in_function(c)) {
        cerror(c, cinput_loc(c, tok + 1), "label '%s' referenced outside of "
               "any function", cident(c, cnode_ident(c, i)));
        c->ty[i] = type_ptr(TT, TYPE_B(VOID));
        c->ck[i] = K_ADDR;
        c->ef[i] = EF_NPC;
        return;
    }
    c->ty[i] = type_ptr(TT, TYPE_B(VOID));
    c->ck[i] = K_ADDR;
    c->cb[i] = CB_NODE | i;
}

/* ---- binary operators ---------------------------------------------------------------- */

/* The value flags of a binary result whose integer value is v (ovf: it
 * overflowed in its type; int_const: gcc's int_const, false when the
 * operation is not a constant expression after all). */
static void bin_value(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                      uint64_t v, bool ovf, bool int_const, SrcLoc loc)
{
    TypeId t = c->ty[i];
    bool prev = ((c->ef[a] | c->ef[b]) & EF_OVERFLOW) != 0;
    c->cv[i] = cexpr_trunc(c, t, v);
    if (is_intcst(c, a) && is_intcst(c, b)) {
        c->ef[i] |= EF_INTOPS;
        if (!int_const) {
            c->ck[i] = K_FOLD;
            return;
        }
        c->ck[i] = K_ICE;
        if (ovf || prev)
            c->ef[i] |= EF_OVERFLOW;
        if (ovf && !prev && !inhibited(c, i, false))
            cwarn(c, loc, "overflow", "integer overflow in expression of type "
                  "%s results in '%s'", type_q(TT, t), vstr(c, t, c->cv[i]));
        return;
    }
    c->ck[i] = K_FOLD;
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
    if (prev)
        c->ef[i] |= EF_OVERFLOW;
    else if (ovf) {
        c->ef[i] |= EF_OVERFLOW | EF_FOLDWARN;
        c->fold_pending++;
    }
}

static void invalid_operands(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                             int op)
{
    const char *sp = punct_spelling[op];
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    /* the left operand of a compound assignment is no rvalue: it keeps its
     * qualifiers unless the promotions change it */
    if (ntag(c, i) == N_ASSIGN && mainv(c, ta) == mainv(c, c->ty[a])) {
        ta = c->ty[a];
        if (tquals(c, ta) & TQ_VOLATILE)
            ta = type_qual(unqual(c, ta), tquals(c, ta) & ~TQ_VOLATILE);
    }
    cerror(c, cnode_loc(c, i), "invalid operands to binary %s (have %s and %s)",
           sp, type_q(TT, ta), type_q(TT, tb));
    set_err(c, i);
}

/* An operand of a binary operator used for its value: false (after an
 * error) for void and incomplete ones. */
static bool binop_operand_at(Checker *c, uint32_t a, SrcLoc loc)
{
    if (is_void(c, rvt(c, a))) {
        cerror(c, expr_loc(c, a), "void value not ignored as it ought to be");
        return false;
    }
    return rvalue_ok_at(c, a, loc);
}

static bool binop_operand(Checker *c, uint32_t a)
{
    return binop_operand_at(c, a, first_loc(c, a));
}

static void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId la, TypeId lb);

static bool truth_ok_at(Checker *c, uint32_t a, SrcLoc loc)
{
    TypeId t = rvt(c, a);
    if (is_record(c, t)) {
        cerror(c, loc, "used %s type value where scalar is "
               "required", tkind(c, t) == TY_UNION ? "union" : "struct");
        return false;
    }
    if (tkind(c, t) == TY_VECTOR) {
        cerror(c, loc, "used vector type where scalar is required");
        return false;
    }
    return true;
}

static bool truth_ok(Checker *c, uint32_t a)
{
    return truth_ok_at(c, a, first_loc(c, a));
}

static void e_comma(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId t;
    if (node_err(c, a) || node_err(c, b)) {
        set_err(c, i);
        return;
    }
    if (!(c->ef[a] & EF_SIDE)) {
        uint32_t s = strip_paren(c, a), ck[3];
        bool voidcast;
        /* (void) a, (void) b, c: a COMPOUND_EXPR ending in a void cast */
        while (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
               nkids(c, s, ck, 3) == 2)
            s = strip_paren(c, ck[1]);
        voidcast = ntag(c, s) == N_CAST && is_void(c, c->ty[s]);
        if (!voidcast && !inhibited(c, i, false))
            cwarn(c, cnode_loc(c, i), "unused-value", "left-hand operand of "
                  "comma expression has no effect");
    }
    else if (!inhibited(c, i, false))
        unused_value(c, a, cnode_loc(c, i));    /* emit_side_effect_warnings */
    t = rvt(c, b);
    c->ty[i] = t;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    if (c->ef[a] & EF_SIDE)
        return;
    /* C99 DR031: a comma of integer constant operands may appear in an
     * unevaluated operand of an integer constant expression */
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
    switch (c->ck[b]) {
    case K_ICE: case K_FOLD:
        c->ck[i] = K_FOLD;
        c->cv[i] = c->cv[b];
        if (is_intcst(c, b))
            c->ef[i] |= EF_CST;
        c->ef[i] |= c->ef[b] & (EF_OVERFLOW | EF_NOPCST);
        break;
    case K_FLOAT: case K_ADDR:
        c->ck[i] = c->ck[b];
        c->cv[i] = c->cv[b];
        c->cb[i] = c->cb[b];
        break;
    default:
        break;
    }
}

static bool from_macro(Checker *c, uint32_t tok);
static bool is_npc(Checker *c, uint32_t n);
static void cst_parts(Checker *c, uint32_t n, bool *neg, uint64_t *mag);

/* ---- -Wlogical-op ----------------------------------------------------------
 * c-family warn_logical_operator: a non-boolean constant operand, operands
 * that are the same test, and range tests on one expression that are always
 * false (&&) or always true (||). */

__extension__ typedef __int128 i128;

typedef struct {
    int op;          /* P_EQEQ, P_NE, P_LT, P_LE, P_GT, P_GE */
    uint32_t l, r;   /* r == NO_NODE: compared with zero */
} LForm;

typedef struct {
    int n;
    i128 lo[4], hi[4];
} IvSet;

static bool lg_cmp(int op)
{
    return op == P_EQEQ || op == P_NE || op == P_LT || op == P_LE ||
           op == P_GT || op == P_GE;
}

static int lg_swap(int op)
{
    switch (op) {
    case P_LT: return P_GT;
    case P_GT: return P_LT;
    case P_LE: return P_GE;
    case P_GE: return P_LE;
    default: return op;
    }
}

static int lg_inv(int op)
{
    switch (op) {
    case P_EQEQ: return P_NE;
    case P_NE: return P_EQEQ;
    case P_LT: return P_GE;
    case P_GE: return P_LT;
    case P_GT: return P_LE;
    default: return P_GT;
    }
}

/* e without the conversions fold removes from a comparison with zero. */
static uint32_t lg_strip(Checker *c, uint32_t e)
{
    for (;;) {
        uint32_t k[3], n;
        e = strip_paren(c, e);
        if (ntag(c, e) != N_CAST || (n = nkids(c, e, k, 3)) < 1)
            return e;
        if (!is_int(c, rvt(c, e)) || !is_int(c, rvt(c, k[n - 1])) ||
            int_bits(c, rvt(c, e)) < int_bits(c, rvt(c, k[n - 1])))
            return e;
        e = k[n - 1];
    }
}

/* A null pointer or integer zero constant. */
static bool lg_zero(Checker *c, uint32_t n)
{
    return is_npc(c, n) || (c->ck[n] == K_ADDR && !c->cb[n] && !c->cv[n]);
}

/* The operand as the comparison gcc builds from it (c_common_truthvalue_
 * conversion, with ! folded into the comparison).  False if not a test. */
static bool lg_form(Checker *c, uint32_t n, LForm *f)
{
    uint32_t k[3], cnt;
    n = strip_paren(c, n);
    cnt = nkids(c, n, k, 3);
    if (ntag(c, n) == N_BINARY && lg_cmp(npunct(c, n)) && cnt >= 2) {
        f->op = npunct(c, n);
        f->l = k[0];
        f->r = k[1];
        /* x == 0 and x != 0 are the truth conversions of x and !x */
        if (f->op == P_EQEQ || f->op == P_NE) {
            if (lg_zero(c, f->r) && !lg_zero(c, f->l)) {
                f->r = NO_NODE;
            } else if (lg_zero(c, f->l) && !lg_zero(c, f->r)) {
                f->l = f->r;
                f->r = NO_NODE;
            }
            if (f->r == NO_NODE)
                f->l = lg_strip(c, f->l);
        }
        return true;
    }
    if (ntag(c, n) == N_BINARY &&
        (npunct(c, n) == P_ANDAND || npunct(c, n) == P_OROR))
        return false;
    if (ntag(c, n) == N_UNARY && npunct(c, n) == P_BANG && cnt >= 1) {
        uint32_t in = strip_paren(c, k[0]);
        if (!lg_form(c, in, f))
            return false;
        if (f->r != NO_NODE && (is_flt(c, rvt(c, f->l)) || is_flt(c, rvt(c, f->r))))
            return false;
        f->op = lg_inv(f->op);
        return true;
    }
    if (ntag(c, n) == N_CAST && tkind(c, rvt(c, n)) == TY_BOOL && cnt >= 1)
        return lg_form(c, k[cnt - 1], f);
    f->op = P_NE;
    f->l = lg_strip(c, n);
    f->r = NO_NODE;
    return true;
}

static bool lg_eqn(Checker *c, uint32_t x, uint32_t y)
{
    if (x == NO_NODE || y == NO_NODE)
        return x == y;
    return opeq(c, x, y);
}

static bool lg_same(Checker *c, const LForm *a, const LForm *b)
{
    return (a->op == b->op && lg_eqn(c, a->l, b->l) && lg_eqn(c, a->r, b->r)) ||
           (a->r != NO_NODE && b->r != NO_NODE && a->op == lg_swap(b->op) &&
            lg_eqn(c, a->l, b->r) && lg_eqn(c, a->r, b->l));
}

/* A test gcc's warning looks at: a comparison, ! or integral value. */
static bool lg_eligible(Checker *c, uint32_t n)
{
    n = strip_paren(c, n);
    if (ntag(c, n) == N_BINARY && (lg_cmp(npunct(c, n)) ||
                                   npunct(c, n) == P_ANDAND ||
                                   npunct(c, n) == P_OROR))
        return true;
    if (ntag(c, n) == N_UNARY && npunct(c, n) == P_BANG)
        return true;
    return is_int(c, rvt(c, n));
}

static i128 lg_val(Checker *c, uint32_t n)
{
    bool neg;
    uint64_t mag;
    cst_parts(c, n, &neg, &mag);
    return neg ? -(i128)mag : (i128)mag;
}

static void iv_add(IvSet *s, i128 lo, i128 hi)
{
    if (lo <= hi && s->n < 4) {
        s->lo[s->n] = lo;
        s->hi[s->n++] = hi;
    }
}

/* The values v in [mn, mx] with 'v op k'. */
static void iv_cmp(IvSet *s, int op, i128 k, i128 mn, i128 mx)
{
    s->n = 0;
    switch (op) {
    case P_EQEQ: iv_add(s, k < mn ? mx + 1 : k, k > mx ? mn - 1 : k); break;
    case P_NE:
        iv_add(s, mn, k - 1 < mx ? k - 1 : mx);
        iv_add(s, k + 1 > mn ? k + 1 : mn, mx);
        if (k < mn || k > mx) {
            s->n = 0;
            iv_add(s, mn, mx);
        }
        break;
    case P_LT: iv_add(s, mn, k - 1 < mx ? k - 1 : mx); break;
    case P_LE: iv_add(s, mn, k < mx ? k : mx); break;
    case P_GT: iv_add(s, k + 1 > mn ? k + 1 : mn, mx); break;
    default: iv_add(s, k > mn ? k : mn, mx); break;
    }
}

static void iv_comp(IvSet *r, const IvSet *s, i128 mn, i128 mx)
{
    i128 at = mn;
    int i;
    r->n = 0;
    for (i = 0; i < s->n; i++) {
        iv_add(r, at, s->lo[i] - 1);
        at = s->hi[i] + 1;
    }
    iv_add(r, at, mx);
}

static void iv_and(IvSet *r, const IvSet *a, const IvSet *b)
{
    int i, j;
    r->n = 0;
    for (i = 0; i < a->n; i++)
        for (j = 0; j < b->n; j++)
            iv_add(r, a->lo[i] > b->lo[j] ? a->lo[i] : b->lo[j],
                   a->hi[i] < b->hi[j] ? a->hi[i] : b->hi[j]);
}

/* The expression a test constrains and the values that make it true. */
static bool lg_atom(Checker *c, const LForm *f, uint32_t *e, IvSet *s,
                    i128 *mn, i128 *mx)
{
    int op = f->op;
    i128 k = 0;
    TypeId t;
    unsigned bits;
    if (f->r == NO_NODE) {
        *e = f->l;
    } else if (is_intcst(c, f->r) && !is_intcst(c, f->l)) {
        *e = f->l;
        k = lg_val(c, f->r);
    } else if (is_intcst(c, f->l) && !is_intcst(c, f->r)) {
        *e = f->r;
        op = lg_swap(op);
        k = lg_val(c, f->l);
    } else {
        return false;
    }
    t = rvt(c, *e);
    if (is_ptr(c, t)) {
        *mn = 0;
        *mx = ((i128)1 << int_bits(c, t)) - 1;
    } else if (is_int(c, t)) {
        bits = tkind(c, t) == TY_BOOL ? 1 : int_bits(c, t);
        if (is_signed(c, t)) {
            *mn = -((i128)1 << (bits - 1));
            *mx = ((i128)1 << (bits - 1)) - 1;
        } else {
            *mn = 0;
            *mx = ((i128)1 << bits) - 1;
        }
    } else {
        return false;
    }
    iv_cmp(s, op, k, *mn, *mx);
    return true;
}

/* An operator expression from a macro (constants and names carry no location
 * in gcc's tree). */
static bool lg_macro(Checker *c, uint32_t n)
{
    return ntag(c, n) != N_IDENT && ntag(c, n) != N_NUMBER &&
           ntag(c, n) != N_CHAR && from_macro(c, c->nodes[n].tok);
}

static void logical_op_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    bool orop = op == P_OROR;
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b), ea, eb;
    SrcLoc loc = cnode_loc(c, i);
    LForm fa, fb;
    IvSet ia, ib, ca, cb, r;
    i128 mna, mxa, mnb, mxb;
    bool okb;
    if (!diag_enabled(c->diag, "logical-op") || inhibited(c, i, false) ||
        lg_macro(c, sa) || lg_macro(c, sb))
        return;
    if (is_intcst(c, sb) && is_int(c, rvt(c, sb)) && truth(c, sa, true) < 0 &&
        !(ntag(c, sb) == N_BINARY && npunct(c, sb) == P_COMMA) &&
        !is_intcst(c, sa)) {
        bool neg;
        uint64_t mag;
        int ta = ntag(c, a), pa = npunct(c, a);
        bool truthy = (ta == N_BINARY && (lg_cmp(pa) || pa == P_ANDAND ||
                                          pa == P_OROR)) ||
                      (ta == N_UNARY && pa == P_BANG);
        cst_parts(c, sb, &neg, &mag);
        if (!truthy && (neg || mag > 1)) {
            cwarn(c, loc, "logical-op", "logical '%s' applied to non-boolean "
                  "constant", orop ? "or" : "and");
            return;
        }
    }
    if (truth(c, sa, true) >= 0 || truth(c, sb, true) >= 0)
        return;
    if (!lg_form(c, sa, &fa) || !(okb = lg_form(c, sb, &fb)))
        return;
    (void)okb;
    /* the left operand is already a truth value, the right one is not */
    if (lg_eligible(c, sb) && lg_same(c, &fa, &fb)) {
        cwarn(c, loc, "logical-op", "logical '%s' of equal expressions",
              orop ? "or" : "and");
        return;
    }
    if (!lg_atom(c, &fa, &ea, &ia, &mna, &mxa) ||
        !lg_atom(c, &fb, &eb, &ib, &mnb, &mxb) || mna != mnb || mxa != mxb ||
        !opeq(c, ea, eb))
        return;
    iv_comp(&ca, &ia, mna, mxa);
    iv_comp(&cb, &ib, mna, mxa);
    if (orop ? (ca.n == 0 || cb.n == 0) : (ia.n == 0 || ib.n == 0))
        return;     /* one side alone is already always true / false */
    if (orop) {
        iv_and(&r, &ca, &cb);
        if (r.n == 0)
            cwarn(c, loc, "logical-op", "logical 'or' of collectively "
                  "exhaustive tests is always true");
    } else {
        iv_and(&r, &ia, &ib);
        if (r.n == 0)
            cwarn(c, loc, "logical-op", "logical 'and' of mutually exclusive "
                  "tests is always false");
    }
}

static void e_logical(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    bool andand = op == P_ANDAND, ok;
    int ta, tb, r = -1;
    /* gcc converts the left operand when it sees the operator and stops at an
     * error; a right operand of aggregate type is just an invalid operand */
    ok = truth_ok(c, a);
    if (ok && (is_record(c, rvt(c, b)) || tkind(c, rvt(c, b)) == TY_VECTOR)) {
        vec_invalid(c, i, a, b, op, TYPE_B(INT), 0);   /* a is already an int */
        return;
    }
    ok = ok && truth_ok(c, b);
    if (!ok) {
        set_err(c, i);
        return;
    }
    logical_op_warn(c, i, a, b, op);
    cexpr_truth_warn(c, a, first_loc(c, a));
    cexpr_truth_warn(c, b, cnode_loc(c, i));
    c->ty[i] = TYPE_B(INT);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    ta = truth(c, a, true);
    tb = truth(c, b, true);
    if (ta >= 0 && tb >= 0)
        r = andand ? (ta && tb) : (ta || tb);
    else if (ta >= 0 && ta == !andand)
        r = !andand;
    if (r < 0) {
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
        return;
    }
    c->cv[i] = (uint64_t)r;
    /* DR#031: an unevaluated integer-operand right side (2 || 1/0) is OK */
    if (is_intcst(c, a) && (is_intcst(c, b) ||
        (!(tb >= 0) && c->ck[a] == K_ICE && intops(c, b)))) {
        c->ck[i] = K_ICE;
        c->ef[i] |= EF_INTOPS | ((c->ef[a] | c->ef[b]) & EF_OVERFLOW);
        return;
    }
    c->ck[i] = K_FOLD;
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
}

static bool is_npc(Checker *c, uint32_t n)
{
    return (c->ef[n] & EF_NPC) != 0;
}

bool extra_on(Checker *c)
{
    return diag_enabled(c->diag, "sign-compare");
}

/* Is the node a zero character constant (for -Wpointer-compare)? */
static bool char_zero(Checker *c, uint32_t n)
{
    TypeId t;
    if (!is_npc(c, n))
        return false;
    if (ntag(c, strip_paren(c, n)) == N_CHAR)
        return true;
    t = mainv(c, rvt(c, n));
    return t == TYPE_B(CHAR) || t == TYPE_B(SCHAR) || t == TYPE_B(UCHAR);
}

static bool addr_val(Checker *c, uint32_t n, uint32_t *cb, uint64_t *cv)
{
    if (c->ck[n] == K_ADDR) {
        *cb = c->cb[n];
        *cv = c->cv[n];
        return true;
    }
    if (has_ival(c, n)) {
        *cb = 0;
        *cv = c->cv[n];
        return true;
    }
    return false;
}

/* A base symbol whose address may equal another's (weak) or is not
 * an object or function. */
static bool addr_weak(Checker *c, uint32_t cb)
{
    CSym *s = csym(c, cb - 1);
    return (s->kind != CS_OBJ && s->kind != CS_FUNC) ||
           (s->flags & CSF_WEAK);
}

static bool from_macro(Checker *c, uint32_t tok);

/* ---- -Wall / -Wextra expression warnings -------------------------------------
 * -Wsign-compare, -Wtype-limits, -Wbool-compare, -Wtautological-compare,
 * -Wparentheses, -Wlogical-not-parentheses, -Warray-compare, -Waddress and
 * -Wsizeof-pointer-div (gcc's shorten_compare, warn_for_sign_compare,
 * warn_about_parentheses, maybe_warn_for_null_address, ...). */

bool is_cmp_op(int op)
{
    return op == P_EQEQ || op == P_NE || op == P_LT || op == P_GT ||
           op == P_LE || op == P_GE;
}

static const char *cmp_str(int op)
{
    switch (op) {
    case P_EQEQ: return "==";
    case P_NE: return "!=";
    case P_LT: return "<";
    case P_GT: return ">";
    case P_LE: return "<=";
    default: return ">=";
    }
}

static int swap_cmp(int op)
{
    switch (op) {
    case P_LT: return P_GT;
    case P_GT: return P_LT;
    case P_LE: return P_GE;
    case P_GE: return P_LE;
    default: return op;
    }
}

/* CONSTANT_CLASS_P of the folded operand. */
static bool cst_class(Checker *c, uint32_t n)
{
    return c->ck[n] == K_ICE || c->ck[n] == K_FOLD || c->ck[n] == K_FLOAT;
}

/* gcc's truth_value_p of the operand's tree code. */
static bool truth_expr(Checker *c, uint32_t n)
{
    n = strip_paren(c, n);
    if (cst_class(c, n))
        return false;
    if (ntag(c, n) == N_BINARY) {
        switch (npunct(c, n)) {
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR:
            return true;
        default:
            return false;
        }
    }
    return ntag(c, n) == N_UNARY && npunct(c, n) == P_BANG;
}

static bool is_boolish(Checker *c, uint32_t n)
{
    return tkind(c, rvt(c, n)) == TY_BOOL || truth_expr(c, n);
}

/* The node's value when it is an integer constant: *neg, the magnitude. */
static void cst_parts(Checker *c, uint32_t n, bool *neg, uint64_t *mag)
{
    TypeId t = rvt(c, n);
    uint64_t v = c->cv[n];
    *neg = ival_neg(c, t, v);
    *mag = *neg ? (uint64_t)0 - v : v;
}

/* The precision of n before its widening conversions when it is an unsigned
 * integer type, else 0 (tree_binary_nonnegative_warnv_p's zero-extension). */
static unsigned zext_bits(Checker *c, uint32_t n)
{
    TypeId t;
    n = strip_paren(c, n);
    if (ntag(c, n) == N_CAST) {
        uint32_t k[3], cnt = nkids(c, n, k, 3);
        if (cnt >= 1)
            n = strip_paren(c, k[cnt - 1]);
    }
    t = rvt(c, n);
    return is_int(c, t) && tkind(c, t) != TY_BOOL && !is_signed(c, t)
           ? int_bits(c, t) : 0;
}

/* tree_expr_nonnegative_p of a (signed) operand. */
static bool nonneg(Checker *c, uint32_t n)
{
    TypeId t;
    uint32_t k[3], cnt;
    n = strip_paren(c, n);
    t = rvt(c, n);
    if (is_intcst(c, n))
        return !ival_neg(c, t, c->cv[n]);
    if (!is_int(c, t))
        return false;
    if (!is_signed(c, t))
        return true;
    switch (ntag(c, n)) {
    case N_CAST: {
        uint32_t in;
        TypeId it;
        cnt = nkids(c, n, k, 3);
        if (cnt < 1)
            return false;
        in = k[cnt - 1];
        it = rvt(c, in);
        if (!is_int(c, it))
            return false;
        if (!is_signed(c, it))
            return int_bits(c, it) < int_bits(c, t);
        return int_bits(c, it) <= int_bits(c, t) && nonneg(c, in);
    }
    case N_BINARY:
        if (nkids(c, n, k, 3) < 2)
            return false;
        switch (npunct(c, n)) {
        case P_AMP: return nonneg(c, k[0]) || nonneg(c, k[1]);
        case P_PIPE: case P_CARET: case P_SLASH:
            return nonneg(c, k[0]) && nonneg(c, k[1]);
        case P_PERCENT: case P_SHR: return nonneg(c, k[0]);
        case P_COMMA: return nonneg(c, k[1]);
        case P_STAR: /* signed overflow is undefined */
            return nonneg(c, k[0]) && nonneg(c, k[1]);
        case P_PLUS: { /* two zero-extended operands cannot overflow */
            unsigned x = zext_bits(c, k[0]), y = zext_bits(c, k[1]);
            return x && y && (x > y ? x : y) + 1 < int_bits(c, t);
        }
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR:
            return true;
        default: return false;
        }
    case N_ASSIGN:
        return npunct(c, n) == P_ASSIGN && nkids(c, n, k, 3) >= 2 &&
               nonneg(c, k[1]);
    case N_STMT_EXPR:
        return (cnt = stmt_expr_value(c, n)) != NO_NODE && nonneg(c, cnt);
    case N_UNARY:
        if (nkids(c, n, k, 3) < 1)
            return false;
        if (npunct(c, n) == P_BANG)
            return true;
        return npunct(c, n) == P_PLUS && nonneg(c, k[0]);
    case N_COND:
        cnt = nkids(c, n, k, 3);
        if (cnt == 3)
            return nonneg(c, k[1]) && nonneg(c, k[2]);
        return cnt == 2 && nonneg(c, k[0]) && nonneg(c, k[1]);
    default:
        return false;
    }
}

/* The declared width of the bit-field node n (0: not a bit-field). */
unsigned bf_width(Checker *c, uint32_t n)
{
    uint32_t d;
    TypeId rec;
    const Field *f;
    uint64_t off = 0;
    unsigned q = 0;
    n = strip_paren(c, n);
    if (!(c->ef[n] & EF_BITFIELD) || ntag(c, n) != N_MEMBER_EXPR ||
        (d = first_child(c, n)) == NO_NODE)
        return 0;
    rec = (c->nodes[n].flags & NF_ARROW) ? pointee(c, rvt(c, d)) : c->ty[d];
    f = find_field(c, rec, cnode_ident(c, n), &off, &q);
    return f && (f->flags & FF_BITFIELD) ? f->width : 0;
}

/* The type the operand n has in the sign comparison: an unsigned bit-field
 * narrower than int has been promoted to int. */
static TypeId cmp_ty(Checker *c, uint32_t n)
{
    if (c->ef[strip_paren(c, n)] & EF_BFPROMOTE)
        return TYPE_B(INT);
    return rvt(c, n);
}

/* The type of n as gcc names it in -Wsign-compare (a bit-field has a type
 * of its own width: 'signed char:4'). */
static const char *bf_tstr(Checker *c, TypeId t, unsigned w)
{
    bool sg = is_signed(c, t);
    if (w && is_int(c, t) && tkind(c, t) != TY_BOOL && w < int_bits(c, t)) {
        const char *base = w <= 8 ? (sg ? "signed char" : "unsigned char")
                         : w <= 16 ? (sg ? "short int" : "short unsigned int")
                         : w <= 32 ? (sg ? "int" : "unsigned int")
                                   : (sg ? "long int" : "long unsigned int");
        StrBuf *sb = &c->esb[c->enext++ & 1];
        sb->len = 0;
        sb_printf(sb, "'%s:%u'", base, w);
        return sb_cstr(sb);
    }
    return type_q(TT, t);
}

static const char *cmp_tstr(Checker *c, uint32_t n)
{
    return bf_tstr(c, rvt(c, n), bf_width(c, n));
}

/* warn_for_sign_compare */
static void sign_compare(Checker *c, SrcLoc loc, uint32_t a, uint32_t b,
                         int op, TypeId rt)
{
    TypeId ta = cmp_ty(c, a), tb = cmp_ty(c, b);
    bool sa, sb;
    uint32_t sop, uop;
    /* a complex integer type counts by its component type */
    if (is_complex(c, ta))
        ta = cplx_comp(c, ta);
    if (is_complex(c, tb))
        tb = cplx_comp(c, tb);
    if (is_complex(c, rt))
        rt = cplx_comp(c, rt);
    if (is_signed(c, rt) || !is_int(c, ta) || !is_int(c, tb))
        return;
    sa = is_signed(c, ta);
    sb = is_signed(c, tb);
    if (sa == sb)
        return;
    sop = sa ? a : b;
    uop = sa ? b : a;
    if (nonneg(c, sop))
        return;
    if ((op == P_EQEQ || op == P_NE) && is_intcst(c, uop)) {
        unsigned bits = int_bits(c, rt);
        uint64_t smax = bits >= 64 ? INT64_MAX : (UINT64_C(1) << (bits - 1)) - 1;
        if (c->cv[uop] <= smax)
            return;
    }
    cwarn(c, loc, "sign-compare", "comparison of integer expressions of "
          "different signedness: %s and %s", cmp_tstr(c, a), cmp_tstr(c, b));
}

/* c_common_get_narrower: strip the widening conversions of n.  *bf: a
 * bit-field (not narrowed here). */
static uint32_t narrower(Checker *c, uint32_t n, TypeId *ty, unsigned *prec)
{
    *prec = 0;
    for (;;) {
        uint32_t s = strip_paren(c, n), k[3], cnt;
        if (is_intcst(c, s))
            break;
        if (c->ef[s] & EF_BITFIELD) {
            unsigned w = bf_width(c, s);
            TypeId bt = rvt(c, s);
            /* a signed bit-field narrows to its width; an unsigned one
             * narrower than int stays the int it was promoted to */
            if (w && is_int(c, bt) && w < int_bits(c, bt) &&
                !((c->ef[s] & EF_BFPROMOTE) && !is_signed(c, bt)))
                *prec = w;
            else if (c->ef[s] & EF_BFPROMOTE) {
                *ty = TYPE_B(INT);
                return s;
            }
            break;
        }
        if (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
            nkids(c, s, k, 3) == 2) {
            n = k[1];
            continue;
        }
        if (ntag(c, s) == N_CAST) {
            TypeId ot = mainv(c, c->ty[s]), it;
            cnt = nkids(c, s, k, 3);
            if (cnt >= 1 && is_int(c, ot)) {
                it = rvt(c, k[cnt - 1]);
                if (is_int(c, it) && int_bits(c, it) < int_bits(c, ot) &&
                    !(!is_signed(c, ot) && is_signed(c, it))) {
                    n = k[cnt - 1];
                    continue;
                }
            }
        }
        break;
    }
    n = strip_paren(c, n);
    *ty = rvt(c, n);
    return n;
}

/* The value v of a constant converted to a type of width bits. */
static uint64_t norm_val(uint64_t v, unsigned bits, bool sgn)
{
    if (bits < 64) {
        v &= (UINT64_C(1) << bits) - 1;
        if (sgn && (v >> (bits - 1)))
            v |= ~((UINT64_C(1) << bits) - 1);
    }
    return v;
}

static bool val_lt(uint64_t a, uint64_t b, bool sgn)
{
    return sgn ? (int64_t)a < (int64_t)b : a < b;
}

/* The ~E of a comparison operand x, E an unsigned value narrower than the
 * type x is compared in: *w is that type's width, *p the width of E before
 * its promotion (-Wsign-compare's "promoted bitwise complement"). */
static bool bitnot_operand(Checker *c, uint32_t x, unsigned *w, unsigned *p)
{
    uint32_t u = strip_paren(c, x), k[3];
    TypeId t;
    unsigned prec;
    if (u == NO_NODE)
        return false;
    *w = int_bits(c, rvt(c, u));
    if (ntag(c, u) == N_CAST) {
        unsigned n = nkids(c, u, k, 3);
        if (!n || !is_int(c, c->ty[u]))
            return false;
        *w = int_bits(c, c->ty[u]);
        u = strip_paren(c, k[n - 1]);
    }
    if (u == NO_NODE || ntag(c, u) != N_UNARY || npunct(c, u) != P_TILDE ||
        nkids(c, u, k, 2) < 1)
        return false;
    narrower(c, k[0], &t, &prec);
    if (!is_int(c, t) || is_signed(c, t) || tkind(c, t) == TY_BOOL)
        return false;
    *p = prec ? prec : int_bits(c, t);
    return *p < *w;
}

/* The "promoted bitwise complement of an unsigned value" warnings: ~E can
 * only equal values with ones above E's width. */
static bool bitnot_cmp(Checker *c, SrcLoc loc, uint32_t a, uint32_t b)
{
    unsigned w, p, i;
    for (i = 0; i < 2; i++) {
        uint32_t x = i ? b : a, y = strip_paren(c, i ? a : b);
        if (!bitnot_operand(c, x, &w, &p))
            continue;
        if (is_intcst(c, y)) {
            uint64_t m = w >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << w) - 1;
            uint64_t ones = m & ~((UINT64_C(1) << p) - 1);
            uint64_t v = c->cv[y] & m;
            if ((v & ones) == ones)
                return false;
            cwarn(c, loc, "sign-compare", c->cv[y] == 0 ?
                  "promoted bitwise complement of an unsigned value is always "
                  "nonzero" : "comparison of promoted bitwise complement of an "
                  "unsigned value with constant");
            return true;
        } else {
            TypeId t;
            unsigned prec;
            narrower(c, y, &t, &prec);
            if (!is_int(c, t) || is_signed(c, t) || tkind(c, t) == TY_BOOL ||
                (prec ? prec : int_bits(c, t)) >= w)
                return false;
            cwarn(c, loc, "sign-compare", "comparison of promoted bitwise "
                  "complement of an unsigned value with unsigned");
            return true;
        }
    }
    return false;
}

/* shorten_compare's diagnostics.  True when gcc folds the comparison (and
 * so skips the sign-compare check). */
static bool type_limits(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId rt, SrcLoc loc)
{
    TypeId t0, t1, tx;
    unsigned pr0, pr1, prx;
    bool rts, warn;
    uint32_t p0 = narrower(c, a, &t0, &pr0), p1 = narrower(c, b, &t1, &pr1), y;
    int code = op;
    const char *why = NULL;
    if (!is_int(c, rt) || is_flt(c, rvt(c, a)) || is_flt(c, rvt(c, b)) ||
        int_bits(c, rt) > 64)
        return false;
    if (is_intcst(c, p0) && is_intcst(c, p1))
        return false;
    y = p1;
    tx = t0;
    prx = pr0;
    if (is_intcst(c, p0)) {
        y = p0;
        tx = t1;
        prx = pr1;
        code = swap_cmp(op);
    }
    if (!is_intcst(c, y))
        return false;
    warn = !inhibited(c, i, false) && !from_macro(c, c->nodes[i].tok);
    rts = is_signed(c, rt);
    /* constant on the left of a narrower operand: only the range message */
    if (!rts && c->cv[y] == 0 && (code == P_GE || code == P_LT) &&
        !(y == p0 && (prx ? prx : int_bits(c, tx)) < int_bits(c, rt))) {
        if (warn && tkind(c, tx) != TY_ENUM)
            cwarn(c, loc, "type-limits", code == P_GE
                  ? "comparison of unsigned expression in '>= 0' is always true"
                  : "comparison of unsigned expression in '< 0' is always false");
        return true;
    }
    if (!is_int(c, tx) || tkind(c, tx) == TY_BOOL || tkind(c, tx) == TY_ENUM ||
        (prx ? prx : int_bits(c, tx)) >= int_bits(c, rt))
        return false;
    {
        unsigned p = prx ? prx : int_bits(c, tx), w = int_bits(c, rt);
        bool xs = is_signed(c, tx), sgn = rts || xs;
        uint64_t mn, mx, v, yv = c->cv[y];
        bool min_gt, max_gt, min_lt, max_lt;
        int val = 0;
        yv = norm_val(yv, int_bits(c, rvt(c, y)), is_signed(c, rvt(c, y)));
        v = norm_val(yv, w, sgn);
        if (xs) {
            mn = norm_val(norm_val(UINT64_C(1) << (p - 1), p, true), w, true);
            mx = (UINT64_C(1) << (p - 1)) - 1;
        } else {
            mn = 0;
            mx = (UINT64_C(1) << p) - 1;
        }
        min_gt = val_lt(v, mn, sgn);
        max_gt = val_lt(v, mx, sgn);
        min_lt = val_lt(mn, v, sgn);
        max_lt = val_lt(mx, v, sgn);
        switch (code) {
        case P_NE:
            if (max_lt || min_gt) val = 1;
            break;
        case P_EQEQ:
            if (max_lt || min_gt) val = -1;
            break;
        case P_LT:
            if (max_lt) val = 1;
            if (!min_lt) val = -1;
            break;
        case P_GT:
            if (min_gt) val = 1;
            if (!max_gt) val = -1;
            break;
        case P_LE:
            if (!max_gt) val = 1;
            if (min_gt) val = -1;
            break;
        default:
            if (!min_lt) val = 1;
            if (max_lt) val = -1;
            break;
        }
        if (!rts && xs && code != P_EQEQ && code != P_NE)
            val = 0;
        if (!val)
            return false;
        why = val > 0 ? "comparison is always true due to limited range of "
                        "data type"
                      : "comparison is always false due to limited range of "
                        "data type";
    }
    if (warn)
        cwarn(c, loc, "type-limits", "%s", why);
    return true;
}

/* maybe_warn_bool_compare */
static bool bool_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                         SrcLoc loc)
{
    bool ba = is_boolish(c, a), bb = is_boolish(c, b), neg;
    uint32_t k;
    uint64_t mag;
    int r[2], x;
    if (ba == bb || inhibited(c, i, false))
        return false;
    k = ba ? b : a;
    if (!is_intcst(c, k))
        return false;
    cst_parts(c, k, &neg, &mag);
    for (x = 0; x < 2; x++) {
        int cmp;    /* x <=> constant */
        if (neg)
            cmp = 1;
        else
            cmp = (uint64_t)x < mag ? -1 : (uint64_t)x > mag;
        if (!ba)
            cmp = -cmp;     /* the constant is the left operand */
        switch (op) {
        case P_EQEQ: r[x] = cmp == 0; break;
        case P_NE: r[x] = cmp != 0; break;
        case P_LT: r[x] = cmp < 0; break;
        case P_GT: r[x] = cmp > 0; break;
        case P_LE: r[x] = cmp <= 0; break;
        default: r[x] = cmp >= 0; break;
        }
    }
    if (r[0] != r[1])
        return false;
    cwarn(c, loc, "bool-compare", "comparison of constant '%s' with boolean "
          "expression is always %s", vstr(c, rvt(c, k), c->cv[k]),
          r[0] ? "true" : "false");
    return true;
}

/* ---- -Wtautological-compare ---- */

/* operand_equal_p (x, y, 0), for the expressions this checker can see. */
bool opeq(Checker *c, uint32_t x, uint32_t y)
{
    uint32_t kx[3], ky[3], nx, ny;
    x = strip_paren(c, x);
    y = strip_paren(c, y);
    if (ntag(c, x) == N_STMT_EXPR && stmt_expr_single(c, x) != NO_NODE)
        x = strip_paren(c, stmt_expr_single(c, x));
    if (ntag(c, y) == N_STMT_EXPR && stmt_expr_single(c, y) != NO_NODE)
        y = strip_paren(c, stmt_expr_single(c, y));
    if (ntag(c, x) != ntag(c, y) ||
        (ntag(c, x) != N_CALL && (c->ef[x] | c->ef[y]) & EF_SIDE))
        return false;
    if (tquals(c, c->ty[x]) & TQ_VOLATILE)
        return false;
    if (is_intcst(c, x) || is_intcst(c, y))
        return is_intcst(c, x) && is_intcst(c, y) &&
               mainv(c, c->ty[x]) == mainv(c, c->ty[y]) && c->cv[x] == c->cv[y];
    nx = nkids(c, x, kx, 3);
    ny = nkids(c, y, ky, 3);
    switch (ntag(c, x)) {
    case N_IDENT:
        return cnode_ident(c, x) == cnode_ident(c, y) &&
               lookup_ord(c, cnode_ident(c, x)) != SYM_NONE;
    case N_MEMBER_EXPR:
        return cnode_ident(c, x) == cnode_ident(c, y) &&
               (c->nodes[x].flags & NF_ARROW) == (c->nodes[y].flags & NF_ARROW) &&
               nx >= 1 && ny >= 1 && opeq(c, kx[0], ky[0]);
    case N_INDEX:
        return nx == 2 && ny == 2 && opeq(c, kx[0], ky[0]) &&
               opeq(c, kx[1], ky[1]);
    case N_UNARY:
        switch (npunct(c, x)) {
        case P_STAR: case P_AMP: case P_MINUS: case P_PLUS: case P_TILDE:
        case P_BANG:
            return npunct(c, x) == npunct(c, y) && nx >= 1 && ny >= 1 &&
                   opeq(c, kx[0], ky[0]);
        default:
            return false;
        }
    case N_BINARY: {
        int op = npunct(c, x);
        if (op != npunct(c, y) || op == P_COMMA || nx < 2 || ny < 2)
            return false;
        if (opeq(c, kx[0], ky[0]) && opeq(c, kx[1], ky[1]))
            return true;
        if (op == P_PLUS || op == P_STAR || op == P_AMP || op == P_PIPE ||
            op == P_CARET || op == P_EQEQ || op == P_NE)
            return opeq(c, kx[0], ky[1]) && opeq(c, kx[1], ky[0]);
        return false;
    }
    case N_CAST:
        return nx >= 1 && ny >= 1 && mainv(c, c->ty[x]) == mainv(c, c->ty[y]) &&
               opeq(c, kx[nx - 1], ky[ny - 1]);
    case N_CALL: { /* calls of a const or pure function with equal arguments */
        uint32_t ref, j;
        if (nx < 1 || nx != ny || ntag(c, strip_paren(c, kx[0])) != N_IDENT ||
            ntag(c, strip_paren(c, ky[0])) != N_IDENT || nx > 3)
            return false;
        ref = lookup_ord(c, cnode_ident(c, strip_paren(c, kx[0])));
        if (ref == SYM_NONE || ref != lookup_ord(c, cnode_ident(c, strip_paren(c, ky[0]))) ||
            !(csym(c, ref)->flags & (CSF_PURE | CSF_CONSTFN)))
            return false;
        for (j = 1; j < nx; j++)
            if (!opeq(c, kx[j], ky[j]))
                return false;
        return true;
    }
    default:
        return false;
    }
}

/* The first token of loc's line: where gcc reports a restrict clash it has no
 * better location for. */
static SrcLoc line_start_loc(Checker *c, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    uint32_t line, col, len, j = 0;
    const char *s;
    if (!f)
        return loc;
    srcmgr_linecol(f, loc, &line, &col);
    s = srcmgr_line_text(f, line, &len);
    while (j < len && (s[j] == 32 || s[j] == 9))
        j++;
    return j < col ? srcmgr_loc_of(f, line, j + 1) : loc;
}

/* The built-ins whose call with a zero size is not diagnosed. */
bool zero_size_ok(const char *name)
{
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    return !strcmp(name, "memcpy") || !strcmp(name, "strncpy") ||
           !strcmp(name, "strncat");
}

/* The address an argument stands for: casts, `&E[0]` and `E + 0` look through. */
static uint32_t restrict_base(Checker *c, uint32_t a)
{
    for (;;) {
        uint32_t k[3], n, in, ik[3];
        a = strip_paren(c, a);
        n = nkids(c, a, k, 3);
        if (ntag(c, a) == N_CAST && n >= 1 && is_ptr(c, c->ty[a])) {
            a = k[n - 1];
        } else if (ntag(c, a) == N_UNARY && npunct(c, a) == P_AMP && n == 1 &&
                   ntag(c, in = strip_paren(c, k[0])) == N_INDEX &&
                   nkids(c, in, ik, 3) == 2 && is_intcst(c, ik[1]) &&
                   c->cv[ik[1]] == 0 && is_ptr(c, c->ty[strip_paren(c, ik[0])])) {
            a = ik[0];
        } else if (ntag(c, a) == N_BINARY && npunct(c, a) == P_PLUS && n == 2 &&
                   is_ptr(c, rvt(c, k[0])) && is_intcst(c, k[1]) &&
                   c->cv[k[1]] == 0) {
            a = k[0];
        } else {
            return a;
        }
    }
}

/* c-family warn_for_restrict: two arguments of restrict-qualified parameters
 * that are the same address.  A built-in copy of zero bytes is fine. */
bool zero_size_ok(const char *name);
void check_restrict(Checker *c, const uint32_t *kv, uint32_t nk,
                           uint32_t parms, uint32_t nparm, SrcLoc loc,
                           bool builtin, bool is_bt)
{
    uint32_t i, j;
    /* each restrict parameter against every other argument (the variadic ones
     * too); a pair of restrict ones once, the lower first */
    for (i = 0; i < nparm && i + 1 < nk; i++) {
        if (!cparm_restrict(c, parms, i))
            continue;
        for (j = 0; j + 1 < nk; j++) {
            uint32_t a = kv[i + 1], first;
            SrcLoc l = line_start_loc(c, loc);
            if (j == i || (j < i && j < nparm && cparm_restrict(c, parms, j)))
                continue;
            if (is_bt && j >= nparm)   /* the middle end handles those */
                continue;
            if (node_err(c, a) || node_err(c, kv[j + 1]) ||
                !is_ptr(c, rvt(c, a)) || !is_ptr(c, rvt(c, kv[j + 1])))
                continue;
            if (!opeq(c, restrict_base(c, a), restrict_base(c, kv[j + 1])))
                continue;
            if (builtin && nparm == 3 && nk > 3 && is_intcst(c, kv[3]) &&
                c->cv[kv[3]] == 0)
                continue;
            first = restrict_base(c, a);
            if (ntag(c, first) != N_IDENT || is_array(c, c->ty[first]))
                l = expr_loc(c, a);
            cwarn(c, l, "restrict", "passing argument %u to 'restrict'-qualified "
                  "parameter aliases with argument %u", i + 1, j + 1);
        }
    }
}

/* warn_tautological_cmp */
static void tauto_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b), bit, cst, n;
    TypeId t;
    if (!is_cmp_op(op) || from_macro(c, c->nodes[i].tok))
        return;
    if (op == P_EQEQ || op == P_NE) {
        bit = NO_NODE;
        cst = NO_NODE;
        if (ntag(c, sa) == N_BINARY && (npunct(c, sa) == P_AMP ||
                                        npunct(c, sa) == P_PIPE) &&
            is_intcst(c, sb)) {
            bit = sa;
            cst = sb;
        } else if (ntag(c, sb) == N_BINARY && (npunct(c, sb) == P_AMP ||
                                               npunct(c, sb) == P_PIPE) &&
                   is_intcst(c, sa)) {
            bit = sb;
            cst = sa;
        }
        if (bit != NO_NODE) {
            uint32_t k[3], kc = NO_NODE;
            if (nkids(c, bit, k, 3) == 2) {
                if (is_intcst(c, k[0]))
                    kc = k[0];
                else if (is_intcst(c, k[1]))
                    kc = k[1];
            }
            if (kc != NO_NODE) {
                TypeId bt = rvt(c, kc);
                unsigned bits = int_bits(c, bt);
                uint64_t cv = norm_val(c->cv[cst], bits, is_signed(c, bt));
                uint64_t bv = norm_val(c->cv[kc], bits, is_signed(c, bt));
                uint64_t res = norm_val(npunct(c, bit) == P_AMP ? cv & bv : cv | bv,
                                        bits, is_signed(c, bt));
                if (res != cv && bits <= 64)
                    cwarn(c, loc, "tautological-compare", op == P_EQEQ
                          ? "bitwise comparison always evaluates to false"
                          : "bitwise comparison always evaluates to true");
            }
        }
    }
    if (cst_class(c, sa) || cst_class(c, sb))
        return;
    if (ntag(c, sa) == N_CAST || ntag(c, sb) == N_CAST)
        return;
    t = c->ty[sa];
    if (is_flt(c, t) || is_complex(c, t) || is_array(c, t) ||
        is_record(c, t) || tkind(c, t) == TY_VECTOR)
        return;
    for (n = sa; ntag(c, n) == N_INDEX || ntag(c, n) == N_MEMBER_EXPR;) {
        uint32_t k[3];      /* a constant index anywhere in the access chain */
        if (nkids(c, n, k, 3) < 1)
            break;
        if (ntag(c, n) == N_INDEX && nkids(c, n, k, 3) == 2 &&
            is_array(c, c->ty[strip_paren(c, k[0])]) && is_intcst(c, k[1]))
            return;
        n = strip_paren(c, k[0]);
    }
    if (!opeq(c, sa, sb))
        return;
    cwarn(c, loc, "tautological-compare", "self-comparison always evaluates "
          "to %s", (op == P_EQEQ || op == P_LE || op == P_GE) ? "true"
                                                              : "false");
}

/* ---- -Wparentheses ---- */

/* The operator of an unparenthesized binary operand (gcc's original_code). */
static int orig_op(Checker *c, uint32_t n)
{
    int op;
    if (ntag(c, n) != N_BINARY)
        return 0;
    op = npunct(c, n);
    return op == P_COMMA ? 0 : op;
}

#define PW(l, ...) cwarn(c, (l), "parentheses", __VA_ARGS__)

/* `!a & b`, `!a | b`: the right operand is no truth value (gcc's original
 * code is not a comparison, && , || or !). */
static bool lognot_bitop(Checker *c, uint32_t a, uint32_t b, int op, SrcLoc la)
{
    uint32_t k[3], r;
    int rop;
    if (ntag(c, a) != N_UNARY || npunct(c, a) != P_BANG || nkids(c, a, k, 3) < 1)
        return false;
    r = strip_paren(c, b);
    rop = ntag(c, r) == N_BINARY ? npunct(c, r) : 0;
    if (is_cmp_op(rop) || rop == P_ANDAND || rop == P_OROR ||
        (ntag(c, r) == N_UNARY && npunct(c, r) == P_BANG))
        return false;
    PW(la, "suggest parentheses around operand of '!' or "
       "change '%s' to '%s' or '!' to '~'", op == P_AMP ? "&" : "|",
       op == P_AMP ? "&&" : "||");
    return true;
}

/* warn_about_parentheses */
static void parens_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    int ca = orig_op(c, a), cb = orig_op(c, b);
    SrcLoc la = cst_class(c, a) ? loc : cnode_loc(c, a);
    SrcLoc lb = cst_class(c, b) ? loc : cnode_loc(c, b);
    switch (op) {
    case P_SHL: case P_SHR: {
        const char *s = op == P_SHL ? "<<" : ">>";
        if (ca == P_PLUS)
            PW(la, "suggest parentheses around '+' inside '%s'", s);
        else if (cb == P_PLUS)
            PW(lb, "suggest parentheses around '+' inside '%s'", s);
        else if (ca == P_MINUS)
            PW(la, "suggest parentheses around '-' inside '%s'", s);
        else if (cb == P_MINUS)
            PW(lb, "suggest parentheses around '-' inside '%s'", s);
        return;
    }
    case P_OROR:
        if (ca == P_ANDAND)
            PW(la, "suggest parentheses around '&&' within '||'");
        else if (cb == P_ANDAND)
            PW(lb, "suggest parentheses around '&&' within '||'");
        return;
    case P_PIPE:
        if (lognot_bitop(c, a, b, op, la))
            return;
        if (ca == P_AMP || ca == P_CARET || ca == P_PLUS || ca == P_MINUS)
            PW(la, "suggest parentheses around arithmetic in operand of '|'");
        else if (cb == P_AMP || cb == P_CARET || cb == P_PLUS || cb == P_MINUS)
            PW(lb, "suggest parentheses around arithmetic in operand of '|'");
        else if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '|'");
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '|'");
        return;
    case P_CARET:
        if (ca == P_AMP || ca == P_PLUS || ca == P_MINUS)
            PW(la, "suggest parentheses around arithmetic in operand of '^'");
        else if (cb == P_AMP || cb == P_PLUS || cb == P_MINUS)
            PW(lb, "suggest parentheses around arithmetic in operand of '^'");
        else if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '^'");
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '^'");
        return;
    case P_AMP:
        if (lognot_bitop(c, a, b, op, la))
            return;
        if (ca == P_PLUS)
            PW(la, "suggest parentheses around '+' in operand of '&'");
        else if (cb == P_PLUS)
            PW(lb, "suggest parentheses around '+' in operand of '&'");
        else if (ca == P_MINUS)
            PW(la, "suggest parentheses around '-' in operand of '&'");
        else if (cb == P_MINUS)
            PW(lb, "suggest parentheses around '-' in operand of '&'");
        else if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '&'");
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '&'");
        return;
    case P_EQEQ: case P_NE:
        if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '%s'",
               cmp_str(op));
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '%s'",
               cmp_str(op));
        return;
    case P_LT: case P_GT: case P_LE: case P_GE:
        if (is_cmp_op(ca))
            PW(la, "comparisons like 'X<=Y<=Z' do not have their mathematical "
                   "meaning");
        else if (is_cmp_op(cb))
            PW(lb, "comparisons like 'X<=Y<=Z' do not have their mathematical "
                   "meaning");
        return;
    default:
        return;
    }
}

/* A bit operation (| ^ & ~, int casts) whose leaves are all boolean. */
static bool boolish_bits(Checker *c, uint32_t n)
{
    uint32_t k[3];
    unsigned cnt;
    n = strip_paren(c, n);
    if (is_boolish(c, n))
        return true;
    cnt = nkids(c, n, k, 3);
    if (ntag(c, n) == N_CAST && tkind(c, mainv(c, c->ty[n])) == TY_INT && cnt)
        return boolish_bits(c, k[cnt - 1]);
    if (ntag(c, n) == N_UNARY && npunct(c, n) == P_TILDE && cnt == 1)
        return boolish_bits(c, k[0]);
    if (ntag(c, n) == N_BINARY && cnt == 2 &&
        (npunct(c, n) == P_PIPE || npunct(c, n) == P_CARET ||
         npunct(c, n) == P_AMP))
        return boolish_bits(c, k[0]) && boolish_bits(c, k[1]);
    return false;
}

/* warn_logical_not_parentheses (the caller of it in parser_build_binary_op
 * included). */
static void lognot_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    uint32_t k[3], x, y;
    Diagnostic *d;
    if (!is_cmp_op(op) || ntag(c, a) != N_UNARY || npunct(c, a) != P_BANG ||
        (ntag(c, b) == N_UNARY && npunct(c, b) == P_BANG) ||
        nkids(c, a, k, 3) < 1)
        return;
    x = strip_paren(c, k[0]);
    if (ntag(c, x) == N_UNARY && npunct(c, x) == P_BANG && !cst_class(c, x))
        return;
    if (ntag(c, x) == N_BINARY && npunct(c, x) == P_EQEQ &&
        nkids(c, x, k, 3) == 2 && is_intcst(c, k[1]) && c->cv[k[1]] == 0)
        return;
    y = x;
    if (tkind(c, rvt(c, y)) == TY_INT) {
        while (ntag(c, y) == N_CAST && tkind(c, mainv(c, c->ty[y])) == TY_INT &&
               nkids(c, y, k, 3) >= 1)
            y = strip_paren(c, k[nkids(c, y, k, 3) - 1]);
    }
    if (tkind(c, rvt(c, y)) == TY_BOOL)
        return;
    if (tkind(c, rvt(c, b)) == TY_BOOL || truth_expr(c, b) ||
        (is_intcst(c, b) && c->cv[b] == 0 && (op == P_EQEQ || op == P_NE)) ||
        boolish_bits(c, b))
        return;
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "logical-not-parentheses",
                "logical not is only applied to the left hand side of "
                "comparison");
    cnote(c, d, first_loc(c, a), "add parentheses around left hand side "
          "expression to silence this warning");
}

/* ---- -Warray-compare ---- */

static void array_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    Diagnostic *d;
    SrcLoc loc = cnode_loc(c, i);
    if (!is_cmp_op(op) || ntag(c, a) != N_IDENT || ntag(c, b) != N_IDENT ||
        !is_array(c, c->ty[a]) || !is_array(c, c->ty[b]))
        return;
    d = cwarn_d(c, DL_WARNING, loc, "array-compare", "comparison between two "
                "arrays");
    cnote(c, d, loc, "use '&%s[0] %s &%s[0]' to compare the addresses",
          cident(c, cnode_ident(c, a)), cmp_str(op),
          cident(c, cnode_ident(c, b)));
}

/* ---- -Waddress (the address of an object is never null) ---- */

typedef struct AddrInfo {
    const char *name;
    SrcLoc dloc;
    bool direct;         /* the address of a declared object, itself */
    char buf[96];        /* storage for a '*p' name */
    bool label;          /* &&label */
    uint32_t ref;        /* its symbol, SYM_NONE for a member */
} AddrInfo;

/* decl_with_nonnull_addr_p: a weak declaration may be null, a weak definition
 * (not a weakref) cannot be. */
static bool weak_maybe_null(const CSym *s)
{
    return (s->flags & CSF_WEAK) &&
           ((s->flags & CSF_WEAKREF) || (s->flags & CSF_DECL_EXTERNAL));
}

/* '*p' for an identifier p that points to an array. */
static bool deref_name(Checker *c, uint32_t id, AddrInfo *ai)
{
    uint32_t ref;
    id = strip_paren(c, id);
    if (ntag(c, id) != N_IDENT || !is_ptr(c, rvt(c, id)) ||
        !is_array(c, pointee(c, rvt(c, id))))
        return false;
    ref = lookup_ord(c, cnode_ident(c, id));
    if (ref == SYM_NONE)
        return false;
    snprintf(ai->buf, sizeof ai->buf, "*%s", cident(c, csym(c, ref)->name));
    ai->name = ai->buf;
    ai->dloc = csym(c, ref)->loc;
    return true;
}

/* The object whose address the pointer-valued expression n is, if it is one
 * gcc's decl_with_nonnull_addr_p accepts. */
static bool addr_target(Checker *c, uint32_t n, AddrInfo *ai)
{
    uint32_t k[3], e;
    bool deref = false;
    n = strip_paren(c, n);
    memset(ai, 0, sizeof *ai);
    ai->ref = SYM_NONE;
    if (ntag(c, n) == N_ADDR_LABEL) { /* a label is never null */
        ai->name = cident(c, cnode_ident(c, n));
        ai->direct = true;
        ai->label = true;
        return true;
    }
    if (ntag(c, n) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, n));
        CSym *s;
        if (ref == SYM_NONE) {
            const char *nm = cident(c, cnode_ident(c, n));
            if (!is_func(c, c->ty[n]) || strncmp(nm, "__builtin_", 10))
                return false;
            ai->name = nm; /* a built-in function has no symbol */
            ai->direct = true;
            return true;
        }
        s = csym(c, ref);
        if (!((s->kind == CS_FUNC) ||
              (s->kind == CS_OBJ && is_array(c, c->ty[n]))) ||
            weak_maybe_null(s))
            return false;
        ai->name = cident(c, s->name);
        ai->dloc = s->loc;
        ai->direct = true;
        ai->ref = ref;
        return true;
    }
    if ((ntag(c, n) == N_MEMBER_EXPR || ntag(c, n) == N_INDEX) &&
        is_array(c, c->ty[n])) {
        e = n; /* an array decaying to the address of its element */
    } else {
        if (ntag(c, n) != N_UNARY || npunct(c, n) != P_AMP ||
            nkids(c, n, k, 3) < 1)
            return false;
        e = strip_paren(c, k[0]);
        if (ntag(c, e) == N_UNARY && (tckw(c, c->nodes[e].tok) == CK_REAL ||
                                      tckw(c, c->nodes[e].tok) == CK_IMAG) &&
            nkids(c, e, k, 3) >= 1) {
            /* &__real__ x: never null; named '__real__ x' */
            uint32_t in = strip_paren(c, k[0]);
            StrBuf sb = {0};
            if (ntag(c, in) == N_CALL ||
                (ntag(c, in) == N_UNARY && npunct(c, in) != P_STAR))
                return false;
            sb_puts(&sb, tckw(c, c->nodes[e].tok) == CK_REAL ? "__real__ "
                                                              : "__imag__ ");
            pexpr(c, &sb, in, PR_UNARY);
            snprintf(ai->buf, sizeof ai->buf, "%s", sb_cstr(&sb));
            sb_free(&sb);
            {   /* gcc prints a call as f() */
                char *sp = strstr(ai->buf, " ()");
                if (sp)
                    memmove(sp, sp + 1, strlen(sp));
            }
            ai->name = ai->buf;
            return true;
        }
    }
    for (;;) {
        if (ntag(c, e) == N_MEMBER_EXPR) {
            uint32_t d = first_child(c, e);
            bool arrow = (c->nodes[e].flags & NF_ARROW) != 0;
            TypeId rec;
            const Field *f;
            uint64_t off = 0;
            unsigned q = 0;
            if (d == NO_NODE)
                return false;
            rec = arrow ? pointee(c, rvt(c, d)) : c->ty[d];
            f = find_field(c, rec, cnode_ident(c, e), &off, &q);
            if (!f || !f->name)
                return false;
            ai->name = cident(c, f->name);
            ai->dloc = f->loc;
            return true;
        }
        if (ntag(c, e) == N_INDEX) {
            uint32_t base;
            if (nkids(c, e, k, 3) < 1)
                return false;
            base = strip_paren(c, k[0]);
            if (!is_array(c, c->ty[base]))
                return deref_name(c, base, ai);
            e = base;
            deref = true;
            continue;
        }
        break;
    }
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_STAR && is_array(c, c->ty[e]) &&
        nkids(c, e, k, 3) >= 1)
        return deref_name(c, k[0], ai);
    if (ntag(c, e) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, e));
        CSym *s;
        if (ref == SYM_NONE)
            return false;
        s = csym(c, ref);
        if ((s->kind != CS_OBJ && s->kind != CS_FUNC) || weak_maybe_null(s))
            return false;
        ai->name = cident(c, s->name);
        ai->dloc = s->loc;
        ai->direct = !deref;
        ai->ref = ref;
        return true;
    }
    return false;
}

/* x is 'ptr +- int' or '&ptr[int]': the pointer, the integer and the sign. */
static bool plus_split(Checker *c, uint32_t x, uint32_t *pn, uint32_t *in,
                       int *op)
{
    uint32_t k[3];
    x = strip_paren(c, x);
    if (ntag(c, x) == N_UNARY && npunct(c, x) == P_AMP &&
        nkids(c, x, k, 3) >= 1) {
        uint32_t e = strip_paren(c, k[0]), ek[3];
        if (ntag(c, e) != N_INDEX || nkids(c, e, ek, 3) != 2)
            return false;
        if (is_ptr(c, rvt(c, ek[0])) && !is_array(c, c->ty[strip_paren(c, ek[0])]) &&
            is_int(c, rvt(c, ek[1]))) {
            *pn = ek[0];
            *in = ek[1];
            *op = P_PLUS;
            return true;
        }
        return false;
    }
    if (ntag(c, x) == N_INDEX && is_array(c, c->ty[x]) && nkids(c, x, k, 3) == 2 &&
        is_ptr(c, rvt(c, k[0])) && !is_array(c, c->ty[strip_paren(c, k[0])]) &&
        is_int(c, rvt(c, k[1]))) {
        *pn = k[0];
        *in = k[1];
        *op = P_PLUS;
        return true;
    }
    if (ntag(c, x) != N_BINARY || nkids(c, x, k, 3) != 2)
        return false;
    *op = npunct(c, x);
    if (*op != P_PLUS && *op != P_MINUS)
        return false;
    if (is_ptr(c, rvt(c, k[0])) && is_int(c, rvt(c, k[1]))) {
        *pn = k[0];
        *in = k[1];
        return true;
    }
    if (*op == P_PLUS && is_ptr(c, rvt(c, k[1])) && is_int(c, rvt(c, k[0]))) {
        *pn = k[1];
        *in = k[0];
        return true;
    }
    return false;
}

/* maybe_warn_for_null_address's POINTER_PLUS_EXPR case: x is 'ptr +- N'.
 * Prints the sum as gcc's folded tree: base + (sizetype)((unsigned long)N * size).
 * Returns true when x is such a sum (warned or not). */
static bool ptr_plus_warn(Checker *c, SrcLoc loc, uint32_t x, int code)
{
    uint32_t pn, in, pn2, in2;
    int op, op2;
    int64_t k = 0;
    uint32_t var = NO_NODE;
    bool vneg = false;
    uint64_t sz;
    TypeId pt;
    StrBuf sb = {0};
    if (!plus_split(c, x, &pn, &in, &op))
        return false;
    pt = rvt(c, x);
    if (!is_ptr(c, pt) || is_void(c, pointee(c, pt)))
        return true;
    sz = elem_size(c, is_array(c, c->ty[strip_paren(c, pn)]) ? pt : rvt(c, pn));
    for (;;) {
        uint32_t ik[3];
        /* gcc distributes a constant: p + (i + 1) is p + i + 1 */
        in = strip_paren(c, in);
        if (!prints_value(c, in) && ntag(c, in) == N_BINARY &&
            (npunct(c, in) == P_PLUS || npunct(c, in) == P_MINUS) &&
            nkids(c, in, ik, 3) == 2 && prints_value(c, ik[1]) &&
            !prints_value(c, ik[0])) {
            int64_t v = (int64_t)cexpr_trunc(c, rvt(c, ik[1]), c->cv[ik[1]]);
            k += (op == P_MINUS) == (npunct(c, in) == P_MINUS) ? v : -v;
            in = ik[0];
        }
        if (prints_value(c, in)) {
            int64_t v = (int64_t)cexpr_trunc(c, rvt(c, in), c->cv[in]);
            k += op == P_MINUS ? -v : v;
        } else {
            if (var != NO_NODE)
                return true;
            var = in;
            vneg = op == P_MINUS;
        }
        pn = strip_paren(c, pn);
        if (!is_array(c, c->ty[pn]) && plus_split(c, pn, &pn2, &in2, &op2)) {
            pn = pn2;
            in = in2;
            op = op2;
            continue;
        }
        break;
    }
    if (ntag(c, pn) == N_UNARY && npunct(c, pn) == P_AMP) { /* &*p is p */
        uint32_t uk[3], inner;
        if (nkids(c, pn, uk, 3) >= 1) {
            inner = strip_paren(c, uk[0]);
            if (ntag(c, inner) == N_UNARY && npunct(c, inner) == P_STAR &&
                nkids(c, inner, uk, 3) >= 1 && !is_array(c, c->ty[inner]))
                pn = strip_paren(c, uk[0]);
        }
    }
    if (var == NO_NODE && k == 0) /* folds to the pointer itself */
        return true;
    {   /* a cast of an array: (char *)&a */
        uint32_t ck[3];
        if (ntag(c, pn) == N_CAST && is_ptr(c, rvt(c, pn)) &&
            nkids(c, pn, ck, 3) >= 1 &&
            is_array(c, c->ty[strip_paren(c, ck[nkids(c, pn, ck, 3) - 1])])) {
            pt = rvt(c, pn);
            pn = strip_paren(c, ck[nkids(c, pn, ck, 3) - 1]);
        }
    }
    if (is_array(c, c->ty[pn])) {
        sb_putc(&sb, '(');
        type_print(TT, &sb, pt);
        sb_putc(&sb, ')');
        if (ntag(c, pn) == N_UNARY && npunct(c, pn) == P_STAR) {
            uint32_t dk[3];
            if (nkids(c, pn, dk, 3) >= 1)
                pexpr(c, &sb, strip_paren(c, dk[0]), PR_UNARY);
        } else {
            sb_putc(&sb, '&');
            pexpr(c, &sb, pn, PR_UNARY);
        }
    } else {
        pexpr(c, &sb, pn, PR_ADD);
    }
    if (var == NO_NODE) {
        sb_printf(&sb, " + %lld", (long long)(k * (int64_t)sz));
    } else {
        TypeId vt = rvt(c, var);
        bool ul = !is_signed(c, vt) && type_size(TT, vt, NULL) == 8;
        StrBuf v = {0};
        if (sz == 1) {
            sb_puts(&v, "(sizetype)");
            pexpr(c, &v, var, PR_UNARY);
        } else {
            if (k) {
                sb_puts(&v, "((sizetype)");
                pexpr(c, &v, var, PR_UNARY);
                sb_printf(&v, " + %lld) * %llu", (long long)k,
                          (unsigned long long)sz);
            } else {
                sb_puts(&v, "(sizetype)(");
                if (!ul)
                    sb_puts(&v, "(long unsigned int)");
                pexpr(c, &v, var, PR_UNARY);
                sb_printf(&v, " * %llu)", (unsigned long long)sz);
            }
        }
        sb_printf(&sb, " + %s%s", vneg ? "-" : "", sb_cstr(&v));
        if (sz == 1 && k)
            sb_printf(&sb, " + %lld", (long long)k);
        sb_free(&v);
    }
    cwarn(c, loc, "address", "the comparison will always evaluate as '%s' for "
          "the pointer operand in '%s' must not be NULL",
          code == P_EQEQ ? "false" : "true", sb_cstr(&sb));
    sb_free(&sb);
    return true;
}

static void null_addr_msg(Checker *c, SrcLoc loc, const AddrInfo *ai, int code)
{
    Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "address", "the comparison will "
                            "always evaluate as '%s' for the address of '%s' "
                            "will never be NULL",
                            code == P_EQEQ ? "false" : "true", ai->name);
    if (ai->dloc)
        cnote(c, d, ai->dloc, "'%s' declared here", ai->name);
}

/* maybe_warn_for_null_address */
static bool null_addr_warn(Checker *c, SrcLoc loc, uint32_t x, int code,
                           uint32_t cmp)
{
    AddrInfo ai;
    uint32_t k[3];
    x = strip_paren(c, x);
    if (ntag(c, x) == N_CAST) {
        if (is_int(c, mainv(c, c->ty[x])) || nkids(c, x, k, 3) < 1)
            return false;
        x = strip_paren(c, k[nkids(c, x, k, 3) - 1]);
    }
    if (ntag(c, x) == N_BINARY && npunct(c, x) == P_PLUS &&
        nkids(c, x, k, 3) == 2 && is_ptr(c, rvt(c, k[0])) &&
        prints_value(c, k[1]) && c->cv[k[1]] == 0) /* folds to the pointer */
        x = strip_paren(c, k[0]);
    /* judged by the comparison: G (*p, i) != 0 and 'malloc != 0' (malloc a
     * macro) are user code, F (p) with the comparison inside F is not */
    if (inhibited(c, x, false) || from_macro(c, c->nodes[cmp].tok))
        return false;
    if (ptr_plus_warn(c, loc, x, code))
        return true;
    if (!addr_target(c, x, &ai))
        return false;
    null_addr_msg(c, loc, &ai, code);
    return true;
}

/* c_common_truthvalue_conversion's -Waddress checks of the truth-value n
 * (the diagnostic location loc). */
/* Has the truth-value warning for label id already been given in this
 * function?  Records it if not. */
static bool label_warned(Checker *c, uint32_t id)
{
    uint32_t j;
    if (c->lbl_fn != c->func_sym) {
        c->lbl_fn = c->func_sym;
        c->lbl_n = 0;
    }
    for (j = 0; j < c->lbl_n; j++)
        if (c->lbl_ids[j] == id)
            return true;
    if (c->lbl_n < 64)
        c->lbl_ids[c->lbl_n++] = id;
    return false;
}

static SrcLoc colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els);

/* c_common_truthvalue_conversion's -Wint-in-bool-context: the expression n
 * is used as a truth value. */
static bool no_int_bool;

static void int_bool_warn(Checker *c, uint32_t n)
{
    uint32_t s = n, k[3], cnt;
    TypeId t;
    if (no_int_bool || !diag_enabled(c->diag, "int-in-bool-context") ||
        node_err(c, n))
        return;
    for (;;) {      /* operations that keep zero-ness */
        s = strip_paren(c, s);
        cnt = nkids(c, s, k, 3);
        if (ntag(c, s) == N_UNARY && (npunct(c, s) == P_PLUS ||
                                      npunct(c, s) == P_MINUS) && cnt == 1) {
            s = k[0];
        } else if (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
                   cnt == 2) {
            s = k[1];
        } else if (ntag(c, s) == N_CAST && cnt >= 1 &&
                   is_arith(c, c->ty[s]) && tkind(c, c->ty[s]) != TY_BOOL &&
                   is_arith(c, rvt(c, k[cnt - 1])) &&
                   type_size(TT, c->ty[s], &(bool){0}) >=
                   type_size(TT, rvt(c, k[cnt - 1]), &(bool){0})) {
            s = k[cnt - 1];
        } else {
            break;
        }
    }
    if (is_intcst(c, s) || c->ck[s] == K_FLOAT || inhibited(c, s, false) ||
        node_err(c, s))
        return;
    t = rvt(c, s);
    if (ntag(c, s) == N_BINARY && npunct(c, s) == P_STAR && cnt == 2) {
        if (is_arith(c, t))
            cwarn(c, cnode_loc(c, s), "int-in-bool-context", "'*' in boolean "
                  "context, suggest '&&' instead");
    } else if (ntag(c, s) == N_BINARY && npunct(c, s) == P_SHL && cnt == 2) {
        if (is_int(c, t) && is_signed(c, type_int_promote(TT, rvt(c, k[0]))))
            cwarn(c, cnode_loc(c, s), "int-in-bool-context", "'<<' in boolean "
                  "context, did you mean '<'?");
    } else if (ntag(c, s) == N_COND && cnt == 3 && is_int(c, t)) {
        bool c1 = is_intcst(c, k[1]), c2 = is_intcst(c, k[2]);
        long long v1 = c1 ? (long long)c->cv[k[1]] : 0,
                  v2 = c2 ? (long long)c->cv[k[2]] : 0;
        bool nb1 = c1 && v1 != 0 && v1 != 1, nb2 = c2 && v2 != 0 && v2 != 1;
        if (!nb1 && !nb2)
            return;
        if (c1 && c2 && v1 != 0 && v2 != 0)
            cwarn(c, colon_loc(c, s, k[1], k[2]), "int-in-bool-context",
                  "'?:' using integer constants in boolean context, the "
                  "expression will always evaluate to 'true'");
        else
            cwarn(c, colon_loc(c, s, k[1], k[2]), "int-in-bool-context",
                  "'?:' using integer constants in boolean context");
    }
}

void cexpr_truth_warn(Checker *c, uint32_t n, SrcLoc loc)
{
    AddrInfo ai;
    uint32_t s = strip_paren(c, n), k[3];
    unsigned bw, bp;
    int_bool_warn(c, n);
    if (!node_err(c, n) && !inhibited(c, n, false) &&
        (is_flt(c, rvt(c, n)) || is_complex(c, rvt(c, n)))) {
        int rep = is_complex(c, rvt(c, n)) ? 2 : 1;     /* both parts */
        while (rep--)
            cwarn(c, loc, "float-equal", "comparing floating-point with "
                  "'==' or '!=' is unsafe");
    }
    if (diag_enabled(c->diag, "sign-compare") && !node_err(c, n) &&
        !inhibited(c, n, false) && ntag(c, s) == N_UNARY &&
        bitnot_operand(c, s, &bw, &bp)) {
        cwarn(c, loc, "sign-compare", "promoted bitwise complement of an "
              "unsigned value is always nonzero");
        return;
    }
    for (;;) { /* pointer conversions and '+ 0' fold away */
        if (ntag(c, s) == N_CAST && is_ptr(c, mainv(c, c->ty[s])) &&
            (nkids(c, s, k, 3) >= 1)) {
            s = strip_paren(c, k[nkids(c, s, k, 3) - 1]);
        } else if (ntag(c, s) == N_BINARY && npunct(c, s) == P_PLUS &&
                   nkids(c, s, k, 3) == 2 && is_npc(c, k[1]) &&
                   is_int(c, rvt(c, k[1]))) {
            s = strip_paren(c, k[0]);
        } else {
            break;
        }
    }
    if (node_err(c, n) || inhibited(c, n, false) || !is_ptr(c, rvt(c, n)))
        return;
    if (ptr_plus_warn(c, loc, s, P_NE))
        return;
    if (!addr_target(c, s, &ai))
        return;
    if (ai.label && label_warned(c, cnode_ident(c, s)))
        ai.label = false;       /* gcc says it once per label */
    if (ai.label || (ai.direct && ai.ref != SYM_NONE &&
        !(csym(c, ai.ref)->flags & CSF_ADDR_WARNED))) {
        if (!ai.label)
            csym(c, ai.ref)->flags |= CSF_ADDR_WARNED;
        else if (loc == ctok_loc(c, c->nodes[s].tok))
            loc = ctok_loc(c, c->nodes[s].tok - 1);   /* the && of &&label */
        cwarn(c, loc, "address", "the address of '%s' will always evaluate as "
              "'true'", ai.name);
        return;
    }
    null_addr_msg(c, loc, &ai, P_NE);
}

/* ---- -Wsizeof-pointer-memaccess ---- */

enum { MA_CMP = 1, MA_STR = 2, MA_COPY = 4 };
typedef struct {
    const char *name;
    signed char dst, src, len; /* argument positions, -1 for none */
    unsigned char flags;       /* compares / string functions / strn*cpy */
} MemAcc;

static const MemAcc memacc_tab[] = {
    {"strncmp", 0, 1, 2, MA_CMP | MA_STR},
    {"strncasecmp", 0, 1, 2, MA_CMP | MA_STR},
    {"strncpy", 0, 1, 2, MA_STR | MA_COPY},
    {"strncat", 0, 1, 2, MA_STR | MA_COPY},
    {"stpncpy", 0, 1, 2, MA_STR | MA_COPY},
    {"bcopy", 1, 0, 2, 0},
    {"memcpy", 0, 1, 2, 0},
    {"memmove", 0, 1, 2, 0},
    {"bcmp", 0, 1, 2, MA_CMP},
    {"memcmp", 0, 1, 2, MA_CMP},
    {"memset", 0, -1, 2, 0},
    {"bzero", 0, -1, 1, 0},
    {"memchr", -1, 0, 2, 0},
    {"snprintf", 0, -1, 1, MA_STR},
    {"vsnprintf", 0, -1, 1, MA_STR},
};

/* Is `a` (a call argument) the sizeof operand `x`?  `&*p` is `p` for gcc. */
static bool memacc_same(Checker *c, uint32_t a, uint32_t x)
{
    uint32_t k[3];
    a = strip_paren(c, a);
    x = strip_paren(c, x);
    if (ntag(c, a) == N_UNARY && npunct(c, a) == P_AMP &&
        nkids(c, a, k, 3) >= 1 && ntag(c, strip_paren(c, k[0])) == N_UNARY &&
        npunct(c, strip_paren(c, k[0])) == P_STAR &&
        nkids(c, strip_paren(c, k[0]), k, 3) >= 1)
        a = strip_paren(c, k[0]);
    if (ntag(c, a) == N_STRING && ntag(c, x) == N_STRING) {
        StrBuf s0 = {0}, s1 = {0};
        bool eq;
        pexpr(c, &s0, a, PR_UNARY);
        pexpr(c, &s1, x, PR_UNARY);
        eq = !strcmp(sb_cstr(&s0), sb_cstr(&s1));
        sb_free(&s0);
        sb_free(&s1);
        return eq;
    }
    return opeq(c, a, x);
}

/* An array whose size is known (a VLA included). */
static bool array_known(Checker *c, TypeId t)
{
    bool ok = false;
    if (tkind(c, t) == TY_VLA)
        return true;
    return tkind(c, t) == TY_ARRAY && (type_size(TT, t, &ok), ok);
}

/* integer_zerop of the converted operand: a null pointer constant, also
 * cast to another pointer type. */
static bool null_valued(Checker *c, uint32_t n)
{
    uint32_t k[3];
    n = strip_paren(c, n);
    while (is_npc(c, n) == false && ntag(c, n) == N_CAST &&
           is_ptr(c, c->ty[n]) && nkids(c, n, k, 3) >= 1)
        n = strip_paren(c, k[nkids(c, n, k, 3) - 1]);
    return is_npc(c, n);
}

/* A literal zero: one number or character token. */
static bool literal_zero(Checker *c, uint32_t a)
{
    uint32_t t;
    if (!is_intcst(c, a) || c->cv[a] != 0 || first_tok(c, a) != last_tok(c, a))
        return false;
    t = first_tok(c, a);
    return c->u->toks[t].t.kind == TK_PPNUM || c->u->toks[t].t.kind == TK_CHAR;
}

/* warn_for_memset: -Wmemset-transposed-args and -Wmemset-elt-size of a call
 * of memset (kv[0] the callee, then the arguments). */
void memset_args(Checker *c, const uint32_t *kv, uint32_t nk, const char *name)
{
    uint32_t a, ck[3], j;
    TypeId at, et;
    bool ok = false, ok2 = false;
    uint64_t sz, esz;
    if (strcmp(name, "memset") && strcmp(name, "__builtin_memset"))
        return;
    if (nk != 4)
        return;
    for (j = 1; j < 4; j++)
        if (node_err(c, kv[j]))
            return;
    if (diag_enabled(c->diag, "memset-transposed-args") &&
        literal_zero(c, kv[3]) && !literal_zero(c, kv[2]) &&
        is_intcst(c, kv[3]))
        cwarn(c, first_loc(c, kv[0]), "memset-transposed-args", "'memset' used "
              "with constant zero length parameter; this could be due to "
              "transposed parameters");
    if (!diag_enabled(c->diag, "memset-elt-size") || !is_intcst(c, kv[3]))
        return;
    a = strip_paren(c, kv[1]);
    while (ntag(c, a) == N_CAST && nkids(c, a, ck, 3) >= 1)
        a = strip_paren(c, ck[nkids(c, a, ck, 3) - 1]);
    if (ntag(c, a) == N_UNARY && npunct(c, a) == P_AMP &&
        nkids(c, a, ck, 3) == 1)
        a = strip_paren(c, ck[0]);
    at = c->ty[a];
    if (tkind(c, at) != TY_ARRAY)
        return;
    et = elem_of(c, at);
    sz = type_size(TT, at, &ok);
    esz = type_size(TT, et, &ok2);
    if (ok && ok2 && esz > 1 && sz / esz == (uint64_t)c->cv[kv[3]])
        cwarn(c, first_loc(c, kv[0]), "memset-elt-size", "'memset' used with "
              "length equal to number of elements without multiplication by "
              "element size");
}

void sizeof_memaccess(Checker *c, const uint32_t *kv, uint32_t nk,
                             const char *name)
{
    const char *nm = name;
    size_t j, len;
    const MemAcc *m = NULL;
    uint32_t sz, a, role;
    TypeId ty;
    bool is_expr, chk = false, legit = false;
    SrcLoc loc;
    if (!diag_enabled(c->diag, "sizeof-pointer-memaccess"))
        return;
    if (!strncmp(nm, "__builtin___", 12)) {
        nm += 12;
        chk = true;
    } else if (!strncmp(nm, "__builtin_", 10))
        nm += 10;
    len = strlen(nm);
    if (chk) {
        if (len < 5 || strcmp(nm + len - 4, "_chk"))
            return;
        len -= 4;
    }
    for (j = 0; j < sizeof memacc_tab / sizeof *memacc_tab; j++)
        if (strlen(memacc_tab[j].name) == len &&
            !strncmp(memacc_tab[j].name, nm, len))
            m = &memacc_tab[j];
    if (!m || nk < (uint32_t)m->len + 2 || nk < 3)
        return;
    sz = strip_paren(c, kv[1 + m->len]);
    if (ntag(c, sz) != N_SIZEOF_EXPR && ntag(c, sz) != N_SIZEOF_TYPE)
        return;
    a = first_child(c, sz);
    if (a == NO_NODE || node_err(c, a) || is_err(c, c->ty[a]))
        return;
    is_expr = ntag(c, sz) == N_SIZEOF_EXPR;
    ty = c->ty[a];
    loc = first_loc(c, a);
    for (role = 0; role < 2; role++) {
        int idx = role ? m->src : m->dst;
        if (idx >= 0 && node_err(c, kv[1 + idx]))
            return;
    }
    if (!is_ptr(c, ty)) {
        /* strncpy (d, s, sizeof s) with an array or literal s; an array
         * destination or a nonstring source is no mistake */
        uint32_t sa = (m->flags & MA_COPY) ? strip_paren(c, kv[1 + m->src])
                                           : NO_NODE;
        uint32_t sref = sa != NO_NODE && ntag(c, sa) == N_IDENT
                            ? lookup_ord(c, cnode_ident(c, sa)) : SYM_NONE;
        if ((m->flags & MA_COPY) && is_expr && is_array(c, ty) &&
            !array_known(c, c->ty[strip_paren(c, kv[1 + m->dst])]) &&
            !(sref != SYM_NONE &&
              cdecl_aset_has(c, csym(c, sref)->aset, "nonstring", NULL)) &&
            memacc_same(c, kv[1 + m->src], a) &&
            !memacc_same(c, kv[1 + m->dst], kv[1 + m->src]))
            cwarn(c, loc, "sizeof-pointer-memaccess", "argument to 'sizeof' "
                  "in '%s' call is the same expression as the source; did you "
                  "mean to use the size of the destination?", name);
        return;
    }
    /* memcpy (&p, q, sizeof p): an argument whose pointee is the sizeof type
     * is what the size is meant for (casts look through) */
    for (role = 0; role < 2; role++) {
        int idx = role ? m->src : m->dst;
        uint32_t arg;
        uint32_t ck[3];
        TypeId at;
        if (idx < 0)
            continue;
        arg = strip_paren(c, kv[1 + idx]);
        while (ntag(c, arg) == N_CAST && nkids(c, arg, ck, 3) >= 1)
            arg = strip_paren(c, ck[nkids(c, arg, ck, 3) - 1]);
        if (node_err(c, arg))
            continue;
        at = rvt(c, arg);
        if (is_ptr(c, at) && !is_void(c, pointee(c, at)) &&
            type_compatible(TT, mainv(c, pointee(c, at)), mainv(c, ty)))
            legit = true;
    }
    for (role = 0; role < 2; role++) {
        int idx = role ? m->src : m->dst;
        const char *what;
        uint32_t arg;
        TypeId at;
        bool same;
        if (idx < 0)
            continue;
        arg = strip_paren(c, kv[1 + idx]);
        if (node_err(c, arg) || ntag(c, arg) == N_CAST)
            continue;
        at = rvt(c, arg);
        if (!is_ptr(c, at))
            continue;
        what = role ? ((m->flags & MA_CMP) ? "second source" : "source")
                    : ((m->flags & MA_CMP) ? "first source" : "destination");
        if (m->dst < 0)
            what = "source";
        same = is_expr && memacc_same(c, arg, a);
        if (same) {
            const char *how;
            TypeId pt = pointee(c, at);
            uint32_t op = strip_paren(c, a);
            bool one = is_int(c, pt) && !is_void(c, pt);
            if (one) {
                bool ok = false;
                uint64_t sz1 = type_size(TT, pt, &ok);
                one = ok && sz1 == 1;
            }
            if (ntag(c, op) == N_UNARY && npunct(c, op) == P_AMP)
                how = "remove the addressof";
            else if ((m->flags & MA_STR) || one)
                how = "provide an explicit length";
            else
                how = "dereference it";
            cwarn(c, loc, "sizeof-pointer-memaccess", "argument to 'sizeof' "
                  "in '%s' call is the same expression as the %s; did you "
                  "mean to %s?", name, what, how);
            return;
        }
        if (!legit && !(m->flags & MA_STR) && !is_void(c, pointee(c, at)) &&
            tquals(c, ty) == tquals(c, at) &&
            type_compatible(TT, mainv(c, ty), mainv(c, at))) {
            char b0[256], b1[256];
            snprintf(b0, sizeof b0, "%s", type_q(TT, at));
            snprintf(b1, sizeof b1, "%s", type_q(TT, unqual(c, pointee(c, at))));
            cwarn(c, loc, "sizeof-pointer-memaccess", "argument to 'sizeof' "
                  "in '%s' call is the same pointer type %s as the %s; "
                  "expected %s or an explicit length", name, b0, what, b1);
            return;
        }
    }
}

/* ---- -Wsizeof-pointer-div ---- */

static bool sizeof_operand(Checker *c, uint32_t n, TypeId *ty, uint32_t *decl)
{
    uint32_t a;
    n = strip_paren(c, n);
    *decl = NO_NODE;
    if (ntag(c, n) != N_SIZEOF_TYPE && ntag(c, n) != N_SIZEOF_EXPR)
        return false;
    a = first_child(c, n);
    if (a == NO_NODE)
        return false;
    if (ntag(c, n) == N_SIZEOF_EXPR) {
        uint32_t s = strip_paren(c, a);
        if (node_err(c, a))
            return false;
        if (ntag(c, s) == N_IDENT)
            *decl = s;
    }
    *ty = c->ty[a];
    return !is_err(c, *ty);
}

static void sizeof_div(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId t0, t1;
    uint32_t d0, d1;
    StrBuf s0 = {0}, s1 = {0};
    Diagnostic *d;
    if (!sizeof_operand(c, a, &t0, &d0) || !sizeof_operand(c, b, &t1, &d1) ||
        is_err(c, t1))
        return;
    if (tkind(c, t0) == TY_ARRAY) {     /* -Wsizeof-array-div */
        TypeId et = elem_of(c, t0);
        bool ok0 = false, ok1 = false;
        char n0[256], n1[256];
        if (strip_paren(c, b) != b || !diag_enabled(c->diag, "sizeof-array-div") ||
            tkind(c, et) == TY_CHAR || tkind(c, et) == TY_SCHAR ||
            tkind(c, et) == TY_UCHAR || tkind(c, et) == TY_ARRAY ||
            type_size(TT, et, &ok0) == type_size(TT, t1, &ok1) || !ok0 || !ok1)
            return;
        snprintf(n0, sizeof n0, "%s", type_q(TT, unqual(c, et)));
        snprintf(n1, sizeof n1, "%s", type_q(TT, t1));
        d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "sizeof-array-div",
                    "expression does not compute the number of elements in "
                    "this array; element type is %s, not %s", n0, n1);
        cnote(c, d, cnode_loc(c, i), "add parentheses around the second "
              "'sizeof' to silence this warning");
        if (d0 != NO_NODE) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, d0));
            if (ref != SYM_NONE)
                cnote(c, d, csym(c, ref)->loc, "array '%s' declared here",
                      cident(c, cnode_ident(c, d0)));
        }
        return;
    }
    if (!is_ptr(c, t0))
        return;
    if (d0 != NO_NODE) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, d0));
        if (ref != SYM_NONE && (csym(c, ref)->flags & CSF_ARRAY_PARM))
            return;
    }
    if (tquals(c, pointee(c, t0)) != tquals(c, t1) ||
        !type_compatible(TT, mainv(c, pointee(c, t0)), mainv(c, t1)))
        return;
    type_print(TT, &s0, t0);
    type_print(TT, &s1, t1);
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "sizeof-pointer-div",
                "division 'sizeof (%s) / sizeof (%s)' does not compute the "
                "number of array elements", sb_cstr(&s0), sb_cstr(&s1));
    if (d0 != NO_NODE) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, d0));
        if (ref != SYM_NONE)
            cnote(c, d, csym(c, ref)->loc, "first 'sizeof' operand was "
                  "declared here");
    }
    sb_free(&s0);
    sb_free(&s1);
}

/* The warnings of build_binary_op's comparison case that precede the
 * operand checks of gcc's sign-compare (arithmetic operands). */
static bool compare_limits(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                           int op, TypeId ta, TypeId tb)
{
    SrcLoc loc = cnode_loc(c, i);
    bool folded, bfold;
    TypeId rt;
    bfold = bool_compare(c, i, a, b, op, loc);
    if (is_complex(c, ta) || is_complex(c, tb)) {
        /* complex integers: only the sign comparison applies */
        TypeId ca = is_complex(c, ta) ? cplx_comp(c, ta) : ta;
        TypeId cb = is_complex(c, tb) ? cplx_comp(c, tb) : tb;
        if (is_int(c, ca) && is_int(c, cb) && !inhibited(c, i, false) &&
            diag_enabled(c->diag, "sign-compare"))
            sign_compare(c, loc, a, b, op, common_type(c, ta, tb));
        return bfold;
    }
    if (!is_int(c, ta) || !is_int(c, tb))
        return bfold;
    rt = common_type(c, ta, tb);
    {   /* a bit-field wider than int keeps its own width in the conversions */
        unsigned wa = bf_width(c, a), wb = bf_width(c, b);
        unsigned ba = int_bits(c, ta), bb = int_bits(c, tb);
        if (wa >= 32 && wa < ba)
            ba = wa;
        if (wb >= 32 && wb < bb)
            bb = wb;
        if (ba < int_bits(c, ta) || bb < int_bits(c, tb)) {
            if (ba > bb)
                rt = ta;
            else if (bb > ba)
                rt = tb;
        }
    }
    if (!inhibited(c, i, false) && diag_enabled(c->diag, "sign-compare") &&
        bitnot_cmp(c, loc, a, b))
        return bfold;
    folded = type_limits(c, i, a, b, op, rt, loc);
    if (!folded && !inhibited(c, i, false) && diag_enabled(c->diag, "sign-compare"))
        sign_compare(c, loc, a, b, op, rt);
    return folded || bfold;
}


TypeId vec_elem(Checker *c, TypeId vt);
unsigned vec_esize(Checker *c, TypeId el);
static int vec_scalar(Checker *c, uint32_t i, uint32_t sn, TypeId st, TypeId vt,
                      bool strict_int);
static void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId la, TypeId lb);

static void e_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool eq = op == P_EQEQ || op == P_NE;
    bool pa = is_ptr(c, ta), pb = is_ptr(c, tb);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
    if (is_arith(c, ta) && is_arith(c, tb) && tkind(c, ta) != TY_VECTOR &&
        tkind(c, tb) != TY_VECTOR && dec_mix(c, i, ta, tb))
        return;
    if (is_arith(c, ta) && is_arith(c, tb) && tkind(c, ta) != TY_VECTOR &&
        tkind(c, tb) != TY_VECTOR && diag_enabled(c->diag, "double-promotion")) {
        TypeId rt = common_type(c, ta, tb);
        double_promo(c, i, loc, ta, rt, "to match other operand of binary "
                     "expression");
        double_promo(c, i, loc, tb, rt, "to match other operand of binary "
                     "expression");
    }
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        /* a vector comparison yields a signed integer vector of the same
         * lane size and count */
        TypeId vt = tkind(c, ta) == TY_VECTOR ? ta : tb, el, rt;
        bool ok;
        uint64_t esz;
        if (tkind(c, ta) == TY_VECTOR && tkind(c, tb) == TY_VECTOR) {
            TypeId e1 = vec_elem(c, ta), e2 = vec_elem(c, tb);
            if (vec_esize(c, e1) != vec_esize(c, e2) ||
                is_int(c, e1) != is_int(c, e2)) {
                cerror(c, loc, "comparing vectors with different element "
                       "types");
                set_err(c, i);
                return;
            }
            if (type_ent(TT, type_canon(TT, ta))->n !=
                type_ent(TT, type_canon(TT, tb))->n) {
                cerror(c, loc, "comparing vectors with different number of "
                       "elements");
                set_err(c, i);
                return;
            }
            if (is_int(c, e1) && is_signed(c, e1) != is_signed(c, e2) &&
                !inhibited(c, i, false))
                cwarn(c, loc, "sign-compare", "comparison between types %s "
                      "and %s", type_q(TT, rvt(c, a)), type_q(TT, rvt(c, b)));
        } else {
            bool lv = tkind(c, ta) == TY_VECTOR;
            int r = vec_scalar(c, i, lv ? b : a,
                               unqual(c, rvt(c, lv ? b : a)), vt, false);
            if (r < 0)
                return;
            if (!r) {
                vec_invalid(c, i, a, b, op, 0, 0);
                return;
            }
        }
        vt = type_canon(TT, vt);
        el = type_base(TT, vt);
        esz = type_size(TT, el, &ok);
        rt = esz == 1 ? TYPE_B(SCHAR) : esz == 2 ? TYPE_B(SHORT)
           : esz == 4 ? TYPE_B(INT) : esz == 8 ? TYPE_B(LONG) : 0;
        c->ty[i] = ok && rt ? type_vector(TT, rt, type_ent(TT, vt)->n)
                            : TYPE_B(INT);
        return;
    }
    if (!((is_arith(c, ta) && is_arith(c, tb)) || pa || pb) ||
        (!eq && (is_complex(c, ta) || is_complex(c, tb))) ||
        ((pa || pb) && !(pa ? (pb || is_int(c, tb)) : is_int(c, ta)))) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    c->ty[i] = TYPE_B(INT);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    {
        bool fold = false;
        if (is_arith(c, ta) && is_arith(c, tb))
            fold = compare_limits(c, i, a, b, op, ta, tb);
        else if (eq && pa && is_npc(c, b))
            fold = null_addr_warn(c, loc, a, op, i);
        else if (eq && pb && is_npc(c, a))
            fold = null_addr_warn(c, loc, b, op, i);
        if (!fold && !is_flt(c, ta) && !is_flt(c, tb) && opeq(c, a, b))
            fold = true; /* x == x and the like */
        if (fold)
            c->ef[i] |= EF_GCCFOLD;
    }
    if (eq && ((pa && char_zero(c, b)) || (pb && char_zero(c, a)))) {
        uint32_t p = pa ? a : b;
        Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "pointer-compare",
                                "comparison between pointer and zero "
                                "character constant");
        cnote(c, d, first_loc(c, p), "did you mean to dereference the pointer?");
    }
    if (pa && pb) {
        TypeId tta = pointee(c, ta), ttb = pointee(c, tb);
        bool compat = targets_compat(c, loc, tta, ttb);
        if (eq) {
            if (!compat) {
                if (is_void(c, tta) && !(tquals(c, tta) & TQ_ATOMIC)) {
                    if (is_func(c, ttb) && !is_npc(c, a))
                        ped(c, i, loc, "ISO C forbids comparison of 'void *' "
                                       "with function pointer");
                } else if (is_void(c, ttb) && !(tquals(c, ttb) & TQ_ATOMIC)) {
                    if (is_func(c, tta) && !is_npc(c, b))
                        ped(c, i, loc, "ISO C forbids comparison of 'void *' "
                                       "with function pointer");
                } else {
                    cpedwarn(c, loc, "", "comparison of distinct pointer types "
                             "lacks a cast");
                }
            }
        } else if (compat) {
            if (complete(c, tta) != complete(c, ttb))
                ped(c, i, loc, "comparison of complete and incomplete "
                               "pointers");
            else if (is_func(c, tta))
                ped(c, i, loc, "ISO C forbids ordered comparisons of pointers "
                               "to functions");
            else if (extra_on(c) && (is_npc(c, a) || is_npc(c, b)))
                cwarn(c, loc, "extra", "ordered comparison of pointer with null "
                                  "pointer");
        } else {
            cpedwarn(c, loc, "", "comparison of distinct pointer types lacks a "
                                "cast");
        }
    } else if (pa || pb) {
        uint32_t in = pa ? b : a;
        if (is_npc(c, in)) {
            if (!eq) {
                if (c->opt.pedantic && !cexpr_in_extension(c, i))
                    ped(c, i, loc, "ordered comparison of pointer with integer "
                                   "zero");
                else if (extra_on(c))
                    cwarn(c, loc, "extra", "ordered comparison of pointer with "
                                      "integer zero");
            }
        } else {
            cpedwarn(c, loc, "", "comparison between pointer and integer");
        }
    } else if (tkind(c, orig_type(c, a)) == TY_ENUM &&
               tkind(c, orig_type(c, b)) == TY_ENUM &&
               mainv(c, orig_type(c, a)) != mainv(c, orig_type(c, b))) {
        cwarn(c, loc, "enum-compare", "comparison between %s and %s",
              type_q(TT, unqual(c, orig_type(c, a))),
              type_q(TT, unqual(c, orig_type(c, b))));
    }
    if (eq && !inhibited(c, i, false) &&
        (is_flt(c, rvt(c, a)) || is_complex(c, rvt(c, a)) ||
         is_flt(c, rvt(c, b)) || is_complex(c, rvt(c, b))))
        cwarn(c, loc, "float-equal", "comparing floating-point with '==' or "
              "'!=' is unsafe");
    if (!inhibited(c, i, false) &&
        (eq ? (((c->ef[sa] & EF_STRING) && !null_valued(c, b)) ||
               ((c->ef[sb] & EF_STRING) && !null_valued(c, a)))
            : (((c->ef[sa] | c->ef[sb]) & EF_STRING) != 0)))
        cwarn(c, loc, "address", "comparison with string literal results in "
                                 "unspecified behavior");
    /* values */
    if (pa || pb) {
        uint32_t cba, cbb;
        uint64_t va, vb;
        int r = -1;
        if (addr_val(c, a, &cba, &va) && addr_val(c, b, &cbb, &vb)) {
            bool equal = cba == cbb && va == vb;
            if (cba == cbb) {
                int cmp = va < vb ? -1 : va > vb;
                switch (op) {
                case P_EQEQ: r = equal; break;
                case P_NE: r = !equal; break;
                case P_LT: r = cmp < 0; break;
                case P_GT: r = cmp > 0; break;
                case P_LE: r = cmp <= 0; break;
                default: r = cmp >= 0; break;
                }
            } else if (eq && ((cba == 0 && va == 0) || (cbb == 0 && vb == 0))) {
                r = op == P_NE;
            } else if (!eq && ((cba == 0 && va == 0 && cbb && !(cbb & CB_NODE) &&
                                !addr_weak(c, cbb)) ||
                               (cbb == 0 && vb == 0 && cba && !(cba & CB_NODE) &&
                                !addr_weak(c, cba)))) {
                int cmp = cba == 0 ? -1 : 1;    /* an object address > null */
                r = op == P_LT ? cmp < 0 : op == P_GT ? cmp > 0 :
                    op == P_LE ? cmp <= 0 : cmp >= 0;
            } else if (eq && cba && cbb && !(cba & CB_NODE) && !(cbb & CB_NODE) &&
                       !addr_weak(c, cba) && !addr_weak(c, cbb)) {
                r = op == P_NE;         /* distinct declared objects */
            }
        }
        if (r >= 0) {
            c->ck[i] = K_FOLD;
            c->cv[i] = (uint64_t)r;
        }
        return;
    }
    {
        TypeId rt = common_type(c, ta, tb);
        long double fa, fb;
        int r = -1;
        if (is_int(c, rt) && int_bits(c, rt) <= 64 && has_ival(c, a) &&
            has_ival(c, b)) {
            uint64_t x = cexpr_trunc(c, rt, c->cv[a]);
            uint64_t y = cexpr_trunc(c, rt, c->cv[b]);
            int cmp;
            if (is_signed(c, rt))
                cmp = (int64_t)x < (int64_t)y ? -1 : (int64_t)x > (int64_t)y;
            else
                cmp = x < y ? -1 : x > y;
            switch (op) {
            case P_EQEQ: r = cmp == 0; break;
            case P_NE: r = cmp != 0; break;
            case P_LT: r = cmp < 0; break;
            case P_GT: r = cmp > 0; break;
            case P_LE: r = cmp <= 0; break;
            default: r = cmp >= 0; break;
            }
            bin_value(c, i, a, b, (uint64_t)r, false, true, loc);
            return;
        }
        if (is_flt(c, rt) && !is_decimal_flt(c, rt) && fval(c, a, &fa) &&
            fval(c, b, &fb)) {
            switch (op) {
            case P_EQEQ: r = fa == fb; break;
            case P_NE: r = fa != fb; break;
            case P_LT: r = fa < fb; break;
            case P_GT: r = fa > fb; break;
            case P_LE: r = fa <= fb; break;
            default: r = fa >= fb; break;
            }
            c->ck[i] = K_FOLD;
            c->cv[i] = (uint64_t)r;
            return;
        }
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
    }
}

/* wi::min_precision (v, SIGNED) */
static unsigned min_prec_signed(uint64_t v)
{
    unsigned n = 0;
    if ((int64_t)v < 0)
        v = ~v;
    while (v) {
        n++;
        v >>= 1;
    }
    return n + 1;
}

/* ---- vector operands (c-typeck.cc build_binary_op, c-common.cc scalar_to_vector) ---- */

TypeId vec_elem(Checker *c, TypeId vt)
{
    return type_base(TT, type_canon(TT, vt));
}

unsigned vec_esize(Checker *c, TypeId el)
{
    bool ok;
    return (unsigned)type_size(TT, el, &ok);
}

/* gcc's INTEGER_TYPE: no _Bool, no enum. */
static bool int_type(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return is_int(c, t) && k != TY_BOOL && k != TY_ENUM;
}

/* unsafe_conversion_p of scalar operand s (type st) to vector element el. */
static bool vec_unsafe(Checker *c, uint32_t s, TypeId st, TypeId el)
{
    unsigned esz = vec_esize(c, el);
    long double f;
    if (is_int(c, el)) {
        unsigned p = int_bits(c, el), sp = int_bits(c, st);
        if (p >= sp)
            return false;
        if (has_ival(c, s)) {
            bool neg = ival_neg(c, st, c->cv[s]);
            int64_t v = (int64_t)c->cv[s];
            if (!neg && p < 64 && c->cv[s] >> (p - (is_signed(c, el) ? 1 : 0)))
                return true;
            if (neg && p < 64 && v < -((int64_t)1 << (p - 1)))
                return true;
            return false;
        }
        return true;
    }
    if (fval(c, s, &f)) {
        if (f != f)
            return false;
        if (esz == 4)
            return (long double)(float)f != f;
        if (esz == 8)
            return (long double)(double)f != f;
        return false;
    }
    if (is_int(c, st)) {
        unsigned mant = esz == 4 ? 24 : esz == 8 ? 53 : 64;
        return int_bits(c, st) > mant;
    }
    return vec_esize(c, st) > esz;
}

/* scalar_to_vector: operand sn (type st) of a binary operator with vector
 * type vt is converted to vt's element type.  1: converted, 0: not a scalar
 * it converts, -1: an error was reported. */
static int vec_scalar(Checker *c, uint32_t i, uint32_t sn, TypeId st, TypeId vt,
                      bool strict_int)
{
    TypeId el = vec_elem(c, vt);
    bool isc = int_type(c, st), fsc = is_flt(c, st);
    if (strict_int ? !isc : (!isc && !fsc))
        return 0;
    if (is_int(c, el) && fsc)
        return 0;
    if (vec_unsafe(c, sn, st, el)) {
        cerror(c, cnode_loc(c, i), "conversion of scalar %s to vector %s "
               "involves truncation", type_q(TT, st), type_q(TT, vt));
        set_err(c, i);
        return -1;
    }
    return 1;
}

/* "invalid operands": the scalar keeps its own type here, not the promoted
 * one; la/lb override an operand's type once it was converted. */
static void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId la, TypeId lb)
{
    cerror(c, cnode_loc(c, i), "invalid operands to binary %s (have %s and %s)",
           punct_spelling[op], type_q(TT, la ? la : unqual(c, rvt(c, a))),
           type_q(TT, lb ? lb : unqual(c, rvt(c, b))));
    set_err(c, i);
}

/* gcc's rules for a binary operator with a vector operand: both vectors of
 * the same shape, or a vector and a scalar converted to the element type.
 * False (after an error) when they do not apply. */
static bool vec_binop(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                      bool shift)
{
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool va = tkind(c, ta) == TY_VECTOR, vb = tkind(c, tb) == TY_VECTOR;
    TypeId vt = va ? ta : tb, el = vec_elem(c, vt), la = ta, lb = tb;
    bool need_int = shift || op == P_PERCENT || op == P_AMP || op == P_PIPE ||
                    op == P_CARET;
    bool ok = true;
    /* & | ^ and shifts check the element type before converting a scalar; %
     * converts first */
    if ((shift || op == P_AMP || op == P_PIPE || op == P_CARET) &&
        !is_int(c, el))
        ok = false;
    else if (va && vb) {
        TypeId e2 = vec_elem(c, tb);
        ok = type_ent(TT, type_canon(TT, ta))->n ==
                 type_ent(TT, type_canon(TT, tb))->n &&
             vec_esize(c, el) == vec_esize(c, e2) &&
             ((is_int(c, el) && is_int(c, e2)) ||
              (is_flt(c, el) && is_flt(c, e2)));
    } else {
        uint32_t sn = va ? b : a;
        TypeId st = unqual(c, rvt(c, sn));
        int r = 0;
        if (!shift || !va)    /* a vector shifted by a scalar converts nothing */
            r = vec_scalar(c, i, sn, st, vt, shift);
        if (r < 0)
            return false;
        if (r > 0) {
            if (va)
                lb = vt;
            else
                la = vt;
        } else if (!(shift && va && int_type(c, st) && is_int(c, el))) {
            if (!shift && !need_int && is_int(c, el) && is_flt(c, st)) {
                /* error() at gcc's input_location: the parser is past the
                 * right operand */
                cerror(c, cdecl_iloc(c, last_tok(c, i) + 1), "cannot convert "
                       "value to a vector");
                set_err(c, i);
                return false;
            }
            ok = false;
        }
    }
    if (ok && need_int && !is_int(c, el))
        ok = false;
    if (!ok) {
        vec_invalid(c, i, a, b, op, la == ta ? 0 : la, lb == tb ? 0 : lb);
        return false;
    }
    c->ty[i] = vt;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    return true;
}

static void e_shift(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool left = op == P_SHL, int_const = true, cnt_ok = false;
    unsigned prec;
    const char *dir = left ? "left" : "right";
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        /* a constant scalar count is checked against the element's width */
        if (vec_binop(c, i, a, b, op, true) && tkind(c, ta) == TY_VECTOR &&
            tkind(c, tb) != TY_VECTOR && has_ival(c, b) &&
            !inhibited(c, i, false)) {
            if (ival_neg(c, tb, c->cv[b]))
                cwarn(c, loc, "shift-count-negative", "%s shift count is "
                      "negative", dir);
            else if (c->cv[b] >= int_bits(c, vec_elem(c, ta)))
                cwarn(c, loc, "shift-count-overflow", "%s shift count >= "
                      "width of vector element", dir);
        }
        return;
    }
    if (!is_int(c, ta) || !is_int(c, tb)) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    c->ty[i] = ta;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    prec = int_bits(c, ta);
    if (has_ival(c, b)) {
        if (ival_neg(c, tb, c->cv[b])) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, "shift-count-negative", "%s shift count is "
                      "negative", dir);
        } else if (c->cv[b] >= prec) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, "shift-count-overflow", "%s shift count >= width "
                      "of type", dir);
        } else {
            cnt_ok = true;
        }
    }
    if (left && is_intcst(c, a) && is_signed(c, ta) &&
        ival_neg(c, ta, c->cv[a])) {
        int_const = false;
        if (!inhibited(c, i, false))
            cwarn(c, loc, "shift-negative-value", "left shift of negative "
                  "value");
    }
    if (!(cnt_ok && has_ival(c, a) && prec <= 64)) {
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
        if (!cnt_ok && !int_const && is_intcst(c, a) && is_intcst(c, b) &&
            (!ival_neg(c, tb, c->cv[b]) || c->cv[a] == 0)) {
            /* (a negative count only for a zero operand, which fold
             * leaves a constant) a count past the width: not an ICE, but a constant to gcc's
             * initializers (a pedwarn there) */
            c->ck[i] = K_FOLD;
            c->cv[i] = 0;
        }
        return;
    }
    if (left && is_intcst(c, a) && is_intcst(c, b) && is_signed(c, ta)) {
        unsigned mp = min_prec_signed(cexpr_trunc(c, ta, c->cv[a])) +
                      (unsigned)c->cv[b];
        /* 1 << 31 only reaches the sign bit; a negative operand has no such
         * exemption */
        bool neg = ival_neg(c, ta, c->cv[a]);
        bool sign = mp == prec + 1 && !neg;
        if (sign)
            int_const = false;
        if (mp > prec + 1 || (mp == prec + 1 && neg) ||
            (sign && diag_option_level(c->diag, "shift-overflow=", 1) >= 2)) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, "shift-overflow=", "result of '%s' requires %u "
                      "bits to represent, but %s only has %u bits", estr(c, i),
                      mp, type_q(TT, ta), prec);
        }
    }
    {
        bool ovf, zdiv;
        uint64_t v = int_op(c, op, ta, cexpr_trunc(c, ta, c->cv[a]), c->cv[b],
                            &ovf, &zdiv);
        bin_value(c, i, a, b, v, false, int_const, loc);
    }
}

/* Is token tok the result of a macro expansion? */
static bool from_macro(Checker *c, uint32_t tok)
{
    const PTok *t = &c->u->toks[tok];
    return t->exp && t->exp != t->t.loc;
}

/* -Wxor-used-as-pow */
static void xor_pow(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
    unsigned long long l, r;
    Diagnostic *d;
    char sug[64];
    if (!(c->ef[a] & EF_DECIMAL) || !(c->ef[b] & EF_DECIMAL) ||
        !has_ival(c, a) || !has_ival(c, b) || ntag(c, sa) != N_NUMBER ||
        ntag(c, sb) != N_NUMBER || from_macro(c, c->nodes[sa].tok) ||
        from_macro(c, c->nodes[sb].tok))
        return;
    l = c->cv[a];
    r = c->cv[b];
    if (l == 2) {
        if (r <= 30)
            snprintf(sug, sizeof sug, "'1 << %llu' (%llu)", r,
                     (unsigned long long)1 << r);
        else if (r <= 62)
            snprintf(sug, sizeof sug, "'1LL << %llu'", r);
        else if (r <= 64)
            snprintf(sug, sizeof sug, "exponentiation");
        else
            return; /* gcc: the RHS is too large to suggest anything */
    } else if (l == 10) {
        snprintf(sug, sizeof sug, "'1e%llu'", r);
    } else {
        return;
    }
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "xor-used-as-pow",
                "result of '%llu^%llu' is %llu; did you mean %s?", l, r, l ^ r,
                sug);
    cnote(c, d, first_loc(c, a), "you can silence this warning by using a "
          "hexadecimal constant (%s rather than %llu)", l == 2 ? "0x2" : "0xa",
          l);
}

/* pointer +/- integer */
static void ptr_int(Checker *c, uint32_t i, uint32_t p, uint32_t n, bool minus)
{
    TypeId pt = rvt(c, p);
    if (!ptr_arith_ok(c, i, cnode_loc(c, i), pt)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = pt;
    c->ef[i] = (c->ef[p] | c->ef[n]) & EF_PROP;
    if (c->ck[p] == K_ADDR && has_ival(c, n)) {
        uint64_t d = c->cv[n] * elem_size(c, pt);
        c->ck[i] = K_ADDR;
        c->cb[i] = c->cb[p];
        c->cv[i] = cexpr_trunc(c, pt, minus ? c->cv[p] - d : c->cv[p] + d);
    }
}

static void ptr_diff(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId ta = rvt(c, a), tb = rvt(c, b);
    TypeId pa = pointee(c, ta), pb = pointee(c, tb);
    SrcLoc loc = cnode_loc(c, i);
    uint64_t sz;
    if (!targets_compat(c, loc, pa, pb)) {
        invalid_operands(c, i, a, b, P_MINUS);
        return;
    }
    if (is_void(c, pa))
        ped_arith(c, i, loc, "pointer of type 'void *' used in subtraction");
    if (is_func(c, pa))
        ped_arith(c, i, loc, "pointer to a function used in subtraction");
    if (!is_void(c, pa) && !is_func(c, pa) && !complete(c, pa)) {
        cerror(c, loc, "arithmetic on pointer to an incomplete type");
        set_err(c, i);
        return;
    }
    sz = elem_size(c, ta);
    if (!is_void(c, pa) && !is_func(c, pa) && sz == 0 && !var_size(c, pa)) {
        cerror(c, loc, "arithmetic on pointer to an empty aggregate");
        set_err(c, i);
        return;
    }
    c->ty[i] = TYPE_MK(c->tgt->ptrdiff_type, 0);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    if (c->ck[a] == K_ADDR && c->ck[b] == K_ADDR && sz > 0 &&
        !var_size(c, pa) && (c->cb[a] == c->cb[b] || same_addr_const(c, a, b))) {
        c->ck[i] = K_FOLD;
        c->cv[i] = c->cb[a] == c->cb[b]
            ? cexpr_trunc(c, c->ty[i], (uint64_t)((int64_t)(c->cv[a] - c->cv[b]) /
                                                  (int64_t)sz))
            : 0;
    }
}

static bool zero_ice(Checker *c, uint32_t n)
{
    return is_intcst(c, n) && c->ck[n] == K_ICE && c->cv[n] == 0;
}

/* fold-const's x * 0, 0 * x, x & 0, 0 & x and x - x (x a plain variable) */
static bool fold_zero_ident(Checker *c, int op, uint32_t a, uint32_t b)
{
    if (((c->ef[a] | c->ef[b]) & EF_PROP) || !is_int(c, rvt(c, a)) ||
        !is_int(c, rvt(c, b)))
        return false;
    if (op == P_STAR || op == P_AMP)
        return zero_ice(c, a) || zero_ice(c, b);
    if (op == P_MINUS) {
        uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
        return sa != NO_NODE && sb != NO_NODE && ntag(c, sa) == N_IDENT &&
               ntag(c, sb) == N_IDENT &&
               cnode_ident(c, sa) == cnode_ident(c, sb) &&
               lookup_ord(c, cnode_ident(c, sa)) != SYM_NONE &&
               !(TYPE_QUALS(rvt(c, a)) & TQ_VOLATILE);
    }
    return false;
}

static void e_arith(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b), rt;
    bool need_int = op == P_PERCENT || op == P_AMP || op == P_PIPE ||
                    op == P_CARET;
    bool zero_div = false;
    long double fa, fb;
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        vec_binop(c, i, a, b, op, false);
        return;
    }
    if (op == P_PLUS) {
        if (is_ptr(c, ta) && is_int(c, tb)) {
            ptr_int(c, i, a, b, false);
            return;
        }
        if (is_int(c, ta) && is_ptr(c, tb)) {
            ptr_int(c, i, b, a, false);
            return;
        }
    } else if (op == P_MINUS) {
        if (is_ptr(c, ta) && is_int(c, tb)) {
            ptr_int(c, i, a, b, true);
            return;
        }
        if (is_ptr(c, ta) && is_ptr(c, tb)) {
            ptr_diff(c, i, a, b);
            return;
        }
    }
    if (need_int ? !(is_int(c, ta) && is_int(c, tb))
                 : !(is_arith(c, ta) && is_arith(c, tb))) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    if (dec_mix(c, i, ta, tb))
        return;
    rt = common_type(c, ta, tb);
    c->ty[i] = rt;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    double_promo(c, i, loc, ta, rt, "to match other operand of binary "
                 "expression");
    double_promo(c, i, loc, tb, rt, "to match other operand of binary "
                 "expression");
    if (!is_complex(c, rt) && is_arith(c, rt)) {
        conv_operand(c, loc, rt, a, false);
        conv_operand(c, loc, rt, b, false);
    }
    if (op == P_CARET)
        xor_pow(c, i, a, b);
    if ((op == P_SLASH || op == P_PERCENT) && is_int(c, tb) &&
        is_intcst(c, b) && c->cv[b] == 0) {
        zero_div = true;
        c->ef[i] |= EF_ZDIV;
        if (!inhibited(c, i, false))
            cwarn(c, loc, "div-by-zero", "division by zero");
    }
    if (is_complex(c, rt)) {
        long double ar, ai, br, bi, rr, ri;
        if ((op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH) &&
            cplx_conv(c, a, rt, &ar, &ai) && cplx_conv(c, b, rt, &br, &bi) &&
            cplx_arith(c, op, cplx_comp(c, rt), ar, ai, br, bi, &rr, &ri))
            cplx_set(c, i, rr, ri);
        return;
    }
    if (is_int(c, rt)) {
        if (!zero_div && int_bits(c, rt) <= 64 && has_ival(c, a) &&
            has_ival(c, b)) {
            bool ovf, zd;
            uint64_t v = int_op(c, op, rt, cexpr_trunc(c, rt, c->cv[a]),
                                cexpr_trunc(c, rt, c->cv[b]), &ovf, &zd);
            bin_value(c, i, a, b, v, ovf, true, loc);
            return;
        }
        /* fold's identities make x * 0, 0 & x and x - x constants (not
         * integer constant expressions) when x has no side effects */
        if (int_bits(c, rt) <= 64 && fold_zero_ident(c, op, a, b)) {
            c->cv[i] = 0;
            c->ck[i] = K_FOLD;
            return;
        }
    } else if (!is_decimal_flt(c, rt) && !zero_div && fval(c, a, &fa) &&
               fval(c, b, &fb) && !((op == P_SLASH) && fb == 0)) {
        long double r = 0;
        switch (op) {
        case P_PLUS: r = fa + fb; break;
        case P_MINUS: r = fa - fb; break;
        case P_STAR: r = fa * fb; break;
        default: r = fa / fb; break;
        }
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, fround(c, rt, r));
        return;
    }
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
}

static void e_binary(Checker *c, uint32_t i)
{
    uint32_t k[2];
    int op = npunct(c, i);
    bool ok;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    if (op == P_COMMA) {
        e_comma(c, i, k[0], k[1]);
        return;
    }
    if (node_err(c, k[0]) || node_err(c, k[1])) {
        set_err(c, i);
        return;
    }
    /* gcc reports the right operand at its operator */
    ok = binop_operand(c, k[0]);
    ok = binop_operand_at(c, k[1], cnode_loc(c, i)) && ok;
    if (!ok) {
        set_err(c, i);
        return;
    }
    if (op == P_SLASH)
        sizeof_div(c, i, k[0], k[1]);
    parens_warn(c, i, k[0], k[1], op);
    if (is_cmp_op(op)) {
        lognot_warn(c, i, k[0], k[1], op);
        tauto_warn(c, i, k[0], k[1], op);
        array_compare(c, i, k[0], k[1], op);
    }
    switch (op) {
    case P_ANDAND: case P_OROR:
        e_logical(c, i, k[0], k[1], op);
        break;
    case P_LT: case P_GT: case P_LE: case P_GE: case P_EQEQ: case P_NE:
        e_compare(c, i, k[0], k[1], op);
        break;
    case P_SHL: case P_SHR:
        e_shift(c, i, k[0], k[1], op);
        break;
    default:
        e_arith(c, i, k[0], k[1], op);
        break;
    }
}

/* ---- ?: -------------------------------------------------------------------------------- */

static SrcLoc colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els)
{
    uint32_t t, from = (mid != NO_NODE ? last_tok(c, mid) : c->nodes[i].tok) + 1,
             to = first_tok(c, els);
    for (t = from; t < to; t++)
        if (tpunct(c, t) == P_COLON)
            return ctok_loc(c, t);
    return cnode_loc(c, i);
}

SrcLoc cexpr_colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els)
{
    return colon_loc(c, i, mid, els);
}

static void e_cond(Checker *c, uint32_t i)
{
    uint32_t k[3], n = nkids(c, i, k, 3), cond, mid, els, ch;
    TypeId t1, t2, rt;
    char nb1[48], nb2[48];
    const char *n1, *n2;
    SrcLoc cl;
    bool ok, allint;
    int tv;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    cond = k[0];
    mid = n == 3 ? k[1] : NO_NODE;
    els = k[n - 1];
    if (node_err(c, cond) || node_err(c, els) ||
        (mid != NO_NODE && node_err(c, mid))) {
        set_err(c, i);
        return;
    }
    cl = colon_loc(c, i, mid, els);
    if (mid == NO_NODE) {
        uint32_t q[3], tc;
        size_t n0 = c->diag->all.len, at = c->dm[cfirst(c, els)];
        ped(c, i, cl, "ISO C forbids omitting the middle term of a '?:' "
                      "expression");
        for (tc = strip_paren(c, cond); ntag(c, tc) == N_BINARY &&
             npunct(c, tc) == P_COMMA && nkids(c, tc, q, 3) == 2;
             tc = strip_paren(c, q[1]))
            ;
        if (is_boolish(c, tc) && !node_err(c, tc))
            cwarn(c, cl, "parentheses", "the omitted middle operand in '?:' "
                  "will always be 'true', suggest explicit middle operand");
        /* gcc's parser reports both before it reads the third operand */
        if (at < n0 && c->diag->all.len > n0) {
            Diagnostic **d = c->diag->all.data, *tmp[2];
            size_t k = c->diag->all.len - n0;
            if (k <= 2) {
                memcpy(tmp, d + n0, k * sizeof *d);
                memmove(d + at + k, d + at, (n0 - at) * sizeof *d);
                memcpy(d + at, tmp, k * sizeof *d);
            }
        }
    }
    ok = binop_operand_at(c, cond, cnode_loc(c, i)) && truth_ok_at(c, cond, cnode_loc(c, i));
    if (ok) {
        no_int_bool = mid == NO_NODE;   /* the condition is also the value */
        cexpr_truth_warn(c, cond, cnode_loc(c, i));
        no_int_bool = false;
    }
    if (mid != NO_NODE && !is_void(c, rvt(c, mid)))
        ok = rvalue_ok(c, mid) && ok;
    if (!is_void(c, rvt(c, els)))
        ok = rvalue_ok(c, els) && ok;
    if (!ok) {
        set_err(c, i);
        return;
    }
    ch = mid != NO_NODE ? mid : cond;
    t1 = rvt(c, ch);
    t2 = rvt(c, els);
    n1 = fnref_builtin(c, ch, &t1, nb1, sizeof nb1);
    n2 = fnref_builtin(c, els, &t2, nb2, sizeof nb2);
    /* operands with side effects are compared later, with the ifs */
    if (mid != NO_NODE && c->func_node != NO_NODE &&
        !((c->ef[mid] | c->ef[els]) & EF_SIDE) &&
        diag_enabled(c->diag, "duplicated-branches") &&
        cstmt_cond_identical(c, i, true))
        cwarn(c, cl, "duplicated-branches", "this condition has identical "
              "branches");
    if (cexpr_cxx_compat(c, i)) {
        TypeId o1 = orig_type(c, ch), o2 = orig_type(c, els);
        if (tkind(c, o1) == TY_ENUM && tkind(c, o2) == TY_ENUM &&
            mainv(c, o1) != mainv(c, o2))
            cwarn(c, cl, "c++-compat", "different enum types in conditional "
                  "is invalid in C++: %s vs %s", type_q(TT, o1),
                  type_q(TT, o2));
    }
    c->ef[i] = (c->ef[cond] | c->ef[els] | (mid != NO_NODE ? c->ef[mid] : 0)) &
               EF_SIDE;
    if (mainv(c, t1) == mainv(c, t2) && !is_arith(c, t1)) {
        rt = unqual(c, t1);
    } else if (is_arith(c, t1) && is_arith(c, t2)) {
        if (dec_mix(c, i, promoted(c, ch), promoted(c, els)))
            return;
        rt = common_type(c, promoted(c, ch), promoted(c, els));
        double_promo(c, i, cl, promoted(c, ch), rt, "to match other result of "
                     "conditional");
        double_promo(c, i, cl, promoted(c, els), rt, "to match other result of "
                     "conditional");
        if (is_int(c, rt) && !is_signed(c, rt) &&
            is_int(c, promoted(c, ch)) && is_int(c, promoted(c, els)) &&
            is_signed(c, promoted(c, ch)) != is_signed(c, promoted(c, els)) &&
            diag_enabled(c->diag, "sign-compare") && !inhibited(c, i, false)) {
            uint32_t sa = is_signed(c, promoted(c, ch)) ? ch : els;
            uint32_t oa = sa == ch ? els : ch;
            if (!nonneg(c, sa))
                cwarn(c, ctok_loc(c, first_tok(c, sa)), "sign-compare",
                      "operand of '?:' changes signedness from %s to %s due "
                      "to unsignedness of other operand",
                      type_q(TT, rvt(c, sa)), type_q(TT, rvt(c, oa)));
        }
        if (!is_complex(c, rt)) {
            conv_operand(c, cl, rt, ch, true);
            conv_operand(c, cl, rt, els, true);
        }
    } else if (is_void(c, t1) || is_void(c, t2)) {
        ped(c, i, cl, "ISO C forbids conditional expr with only one void "
                      "side");
        rt = TYPE_B(VOID);
    } else if (is_ptr(c, t1) && is_ptr(c, t2)) {
        TypeId p1 = pointee(c, t1), p2 = pointee(c, t2);
        unsigned q = gq(c, p1) | gq(c, p2);
        if (targets_compat(c, cl, p1, p2)) {
            rt = type_ptr(TT, type_qual(type_composite(TT, mvt(c, p1),
                                                       mvt(c, p2)), q));
        } else if (is_npc(c, ch)) {
            rt = t2;
        } else if (is_npc(c, els)) {
            rt = t1;
        } else if (is_void(c, p1) || is_void(c, p2)) {
            TypeId vo = is_void(c, p1) ? p1 : p2, ot = vo == p1 ? p2 : p1;
            if (is_array(c, ot) && (gq(c, ot) & ~TYPE_QUALS(vo)))
                cwarn(c, cl, "discarded-array-qualifiers", "pointer to array "
                      "loses qualifier in conditional expression");
            if (is_func(c, p1) || is_func(c, p2))
                ped(c, i, cl, "ISO C forbids conditional expr between 'void *' "
                              "and function pointer");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), tquals(c, p1) | tquals(c, p2)));
        } else {
            if (n1 && n2)
                cpedwarn(c, cl, "incompatible-pointer-types", "pointer type "
                         "mismatch between %s and %s of '%s' and '%s' in "
                         "conditional expression", type_q(TT, t1),
                         type_q(TT, t2), n1, n2);
            else
                cpedwarn(c, cl, "", "pointer type mismatch in conditional "
                                    "expression");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), q));
        }
    } else if (is_ptr(c, t1) && is_int(c, t2)) {
        if (!is_npc(c, els))
            cpedwarn(c, cl, "", "pointer/integer type mismatch in conditional "
                                "expression");
        rt = t1;
    } else if (is_int(c, t1) && is_ptr(c, t2)) {
        if (!is_npc(c, ch))
            cpedwarn(c, cl, "", "pointer/integer type mismatch in conditional "
                                "expression");
        rt = t2;
    } else {
        cerror(c, cl, "type mismatch in conditional expression");
        set_err(c, i);
        return;
    }
    c->ty[i] = rt;
    allint = intops(c, cond) && intops(c, els) &&
             (mid == NO_NODE || intops(c, mid));
    if (allint)
        c->ef[i] |= EF_INTOPS;
    tv = truth(c, cond, true);
    if (tv >= 0 && (is_scalar(c, rt))) {
        uint32_t pick = tv ? ch : els;
        uint32_t keep = c->ef[i];
        if (c->ck[pick] == K_NONE || c->ck[pick] == K_ERR)
            return;
        conv_const(c, i, pick, rt);
        c->ef[i] |= keep;
        if (c->ck[i] == K_ICE || c->ck[i] == K_FOLD) {
            if (allint && c->ck[cond] == K_ICE && is_intcst(c, pick)) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS | (c->ef[pick] & EF_OVERFLOW);
            } else {
                c->ck[i] = K_FOLD;
                c->ef[i] &= ~(EF_CST | EF_NOPCST);
            }
        }
    }
}

/* ---- assignment ------------------------------------------------------------------------- */

int assign_binop(int op)
{
    switch (op) {
    case P_MUL_ASSIGN: return P_STAR;
    case P_DIV_ASSIGN: return P_SLASH;
    case P_MOD_ASSIGN: return P_PERCENT;
    case P_ADD_ASSIGN: return P_PLUS;
    case P_SUB_ASSIGN: return P_MINUS;
    case P_SHL_ASSIGN: return P_SHL;
    case P_SHR_ASSIGN: return P_SHR;
    case P_AND_ASSIGN: return P_AMP;
    case P_XOR_ASSIGN: return P_CARET;
    case P_OR_ASSIGN: return P_PIPE;
    default: return op;
    }
}

static void e_assign(Checker *c, uint32_t i)
{
    uint32_t k[2], l, r;
    int op = npunct(c, i);
    SrcLoc loc = cnode_loc(c, i);
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    l = k[0];
    r = k[1];
    if (node_err(c, l) || node_err(c, r)) {
        set_err(c, i);
        return;
    }
    if (!(c->ef[l] & EF_LVALUE) || is_func(c, c->ty[l])) {
        cerror(c, loc, "lvalue required as left operand of assignment");
        set_err(c, i);
        return;
    }
    if (is_array(c, c->ty[l])) {
        if (!complete(c, c->ty[l]))
            incomplete_error(c, loc, l, c->ty[l]);
        else
            cerror(c, loc, "assignment to expression with array type");
        set_err(c, i);
        return;
    }
    if (op == P_ASSIGN) {
        bool bad = !rvalue_ok(c, r);
        TypeId lt = unqual(c, c->ty[l]);
        if (!is_void(c, lt) && !complete(c, lt)) {
            incomplete_error(c, loc, l, c->ty[l]);
            bad = true;
        }
        if (bad) {
            set_err(c, i);
            return;
        }
    }
    /* _Bool = a = b: the inner assignment is a truth value */
    if (op == P_ASSIGN && tkind(c, unqual(c, c->ty[l])) == TY_BOOL &&
        ntag(c, r) == N_ASSIGN && npunct(c, r) == P_ASSIGN)
        PW(first_loc(c, i), "suggest parentheses around assignment used as "
           "truth value");
    {
        ConvInfo ci;
        TypeId lt = unqual(c, c->ty[l]);
        memset(&ci, 0, sizeof ci);
        ci.context = CONV_ASSIGN;
        ci.loc = loc;
        ci.lhs_bitfield = (c->ef[l] & EF_BITFIELD) != 0;
        ci.lhs_bits = bf_width(c, l);
        if (op != P_ASSIGN) {
            /* a op= b: the operation first, then the conversion of its
             * result */
            int bop = assign_binop(op);
            bool ok = binop_operand(c, l);
            ok = binop_operand(c, r) && ok;
            if (ok) {
                if (bop == P_SHL || bop == P_SHR)
                    e_shift(c, i, l, r, bop);
                else
                    e_arith(c, i, l, r, bop);
                ok = !node_err(c, i);
            }
            if (!ok) {
                /* gcc goes on to the read-only check */
                readonly_check(c, l, loc, 0);
                set_err(c, i);
                return;
            }
            c->ck[i] = K_NONE;
            c->cb[i] = 0;
            c->ef[i] = 0;
            ci.eloc = first_loc(c, r);
        }
        if (readonly_check(c, l, loc, 0)) {
            set_err(c, i);
            return;
        }
        if (is_array(c, c->ty[l])) {
            cerror(c, loc, "assignment to expression with array type");
            set_err(c, i);
            return;
        }
        if (!cexpr_assign_check(c, op != P_ASSIGN ? i : r, lt, &ci)) {
            set_err(c, i);
            return;
        }
    }
    c->ck[i] = K_NONE;
    c->ty[i] = unqual(c, c->ty[l]);
    c->ef[i] = EF_SIDE;
}

/* ---- the dispatcher ------------------------------------------------------------------------- */

bool cexpr_is_expr(unsigned tag)
{
    switch (tag) {
    case N_IDENT: case N_NUMBER: case N_CHAR: case N_STRING: case N_PAREN:
    case N_CALL: case N_INDEX: case N_MEMBER_EXPR: case N_POSTFIX:
    case N_UNARY: case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
    case N_ALIGNOF_TYPE: case N_CAST: case N_COMPOUND_LIT: case N_BINARY:
    case N_ASSIGN: case N_COND: case N_STMT_EXPR: case N_VA_ARG:
    case N_OFFSETOF: case N_TYPES_COMPAT: case N_CONVERTVECTOR: case N_GENERIC:
    case N_GENERIC_ASSOC: case N_ADDR_LABEL: case N_HAS_ATTR:
        return true;
    default:
        return false;
    }
}

/* The overflow warnings gcc gives when it folds the whole expression rooted
 * at i (c_fully_fold). */
static void fold_flush(Checker *c, uint32_t i)
{
    uint32_t k;
    for (k = cfirst(c, i); k <= i && c->fold_pending; k++) {
        TypeId t;
        if (!(c->ef[k] & EF_FOLDWARN))
            continue;
        c->ef[k] &= ~EF_FOLDWARN;
        c->fold_pending--;
        if (inhibited(c, k, true))
            continue;
        t = c->ty[k];
        if (ntag(c, k) == N_UNARY) {
            uint32_t a = first_child(c, k);
            cwarn(c, cnode_loc(c, k), "overflow", "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, a),
                  type_q(TT, t), vstr(c, t, c->cv[k]));
        } else if (ntag(c, k) == N_BINARY) {
            cwarn(c, cnode_loc(c, k), "overflow", "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, k),
                  type_q(TT, t), vstr(c, t, c->cv[k]));
        } else {
            cwarn(c, cnode_loc(c, k), "overflow", "integer overflow in "
                  "expression of type %s results in '%s'", type_q(TT, t),
                  vstr(c, t, c->cv[k]));
        }
    }
}

/* ---- asm statements ------------------------------------------------------------------- */

/* The characters of the string literal node n (adjacent literals joined;
 * simple escapes decoded). */
size_t asm_string(Checker *c, uint32_t n, char *out, size_t cap)
{
    uint32_t np = node_pieces(c, n), t;
    size_t o = 0;
    for (t = 0; t < np; t++) {
        size_t len, j;
        const char *s = ttext(c, c->nodes[n].tok + t, &len);
        for (j = 0; j < len && s[j] != '"'; j++)
            ;
        for (j++; j + 1 < len && o + 1 < cap; j++) {
            char ch = s[j];
            if (ch == '\\' && j + 2 < len) {
                ch = s[++j];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'u': case 'U': {   /* a UCN: its UTF-8 */
                    unsigned digits = ch == 'u' ? 4 : 8, k = 0;
                    uint32_t v = 0;
                    while (k < digits && j + 1 + k < len &&
                           isxdigit((unsigned char)s[j + 1 + k])) {
                        char h = s[j + 1 + k++];
                        v = v << 4 | (uint32_t)(h <= '9' ? h - '0'
                                                : (h | 32) - 'a' + 10);
                    }
                    if (k == digits && v >= 0x80 && v < 0x110000 &&
                        o + 4 < cap) {
                        unsigned nb = v < 0x800 ? 2 : v < 0x10000 ? 3 : 4, b;
                        out[o++] = (char)(nb == 2 ? 0xC0 | v >> 6
                                          : nb == 3 ? 0xE0 | v >> 12
                                                    : 0xF0 | v >> 18);
                        for (b = nb - 1; b-- > 0;)
                            out[o++] = (char)(0x80 | ((v >> (6 * b)) & 0x3F));
                        j += digits;
                        continue;
                    }
                    ch = '\1';         /* not a valid one: left undecoded */
                    break;
                }
                case 'x': {
                    int v = 0;
                    while (j + 2 < len && isxdigit((unsigned char)s[j + 1])) {
                        char h = s[++j];
                        v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                    }
                    ch = (char)v;
                    break;
                }
                default:
                    if (ch >= '0' && ch <= '7') {
                        int v = ch - '0', k = 0;
                        while (k++ < 2 && j + 2 < len && s[j + 1] >= '0' &&
                               s[j + 1] <= '7')
                            v = v * 8 + (s[++j] - '0');
                        ch = (char)v;
                    }
                    break;
                }
            }
            out[o++] = ch;
        }
    }
    out[o] = 0;
    return o;
}

static const char *asm_chr(char ch, char *buf)
{
    if (ch >= 32 && ch < 127)
        snprintf(buf, 8, "%c", ch);
    else
        snprintf(buf, 8, "\\x%02x", (unsigned char)ch);
    return buf;
}

/* parse_output_constraint / parse_input_constraint, as far as they report
 * errors; false after one. */
static bool asm_constraint(Checker *c, SrcLoc loc, const char *k, bool out,
                           bool last, uint32_t nouts, char opn[][64],
                           uint32_t nopn, bool *reg, bool *mem)
{
    const char *p;
    char b[8];
    *reg = *mem = false;
    if (out && k[0] != '=' && k[0] != '+') {
        cerror(c, loc, "output operand constraint lacks '='");
        return false;
    }
    for (p = k; *p; p++) {
        switch (*p) {
        case '=': case '+':
            if (!out) {
                cerror(c, loc, "input operand constraint contains '%c'", *p);
                return false;
            }
            if (p != k) {
                cerror(c, loc, "operand constraint contains incorrectly "
                       "positioned '+' or '='");
                return false;
            }
            break;
        case '&':
            if (!out) {
                cerror(c, loc, "input operand constraint contains '&'");
                return false;
            }
            break;
        case '%':
            if (last) {
                cerror(c, loc, "'%%' constraint used with last operand");
                return false;
            }
            break;
        case '?': case '!': case '*': case '#': case '$': case '^': case ',':
        case '<': case '>':
            break;
        case ' ': case '\t':      /* only an output constraint takes them */
            if (!out) {
                cerror(c, loc, "invalid punctuation '%s' in constraint",
                       asm_chr(*p, b));
                return false;
            }
            break;
        case 'V': case 'm': case 'o':
            *mem = true;
            break;
        case 'g': case 'X':
            *reg = *mem = true;
            break;
        case 'r': case 'p': case 'a': case 'b': case 'c': case 'd': case 'S':
        case 'D': case 'q': case 'Q': case 'R': case 'l': case 'A': case 'f':
        case 't': case 'u': case 'y': case 'x': case 'Y': case 'k': case 'v':
            *reg = true;
            break;
        case 'E': case 'F': case 'G': case 'H': case 's': case 'i': case 'n':
        case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'e': case 'Z': case 'B': case 'C': case 'T': case 'W':
            break;
        case '[': {
            /* resolve_asm_operand_names: [name] is an operand number */
            const char *e = strchr(p, ']');
            char nm[64];
            uint32_t q, idx = nopn;
            if (out) {
                cerror(c, loc, "matching constraint not valid in output "
                       "operand");
                return false;
            }
            if (!e) {
                cerror(c, loc, "missing close brace for named operand");
                cerror(c, loc, "invalid punctuation '[' in constraint");
                return false;
            }
            snprintf(nm, sizeof nm, "%.*s", (int)(e - p - 1), p + 1);
            for (q = 0; q < nopn; q++)
                if (!strcmp(opn[q], nm)) {
                    idx = q;
                    break;
                }
            if (idx == nopn) {
                cerror(c, loc, "undefined named operand '%s'", nm);
                return false;
            }
            if (idx >= nouts) {
                cerror(c, loc, "matching constraint references invalid "
                       "operand number");
                return false;
            }
            *reg = true;
            p = e;
            break;
        }
        default:
            if (*p >= '0' && *p <= '9') {
                if (out) {
                    cerror(c, loc, "matching constraint not valid in output "
                           "operand");
                    return false;
                }
                if (strtoul(p, NULL, 10) >= nouts) {
                    cerror(c, loc, "matching constraint references invalid "
                           "operand number");
                    return false;
                }
                while (p[1] >= '0' && p[1] <= '9')
                    p++;
                *reg = true;
            } else if (!out) {
                if (!isalpha((unsigned char)*p)) {
                    cerror(c, loc, "invalid punctuation '%s' in constraint",
                           asm_chr(*p, b));
                    return false;
                }
                *reg = *mem = true;     /* unknown: treat like "g" */
            } else {
                *reg = *mem = true;     /* an output: any other character */
            }
            break;
        }
    }
    return true;
}

/* An asm operand's expression e (build_asm_expr). */
static void asm_operand(Checker *c, SrcLoc loc, uint32_t e, bool out, bool reg,
                        bool mem)
{
    uint32_t s;
    TypeId t;
    if (node_err(c, e))
        return;
    t = c->ty[e];
    if (out) {
        if (!(c->ef[e] & EF_LVALUE)) {
            cerror(c, loc, "lvalue required in 'asm' statement");
            return;
        }
        if (tquals(c, t) & TQ_CONST) {
            uint32_t v = strip_paren(c, e), ref = SYM_NONE;
            if (ntag(c, v) == N_IDENT)
                ref = lookup_ord(c, cnode_ident(c, v));
            if (ntag(c, v) == N_MEMBER_EXPR && cnode_ident(c, v))
                cerror(c, loc, "read-only member '%s' used as 'asm' output",
                       cident(c, cnode_ident(c, v)));
            else if (ref != SYM_NONE && csym(c, ref)->kind != CS_FUNC)
                cerror(c, loc, csym(c, ref)->flags & CSF_PARAM
                           ? "read-only parameter '%s' use as 'asm' output"
                           : "read-only variable '%s' used as 'asm' output",
                       cident(c, cnode_ident(c, v)));
            else
                cerror(c, loc, "read-only location '%s' used as 'asm' output",
                       estr(c, e));
            return;
        }
    }
    if (reg && (is_void(c, t) || (!out && !is_func(c, t) && tkind(c, t) != TY_ARRAY &&
                                   !type_is_complete(TT, rvt(c, e))))) {
        incomplete_error(c, loc, NO_NODE, rvt(c, e));
        return;
    }
    if (!reg && mem) {          /* c_mark_addressable */
        s = strip_paren(c, e);
        while (ntag(c, s) == N_CAST && s > 0)
            s = strip_paren(c, s - 1);
        if (c->ef[e] & EF_BITFIELD)
            cerror(c, loc, "cannot take address of bit-field '%s'",
                   ntag(c, s) == N_MEMBER_EXPR ? cident(c, cnode_ident(c, s))
                                               : "");
        else if (ntag(c, s) == N_IDENT && (c->ef[s] & EF_REGISTER)) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, s));
            cerror(c, loc, ref != SYM_NONE && !(ref & SYM_LOCAL)
                       ? "address of global register variable '%s' requested"
                       : "address of register variable '%s' requested",
                   cident(c, cnode_ident(c, s)));
        }
    }
}

uint32_t cdep_msg(Checker *c, uint32_t str_node)
{
    char buf[512];
    asm_string(c, str_node, buf, sizeof buf);
    vec_push(&c->dep_msgs, xstrdup(buf));
    return c->dep_msgs.len;
}

void cdep_use(Checker *c, SrcLoc loc, const CSym *s, const SrcLoc *note)
{
    cdep_report(c, loc, s->name, s->flags, s->dep_msg, note);
}

void cdep_report(Checker *c, SrcLoc loc, uint32_t nameid, uint32_t flags,
                 uint32_t dep_msg, const SrcLoc *note)
{
    if (nameid)
        cdep_named(c, loc, cident(c, nameid), flags, dep_msg, note);
}

/* name NULL: an anonymous type ("type is deprecated") */
void cdep_named(Checker *c, SrcLoc loc, const char *name, uint32_t flags,
                uint32_t dep_msg, const SrcLoc *note)
{
    const char *msg = NULL;
    char what[160];
    Diagnostic *d;
    if (!(flags & (CSF_DEPRECATED | CSF_UNAVAILABLE)))
        return;
    if (dep_msg && dep_msg <= c->dep_msgs.len)
        msg = c->dep_msgs.data[dep_msg - 1];
    if (name)
        snprintf(what, sizeof what, "'%s'", name);
    else
        snprintf(what, sizeof what, "type");
    if (flags & CSF_UNAVAILABLE)
        d = msg ? cerror_d(c, loc, "%s is unavailable: %s", what, msg)
                : cerror_d(c, loc, "%s is unavailable", what);
    else
        d = msg ? cwarn_d(c, DL_WARNING, loc, "deprecated-declarations",
                          "%s is deprecated: %s", what, msg)
                : cwarn_d(c, DL_WARNING, loc, "deprecated-declarations",
                          "%s is deprecated", what);
    if (d && note)
        cnote(c, d, *note, "declared here");
}

typedef struct AsmOp {
    uint32_t e;
    bool out, reg, mem;
} AsmOp;

void cexpr_asm(Checker *c, uint32_t i)
{
    uint32_t kids[64], nk = node_children(c->nodes, i, kids, 64), k, j;
    SrcLoc loc = cnode_loc(c, i);
    char tmpl[1024], names[64][64];
    uint32_t nname = 0, nops = 0, tn = NO_NODE, nouts = 0, nins = 0;
    char opn[64][64];                   /* operand names by ordinal */
    bool extended = false, ok = true;
    AsmOp ops[64];
    if (nk > 64)
        return;
    /* count the operands first (the last one may not use '%') */
    for (k = 0; k < nk; k++) {
        if (ntag(c, kids[k]) == N_STRING && tn == NO_NODE)
            tn = kids[k];
        if (ntag(c, kids[k]) == N_ASM_SECTION) {
            uint32_t sec = c->nodes[kids[k]].aux, oc[64];
            uint32_t n2 = node_children(c->nodes, kids[k], oc, 64);
            extended = true;
            for (j = 0; j < n2 && j < 64; j++)
                if (ntag(c, oc[j]) == N_ASM_OPERAND) {
                    uint32_t ch[8], pc, q, ord = nouts + nins;
                    if (ord < 64) {
                        opn[ord][0] = 0;
                        pc = node_children(c->nodes, oc[j], ch, 8);
                        for (q = 0; q < pc && q < 8; q++)
                            if (ntag(c, ch[q]) == N_NAME) {
                                size_t len;
                                const char *s = ttext(c, c->nodes[ch[q]].tok,
                                                      &len);
                                const Tok *nt = cnode_tok(c, ch[q]);
                                if (nt->kind == TK_IDENT)
                                    snprintf(opn[ord], 64, "%s",
                                             ident_by_id(c->in,
                                                         nt->aux)->str);
                                else
                                    snprintf(opn[ord], 64, "%.*s", (int)len, s);
                            }
                    }
                    if (sec == 1)
                        nouts++;
                    else if (sec == 2)
                        nins++;
                }
        }
    }
    for (k = 0; k < nk; k++) {
        uint32_t sec, n2, o, all[64];
        if (ntag(c, kids[k]) != N_ASM_SECTION)
            continue;
        sec = c->nodes[kids[k]].aux;
        n2 = node_children(c->nodes, kids[k], all, 64);
        if (sec > 2) {
            if (sec == 4)
                for (j = 0; j < n2 && j < 64; j++)
                    if (ntag(c, all[j]) == N_NAME && nname < 64) {
                        size_t len;
                        const char *s = ttext(c, c->nodes[all[j]].tok, &len);
                        const Tok *nt = cnode_tok(c, all[j]);
                        if (nt->kind == TK_IDENT)   /* UTF-8, UCNs decoded */
                            snprintf(names[nname++], 64, "%s",
                                     ident_by_id(c->in, nt->aux)->str);
                        else
                            snprintf(names[nname++], 64, "%.*s", (int)len, s);
                    }
            continue;
        }
        for (o = 0; o < n2 && o < 64; o++) {
            uint32_t oper = all[o], pc, st = NO_NODE, ex = NO_NODE;
            uint32_t nm = NO_NODE, ch[8];
            char con[256];
            bool reg, mem;
            if (ntag(c, oper) != N_ASM_OPERAND)
                continue;
            pc = node_children(c->nodes, oper, ch, 8);
            for (j = 0; j < pc && j < 8; j++) {
                if (ntag(c, ch[j]) == N_NAME)
                    nm = ch[j];
                else if (ntag(c, ch[j]) == N_STRING)
                    st = ch[j];
                else
                    ex = ch[j];
            }
            if (nm != NO_NODE && nname < 64) {
                size_t len, q;
                const char *s = ttext(c, c->nodes[nm].tok, &len);
                char nb[64];
                snprintf(nb, sizeof nb, "%.*s", (int)len, s);
                for (q = 0; q < nname; q++)
                    if (!strcmp(names[q], nb)) {
                        cerror(c, loc, "duplicate 'asm' operand name '%s'", nb);
                        ok = false;
                        break;
                    }
                strcpy(names[nname++], nb);
            }
            if (st == NO_NODE || ex == NO_NODE)
                continue;
            asm_string(c, st, con, sizeof con);
            if (asm_constraint(c, loc, con, sec == 1,
                               nops + 1 == nouts + nins, nouts, opn,
                               nouts + nins < 64 ? nouts + nins : 64,
                               &reg, &mem) &&
                nops < 64) {
                ops[nops].e = ex;
                ops[nops].out = sec == 1;
                ops[nops].reg = reg;
                ops[nops].mem = mem;
                nops++;
            }
        }
    }
    for (j = 0; j < nops; j++)
        asm_operand(c, loc, ops[j].e, ops[j].out, ops[j].reg, ops[j].mem);
    if (!extended || tn == NO_NODE || !ok)
        return;
    /* %[name] in the template must name an operand */
    asm_string(c, tn, tmpl, sizeof tmpl);
    {
        const char *p;
        for (p = tmpl; *p; p++) {
            const char *q;
            size_t len, m;
            bool found = false;
            char nb[64];
            if (*p != '%')
                continue;
            q = p + 1;
            if (*q == '%') {
                p++;
                continue;
            }
            while (isalpha((unsigned char)*q) || *q == '=' || *q == '+' ||
                   *q == '-' || *q == '#' || *q == '*' || *q == '&')
                q++;
            if (*q != '[')
                continue;
            q++;
            len = strcspn(q, "]");
            if (!q[len])
                continue;
            snprintf(nb, sizeof nb, "%.*s", (int)len, q);
            for (m = 0; m < nname; m++)
                if (!strcmp(names[m], nb) || strchr(names[m], 92) ||
                    strchr(nb, 1))
                    found = true;
            if (!found) {
                cerror(c, cinput_loc(c, last_tok(c, i) + 1),
                       "undefined named operand '%s'", cident_ucn(nb));
                return;
            }
            p = q + len;
        }
    }
}

/* ---- -Wsequence-point (a port of c-family/c-common.cc verify_tree) -------- */

/* A reference to an object: read when writer is NO_NODE, else written by
 * the node writer (an assignment or an increment). */
typedef struct TL {
    struct TL *next;
    uint32_t expr, writer;
} TL;

typedef struct SaveEnt {
    struct SaveEnt *next;
    uint32_t node;
    TL *before, *after;
} SaveEnt;

typedef struct {
    Checker *c;
    TL **pool;                  /* every allocated entry, freed at the end */
    size_t npool, cappool;
    TL *warned;
    SaveEnt *saves;
    uint32_t *saved;            /* nodes that gcc wraps in a SAVE_EXPR */
    uint32_t skip;
    size_t nsaved, capsaved;
    uint32_t *lnode;            /* locations gcc's fold moved to a comma */
    SrcLoc *lloc;
    size_t nloc, caploc;
} SeqCtx;

static TL *sq_new(SeqCtx *s, TL *next, uint32_t t, uint32_t writer)
{
    TL *l = malloc(sizeof *l);
    if (s->npool == s->cappool) {
        s->cappool = s->cappool ? s->cappool * 2 : 64;
        s->pool = realloc(s->pool, s->cappool * sizeof *s->pool);
    }
    s->pool[s->npool++] = l;
    l->next = next;
    l->expr = t;
    l->writer = writer;
    return l;
}

static bool sq_side(Checker *c, uint32_t x);

static uint32_t sq_strip(Checker *c, uint32_t x)
{
    while (x != NO_NODE && ntag(c, x) == N_PAREN)
        x = first_child(c, x);
    return x;
}

/* Whether the trees of x and y are the same object expression
 * (operand_equal_p): no side effects, same structure. */
static bool sq_eq(Checker *c, uint32_t x, uint32_t y)
{
    x = sq_strip(c, x);
    y = sq_strip(c, y);
    if (x == y)
        return x != NO_NODE;
    return x != NO_NODE && y != NO_NODE && opeq(c, x, y);
}

static bool sq_same(Checker *c, uint32_t x, uint32_t y)
{
    return x == y || (x != NO_NODE && y != NO_NODE && sq_eq(c, x, y));
}

/* warning_candidate_p */
static bool sq_cand(Checker *c, uint32_t x)
{
    switch (ntag(c, x)) {
    case N_IDENT: case N_MEMBER_EXPR: case N_INDEX:
        break;
    case N_UNARY:
        if (npunct(c, x) != P_STAR)
            return false;
        break;
    default:
        return false;
    }
    return (c->ef[x] & EF_LVALUE) && !is_func(c, c->ty[x]) &&
           !is_array(c, c->ty[x]) &&
           !is_void(c, c->ty[x]) && !is_err(c, c->ty[x]);
}

static void sq_add(SeqCtx *s, TL **to, TL *add, uint32_t exclude, bool copy)
{
    while (add) {
        TL *next = add->next;
        if (!copy)
            add->next = *to;
        if (exclude == NO_NODE || !sq_same(s->c, add->writer, exclude))
            *to = copy ? sq_new(s, *to, add->expr, add->writer) : add;
        add = next;
    }
}

static void sq_merge(SeqCtx *s, TL **to, TL *add, bool copy)
{
    TL **end = to;
    while (*end)
        end = &(*end)->next;
    while (add) {
        bool found = false;
        TL *t2, *next = add->next;
        for (t2 = *to; t2; t2 = t2->next)
            if (sq_same(s->c, t2->expr, add->expr)) {
                found = true;
                if (t2->writer == NO_NODE)
                    t2->writer = add->writer;
            }
        if (!found) {
            *end = copy ? sq_new(s, NULL, add->expr, add->writer) : add;
            end = &(*end)->next;
            *end = NULL;
        }
        add = next;
    }
}

static SrcLoc sq_loc(SeqCtx *s, uint32_t n)
{
    for (size_t k = s->nloc; k-- > 0;)
        if (s->lnode[k] == n)
            return s->lloc[k];
    return cnode_loc(s->c, n);
}

static void sq_setloc(SeqCtx *s, uint32_t n, SrcLoc loc)
{
    if (s->nloc == s->caploc) {
        s->caploc = s->caploc ? s->caploc * 2 : 8;
        s->lnode = realloc(s->lnode, s->caploc * sizeof *s->lnode);
        s->lloc = realloc(s->lloc, s->caploc * sizeof *s->lloc);
    }
    s->lnode[s->nloc] = n;
    s->lloc[s->nloc++] = loc;
}

static void sq_collide1(SeqCtx *s, uint32_t written, uint32_t writer, TL *list,
                        bool only_writes)
{
    TL *t;
    for (t = s->warned; t; t = t->next)
        if (sq_same(s->c, t->expr, written))
            return;
    for (; list; list = list->next)
        if (sq_same(s->c, list->expr, written) &&
            !sq_same(s->c, list->writer, writer) &&
            (!only_writes || list->writer != NO_NODE)) {
            s->warned = sq_new(s, s->warned, written, NO_NODE);
            cwarn(s->c, sq_loc(s, writer), "sequence-point",
                  "operation on '%s' may be undefined",
                  estr(s->c, list->expr));
        }
}

static void sq_collide(SeqCtx *s, TL *list)
{
    TL *t;
    for (t = list; t; t = t->next)
        if (t->writer != NO_NODE)
            sq_collide1(s, t->expr, t->writer, list, false);
}

static void sq_verify(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno,
                      uint32_t writer);
static bool sq_truth_wraps(Checker *c, uint32_t e);

/* an operand that gcc truth-converts to `x != 0` first: one more level */
static void sq_verify_t(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno)
{
    if (sq_truth_wraps(s->c, x)) {
        TL *tb = NULL, *tn = NULL;
        sq_verify(s, x, &tb, &tn, NO_NODE);
        sq_merge(s, &tn, tb, false);
        sq_add(s, pno, tn, NO_NODE, false);
    } else {
        sq_verify(s, x, pbefore, pno, NO_NODE);
    }
}

/* verify_tree's default case: the operands of one expression, unordered. */
static void sq_ops(SeqCtx *s, const uint32_t *ops, uint32_t n, TL **pno)
{
    for (uint32_t k = 0; k < n; k++) {
        TL *tb = NULL, *tn = NULL;
        uint32_t o = sq_strip(s->c, ops[k]);
        sq_verify(s, ops[k], &tb, &tn, NO_NODE);
        /* fold hoists a comma operand's left side out: (A, B) + C => A, (B + C) */
        if (!(o != NO_NODE && ntag(s->c, o) == N_BINARY &&
              npunct(s->c, o) == P_COMMA))
            sq_merge(s, &tn, tb, false);
        sq_add(s, pno, tn, NO_NODE, false);
    }
}

static bool sq_saved(SeqCtx *s, uint32_t x)
{
    for (size_t k = 0; k < s->nsaved; k++)
        if (s->saved[k] == x)
            return true;
    return false;
}

static void sq_mark_saved(SeqCtx *s, uint32_t x)
{
    if (s->nsaved == s->capsaved) {
        s->capsaved = s->capsaved ? s->capsaved * 2 : 16;
        s->saved = realloc(s->saved, s->capsaved * sizeof *s->saved);
    }
    s->saved[s->nsaved++] = x;
}

/* Whether a subtree has side effects (TREE_SIDE_EFFECTS). */
static bool sq_side(Checker *c, uint32_t x)
{
    uint32_t k, f = cfirst(c, x);
    for (k = f; k <= x; k++) {
        unsigned t = ntag(c, k);
        if (t == N_CALL || t == N_ASSIGN || t == N_POSTFIX || t == N_STMT_EXPR ||
            (t == N_UNARY && (npunct(c, k) == P_INC || npunct(c, k) == P_DEC)))
            return true;
    }
    return false;
}

/* stabilize_reference: the side-effecting leaves of an lvalue become
 * SAVE_EXPRs, evaluated once although the tree mentions them twice. */
static void sq_stab1(SeqCtx *s, uint32_t x)
{
    Checker *c = s->c;
    uint32_t k[3], n;
    x = sq_strip(c, x);
    if (x == NO_NODE || !sq_side(c, x))
        return;
    switch (ntag(c, x)) {
    case N_BINARY:
        if (npunct(c, x) == P_COMMA || npunct(c, x) == P_ANDAND ||
            npunct(c, x) == P_OROR)
            break;
        n = nkids(c, x, k, 3);
        for (uint32_t j = 0; j < n; j++)
            sq_stab1(s, k[j]);
        return;
    case N_CAST:
        n = nkids(c, x, k, 3);
        if (n >= 2)
            sq_stab1(s, k[1]);
        return;
    case N_UNARY:
        if (npunct(c, x) != P_INC && npunct(c, x) != P_DEC &&
            npunct(c, x) != P_STAR && npunct(c, x) != P_AMP) {
            n = nkids(c, x, k, 3);
            if (n)
                sq_stab1(s, k[0]);
            return;
        }
        break;
    default:
        break;
    }
    sq_mark_saved(s, x);
}

static void sq_stab(SeqCtx *s, uint32_t x)
{
    Checker *c = s->c;
    uint32_t k[3], n;
    x = sq_strip(c, x);
    if (x == NO_NODE)
        return;
    n = nkids(c, x, k, 3);
    switch (ntag(c, x)) {
    case N_UNARY:
        if (npunct(c, x) == P_STAR && n)
            sq_stab1(s, k[0]);
        break;
    case N_INDEX:
        if (n >= 2) {
            if (is_array(c, c->ty[strip_paren(c, k[0])])) {
                sq_stab(s, k[0]);
                sq_stab1(s, k[1]);
            } else {
                sq_stab1(s, k[0]);
                sq_stab1(s, k[1]);
            }
        }
        break;
    case N_MEMBER_EXPR:
        if (n) {
            if (c->nodes[x].flags & NF_ARROW)
                sq_stab1(s, k[0]);
            else
                sq_stab(s, k[0]);
        }
        break;
    default:
        break;
    }
}

static void sq_verify(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno,
                      uint32_t writer)
{
    Checker *c = s->c;
    TL *tmp_before, *tmp_nosp, *tmp_list2, *tmp_list3;
    uint32_t k[64], n;
restart:
    x = sq_strip(c, x);
    if (x == NO_NODE || node_err(c, x))
        return;
    if (sq_saved(s, x) && s->skip != x) {
        SaveEnt *t;
        for (t = s->saves; t; t = t->next)
            if (t->node == x)
                break;
        if (!t) {
            t = malloc(sizeof *t);
            t->next = s->saves;
            s->saves = t;
            t->node = x;
            tmp_before = tmp_nosp = tmp_list3 = NULL;
            s->skip = x;
            sq_verify(s, x, &tmp_before, &tmp_nosp, NO_NODE);
            sq_collide(s, tmp_nosp);
            sq_merge(s, &tmp_list3, tmp_nosp, false);
            t->before = tmp_before;
            t->after = tmp_list3;
        }
        sq_merge(s, pbefore, t->before, true);
        sq_add(s, pno, t->after, NO_NODE, true);
        return;
    }
    s->skip = NO_NODE;
    if (sq_cand(c, x))
        *pno = sq_new(s, *pno, x, writer);
    n = nkids(c, x, k, 64);
    switch (ntag(c, x)) {
    case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
    case N_ALIGNOF_TYPE: case N_COMPOUND_LIT: case N_STMT_EXPR: case N_GENERIC:
        return;
    case N_BINARY:
        if (n >= 2 && npunct(c, x) == P_COMMA && !sq_side(c, k[0]) &&
            !(c->ef[k[0]] & EF_SIDE)) {
            /* fold drops a side-effect-free left operand and gives the right
             * one the comma's location */
            uint32_t r = sq_strip(c, k[1]);
            sq_setloc(s, r, cnode_loc(c, x));
            x = r;
            goto restart;
        }
        if (n >= 2 && (npunct(c, x) == P_ANDAND || npunct(c, x) == P_OROR)) {
            /* fold: a constant left operand picks the result; a constant
             * deciding right operand swallows a side-effect-free left one */
            bool orr = npunct(c, x) == P_OROR;
            uint32_t l = sq_strip(c, k[0]), r = sq_strip(c, k[1]);
            if (c->ck[l] == K_ICE) {
                if ((c->cv[l] != 0) == orr)
                    return;
                x = r;
                goto restart;
            }
            if (c->ck[r] == K_ICE && (c->cv[r] != 0) == orr &&
                !sq_side(c, l) && !(c->ef[l] & EF_SIDE))
                return;
        }
        if (n >= 2 && (npunct(c, x) == P_COMMA || npunct(c, x) == P_ANDAND ||
                       npunct(c, x) == P_OROR)) {
            tmp_before = tmp_nosp = tmp_list2 = tmp_list3 = NULL;
            bool tr = npunct(c, x) != P_COMMA;
            if (tr)
                sq_verify_t(s, k[0], &tmp_before, &tmp_nosp);
            else
                sq_verify(s, k[0], &tmp_before, &tmp_nosp, NO_NODE);
            sq_collide(s, tmp_nosp);
            sq_merge(s, pbefore, tmp_before, false);
            sq_merge(s, pbefore, tmp_nosp, false);
            if (tr)
                sq_verify_t(s, k[1], &tmp_list3, &tmp_list2);
            else
                sq_verify(s, k[1], &tmp_list3, &tmp_list2, NO_NODE);
            sq_collide(s, tmp_list2);
            sq_merge(s, pbefore, tmp_list3, false);
            sq_merge(s, pno, tmp_list2, false);
            return;
        }
        if (n == 2 && sq_eq(c, sq_strip(c, k[0]), sq_strip(c, k[1])) &&
            !sq_side(c, k[0]) && !(c->ef[k[0]] & EF_SIDE) &&
            is_int(c, c->ty[k[0]])) {
            switch (npunct(c, x)) {     /* fold: x op x is a constant */
            case P_MINUS: case P_CARET: case P_LT: case P_GT: case P_LE:
            case P_GE: case P_EQEQ: case P_NE: case P_SLASH: case P_PERCENT:
                return;
            default:
                break;
            }
        }
        sq_ops(s, k, n, pno);
        return;
    case N_COND: {
        uint32_t cond = k[0], a1 = n == 3 ? k[1] : k[0], a2 = k[n - 1];
        if (n < 2)
            return;
        if (c->ck[sq_strip(c, cond)] == K_ICE) {    /* folded to one arm */
            uint32_t arm = c->cv[sq_strip(c, cond)] ? a1 : a2;
            x = arm;
            goto restart;
        }
        tmp_before = tmp_list2 = NULL;
        sq_verify_t(s, cond, &tmp_before, &tmp_list2);
        sq_collide(s, tmp_list2);
        sq_merge(s, pbefore, tmp_before, false);
        sq_merge(s, pbefore, tmp_list2, false);

        tmp_list3 = tmp_nosp = NULL;
        sq_verify(s, a1, &tmp_list3, &tmp_nosp, NO_NODE);
        sq_collide(s, tmp_nosp);
        sq_merge(s, pbefore, tmp_list3, false);

        tmp_list3 = tmp_list2 = NULL;
        sq_verify(s, a2, &tmp_list3, &tmp_list2, NO_NODE);
        sq_collide(s, tmp_list2);
        sq_merge(s, pbefore, tmp_list3, false);
        sq_merge(s, &tmp_nosp, tmp_list2, false);
        sq_add(s, pno, tmp_nosp, NO_NODE, false);
        return;
    }
    case N_POSTFIX:
        if (n >= 1)
            sq_verify(s, k[0], pno, pno, x);
        return;
    case N_UNARY:
        switch (npunct(c, x)) {
        case P_INC: case P_DEC:
            if (n >= 1) {
                sq_stab(s, k[0]);
                sq_verify(s, k[0], pno, pno, x);
            }
            return;
        case P_AMP:
            if (n >= 1) {
                uint32_t o = sq_strip(c, k[0]);
                if (o != NO_NODE && ntag(c, o) == N_IDENT)
                    return;
                x = o;
                writer = NO_NODE;
                goto restart;
            }
            return;
        case P_STAR:
            sq_ops(s, k, n, pno);
            return;
        default:
            if (n >= 1) {
                x = k[0];
                writer = NO_NODE;
                goto restart;
            }
            return;
        }
    case N_CAST:
        if (n >= 2) {
            x = k[1];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_VA_ARG:
        if (n >= 1) {
            x = k[0];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_MEMBER_EXPR:
        if (n >= 1) {
            x = k[0];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_INDEX:
        sq_ops(s, k, n < 2 ? n : 2, pno);
        return;
    case N_ASSIGN: {
        int op = npunct(c, x);
        if (n < 2)
            return;
        tmp_before = tmp_nosp = tmp_list3 = NULL;
        if (op != P_ASSIGN)
            sq_stab(s, k[0]);
        if (op == P_ASSIGN) {
            sq_verify(s, k[1], &tmp_before, &tmp_nosp, NO_NODE);
        } else {
            /* lhs = lhs op rhs: the right side is a binary expression */
            uint32_t two[2];
            two[0] = k[0];
            two[1] = k[1];
            sq_ops(s, two, 2, &tmp_nosp);
        }
        sq_verify(s, k[0], &tmp_list3, &tmp_list3, x);
        sq_add(s, &tmp_before, tmp_list3, x, true);
        sq_collide(s, tmp_before);
        sq_add(s, pno, tmp_list3, x, false);
        sq_collide1(s, k[0], x, tmp_nosp, true);

        sq_merge(s, pbefore, tmp_before, false);
        if (sq_cand(c, sq_strip(c, k[0])))
            sq_merge(s, &tmp_nosp, sq_new(s, NULL, sq_strip(c, k[0]), x), false);
        sq_add(s, pno, tmp_nosp, NO_NODE, true);
        return;
    }
    case N_CALL: {
        uint32_t j;
        tmp_before = tmp_nosp = NULL;
        if (n < 1)
            return;
        sq_verify(s, k[0], &tmp_before, &tmp_nosp, NO_NODE);
        for (j = 1; j < n; j++) {
            tmp_list2 = tmp_list3 = NULL;
            sq_verify(s, k[j], &tmp_list2, &tmp_list3, NO_NODE);
            sq_merge(s, &tmp_list3, tmp_list2, false);
            sq_add(s, &tmp_before, tmp_list3, NO_NODE, false);
        }
        sq_add(s, &tmp_before, tmp_nosp, NO_NODE, false);
        sq_collide(s, tmp_before);
        sq_add(s, pbefore, tmp_before, NO_NODE, false);
        return;
    }
    default:
        return;
    }
}

/* A full expression: gcc's verify_sequence_points. */
static bool sq_for_cond(Checker *c, uint32_t p, uint32_t i)
{
    uint32_t kids[32], cnt = node_children(c->nodes, p, kids, 32);
    return cnt > 2 && kids[2] == i;
}

static bool sq_truth_wraps(Checker *c, uint32_t e)
{
    e = sq_strip(c, e);
    switch (ntag(c, e)) {
    case N_COND:
        return false;
    case N_UNARY:
        return npunct(c, e) != P_BANG;
    case N_BINARY:
        switch (npunct(c, e)) {
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR: case P_COMMA:
            return false;
        case P_MINUS: case P_CARET:     /* fold turns `a - b != 0` into `a != b` */
            return !is_int(c, c->ty[e]);
        default:
            return true;
        }
    default:
        return true;
    }
}

static void sq_check(Checker *c, uint32_t e, bool cond)
{
    SeqCtx s;
    TL *before = NULL, *after = NULL;
    if (!diag_enabled(c->diag, "sequence-point") || node_err(c, e) ||
        e - cfirst(c, e) > 400)
        return;
    memset(&s, 0, sizeof s);
    s.c = c;
    s.skip = NO_NODE;
    if (cond && sq_truth_wraps(c, e)) {
        /* gcc checks `e != 0`: one more level of operand merging */
        sq_ops(&s, &e, 1, &after);
    } else {
        sq_verify(&s, e, &before, &after, NO_NODE);
    }
    sq_collide(&s, after);
    for (size_t k = 0; k < s.npool; k++)
        free(s.pool[k]);
    free(s.pool);
    free(s.saved);
    free(s.lnode);
    free(s.lloc);
    while (s.saves) {
        SaveEnt *nx = s.saves->next;
        free(s.saves);
        s.saves = nx;
    }
}

/* gcc -O: c_fully_fold replaces a read of a const, non-volatile integer
 * variable that has a constant initializer by that value (decl_constant_
 * value), inside a function or an initializer.  Done where the identifier is an operand;
 * &a, a++, a = .. and the like keep the variable. */
static void fold_const_var(Checker *c, uint32_t i)
{
    uint32_t t = i, p = c->par[i], ref;
    const CSym *s;
    if (!in_function(c)) {
        /* file scope: only an initializer folds (gcc, in_init), not an array
         * bound or a declarator */
        uint32_t r = i;
        while (c->par[r] != NO_NODE && cexpr_is_expr(ntag(c, c->par[r])))
            r = c->par[r];
        if (c->par[r] == NO_NODE || ntag(c, c->par[r]) != N_INIT_DECL ||
            r != c->par[r] - 1)
            return;
    }
    while (p != NO_NODE && ntag(c, p) == N_PAREN) {
        t = p;
        p = c->par[t];
    }
    if (p == NO_NODE)
        return;
    switch (ntag(c, p)) {
    case N_BINARY: case N_COND: case N_CAST: case N_INIT_DECL:
        break;
    case N_UNARY:
        if (npunct(c, p) != P_PLUS && npunct(c, p) != P_MINUS &&
            npunct(c, p) != P_TILDE && npunct(c, p) != P_BANG)
            return;
        break;
    case N_ASSIGN:
        if (t != p - 1)
            return;
        break;
    default:
        return;
    }
    ref = lookup_ord(c, cnode_ident(c, i));
    if (ref == SYM_NONE)
        return;
    s = csym(c, ref);
    if (s->kind != CS_OBJ || !(s->flags & CSF_CONST_VAL) ||
        !is_int(c, c->ty[i]) || (tquals(c, s->ty) & TQ_VOLATILE))
        return;
    c->ck[i] = K_FOLD;
    c->cv[i] = s->val;
    c->cb[i] = 0;
    c->ef[i] = EF_CST;
}

void cexpr_node(Checker *c, uint32_t i)
{
    uint32_t p;
    switch (ntag(c, i)) {
    case N_IDENT:
        e_ident(c, i);
        if (c->opt.opt_level && c->opt.opt_level != '0' && c->ck[i] != K_ERR &&
            !(c->ef[i] & EF_SIDE))
            fold_const_var(c, i);
        break;
    case N_NUMBER: e_number(c, i); break;
    case N_CHAR: e_char(c, i); break;
    case N_STRING: e_string(c, i); break;
    case N_PAREN: e_paren(c, i); break;
    case N_CALL: e_call(c, i); break;
    case N_INDEX: {
        /* a subscript cut short by a syntax error is checked after it */
        DiagOrd o0 = diag_ord(c->diag, c->nodes[i].flags & NF_CUT ? ORD_CUT
                                                                  : ORD_NORMAL);
        e_index(c, i);
        diag_ord(c->diag, o0);
        break;
    }
    case N_MEMBER_EXPR: e_member(c, i); break;
    case N_POSTFIX: e_postfix(c, i); break;
    case N_UNARY: e_unary(c, i); break;
    case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: e_sizeof(c, i, false); break;
    case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE: e_sizeof(c, i, true); break;
    case N_CAST: e_cast(c, i); break;
    case N_COMPOUND_LIT: e_complit(c, i); break;
    case N_BINARY: e_binary(c, i); break;
    case N_ASSIGN: e_assign(c, i); break;
    case N_COND: e_cond(c, i); break;
    case N_STMT_EXPR: e_stmt_expr(c, i); break;
    case N_VA_ARG: e_va_arg(c, i); break;
    case N_OFFSETOF: e_offsetof(c, i); break;
    case N_TYPES_COMPAT: e_types_compat(c, i); break;
    case N_HAS_ATTR: e_has_attr(c, i); break;
    case N_CONVERTVECTOR: e_convertvector(c, i); break;
    case N_GENERIC: e_generic(c, i); break;
    case N_GENERIC_ASSOC: e_generic_assoc(c, i); break;
    case N_ADDR_LABEL: e_addr_label(c, i); break;
    default: set_err(c, i); break;
    }
    if (ntag(c, i) == N_MEMBER_EXPR || ntag(c, i) == N_INDEX)
        sso_decay(c, i);
    p = c->par[i];
    if (p != NO_NODE && !cexpr_is_expr(ntag(c, p))) {
        switch (ntag(c, p)) {
        case N_EXPR_STMT: case N_IF: case N_WHILE: case N_DO: case N_FOR:
        case N_RETURN: case N_SWITCH: case N_INIT_DECL: case N_INIT_LIST:
            sq_check(c, i, ntag(c, p) == N_IF || ntag(c, p) == N_WHILE ||
                           ntag(c, p) == N_DO ||
                           (ntag(c, p) == N_FOR && sq_for_cond(c, p, i)));
            break;
        default:
            break;
        }
    }
    /* a null pointer constant: an integer constant expression with value 0 */
    if (is_intcst(c, i) && !(c->ef[i] & EF_OVERFLOW) && c->cv[i] == 0 &&
        is_int(c, c->ty[i]) && !constvar_in(c, i) &&
        !(ntag(c, strip_paren(c, i)) == N_BINARY &&
          npunct(c, strip_paren(c, i)) == P_COMMA))
        c->ef[i] |= EF_NPC;
    p = c->par[i];
    if (c->fold_pending && (p == NO_NODE || !cexpr_is_expr(ntag(c, p))))
        fold_flush(c, i);
}
