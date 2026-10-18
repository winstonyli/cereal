/* tast.c - the typed AST export, stage T1 (docs/TAST_T1_PLAN.md).
 *
 * The checker has already typed every node (Checker.ty, cv, ck, ef) and
 * recorded which symbol each identifier names (Checker.sym).  This pass
 * does not re-derive types: it reads them, calls the checker's own
 * conversion helpers (promoted, common_type) to find the type an operator
 * needs, and inserts a cast node wherever an operand's type differs.  A
 * construct outside the T1 subset sets El.why and the whole unit prints as
 * unsupported. */
#include "c/tast.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "c/cexpr_int.h"

#define TN UINT32_MAX

#define TAST_OPS(X)                                                         \
    X(CONST, "const") X(SYM, "sym") X(LOAD, "load") X(STORE, "store")        \
    X(CAST, "cast") X(UNOP, "unop") X(BINOP, "binop") X(CMP, "cmp")          \
    X(LOGAND, "logand") X(LOGOR, "logor") X(COND, "cond")                    \
    X(ELVIS, "elvis") X(COMMA, "comma") X(CALL, "call") X(POST, "post")      \
    X(BLOCK, "block") X(LOCAL, "local") X(EXPRSTMT, "exprstmt")              \
    X(IF, "if") X(WHILE, "while") X(DO, "do") X(FOR, "for")                  \
    X(SWITCH, "switch") X(CASE, "case") X(DEFAULT, "default")                \
    X(BREAK, "break") X(CONTINUE, "continue") X(GOTO, "goto")                \
    X(LABEL, "label") X(RETURN, "return") X(NOP, "nop")

typedef enum {
#define X(n, s) TO_##n,
    TAST_OPS(X)
#undef X
    TO_COUNT
} TastOp;

static const char *const op_names[TO_COUNT] = {
#define X(n, s) s,
    TAST_OPS(X)
#undef X
};

/* Nodes up to and including TO_POST are expressions: they print a type. */
#define IS_EXPR(op) ((op) <= TO_POST)

typedef struct TNode {
    uint8_t op;
    uint16_t pun;           /* unop/binop/cmp: the operator (Punct) */
    TypeId ty;
    SrcLoc loc;
    uint32_t kid0, nkids;   /* kids: El.kids[kid0 .. kid0 + nkids) */
    int64_t val;            /* const: value; case: value; goto/label: ident id */
    uint32_t sym;           /* sym, local, call: the symbol ref */
} TNode;

typedef struct El {
    Checker *c;
    VEC(TNode) nodes;
    VEC(uint32_t) kids;
    TypeId ret;             /* the function's return type */
    TypeId swty;            /* the innermost switch's promoted type, 0: none */
    char why[96];           /* non-empty: unsupported, and the reason */
} El;

static uint32_t unsup(El *e, const char *fmt, ...)
{
    if (!e->why[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->why, sizeof e->why, fmt, ap);
        va_end(ap);
    }
    return TN;
}

static uint32_t mk(El *e, int op, TypeId ty, SrcLoc loc, int64_t val,
                   uint32_t sym, unsigned pun, const uint32_t *k, uint32_t nk)
{
    TNode n;
    uint32_t j;
    memset(&n, 0, sizeof n);
    n.op = (uint8_t)op;
    n.pun = (uint16_t)pun;
    n.ty = ty;
    n.loc = loc;
    n.val = val;
    n.sym = sym;
    n.kid0 = (uint32_t)e->kids.len;
    n.nkids = nk;
    for (j = 0; j < nk; j++)
        vec_push(&e->kids, k[j]);
    vec_push(&e->nodes, n);
    return (uint32_t)e->nodes.len - 1;
}

static bool ok_int(El *e, TypeId t)
{
    TypeKind k = type_ckind(&e->c->tt, t);
    return (k >= TY_BOOL && k <= TY_ULLONG) || k == TY_ENUM;
}

/* Why a type is outside the subset, for the reason histogram. */
static const char *why_type(El *e, TypeId t)
{
    switch (type_ckind(&e->c->tt, t)) {
    case TY_PTR: return "pointer type";
    case TY_ARRAY: case TY_VLA: return "array type";
    case TY_FUNC: return "function designator";
    case TY_STRUCT: case TY_UNION: return "struct/union type";
    case TY_COMPLEX: return "complex type";
    case TY_VECTOR: return "vector type";
    case TY_INT128: case TY_UINT128: return "__int128 type";
    case TY_VOID: return "void value";
    default: return "floating type";
    }
}

static TypeId nty(El *e, uint32_t id)
{
    return e->nodes.data[id].ty;
}

static uint32_t cnst(El *e, TypeId t, uint64_t v, SrcLoc loc)
{
    t = unqual(e->c, t);
    return mk(e, TO_CONST, t, loc, (int64_t)cexpr_trunc(e->c, t, v), 0, 0, NULL, 0);
}

/* id converted to type `to` (a no-op when the types are the same; a
 * constant folds, as gcc does). */
static uint32_t conv(El *e, uint32_t id, TypeId to, SrcLoc loc)
{
    Checker *c = e->c;
    const TNode *nd;
    if (id == TN)
        return TN;
    to = unqual(c, to);
    if (mainv(c, nty(e, id)) == mainv(c, to))
        return id;
    nd = &e->nodes.data[id];
    if (nd->op == TO_CONST) {   /* nothing else refers to it: retype in place */
        e->nodes.data[id].val = (int64_t)cexpr_trunc(c, to, (uint64_t)nd->val);
        e->nodes.data[id].ty = to;
        return id;
    }
    return mk(e, TO_CAST, to, loc, 0, 0, 0, &id, 1);
}

