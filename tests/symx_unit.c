/* Unit check for src/c/symx.c: expressions and the solver pipe.
 *   gcc -O1 -Isrc -Isrc/c tests/symx_unit.c src/c/symx.c src/common.c -o /tmp/symx_unit
 *   /tmp/symx_unit 'z3 -in -smt2'
 * Exit 0 and "ok" when every query answers as expected. */
#include "../src/c/symx.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint64_t val(const SxModel *m, uint32_t off) {
    for (size_t i = 0; i < m->off.len; i++) if (m->off.data[i] == off) return m->val.data[i];
    return ~0ull;
}

int main(int argc, char **argv) {
    char err[256];
    Sx sx = {0};
    SxModel m = {0};
    SxSolver *s = sx_open(argc > 1 ? argv[1] : "z3 -in -smt2", err, sizeof err);
    if (!s) { printf("open: %s\n", err); return 1; }

    /* x = zero-extended byte 0; x * 3 == 99 */
    uint32_t b0 = sx_sym(&sx, 0);
    uint32_t x = sx_ext(&sx, b0, 1, false, false);
    uint32_t e = sx_op(&sx, SX_EQ, sx_op(&sx, SX_MUL, x, sx_const(&sx, 64, 3)), sx_const(&sx, 64, 99));
    sx_begin(s, &sx);
    CHECK(sx_try(s, &(SxCond){e, true}, 1, &m) == SX_SAT);
    CHECK(val(&m, 0) == 0x21);
    /* the same problem asserted: a second, contradicting question is UNSAT, and the stack unwinds */
    CHECK(sx_assert(s, e, true));
    uint32_t ne = sx_op(&sx, SX_EQ, x, sx_const(&sx, 64, 7));
    sx_begin(s, &sx);
    CHECK(sx_assert(s, e, true));
    CHECK(sx_try(s, &(SxCond){ne, true}, 1, &m) == SX_UNSAT);
    CHECK(sx_try(s, &(SxCond){ne, false}, 1, &m) == SX_SAT && val(&m, 0) == 0x21);

    /* a 16-bit little-endian value from bytes 1 (high) and 2 (low), signed: v < 0 and v * v == 0x10000 * 0 + 144 + 0 */
    uint32_t v = sx_ext(&sx, sx_join(&sx, sx_sym(&sx, 1), sx_sym(&sx, 2)), 2, true, false);
    uint32_t sq = sx_op(&sx, SX_EQ, sx_op(&sx, SX_MUL, v, v), sx_const(&sx, 64, 144));
    uint32_t neg = sx_op(&sx, SX_SLT, v, sx_const(&sx, 64, 0));
    sx_begin(s, &sx);
    CHECK(sx_assert(s, sq, true));
    CHECK(sx_try(s, &(SxCond){neg, true}, 1, &m) == SX_SAT);
    CHECK(val(&m, 1) == 0xff && val(&m, 2) == 0xf4);        /* -12 */
    /* byte j of a slot and a boolean cast */
    uint32_t hi = sx_ext(&sx, sx_byte(&sx, v, 1), 1, false, false);
    uint32_t bl = sx_ext(&sx, hi, 8, false, true);
    sx_begin(s, &sx);
    CHECK(sx_try(s, &(SxCond){bl, false}, 1, &m) == SX_SAT && val(&m, 1) == 0);

    /* a mask: w kept when the flag byte is non-zero; a test of the masked word for zero is a test of the flag */
    uint32_t w = sx_const(&sx, 64, 0x100000080ull), fl = sx_ext(&sx, sx_sym(&sx, 3), 1, false, false);
    uint32_t mk = sx_op(&sx, SX_MASK, w, fl), isnull = sx_op(&sx, SX_EQ, mk, sx_const(&sx, 64, 0));
    sx_begin(s, &sx);
    CHECK(sx_try(s, &(SxCond){isnull, true}, 1, &m) == SX_SAT && val(&m, 3) == 0);
    CHECK(sx_try(s, &(SxCond){isnull, false}, 1, &m) == SX_SAT && val(&m, 3) != 0);
    CHECK(sx_assert(s, sx_op(&sx, SX_EQ, mk, w), true));
    CHECK(sx_try(s, &(SxCond){isnull, true}, 1, &m) == SX_UNSAT);

    sx_model_free(&m);
    sx_free(&sx);
    sx_close(s);
    if (!fails) puts("ok");
    return fails != 0;
}
