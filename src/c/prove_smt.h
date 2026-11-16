/* prove_smt.h - the prover's term DAG and its solver pipe (docs/PROVER_DESIGN.md, stage 2).
 *
 * Terms have four sorts: Bool, bit-vectors of 8, 16, 32 or 64 bits, unbounded Int and the array
 * (64-bit index, an element of 8, 16, 32 or 64 bits: the sort's w) that a per-allocation memory is made of.  Variables are
 * named; a quantifier binds one.  The rewrites the engine's tiers need are functions over the DAG
 * (substitution, the bit-vector-to-Int rewrite with its no-wrap obligations); printing is SMT-LIB2.
 * symx.h's DAG stays the evaluator's; the two share only the solver pipe. */
#ifndef CEREAL_PROVE_SMT_H
#define CEREAL_PROVE_SMT_H

#include "common.h"

typedef uint32_t PsT;                   /* a term: an index + 1; 0 is "no term" */

enum { PS_BOOL, PS_BV, PS_INT, PS_ARR };

enum {
    PS_TRUE, PS_FALSE,
    PS_BVC,                             /* k: the value; w bits */
    PS_INTC,                            /* k: the value */
    PS_P64,                             /* the Int 2^64 */
    PS_VAR,                             /* k: index into the variable table */
    PS_BVADD, PS_BVMUL, PS_BVNOT, PS_BVULT, PS_BVULE,
    PS_BVSUB, PS_BVSLT, PS_BVSLE,
    PS_RESIZE,                          /* a: the operand; k: target width, | PS_SIGNED to sign-extend (truncates when narrower) */
    PS_IADD, PS_IMUL, PS_ILT, PS_ILE,
    PS_EQ, PS_AND, PS_OR, PS_NOT, PS_IMP,
    PS_SEL, PS_STORE,                   /* array a, index b [, element c] */
    PS_FORALL                           /* a: the bound variable, b: the body */
};

#define PS_SIGNED 0x100                 /* in a PS_RESIZE's k */

typedef struct PsNode {
    uint8_t op, sort, w;
    PsT a, b, c;
    uint64_t k;
} PsNode;

typedef struct PsVar {
    char name[40];
    uint8_t sort, w;
} PsVar;

typedef struct Ps {
    VEC(PsNode) n;
    VEC(PsVar) v;
} Ps;

void ps_free(Ps *ps);
static inline const PsNode *ps_at(const Ps *ps, PsT t) { return &ps->n.data[t - 1]; }

PsT ps_true(Ps *ps);
PsT ps_bvc(Ps *ps, unsigned w, uint64_t v);
PsT ps_intc(Ps *ps, uint64_t v);
PsT ps_var(Ps *ps, const char *name, int sort, unsigned w);   /* the same name, the same variable */
PsT ps_op(Ps *ps, int op, PsT a, PsT b);                      /* the binary and unary operations (b: 0 for PS_BVNOT, PS_NOT) */
PsT ps_resize(Ps *ps, PsT a, unsigned w, bool sign);          /* a bit-vector as w bits: zero- or sign-extended, or truncated */
PsT ps_store(Ps *ps, PsT arr, PsT idx, PsT val);
PsT ps_forall(Ps *ps, PsT var, PsT body);

PsT ps_subst(Ps *ps, PsT t, PsT var, PsT repl);               /* t with variable var replaced */
bool ps_has(const Ps *ps, PsT t, int op);                     /* some node of t is op */

/* What the rewrite of bit-vector terms to Int owes: Bool terms that must hold for it to mean what the original meant. */
typedef struct PsInt {
    VEC(PsT) wrapbv;                    /* an add with no multiply under it does not wrap: a <=u ~b, over the original operands */
    VEC(PsT) wrapint;                   /* a multiply, or an add over one, stays below 2^64, as an Int fact */
    VEC(PsT) nonneg;                    /* the facts 0 <= x for the Int variables made */
} PsInt;
/* t as an Int-sorted (or Bool) term, 0 if it has something Int cannot say (arrays, bit-wise operations, quantifiers,
 * 8-bit values); the obligations are appended to obl. */
PsT ps_to_int(Ps *ps, PsT t, PsInt *obl);
void psint_free(PsInt *o);

void ps_print(const Ps *ps, PsT t, StrBuf *sb);

/* ---- the solver ---- */

typedef struct PsSolver PsSolver;
enum { PS_VALID, PS_INVALID, PS_UNKNOWN, PS_ERROR };

PsSolver *ps_open(const char *cmd, char *err, size_t errn);   /* cmd as symx's sx_open takes it (`z3 -in -smt2`) */
void ps_close(PsSolver *s);
/* Is goal true whenever all n hyps are?  One pipe serves many queries: variables are declared once, each
 * query lives in a push/pop.  rlimit is z3's deterministic budget (0: none). */
int ps_valid(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit);
/* The same, by the Int tier (docs/PROVER_DESIGN.md S7): needs a goal without a quantifier; hypotheses with one are
 * left out.  The rewrite is used only after proving that no add or multiply in it wraps, the bit-vector facts for
 * the linear ones and Int ones for those over a multiply, from the multiply-free hypotheses alone. */
int ps_valid_int(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit);

int prove_selftest(void);

#endif
