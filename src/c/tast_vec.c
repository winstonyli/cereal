/* tast_vec.c - vector types in the typed AST: an operation on vectors is the scalar operation on
 * every lane (docs/TAST_DESIGN.md).  Split from tast.c. */
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
#include "c/lit.h"

#include "c/tast_int.h"



/* ---- vector types ------------------------------------------------------------
 * A vector is a memory aggregate: the value of one is its address.  An operation on vectors
 * is the scalar operation on every lane.  The result is a compound literal whose elements are
 * the lane expressions; a lane of an operand is read from the operand's own memory (an lvalue
 * without side effects is named again for every lane, any other operand is stored into a
 * temporary first). */

/* The lane type and the number of lanes of a vector type. */
bool vec_dims(El *e, TypeId vt, TypeId *et, uint32_t *n)
{
    Checker *c = e->c;
    bool ok = false;
    uint64_t w, bytes;
    *et = unqual(c, elem_of(c, vt));
    w = type_size(&c->tt, *et, &ok);
    bytes = ok ? type_size(&c->tt, vt, &ok) : 0;
    if (!ok || !w || !(ok_int(e, *et) || (is_flt(c, *et) && type_ckind(&c->tt, *et) != TY_LDOUBLE))) {
        tunsup(e, "vector element type");
        return false;
    }
    *n = (uint32_t)(bytes / w);
    return true;
}

/* The vector value v, of type st, as an operand: stored into a temporary. */
static bool vop_value(El *e, uint32_t v, TypeId st, VOp *o, U32Vec *setup, SrcLoc loc)
{
    uint32_t ka[2], s;
    if (v == TN)
        return false;
    o->st = unqual(e->c, st);
    o->x = TN;
    o->tmp = e->ntmp++;
    ka[0] = mk(e, TO_TMP, o->st, loc, o->tmp, 0, 0, NULL, 0);
    ka[1] = v;
    s = mk(e, TO_STORE, o->st, loc, 0, 0, 0, ka, 2);
    vec_push(setup, mk(e, TO_EXPRSTMT, 0, loc, 0, 0, 0, &s, 1));
    return true;
}

/* The vector value v, of type vt, as an operand. */
bool vop_val(El *e, uint32_t v, TypeId vt, VOp *o, U32Vec *setup, SrcLoc loc)
{
    uint32_t n;
    o->vt = unqual(e->c, vt);
    o->scalar = false;
    return vec_dims(e, o->vt, &o->et, &n) && vop_value(e, v, vt, o, setup, loc);
}

/* The checker's node k as an operand of a vector operation whose vector type is vt. */
bool vop_node(El *e, uint32_t k, TypeId vt, VOp *o, U32Vec *setup, SrcLoc loc)
{
    Checker *c = e->c;
    uint32_t x = strip_paren(c, k), kk[2], n;
    for (;;) {                  /* a cast between vector types only names the same bytes differently */
        bool ok = false, ok2 = false;
        if (ntag(c, x) == N_CAST && tkind(c, c->ty[x]) == TY_VECTOR && nkids(c, x, kk, 2) == 2 &&
            tkind(c, c->ty[kk[1]]) == TY_VECTOR &&
            type_size(&c->tt, c->ty[x], &ok) == type_size(&c->tt, c->ty[kk[1]], &ok2) && ok && ok2)
            x = strip_paren(c, kk[1]);
        else
            break;
    }
    o->tmp = UINT32_MAX;
    o->x = x;
    o->scalar = false;
    if (tkind(c, c->ty[x]) != TY_VECTOR) {      /* a scalar: every lane is its value */
        if (c->ef[x] & EF_SIDE) {
            tunsup(e, "side effect in a scalar operand of a vector operation");
            return false;
        }
        o->vt = unqual(c, vt);
        o->scalar = true;
        return vec_dims(e, o->vt, &o->et, &n);
    }
    o->vt = unqual(c, rvt(c, k));       /* the lanes as the operation sees them */
    o->st = unqual(c, c->ty[x]);
    if (!vec_dims(e, o->vt, &o->et, &n))
        return false;
    if ((c->ef[x] & EF_LVALUE) && !(c->ef[x] & EF_SIDE))
        return true;
    return vop_value(e, rv(e, x), o->st, o, setup, loc);
}