/* The checker's result type must be the one the rule gives. */
static bool agree(El *e, uint32_t i, TypeId rule)
{
    Checker *c = e->c;
    if (mainv(c, rule) == mainv(c, c->ty[i]))
        return true;
    unsup(e, "rule disagreement (%s)", node_names[ntag(c, i)]);
    return false;
}

static uint32_t ex(El *e, uint32_t n, bool allow_void);

static uint32_t rv(El *e, uint32_t n)
{
    return ex(e, n, false);
}

/* The lvalue of a plain variable. */
static uint32_t lv(El *e, uint32_t n)
{
    Checker *c = e->c;
    uint32_t i = strip_paren(c, n), ref;
    const CSym *s;
    if (e->why[0])
        return TN;
    if (ntag(c, i) != N_IDENT || !(c->ef[i] & EF_LVALUE))
        return unsup(e, "lvalue form");
    ref = c->sym[i];
    if (ref == SYM_NONE || csym(c, ref)->kind != CS_OBJ)
        return unsup(e, "lvalue form");
    s = csym(c, ref);
    if (!ok_int(e, s->ty))
        return unsup(e, "%s", why_type(e, s->ty));
    return mk(e, TO_SYM, s->ty, cnode_loc(c, i), 0, ref, 0, NULL, 0);
}

static uint32_t load(El *e, uint32_t symid)
{
    uint32_t id = symid;
    if (symid == TN)
        return TN;
    return mk(e, TO_LOAD, unqual(e->c, nty(e, symid)), e->nodes.data[symid].loc,
              0, 0, 0, &id, 1);
}

static int assign_op(int p)
{
    switch (p) {
    case P_MUL_ASSIGN: return P_STAR;
    case P_DIV_ASSIGN: return P_SLASH;
    case P_MOD_ASSIGN: return P_PERCENT;
    case P_ADD_ASSIGN: return P_PLUS;
    case P_SUB_ASSIGN: return P_MINUS;
    case P_SHL_ASSIGN: return P_SHL;
    case P_SHR_ASSIGN: return P_SHR;
    case P_AND_ASSIGN: return P_AMP;
    case P_XOR_ASSIGN: return P_CARET;
    case P_OR_ASSIGN: return P_PIPE;
    default: return 0;
    }
}

/* a op b with the operands converted as the operator requires; the type of
 * the result is returned in *rt.  The caller supplies the checker's node
 * for the type check (0: none). */
static uint32_t arith(El *e, uint32_t i, int op, uint32_t a, uint32_t b,
                      SrcLoc loc, TypeId *rt)
{
    Checker *c = e->c;
    TypeId lt, rtt, ct;
    uint32_t ka[2];
    uint32_t x, y;
    a = strip_paren(c, a);
    b = strip_paren(c, b);
    x = rv(e, a);
    y = rv(e, b);
    if (x == TN || y == TN)
        return TN;
    if (op == P_SHL || op == P_SHR) {
        lt = promoted(c, a);
        rtt = promoted(c, b);
        ka[0] = conv(e, x, lt, loc);
        ka[1] = conv(e, y, rtt, loc);
        ct = lt;
    } else {
        ct = common_type(c, promoted(c, a), promoted(c, b));
        ka[0] = conv(e, x, ct, loc);
        ka[1] = conv(e, y, ct, loc);
    }
    if (i != NO_NODE && !agree(e, i, ct))
        return TN;
    *rt = ct;
    return mk(e, TO_BINOP, unqual(c, ct), loc, 0, 0, (unsigned)op, ka, 2);
}

/* ++x, --x, x++, x-- as load, add, convert back, store. */
static uint32_t incdec(El *e, uint32_t i, uint32_t x, int punct, bool post)
{
    Checker *c = e->c;
    TypeId xt, op;
    uint32_t l1, l2, a, one, b, v, st, ka[2];
    SrcLoc loc = cnode_loc(c, i);
    x = strip_paren(c, x);
    xt = unqual(c, c->ty[x]);
    if (!ok_int(e, xt))
        return unsup(e, "%s", why_type(e, xt));
    op = common_type(c, promoted(c, x), TYPE_B(INT));
    l1 = lv(e, x);
    l2 = lv(e, x);
    if (l1 == TN || l2 == TN)
        return TN;
    a = conv(e, load(e, l2), op, loc);
    one = cnst(e, op, 1, loc);
    ka[0] = a;
    ka[1] = one;
    b = mk(e, TO_BINOP, unqual(c, op), loc, 0, 0,
           punct == P_INC ? P_PLUS : P_MINUS, ka, 2);
    v = conv(e, b, xt, loc);
    ka[0] = l1;
    ka[1] = v;
    st = mk(e, TO_STORE, xt, loc, 0, 0, 0, ka, 2);
    return post ? mk(e, TO_POST, xt, loc, 0, 0, 0, &st, 1) : st;
}

