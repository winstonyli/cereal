/* tast_x86.c - the x86 intrinsics (cbuiltin_x86.h) as operations on lanes.  Split from tast.c. */
#include "c/tast.h"

#include <ctype.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/cgx86.h"
#include "c/cintrin.h"
#include "c/csymidx.h"
#include "c/lit.h"

#include "c/tast_int.h"



/* ---- x86 built-ins ----------------------------------------------------------
 * The calls the intrinsic headers make (cbuiltin_x86.h), written as the operations on the lanes
 * of their vector arguments.  A tree is used once, so every use of a lane or of an intermediate
 * value builds it again; an intermediate that is used more than once lives in a temporary. */
typedef struct Ia {
    const IaBt *r;
    VOp a, b;                   /* the vector arguments, read as lanes of the row's type */
    TypeId rl;                  /* the lane type of the result, and ... */
    uint32_t n;                 /* ... its number of lanes */
    uint32_t asz;               /* the size in bytes of the first vector argument (0: 16) */
    uint32_t cnt;               /* the shift count's temporary (an unsigned long), or UINT32_MAX */
    uint32_t set;               /* the scalar that replaces a lane */
    uint32_t t;                 /* a temporary of type long for the lane's intermediate value */
    int64_t imm;                /* the constant argument */
    SrcLoc loc;
} Ia;

TypeId ia_ty(int l)
{
    static const TypeKind k[] = {TY_SCHAR, TY_UCHAR, TY_SHORT, TY_USHORT, TY_INT, TY_UINT, TY_LONG, TY_ULONG,
                                 TY_FLOAT, TY_DOUBLE};
    return TYPE_MK(k[l], 0);
}

static unsigned ia_bits(int l)
{
    return l == IAL_F32 ? 32 : l == IAL_F64 ? 64 : 8u << (l >> 1);
}

/* The integer lane type with the bits of lane type l. */
static TypeId ia_bitty(int l)
{
    return ia_ty(l == IAL_F32 ? IAL_S32 : l == IAL_F64 ? IAL_S64 : l);
}

/* Kinds that move or combine bits: the result is made of lanes of the integer type of the row's
 * lanes and then seen as the return type. */
static bool ia_bitspace(int kind)
{
    return kind == IAK_ANDN || kind == IAK_BITOP || kind == IAK_FCMP || kind == IAK_UNPACK || kind == IAK_SHUF ||
           kind == IAK_SHUFP || kind == IAK_MOVLOW || kind == IAK_MOVHL || kind == IAK_BSHIFT || kind == IAK_MOVQ;
}

uint32_t ia_bin(El *e, int p, TypeId t, uint32_t a, uint32_t b, SrcLoc loc)
{
    uint32_t ka[2];
    ka[0] = a;
    ka[1] = b;
    return a == TN || b == TN ? TN : mk(e, TO_BINOP, t, loc, 0, 0, (unsigned)p, ka, 2);
}

static uint32_t ia_un(El *e, int p, TypeId t, uint32_t a, SrcLoc loc)
{
    return a == TN ? TN : mk(e, TO_UNOP, t, loc, 0, 0, (unsigned)p, &a, 1);
}

/* -(a p b) in type t: all ones when the comparison holds, else 0 */
static uint32_t ia_mask(El *e, int p, TypeId t, uint32_t a, uint32_t b, SrcLoc loc)
{
    uint32_t ka[2], m;
    ka[0] = a;
    ka[1] = b;
    if (a == TN || b == TN)
        return TN;
    m = conv(e, mk(e, TO_CMP, TYPE_B(INT), loc, 0, 0, (unsigned)p, ka, 2), t, loc);
    return ia_un(e, P_MINUS, t, m, loc);
}

/* Lane k of argument 0 (a) or 1 (b), converted to type to. */
static uint32_t ia_rd(El *e, const Ia *x, int which, uint32_t k, TypeId to)
{
    return conv(e, vop_lane(e, which ? &x->b : &x->a, k, x->loc), to, x->loc);
}

static uint32_t ia_tmp(El *e, uint32_t t, TypeId ty, SrcLoc loc)
{
    return mk(e, TO_TMP, ty, loc, t, 0, 0, NULL, 0);
}

