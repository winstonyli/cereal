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

#include <ctype.h>
#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/cgx86.h"
#include "c/cintrin.h"
#include "c/csymidx.h"
#include "c/eval.h"
#include "c/lit.h"

#include "c/tast_int.h"


static const char *const op_names[TO_COUNT] = {
#define X(n, s) s,
    TAST_OPS(X)
#undef X
};


uint32_t tunsup(El *e, const char *fmt, ...)
{
    if (!e->why[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->why, sizeof e->why, fmt, ap);
        va_end(ap);
    }
    return TN;
}

uint32_t mk(El *e, int op, TypeId ty, SrcLoc loc, int64_t val,
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

/* The row (cintrin.h) of a built-in called by identifier `name`, or 0: the back end refuses it. */
static unsigned intrin_row(Checker *c, uint32_t name)
{
    const char *nm = cident(c, name);
    return bi_find_call(nm);
}

/* A back-end built-in (a row of cintrin.h, named without its "__builtin_") applied to args. */
uint32_t intrin(El *e, const char *nm, TypeId rt, const uint32_t *args, uint32_t n, SrcLoc loc)
{
    char full[64];
    uint32_t sym;
    snprintf(full, sizeof full, "__builtin_%s", nm);
    sym = intern_cstr(e->c->in, full)->id;
    return mk(e, TO_INTRIN, rt, loc, n, sym, bi_find(nm), args, n);
}

bool ok_int(El *e, TypeId t)
{
    TypeKind k = type_ckind(&e->c->tt, t);
    return (k >= TY_BOOL && k <= TY_ULLONG) || k == TY_ENUM;
}

/* A type the pass handles: integers, pointers, complete structs and unions,
 * and arrays of those. */
static bool ok_ty(El *e, TypeId t)
{
    Checker *c = e->c;
    switch (type_ckind(&c->tt, t)) {
    case TY_PTR: case TY_NULLPTR: return true;   /* nullptr_t: a pointer-sized zero */
    case TY_STRUCT: case TY_UNION: return type_is_complete(&c->tt, t);
    case TY_ARRAY: return ok_ty(e, elem_of(c, t)) && type_is_complete(&c->tt, t);
    case TY_VECTOR: return true;
    case TY_FLOAT: case TY_DOUBLE: case TY_LDOUBLE: return true;
    default: return ok_int(e, t);
    }
}

static bool is_lvop(int op)
{
    return op == TO_SYM || op == TO_TMP || op == TO_REG || op == TO_COMPLIT || op == TO_DEREF || op == TO_FIELD || op == TO_STR;
}

/* Why a type is outside the subset, for the reason histogram. */
static const char *why_type(El *e, TypeId t)
{
    switch (type_ckind(&e->c->tt, t)) {
    case TY_PTR: return "pointer type";
    case TY_ARRAY: case TY_VLA: return "array type";
    case TY_FUNC: return "function designator";
    case TY_STRUCT: case TY_UNION: return "incomplete struct/union type";
    case TY_COMPLEX: return "complex type";
    case TY_VECTOR: return "vector type";
    case TY_INT128: case TY_UINT128: return "__int128 type";
    case TY_VOID: return "void value";
    case TY_DEC32: case TY_DEC64: case TY_DEC128: return "decimal floating type";
    case TY_FLOAT16: case TY_FLOAT32: case TY_FLOAT64: case TY_FLOAT128:
    case TY_FLOAT32X: case TY_FLOAT64X: return "_FloatN type";
    default: return "unsupported type";
    }
}

TypeId nty(El *e, uint32_t id)
{
    return e->nodes.data[id].ty;
}

uint32_t cnst(El *e, TypeId t, uint64_t v, SrcLoc loc)
{
    t = unqual(e->c, t);
    return mk(e, TO_CONST, t, loc, (int64_t)cexpr_trunc(e->c, t, v), 0, 0, NULL, 0);
}

/* Node idx becomes the floating constant v of type t: its bytes in the
 * target's format go to El.bytes.  The checker rounded v to t already (the
 * literal parser rounds once), so this only changes format.  False, with the
 * unit unsupported, when the target's format is not the host's. */
static bool fset(El *e, uint32_t idx, TypeId t, long double v)
{
    Checker *c = e->c;
    bool ok = false;
    size_t sz = type_size(&c->tt, t, &ok), j, off = e->bytes.len;
    uint8_t b[16] = {0};
    TypeKind k = type_ckind(&c->tt, t);
    if (!ok || sz > sizeof b) {
        tunsup(e, "floating constant format");
        return false;
    }
    if (k == TY_FLOAT && sz == 4) {
        float f = (float)v;
        memcpy(b, &f, 4);
    } else if (k == TY_DOUBLE ||
               (k == TY_LDOUBLE && c->tgt->long_double == LD_IEEE64)) {
        double d = (double)v;
        if (sz != 8) {
            tunsup(e, "floating constant format");
            return false;
        }
        memcpy(b, &d, 8);
    } else if (k == TY_LDOUBLE && c->tgt->long_double == LD_X87 &&
               LDBL_MANT_DIG == 64 && sz >= 10) {
        memcpy(b, &v, 10);
    } else {
        tunsup(e, "long double format");
        return false;
    }
    for (j = 0; j < sz; j++)
        vec_push(&e->bytes, b[j]);
    e->nodes.data[idx].op = TO_FCONST;
    e->nodes.data[idx].ty = t;
    e->nodes.data[idx].val = (int64_t)off;
    e->nodes.data[idx].width = (uint32_t)sz;
    return true;
}

uint32_t fconst(El *e, TypeId t, long double v, SrcLoc loc)
{
    uint32_t id = mk(e, TO_FCONST, unqual(e->c, t), loc, 0, 0, 0, NULL, 0);
    return fset(e, id, unqual(e->c, t), v) ? id : TN;
}

/* id converted to type `to` (a no-op when the types are the same; a
 * constant folds, as gcc does: an integer constant converted to a floating
 * type becomes a floating constant, every other conversion of a floating
 * constant stays a cast). */
uint32_t conv(El *e, uint32_t id, TypeId to, SrcLoc loc)
{
    Checker *c = e->c;
    const TNode *nd;
    if (id == TN)
        return TN;
    to = unqual(c, to);
    if (mainv(c, nty(e, id)) == mainv(c, to))
        return id;
    nd = &e->nodes.data[id];
    if (nd->op == TO_CONST && is_flt(c, to)) {
        long double v = type_is_signed(&c->tt, nd->ty) ? (long double)nd->val
                                                       : (long double)(uint64_t)nd->val;
        return fset(e, id, to, v) ? id : TN;
    }
    if (nd->op == TO_CONST && type_ckind(&c->tt, to) != TY_UNION) {   /* nothing else refers to it: retype in place (a union keeps a cast) */
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
    tunsup(e, "rule disagreement (%s)", node_names[ntag(c, i)]);
    return false;
}

static uint32_t ex(El *e, uint32_t n, bool allow_void);
static uint32_t stmt_expr(El *e, uint32_t i, TypeId t, SrcLoc loc);
static bool init_elems(El *e, uint32_t init, TypeId oty, bool is_static, SrcLoc loc,
                       U32Vec *out);

uint32_t rv(El *e, uint32_t n)
{
    return ex(e, n, false);
}

static TypeId ptrdiff_ty(El *e)
{
    return TYPE_MK(e->c->tgt->ptrdiff_type, 0);
}


uint32_t load(El *e, uint32_t symid)
{
    uint32_t id = symid;
    if (symid == TN)
        return TN;
    return mk(e, TO_LOAD, unqual(e->c, nty(e, symid)), e->nodes.data[symid].loc,
              0, 0, 0, &id, 1);
}

uint32_t addr_t(El *e, uint32_t l, TypeId pty, SrcLoc loc)
{
    if (l == TN)
        return TN;
    return mk(e, TO_ADDR, unqual(e->c, pty), loc, 0, 0, 0, &l, 1);
}

/* The object a pointer value designates; t is its type (a function type is
 * allowed: *fp decays straight back). */
uint32_t deref_t(El *e, uint32_t p, TypeId t, SrcLoc loc)
{
    if (p == TN)
        return TN;
    if (type_is_void(&e->c->tt, t))
        return tunsup(e, "void value");
    if (type_ckind(&e->c->tt, t) != TY_FUNC && !ok_ty(e, t))
        return tunsup(e, "%s", why_type(e, t));
    return mk(e, TO_DEREF, t, loc, 0, 0, 0, &p, 1);
}

/* p + x (or p - x when neg): x is converted to ptrdiff_t, the node carries
 * the element size.  pty is the pointer type of p. */
uint32_t ptradd(El *e, TypeId pty, uint32_t p, uint32_t x, bool neg, SrcLoc loc)
{
    uint32_t ka[2];
    TypeId dt = ptrdiff_ty(e);
    if (p == TN || x == TN)
        return TN;
    x = conv(e, x, dt, loc);
    if (neg) {
        if (e->nodes.data[x].op == TO_CONST)
            e->nodes.data[x].val = (int64_t)cexpr_trunc(e->c, dt, (uint64_t)-e->nodes.data[x].val);
        else
            x = mk(e, TO_UNOP, dt, loc, 0, 0, P_MINUS, &x, 1);
    }
    ka[0] = p;
    ka[1] = x;
    return mk(e, TO_PTRADD, unqual(e->c, pty), loc, (int64_t)elem_size(e->c, pty), 0, 0, ka, 2);
}

/* A string literal's code units, decoded once into El.bytes (little-endian
 * units, terminator included); the node holds where they are. */
static uint32_t str_node(El *e, uint32_t i, TypeId t, SrcLoc loc)
{
    Checker *c = e->c;
    bool ok = false;
    unsigned unit = (unsigned)type_size(&c->tt, type_base(&c->tt, type_canon(&c->tt, t)), &ok);
    uint32_t np = node_pieces(c, i), k, off = (uint32_t)e->bytes.len, j;
    if (!ok || (unit != 1 && unit != 2 && unit != 4))
        return tunsup(e, "string element size");
    for (k = 0; k < np; k++) {
        size_t len, m;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        size_t at = e->bytes.len;
        m = lit_str_bytes(s, len, unit, NULL);
        for (j = 0; j < m; j++)
            vec_push(&e->bytes, 0);
        lit_str_bytes(s, len, unit, e->bytes.data + at);
    }
    for (j = 0; j < unit; j++)
        vec_push(&e->bytes, 0);
    j = mk(e, TO_STR, t, loc, off, 0, unit, NULL, 0);
    e->nodes.data[j].width = (uint32_t)e->bytes.len - off;
    return j;
}

/* A one-byte-element string node holding the n given bytes. */
static uint32_t str_bytes(El *e, const char *p, size_t n, TypeId t, SrcLoc loc)
{
    uint32_t off = (uint32_t)e->bytes.len, id;
    size_t j;
    for (j = 0; j < n; j++)
        vec_push(&e->bytes, (uint8_t)p[j]);
    vec_push(&e->bytes, 0);
    id = mk(e, TO_STR, t, loc, off, 0, 1, NULL, 0);
    e->nodes.data[id].width = (uint32_t)e->bytes.len - off;
    return id;
}

/* __func__, __FUNCTION__, __PRETTY_FUNCTION__: the enclosing function's name
 * as a string literal. */
static uint32_t func_str(El *e, TypeId t, SrcLoc loc)
{
    const char *nm = cident(e->c, e->fname);
    size_t len = strlen(nm), j;
    uint32_t off = (uint32_t)e->bytes.len, id;
    for (j = 0; j <= len; j++)
        vec_push(&e->bytes, (uint8_t)nm[j]);
    id = mk(e, TO_STR, t, loc, off, 0, 1, NULL, 0);
    e->nodes.data[id].width = (uint32_t)len + 1;
    return id;
}

/* The lvalue an expression designates: a variable (or function), *p, a
 * member, a string literal.  a[i] is *(a + i); p->f is (*p).f. */
uint32_t lval(El *e, uint32_t n)
{
    Checker *c = e->c;
    uint32_t i = strip_paren(c, n), k[2];
    TypeId t;
    SrcLoc loc;
    if (e->why[0])
        return TN;
    loc = cnode_loc(c, i);
    t = c->ty[i];
    if (c->ck[i] == K_ERR || type_ckind(&c->tt, t) == TY_ERROR)
        return tunsup(e, "type error");
    switch (ntag(c, i)) {
    case N_IDENT: {
        uint32_t ref = c->sym[i];
        const CSym *s;
        if (ref == SYM_NONE) {
            const char *nm = cident(c, cnode_ident(c, i));
            if (e->infunc && (!strcmp(nm, "__func__") || !strcmp(nm, "__FUNCTION__") ||
                              !strcmp(nm, "__PRETTY_FUNCTION__")))
                return func_str(e, t, loc);
            return tunsup(e, "builtin or undeclared identifier %s", nm);
        }
        s = csym(c, ref);
        if (s->kind != CS_OBJ && s->kind != CS_FUNC)
            return tunsup(e, "identifier is not a variable");
        if (s->kind == CS_OBJ && !ok_ty(e, s->ty) &&      /* an extern array of unknown size only decays */
            !(type_ckind(&c->tt, s->ty) == TY_ARRAY && ok_ty(e, elem_of(c, s->ty))))
            return tunsup(e, "%s", why_type(e, s->ty));
        return mk(e, TO_SYM, s->ty, loc, 0, ref, 0, NULL, 0);
    }
    case N_STRING:
        return str_node(e, i, t, loc);
    case N_COMPOUND_LIT: {
        uint32_t cl[2], id = TN;
        U32Vec el = {0};
        bool stat = !e->infunc;
        if (nkids(c, i, cl, 2) < 2)
            return tunsup(e, "malformed compound literal");
        if (!ok_ty(e, t))
            return tunsup(e, "%s", why_type(e, t));
        if (init_elems(e, cl[1], t, stat, loc, &el))
            id = mk(e, TO_COMPLIT, t, loc, e->ncl++, 0, stat, el.data, (uint32_t)el.len);
        vec_free(&el);
        return id;
    }
    case N_INDEX: {
        uint32_t a, x, p, ix;
        if (nkids(c, i, k, 2) < 2)
            return tunsup(e, "malformed index");
        a = k[0];
        x = k[1];
        if (!is_array(c, c->ty[a]) && !is_ptr(c, c->ty[a]) && tkind(c, c->ty[a]) != TY_VECTOR) {
            a = k[1];
            x = k[0];
        }
        if (tkind(c, c->ty[a]) == TY_VECTOR) {                      /* the lane's address: the vector's, as a pointer to the lanes, plus the index */
            TypeId et = unqual(c, elem_of(c, c->ty[a])), pt = type_ptr(&c->tt, et);
            uint32_t l, q;
            if (c->ef[strip_paren(c, a)] & EF_LVALUE) {
                l = lval(e, a);
            } else {                                                /* a value: a compound literal that holds it */
                uint32_t v = ex(e, a, false), el = TN;
                if (v != TN)
                    el = mk(e, TO_ELEM, unqual(c, nty(e, v)), loc, 0, 0, 0, &v, 1);
                l = el == TN ? TN : mk(e, TO_COMPLIT, unqual(c, nty(e, v)), loc, e->ncl++, 0, !e->infunc, &el, 1);
            }
            if (l == TN)
                return TN;
            q = conv(e, addr_t(e, l, type_ptr(&c->tt, nty(e, l)), loc), pt, loc);
            ix = ex(e, x, false);
            return deref_t(e, ptradd(e, pt, q, ix, false, loc), t, loc);
        }
        p = ex(e, a, false);
        ix = ex(e, x, false);
        return deref_t(e, ptradd(e, rvt(c, a), p, ix, false, loc), t, loc);
    }
    case N_MEMBER_EXPR: {
        uint32_t d = first_child(c, i), base;
        TypeId rec;
        const Field *f;
        uint64_t off = 0;
        unsigned q = 0;
        TNode *nd;
        uint32_t id;
        if (d == NO_NODE)
            return tunsup(e, "malformed member");
        if (c->nodes[i].flags & NF_ARROW) {
            rec = pointee(c, rvt(c, d));
            base = deref_t(e, ex(e, d, false), rec, loc);
        } else {
            if (!(c->ef[d] & EF_LVALUE))
                return tunsup(e, "member of rvalue");
            rec = c->ty[d];
            base = lval(e, d);
        }
        if (base == TN)
            return TN;
        f = find_field(c, rec, cnode_ident(c, i), &off, &q);
        if (!f)
            return tunsup(e, "member not found");
        if (!ok_ty(e, t) && !(type_ckind(&c->tt, t) == TY_ARRAY && ok_ty(e, elem_of(c, t))))   /* a flexible array member only decays */
            return tunsup(e, "%s", why_type(e, t));
        id = mk(e, TO_FIELD, t, loc, (int64_t)(off / 8), 0, (unsigned)(off % 8), &base, 1);
        nd = &e->nodes.data[id];
        nd->width = (f->flags & FF_BITFIELD) ? f->width : 0;
        return id;
    }
    case N_UNARY: {
        const Tok *tk = cnode_tok(c, i);
        if (tk->kind != TK_PUNCT)       /* __extension__ */
            return lval(e, first_child(c, i));
        if (npunct(c, i) == P_STAR)
            return deref_t(e, ex(e, first_child(c, i), false), t, loc);
        return tunsup(e, "lvalue form");
    }
    default:
        return tunsup(e, "lvalue form");
    }
}

/* Compound assignment and ++/-- name the lvalue twice.  When naming it
 * evaluates something, its address is computed once into a temporary
 * (`tmp N`, a pointer) and every mention is `deref (load tmp)`; the result
 * is then wrapped in a stmtexpr whose first statement stores the address.
 * Without a side effect the lvalue is simply built twice. */
typedef struct Once {
    uint32_t x;             /* the checker's lvalue node */
    uint32_t tmp;           /* the temporary's number, UINT32_MAX: none */
    TypeId pt;              /* its type: pointer to the lvalue's type */
    unsigned bpun, bwidth;  /* a bit-field: its bit offset in the addressed byte, its width */
    SrcLoc loc;
} Once;

/* Prepare to name x more than once.  *setup is the statement that makes
 * the temporary, TN when there is none; false: the unit is unsupported. */
static bool once_init(El *e, uint32_t x, Once *o, uint32_t *setup)
{
    Checker *c = e->c;
    uint32_t l, a, t, ka[2];
    x = strip_paren(c, x);
    o->x = x;
    o->tmp = UINT32_MAX;
    o->bwidth = 0;
    o->loc = cnode_loc(c, x);
    *setup = TN;
    if (ntag(c, x) == N_IDENT || !(c->ef[x] & EF_SIDE))
        return true;
    l = lval(e, x);
    if (l == TN)
        return false;
    if (e->nodes.data[l].op == TO_FIELD && e->nodes.data[l].width) {   /* the temporary holds the field's byte address */
        o->bpun = e->nodes.data[l].pun;
        o->bwidth = e->nodes.data[l].width;
    }
    o->pt = type_ptr(&c->tt, nty(e, l));
    a = addr_t(e, l, o->pt, o->loc);
    o->tmp = e->ntmp++;
    t = mk(e, TO_TMP, o->pt, o->loc, o->tmp, 0, 0, NULL, 0);
    ka[0] = t;
    ka[1] = a;
    t = mk(e, TO_STORE, o->pt, o->loc, 0, 0, 0, ka, 2);
    *setup = mk(e, TO_EXPRSTMT, 0, o->loc, 0, 0, 0, &t, 1);
    return true;
}

/* A fresh lvalue node for the object (a node is used once). */
static uint32_t once_ref(El *e, Once *o)
{
    uint32_t t;
    if (o->tmp == UINT32_MAX)
        return lval(e, o->x);
    t = mk(e, TO_TMP, o->pt, o->loc, o->tmp, 0, 0, NULL, 0);
    t = load(e, t);
    t = deref_t(e, t, pointee(e->c, o->pt), o->loc);
    if (o->bwidth && t != TN) {
        t = mk(e, TO_FIELD, pointee(e->c, o->pt), o->loc, 0, 0, o->bpun, &t, 1);
        e->nodes.data[t].width = o->bwidth;
    }
    return t;
}

/* v, preceded by the temporary's setup when there is one. */
static uint32_t once_wrap(El *e, uint32_t setup, uint32_t v)
{
    uint32_t ka[2];
    if (setup == TN || v == TN)
        return v;
    ka[0] = setup;
    ka[1] = v;
    return mk(e, TO_STMTEXPR, e->nodes.data[v].ty, e->nodes.data[v].loc, 0, 0, 0, ka, 2);
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
uint32_t arith(El *e, uint32_t i, int op, uint32_t a, uint32_t b,
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
    uint32_t l1, l2, a, one, b, v, st, ka[2], setup;
    Once o;
    SrcLoc loc = cnode_loc(c, i);
    VOp oa = {0};
    x = strip_paren(c, x);
    xt = unqual(c, c->ty[x]);
    if (!is_ptr(c, xt) && !ok_int(e, xt) && !is_flt(c, xt) && tkind(c, xt) != TY_VECTOR)
        return tunsup(e, "%s", why_type(e, xt));
    if (!once_init(e, x, &o, &setup))
        return TN;
    l1 = once_ref(e, &o);
    l2 = once_ref(e, &o);
    if (l1 == TN || l2 == TN)
        return TN;
    if (is_ptr(c, xt)) {
        a = load(e, l2);
        one = cnst(e, ptrdiff_ty(e), 1, loc);
        v = ptradd(e, xt, a, one, punct == P_DEC, loc);
    } else if (tkind(c, xt) == TY_VECTOR) {        /* every lane plus or minus 1 */
        U32Vec vs = {0};
        v = vop_val(e, load(e, l2), xt, &oa, &vs, loc) ? vec_un(e, xt, punct == P_INC ? P_PLUS : P_MINUS, true, &oa, loc, &vs) : TN;
        vec_free(&vs);
    } else {
        op = common_type(c, promoted(c, x), TYPE_B(INT));
        a = conv(e, load(e, l2), op, loc);
        one = is_flt(c, op) ? fconst(e, op, 1, loc) : cnst(e, op, 1, loc);
        if (one == TN)
            return TN;
        ka[0] = a;
        ka[1] = one;
        b = mk(e, TO_BINOP, unqual(c, op), loc, 0, 0,
               punct == P_INC ? P_PLUS : P_MINUS, ka, 2);
        v = conv(e, b, xt, loc);
    }
    if (v == TN)
        return TN;
    ka[0] = l1;
    ka[1] = v;
    st = mk(e, TO_STORE, xt, loc, 0, 0, 0, ka, 2);
    if (post && tkind(c, xt) == TY_VECTOR) {       /* the old value is the temporary the lanes were read from */
        U32Vec ss = {0};
        vec_push(&ss, mk(e, TO_EXPRSTMT, 0, loc, 0, 0, 0, &st, 1));
        v = vec_wrap(e, load(e, mk(e, TO_TMP, oa.st, loc, oa.tmp, 0, 0, NULL, 0)), xt, &ss, loc);
        vec_free(&ss);
        return once_wrap(e, setup, v);
    }
    return once_wrap(e, setup, post ? mk(e, TO_POST, xt, loc, 0, 0, 0, &st, 1) : st);
}

static uint32_t e_assign_t(El *e, uint32_t i)
{
    Checker *c = e->c;
    uint32_t k[2], lhs, rhs, l1, ka[2], v, setup = TN;
    Once o;
    int p = npunct(c, i), op = assign_op(p);
    TypeId lt;
    SrcLoc loc = cnode_loc(c, i);
    if (nkids(c, i, k, 2) != 2)
        return tunsup(e, "malformed assignment");
    lhs = strip_paren(c, k[0]);
    rhs = strip_paren(c, k[1]);
    lt = unqual(c, c->ty[lhs]);
    if (!ok_ty(e, lt))
        return tunsup(e, "%s", why_type(e, lt));
    if (op && !once_init(e, lhs, &o, &setup))
        return TN;
    l1 = op ? once_ref(e, &o) : lval(e, lhs);
    if (l1 == TN)
        return TN;
    if (p == P_ASSIGN) {
        v = conv(e, ex(e, rhs, false), lt, loc);
    } else if (op && is_ptr(c, lt) && (op == P_PLUS || op == P_MINUS)) {
        uint32_t l2 = once_ref(e, &o), x, y;
        x = l2 == TN ? TN : load(e, l2);
        y = ex(e, rhs, false);
        v = ptradd(e, lt, x, y, op == P_MINUS, loc);
    } else if (op && tkind(c, lt) == TY_VECTOR) {      /* x = x op y on the lanes */
        U32Vec vs = {0};
        VOp oa, ob;
        uint32_t l2 = once_ref(e, &o);
        TypeId yt = tkind(c, rvt(c, rhs)) == TY_VECTOR ? rvt(c, rhs) : lt;
        bool good = l2 != TN && vop_val(e, load(e, l2), lt, &oa, &vs, loc) && vop_node(e, rhs, yt, &ob, &vs, loc);
        v = good ? vec_op(e, op, lt, &oa, &ob, loc, &vs) : TN;
        vec_free(&vs);
    } else if (op && (ok_int(e, lt) || is_flt(c, lt))) {
        /* x op= y is x = (T)((op type)x op (op type)y) */
        TypeId ot;
        uint32_t l2 = once_ref(e, &o), b;
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
        return tunsup(e, "assignment operator");
    }
    if (v == TN)
        return TN;
    ka[0] = l1;
    ka[1] = v;
    return once_wrap(e, setup, mk(e, TO_STORE, lt, loc, 0, 0, 0, ka, 2));
}

/* p + x, x + p, p - x, p - q. */
static uint32_t ptr_binary(El *e, uint32_t i, int p, uint32_t a, uint32_t b, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId ta = rvt(c, a), tb = rvt(c, b), rt;
    uint32_t r, pa, pb;
    if (is_ptr(c, ta) && is_ptr(c, tb)) {
        uint32_t ka[2];
        ka[0] = ex(e, a, false);
        ka[1] = ex(e, b, false);
        if (ka[0] == TN || ka[1] == TN)
            return TN;
        rt = ptrdiff_ty(e);
        if (!agree(e, i, rt))
            return TN;
        return mk(e, TO_PTRDIFF, rt, loc, (int64_t)elem_size(c, ta), 0, 0, ka, 2);
    }
    pa = ex(e, a, false);
    pb = ex(e, b, false);
    if (is_ptr(c, ta))
        r = ptradd(e, ta, pa, pb, p == P_MINUS, loc);
    else
        r = ptradd(e, tb, pb, pa, false, loc);
    if (r != TN && !agree(e, i, nty(e, r)))
        return TN;
    return r;
}

/* The arguments k[0..na) of a call to a function of type ft, appended to
 * args: the declared parameters get the parameter type, the rest the default
 * argument promotions (C11 6.5.2.2p6: integers promote, float becomes double).  Returns
 * false when the unit became unsupported. */
static bool call_args(El *e, TypeId ft, const uint32_t *k, uint32_t na,
                      SrcLoc loc, U32Vec *args)
{
    Checker *c = e->c;
    const TypeEnt *fe = type_ent(&c->tt, ft);
    uint32_t np = (fe->flags & TF_NOPROTO) ? 0 : (uint32_t)fe->n, j;
    if (na < np || (na > np && !(fe->flags & (TF_NOPROTO | TF_VARIADIC)))) {
        tunsup(e, "call argument count");
        return false;
    }
    for (j = 0; j < na; j++) {
        uint32_t a = strip_paren(c, k[j]), v, ca;
        TypeId pt;
        if (j < np) {
            pt = unqual(c, type_params(&c->tt, ft)[j]);
            if (!ok_ty(e, pt)) {
                tunsup(e, "%s", why_type(e, pt));
                return false;
            }
            v = ex(e, a, false);
        } else {
            v = ex(e, a, false);
            if (v == TN)
                return false;
            pt = is_int(c, nty(e, v)) ? promoted(c, a)
               : type_ckind(&c->tt, nty(e, v)) == TY_FLOAT ? TYPE_B(DOUBLE) : nty(e, v);
        }
        ca = conv(e, v, pt, loc);
        if (ca == TN)
            return false;
        vec_push(args, ca);
    }
    return true;
}

/* A builtin called by its __builtin_ spelling, with a fixed prototype in the
 * checker's table: the prototype, 0 when the pass cannot take it. */
static TypeId builtin_ft(El *e, uint32_t callee)
{
    Checker *c = e->c;
    const char *nm = cident(c, cnode_ident(c, callee));
    const BTab *bt;
    TypeId ft;
    if (strncmp(nm, "__builtin_", 10))
        return 0;
    bt = bt_find(c, nm + 10, true);
    if (!bt || bt->gnu == 3 || !strchr(bt->sig, '|') || !strcmp(strchr(bt->sig, '|') + 1, "?"))
        return 0;
    ft = type_canon(&c->tt, bt_func_type(c, bt));
    return type_ckind(&c->tt, ft) == TY_FUNC ? ft : 0;
}

/* va_start (also c23_va_start, whose last-named argument is optional), va_end,
 * va_copy and va_arg: one node, `va start|end|copy|arg`,
 * whose kids are the va_list operands as pointers to its first element
 * (start's last-named-parameter argument is not evaluated and not kept);
 * the type is void, or for arg the type read. */
enum { VA_NONE, VA_START, VA_END, VA_COPY, VA_ARG };

static int va_kind(Checker *c, uint32_t i)
{
    uint32_t k[1];
    const char *nm;
    if (ntag(c, i) == N_VA_ARG)
        return VA_ARG;
    if (ntag(c, i) != N_CALL || nkids(c, i, k, 1) < 1)
        return VA_NONE;
    k[0] = strip_paren(c, k[0]);
    if (ntag(c, k[0]) != N_IDENT || c->sym[k[0]] != SYM_NONE)
        return VA_NONE;
    nm = cident(c, cnode_ident(c, k[0]));
    return !strcmp(nm, "__builtin_va_start") ||
                   !strcmp(nm, "__builtin_c23_va_start") ? VA_START
         : !strcmp(nm, "__builtin_va_end") ? VA_END
         : !strcmp(nm, "__builtin_va_copy") ? VA_COPY : VA_NONE;
}

static uint32_t e_va(El *e, uint32_t i, int kind)
{
    Checker *c = e->c;
    uint32_t k[3], nk = nkids(c, i, k, 3), ka[2], nka = 0, j;
    SrcLoc loc = cnode_loc(c, i);
    TypeId t = kind == VA_ARG ? unqual(c, c->ty[i]) : TYPE_B(VOID);
    uint32_t want = kind == VA_COPY ? 2 : 1;
    if (kind != VA_ARG && (nk < want + 1 || (kind == VA_START && nk > 3) ||
                           (kind != VA_START && nk != want + 1)))
        return tunsup(e, "va builtin arguments");
    if (kind == VA_ARG && nk != 2)
        return tunsup(e, "malformed va_arg");
    if (kind == VA_ARG && !ok_ty(e, t))
        return tunsup(e, "%s", why_type(e, t));
    for (j = 0; j < want; j++) {
        ka[nka] = rv(e, k[kind == VA_ARG ? 0 : j + 1]);
        if (ka[nka++] == TN)
            return TN;
    }
    return mk(e, TO_VA, t, loc, 0, 0, (unsigned)kind, ka, nka);
}

static uint32_t e_call_t(El *e, uint32_t i)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, i, NULL, 0), *k, callee, ref = 0, name = 0, row;
    TypeId ft;
    const TypeEnt *fe;
    U32Vec args = {0};
    uint32_t id = TN;
    bool indirect = false;
    SrcLoc loc = cnode_loc(c, i);
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, i, k, nk);
    callee = strip_paren(c, k[0]);
    if (ntag(c, callee) == N_IDENT && c->sym[callee] != SYM_NONE &&
        csym(c, c->sym[callee])->kind == CS_FUNC) {
        ref = c->sym[callee];
        ft = type_canon(&c->tt, csym(c, ref)->ty);
    } else if (ntag(c, callee) == N_IDENT && c->sym[callee] == SYM_NONE) {
        const char *bn = cident(c, cnode_ident(c, callee));
        if (!strcmp(bn, "__builtin_shuffle") || !strcmp(bn, "__builtin_shufflevector")) {
            id = vec_shuffle(e, i, k, nk, bn[17] == 'v', loc);
            goto out;
        }
        ft = builtin_ft(e, callee);
        row = ft ? 0 : intrin_row(c, cnode_ident(c, callee));
        if (bi_rows[row].fl) {
            /* a call lowered from its row: without a prototype (the overloaded atomics, the
             * generic overflow built-ins) the arguments are pointers and ints taken as they
             * are, with one the operands convert to the prototype's parameters */
            bool proto = !(bi_rows[row].fl & BIF_GEN);
            TypeId pft = proto ? type_canon(&c->tt, pointee(c, rvt(c, callee))) : 0;
            uint32_t j;
            if (bi_rows[row].argc && nk - 1 != bi_rows[row].argc) {
                tunsup(e, "call argument count");
                goto out;
            }
            for (j = 1; j < nk; j++) {
                uint32_t x = rv(e, k[j]);
                if (x != TN && proto && !is_ptr(c, type_params(&c->tt, pft)[j - 1]))
                    x = conv(e, x, unqual(c, type_params(&c->tt, pft)[j - 1]), loc);
                if (x == TN)
                    goto out;
                vec_push(&args, x);
            }
            id = mk(e, TO_INTRIN, unqual(c, c->ty[i]), loc, 0, cnode_ident(c, callee), row,
                    args.data, (uint32_t)args.len);
            goto out;
        }
        if (!ft) {
            tunsup(e, "builtin or undeclared call %s", cident(c, cnode_ident(c, callee)));
            goto out;
        }
        name = cnode_ident(c, callee);
    } else {
        TypeId pt = rvt(c, callee);
        uint32_t cv;
        if (!is_ptr(c, pt) || !is_func(c, pointee(c, pt))) {
            tunsup(e, "call of a non-function");
            goto out;
        }
        ft = type_canon(&c->tt, pointee(c, pt));
        cv = ex(e, callee, false);
        if (cv == TN)
            goto out;
        vec_push(&args, cv);
        indirect = true;
    }
    fe = type_ent(&c->tt, ft);
    if (!call_args(e, ft, k + 1, nk - 1, loc, &args))
        goto out;
    if (name && !strncmp(cident(c, name), "__builtin_ia32_", 15)) {
        id = ia32_call(e, cident(c, name) + 10, &args, unqual(c, c->ty[i]), loc);
        goto out;
    }
    {
        uint32_t nf = (fe->flags & TF_NOPROTO) ? 0 : (uint32_t)fe->n;
        id = mk(e, name ? TO_INTRIN : TO_CALL, unqual(c, c->ty[i]), loc, nf,
                name ? name : ref, name ? intrin_row(c, name) : indirect, args.data, (uint32_t)args.len);
        if (id != TN && (fe->flags & (TF_NOPROTO | TF_VARIADIC)))
            e->nodes.data[id].width = 1;
    }
out:
    vec_free(&args);
    free(k);
    return id;
}