static uint32_t e_assign_t(El *e, uint32_t i)
{
    Checker *c = e->c;
    uint32_t k[2], lhs, rhs, l1, ka[2], v;
    int p = npunct(c, i), op = assign_op(p);
    TypeId lt;
    SrcLoc loc = cnode_loc(c, i);
    if (nkids(c, i, k, 2) != 2)
        return unsup(e, "malformed assignment");
    lhs = strip_paren(c, k[0]);
    rhs = strip_paren(c, k[1]);
    lt = unqual(c, c->ty[lhs]);
    if (!ok_int(e, lt))
        return unsup(e, "%s", why_type(e, lt));
    l1 = lv(e, lhs);
    if (l1 == TN)
        return TN;
    if (p == P_ASSIGN) {
        v = conv(e, rv(e, rhs), lt, loc);
    } else if (op) {
        /* x op= y is x = (T)((op type)x op (op type)y); the lvalue is a
         * plain variable, so naming it twice evaluates nothing twice. */
        TypeId ot;
        uint32_t l2 = lv(e, lhs), b;
        uint32_t x, y, kb[2];
        if (l2 == TN)
            return TN;
        x = load(e, l2);
        y = rv(e, rhs);
        if (y == TN)
            return TN;
        if (op == P_SHL || op == P_SHR) {
            ot = promoted(c, lhs);
            kb[1] = conv(e, y, promoted(c, rhs), loc);
        } else {
            ot = common_type(c, promoted(c, lhs), promoted(c, rhs));
            kb[1] = conv(e, y, ot, loc);
        }
        kb[0] = conv(e, x, ot, loc);
        b = mk(e, TO_BINOP, unqual(c, ot), loc, 0, 0, (unsigned)op, kb, 2);
        v = conv(e, b, lt, loc);
    } else {
        return unsup(e, "assignment operator");
    }
    if (v == TN)
        return TN;
    ka[0] = l1;
    ka[1] = v;
    return mk(e, TO_STORE, lt, loc, 0, 0, 0, ka, 2);
}

static uint32_t e_call_t(El *e, uint32_t i)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, i, NULL, 0), *k, callee, ref, j, np;
    const CSym *s;
    TypeId ft;
    const TypeEnt *fe;
    VEC(uint32_t) args = {0};
    uint32_t id = TN;
    SrcLoc loc = cnode_loc(c, i);
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, i, k, nk);
    callee = strip_paren(c, k[0]);
    if (ntag(c, callee) != N_IDENT) {
        unsup(e, "indirect call");
        goto out;
    }
    ref = c->sym[callee];
    if (ref == SYM_NONE) {
        unsup(e, "builtin or undeclared call");
        goto out;
    }
    s = csym(c, ref);
    if (s->kind != CS_FUNC) {
        unsup(e, "indirect call");
        goto out;
    }
    ft = type_canon(&c->tt, s->ty);
    fe = type_ent(&c->tt, ft);
    if (fe->flags & (TF_NOPROTO | TF_VARIADIC)) {
        unsup(e, "call to unprototyped or variadic function");
        goto out;
    }
    np = (uint32_t)fe->n;
    if (np != nk - 1) {
        unsup(e, "call argument count");
        goto out;
    }
    for (j = 0; j < np; j++) {
        TypeId pt = unqual(c, type_params(&c->tt, ft)[j]);
        uint32_t a, ca;
        if (!ok_int(e, pt)) {
            unsup(e, "%s", why_type(e, pt));
            goto out;
        }
        a = strip_paren(c, k[1 + j]);
        ca = conv(e, rv(e, a), pt, loc);
        if (ca == TN)
            goto out;
        vec_push(&args, ca);
    }
    id = mk(e, TO_CALL, unqual(c, c->ty[i]), loc, 0, ref, 0, args.data, (uint32_t)args.len);
out:
    vec_free(&args);
    free(k);
    return id;
}

