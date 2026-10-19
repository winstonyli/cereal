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
    X(INTRIN, "intrin") X(VA, "va") X(LABADDR, "labaddr")                                        \
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

#endif