/* Lane j of an operand, a value of the lane type. */
uint32_t vop_lane(El *e, const VOp *o, uint32_t j, SrcLoc loc)
{
    uint32_t b, f;
    bool ok = false;
    if (o->scalar)
        return conv(e, rv(e, o->x), o->et, loc);
    b = o->tmp != UINT32_MAX ? mk(e, TO_TMP, o->st, loc, o->tmp, 0, 0, NULL, 0) : lval(e, o->x);
    if (b == TN)
        return TN;
    f = mk(e, TO_FIELD, o->et, loc, (int64_t)(j * type_size(&e->c->tt, o->et, &ok)), 0, 0, &b, 1);
    return load(e, f);
}

/* The value v of type t, after the statements in setup. */
uint32_t vec_wrap(El *e, uint32_t v, TypeId t, const U32Vec *setup, SrcLoc loc)
{
    size_t s;
    for (s = setup->len; v != TN && s-- > 0;) {
        uint32_t ka[2];
        ka[0] = setup->data[s];
        ka[1] = v;
        v = mk(e, TO_STMTEXPR, t, loc, 0, 0, 0, ka, 2);
    }
    return v;
}

/* The vector of type vt whose lanes are lanes[0 .. n), after the statements in setup. */
uint32_t vec_make(El *e, TypeId vt, SrcLoc loc, const uint32_t *lanes, uint32_t n,
                         const U32Vec *setup)
{
    Checker *c = e->c;
    U32Vec el = {0};
    TypeId et = unqual(c, elem_of(c, vt));
    bool ok = false;
    uint64_t w = type_size(&c->tt, et, &ok);
    uint32_t j, v;
    for (j = 0; j < n; j++) {
        uint32_t id;
        if (lanes[j] == TN) {
            vec_free(&el);
            return TN;
        }
        id = mk(e, TO_ELEM, et, loc, (int64_t)(j * w), 0, 0, &lanes[j], 1);
        vec_push(&el, id);
    }
    v = mk(e, TO_COMPLIT, unqual(c, vt), loc, e->ncl++, 0, 0, el.data, n);
    vec_free(&el);
    return vec_wrap(e, load(e, v), unqual(c, vt), setup, loc);
}

uint32_t vl_bin(El *e, int p, TypeId t, uint32_t a, uint32_t b, SrcLoc loc)
{
    uint32_t ka[2];
    ka[0] = a;
    ka[1] = b;
    return a == TN || b == TN ? TN : mk(e, TO_BINOP, t, loc, 0, 0, (unsigned)p, ka, 2);
}

uint32_t vl_un(El *e, int p, TypeId t, uint32_t a, SrcLoc loc)
{
    return a == TN ? TN : mk(e, TO_UNOP, t, loc, 0, 0, (unsigned)p, &a, 1);
}

/* a p b as an int. */
uint32_t vl_cmp(El *e, int p, uint32_t a, uint32_t b, SrcLoc loc)
{
    uint32_t ka[2];
    ka[0] = a;
    ka[1] = b;
    return a == TN || b == TN ? TN : mk(e, TO_CMP, TYPE_B(INT), loc, 0, 0, (unsigned)p, ka, 2);
}

/* -(a p b) in type t: all ones when the comparison holds, else 0 */
uint32_t vl_mask(El *e, int p, TypeId t, uint32_t a, uint32_t b, SrcLoc loc)
{
    uint32_t m = vl_cmp(e, p, a, b, loc);
    return m == TN ? TN : vl_un(e, P_MINUS, t, conv(e, m, t, loc), loc);
}

/* The vector of type vt whose lane j is f (e, ctx, j), after the statements in setup.  Every
 * lane-wise operation below, and the x86 intrinsics, is one. */
uint32_t vmap(El *e, TypeId vt, SrcLoc loc, const U32Vec *setup, LaneFn f, void *ctx)
{
    TypeId et;
    uint32_t n, j, id, *lanes;
    if (!e->infunc)
        return tunsup(e, "vector operation outside a function");
    if (!vec_dims(e, vt, &et, &n))
        return TN;
    lanes = xmalloc((n ? n : 1) * sizeof *lanes);
    for (j = 0; j < n; j++)
        lanes[j] = f(e, ctx, j);
    id = vec_make(e, vt, loc, lanes, n, setup);
    free(lanes);
    return id;
}