static uint32_t ex(El *e, uint32_t n, bool allow_void)
{
    Checker *c = e->c;
    uint32_t i = strip_paren(c, n), k[3], ref;
    TypeId t;
    SrcLoc loc;
    bool isvoid;
    if (e->why[0])
        return TN;
    loc = cnode_loc(c, i);
    t = c->ty[i];
    if (c->ck[i] == K_ERR || type_ckind(&c->tt, t) == TY_ERROR)
        return unsup(e, "type error");
    isvoid = type_is_void(&c->tt, t);
    if (isvoid) {
        if (!allow_void)
            return unsup(e, "void value");
    } else {
        t = rvt(c, i);
        if (!ok_int(e, t))
            return unsup(e, "%s", why_type(e, t));
        if ((c->ck[i] == K_ICE || c->ck[i] == K_FOLD) && !(c->ef[i] & EF_SIDE))
            return cnst(e, t, c->cv[i], loc);
    }
    switch (ntag(c, i)) {
    case N_IDENT: {
        uint32_t sid;
        ref = c->sym[i];
        if (ref == SYM_NONE)
            return unsup(e, "builtin or undeclared identifier");
        if (csym(c, ref)->kind != CS_OBJ)
            return unsup(e, "identifier is not a variable");
        sid = lv(e, i);
        return load(e, sid);
    }
    case N_CALL:
        return e_call_t(e, i);
    case N_CAST: {
        uint32_t x;
        if (nkids(c, i, k, 2) != 2)
            return unsup(e, "malformed cast");
        if (isvoid) {
            x = ex(e, k[1], true);
            return x == TN ? TN : mk(e, TO_CAST, t, loc, 0, 0, 0, &x, 1);
        }
        return conv(e, rv(e, k[1]), t, loc);
    }
    case N_UNARY: {
        const Tok *tk = cnode_tok(c, i);
        int p;
        uint32_t x, a;
        if (tk->kind != TK_PUNCT)       /* __extension__ */
            return ex(e, first_child(c, i), allow_void);
        p = npunct(c, i);
        if (p == P_INC || p == P_DEC)
            return incdec(e, i, first_child(c, i), p, false);
        if (p != P_PLUS && p != P_MINUS && p != P_TILDE && p != P_BANG)
            return unsup(e, "unary %s", punct_spelling[p]);
        a = first_child(c, i);
        x = rv(e, a);
        if (x == TN)
            return TN;
        if (p == P_BANG)
            return mk(e, TO_UNOP, t, loc, 0, 0, P_BANG, &x, 1);
        if (!agree(e, i, promoted(c, a)))
            return TN;
        x = conv(e, x, t, loc);
        if (p == P_PLUS)
            return x;
        return mk(e, TO_UNOP, t, loc, 0, 0, (unsigned)p, &x, 1);
    }
    case N_POSTFIX:
        return incdec(e, i, first_child(c, i), npunct(c, i), true);
    case N_ASSIGN:
        return e_assign_t(e, i);
    case N_COND: {
        uint32_t nk = nkids(c, i, k, 3), cd, x, y, ka[3];
        if (c->nodes[i].flags & NF_OMITTED) {
            if (nk != 2)
                return unsup(e, "malformed ?:");
            x = rv(e, k[0]);
            y = ex(e, k[1], isvoid);
            if (x == TN || y == TN)
                return TN;
            ka[0] = x;
            ka[1] = isvoid ? y : conv(e, y, t, loc);
            return mk(e, TO_ELVIS, t, loc, 0, 0, 0, ka, 2);
        }
        if (nk != 3)
            return unsup(e, "malformed ?:");
        cd = rv(e, k[0]);
        x = ex(e, k[1], isvoid);
        y = ex(e, k[2], isvoid);
        if (cd == TN || x == TN || y == TN)
            return TN;
        ka[0] = cd;
        ka[1] = isvoid ? x : conv(e, x, t, loc);
        ka[2] = isvoid ? y : conv(e, y, t, loc);
        return mk(e, TO_COND, t, loc, 0, 0, 0, ka, 3);
    }
    case N_BINARY: {
        int p = npunct(c, i);
        uint32_t ka[2], x, y;
        TypeId rt;
        if (nkids(c, i, k, 2) != 2)
            return unsup(e, "malformed binary");
        switch (p) {
        case P_COMMA:
            x = ex(e, k[0], true);
            y = ex(e, k[1], isvoid);
            if (x == TN || y == TN)
                return TN;
            ka[0] = x;
            ka[1] = y;
            return mk(e, TO_COMMA, t, loc, 0, 0, 0, ka, 2);
        case P_ANDAND: case P_OROR:
            x = rv(e, k[0]);
            y = rv(e, k[1]);
            if (x == TN || y == TN)
                return TN;
            ka[0] = x;
            ka[1] = y;
            return mk(e, p == P_ANDAND ? TO_LOGAND : TO_LOGOR, t, loc, 0, 0, 0, ka, 2);
        case P_LT: case P_GT: case P_LE: case P_GE: case P_EQEQ: case P_NE: {
            TypeId ct = common_type(c, promoted(c, k[0]), promoted(c, k[1]));
            x = rv(e, k[0]);
            y = rv(e, k[1]);
            if (x == TN || y == TN)
                return TN;
            ka[0] = conv(e, x, ct, loc);
            ka[1] = conv(e, y, ct, loc);
            return mk(e, TO_CMP, t, loc, 0, 0, (unsigned)p, ka, 2);
        }
        case P_PLUS: case P_MINUS: case P_STAR: case P_SLASH: case P_PERCENT:
        case P_AMP: case P_PIPE: case P_CARET: case P_SHL: case P_SHR:
            return arith(e, i, p, k[0], k[1], loc, &rt);
        default:
            return unsup(e, "binary %s", punct_spelling[p]);
        }
    }
    default:
        return unsup(e, "expression %s", node_names[ntag(c, i)]);
    }
}

/* ---- statements ---------------------------------------------------------- */

static uint32_t st(El *e, uint32_t n);

/* The children of n that are not scope markers, in order. */
static uint32_t fkids(El *e, uint32_t n, uint32_t **out)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, n, NULL, 0), *all, j, m = 0;
    all = xmalloc((nk ? nk : 1) * sizeof *all);
    nkids(c, n, all, nk);
    for (j = 0; j < nk; j++) {
        unsigned t = ntag(c, all[j]);
        if (t != N_SCOPE && t != N_SCOPE_END && t != N_BODY)
            all[m++] = all[j];
    }
    *out = all;
    return m;
}

static uint32_t nop(El *e, SrcLoc loc)
{
    return mk(e, TO_NOP, 0, loc, 0, 0, 0, NULL, 0);
}

/* The DECLARED marker among the children of an INIT_DECL (or FUNC_DEF). */
static uint32_t declared_of(El *e, uint32_t n, uint32_t *after)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, n, NULL, 0), *k, j, r = NO_NODE;
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, n, k, nk);
    for (j = 0; j < nk; j++)
        if (ntag(c, k[j]) == N_DECLARED) {
            r = k[j];
            if (after)
                *after = j + 1 < nk ? k[j + 1] : NO_NODE;
            break;
        }
    free(k);
    return r;
}

/* The local variables a block-scope declaration introduces. */
static void decl(El *e, uint32_t n, U32Vec *out)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, n, NULL, 0), *k, j;
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, n, k, nk);
    for (j = 0; j < nk && !e->why[0]; j++) {
        uint32_t d, init, ref, ik[1], id;
        const CSym *s;
        SrcLoc loc;
        if (ntag(c, k[j]) != N_INIT_DECL)
            continue;
        d = declared_of(e, k[j], &init);
        if (d == NO_NODE || !c->cb[d]) {
            unsup(e, "declaration without a symbol");
            break;
        }
        ref = c->cb[d] - 1;
        s = csym(c, ref);
        loc = cnode_loc(c, k[j]);
        if (s->kind == CS_TYPEDEF)
            continue;
        if (s->kind == CS_FUNC) {
            unsup(e, "local function declaration");
            break;
        }
        if (s->sc == SC_STATIC || s->sc == SC_EXTERN) {
            unsup(e, "static or extern local");
            break;
        }
        if (!ok_int(e, s->ty)) {
            unsup(e, "%s", why_type(e, s->ty));
            break;
        }
        if (init != NO_NODE) {
            if (ntag(c, init) == N_INIT_LIST) {
                unsup(e, "braced initializer");
                break;
            }
            ik[0] = conv(e, rv(e, init), s->ty, loc);
            if (ik[0] == TN)
                break;
            id = mk(e, TO_LOCAL, s->ty, loc, 0, ref, 0, ik, 1);
        } else {
            id = mk(e, TO_LOCAL, s->ty, loc, 0, ref, 0, NULL, 0);
        }
        vec_push(out, id);
    }
    free(k);
}