static uint32_t ex(El *e, uint32_t n, bool allow_void)
{
    Checker *c = e->c;
    uint32_t i = strip_paren(c, n), k[3];
    TypeId t;
    SrcLoc loc;
    bool isvoid;
    if (e->why[0])
        return TN;
    loc = cnode_loc(c, i);
    t = c->ty[i];
    if (c->ck[i] == K_ERR || type_ckind(&c->tt, t) == TY_ERROR)
        return va_kind(c, i) != VA_NONE
                   ? e_va(e, i, va_kind(c, i)) : tunsup(e, "type error");
    isvoid = type_is_void(&c->tt, t);
    if (isvoid) {
        if (!allow_void)
            return tunsup(e, "void value");
    } else {
        TypeKind uk = tkind(c, t);
        t = rvt(c, i);
        if (!ok_ty(e, t))
            return tunsup(e, "%s", why_type(e, t));
        if (c->ck[i] == K_FLOAT && is_flt(c, t) && !(c->ef[i] & EF_SIDE))
            return fconst(e, t, c->fv.data[c->cv[i]], loc);
        if ((c->ck[i] == K_ICE || c->ck[i] == K_FOLD ||
             (c->ck[i] == K_ADDR && !c->cb[i] &&
              (is_ptr(c, t) || tkind(c, t) == TY_NULLPTR))) &&
            !(c->ef[i] & EF_SIDE)) {   /* K_ADDR with no base: a null or absolute address */
            if (is_flt(c, t))
                return tunsup(e, "integer-valued floating constant");
            return cnst(e, t, c->cv[i], loc);
        }
        if (uk == TY_ARRAY || uk == TY_FUNC)      /* decay */
            return addr_t(e, lval(e, i), t, loc);
        if (c->ef[i] & EF_LVALUE)
            return load(e, lval(e, i));
    }
    switch (ntag(c, i)) {
    case N_INDEX:               /* a lane of a vector value (lval holds it in a compound literal) */
        return load(e, lval(e, i));
    case N_MEMBER_EXPR: {       /* f().x: a member of an aggregate value, which is its address */
        uint32_t d = first_child(c, i), base;
        const Field *f;
        uint64_t off = 0;
        unsigned q = 0;
        if (d == NO_NODE || (c->nodes[i].flags & NF_ARROW))
            return tunsup(e, "malformed member");
        base = rv(e, d);
        if (base == TN)
            return TN;
        f = find_field(c, c->ty[d], cnode_ident(c, i), &off, &q);
        if (!f)
            return tunsup(e, "member not found");
        if (f->flags & FF_BITFIELD)
            return tunsup(e, "bit-field of an rvalue");
        if (!ok_ty(e, t) || type_ckind(&c->tt, t) == TY_ARRAY)
            return tunsup(e, "%s", why_type(e, t));
        return mk(e, TO_RFIELD, t, loc, (int64_t)(off / 8), 0, 0, &base, 1);
    }
    case N_VA_ARG:
        return e_va(e, i, VA_ARG);
    case N_ADDR_LABEL:
        return mk(e, TO_LABADDR, t, loc, cnode_ident(c, i), 0, 0, NULL, 0);
    case N_CALL:
        return va_kind(c, i) != VA_NONE ? e_va(e, i, va_kind(c, i)) : e_call_t(e, i);
    case N_STMT_EXPR:
        return stmt_expr(e, i, t, loc);
    case N_CAST: {
        uint32_t x;
        if (nkids(c, i, k, 2) != 2)
            return tunsup(e, "malformed cast");
        if (isvoid) {
            x = ex(e, k[1], true);
            return x == TN ? TN : mk(e, TO_CAST, t, loc, 0, 0, 0, &x, 1);
        }
        if (vec_bitcast(e, t, k[1]))
            return vec_bits(e, k[1], t, loc);
        return conv(e, rv(e, k[1]), t, loc);
    }
    case N_CONVERTVECTOR:
        return vec_convert(e, i, first_child(c, i), loc);
    case N_UNARY: {
        const Tok *tk = cnode_tok(c, i);
        int p;
        uint32_t x, a;
        if (tk->kind != TK_PUNCT)       /* __extension__ */
            return ex(e, first_child(c, i), allow_void);
        p = npunct(c, i);
        if (p == P_INC || p == P_DEC)
            return incdec(e, i, first_child(c, i), p, false);
        if (p == P_AMP)
            return addr_t(e, lval(e, first_child(c, i)), t, loc);
        if (p != P_PLUS && p != P_MINUS && p != P_TILDE && p != P_BANG)
            return tunsup(e, "unary %s", punct_spelling[p]);
        a = first_child(c, i);
        if (tkind(c, rvt(c, a)) == TY_VECTOR)
            return vec_unary(e, i, p, a, loc);
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
                return tunsup(e, "malformed ?:");
            x = rv(e, k[0]);
            y = ex(e, k[1], isvoid);
            if (x == TN || y == TN)
                return TN;
            ka[0] = x;
            ka[1] = isvoid ? y : conv(e, y, t, loc);
            return mk(e, TO_ELVIS, t, loc, 0, 0, 0, ka, 2);
        }
        if (nk != 3)
            return tunsup(e, "malformed ?:");
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
            return tunsup(e, "malformed binary");
        if (p != P_COMMA && (tkind(c, rvt(c, k[0])) == TY_VECTOR || tkind(c, rvt(c, k[1])) == TY_VECTOR))
            return vec_binary(e, i, p, k[0], k[1], loc);
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
            TypeId ta = rvt(c, k[0]), tb = rvt(c, k[1]), ct;
            if (is_ptr(c, ta) || is_ptr(c, tb))
                ct = unqual(c, is_ptr(c, ta) ? ta : tb);
            else
                ct = common_type(c, promoted(c, k[0]), promoted(c, k[1]));
            x = rv(e, k[0]);
            y = rv(e, k[1]);
            if (x == TN || y == TN)
                return TN;
            ka[0] = conv(e, x, ct, loc);
            ka[1] = conv(e, y, ct, loc);
            return mk(e, TO_CMP, t, loc, 0, 0, (unsigned)p, ka, 2);
        }
        case P_PLUS: case P_MINUS:
            if (is_ptr(c, rvt(c, k[0])) || is_ptr(c, rvt(c, k[1])))
                return ptr_binary(e, i, p, k[0], k[1], loc);
            return arith(e, i, p, k[0], k[1], loc, &rt);
        case P_STAR: case P_SLASH: case P_PERCENT:
        case P_AMP: case P_PIPE: case P_CARET: case P_SHL: case P_SHR:
            return arith(e, i, p, k[0], k[1], loc, &rt);
        default:
            return tunsup(e, "binary %s", punct_spelling[p]);
        }
    }
    case N_OFFSETOF: {          /* an index that is not constant: the constant part plus index * size */
        uint32_t ok[64], nk = nkids(c, i, ok, 64), j, sum, ka[2];
        TypeId lt = size_type(c);
        if (c->cv[i] == ~(uint64_t)0)
            return tunsup(e, "offsetof in a variable length array");
        sum = cnst(e, lt, c->cv[i], loc);
        for (j = 1; j < nk; j++) {
            uint32_t x;
            if (ntag(c, ok[j]) != N_DESIG_INDEX || !c->cv[ok[j]])
                continue;
            x = rv(e, first_child(c, ok[j]));
            if (x == TN)
                return TN;
            ka[0] = conv(e, x, lt, loc);
            ka[1] = cnst(e, lt, c->cv[ok[j]], loc);
            ka[0] = mk(e, TO_BINOP, lt, loc, 0, 0, (unsigned)P_STAR, ka, 2);
            ka[1] = sum;
            sum = mk(e, TO_BINOP, lt, loc, 0, 0, (unsigned)P_PLUS, ka, 2);
        }
        return sum;
    }
    default:
        return tunsup(e, "expression %s", node_names[ntag(c, i)]);
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

