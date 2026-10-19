/* tcfg_int.h - the control-flow graph of one function (tcfg.c), as the code
 * generator (cgx86.c) reads it. */
#ifndef CEREAL_TCFG_INT_H
#define CEREAL_TCFG_INT_H

#include "c/tast_int.h"

enum { TK_NONE, TK_JMP, TK_BR, TK_SWTCH, TK_IJMP, TK_RET };

typedef struct CCase {
    int64_t v;
    uint32_t to;
} CCase;

typedef struct CBlk {
    U32Vec st;              /* the statement nodes, in order */
    VEC(CCase) cases;       /* swtch: the case values and their blocks */
    uint32_t val;           /* br, swtch, ijmp, ret: the value node, TN: none */
    uint32_t a, b;          /* jmp: a; br: a if true, else b; swtch: a = default */
    uint32_t pos;           /* placement order (the printed number), TN: unplaced */
    uint8_t term;
    bool used;              /* some edge leads here */
    bool reach;
} CBlk;

typedef struct LabEnt {
    uint32_t id, blk;
} LabEnt;

typedef struct Tgt {
    uint32_t brk, cont, sw; /* sw: the switch's dispatch block, TN: a loop */
} Tgt;

typedef struct Cfg {
    El *e;
    VEC(CBlk) b;
    VEC(LabEnt) labs;
    VEC(Tgt) tg;
    uint32_t cur;           /* the block being filled, TN: none (dead code) */
    uint32_t npl;
    uint8_t *hs;            /* per original node: control flow hides in its subtree */
    uint32_t n0;            /* the original nodes are 0 .. n0 - 1 */
    uint32_t nreg;          /* registers made so far (reg N, N < nreg) */
    long adj;               /* statements added (spill stores) less dropped */
    char why[96];
} Cfg;


static inline const TNode *nd(Cfg *L, uint32_t n)
{
    return &L->e->nodes.data[n];
}

static inline uint32_t kid(Cfg *L, uint32_t n, uint32_t j)
{
    return L->e->kids.data[nd(L, n)->kid0 + j];
}

/* cgx86.c: the function as x86-64 assembly (AT&T syntax) on out, or a
 * comment line saying why not.  ord lists the placed blocks in order. */
void x86_func(Cfg *L, const uint32_t *ord, FILE *out);

#endif