static bool is_relop(int p)
{
    return p == P_LT || p == P_GT || p == P_LE || p == P_GE || p == P_EQEQ || p == P_NE;
}

typedef struct VBin {
    int p;
    TypeId ot, rl;              /* the lane type of the operation, and of the result */
    const VOp *a, *b;
    SrcLoc loc;
} VBin;

static uint32_t bin_lane(El *e, void *ctx, uint32_t j)
{
    const VBin *x = ctx;
    uint32_t a = conv(e, vop_lane(e, x->a, j, x->loc), x->ot, x->loc);
    uint32_t b = conv(e, vop_lane(e, x->b, j, x->loc), x->ot, x->loc);
    return is_relop(x->p) ? vl_mask(e, x->p, x->rl, a, b, x->loc) : vl_bin(e, x->p, x->ot, a, b, x->loc);
}

/* a op b on the lanes of the operands oa and ob into a vector of type vt: the relational
 * operators give -1 or 0 in the result's lane type. */
uint32_t vec_op(El *e, int p, TypeId vt, const VOp *oa, const VOp *ob, SrcLoc loc,
                       U32Vec *setup)
{
    VBin x;
    uint32_t n;
    if (!vec_dims(e, vt, &x.rl, &n))
        return TN;
    x.p = p;
    x.ot = is_relop(p) ? oa->et : x.rl;     /* the lanes are the operands' for a comparison, the result's else */
    x.a = oa;
    x.b = ob;
    x.loc = loc;
    return vmap(e, vt, loc, setup, bin_lane, &x);
}

/* a op b where an operand or the result is a vector (the checker's nodes a and b). */
uint32_t vec_binary(El *e, uint32_t i, int p, uint32_t a, uint32_t b, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId ta = rvt(c, a), tb = rvt(c, b), ovt = tkind(c, ta) == TY_VECTOR ? ta : tb;
    U32Vec setup = {0};
    VOp oa, ob;
    uint32_t id = TN;
    if (tkind(c, ta) == TY_VECTOR && tkind(c, tb) == TY_VECTOR && mainv(c, ta) != mainv(c, tb)) {
        bool ok = false, ok2 = false;       /* the same lanes under another element type (long, long long) */
        if (type_size(&c->tt, ta, &ok) != type_size(&c->tt, tb, &ok2) || !ok || !ok2 ||
            type_size(&c->tt, elem_of(c, ta), &ok) != type_size(&c->tt, elem_of(c, tb), &ok2))
            return tunsup(e, "mixed vector types");
    }
    if (tkind(c, c->ty[i]) != TY_VECTOR)
        return tunsup(e, "vector operation without a vector result");
    if (vop_node(e, a, ovt, &oa, &setup, loc) && vop_node(e, b, ovt, &ob, &setup, loc))
        id = vec_op(e, p, unqual(c, c->ty[i]), &oa, &ob, loc, &setup);
    vec_free(&setup);
    return id;
}

typedef struct VUn {
    int p;
    const VOp *a;
    bool step;                  /* a + 1 (P_PLUS) or a - 1 (P_MINUS) instead of the unary operator */
    SrcLoc loc;
} VUn;

static uint32_t un_lane(El *e, void *ctx, uint32_t j)
{
    const VUn *x = ctx;
    TypeId t = x->a->et;
    uint32_t v = vop_lane(e, x->a, j, x->loc);
    if (x->step)
        return vl_bin(e, x->p, t, v, is_flt(e->c, t) ? fconst(e, t, 1, x->loc) : cnst(e, t, 1, x->loc), x->loc);
    return vl_un(e, x->p, t, v, x->loc);
}

/* p oa on every lane (-, ~), or with step the value ++ (P_PLUS) or -- (P_MINUS) stores: oa + 1, oa - 1. */
uint32_t vec_un(El *e, TypeId vt, int p, bool step, const VOp *oa, SrcLoc loc, U32Vec *setup)
{
    VUn x;
    x.p = p;
    x.a = oa;
    x.step = step;
    x.loc = loc;
    return vmap(e, vt, loc, setup, un_lane, &x);
}