/* ---- initializers (docs/TAST_T3_PLAN.md, section 3) -------------------------- */

typedef VEC(const TIRec *) TIRecPtrVec;

static int tir_cmp(const void *a, const void *b)
{
    const TIRec *x = *(TIRec *const *)a, *y = *(TIRec *const *)b;
    return x->bit0 < y->bit0 ? -1 : x->bit0 > y->bit0;
}

static bool tir_inside(const TIRec *r, uint64_t lo, uint64_t hi)
{
    return r->bit0 >= lo && r->bit0 + r->bits <= hi;
}

/* Drop the live elements the extent [lo, hi) of a later record replaces:
 * the ones before fence that lie inside it.  An overwritten
 * element is not evaluated even if it has side effects (C11 6.7.9p19; gcc
 * drops it too, with -Woverride-init-side-effects). */
static void tir_drop(El *e, TIRecPtrVec *live, uint64_t lo, uint64_t hi,
                     uint32_t fence)
{
    Checker *c = e->c;
    size_t m, w = 0;
    for (m = 0; m < live->len; m++) {
        const TIRec *r = live->data[m];
        if ((uint32_t)(r - c->tinit.data) < fence && tir_inside(r, lo, hi)) {
            continue;
        }
        live->data[w++] = r;
    }
    live->len = w;
}