/* The statement `tmp t = v`, appended to st. */
static void ia_set(El *e, U32Vec *st, uint32_t t, TypeId ty, uint32_t v, SrcLoc loc)
{
    uint32_t ka[2], s;
    ka[0] = ia_tmp(e, t, ty, loc);
    ka[1] = v;
    s = v == TN ? TN : mk(e, TO_STORE, ty, loc, 0, 0, 0, ka, 2);
    if (s != TN)
        vec_push(st, mk(e, TO_EXPRSTMT, 0, loc, 0, 0, 0, &s, 1));
}

/* Temporary t, clamped to lo .. hi, as statements in st: t = t ^ ((t ^ b) & -(t cmp b)). */
static void ia_clamp(El *e, U32Vec *st, uint32_t t, TypeId ty, int64_t lo, int64_t hi, SrcLoc loc)
{
    int k;
    for (k = 0; k < 2; k++) {
        int64_t b = k ? lo : hi;
        uint32_t x = ia_bin(e, P_CARET, ty, load(e, ia_tmp(e, t, ty, loc)), cnst(e, ty, (uint64_t)b, loc), loc);
        x = ia_bin(e, P_AMP, ty, x,
                   ia_mask(e, k ? P_LT : P_GT, ty, load(e, ia_tmp(e, t, ty, loc)), cnst(e, ty, (uint64_t)b, loc), loc), loc);
        ia_set(e, st, t, ty, ia_bin(e, P_CARET, ty, load(e, ia_tmp(e, t, ty, loc)), x, loc), loc);
    }
}

/* a p b as an int. */
uint32_t ia_cmp(El *e, int p, uint32_t a, uint32_t b, SrcLoc loc)
{
    uint32_t ka[2];
    ka[0] = a;
    ka[1] = b;
    return a == TN || b == TN ? TN : mk(e, TO_CMP, TYPE_B(INT), loc, 0, 0, (unsigned)p, ka, 2);
}

/* Float v as an int the way cvttss2si (round: cvtss2si) leaves it: 0x80000000 when it does not fit.
 * (A C cast is undefined there.)  Other targets are plain conversions. */
static uint32_t ia_f2i(El *e, Ia *x, uint32_t v, TypeId to, bool round, SrcLoc loc)
{
    TypeId W = ia_ty(IAL_S64), I = ia_ty(IAL_S32);
    uint32_t r;
    if (!round && (to != I || x->r->lane < IAL_F32))
        return conv(e, v, to, loc);
    r = v == TN ? TN : mk(e, TO_F2I, to == I ? I : W, loc, 0, 0, round, &v, 1);
    return r == TN ? TN : conv(e, r, to, loc);
}

/* Lane k of argument 0 or 1 as bits (the integer lane type of the row's). */
static uint32_t ia_rdbits(El *e, const Ia *x, int which, uint32_t k)
{
    VOp o = which ? x->b : x->a;
    o.et = ia_bitty(x->r->lane);
    return vop_lane(e, &o, k, x->loc);
}

/* The comparison predicate pred of cmpps and cmppd on lane j of a and b, an int: 0 eq, 1 lt, 2 le,
 * 3 unordered, 4 neq, 5 not lt, 6 not le, 7 ordered, 8 gt, 9 ge, 10 not gt, 11 not ge. */
static uint32_t ia_pred(El *e, const Ia *x, uint32_t j, int pred)
{
    static const int op[] = {P_EQEQ, P_LT, P_LE, 0, P_NE, P_LT, P_LE, 0, P_GT, P_GE, P_GT, P_GE};
    TypeId L = ia_ty(x->r->lane);
    SrcLoc loc = x->loc;
    TypeId it = TYPE_B(INT);
    if (pred == 3 || pred == 7) {       /* unordered: a != a | b != b; ordered: a == a & b == b */
        int p = pred == 3 ? P_NE : P_EQEQ;
        return ia_bin(e, pred == 3 ? P_PIPE : P_AMP, it, ia_cmp(e, p, ia_rd(e, x, 0, j, L), ia_rd(e, x, 0, j, L), loc),
                      ia_cmp(e, p, ia_rd(e, x, 1, j, L), ia_rd(e, x, 1, j, L), loc), loc);
    }
    {
        uint32_t c = ia_cmp(e, op[pred], ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc);
        return pred == 5 || pred == 6 || pred == 10 || pred == 11 ? ia_cmp(e, P_EQEQ, c, cnst(e, it, 0, loc), loc) : c;
    }
}

