#ifndef CEREAL_FPARITH_H
#define CEREAL_FPARITH_H
#include <float.h>

#include "token.h"

/* + - * / on floating-point constants in the operands' own precision, shared by the constant folder
 * (c/cexpr.c) and the evaluator (c/eval.c).  The folder used to compute in long double and round
 * after, which rounds twice for double. */
#if FLT_EVAL_METHOD != 0
#error "fparith.h needs FLT_EVAL_METHOD == 0: an operation must round to its own type"
#endif

typedef enum { FP_F32, FP_F64, FP_LD } FpTy;

/* a OP b for op = P_PLUS, P_MINUS, P_STAR or (anything else) division: each operand is converted to
 * the type first (the C rule for an operand of another type), the operation is done in the type,
 * and the result is widened */
static inline long double fl_binop(int op, FpTy t, long double a, long double b)
{
    if (t == FP_F32) {
        float x = (float)a, y = (float)b;
        return op == P_PLUS ? x + y : op == P_MINUS ? x - y : op == P_STAR ? x * y : x / y;
    }
    if (t == FP_F64) {
        double x = (double)a, y = (double)b;
        return op == P_PLUS ? x + y : op == P_MINUS ? x - y : op == P_STAR ? x * y : x / y;
    }
    return op == P_PLUS ? a + b : op == P_MINUS ? a - b : op == P_STAR ? a * b : a / b;
}

int fparith_selftest(void);   /* `cereal selftest fparith`: the kernel against hardware float and double */

#endif
