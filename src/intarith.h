#ifndef CEREAL_INTARITH_H
#define CEREAL_INTARITH_H
#include "token.h"

/* integer arithmetic in two's complement on at most 64 bits, shared by the constant folder
 * (c/cexpr.c), the evaluator (c/eval.c), #if (ppexpr.c) and presence conditions
 * (analysis/pcexpr.c).  The kernel never reports: it returns a defined value and flags
 * for what was undefined or lost, and each caller applies its own policy. */

/* a type: its width, signedness, and whether it is _Bool (canonical form v != 0) */
typedef struct { uint8_t bits; bool sg, boolean; } IaTy;
enum {
    IA_OVF = 1,          /* + - * / %: a signed result that does not fit (minimum / -1 and % -1 too) */
    IA_ZDIV = 2,         /* / % by zero (the value is 0) */
    IA_SHIFT_RANGE = 4,  /* count >= width (a negative count is huge) */
    IA_SHIFT_NEG = 8,    /* signed << of a negative value */
    IA_SHIFT_LOSSY = 16  /* signed <<: shifting the result back does not give the operand */
};

/* v as a value of t: truncated to its width, then sign- or zero-extended */
static inline uint64_t ia_canon(IaTy t, uint64_t v)
{
    unsigned sh;
    if (t.boolean)
        return v != 0;
    if (t.bits == 0 || t.bits >= 64)
        return v;
    sh = 64 - t.bits;
    return t.sg ? (uint64_t)((int64_t)(v << sh) >> sh) : (v << sh) >> sh;
}

/* the operators ia_binop computes */
static inline bool ia_is_arith(int op)
{
    return op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH || op == P_PERCENT ||
           op == P_AMP || op == P_PIPE || op == P_CARET || op == P_SHL || op == P_SHR;
}

/* a OP b for canonical operands of t; the result is canonical, always defined */
static inline uint64_t ia_binop(int op, IaTy t, uint64_t a, uint64_t b, unsigned *flags)
{
    unsigned bits = t.bits == 0 || t.bits > 64 ? 64 : t.bits;
    bool sg = t.sg && !t.boolean;
    int64_t sa = (int64_t)a, sb = (int64_t)b, sr = 0;
    uint64_t r = 0;
    *flags = 0;
    switch (op) {
    case P_PLUS: case P_MINUS: case P_STAR: {
        bool o = op == P_PLUS ? __builtin_add_overflow(sa, sb, &sr)
                 : op == P_MINUS ? __builtin_sub_overflow(sa, sb, &sr)
                                 : __builtin_mul_overflow(sa, sb, &sr);
        r = op == P_PLUS ? a + b : op == P_MINUS ? a - b : a * b;
        if (sg && (o || ia_canon(t, (uint64_t)sr) != (uint64_t)sr))
            *flags |= IA_OVF;
        return ia_canon(t, r);
    }
    case P_SLASH: case P_PERCENT:
        if (b == 0) {
            *flags |= IA_ZDIV;
            return 0;
        }
        if (sg) {
            int64_t mn = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
            if (sb == -1) {
                if (sa == mn)
                    *flags |= IA_OVF;
                r = op == P_SLASH ? 0 - a : 0;
            } else {
                r = (uint64_t)(op == P_SLASH ? sa / sb : sa % sb);
            }
        } else {
            r = op == P_SLASH ? a / b : a % b;
        }
        return ia_canon(t, r);
    case P_AMP: return ia_canon(t, a & b);
    case P_PIPE: return ia_canon(t, a | b);
    case P_CARET: return ia_canon(t, a ^ b);
    case P_SHL: case P_SHR:
        if (b >= bits) {
            *flags |= IA_SHIFT_RANGE;
            if (op == P_SHL && sg && a != 0)
                *flags |= IA_SHIFT_LOSSY;
            return op == P_SHR && sg && sa < 0 ? ~UINT64_C(0) : 0;
        }
        if (op == P_SHR)
            return ia_canon(t, sg ? (uint64_t)(sa >> b) : a >> b);
        r = ia_canon(t, a << b);
        if (sg) {
            if (sa < 0)
                *flags |= IA_SHIFT_NEG;
            if ((int64_t)r >> b != sa)
                *flags |= IA_SHIFT_LOSSY;
        }
        return r;
    default:
        return 0;
    }
}

/* - or ~ of a canonical operand of t; - of the signed minimum wraps and sets IA_OVF */
static inline uint64_t ia_unop(int op, IaTy t, uint64_t a, unsigned *flags)
{
    if (op == P_MINUS)
        return ia_binop(P_MINUS, t, 0, a, flags);
    *flags = 0;
    return ia_canon(t, ~a);
}

/* #if and presence conditions: a negative count shifts the other way (libcpp); b_neg says the
 * count is a negative signed value, b its two's complement bits */
static inline uint64_t ia_shift_cpp(int op, IaTy t, uint64_t a, uint64_t b, bool b_neg, unsigned *flags)
{
    if (b_neg) {
        op = op == P_SHL ? P_SHR : P_SHL;
        b = 0 - b;
    }
    return ia_binop(op, t, a, b, flags);
}

int intarith_selftest(void);   /* `cereal selftest intarith`: the kernel against a wide reference */

#endif
