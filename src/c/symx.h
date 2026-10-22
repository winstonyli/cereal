/* symx.h - expressions over a call's input bytes and the solver that answers questions
 * about them (docs/CONCOLIC_DESIGN.md).
 *
 * A value is a node of a DAG of bit-vector operations: 64-bit slots (as the evaluator keeps
 * them, canonical for their type), 8-bit input symbols and bytes of slots.  A node id is
 * an index + 1; 0 means "concrete".  The solver side takes constraints (a node and whether
 * it must be non-zero) and returns bytes for the symbols; the SMT-LIB2 backend drives any
 * solver that reads it on stdin (`z3 -in -smt2`, cvc5, bitwuzla, yices), and an in-repo
 * backend can implement the same four functions on the same DAG. */
#ifndef CEREAL_SYMX_H
#define CEREAL_SYMX_H

#include "common.h"

enum {
    SX_SYM,                     /* k: the input byte's offset; 8 bits */
    SX_CONST,                   /* k: the value; w bits */
    SX_BYTE,                    /* byte k of a (a is 64 bits); 8 bits */
    SX_JOIN,                    /* a (high) and b (low) side by side */
    SX_EXT,                     /* the low bytes of a, as ev_canon does: k = bytes | signed << 8 | boolean << 9 */
    SX_ADD, SX_SUB, SX_MUL, SX_UDIV, SX_SDIV, SX_UREM, SX_SREM,
    SX_AND, SX_OR, SX_XOR, SX_SHL, SX_LSHR, SX_ASHR,
    SX_NEG, SX_NOT, SX_LNOT,    /* unary, on 64 bits */
    SX_EQ, SX_NE, SX_ULT, SX_ULE, SX_SLT, SX_SLE   /* 64-bit 0 or 1 */
};

typedef struct SxNode {
    uint8_t op;
    uint8_t w;                  /* width in bits: 8, 16, 32 or 64 (a join of bytes) */
    uint32_t a, b;
    uint64_t k;
} SxNode;

typedef struct Sx {
    VEC(SxNode) n;
} Sx;

void sx_free(Sx *sx);
void sx_clear(Sx *sx);
uint32_t sx_sym(Sx *sx, uint32_t off);
uint32_t sx_const(Sx *sx, uint8_t w, uint64_t v);
uint32_t sx_byte(Sx *sx, uint32_t a, unsigned j);
uint32_t sx_join(Sx *sx, uint32_t hi, uint32_t lo);
uint32_t sx_ext(Sx *sx, uint32_t a, unsigned bytes, bool sg, bool boolean);
uint32_t sx_op(Sx *sx, int op, uint32_t a, uint32_t b);       /* a 64-bit operation (b: 0 for the unary ones) */

/* ---- the solver ---- */

typedef struct SxSolver SxSolver;

enum { SX_SAT, SX_UNSAT, SX_UNKNOWN, SX_ERROR };

typedef struct SxModel {
    VEC(uint32_t) off;          /* the symbols mentioned so far, by input offset ... */
    VEC(uint8_t) val;           /* ... and their values in the model */
} SxModel;

/* Starts the command (run by sh -c) and checks it with a probe query; NULL, with the reason in
 * err, if it does not start or does not answer. */
SxSolver *sx_open(const char *cmd, char *err, size_t errn);
void sx_close(SxSolver *s);
/* Resource limit per query for the next sx_begin (0: none); a deterministic budget, not time. */
void sx_limit(SxSolver *s, unsigned long rlimit);
/* A new problem over the nodes of sx: forgets everything asserted. */
void sx_begin(SxSolver *s, const Sx *sx);
/* Asserts that node a is non-zero (want) or zero (!want), for good. */
bool sx_assert(SxSolver *s, uint32_t a, bool want);
typedef struct SxCond {
    uint32_t a;
    bool want;                  /* node a is non-zero (want) or zero */
} SxCond;

/* Asks whether the asserted constraints and the n conditions hold together, without keeping
 * the conditions; a model fills m (cleared first) for SX_SAT. */
int sx_try(SxSolver *s, const SxCond *c, size_t n, SxModel *m);
void sx_model_free(SxModel *m);

#endif