static uint32_t block_of(El *e, U32Vec *v, SrcLoc loc)
{
    uint32_t id = mk(e, TO_BLOCK, 0, loc, 0, 0, 0, v->data, (uint32_t)v->len);
    vec_free(v);
    return id;
}

static uint32_t st(El *e, uint32_t n)
{
    Checker *c = e->c;
    uint32_t *k = NULL, m, id = TN, ka[4], j;
    SrcLoc loc = cnode_loc(c, n);
    if (e->why[0])
        return TN;
    switch (ntag(c, n)) {
    case N_COMPOUND: {
        U32Vec v = {0};
        m = fkids(e, n, &k);
        for (j = 0; j < m && !e->why[0]; j++) {
            if (ntag(c, k[j]) == N_DECL)
                decl(e, k[j], &v);
            else if (ntag(c, k[j]) != N_STATIC_ASSERT)
                vec_push(&v, st(e, k[j]));
        }
        id = block_of(e, &v, loc);
        break;
    }
    case N_DECL: {
        U32Vec v = {0};
        decl(e, n, &v);
        id = block_of(e, &v, loc);
        break;
    }
    case N_EXPR_STMT: {
        uint32_t x;
        m = fkids(e, n, &k);
        if (m == 0) {
            id = nop(e, loc);
            break;
        }
        x = ex(e, k[0], true);
        id = x == TN ? TN : mk(e, TO_EXPRSTMT, 0, loc, 0, 0, 0, &x, 1);
        break;
    }
    case N_IF: {
        m = fkids(e, n, &k);
        if (m < 2) {
            unsup(e, "malformed if");
            break;
        }
        ka[0] = rv(e, k[0]);
        ka[1] = st(e, k[1]);
        if (m > 2)
            ka[2] = st(e, k[2]);
        if (ka[0] != TN && ka[1] != TN && (m < 3 || ka[2] != TN))
            id = mk(e, TO_IF, 0, loc, 0, 0, 0, ka, m > 2 ? 3 : 2);
        break;
    }
    case N_WHILE: case N_DO: {
        bool dw = ntag(c, n) == N_DO;
        m = fkids(e, n, &k);
        if (m != 2) {
            unsup(e, "malformed loop");
            break;
        }
        if (dw) {
            ka[0] = st(e, k[0]);
            ka[1] = rv(e, k[1]);
        } else {
            ka[0] = rv(e, k[0]);
            ka[1] = st(e, k[1]);
        }
        if (ka[0] != TN && ka[1] != TN)
            id = mk(e, dw ? TO_DO : TO_WHILE, 0, loc, 0, 0, 0, ka, 2);
        break;
    }
    case N_FOR: {
        m = fkids(e, n, &k);
        if (m != 4) {
            unsup(e, "malformed for");
            break;
        }
        if (ntag(c, k[0]) == N_NONE) {
            ka[0] = nop(e, loc);
        } else if (ntag(c, k[0]) == N_DECL) {
            ka[0] = st(e, k[0]);
        } else {
            uint32_t x = ex(e, k[0], true);
            ka[0] = x == TN ? TN : mk(e, TO_EXPRSTMT, 0, loc, 0, 0, 0, &x, 1);
        }
        ka[1] = ntag(c, k[1]) == N_NONE ? nop(e, loc) : rv(e, k[1]);
        if (ntag(c, k[2]) == N_NONE) {
            ka[2] = nop(e, loc);
        } else {
            uint32_t x = ex(e, k[2], true);
            ka[2] = x == TN ? TN : mk(e, TO_EXPRSTMT, 0, loc, 0, 0, 0, &x, 1);
        }
        ka[3] = st(e, k[3]);
        if (ka[0] != TN && ka[1] != TN && ka[2] != TN && ka[3] != TN)
            id = mk(e, TO_FOR, 0, loc, 0, 0, 0, ka, 4);
        break;
    }
    case N_SWITCH: {
        TypeId pt, save = e->swty;
        uint32_t x;
        m = fkids(e, n, &k);
        if (m != 2) {
            unsup(e, "malformed switch");
            break;
        }
        pt = promoted(c, k[0]);
        x = rv(e, k[0]);
        if (x == TN)
            break;
        ka[0] = conv(e, x, pt, loc);
        e->swty = unqual(c, pt);
        ka[1] = st(e, k[1]);
        e->swty = save;
        if (ka[0] != TN && ka[1] != TN)
            id = mk(e, TO_SWITCH, 0, loc, 0, 0, 0, ka, 2);
        break;
    }
    case N_CASE: {
        m = fkids(e, n, &k);
        if (c->nodes[n].flags & NF_RANGE) {
            unsup(e, "case range");
            break;
        }
        if (m != 2 || !e->swty || c->ck[k[0]] != K_ICE) {
            unsup(e, "case label form");
            break;
        }
        ka[0] = st(e, k[1]);
        if (ka[0] != TN)
            id = mk(e, TO_CASE, 0, loc, (int64_t)cexpr_trunc(c, e->swty, c->cv[k[0]]),
                    0, 0, ka, 1);
        break;
    }
    case N_DEFAULT: {
        m = fkids(e, n, &k);
        ka[0] = m ? st(e, k[0]) : nop(e, loc);
        if (ka[0] != TN)
            id = mk(e, TO_DEFAULT, 0, loc, 0, 0, 0, ka, 1);
        break;
    }
    case N_LABEL: {
        uint32_t s = TN;
        m = fkids(e, n, &k);
        for (j = 0; j < m; j++)
            if (ntag(c, k[j]) != N_ATTRIBUTE)
                s = st(e, k[j]);
        if (s == TN && !e->why[0])
            s = nop(e, loc);
        if (s != TN)
            id = mk(e, TO_LABEL, 0, loc, cnode_ident(c, n), 0, 0, &s, 1);
        break;
    }
    case N_GOTO:
        id = mk(e, TO_GOTO, 0, loc, cnode_ident(c, n), 0, 0, NULL, 0);
        break;
    case N_BREAK:
        id = mk(e, TO_BREAK, 0, loc, 0, 0, 0, NULL, 0);
        break;
    case N_CONTINUE:
        id = mk(e, TO_CONTINUE, 0, loc, 0, 0, 0, NULL, 0);
        break;
    case N_RETURN: {
        uint32_t x;
        m = fkids(e, n, &k);
        if (m == 0) {
            id = mk(e, TO_RETURN, 0, loc, 0, 0, 0, NULL, 0);
        } else if (type_is_void(&c->tt, e->ret)) {
            unsup(e, "value returned from void function");
        } else {
            x = conv(e, rv(e, k[0]), e->ret, loc);
            if (x != TN)
                id = mk(e, TO_RETURN, 0, loc, 0, 0, 0, &x, 1);
        }
        break;
    }
    default:
        unsup(e, "statement %s", node_names[ntag(c, n)]);
        break;
    }
    free(k);
    return id;
}