/* -v, ~v, +v. */
uint32_t vec_unary(El *e, uint32_t i, int p, uint32_t a, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId vt = unqual(c, c->ty[i]);
    U32Vec setup = {0};
    VOp oa;
    uint32_t id = TN;
    if (p == P_PLUS)
        return conv(e, rv(e, a), vt, loc);
    if ((p != P_MINUS && p != P_TILDE) || tkind(c, vt) != TY_VECTOR)
        return tunsup(e, "vector operator");
    if (vop_node(e, a, vt, &oa, &setup, loc))
        id = vec_un(e, vt, p, false, &oa, loc, &setup);
    vec_free(&setup);
    return id;
}

/* A cast between a vector and an integer or float of the same size names the same bytes: the vector
 * read as one lane of the scalar type, or the scalar made the only lane of such a vector. */
bool vec_bitcast(El *e, TypeId to, uint32_t k)
{
    Checker *c = e->c;
    TypeId from = rvt(c, k);
    bool a = false, b = false, tv = tkind(c, to) == TY_VECTOR, fv = tkind(c, from) == TY_VECTOR;
    TypeId sc = tv ? from : to;
    if (tv == fv || !(ok_int(e, sc)))
        return false;
    return type_size(&c->tt, to, &a) == type_size(&c->tt, from, &b) && a && b;
}

uint32_t vec_bits(El *e, uint32_t k, TypeId to, SrcLoc loc)
{
    Checker *c = e->c;
    VOp o = {0};
    U32Vec setup = {0};
    uint32_t id = TN, x;
    bool ok = false;
    if (tkind(c, to) == TY_VECTOR) {            /* a scalar: the only lane of a vector of that type */
        TypeId sc = unqual(c, rvt(c, k)), lt = type_vector(&c->tt, sc, (uint32_t)type_size(&c->tt, sc, &ok));
        x = rv(e, k);
        id = x == TN ? TN : vec_make(e, lt, loc, &x, 1, &setup);
        id = id == TN ? TN : mk(e, TO_CAST, unqual(c, to), loc, 0, 0, 0, &id, 1);
    } else if (vop_node(e, k, rvt(c, k), &o, &setup, loc)) {
        o.et = unqual(c, to);
        id = vec_wrap(e, vop_lane(e, &o, 0, loc), unqual(c, to), &setup, loc);
    }
    vec_free(&setup);
    return id;
}

typedef struct VConv {
    const VOp *a;
    TypeId et;
    SrcLoc loc;
} VConv;

static uint32_t conv_lane(El *e, void *ctx, uint32_t j)
{
    const VConv *x = ctx;
    return conv(e, vop_lane(e, x->a, j, x->loc), x->et, x->loc);
}

/* __builtin_convertvector (v, type): every lane converted to the lane type of type. */
uint32_t vec_convert(El *e, uint32_t i, uint32_t a, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId vt = unqual(c, c->ty[i]);
    U32Vec setup = {0};
    VOp oa;
    VConv x;
    uint32_t n, id = TN;
    if (vec_dims(e, vt, &x.et, &n) && vop_node(e, a, rvt(c, a), &oa, &setup, loc)) {
        x.a = &oa;
        x.loc = loc;
        id = vmap(e, vt, loc, &setup, conv_lane, &x);
    }
    vec_free(&setup);
    return id;
}

/* Lane ix (a node of type int, in range) of operand o, found through the vector's address. */
static uint32_t vop_dyn(El *e, const VOp *o, uint32_t ix, SrcLoc loc)
{
    TypeTable *tt = &e->c->tt;
    TypeId pt = type_ptr(tt, o->et);
    uint32_t b = o->tmp != UINT32_MAX ? mk(e, TO_TMP, o->st, loc, o->tmp, 0, 0, NULL, 0) : lval(e, o->x);
    if (b == TN || ix == TN)
        return TN;
    return load(e, deref_t(e, ptradd(e, pt, conv(e, addr_t(e, b, type_ptr(tt, nty(e, b)), loc), pt, loc), ix, false, loc),
                           o->et, loc));
}

typedef struct VShuf {
    VOp a, b, m;                /* the sources, and the mask (unused when the indices are constants) */
    const uint32_t *cix;        /* constant indices, one per lane (a lane of b from n up), or NULL */
    uint32_t n;                 /* the lanes of a source */
    bool two;
    SrcLoc loc;
} VShuf;