/* The elements that initialize an object of type oty from init (an
 * INIT_LIST, or a bare expression), appended to out as elem nodes: one per
 * scalar, string or aggregate value, with the later of two overlapping
 * ones winning.  A static object's come sorted by offset; an automatic
 * object's keep source order (they are evaluated in it). */
static bool init_elems(El *e, uint32_t init, TypeId oty, bool is_static, SrcLoc loc,
                       U32Vec *out)
{
    Checker *c = e->c;
    TIRecPtrVec live = {0};
    size_t i, m;
    bool good = false;
    TypeId ou = unqual(c, oty);
    if (ntag(c, init) != N_INIT_LIST) {
        uint32_t x = strip_paren(c, init), ka[1], id;
        if (is_static && ntag(c, x) == N_COMPOUND_LIT && type_ckind(&c->tt, c->ty[x]) != TY_ARRAY) {
            uint32_t cl[2];     /* the literal's elements are the object's (gcc folds it) */
            if (nkids(c, x, cl, 2) < 2)
                return tunsup(e, "malformed compound literal"), false;
            return init_elems(e, cl[1], oty, is_static, loc, out);
        }
        if (type_ckind(&c->tt, ou) == TY_ARRAY) {
            if (ntag(c, x) != N_STRING) {
                tunsup(e, "array initializer form");
                return false;
            }
            ka[0] = lval(e, x);
            if (ka[0] == TN)
                return false;
            id = mk(e, TO_ELEM, ou, loc, 0, 0, 0, ka, 1);
        } else {
            ka[0] = conv(e, rv(e, init), ou, loc);
            if (ka[0] == TN)
                return false;
            id = mk(e, TO_ELEM, ou, loc, 0, 0, 0, ka, 1);
        }
        vec_push(out, id);
        return true;
    }
    for (i = 0; i < c->tinit.len; i++) {
        const TIRec *r = &c->tinit.data[i];
        if (r->list != init)
            continue;
        if (r->kind == TI_BAD) {
            tunsup(e, "flexible array or incomplete initializer");
            goto done;
        }
        if (r->kind == TI_CLEAR) {
            tir_drop(e, &live, r->bit0, r->bit0 + r->bits, r->fence);
            continue;
        }
        tir_drop(e, &live, r->bit0, r->bit0 + r->bits, (uint32_t)i);
        vec_push(&live, r);
    }
    if (is_static)
        qsort(live.data, live.len, sizeof *live.data, tir_cmp);
    for (m = 0; m < live.len; m++) {
        const TIRec *r = live.data[m];
        TypeId ty = unqual(c, r->ty);
        uint32_t ka[1], id, x;
        if (r->node == UINT32_MAX) {
            tunsup(e, "compound literal initializer");
            goto done;
        }
        x = strip_paren(c, r->node);
        if (r->str) {
            if (type_ckind(&c->tt, ty) == TY_ARRAY && (type_ent(&c->tt, type_canon(&c->tt, ty))->flags & TF_INCOMPLETE))
                ty = ou;       /* char t[] = "..": the completed type */
            if (ntag(c, x) != N_STRING) {
                tunsup(e, "string initializer form");
                goto done;
            }
            ka[0] = lval(e, x);
        } else {
            if (!ok_ty(e, ty)) {
                tunsup(e, "%s", why_type(e, ty));
                goto done;
            }
            ka[0] = conv(e, rv(e, r->node), ty, loc);
        }
        if (ka[0] == TN)
            goto done;
        id = mk(e, TO_ELEM, ty, loc, (int64_t)(r->bit0 / 8), 0, (unsigned)(r->bit0 % 8), ka, 1);
        e->nodes.data[id].width = r->width;
        vec_push(out, id);
    }
    good = true;
done:
    vec_free(&live);
    return good;
}

