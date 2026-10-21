/* tast_int.h - the typed AST's node and per-function state (tast.c, tcfg.c). */
#ifndef CEREAL_TAST_INT_H
#define CEREAL_TAST_INT_H

#include "c/tast.h"
#include "c/csymidx.h"

#define TN UINT32_MAX

#define TAST_OPS(X)                                                         \
    X(CONST, "const") X(FCONST, "fconst") X(SYM, "sym") X(TMP, "tmp") X(REG, "reg") X(COMPLIT, "complit") X(LOAD, "load") X(STORE, "store")        \
    X(CAST, "cast") X(UNOP, "unop") X(BINOP, "binop") X(CMP, "cmp")          \
    X(LOGAND, "logand") X(LOGOR, "logor") X(COND, "cond")                    \
    X(ELVIS, "elvis") X(COMMA, "comma") X(CALL, "call")                      \
    X(INTRIN, "intrin") X(F2I, "f2i") X(VA, "va") X(LABADDR, "labaddr")                                        \
    X(DEREF, "deref") X(FIELD, "field") X(RFIELD, "rfield") X(ADDR, "addr") X(STR, "str")        \
    X(PTRADD, "ptradd") X(PTRDIFF, "ptrdiff") X(STMTEXPR, "stmtexpr")        \
    X(POST, "post")                                                          \
    X(BLOCK, "block") X(LOCAL, "local") X(EXPRSTMT, "exprstmt")              \
    X(IF, "if") X(WHILE, "while") X(DO, "do") X(FOR, "for")                  \
    X(SWITCH, "switch") X(CASE, "case") X(DEFAULT, "default")                \
    X(BREAK, "break") X(CONTINUE, "continue") X(GOTO, "goto") X(IGOTO, "igoto")          \
    X(LABEL, "label") X(RETURN, "return") X(NOP, "nop") X(ASM, "asm")                     \
    X(ELEM, "elem") X(DATA, "data")

typedef enum {
#define X(n, s) TO_##n,
    TAST_OPS(X)
#undef X
    TO_COUNT
} TastOp;

/* Nodes up to and including TO_POST are expressions: they print a type. */
#define IS_EXPR(op) ((op) <= TO_POST)

typedef struct TNode {
    uint8_t op;
    uint16_t pun;           /* unop/binop/cmp: the operator (Punct); field: bit
                             * offset within byte val; call: 1 when indirect;
                             * 2 never (intrin has its own op); elem: bit
                             * offset within byte val; str: unit size */
    TypeId ty;
    SrcLoc loc;
    uint32_t kid0, nkids;   /* kids: El.kids[kid0 .. kid0 + nkids) */
    int64_t val;            /* const: value; case: value; goto/label: ident id;
                             * field: byte offset; ptradd/ptrdiff: element size;
                             * call, intrin: arguments converted to parameter
                             * types (the rest get the default promotions);
                             * elem: byte offset; str, fconst: offset in
                             * El.bytes */
    uint32_t sym;           /* sym, local, call: the symbol ref; intrin: the
                             * identifier id of the builtin's name */
    uint32_t width;         /* field: bit-field width, 0 otherwise; call,
                             * intrin: 1 when the callee is variadic or
                             * unprototyped; elem: bit-field width, 0 whole;
                             * str: byte length including the terminator;
                             * fconst: byte length (the type's size) */
} TNode;

typedef struct El {
    Checker *c;
    VEC(TNode) nodes;
    VEC(uint32_t) kids;
    TypeId ret;             /* the function's return type */
    TypeId swty;            /* the innermost switch's promoted type, 0: none */
    VEC(uint8_t) bytes;     /* the code units of the str nodes */
    U32Vec params;          /* the function's N_PARAM nodes, outermost, in order (not f(void)) */
    uint32_t fname;         /* the function's name (identifier id), when in one */
    uint32_t fsym;          /* its symbol ref (the code generator reads its linkage) */
    uint32_t ntmp;          /* temporaries made so far (tmp N, N < ntmp) */
    uint32_t ncl;           /* compound literals made so far (complit N) */
    bool infunc;
    char why[96];           /* non-empty: unsupported, and the reason */
} El;

/* Prints node i as one dump line (tast.c). */
void tast_print_node(El *e, FILE *out, size_t i);

/* Prints the unit's control-flow graph and, with c->opt.verify_cfg, checks it
 * (tcfg.c, docs/TAST_T8_PLAN.md).  root is the function body's block. */