/* Lane k of a - b as a long. */
static uint32_t ia_diff(El *e, const Ia *x, uint32_t k)
{
    TypeId w = TYPE_MK(TY_LONG, 0);
    return ia_bin(e, P_MINUS, w, ia_rd(e, x, 0, k, w), ia_rd(e, x, 1, k, w), x->loc);
}

/* Lane j of the result of the row in x. */
static uint32_t ia_lane(El *e, Ia *x, uint32_t j)
{
    const IaBt *r = x->r;
    SrcLoc loc = x->loc;
    TypeId L = ia_ty(r->lane), W = TYPE_MK(TY_LONG, 0), UW = TYPE_MK(TY_ULONG, 0);
    unsigned bits = ia_bits(r->lane);
    uint32_t na = (x->asz ? x->asz : 16) / (bits / 8), s;
    int arg = r->arg;
    U32Vec st = {0};
    switch (r->kind) {
    case IAK_ANDN:                      /* ~a & b */
        return ia_bin(e, P_AMP, L, ia_un(e, P_TILDE, L, ia_rd(e, x, 0, j, L), loc), ia_rd(e, x, 1, j, L), loc);
    case IAK_MINMAX: {                  /* b ^ ((a ^ b) & -(a cmp b)) */
        uint32_t m = ia_mask(e, arg ? P_GT : P_LT, L, ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc);
        uint32_t d = ia_bin(e, P_CARET, L, ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc);
        return ia_bin(e, P_CARET, L, ia_rd(e, x, 1, j, L), ia_bin(e, P_AMP, L, d, m, loc), loc);
    }
    case IAK_SAT: {                     /* a +- b, clamped to the lane type's range */
        bool sg = !(r->lane & 1);
        int64_t hi = sg ? (int64_t)((1ull << (bits - 1)) - 1) : (int64_t)((1ull << bits) - 1),
                lo = sg ? -hi - 1 : 0;
        ia_set(e, &st, x->t, W, ia_bin(e, arg ? P_MINUS : P_PLUS, W, ia_rd(e, x, 0, j, W), ia_rd(e, x, 1, j, W), loc), loc);
        ia_clamp(e, &st, x->t, W, lo, hi, loc);
        return vec_wrap(e, conv(e, load(e, ia_tmp(e, x->t, W, loc)), L, loc), L, &st, loc);
    }
    case IAK_AVG:                       /* (a + b + 1) >> 1 */
        s = ia_bin(e, P_PLUS, W, ia_bin(e, P_PLUS, W, ia_rd(e, x, 0, j, W), ia_rd(e, x, 1, j, W), loc), cnst(e, W, 1, loc), loc);
        return conv(e, ia_bin(e, P_SHR, W, s, cnst(e, W, 1, loc), loc), L, loc);
    case IAK_MULH:                      /* the high half of the product */
        s = ia_bin(e, P_STAR, W, ia_rd(e, x, 0, j, W), ia_rd(e, x, 1, j, W), loc);
        return conv(e, ia_bin(e, P_SHR, W, s, cnst(e, W, 16, loc), loc), L, loc);
    case IAK_MADD:                      /* pairs of products, added */
        s = ia_bin(e, P_STAR, W, ia_rd(e, x, 0, 2 * j, W), ia_rd(e, x, 1, 2 * j, W), loc);
        s = ia_bin(e, P_PLUS, W, s, ia_bin(e, P_STAR, W, ia_rd(e, x, 0, 2 * j + 1, W), ia_rd(e, x, 1, 2 * j + 1, W), loc), loc);
        return conv(e, s, x->rl, loc);
    case IAK_MULW:                      /* the even lanes, multiplied to 64 bits */
        return conv(e, ia_bin(e, P_STAR, UW, ia_rd(e, x, 0, 2 * j, UW), ia_rd(e, x, 1, 2 * j, UW), loc), x->rl, loc);
    case IAK_SAD: {                     /* the sum of the absolute differences of 8 bytes: |d| = (d ^ m) - m, m = -(d < 0) */
        uint32_t i, sum = TN;
        for (i = 0; i < 8; i++) {
            uint32_t m = ia_mask(e, P_LT, W, ia_diff(e, x, 8 * j + i), cnst(e, W, 0, loc), loc);
            uint32_t m2 = ia_mask(e, P_LT, W, ia_diff(e, x, 8 * j + i), cnst(e, W, 0, loc), loc);
            uint32_t ab = ia_bin(e, P_MINUS, W, ia_bin(e, P_CARET, W, ia_diff(e, x, 8 * j + i), m, loc), m2, loc);
            sum = i ? ia_bin(e, P_PLUS, W, sum, ab, loc) : ab;
        }
        return conv(e, sum, x->rl, loc);
    }
    case IAK_UNPACK: {                  /* the lanes of a and b, interleaved */
        uint32_t k = j / 2 + (arg ? na / 2 : 0);
        return conv(e, ia_rd(e, x, j & 1, k, L), x->rl, loc);
    }
    case IAK_PACK: {                    /* a then b, clamped to the narrower lane type */
        unsigned ob = bits / 2;
        bool us = arg == 1;
        int64_t hi = us ? (int64_t)((1ull << ob) - 1) : (int64_t)((1ull << (ob - 1)) - 1),
                lo = us ? 0 : -hi - 1;
        ia_set(e, &st, x->t, W, ia_rd(e, x, j >= na, j % na, W), loc);
        ia_clamp(e, &st, x->t, W, lo, hi, loc);
        return vec_wrap(e, conv(e, load(e, ia_tmp(e, x->t, W, loc)), x->rl, loc), x->rl, &st, loc);
    }
    case IAK_SHIFT: {                   /* the lane shifted by the count; a count of the lane's width or more empties it */
        bool left = arg == 0, arith = arg == 2;
        uint32_t cnt = load(e, ia_tmp(e, x->cnt, UW, loc)), cnt2 = load(e, ia_tmp(e, x->cnt, UW, loc));
        uint32_t v = ia_rd(e, x, 0, j, arith ? W : UW), big, sh, res;
        sh = ia_bin(e, P_AMP, UW, cnt, cnst(e, UW, bits - 1, loc), loc);
        if (arith) {                    /* a count past the width shifts by width - 1 */
            big = ia_mask(e, P_GE, UW, cnt2, cnst(e, UW, bits, loc), loc);
            sh = ia_bin(e, P_PIPE, UW, sh, ia_bin(e, P_AMP, UW, big, cnst(e, UW, bits - 1, loc), loc), loc);
            res = ia_bin(e, P_SHR, W, v, sh, loc);
        } else {
            big = ia_mask(e, P_LT, UW, cnt2, cnst(e, UW, bits, loc), loc);
            res = ia_bin(e, P_AMP, UW, ia_bin(e, left ? P_SHL : P_SHR, UW, v, sh, loc), big, loc);
        }
        return conv(e, res, x->rl, loc);
    }
    case IAK_SHUF: {                    /* pshufd: 4 lanes; pshuflw, pshufhw: 4 of 8 */
        uint32_t lo = arg == 2 ? 4 : 0, in = j;
        if (j >= lo && j < lo + 4)
            in = lo + (uint32_t)((x->imm >> (2 * (j - lo))) & 3);
        return conv(e, ia_rd(e, x, 0, in, L), x->rl, loc);
    }
    case IAK_BSHIFT: {                  /* the whole vector shifted by imm / 8 bytes */
        int64_t by = x->imm / 8, k = arg ? (int64_t)j + by : (int64_t)j - by;
        if (x->imm < 0 || k < 0 || k >= 16)
            return cnst(e, x->rl, 0, loc);
        return conv(e, ia_rd(e, x, 0, (uint32_t)k, L), x->rl, loc);
    }
    case IAK_SET:                       /* lane imm replaced by the scalar */
        return j == (uint32_t)(x->imm & (na - 1)) ? x->set : conv(e, ia_rd(e, x, 0, j, L), x->rl, loc);
    case IAK_MOVQ:                      /* the low lane, the high one cleared */
        return j ? cnst(e, x->rl, 0, loc) : conv(e, ia_rd(e, x, 0, 0, L), x->rl, loc);
    case IAK_BITOP:                     /* and, or, xor of the bits */
        return ia_bin(e, arg, L, ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc);
    case IAK_FSOP:                      /* lane 0 of a op b, the other lanes of a */
        if (j)
            return ia_rd(e, x, 0, j, L);
        /* fall through */
    case IAK_FPMM:                      /* min and max as the instructions have them: a < b ? a : b */
        if (arg < 4)
            return ia_bin(e, arg == 0 ? P_PLUS : arg == 1 ? P_MINUS : arg == 2 ? P_STAR : P_SLASH, L,
                          ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc);
        else {
            uint32_t ka[3];
            ka[0] = ia_cmp(e, arg == 4 ? P_LT : P_GT, ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc);
            ka[1] = ia_rd(e, x, 0, j, L);
            ka[2] = ia_rd(e, x, 1, j, L);
            return ka[0] == TN || ka[1] == TN || ka[2] == TN ? TN : mk(e, TO_COND, L, loc, 0, 0, 0, ka, 3);
        }
    case IAK_FCMP: {                    /* a mask of ones where the predicate holds (the scalar forms keep a's other lanes) */
        TypeId bt = x->rl;
        uint32_t c = TN;
        if ((r->flags & IAF_SCALAR) && j)
            return ia_rdbits(e, x, 0, j);
        c = ia_pred(e, x, j, arg);
        return c == TN ? TN : ia_un(e, P_MINUS, bt, conv(e, c, bt, loc), loc);
    }
    case IAK_SHUFP:                     /* shufps: two lanes of a then two of b; shufpd: one of each */
        if (bits == 32)
            return ia_rd(e, x, j >= 2, (uint32_t)((x->imm >> (2 * j)) & 3), L);
        return ia_rd(e, x, j, (uint32_t)((x->imm >> j) & 1), L);
    case IAK_MOVLOW:                    /* lane 0 of b, the rest of a (movss, movsd) */
        return ia_rd(e, x, j == 0, j, L);
    case IAK_MOVHL:                     /* movhlps: b's high half, a's high half; movlhps: a's low half, b's low half */
        return arg ? ia_rd(e, x, j >= 2, j & 1, L) : j < 2 ? ia_rd(e, x, 1, j + 2, L) : ia_rd(e, x, 0, j, L);
    case IAK_CVT:                       /* the first arg lanes converted, the rest cleared */
        if (j >= (uint32_t)arg)
            return conv(e, cnst(e, W, 0, loc), x->rl, loc);
        return ia_f2i(e, x, ia_rd(e, x, 0, j, L), x->rl, r->flags & IAF_ROUND, loc);
    case IAK_LOADHL: {                  /* loadlps: lanes 0, 1 from memory, loadhps: lanes 2, 3 (floats at the pointer) */
        TypeId pt = type_ptr(&e->c->tt, L);
        if (arg ? j < 2 : j >= 2)
            return ia_rd(e, x, 0, j, L);
        return load(e, deref_t(e, ptradd(e, pt, load(e, ia_tmp(e, x->cnt, pt, loc)), cnst(e, TYPE_B(INT), j & 1, loc), false, loc), L, loc));
    }
    case IAK_SQRT:                      /* sqrt (arg 0), rcp (1) or rsqrt (2) of every lane */
        if ((r->flags & IAF_SCALAR) && j)
            return ia_rd(e, x, 0, j, L);
    {
        uint32_t v = ia_rd(e, x, 0, j, L);
        return intrin(e, arg == 1 ? "ia32_rcpss" : arg == 2 ? "ia32_rsqrtss" :
                         r->lane == IAL_F32 ? "sqrtf" : "sqrt", L, &v, 1, loc);
    }
    case IAK_CVTPI:                     /* cvtpi2ps: lanes 0, 1 from the two ints of b, the rest of a */
        return j < 2 ? conv(e, ia_rd(e, x, 1, j, x->b.et), x->rl, loc) : ia_rd(e, x, 0, j, x->rl);
    case IAK_PCMPGT:                    /* ones where a > b */
        return ia_un(e, P_MINUS, L, conv(e, ia_cmp(e, P_GT, ia_rd(e, x, 0, j, L), ia_rd(e, x, 1, j, L), loc), L, loc), loc);
    case IAK_CVTS:                      /* lane 0 from a scalar (arg 0) or from lane 0 of b, the rest of a */
        if (j)
            return ia_rd(e, x, 0, j, x->rl);
        return arg ? conv(e, ia_rd(e, x, 1, 0, ia_ty(arg - 1)), x->rl, loc) : x->set;
    default:
        return tunsup(e, "x86 built-in kind");
    }
}

