/* intarith.c - `cereal selftest intarith`: ia_binop, ia_unop and ia_canon against a 128-bit reference. */
#include <stdio.h>

#include "intarith.h"

__extension__ typedef __int128 Wide;
__extension__ typedef unsigned __int128 UWide;

static Wide t_lo(unsigned bits, bool sg) { return sg ? -((Wide)1 << (bits - 1)) : 0; }
static Wide t_hi(unsigned bits, bool sg)
{
    return sg ? ((Wide)1 << (bits - 1)) - 1 : ((Wide)1 << bits) - 1;
}
static bool t_fits(unsigned bits, bool sg, Wide v) { return v >= t_lo(bits, sg) && v <= t_hi(bits, sg); }

/* the canonical encoding of v modulo 2^bits */
static uint64_t t_wrap(unsigned bits, bool sg, UWide v)
{
    UWide m = bits == 64 ? (((UWide)1 << 64) - 1) : (((UWide)1 << bits) - 1);
    return ia_canon((IaTy){(uint8_t)bits, sg, false}, (uint64_t)(v & m));
}

static int t_fail(const char *what, unsigned bits, bool sg, Wide x, Wide y, uint64_t got, unsigned fl)
{
    fprintf(stderr, "intarith: %s bits=%u sg=%d x=%lld y=%lld got=%llx flags=%u\n", what, bits, sg,
            (long long)x, (long long)y, (unsigned long long)got, fl);
    return 1;
}

