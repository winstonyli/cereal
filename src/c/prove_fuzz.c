/* prove_fuzz.c - differential soundness check of the prover's solver tiers (docs/NARROW_INT_PLAN.md).
 *
 * Random goals over two 8-bit variables (terms of 8 and 16 bits: add, sub, mul, resize, signed and unsigned compares) are
 * decided by brute force over all 65536 assignments and by the tiers.  Bit-vectors are exact, so ps_valid must agree with
 * the brute force in both directions (that checks the evaluator below; a goal with exact Int atoms over bit-vector readings,
 * PS_B2I, may be `unknown` there: the exact tier does not read them); the Int tier and ps_prove may say VALID only when
 * the brute force does (they may say less: `unknown` and, with a hypothesis left out, `invalid` are not claims). */
#include "prove_smt.h"

#include <stdio.h>
#include <string.h>

typedef struct Fz {
    Ps *ps;
    uint64_t rng;
    PsT x, y;                           /* the two 8-bit variables */
} Fz;

static uint64_t rnd(Fz *f)
{
    f->rng ^= f->rng << 13; f->rng ^= f->rng >> 7; f->rng ^= f->rng << 17;
    return f->rng;
}

static uint64_t mask(unsigned w) { return w == 64 ? ~UINT64_C(0) : (UINT64_C(1) << w) - 1; }
static int64_t sgn(uint64_t v, unsigned w) { return (int64_t)(v << (64 - w)) >> (64 - w); }

static PsT gen_bv(Fz *f, unsigned w, int depth)
{
    static const uint64_t consts[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 100, 127, 128, 129, 200, 254, 255};
    Ps *ps = f->ps;
    unsigned r = (unsigned)(rnd(f) % 10);
    if (depth == 0 || r < 3) {
        if (w == 8 && r % 2) return rnd(f) & 1 ? f->x : f->y;
        if (w == 16 && r % 2) return ps_resize(ps, rnd(f) & 1 ? f->x : f->y, 16, rnd(f) & 1);
        return ps_bvc(ps, w, (rnd(f) % 3 ? consts[rnd(f) % 16] : rnd(f)) & mask(w));
    }
    if (r == 3 && w == 16) return ps_resize(ps, gen_bv(f, 8, depth - 1), 16, rnd(f) & 1);
    if (r == 4 && w == 8) return ps_resize(ps, gen_bv(f, 16, depth - 1), 8, false);
    return ps_op(ps, r < 7 ? PS_BVADD : r < 9 ? PS_BVSUB : PS_BVMUL, gen_bv(f, w, depth - 1), gen_bv(f, w, depth - 1));
}

/* exact arithmetic over bit-vector readings, as a spec's: small enough (8- and 16-bit leaves) that no product leaves int64 */
static PsT gen_int(Fz *f, int depth)
{
    Ps *ps = f->ps;
    unsigned r = (unsigned)(rnd(f) % 8);
    if (depth == 0 || r < 3) {
        if (r == 0) return ps_intc(ps, rnd(f) % 300);
        return ps_b2i(ps, gen_bv(f, rnd(f) & 1 ? 8 : 16, 1), rnd(f) & 1);
    }
    return ps_op(ps, r < 5 ? PS_IADD : r < 7 ? PS_ISUB : PS_IMUL, gen_int(f, depth - 1), gen_int(f, depth - 1));
}

static PsT gen_fm(Fz *f, int depth)
{
    static const int cmp[] = {PS_BVULT, PS_BVULE, PS_BVSLT, PS_BVSLE, PS_EQ};
    Ps *ps = f->ps;
    unsigned r = (unsigned)(rnd(f) % 8), w = rnd(f) % 3 ? 8 : 16;
    if ((depth == 0 || r < 5) && rnd(f) % 4 == 0) {
        static const int icmp[] = {PS_ILT, PS_ILE, PS_EQ};
        return ps_op(ps, icmp[rnd(f) % 3], gen_int(f, 1), gen_int(f, 1));
    }
    if (depth == 0 || r < 5)
        return ps_op(ps, cmp[rnd(f) % 5], gen_bv(f, w, 2), gen_bv(f, w, 2));
    if (r == 5) return ps_op(ps, PS_NOT, gen_fm(f, depth - 1), 0);
    return ps_op(ps, r == 6 ? PS_AND : rnd(f) & 1 ? PS_OR : PS_IMP, gen_fm(f, depth - 1), gen_fm(f, depth - 1));
}