/* A static object: its elements (none: zero-initialized), where it is
 * declared. */
static uint32_t data_node(El *e, uint32_t ref, uint32_t init, SrcLoc loc)
{
    U32Vec el = {0};
    const CSym *s = csym(e->c, ref);
    uint32_t id = TN;
    if (!ok_ty(e, s->ty)) {
        tunsup(e, "%s", why_type(e, s->ty));
        return TN;
    }
    if (init == NO_NODE || init_elems(e, init, s->ty, true, loc, &el))
        id = mk(e, TO_DATA, s->ty, loc, 0, ref, 0, el.data, (uint32_t)el.len);
    vec_free(&el);
    return id;
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
            tunsup(e, "declaration without a symbol");
            break;
        }
        ref = c->cb[d] - 1;
        s = csym(c, ref);
        loc = cnode_loc(c, k[j]);
        if (s->kind == CS_TYPEDEF)
            continue;
        if (s->kind == CS_FUNC) {
            tunsup(e, "local function declaration");
            break;
        }
        if (s->sc == SC_EXTERN)     /* names a global declared elsewhere */
            continue;
        if (s->sc == SC_STATIC) {
            id = data_node(e, ref, init, loc);
            if (id == TN)
                break;
            vec_push(out, id);
            continue;
        }
        if (!ok_ty(e, s->ty)) {
            tunsup(e, "%s", why_type(e, s->ty));
            break;
        }
        if (init != NO_NODE) {
            if (ntag(c, init) == N_INIT_LIST || type_ckind(&c->tt, s->ty) == TY_ARRAY) {
                U32Vec el = {0};
                bool good = init_elems(e, init, s->ty, false, loc, &el);
                if (good)
                    id = mk(e, TO_LOCAL, s->ty, loc, 0, ref, 0, el.data, (uint32_t)el.len);
                vec_free(&el);
                if (!good)
                    break;
                vec_push(out, id);
                continue;
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
            tunsup(e, "malformed if");
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
            tunsup(e, "malformed loop");
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
            tunsup(e, "malformed for");
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
            tunsup(e, "malformed switch");
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
            tunsup(e, "case range");
            break;
        }
        if (m != 2 || !e->swty || c->ck[k[0]] != K_ICE) {
            tunsup(e, "case label form");
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
    case N_GOTO_EXPR:
        m = fkids(e, n, &k);
        if (m != 1) {
            tunsup(e, "malformed computed goto");
            break;
        }
        ka[0] = rv(e, k[0]);
        if (ka[0] != TN)
            id = mk(e, TO_IGOTO, 0, loc, 0, 0, 0, ka, 1);
        break;
    case N_ASM: {           /* the template text is the string kid; operands follow it: a string of
                             * their constraint letters, the outputs (lvalues), then the inputs */
        uint32_t q, ts = TN, sec[64], ns, ops[2][16], no[2] = {0, 0}, x[34], nx = 2;
        bool bad = false;
        m = fkids(e, n, &k);
        for (q = 0; q < m && !bad; q++) {
            if (ntag(c, k[q]) == N_STRING && ts == TN) {
                ts = k[q];
            } else if (ntag(c, k[q]) == N_ASM_SECTION) {
                unsigned which = c->nodes[k[q]].aux;
                ns = nkids(c, k[q], sec, 64);
                for (j = 0; j < ns; j++)
                    if (ntag(c, sec[j]) == N_ASM_OPERAND && (which == 1 || which == 2) &&
                        no[which - 1] < 16)
                        ops[which - 1][no[which - 1]++] = sec[j];
                    else if (ntag(c, sec[j]) == N_ASM_OPERAND)
                        bad = true;
            }
        }
        if (bad || ts == TN) {
            tunsup(e, "asm with operands");
            break;
        }
        x[0] = str_node(e, ts, c->ty[ts], loc);
        if (x[0] == TN)
            break;
        if (no[0] + no[1]) {
            uint32_t w, kk[4], nk;
            char let[40];
            size_t nl = 0;
            for (w = 0; w < 2 && !bad; w++)
                for (q = 0; q < no[w] && !bad; q++) {
                    uint32_t v, cs;
                    size_t len, b;
                    const char *t;
                    nk = nkids(c, ops[w][q], kk, 4);
                    if (nk < 2) {
                        bad = true;
                        break;
                    }
                    cs = kk[nk - 2];
                    t = ttext(c, c->nodes[cs].tok, &len);
                    for (b = 0; b < len && !isalpha((unsigned char)t[b]); b++)
                        ;
                    let[nl++] = b >= len ? '?' : memchr(t, '+', len) ? (char)toupper((unsigned char)t[b]) : t[b];
                    if (!ok_int(e, c->ty[kk[nk - 1]]) &&
                        type_ckind(&c->tt, c->ty[kk[nk - 1]]) != TY_PTR) {
                        tunsup(e, "asm operand type");
                        bad = true;
                        break;
                    }
                    v = w == 0 ? lval(e, kk[nk - 1]) : rv(e, kk[nk - 1]);
                    if (v == TN) {
                        bad = true;
                        break;
                    }
                    x[nx++] = v;
                }
            if (bad)
                break;
            let[nl] = 0;
            x[1] = str_bytes(e, let, nl, c->ty[ts], loc);
            id = mk(e, TO_ASM, 0, loc, (int64_t)no[0], 0, no[1], x, nx);
        } else {
            id = mk(e, TO_ASM, 0, loc, 0, 0, 0, x, 1);
        }
        break;
    }
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
            tunsup(e, "value returned from void function");
        } else {
            x = conv(e, rv(e, k[0]), e->ret, loc);
            if (x != TN)
                id = mk(e, TO_RETURN, 0, loc, 0, 0, 0, &x, 1);
        }
        break;
    }
    case N_PRAGMA:          /* a pragma between statements changes nothing here */
    case N_ATTR_STMT:       /* [[fallthrough]], __attribute__((fallthrough)) */
        id = nop(e, loc);
        break;
    default:
        tunsup(e, "statement %s", node_names[ntag(c, n)]);
        break;
    }
    free(k);
    return id;
}

/* ({ ... }): the block's statements, then (unless the type is void) the
 * last expression's value, converted to the expression's type. */
static uint32_t stmt_expr(El *e, uint32_t i, TypeId t, SrcLoc loc)
{
    Checker *c = e->c;
    uint32_t cp = first_child(c, i), v = stmt_expr_value(c, i), *k = NULL, m, j, id = TN;
    bool isvoid = type_is_void(&c->tt, t);
    U32Vec kids = {0};
    if (ntag(c, cp) != N_COMPOUND)
        return tunsup(e, "statement expression form");
    m = fkids(e, cp, &k);
    for (j = 0; j < m && !e->why[0]; j++) {
        if (!isvoid && j + 1 == m) {
            uint32_t x;
            if (ntag(c, k[j]) != N_EXPR_STMT || v == NO_NODE) {
                tunsup(e, "statement expression value form");
                break;
            }
            x = conv(e, rv(e, v), t, loc);
            if (x != TN)
                vec_push(&kids, x);
        } else if (ntag(c, k[j]) == N_DECL) {
            decl(e, k[j], &kids);
        } else if (ntag(c, k[j]) != N_STATIC_ASSERT) {
            vec_push(&kids, st(e, k[j]));
        }
    }
    if (!e->why[0])
        id = mk(e, TO_STMTEXPR, unqual(c, t), loc, 0, 0, 0, kids.data, (uint32_t)kids.len);
    vec_free(&kids);
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
    if (c->sx)                  /* the symbol index declaration (cindex_key identity) */
        fprintf(out, "=#%u", csx_decl_of(c, ref) - 1);
    if (ref & SYM_LOCAL) {
        SrcFile *f = srcmgr_file_of(c->sm, s->loc);
        uint32_t line = 0, col = 0;
        if (f)
            srcmgr_linecol(f, s->loc, &line, &col);
        fprintf(out, "@%u:%u", line, col);
    }
}

