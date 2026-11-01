/* fparith.c - `cereal selftest fparith`: fl_binop against the hardware's float and double operations. */
#include <stdio.h>

#include "fparith.h"

static bool same(long double a, long double b)
{
    return a == b || (a != a && b != b);
}

int fparith_selftest(void)
{
    static const double vals[] = {0, 1, -1, 3, 0.1, 1e300, 1e-300, 0x1p-24, 0x1.000002p-24, 0x1p-53,
                                  0x1.0000002p-53, 0x1p-52, 9007199254740993.0, 4294967297.0, -0.5};
    static const int ops[] = {P_PLUS, P_MINUS, P_STAR, P_SLASH};
    unsigned i, j, k;
    int bad = 0;
    for (i = 0; i < sizeof vals / sizeof vals[0]; i++)
        for (j = 0; j < sizeof vals / sizeof vals[0]; j++)
            for (k = 0; k < 4; k++) {
                volatile double x = vals[i], y = vals[j], rd;
                volatile float fx = (float)vals[i], fy = (float)vals[j], rf;
                int op = ops[k];
                rd = op == P_PLUS ? x + y : op == P_MINUS ? x - y : op == P_STAR ? x * y : x / y;
                rf = op == P_PLUS ? fx + fy : op == P_MINUS ? fx - fy : op == P_STAR ? fx * fy : fx / fy;
                if (!same(fl_binop(op, FP_F64, vals[i], vals[j]), rd)) {
                    fprintf(stderr, "fparith: double %g %d %g\n", vals[i], op, vals[j]);
                    bad++;
                }
                if (!same(fl_binop(op, FP_F32, vals[i], vals[j]), rf)) {
                    fprintf(stderr, "fparith: float %g %d %g\n", vals[i], op, vals[j]);
                    bad++;
                }
            }
    /* the midpoint case: 1 + (2^-53 + 2^-80) is above the midpoint, so one rounding gives 1 + 2^-52 */
    if (fl_binop(P_PLUS, FP_F64, 1.0L, 0x1.0000002p-53L) != 0x1.0000000000001p0L) {
        fprintf(stderr, "fparith: midpoint case\n");
        bad++;
    }
    /* an integer operand is rounded to double before the add */
    if (fl_binop(P_PLUS, FP_F64, 1.0L, 9007199254740993.0L) != 9007199254740992.0L) {
        fprintf(stderr, "fparith: operand conversion\n");
        bad++;
    }
    if (bad)
        fprintf(stderr, "fparith: %d failures\n", bad);
    return bad != 0;
}