/* the value of t (a bit-vector masked to its width, or a Bool as 0/1) at x, y */
static uint64_t eval(const Ps *ps, PsT t, uint64_t x, uint64_t y)
{
    const PsNode *n = ps_at(ps, t);
    uint64_t a = 0, b = 0;
    unsigned w = n->w;
    switch (n->op) {
    case PS_TRUE: return 1;
    case PS_FALSE: return 0;
    case PS_BVC: return n->k & mask(w);
    case PS_VAR: return ps->v.data[n->k].name[0] == 'x' ? x : y;
    case PS_RESIZE: {
        unsigned from = ps_at(ps, n->a)->w, to = (unsigned)(n->k & 0xff);
        a = eval(ps, n->a, x, y);
        if (to > from && (n->k & PS_SIGNED)) a = (uint64_t)sgn(a, from);
        return a & mask(to);
    }
    case PS_INTC: return n->k;
    case PS_B2I:                        /* Int values are int64 held in the same word */
        a = eval(ps, n->a, x, y);
        return n->k & PS_SIGNED ? (uint64_t)sgn(a, ps_at(ps, n->a)->w) : a;
    default: break;
    }
    a = eval(ps, n->a, x, y);
    if (n->op != PS_NOT) b = eval(ps, n->b, x, y);
    switch (n->op) {
    case PS_IADD: return a + b;
    case PS_ISUB: return a - b;
    case PS_IMUL: return a * b;
    case PS_ILT: return (int64_t)a < (int64_t)b;
    case PS_ILE: return (int64_t)a <= (int64_t)b;
    case PS_BVADD: return (a + b) & mask(w);
    case PS_BVSUB: return (a - b) & mask(w);
    case PS_BVMUL: return (a * b) & mask(w);
    case PS_BVULT: return a < b;
    case PS_BVULE: return a <= b;
    case PS_BVSLT: return sgn(a, ps_at(ps, n->a)->w) < sgn(b, ps_at(ps, n->a)->w);
    case PS_BVSLE: return sgn(a, ps_at(ps, n->a)->w) <= sgn(b, ps_at(ps, n->a)->w);
    case PS_EQ: return a == b;
    case PS_AND: return a && b;
    case PS_OR: return a || b;
    case PS_NOT: return !a;
    case PS_IMP: return !a || b;
    default: return 0;
    }
}

static bool truly_valid(const Ps *ps, const PsT *h, size_t nh, PsT goal)
{
    unsigned x, y;
    size_t i;
    for (x = 0; x < 256; x++)
        for (y = 0; y < 256; y++) {
            for (i = 0; i < nh && eval(ps, h[i], x, y); i++) ;
            if (i == nh && !eval(ps, goal, x, y)) return false;
        }
    return true;
}

static void report(Ps *ps, const char *what, const PsT *h, size_t nh, PsT goal, int got, bool truth)
{
    StrBuf sb = {0};
    size_t i;
    for (i = 0; i < nh; i++) { sb_puts(&sb, " H: "); ps_print(ps, h[i], &sb); }
    sb_puts(&sb, " G: ");
    ps_print(ps, goal, &sb);
    fprintf(stderr, "prove fuzz: %s says %d, truth is %s:%s\n", what, got, truth ? "valid" : "invalid", sb_cstr(&sb));
    sb_free(&sb);
}

int prove_fuzz(PsSolver *s, unsigned n, uint64_t seed)
{
    Ps ps = {0};
    Fz f = {&ps, seed ? seed : 1, 0, 0};
    unsigned i, valid = 0, int_valid = 0, failures = 0;
    f.x = ps_var(&ps, "x", PS_BV, 8);
    f.y = ps_var(&ps, "y", PS_BV, 8);
    for (i = 0; i < n; i++) {
        PsT h[3], goal;
        size_t nh = rnd(&f) % 4, k;
        bool truth, withint;
        int r;
        for (k = 0; k < nh; k++) h[k] = gen_fm(&f, 1);
        goal = gen_fm(&f, 2);
        truth = truly_valid(&ps, h, nh, goal);
        valid += truth;
        /* bit-vectors alone are decided exactly; an Int term (bv2nat, a product) may leave the solver `unknown`, never wrong */
        for (k = 0, withint = ps_has(&ps, goal, PS_B2I); k < nh; k++) withint |= ps_has(&ps, h[k], PS_B2I);
        r = ps_valid(s, &ps, h, nh, goal, withint ? 1000000 : 0);
        if (r != (truth ? PS_VALID : PS_INVALID) && !(withint && r == PS_UNKNOWN)) {
            report(&ps, "bit-vectors", h, nh, goal, r, truth);
            failures++;
        }
        r = ps_valid_int(s, &ps, h, nh, goal, 1000000);
        int_valid += r == PS_VALID;
        if (r == PS_VALID && !truth) { report(&ps, "the Int tier", h, nh, goal, r, truth); failures++; }
        r = ps_prove(s, &ps, h, nh, goal, 1000000);
        if (r == PS_VALID && !truth) { report(&ps, "ps_prove", h, nh, goal, r, truth); failures++; }
    }
    if (n >= 100 && (valid < n / 20 || int_valid < n / 40)) {      /* the generator must reach valid goals, or the check is empty */
        fprintf(stderr, "prove fuzz: of %u goals only %u are valid and the Int tier proved %u\n", n, valid, int_valid);
        failures++;
    }
    if (n >= 1000)
        fprintf(stderr, "prove fuzz: %u goals, %u valid, Int tier proved %u, %u failures\n", n, valid, int_valid, failures);
    ps_free(&ps);
    return failures != 0;
}

int prove_fuzz_main(unsigned n, uint64_t seed)
{
    char err[200];
    PsSolver *s = ps_open("z3 -in -smt2", err, sizeof err);
    int r;
    if (!s) { fprintf(stderr, "prove fuzz: %s\n", err); return 1; }
    r = prove_fuzz(s, n, seed);
    ps_close(s);
    return r;
}