/* Mask lane j, reduced to the lanes in play. */
static uint32_t shuf_ix(El *e, const VShuf *x, uint32_t j)
{
    TypeId it = TYPE_B(INT);
    return vl_bin(e, P_AMP, it, conv(e, vop_lane(e, &x->m, j, x->loc), it, x->loc),
                  cnst(e, it, x->two ? 2 * x->n - 1 : x->n - 1, x->loc), x->loc);
}

static uint32_t shuf_lane(El *e, void *ctx, uint32_t j)
{
    const VShuf *x = ctx;
    TypeId it = TYPE_B(INT);
    SrcLoc loc = x->loc;
    if (x->cix)
        return vop_lane(e, x->cix[j] >= x->n ? &x->b : &x->a, x->cix[j] % x->n, loc);
    if (x->two) {
        uint32_t ka[3];
        ka[0] = vl_cmp(e, P_LT, shuf_ix(e, x, j), cnst(e, it, x->n, loc), loc);
        ka[1] = vop_dyn(e, &x->a, shuf_ix(e, x, j), loc);
        ka[2] = vop_dyn(e, &x->b, vl_bin(e, P_MINUS, it, shuf_ix(e, x, j), cnst(e, it, x->n, loc), loc), loc);
        return ka[0] == TN || ka[1] == TN || ka[2] == TN ? TN : mk(e, TO_COND, x->a.et, loc, 0, 0, 0, ka, 3);
    }
    return vop_dyn(e, &x->a, shuf_ix(e, x, j), loc);
}

/* The indices of a mask that is a compound literal of one integer constant per lane, reduced to
 * mask, in ix[0 .. n); false for any other mask. */
static bool shuf_consts(Checker *c, uint32_t m, uint32_t n, uint64_t mask, uint32_t *ix)
{
    uint32_t cl[2], kid[64], j;
    m = strip_paren(c, m);
    if (n > 64 || ntag(c, m) != N_COMPOUND_LIT || nkids(c, m, cl, 2) < 2 || ntag(c, cl[1]) != N_INIT_LIST ||
        nkids(c, cl[1], NULL, 0) != n)
        return false;
    nkids(c, cl[1], kid, n);
    for (j = 0; j < n; j++) {
        if (c->ck[kid[j]] != K_ICE)
            return false;
        ix[j] = (uint32_t)((uint64_t)cexpr_sval(c, kid[j]) & mask);
    }
    return true;
}

/* __builtin_shuffle (v, [w,] mask) and __builtin_shufflevector (v, w, index...): lane j of the
 * result is a lane of v, or of w, named by the mask's lane j (taken modulo the lanes in play) or by
 * the constant index j.  A constant mask or index list is read at compile time; any other mask is
 * read through the vector's memory.  k[1 ...] are the checker's argument nodes. */
uint32_t vec_shuffle(El *e, uint32_t i, const uint32_t *k, uint32_t nk, bool fixed, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId rt = unqual(c, c->ty[i]), et, ret;
    uint32_t nr, id = TN, cix[64], j;
    VShuf x = {0};
    U32Vec setup = {0};
    x.two = fixed || nk == 4;
    x.loc = loc;
    if (!vop_node(e, k[1], rvt(c, k[1]), &x.a, &setup, loc) ||
        (x.two && !vop_node(e, k[2], rvt(c, k[2]), &x.b, &setup, loc)) || !vec_dims(e, x.a.vt, &et, &x.n) ||
        !vec_dims(e, rt, &ret, &nr))
        goto done;
    if (nr > 64) {
        tunsup(e, "shuffle of more than 64 lanes");
        goto done;
    }
    if (fixed) {                    /* an index of -1 is any lane */
        for (j = 0; j < nr; j++) {
            int64_t ix = cexpr_sval(c, k[3 + j]);
            cix[j] = ix < 0 ? 0 : (uint32_t)ix;
        }
        x.cix = cix;
    } else if (!fixed && shuf_consts(c, k[nk - 1], nr, x.two ? 2 * x.n - 1 : x.n - 1, cix)) {
        x.cix = cix;
    } else if (fixed || !vop_node(e, k[nk - 1], rvt(c, k[nk - 1]), &x.m, &setup, loc)) {
        goto done;
    }
    id = vmap(e, rt, loc, &setup, shuf_lane, &x);
done:
    vec_free(&setup);
    return id;
}