/* ---- printing -------------------------------------------------------------- */

static void put_type(Checker *c, FILE *out, TypeId t)
{
    StrBuf sb = {0};
    type_print(&c->tt, &sb, t);
    fputs(sb_cstr(&sb), out);
    sb_free(&sb);
}

static void put_loc(Checker *c, FILE *out, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    uint32_t line = 0, col = 0;
    if (f)
        srcmgr_linecol(f, loc, &line, &col);
    fprintf(out, "%s:%u:%u", f ? f->name : "?", line, col);
}

static void put_sym(Checker *c, FILE *out, uint32_t ref)
{
    const CSym *s = csym(c, ref);
    fputs(cident(c, s->name), out);
    if (ref & SYM_LOCAL) {
        SrcFile *f = srcmgr_file_of(c->sm, s->loc);
        uint32_t line = 0, col = 0;
        if (f)
            srcmgr_linecol(f, s->loc, &line, &col);
        fprintf(out, "@%u:%u", line, col);
    }
}

static void print_tree(El *e, FILE *out)
{
    Checker *c = e->c;
    size_t i;
    uint32_t j;
    for (i = 0; i < e->nodes.len; i++) {
        const TNode *nd = &e->nodes.data[i];
        fprintf(out, "  #%zu %s", i, op_names[nd->op]);
        switch (nd->op) {
        case TO_CONST:
            if (type_is_signed(&c->tt, nd->ty))
                fprintf(out, " %lld", (long long)nd->val);
            else
                fprintf(out, " %llu", (unsigned long long)nd->val);
            break;
        case TO_SYM: case TO_LOCAL: case TO_CALL:
            fputc(' ', out);
            put_sym(c, out, nd->sym);
            break;
        case TO_UNOP: case TO_BINOP: case TO_CMP:
            fprintf(out, " %s", punct_spelling[nd->pun]);
            break;
        case TO_CASE:
            fprintf(out, " %lld", (long long)nd->val);
            break;
        case TO_GOTO: case TO_LABEL:
            fprintf(out, " %s", cident(c, (uint32_t)nd->val));
            break;
        default:
            break;
        }
        for (j = 0; j < nd->nkids; j++)
            fprintf(out, " #%u", e->kids.data[nd->kid0 + j]);
        if (IS_EXPR(nd->op) || nd->op == TO_LOCAL) {
            fputc(' ', out);
            put_type(c, out, nd->ty);
        }
        fputc('\n', out);
    }
}

/* ---- the verifier (--verify-tast) ------------------------------------------ */

static void vbad(El *e, FILE *out, size_t i, const char *msg)
{
    if (e->c->opt.tast_bad)
        ++*e->c->opt.tast_bad;
    fprintf(out, "verify: #%zu %s: %s\n", i, op_names[e->nodes.data[i].op], msg);
}

/* Checks the tree against the shape rules of docs/TAST_T1_PLAN.md: kids
 * come before their parent and every node is used once (a tree rooted at
 * the last node), values are never a bare lvalue, operand types are the
 * ones the operator needs, statement and expression positions are kept
 * apart, every goto has a label.  Prints one "verify:" line per finding. */