int intarith_selftest(void)
{
    static const unsigned widths[] = {8, 16, 32, 64};
    int bad = 0;
    unsigned wi, i, j, k;
    int sgi;
    /* canon: _Bool, and the width casts */
    if (ia_canon((IaTy){1, false, true}, 256) != 1 || ia_canon((IaTy){1, false, true}, 0) != 0 ||
        ia_canon((IaTy){8, true, false}, 0x80) != (uint64_t)-128 ||
        ia_canon((IaTy){8, false, false}, ~UINT64_C(0)) != 255 ||
        ia_canon((IaTy){32, true, false}, 0xffffffffu) != ~UINT64_C(0))
        bad += t_fail("canon", 0, 0, 0, 0, 0, 0);
    for (wi = 0; wi < 4; wi++)
        for (sgi = 0; sgi < 2; sgi++) {
            unsigned bits = widths[wi];
            bool sg = sgi;
            IaTy t = {(uint8_t)bits, sg, false};
            Wide v[48];
            unsigned n = 0;
            const Wide small[] = {0, 1, 2, 3, 7, bits - 1, bits, bits + 1};
            for (i = 0; i < 8; i++) {
                v[n++] = small[i];
                if (sg)
                    v[n++] = -small[i];
            }
            v[n++] = t_hi(bits, sg);
            v[n++] = t_lo(bits, sg);
            v[n++] = t_hi(bits, sg) - 1;
            v[n++] = t_lo(bits, sg) + 1;
            for (i = 0; i < n; i++) {
                Wide x = v[i];
                unsigned fl;
                uint64_t a, r;
                if (!t_fits(bits, sg, x))
                    continue;
                a = t_wrap(bits, sg, x);
                r = ia_unop(P_MINUS, t, a, &fl);
                if (r != t_wrap(bits, sg, (UWide)0 - (UWide)x) || fl != (sg && x == t_lo(bits, sg) ? IA_OVF : 0u))
                    bad += t_fail("unary -", bits, sg, x, 0, r, fl);
                r = ia_unop(P_TILDE, t, a, &fl);
                if (r != t_wrap(bits, sg, ~(UWide)x) || fl)
                    bad += t_fail("~", bits, sg, x, 0, r, fl);
            }
            for (i = 0; i < n; i++)
                for (j = 0; j < n; j++) {
                    Wide x = v[i], y = v[j];
                    uint64_t a, b, r;
                    unsigned fl;
                    static const int arith[] = {P_PLUS, P_MINUS, P_STAR};
                    if (!t_fits(bits, sg, x) || !t_fits(bits, sg, y))
                        continue;
                    a = t_wrap(bits, sg, x);
                    b = t_wrap(bits, sg, y);
                    for (k = 0; k < 3; k++) {
                        UWide e = arith[k] == P_PLUS ? (UWide)x + (UWide)y
                                  : arith[k] == P_MINUS ? (UWide)x - (UWide)y : (UWide)x * (UWide)y;
                        bool fit = !sg || t_fits(bits, sg, (Wide)e);    /* signed operands: |e| < 2^127 */
                        r = ia_binop(arith[k], t, a, b, &fl);
                        if (r != t_wrap(bits, sg, e) || fl != (fit ? 0u : IA_OVF))
                            bad += t_fail("+-*", bits, sg, x, y, r, fl);
                    }
                    if (y == 0) {
                        r = ia_binop(P_SLASH, t, a, b, &fl);
                        if (r != 0 || fl != IA_ZDIV)
                            bad += t_fail("zdiv", bits, sg, x, y, r, fl);
                    } else {
                        unsigned want = sg && y == -1 && x == t_lo(bits, sg) ? IA_OVF : 0u;
                        r = ia_binop(P_SLASH, t, a, b, &fl);
                        if (r != t_wrap(bits, sg, x / y) || fl != want)
                            bad += t_fail("/", bits, sg, x, y, r, fl);
                        r = ia_binop(P_PERCENT, t, a, b, &fl);
                        if (r != t_wrap(bits, sg, x % y) || fl != want)
                            bad += t_fail("%", bits, sg, x, y, r, fl);
                    }
                    if (ia_binop(P_AMP, t, a, b, &fl) != t_wrap(bits, sg, sg ? (Wide)((int64_t)a & (int64_t)b) : (Wide)(a & b)) || fl)
                        bad += t_fail("&", bits, sg, x, y, 0, fl);
                    if (y >= 0) {
                        r = ia_binop(P_SHR, t, a, b, &fl);
                        if (y >= bits ? fl != IA_SHIFT_RANGE || r != (sg && x < 0 ? ~UINT64_C(0) : 0)
                                      : fl || r != t_wrap(bits, sg, x >> (int)y))
                            bad += t_fail(">>", bits, sg, x, y, r, fl);
                        r = ia_binop(P_SHL, t, a, b, &fl);
                        if (y >= bits) {
                            if (fl != (IA_SHIFT_RANGE | (sg && x != 0 ? IA_SHIFT_LOSSY : 0u)) || r != 0)
                                bad += t_fail("<< range", bits, sg, x, y, r, fl);
                        } else {
                            Wide e = x * ((Wide)1 << (int)y);
                            unsigned want = (sg && x < 0 ? IA_SHIFT_NEG : 0u) |
                                            (sg && !t_fits(bits, sg, e) ? IA_SHIFT_LOSSY : 0u);
                            if (fl != want || r != t_wrap(bits, sg, e))
                                bad += t_fail("<<", bits, sg, x, y, r, fl);
                        }
                    }
                }
        }
    /* the libcpp rule: a negative count shifts the other way */
    {
        IaTy t = {64, true, false};
        unsigned fl;
        if (ia_shift_cpp(P_SHL, t, 8, (uint64_t)-1, true, &fl) != 4 || fl ||
            ia_shift_cpp(P_SHR, t, 8, (uint64_t)-2, true, &fl) != 32 || fl ||
            ia_shift_cpp(P_SHL, t, (uint64_t)-8, (uint64_t)-1, true, &fl) != (uint64_t)-4 || fl)
            bad += t_fail("shift_cpp", 64, 1, 0, 0, 0, fl);
    }
    if (bad)
        fprintf(stderr, "intarith: %d failures\n", bad);
    return bad != 0;
}