/* The constant argument k of a call. */
static bool ia_const(El *e, const U32Vec *args, size_t k, const char *nm, int64_t *v)
{
    const TNode *nd = &e->nodes.data[args->data[k]];
    if (nd->op != TO_CONST) {
        tunsup(e, "x86 built-in %s with a variable argument", nm);
        return false;
    }
    *v = nd->val;
    return true;
}

/* Built-ins that move memory: unaligned loads and stores, the half stores, maskmovdqu. */
static uint32_t ia_mem(El *e, const IaBt *r, const U32Vec *args, TypeId rt, SrcLoc loc)
{
    Checker *c = e->c;
    Ia x;
    U32Vec setup = {0};
    uint32_t j, id = TN;
    memset(&x, 0, sizeof x);
    x.r = r;
    x.loc = loc;
    x.cnt = x.t = UINT32_MAX;
    if (r->kind == IAK_LOAD || r->kind == IAK_STORE) {   /* unaligned: the same bytes */
        bool st = r->kind == IAK_STORE;
        TypeId vt = unqual(c, st ? nty(e, args->data[1]) : rt);
        uint32_t d = deref_t(e, conv(e, args->data[0], type_ptr(&c->tt, vt), loc), vt, loc), ka[2];
        if (d == TN || !st)
            return d == TN ? TN : load(e, d);
        ka[0] = d;
        ka[1] = args->data[1];
        ka[0] = mk(e, TO_STORE, vt, loc, 0, 0, 0, ka, 2);
        return mk(e, TO_CAST, rt, loc, 0, 0, 0, &ka[0], 1);
    }
    if (r->kind == IAK_MASKMOV) {           /* the bytes of a whose mask byte has its top bit set, stored at the pointer */
        TypeId b8 = ia_ty(IAL_S8), it = TYPE_B(INT);
        uint32_t chain = TN;
        if (!vop_val(e, args->data[0], nty(e, args->data[0]), &x.a, &setup, loc) ||
            !vop_val(e, args->data[1], nty(e, args->data[1]), &x.b, &setup, loc))
            goto done;
        x.a.et = x.b.et = b8;
        x.t = e->ntmp++;
        ia_set(e, &setup, x.t, type_ptr(&c->tt, b8), conv(e, args->data[2], type_ptr(&c->tt, b8), loc), loc);
        for (j = 0; j < 16; j++) {
            uint32_t ka[2], st, one;
            ka[0] = deref_t(e, ptradd(e, type_ptr(&c->tt, b8), load(e, ia_tmp(e, x.t, type_ptr(&c->tt, b8), loc)), cnst(e, it, j, loc), false, loc), b8, loc);
            ka[1] = ia_rd(e, &x, 0, j, b8);
            st = ka[0] == TN || ka[1] == TN ? TN : mk(e, TO_STORE, b8, loc, 0, 0, 0, ka, 2);
            ka[0] = st;
            ka[1] = cnst(e, it, 1, loc);
            one = st == TN ? TN : mk(e, TO_COMMA, it, loc, 0, 0, 0, ka, 2);
            ka[0] = ia_cmp(e, P_LT, ia_rd(e, &x, 1, j, b8), cnst(e, b8, 0, loc), loc);
            ka[1] = one;
            one = ka[0] == TN || one == TN ? TN : mk(e, TO_LOGAND, it, loc, 0, 0, 0, ka, 2);
            if (one == TN) {
                chain = TN;
                break;
            }
            ka[0] = chain;
            ka[1] = one;
            chain = j ? mk(e, TO_COMMA, it, loc, 0, 0, 0, ka, 2) : one;
        }
        id = chain == TN ? TN : vec_wrap(e, chain, it, &setup, loc);
        id = id == TN ? TN : mk(e, TO_CAST, rt, loc, 0, 0, 0, &id, 1);
        goto done;
    }
    if (r->kind == IAK_STOREHL) {           /* storelps, storehps: two lanes as one 64-bit store */
        TypeId ll = ia_ty(IAL_S64), u32 = TYPE_MK(TY_UINT, 0), uw = TYPE_MK(TY_ULONG, 0);
        uint32_t k0 = r->arg ? 2 : 0, lo, hi, ka[2];
        if (!vop_val(e, args->data[1], nty(e, args->data[1]), &x.a, &setup, loc))
            goto done;
        x.a.et = ia_ty(r->lane);
        lo = conv(e, conv(e, ia_rdbits(e, &x, 0, k0), u32, loc), uw, loc);
        hi = conv(e, conv(e, ia_rdbits(e, &x, 0, k0 + 1), u32, loc), uw, loc);
        ka[0] = deref_t(e, conv(e, args->data[0], type_ptr(&c->tt, ll), loc), ll, loc);
        ka[1] = conv(e, ia_bin(e, P_PIPE, uw, lo, ia_bin(e, P_SHL, uw, hi, cnst(e, uw, 32, loc), loc), loc), ll, loc);
        id = ka[0] == TN || ka[1] == TN ? TN : mk(e, TO_STORE, ll, loc, 0, 0, 0, ka, 2);
        id = vec_wrap(e, id, ll, &setup, loc);
        id = id == TN ? TN : mk(e, TO_CAST, rt, loc, 0, 0, 0, &id, 1);
        goto done;
    }
done:
    vec_free(&setup);
    return id;
}