void tast_print_node(El *e, FILE *out, size_t i)
{
    Checker *c = e->c;
    uint32_t j;
    {
        const TNode *nd = &e->nodes.data[i];
        fprintf(out, "  #%zu %s", i, op_names[nd->op]);
        switch (nd->op) {
        case TO_CONST:
            if (type_is_signed(&c->tt, nd->ty))
                fprintf(out, " %lld", (long long)nd->val);
            else
                fprintf(out, " %llu", (unsigned long long)nd->val);
            break;
        case TO_FCONST: {
            uint32_t b;
            fputc(' ', out);
            for (b = nd->width; b-- > 0;)       /* most significant byte first */
                fprintf(out, "%02x", e->bytes.data[nd->val + b]);
            break;
        }
        case TO_TMP: case TO_REG:
            fprintf(out, " %lld", (long long)nd->val);
            break;
        case TO_COMPLIT:
            fprintf(out, " %lld%s", (long long)nd->val, nd->pun ? " static" : "");
            break;
        case TO_SYM: case TO_LOCAL: case TO_DATA:
            fputc(' ', out);
            put_sym(c, out, nd->sym);
            break;
        case TO_STR: {
            uint32_t b;
            fprintf(out, " u%u ", nd->pun);
            for (b = 0; b < nd->width; b++)
                fprintf(out, "%02x", e->bytes.data[nd->val + b]);
            break;
        }
        case TO_ELEM:
            fprintf(out, " +%lld", (long long)nd->val);
            if (nd->width)
                fprintf(out, " bit %u width %u", nd->pun, nd->width);
            break;
        case TO_CALL:
            if (nd->pun)
                fputs(" *", out);
            else {
                fputc(' ', out);
                put_sym(c, out, nd->sym);
            }
            if (nd->width)
                fprintf(out, " fixed %lld", (long long)nd->val);
            break;
        case TO_VA:
            fprintf(out, " %s", nd->pun == 1 ? "start" : nd->pun == 2 ? "end" : nd->pun == 3 ? "copy" : "arg");
            break;
        case TO_INTRIN:
            if (bi_rows[nd->pun].cls == BIC_ATOMIC) {
                fprintf(out, " %s %s", cident(c, nd->sym) + 2,
                        bi_rows[nd->pun].chk == BK_GENERIC ? "generic" : "sized");
                break;
            }
            fprintf(out, " %s", cident(c, nd->sym) + 10);
            if (nd->width)
                fprintf(out, " fixed %lld", (long long)nd->val);
            break;
        case TO_FIELD: case TO_RFIELD:
            fprintf(out, " +%lld", (long long)nd->val);
            if (nd->width)
                fprintf(out, " bit %u width %u", nd->pun, nd->width);
            break;
        case TO_PTRADD: case TO_PTRDIFF:
            fprintf(out, " *%lld", (long long)nd->val);
            break;
        case TO_UNOP: case TO_BINOP: case TO_CMP:
            fprintf(out, " %s", punct_spelling[nd->pun]);
            break;
        case TO_F2I:
            fputs(nd->pun ? " round" : " trunc", out);
            break;
        case TO_CASE:
            fprintf(out, " %lld", (long long)nd->val);
            break;
        case TO_GOTO: case TO_LABEL: case TO_LABADDR:
            fprintf(out, " %s", cident(c, (uint32_t)nd->val));
            break;
        default:
            break;
        }
        for (j = 0; j < nd->nkids; j++)
            fprintf(out, " #%u", e->kids.data[nd->kid0 + j]);
        if (IS_EXPR(nd->op) || nd->op == TO_LOCAL || nd->op == TO_ELEM || nd->op == TO_DATA) {
            fputc(' ', out);
            put_type(c, out, nd->ty);
        }
        fputc('\n', out);
    }
}

static void print_tree(El *e, FILE *out)
{
    size_t i;
    for (i = 0; i < e->nodes.len; i++)
        tast_print_node(e, out, i);
}

/* ---- the verifier (--verify-tast) ------------------------------------------ */

static void vbad(El *e, FILE *out, size_t i, const char *msg)
{
    if (e->c->opt.tast_bad)
        ++*e->c->opt.tast_bad;
    fprintf(out, "verify: #%zu %s: %s\n", i, op_names[e->nodes.data[i].op], msg);
}

static bool sconst(El *e, uint32_t i);

/* An lvalue whose address is a link-time constant. */
static bool slv(El *e, uint32_t i)
{
    const TNode *nd = &e->nodes.data[i];
    const uint32_t *k = e->kids.data + nd->kid0;
    switch (nd->op) {
    case TO_SYM: {
        const CSym *s = csym(e->c, nd->sym);
        return !(nd->sym & SYM_LOCAL) || s->sc == SC_STATIC || s->sc == SC_EXTERN ||
               s->kind == CS_FUNC;
    }
    case TO_STR: return true;
    case TO_COMPLIT: return nd->pun != 0;    /* its own check holds the elems constant */
    case TO_FIELD: return slv(e, k[0]);
    case TO_DEREF: return sconst(e, k[0]);
    default: return false;
    }
}

/* A value computed at translation time from constants and addresses. */
static bool sconst(El *e, uint32_t i)
{
    const TNode *nd = &e->nodes.data[i];
    const uint32_t *k = e->kids.data + nd->kid0;
    uint32_t j;
    switch (nd->op) {
    case TO_CONST: case TO_FCONST: case TO_LABADDR: return true;
    case TO_ADDR: return slv(e, k[0]);
    case TO_CAST: case TO_F2I: case TO_UNOP: case TO_BINOP: case TO_CMP: case TO_LOGAND:
    case TO_LOGOR: case TO_COND: case TO_PTRADD: case TO_PTRDIFF:
        for (j = 0; j < nd->nkids; j++)
            if (!sconst(e, k[j]))
                return false;
        return true;
    default: return false;
    }
}

/* The elem kids of a data or local node: inside the object, and for a
 * static object also ordered and disjoint. */
