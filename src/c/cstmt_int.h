/* cstmt_int.h - what cstmt.c and cstmt_warn.c share. */
#ifndef CEREAL_CSTMT_INT_H
#define CEREAL_CSTMT_INT_H

#include "c/check_int.h"

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)
#define NOB 0xFFFFFFFFu

typedef struct CBlock {
    uint32_t parent, depth;
    bool stmtexpr;           /* the block of a statement expression */
} CBlock;

/* A declaration a jump must not cross: variably modified, or (with
 * -Wjump-misses-init) an initialized automatic variable. */
typedef struct CUnsafe {
    uint32_t seq, block;
    SrcLoc loc;
    uint32_t name;
    bool vm;
    bool anon;               /* gcc's nameless declaration of a VM pointee */
} CUnsafe;

typedef struct CGoto {
    SrcLoc loc;
    uint32_t block, seq;
    uint32_t next;           /* next pending goto to the label + 1 */
} CGoto;

typedef struct CLabel {
    uint32_t name;
    uint32_t prev;           /* the binding it shadows + 1 */
    uint32_t block;          /* the scope it is bound in */
    uint32_t key;            /* node index: orders it among declarations */
    uint32_t fn;             /* function nesting depth */
    uint32_t def_seq, def_block;
    SrcLoc loc;
    uint32_t ghead, gtail;   /* pending gotos + 1 */
    bool defined, used, declared, emitted, popped;
} CLabel;

typedef struct CCase {
    uint64_t lo, hi;
    SrcLoc loc;
    bool range;
} CCase;

typedef struct CSwitch {
    uint32_t node, block;
    SrcLoc loc;
    TypeId ty;               /* the promoted type of the condition */
    TypeId orig;             /* the original type (ERRT: not integral) */
    bool cond_err;           /* the condition itself was erroneous */
    bool bool_cond;
    bool has_default;
    SrcLoc def_loc;
    uint32_t cbase;
    uint32_t cond;           /* the condition's node */
} CSwitch;

typedef struct FuncState {
    uint32_t label_base, block_base, unsafe_base, goto_base, sw_base,
        case_base;
    uint32_t ndeclared, seq, fblock;
    uint32_t sym;
    bool rv, rnull, abn;     /* return expr; / return; / a noreturn call */
} FuncState;

typedef struct CEnumSet {
    TypeId ty;
    uint32_t n;
    uint32_t *name;
    uint64_t *val;
    uint8_t *unused;     /* __attribute__((unused)) enumerators */
} CEnumSet;

typedef struct CStmt {
    VEC(CBlock) blocks;
    VEC(CUnsafe) unsafe;
    VEC(CGoto) gotos;
    VEC(CLabel) labels;
    VEC(CSwitch) sw;
    VEC(CCase) cases;
    VEC(FuncState) fs;
    VEC(uint32_t) tmp;
    uint32_t *lmap;          /* ident -> innermost label + 1 */
    uint32_t nmap;
    uint32_t cur;            /* the innermost open block, NOB: none */
    CEnumSet en;
} CStmt;
static inline unsigned tg(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

/* gcc's EXPR_LOCATION of an expression, as far as it matters (cexpr.c's
 * expr_loc). */
static inline uint32_t strip_paren(const Checker *c, uint32_t i)
{
    while (tg(c, i) == N_PAREN && c->nodes[i].size > 1)
        i--;
    return i;
}

static inline bool node_err(Checker *c, uint32_t i)
{
    return i == NOB || c->ty[i] == ERRT || c->ck[i] == K_ERR;
}

void misleading_scope_end(Checker *c, uint32_t i, uint32_t p);
void use_label(Checker *c, CStmt *s, uint32_t name, SrcLoc gloc,
    SrcLoc uloc, uint32_t key);
bool is_array_ty(Checker *c, TypeId t);
void stmt_return(Checker *c, uint32_t i);
void stmt_bc(Checker *c, uint32_t i, bool is_break);
void for_loop_decls(Checker *c, uint32_t fornode);
bool is_attr_kid(Checker *c, uint32_t k);
void label_at_end(Checker *c, uint32_t i);
void label_attrs(Checker *c, CStmt *s, uint32_t i);
void local_labels(Checker *c, CStmt *s, uint32_t i);
void stmt_struct_defined(Checker *c, CStmt *s, uint32_t i);
void stmt_declared(Checker *c, CStmt *s, uint32_t i);
void stmt_if(Checker *c, uint32_t i);

#endif