static void verify(El *e, FILE *out)
{
    Checker *c = e->c;
    size_t n = e->nodes.len, i;
    uint8_t *refs = xcalloc(n ? n : 1, 1);
    U32Vec labels = {0};
    uint32_t j;
    for (i = 0; i < n; i++)
        if (e->nodes.data[i].op == TO_LABEL)
            vec_push(&labels, (uint32_t)e->nodes.data[i].val);
    for (i = 0; i < n; i++) {
        const TNode *nd = &e->nodes.data[i];
        const uint32_t *k = e->kids.data + nd->kid0;
        bool ex_ = IS_EXPR(nd->op);
#define KID(x) (&e->nodes.data[k[x]])
#define NEED(cnt) do { if (nd->nkids != (cnt)) { vbad(e, out, i, "wrong number of kids"); goto next; } } while (0)
#define ISVAL(p) (IS_EXPR((p)->op) && (p)->op != TO_SYM)
#define ISSTMT(p) (!IS_EXPR((p)->op))
#define SAME(p, q) (mainv(c, (p)->ty) == mainv(c, (q)->ty))
#define VOIDT type_is_void(&c->tt, nd->ty)
        for (j = 0; j < nd->nkids; j++) {
            if (k[j] >= i)
                vbad(e, out, i, "kid does not precede the node");
            else
                refs[k[j]]++;
        }
        for (j = 0; j < nd->nkids; j++)
            if (k[j] >= i)
                goto next;
        if (ex_ && !VOIDT && !ok_int(e, nd->ty))
            vbad(e, out, i, "type outside the subset");
        if (ex_ && VOIDT && nd->op != TO_CAST && nd->op != TO_CALL &&
            nd->op != TO_COND && nd->op != TO_COMMA)
            vbad(e, out, i, "void type");
        switch (nd->op) {
        case TO_CONST: NEED(0); break;
        case TO_SYM:
            NEED(0);
            if (csym(c, nd->sym)->kind != CS_OBJ)
                vbad(e, out, i, "not a variable");
            break;
        case TO_LOAD:
            NEED(1);
            if (KID(0)->op != TO_SYM)
                vbad(e, out, i, "operand is not a variable");
            else if (nd->ty != unqual(c, KID(0)->ty))
                vbad(e, out, i, "type is not the unqualified variable type");
            break;
        case TO_STORE:
            NEED(2);
            if (KID(0)->op != TO_SYM || !ISVAL(KID(1)))
                vbad(e, out, i, "operands");
            else if (!SAME(KID(0), KID(1)) || nd->ty != unqual(c, KID(0)->ty))
                vbad(e, out, i, "stored value has another type");
            break;
        case TO_CAST:
            NEED(1);
            if (!ISVAL(KID(0)))
                vbad(e, out, i, "operand");
            else if (!VOIDT && SAME(KID(0), nd))
                vbad(e, out, i, "cast to the same type");
            break;
        case TO_UNOP:
            NEED(1);
            if (!ISVAL(KID(0)))
                vbad(e, out, i, "operand");
            else if (nd->pun == P_BANG ? nd->ty != TYPE_B(INT) : !SAME(KID(0), nd))
                vbad(e, out, i, "operand type");
            break;
        case TO_BINOP:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)))
                vbad(e, out, i, "operands");
            else if (!SAME(KID(0), nd) ||
                     (nd->pun != P_SHL && nd->pun != P_SHR && !SAME(KID(1), nd)))
                vbad(e, out, i, "operand types");
            break;
        case TO_CMP:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)) || !SAME(KID(0), KID(1)) ||
                nd->ty != TYPE_B(INT))
                vbad(e, out, i, "operands");
            break;
        case TO_LOGAND: case TO_LOGOR:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)) || nd->ty != TYPE_B(INT))
                vbad(e, out, i, "operands");
            break;
        case TO_COND:
            NEED(3);
            if (!ISVAL(KID(0)))
                vbad(e, out, i, "condition");
            else if (VOIDT ? !IS_EXPR(KID(1)->op) || !IS_EXPR(KID(2)->op)
                           : !ISVAL(KID(1)) || !ISVAL(KID(2)) ||
                                 !SAME(KID(1), nd) || !SAME(KID(2), nd))
                vbad(e, out, i, "arms");
            break;
        case TO_ELVIS:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)) || !SAME(KID(1), nd))
                vbad(e, out, i, "operands");
            break;
        case TO_COMMA:
            NEED(2);
            if (!IS_EXPR(KID(0)->op) || KID(0)->op == TO_SYM ||
                !IS_EXPR(KID(1)->op) || !SAME(KID(1), nd))
                vbad(e, out, i, "operands");
            break;
        case TO_POST:
            NEED(1);
            if (KID(0)->op != TO_STORE || !SAME(KID(0), nd))
                vbad(e, out, i, "operand is not a store of the same type");
            break;
        case TO_CALL: {
            const CSym *s = csym(c, nd->sym);
            TypeId ft = type_canon(&c->tt, s->ty);
            if (s->kind != CS_FUNC || nd->nkids != type_ent(&c->tt, ft)->n) {
                vbad(e, out, i, "callee or argument count");
                break;
            }
            for (j = 0; j < nd->nkids; j++)
                if (!ISVAL(KID(j)) ||
                    mainv(c, KID(j)->ty) != mainv(c, type_params(&c->tt, ft)[j]))
                    vbad(e, out, i, "argument type");
            break;
        }
        case TO_BLOCK:
            for (j = 0; j < nd->nkids; j++)
                if (!ISSTMT(KID(j)))
                    vbad(e, out, i, "item is not a statement");
            break;
        case TO_LOCAL:
            if (nd->nkids > 1 || (nd->nkids && (!ISVAL(KID(0)) || !SAME(KID(0), nd))))
                vbad(e, out, i, "initializer");
            break;
        case TO_EXPRSTMT:
            NEED(1);
            if (!IS_EXPR(KID(0)->op) || KID(0)->op == TO_SYM)
                vbad(e, out, i, "operand");
            break;
        case TO_IF:
            if (nd->nkids < 2 || nd->nkids > 3 || !ISVAL(KID(0)) || !ISSTMT(KID(1)) ||
                (nd->nkids == 3 && !ISSTMT(KID(2))))
                vbad(e, out, i, "operands");
            break;
        case TO_WHILE: case TO_SWITCH:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISSTMT(KID(1)))
                vbad(e, out, i, "operands");
            break;
        case TO_DO:
            NEED(2);
            if (!ISSTMT(KID(0)) || !ISVAL(KID(1)))
                vbad(e, out, i, "operands");
            break;
        case TO_FOR:
            NEED(4);
            if (!ISSTMT(KID(0)) || !(ISVAL(KID(1)) || KID(1)->op == TO_NOP) ||
                !ISSTMT(KID(2)) || !ISSTMT(KID(3)))
                vbad(e, out, i, "operands");
            break;
        case TO_CASE: case TO_DEFAULT: case TO_LABEL:
            NEED(1);
            if (!ISSTMT(KID(0)))
                vbad(e, out, i, "operand is not a statement");
            break;
        case TO_RETURN:
            if (nd->nkids > 1 ||
                (nd->nkids && (!ISVAL(KID(0)) || mainv(c, KID(0)->ty) != mainv(c, e->ret))))
                vbad(e, out, i, "returned value");
            break;
        case TO_GOTO: {
            size_t q;
            for (q = 0; q < labels.len && labels.data[q] != (uint32_t)nd->val; q++)
                ;
            if (q == labels.len)
                vbad(e, out, i, "goto without a label");
            break;
        }
        default:
            break;
        }