static void velems(El *e, FILE *out, size_t i, bool is_static)
{
    Checker *c = e->c;
    const TNode *nd = &e->nodes.data[i];
    const uint32_t *k = e->kids.data + nd->kid0;
    bool ok = false;
    uint64_t os = type_size(&c->tt, nd->ty, &ok) * 8, end = 0;
    uint32_t j;
    for (j = 0; j < nd->nkids; j++) {
        const TNode *el = &e->nodes.data[k[j]];
        uint64_t lo, sz;
        if (el->op != TO_ELEM) {
            vbad(e, out, i, "kid is not an elem");
            return;
        }
        sz = el->width ? el->width : 8 * type_size(&c->tt, el->ty, &ok);
        lo = (uint64_t)el->val * 8 + (el->width ? el->pun : 0);
        if (ok && lo + sz > os)
            vbad(e, out, i, "elem outside the object");
        if (is_static && lo < end)
            vbad(e, out, i, "elems overlap or are out of order");
        if (is_static && el->nkids == 1 && e->nodes.data[e->kids.data[el->kid0]].op != TO_STR &&
            !sconst(e, e->kids.data[el->kid0]))
            vbad(e, out, i, "static initializer is not constant");
        end = lo + sz;
    }
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
    uint32_t *tstores = xcalloc(e->ntmp ? e->ntmp : 1, sizeof *tstores);
    TypeId *ttype = xcalloc(e->ntmp ? e->ntmp : 1, sizeof *ttype);
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
#define ISVAL(p) (IS_EXPR((p)->op) && !is_lvop((p)->op))
#define ISLV(p) is_lvop((p)->op)
#define KIND(t) type_ckind(&c->tt, (t))
#define ISSTMT(p) (!IS_EXPR((p)->op) && (p)->op != TO_ELEM)
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
        if (ex_ && !VOIDT && !ok_ty(e, nd->ty) &&
            !((nd->op == TO_DEREF || nd->op == TO_SYM) && KIND(nd->ty) == TY_FUNC) &&
            !(nd->op == TO_FIELD && KIND(nd->ty) == TY_ARRAY && ok_ty(e, elem_of(c, nd->ty))))
            vbad(e, out, i, "type outside the subset");
        if (ex_ && VOIDT && nd->op != TO_CAST && nd->op != TO_CALL &&
            nd->op != TO_COND && nd->op != TO_COMMA && nd->op != TO_STMTEXPR &&
            nd->op != TO_INTRIN && nd->op != TO_VA)
            vbad(e, out, i, "void type");
        switch (nd->op) {
        case TO_CONST: NEED(0); break;
        case TO_FCONST: {
            bool sz_ok = false;
            NEED(0);
            if (!is_flt(c, nd->ty) ||
                nd->width != type_size(&c->tt, nd->ty, &sz_ok) || !sz_ok ||
                (size_t)nd->val + nd->width > e->bytes.len)
                vbad(e, out, i, "floating constant bytes");
            break;
        }
        case TO_SYM:
            NEED(0);
            if (c->sx && !csx_decl_of(c, nd->sym))
                vbad(e, out, i, "symbol has no index declaration");
            if (csym(c, nd->sym)->kind != CS_OBJ && csym(c, nd->sym)->kind != CS_FUNC)
                vbad(e, out, i, "not a variable or function");
            break;
        case TO_TMP:
            NEED(0);
            if ((uint64_t)nd->val >= e->ntmp || (KIND(nd->ty) != TY_PTR && KIND(nd->ty) != TY_VECTOR && !ok_int(e, nd->ty))) {
                vbad(e, out, i, "temporary");
            } else if (ttype[nd->val] && ttype[nd->val] != nd->ty) {
                vbad(e, out, i, "temporary has two types");
            } else {
                ttype[nd->val] = nd->ty;
            }
            break;
        case TO_LOAD:
            NEED(1);
            if (KID(0)->op == TO_TMP && (uint64_t)KID(0)->val < e->ntmp &&
                !tstores[KID(0)->val])
                vbad(e, out, i, "temporary loaded before it is stored");
            if (!ISLV(KID(0)))
                vbad(e, out, i, "operand is not an lvalue");
            else if (KIND(KID(0)->ty) == TY_ARRAY || KIND(KID(0)->ty) == TY_FUNC)
                vbad(e, out, i, "load of an array or function");
            else if (nd->ty != unqual(c, KID(0)->ty))
                vbad(e, out, i, "type is not the unqualified variable type");
            break;
        case TO_STORE:
            NEED(2);
            if (KID(0)->op == TO_TMP && (uint64_t)KID(0)->val < e->ntmp)
                tstores[KID(0)->val]++;
            if (!ISLV(KID(0)) || !ISVAL(KID(1)))
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
        case TO_F2I:
            NEED(1);
            if (!ISVAL(KID(0)) || !is_flt(c, KID(0)->ty) || (nd->ty != ia_ty(IAL_S32) && nd->ty != ia_ty(IAL_S64)))
                vbad(e, out, i, "float to int");
            break;
        case TO_UNOP:
            NEED(1);
            if (!ISVAL(KID(0)))
                vbad(e, out, i, "operand");
            else if (nd->pun == P_BANG ? nd->ty != TYPE_B(INT) : !SAME(KID(0), nd))
                vbad(e, out, i, "operand type");
            else if (is_flt(c, nd->ty) && nd->pun != P_MINUS)
                vbad(e, out, i, "operator on a floating operand");
            break;
        case TO_BINOP:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)))
                vbad(e, out, i, "operands");
            else if (!SAME(KID(0), nd) ||
                     (nd->pun != P_SHL && nd->pun != P_SHR && !SAME(KID(1), nd)))
                vbad(e, out, i, "operand types");
            else if (is_flt(c, nd->ty) && nd->pun != P_PLUS && nd->pun != P_MINUS &&
                     nd->pun != P_STAR && nd->pun != P_SLASH)
                vbad(e, out, i, "operator on floating operands");
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
            if (!IS_EXPR(KID(0)->op) || ISLV(KID(0)) ||
                !IS_EXPR(KID(1)->op) || !SAME(KID(1), nd))
                vbad(e, out, i, "operands");
            break;
        case TO_POST:
            NEED(1);
            if (KID(0)->op != TO_STORE || !SAME(KID(0), nd))
                vbad(e, out, i, "operand is not a store of the same type");
            break;
        case TO_VA: {
            uint32_t want = nd->pun == 3 ? 2 : 1;
            if (nd->pun < 1 || nd->pun > 4 || nd->nkids != want || (nd->pun == 4) == VOIDT) {
                vbad(e, out, i, "va operands");
                break;
            }
            for (j = 0; j < nd->nkids; j++)
                if (!ISVAL(KID(j)) || KIND(KID(j)->ty) != TY_PTR)
                    vbad(e, out, i, "va_list operand is not a pointer");
            break;
        }
        case TO_CALL: case TO_INTRIN: {
            TypeId ft = 0;
            const TypeEnt *fe;
            bool sz_ok = false;
            uint32_t f0 = nd->op == TO_CALL && nd->pun ? 1 : 0, np;
            if (nd->op == TO_INTRIN && bi_rows[nd->pun].fl) {
                if (nd->pun != intrin_row(c, nd->sym))
                    vbad(e, out, i, "intrinsic row");
                for (j = 0; j < nd->nkids; j++)
                    if (!ISVAL(KID(j)) || (KIND(KID(j)->ty) != TY_PTR && !is_int(c, KID(j)->ty)))
                        vbad(e, out, i, "argument is not a pointer or integer value");
                break;
            }
            if (nd->op == TO_INTRIN) {
                const char *nm = cident(c, nd->sym);
                const BTab *bt = strncmp(nm, "__builtin_", 10) ? NULL : bt_find(c, nm + 10, true);
                if (!bt || bt->gnu == 3 || !strchr(bt->sig, '|')) {
                    vbad(e, out, i, "builtin");
                    break;
                }
                if (nd->pun != intrin_row(c, nd->sym))
                    vbad(e, out, i, "builtin row");
                ft = type_canon(&c->tt, bt_func_type(c, bt));
            } else if (nd->pun) {
                if (nd->nkids < 1 || !ISVAL(KID(0)) || KIND(KID(0)->ty) != TY_PTR ||
                    KIND(pointee(c, KID(0)->ty)) != TY_FUNC) {
                    vbad(e, out, i, "callee");
                    break;
                }
                ft = type_canon(&c->tt, pointee(c, KID(0)->ty));
            } else {
                const CSym *s = csym(c, nd->sym);
                if (c->sx && !csx_decl_of(c, nd->sym))
                    vbad(e, out, i, "symbol has no index declaration");
                if (s->kind != CS_FUNC) {
                    vbad(e, out, i, "callee");
                    break;
                }
                ft = type_canon(&c->tt, s->ty);
            }
            if (KIND(ft) != TY_FUNC) {
                vbad(e, out, i, "callee");
                break;
            }
            fe = type_ent(&c->tt, ft);
            np = (fe->flags & TF_NOPROTO) ? 0 : (uint32_t)fe->n;
            if (nd->width != ((fe->flags & (TF_NOPROTO | TF_VARIADIC)) != 0) ||
                nd->val != np) {
                vbad(e, out, i, "fixed argument count");
                break;
            }
            if (nd->nkids - f0 < np || (!nd->width && nd->nkids - f0 != np)) {
                vbad(e, out, i, "argument count");
                break;
            }
            for (j = f0; j < nd->nkids; j++) {
                if (!ISVAL(KID(j))) {
                    vbad(e, out, i, "argument is not a value");
                } else if (j - f0 < np) {
                    if (mainv(c, KID(j)->ty) != mainv(c, type_params(&c->tt, ft)[j - f0]))
                        vbad(e, out, i, "argument type");
                } else if (is_int(c, KID(j)->ty) &&
                           (KIND(KID(j)->ty) == TY_BOOL ||
                            (KIND(KID(j)->ty) != TY_ENUM &&
                             type_size(&c->tt, KID(j)->ty, &sz_ok) < 4))) {
                    vbad(e, out, i, "extra argument not promoted");
                } else if (KIND(KID(j)->ty) == TY_FLOAT) {
                    vbad(e, out, i, "extra argument not promoted");
                }
            }
            break;
        }
        case TO_DEREF:
            NEED(1);
            if (!ISVAL(KID(0)) || KIND(KID(0)->ty) != TY_PTR)
                vbad(e, out, i, "operand is not a pointer value");
            else if (mainv(c, pointee(c, KID(0)->ty)) != mainv(c, nd->ty))
                vbad(e, out, i, "type is not the pointee type");
            break;
        case TO_STR:
            NEED(0);
            if (KIND(nd->ty) != TY_ARRAY)
                vbad(e, out, i, "not an array");
            break;
        case TO_FIELD: {
            bool ok = false;
            uint64_t rs = 0;
            NEED(1);
            if (nd->width && ISLV(KID(0)) && !is_record(c, KID(0)->ty) && KID(0)->op == TO_DEREF) {
                /* a bit-field named again through the temporary that holds its byte's address */
                if (nd->val || !ok_int(e, nd->ty) || nd->width > 8 * type_size(&c->tt, nd->ty, &ok))
                    vbad(e, out, i, "bit-field type or width");
                break;
            }
            if (!ISLV(KID(0)) || !(is_record(c, KID(0)->ty) || KIND(KID(0)->ty) == TY_VECTOR)) {
                vbad(e, out, i, "operand is not a struct, union or vector lvalue");
                break;
            }
            rs = type_size(&c->tt, KID(0)->ty, &ok);
            if (ok && (uint64_t)nd->val * 8 + nd->pun + (nd->width ? nd->width : 8 * type_size(&c->tt, nd->ty, &ok)) > rs * 8)
                vbad(e, out, i, "member outside the record");
            if (nd->width && (!ok_int(e, nd->ty) || nd->width > 8 * type_size(&c->tt, nd->ty, &ok)))
                vbad(e, out, i, "bit-field type or width");
            break;
        }
        case TO_RFIELD: {
            bool ok = false;
            uint64_t rs;
            NEED(1);
            if (!ISVAL(KID(0)) || !is_record(c, KID(0)->ty)) {
                vbad(e, out, i, "operand is not a struct or union value");
                break;
            }
            rs = type_size(&c->tt, KID(0)->ty, &ok);
            if (ok && (uint64_t)nd->val + type_size(&c->tt, nd->ty, &ok) > rs)
                vbad(e, out, i, "member outside the record");
            break;
        }
        case TO_ADDR:
            NEED(1);
            if (!ISLV(KID(0)) || KIND(nd->ty) != TY_PTR)
                vbad(e, out, i, "operand or type");
            else if (mainv(c, pointee(c, nd->ty)) != mainv(c, KID(0)->ty) &&
                     !(KIND(KID(0)->ty) == TY_ARRAY &&
                       mainv(c, pointee(c, nd->ty)) == mainv(c, elem_of(c, KID(0)->ty))))
                vbad(e, out, i, "pointer type does not match the object");
            break;
        case TO_PTRADD:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)) || KIND(KID(0)->ty) != TY_PTR ||
                mainv(c, KID(1)->ty) != mainv(c, ptrdiff_ty(e)) || !SAME(KID(0), nd))
                vbad(e, out, i, "operands");
            else if ((uint64_t)nd->val != elem_size(c, nd->ty))
                vbad(e, out, i, "element size");
            break;
        case TO_PTRDIFF:
            NEED(2);
            if (!ISVAL(KID(0)) || !ISVAL(KID(1)) || KIND(KID(0)->ty) != TY_PTR ||
                KIND(KID(1)->ty) != TY_PTR || nd->ty != ptrdiff_ty(e))
                vbad(e, out, i, "operands");
            else if ((uint64_t)nd->val != elem_size(c, KID(0)->ty))
                vbad(e, out, i, "element size");
            break;
        case TO_STMTEXPR:       /* statements, then the value unless void */
            for (j = 0; j < nd->nkids; j++)
                if (j + 1 == nd->nkids && !VOIDT ? !(ISVAL(KID(j)) && SAME(KID(j), nd))
                                                 : !ISSTMT(KID(j)))
                    vbad(e, out, i, j + 1 == nd->nkids && !VOIDT ? "value" : "item is not a statement");
            if (!VOIDT && !nd->nkids)
                vbad(e, out, i, "no value");
            break;
        case TO_BLOCK:
            for (j = 0; j < nd->nkids; j++)
                if (!ISSTMT(KID(j)))
                    vbad(e, out, i, "item is not a statement");
            break;
        case TO_LOCAL:
            if (c->sx && !csx_decl_of(c, nd->sym))
                vbad(e, out, i, "symbol has no index declaration");
            if (nd->nkids && KID(0)->op == TO_ELEM)
                velems(e, out, i, false);
            else if (nd->nkids > 1 || (nd->nkids && (!ISVAL(KID(0)) || !SAME(KID(0), nd))))
                vbad(e, out, i, "initializer");
            break;
        case TO_COMPLIT:
            if ((uint64_t)nd->val >= e->ncl || nd->pun > 1)
                vbad(e, out, i, "compound literal number or storage");
            if (nd->nkids && KID(0)->op == TO_ELEM)
                velems(e, out, i, nd->pun);
            else
                vbad(e, out, i, "initializer");
            break;
        case TO_DATA:
            if (c->sx && !csx_decl_of(c, nd->sym))
                vbad(e, out, i, "symbol has no index declaration");
            if (csym(c, nd->sym)->kind != CS_OBJ || nd->ty != csym(c, nd->sym)->ty)
                vbad(e, out, i, "not an object of the symbol's type");
            velems(e, out, i, true);
            break;
        case TO_ELEM:
            NEED(1);
            if (nd->width && !ok_int(e, nd->ty))
                vbad(e, out, i, "bit-field type");
            if (KIND(nd->ty) == TY_ARRAY ? KID(0)->op != TO_STR : !ISVAL(KID(0)) || !SAME(KID(0), nd))
                vbad(e, out, i, "operand");
            break;
        case TO_EXPRSTMT:
            NEED(1);
            if (!IS_EXPR(KID(0)->op) || ISLV(KID(0)))
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
        case TO_GOTO: case TO_LABADDR: {
            size_t q;
            if (nd->op == TO_LABADDR && (nd->nkids || KIND(nd->ty) != TY_PTR))
                vbad(e, out, i, "label address is not a leaf pointer");
            for (q = 0; q < labels.len && labels.data[q] != (uint32_t)nd->val; q++)
                ;
            if (q == labels.len)
                vbad(e, out, i, "goto without a label");
            break;
        }
        case TO_ASM: {
            uint32_t q;
            if (nd->nkids == 1 && KID(0)->op == TO_STR && !nd->val && !nd->pun)
                break;
            if (nd->nkids < 2 || KID(0)->op != TO_STR || KID(1)->op != TO_STR ||
                nd->nkids != 2 + nd->val + nd->pun) {
                vbad(e, out, i, "template or operands");
                break;
            }
            for (q = 0; q < nd->val + nd->pun; q++)
                if (nd->val > q ? !ISLV(KID(2 + q)) : !ISVAL(KID(2 + q)))
                    vbad(e, out, i, "operand form");
            break;
        }
        case TO_IGOTO:
            NEED(1);
            if (!ISVAL(KID(0)) || KIND(KID(0)->ty) != TY_PTR)
                vbad(e, out, i, "computed goto operand is not a pointer value");
            break;
        default:
            break;
        }