/* Built-ins that are a map over lanes (ia_lane), or a scalar made from them. */
static uint32_t ia_lanes(El *e, const IaBt *r, const char *nm, const U32Vec *args, TypeId rt, SrcLoc loc)
{
    Checker *c = e->c;
    Ia x;
    U32Vec setup = {0};
    uint32_t j, id = TN, *lanes = NULL;
    bool vres = tkind(c, rt) == TY_VECTOR;
    TypeId mt = rt;
    memset(&x, 0, sizeof x);
    x.r = r;
    x.loc = loc;
    x.cnt = x.t = UINT32_MAX;
    if (r->kind == IAK_VINIT) {             /* the lanes are the arguments */
        if (!vres || !vec_dims(e, rt, &x.rl, &x.n) || x.n != args->len)
            goto done;
        lanes = xmalloc(x.n * sizeof *lanes);
        for (j = 0; j < x.n; j++)
            lanes[j] = conv(e, args->data[j], x.rl, loc);
        id = vec_make(e, rt, loc, lanes, x.n, &setup);
        goto done;
    }
    if (!vop_val(e, args->data[0], nty(e, args->data[0]), &x.a, &setup, loc))
        goto done;
    x.a.et = ia_ty(r->lane);
    {
        bool ok = false;
        x.asz = (uint32_t)type_size(&c->tt, x.a.vt, &ok);
    }
    if (r->kind == IAK_LOADHL) {            /* the pointer, kept for both lanes */
        TypeId pt = type_ptr(&c->tt, ia_ty(r->lane));
        x.cnt = e->ntmp++;
        ia_set(e, &setup, x.cnt, pt, conv(e, args->data[1], pt, loc), loc);
    }
    if (args->len > 1 && tkind(c, nty(e, args->data[1])) == TY_VECTOR) {
        if (!vop_val(e, args->data[1], nty(e, args->data[1]), &x.b, &setup, loc))
            goto done;
        x.b.et = ia_ty(r->kind == IAK_SHIFT ? IAL_U64 : r->lane);
    }
    if (r->kind == IAK_SHIFT) {             /* the count: a scalar, or the low 64 bits of a vector */
        TypeId uw = TYPE_MK(TY_ULONG, 0);
        uint32_t cv = r->flags & IAF_VEC ? vop_lane(e, &x.b, 0, loc) : args->data[1];
        x.cnt = e->ntmp++;
        ia_set(e, &setup, x.cnt, uw, conv(e, cv, uw, loc), loc);
    }
    if ((r->kind == IAK_SHUF || r->kind == IAK_BSHIFT) && !ia_const(e, args, 1, nm, &x.imm))
        goto done;
    if ((r->kind == IAK_EXT && args->len > 1) || r->kind == IAK_SET || r->kind == IAK_SHUFP) {
        if (!ia_const(e, args, args->len - 1, nm, &x.imm))
            goto done;
    }
    if (r->kind == IAK_SET) {
        x.set = conv(e, args->data[1], ia_ty(r->lane), loc);
    }
    if (r->kind == IAK_CVTS && !r->arg)
        x.set = conv(e, args->data[1], ia_ty(r->lane), loc);
    if ((r->kind == IAK_CVTS || r->kind == IAK_CVTPI) && r->arg)
        x.b.et = ia_ty(r->arg - 1);
    if (r->kind == IAK_FCOMI) {                 /* lane 0 of a p lane 0 of b, an int */
        id = vec_wrap(e, conv(e, ia_cmp(e, r->arg, ia_rd(e, &x, 0, 0, x.a.et), ia_rd(e, &x, 1, 0, x.a.et), loc), rt, loc), rt, &setup, loc);
        goto done;
    }
    if (r->kind == IAK_SAT || r->kind == IAK_PACK)
        x.t = e->ntmp++;
    if (r->kind == IAK_MOVMSK) {            /* the sign bit of every byte, one bit each */
        uint32_t sum = TN;
        TypeId it = TYPE_B(INT);
        for (j = 0; j < x.asz / (ia_bits(r->lane) / 8); j++) {
            uint32_t ka[2], m, b;
            ka[0] = ia_rd(e, &x, 0, j, x.a.et);
            ka[1] = cnst(e, x.a.et, 0, loc);
            m = mk(e, TO_CMP, it, loc, 0, 0, P_LT, ka, 2);
            b = j ? ia_bin(e, P_SHL, it, m, cnst(e, it, j, loc), loc) : m;
            sum = sum == TN ? b : ia_bin(e, P_PIPE, it, sum, b, loc);
        }
        id = vec_wrap(e, sum, it, &setup, loc);
        goto done;
    }
    if (r->kind == IAK_EXT) {
        uint32_t n = 16 / (ia_bits(r->lane) / 8);
        {
            uint32_t v = ia_rd(e, &x, 0, (uint32_t)(x.imm & (n - 1)), ia_ty(r->lane));
            id = vec_wrap(e, ia_f2i(e, &x, v, rt, r->flags & IAF_ROUND, loc), rt, &setup, loc);
        }
        goto done;
    }
    if (!vres || !vec_dims(e, rt, &x.rl, &x.n))
        goto done;
    if (ia_bitspace(r->kind)) {               /* made of integer lanes, then seen as the return type */
        bool ok = false;
        uint32_t rsz = (uint32_t)type_size(&c->tt, rt, &ok);
        x.rl = ia_bitty(r->lane);
        x.n = rsz / (ia_bits(r->lane) / 8);
        mt = type_vector(&c->tt, x.rl, rsz);
    }
    lanes = xmalloc((x.n ? x.n : 1) * sizeof *lanes);
    for (j = 0; j < x.n; j++)
        lanes[j] = ia_lane(e, &x, j);
    id = vec_make(e, mt, loc, lanes, x.n, &setup);
    if (id != TN && mt != rt)
        id = mk(e, TO_CAST, rt, loc, 0, 0, 0, &id, 1);
done:
    free(lanes);
    vec_free(&setup);
    return id;
}

/* A call of __builtin_ia32_NAME (nm is "ia32_NAME"): by its row's kind, an opaque back-end built-in
 * (NODE), an operation on memory (ia_mem), or a map over lanes (ia_lanes). */
uint32_t ia32_call(El *e, const char *nm, const U32Vec *args, TypeId rt, SrcLoc loc)
{
    const IaBt *r = ia32_find(nm);
    if (!r || !e->infunc)
        return tunsup(e, "x86 built-in %s", nm);
    switch (r->kind) {
    case IAK_NODE:                      /* fences and the like: a back-end built-in of the same name */
        return intrin(e, nm, rt, args->data, (uint32_t)args->len, loc);
    case IAK_LOAD: case IAK_STORE: case IAK_STOREHL: case IAK_MASKMOV:
        return ia_mem(e, r, args, rt, loc);
    default:
        return ia_lanes(e, r, nm, args, rt, loc);
    }
}