next:;
#undef KID
#undef NEED
#undef ISVAL
#undef ISSTMT
#undef SAME
#undef VOIDT
    }
    for (i = 0; i < n; i++)
        if (refs[i] != (i + 1 == n ? 0 : 1))
            vbad(e, out, i, i + 1 == n ? "root is used" : "not used exactly once");
    if (n && e->nodes.data[n - 1].op != TO_BLOCK)
        vbad(e, out, n - 1, "root is not a block");
    vec_free(&labels);
    free(refs);
}

/* ---- the unit -------------------------------------------------------------- */

/* The declared symbols of a unit that is only declarations. */
static void print_decls(Checker *c, El *e, uint32_t root, FILE *out)
{
    uint32_t nk = nkids(c, root, NULL, 0), *k, j, any = 0;
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, root, k, nk);
    for (j = 0; j < nk; j++) {
        uint32_t init;
        if (ntag(c, k[j]) == N_INIT_DECL && declared_of(e, k[j], &init) != NO_NODE &&
            init != NO_NODE)
            unsup(e, "global initializer");
    }
    for (j = 0; j < nk && !e->why[0]; j++) {
        uint32_t d, init;
        if (ntag(c, k[j]) != N_INIT_DECL)
            continue;
        d = declared_of(e, k[j], &init);
        if (d == NO_NODE || !c->cb[d])
            continue;
        any = 1;
        fputs("unit ", out);
        put_loc(c, out, cnode_loc(c, k[j]));
        fputs(" decl ", out);
        put_sym(c, out, c->cb[d] - 1);
        fputc(' ', out);
        put_type(c, out, csym(c, c->cb[d] - 1)->ty);
        fputc('\n', out);
    }
    if (!any && !e->why[0]) {
        fputs("unit ", out);
        put_loc(c, out, cnode_loc(c, root));
        fputs(" decl\n", out);
    }
    free(k);
}

static void func_def(El *e, uint32_t root)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, root, NULL, 0), *k, j, d = NO_NODE, body = NO_NODE;
    const CSym *s;
    TypeId ft;
    const TypeEnt *fe;
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, root, k, nk);
    for (j = 0; j < nk; j++) {
        if (ntag(c, k[j]) == N_DECLARED)
            d = k[j];
        else if (ntag(c, k[j]) == N_COMPOUND)
            body = k[j];
        else if (ntag(c, k[j]) == N_DECL)
            unsup(e, "K&R definition");
    }
    free(k);
    if (e->why[0])
        return;
    if (d == NO_NODE || !c->cb[d] || body == NO_NODE) {
        unsup(e, "function without a symbol");
        return;
    }
    s = csym(c, c->cb[d] - 1);
    ft = type_canon(&c->tt, s->ty);
    fe = type_ent(&c->tt, ft);
    if (fe->flags & (TF_NOPROTO | TF_VARIADIC)) {
        unsup(e, "unprototyped or variadic definition");
        return;
    }
    e->ret = unqual(c, fe->base);
    if (!type_is_void(&c->tt, e->ret) && !ok_int(e, e->ret)) {
        unsup(e, "%s", why_type(e, e->ret));
        return;
    }
    for (j = 0; j < fe->n; j++) {
        TypeId pt = unqual(c, type_params(&c->tt, ft)[j]);
        if (!ok_int(e, pt)) {
            unsup(e, "%s", why_type(e, pt));
            return;
        }
    }
    st(e, body);
}

void tast_unit(Checker *c)
{
    El e;
    uint32_t root = c->nn - 1;
    FILE *out = c->opt.tast;
    SrcLoc loc = cnode_loc(c, root);
    unsigned tag = ntag(c, root);
    if (tag != N_FUNC_DEF && tag != N_DECL && tag != N_TOP_ASM)
        return;
    if (cin_system(c, loc))
        return;
    if (c->unit_err) {
        fputs("unit ", out);
        put_loc(c, out, loc);
        fputs(" skipped: errors\n", out);
        return;
    }
    memset(&e, 0, sizeof e);
    e.c = c;
    if (tag == N_DECL) {
        print_decls(c, &e, root, out);
        if (e.why[0]) {
            fputs("unit ", out);
            put_loc(c, out, loc);
            fprintf(out, " unsupported: %s\n", e.why);
        }
    } else {
        if (tag == N_TOP_ASM)
            unsup(&e, "top-level asm");
        else
            func_def(&e, root);
        fputs("unit ", out);
        put_loc(c, out, loc);
        if (e.why[0]) {
            fprintf(out, " unsupported: %s\n", e.why);
        } else {
            uint32_t d = declared_of(&e, root, NULL);
            fputs(" func ", out);
            put_sym(c, out, c->cb[d] - 1);
            fputc(' ', out);
            put_type(c, out, csym(c, c->cb[d] - 1)->ty);
            fputc('\n', out);
            print_tree(&e, out);
            if (c->opt.verify_tast)
                verify(&e, out);
        }
    }
    vec_free(&e.nodes);
    vec_free(&e.kids);
}