next:;
#undef KID
#undef NEED
#undef ISVAL
#undef ISSTMT
#undef ISLV
#undef KIND
#undef SAME
#undef VOIDT
    }
    for (i = 0; i < n; i++)
        if (refs[i] != (i + 1 == n ? 0 : 1))
            vbad(e, out, i, i + 1 == n ? "root is used" : "not used exactly once");
    if (n && e->nodes.data[n - 1].op != TO_BLOCK)
        vbad(e, out, n - 1, "root is not a block");
    for (j = 0; j < e->ntmp; j++)
        if (tstores[j] != 1)
            vbad(e, out, n - 1, "temporary is not stored exactly once");
    vec_free(&labels);
    free(tstores);
    free(ttype);
    free(refs);
}

/* ---- the unit -------------------------------------------------------------- */

/* The declared symbols of a unit that is only declarations: printed (the
 * tree dump), and defined as data by the back end (assembly, object). */
static void print_decls(Checker *c, El *e, uint32_t root, FILE *out)
{
    uint32_t nk = nkids(c, root, NULL, 0), *k, j, any = 0;
    bool show = c->opt.tast_tree;
    k = xmalloc((nk ? nk : 1) * sizeof *k);
    nkids(c, root, k, nk);
    U32Vec items = {0};
    for (j = 0; j < nk && !e->why[0]; j++) {     /* initialized objects: data nodes */
        uint32_t d, init, id;
        if (ntag(c, k[j]) != N_INIT_DECL)
            continue;
        d = declared_of(e, k[j], &init);
        if (d == NO_NODE || !c->cb[d] || init == NO_NODE ||
            csym(c, c->cb[d] - 1)->kind != CS_OBJ)
            continue;
        id = data_node(e, c->cb[d] - 1, init, cnode_loc(c, k[j]));
        if (id != TN)
            vec_push(&items, id);
    }
    for (j = 0; j < nk && !e->why[0]; j++) {
        uint32_t d, init;
        if (ntag(c, k[j]) != N_INIT_DECL)
            continue;
        d = declared_of(e, k[j], &init);
        if (d == NO_NODE || !c->cb[d])
            continue;
        any = 1;
        if (init != NO_NODE && csym(c, c->cb[d] - 1)->kind == CS_OBJ)
            continue;
        if (CHK_ASM(c->opt) && csym(c, c->cb[d] - 1)->kind == CS_OBJ &&
            csym(c, c->cb[d] - 1)->sc != SC_EXTERN)
            x86_tentative(c, c->cb[d] - 1);
        if (c->opt.eval_main && csym(c, c->cb[d] - 1)->kind == CS_OBJ &&
            csym(c, c->cb[d] - 1)->sc != SC_EXTERN)
            eval_tentative(c, c->cb[d] - 1);
        if (!show)
            continue;
        fputs("unit ", out);
        put_loc(c, out, cnode_loc(c, k[j]));
        fputs(" decl ", out);
        put_sym(c, out, c->cb[d] - 1);
        fputc(' ', out);
        put_type(c, out, csym(c, c->cb[d] - 1)->ty);
        fputc('\n', out);
    }
    if (c->opt.eval_main && !e->why[0] && items.len)
        eval_data(c, e);
    if (CHK_ASM(c->opt) && !e->why[0]) {
        size_t m;
        for (m = 0; m < items.len; m++) {
            char why[96];
            if (!x86_data_node(e, items.data[m], why, sizeof why))
                fprintf(out, "# x86: %s unsupported: %s\n",
                        cident(c, csym(c, e->nodes.data[items.data[m]].sym)->name), why);
        }
    }
    if (!show) {
        vec_free(&items);
        free(k);
        return;
    }
    if (items.len && !e->why[0]) {
        block_of(e, &items, cnode_loc(c, root));
        fputs("unit ", out);
        put_loc(c, out, cnode_loc(c, root));
        fputs(" data\n", out);
        print_tree(e, out);
        if (c->opt.verify_tast)
            verify(e, out);
    } else if (!any && !e->why[0]) {
        fputs("unit ", out);
        put_loc(c, out, cnode_loc(c, root));
        fputs(" decl\n", out);
    }
    vec_free(&items);
    free(k);
}

static void func_def(El *e, uint32_t root)
{
    Checker *c = e->c;
    uint32_t nk = nkids(c, root, NULL, 0), *k, i, j, d = NO_NODE, body = NO_NODE;
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
            tunsup(e, "K&R definition");
    }
    free(k);
    if (e->why[0])
        return;
    if (d == NO_NODE || !c->cb[d] || body == NO_NODE) {
        tunsup(e, "function without a symbol");
        return;
    }
    s = csym(c, c->cb[d] - 1);
    ft = type_canon(&c->tt, s->ty);
    fe = type_ent(&c->tt, ft);
    if ((fe->flags & TF_NOPROTO) && fe->n) {
        tunsup(e, "unprototyped definition");
        return;
    }
    e->fname = s->name;
    e->infunc = true;
    e->ret = unqual(c, fe->base);
    if (!type_is_void(&c->tt, e->ret) && !ok_ty(e, e->ret)) {
        tunsup(e, "%s", why_type(e, e->ret));
        return;
    }
    for (j = 0; j < fe->n; j++) {
        TypeId pt = unqual(c, type_params(&c->tt, ft)[j]);
        if (!ok_ty(e, pt)) {
            tunsup(e, "%s", why_type(e, pt));
            return;
        }
    }
    for (i = cfirst(c, root); i < cfirst(c, body); i++) {
        if (!is_real_param(c, i) || !c->cb[i] || type_ckind(&c->tt, c->ty[i]) == TY_VOID)
            continue;           /* not f(void) */
        while (e->params.len && e->params.data[e->params.len - 1] >= cfirst(c, i))
            e->params.len--;    /* a parameter of a parameter's own type */
        vec_push(&e->params, i);
    }
    st(e, body);
}

/* Where the output about a header's functions goes. */
static FILE *discard(void)
{
#ifdef _WIN32
    return fopen("NUL", "w");
#else
    return fopen("/dev/null", "w");
#endif
}

static void unit_run(Checker *c, FILE *out)
{
    El e;
    uint32_t root = c->nn - 1;
    const char *pf;
    bool sys;
    SrcLoc loc = cnode_loc(c, root);
    unsigned tag = ntag(c, root);
    if (tag != N_FUNC_DEF && tag != N_DECL && tag != N_TOP_ASM)
        return;
    sys = cin_system(c, loc);
    pf =CHK_ASM(c->opt) && !c->opt.dump_cfg && !c->opt.tast_tree ? "# " : "";
    if (c->unit_err) {
        fputs(pf, out);
        fputs("unit ", out);
        put_loc(c, out, loc);
        fputs(" skipped: errors\n", out);
        return;
    }
    if (tag == N_DECL && !c->opt.tast_tree && !c->opt.eval_main && !CHK_ASM(c->opt))
        return;
    memset(&e, 0, sizeof e);
    e.c = c;
    if (tag == N_DECL) {
        print_decls(c, &e, root, out);
        if (e.why[0]) {
            fputs(pf, out);
            fputs("unit ", out);
            put_loc(c, out, loc);
            fprintf(out, " unsupported: %s\n", e.why);
        }
    } else {
        if (tag == N_TOP_ASM)
            tunsup(&e, "top-level asm");
        else
            func_def(&e, root);
        fputs(pf, out);
        fputs("unit ", out);
        put_loc(c, out, loc);
        if (e.why[0]) {
            fprintf(out, " unsupported: %s\n", e.why);
            if (sys)
                x86_func_fail(c, cident(c, csym(c, c->cb[declared_of(&e, root, NULL)] - 1)->name), e.why);
        } else {
            uint32_t d = declared_of(&e, root, NULL);
            e.fsym = c->cb[d] - 1;
            fputs(" func ", out);
            put_sym(c, out, c->cb[d] - 1);
            fputc(' ', out);
            put_type(c, out, csym(c, c->cb[d] - 1)->ty);
            fputc('\n', out);
            if (c->opt.tast_tree) {
                print_tree(&e, out);
                if (c->opt.verify_tast)
                    verify(&e, out);
            }
            if (c->opt.dump_cfg || c->opt.eval_main || CHK_ASM(c->opt))
                tast_cfg(&e, (uint32_t)e.nodes.len - 1, out);
        }
    }
    vec_free(&e.nodes);
    vec_free(&e.kids);
    vec_free(&e.bytes);
    vec_free(&e.params);
}

void tast_unit(Checker *c)
{
    uint32_t root = c->nn - 1;
    unsigned tag = ntag(c, root);
    if (tag == N_FUNC_DEF || tag == N_DECL || tag == N_TOP_ASM) {
        if (cin_system(c, cnode_loc(c, root))) {     /* a header's functions are emitted when used */
            FILE *d = tag == N_FUNC_DEF && CHK_ASM(c->opt) ? discard() : NULL;
            if (d) {
                unit_run(c, d);
                fclose(d);
            }
            return;
        }
    }
    unit_run(c, c->opt.tast);
}