void tast_cfg(El *e, uint32_t root, FILE *out);


/* ---- shared by tast.c, tast_vec.c and tast_x86.c ---------------------------- */
typedef struct VOp {
    TypeId vt, et;          /* the vector type the operation sees, and its lane type */
    TypeId st;              /* the storage's type: casts between vector types are looked through */
    uint32_t x;             /* the checker's node, named again for every lane */
    uint32_t tmp;           /* UINT32_MAX, else the temporary that holds the value */
    bool scalar;            /* a scalar operand: every lane is its value */
} VOp;

uint32_t tunsup(El *e, const char *fmt, ...);
uint32_t mk(El *e, int op, TypeId ty, SrcLoc loc, int64_t val, uint32_t sym, unsigned pun, const uint32_t *k, uint32_t nk);
uint32_t intrin(El *e, const char *nm, TypeId rt, const uint32_t *args, uint32_t n, SrcLoc loc);
bool ok_int(El *e, TypeId t);
TypeId nty(El *e, uint32_t id);
uint32_t cnst(El *e, TypeId t, uint64_t v, SrcLoc loc);
uint32_t conv(El *e, uint32_t id, TypeId to, SrcLoc loc);
uint32_t rv(El *e, uint32_t n);
uint32_t load(El *e, uint32_t symid);
uint32_t addr_t(El *e, uint32_t l, TypeId pty, SrcLoc loc);
uint32_t deref_t(El *e, uint32_t p, TypeId t, SrcLoc loc);
uint32_t ptradd(El *e, TypeId pty, uint32_t p, uint32_t x, bool neg, SrcLoc loc);
uint32_t lval(El *e, uint32_t n);
uint32_t arith(El *e, uint32_t i, int op, uint32_t a, uint32_t b, SrcLoc loc, TypeId *rt);
bool vec_dims(El *e, TypeId vt, TypeId *et, uint32_t *n);
bool vop_val(El *e, uint32_t v, TypeId vt, VOp *o, U32Vec *setup, SrcLoc loc);
bool vop_node(El *e, uint32_t k, TypeId vt, VOp *o, U32Vec *setup, SrcLoc loc);
uint32_t vop_lane(El *e, const VOp *o, uint32_t j, SrcLoc loc);
uint32_t vec_wrap(El *e, uint32_t v, TypeId t, const U32Vec *setup, SrcLoc loc);
uint32_t vec_make(El *e, TypeId vt, SrcLoc loc, const uint32_t *lanes, uint32_t n, const U32Vec *setup);
uint32_t vec_op(El *e, int p, TypeId vt, const VOp *oa, const VOp *ob, SrcLoc loc, U32Vec *setup);
uint32_t vec_binary(El *e, uint32_t i, int p, uint32_t a, uint32_t b, SrcLoc loc);
uint32_t vec_unary(El *e, uint32_t i, int p, uint32_t a, SrcLoc loc);
bool vec_bitcast(El *e, TypeId to, uint32_t k);
uint32_t vec_bits(El *e, uint32_t k, TypeId to, SrcLoc loc);
uint32_t vec_convert(El *e, uint32_t i, uint32_t a, SrcLoc loc);
uint32_t vec_shuffle(El *e, uint32_t i, const uint32_t *k, uint32_t nk, bool fixed, SrcLoc loc);
TypeId ia_ty(int l);
typedef uint32_t (*LaneFn)(El *e, void *ctx, uint32_t j);
uint32_t vmap(El *e, TypeId vt, SrcLoc loc, const U32Vec *setup, LaneFn f, void *ctx);
uint32_t vec_un(El *e, TypeId vt, int p, bool step, const VOp *oa, SrcLoc loc, U32Vec *setup);
uint32_t fconst(El *e, TypeId t, long double v, SrcLoc loc);
uint32_t vl_bin(El *e, int p, TypeId t, uint32_t a, uint32_t b, SrcLoc loc);
uint32_t vl_un(El *e, int p, TypeId t, uint32_t a, SrcLoc loc);
uint32_t vl_mask(El *e, int p, TypeId t, uint32_t a, uint32_t b, SrcLoc loc);
uint32_t vl_cmp(El *e, int p, uint32_t a, uint32_t b, SrcLoc loc);
uint32_t ia32_call(El *e, const char *nm, const U32Vec *args, TypeId rt, SrcLoc loc);

#endif
