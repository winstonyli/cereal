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

static bool is_relop(int p)
{
    return p == P_LT || p == P_GT || p == P_LE || p == P_GE || p == P_EQEQ || p == P_NE;
}

/* a op b on the lanes of the operands oa and ob into a vector of type vt: the relational
 * operators give -1 or 0 in the result's lane type. */
uint32_t vec_op(El *e, int p, TypeId vt, const VOp *oa, const VOp *ob, SrcLoc loc,
                       U32Vec *setup)
{
    TypeId rl, ot;
    uint32_t n, j, id;
    uint32_t *lanes;
    if (!e->infunc)
        return tunsup(e, "vector operation outside a function");
    if (!vec_dims(e, vt, &rl, &n))
        return TN;
    ot = is_relop(p) ? oa->et : rl;       /* the lanes are the operands' for a comparison, the result's else */
    lanes = xmalloc((n ? n : 1) * sizeof *lanes);
    for (j = 0; j < n; j++) {
        uint32_t ka[2];
        ka[0] = conv(e, vop_lane(e, oa, j, loc), ot, loc);
        ka[1] = conv(e, vop_lane(e, ob, j, loc), ot, loc);
        if (ka[0] == TN || ka[1] == TN) {
            lanes[j] = TN;
        } else if (is_relop(p)) {
            uint32_t m = mk(e, TO_CMP, TYPE_B(INT), loc, 0, 0, (unsigned)p, ka, 2);
            m = conv(e, m, rl, loc);
            lanes[j] = mk(e, TO_UNOP, rl, loc, 0, 0, P_MINUS, &m, 1);
        } else {
            lanes[j] = mk(e, TO_BINOP, ot, loc, 0, 0, (unsigned)p, ka, 2);
        }
    }
    id = vec_make(e, vt, loc, lanes, n, setup);
    free(lanes);
    return id;
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

/* -v, ~v, +v. */
uint32_t vec_unary(El *e, uint32_t i, int p, uint32_t a, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId vt = unqual(c, c->ty[i]), et;
    U32Vec setup = {0};
    VOp oa;
    uint32_t n, j, id = TN, *lanes;
    if (p == P_PLUS)
        return conv(e, rv(e, a), vt, loc);
    if ((p != P_MINUS && p != P_TILDE) || tkind(c, vt) != TY_VECTOR)
        return tunsup(e, "vector operator");
    if (!e->infunc)
        return tunsup(e, "vector operation outside a function");
    if (!vop_node(e, a, vt, &oa, &setup, loc) || !vec_dims(e, vt, &et, &n)) {
        vec_free(&setup);
        return TN;
    }
    lanes = xmalloc((n ? n : 1) * sizeof *lanes);
    for (j = 0; j < n; j++) {
        uint32_t x = vop_lane(e, &oa, j, loc);
        lanes[j] = x == TN ? TN : mk(e, TO_UNOP, oa.et, loc, 0, 0, (unsigned)p, &x, 1);
    }
    id = vec_make(e, vt, loc, lanes, n, &setup);
    free(lanes);
    vec_free(&setup);
    return id;
}

/* __builtin_convertvector (v, type): every lane converted to the lane type of type. */
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

uint32_t vec_convert(El *e, uint32_t i, uint32_t a, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId vt = unqual(c, c->ty[i]), et, st;
    U32Vec setup = {0};
    VOp oa;
    uint32_t n, j, id = TN, *lanes;
    if (!e->infunc)
        return tunsup(e, "vector operation outside a function");
    st = rvt(c, a);
    if (!vec_dims(e, vt, &et, &n) || !vop_node(e, a, st, &oa, &setup, loc)) {
        vec_free(&setup);
        return TN;
    }
    lanes = xmalloc((n ? n : 1) * sizeof *lanes);
    for (j = 0; j < n; j++)
        lanes[j] = conv(e, vop_lane(e, &oa, j, loc), et, loc);
    id = vec_make(e, vt, loc, lanes, n, &setup);
    free(lanes);
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

/* __builtin_shuffle (v, [w,] mask) and __builtin_shufflevector (v, w, index...): lane j of the
 * result is a lane of v, or of w, named by the mask's lane j (taken modulo the lanes in play) or by
 * the constant index j.  k[1 ...] are the checker's argument nodes. */
uint32_t vec_shuffle(El *e, uint32_t i, const uint32_t *k, uint32_t nk, bool fixed, SrcLoc loc)
{
    Checker *c = e->c;
    TypeId rt = unqual(c, c->ty[i]), et, ret, it = TYPE_B(INT);
    uint32_t n, nr, j, id = TN, *lanes;
    bool two = fixed || nk == 4;
    VOp a = {0}, b = {0}, m = {0};
    U32Vec setup = {0};
    if (!vop_node(e, k[1], rvt(c, k[1]), &a, &setup, loc) || (two && !vop_node(e, k[2], rvt(c, k[2]), &b, &setup, loc)) ||
        (!fixed && !vop_node(e, k[nk - 1], rvt(c, k[nk - 1]), &m, &setup, loc)) || !vec_dims(e, a.vt, &et, &n) ||
        !vec_dims(e, rt, &ret, &nr))
        goto done;
    lanes = xmalloc(nr * sizeof *lanes);
    for (j = 0; j < nr; j++) {
        if (fixed) {                        /* an index of -1 is any lane */
            int64_t ix = cexpr_sval(c, k[3 + j]);
            ix = ix < 0 ? 0 : ix;
            lanes[j] = vop_lane(e, ix >= (int64_t)n ? &b : &a, (uint32_t)(ix % n), loc);
        } else {
            uint64_t mask = two ? 2 * n - 1 : n - 1;
#define SHIX() ia_bin(e, P_AMP, it, conv(e, vop_lane(e, &m, j, loc), it, loc), cnst(e, it, mask, loc), loc)
            if (two) {
                uint32_t ka[3];
                ka[0] = ia_cmp(e, P_LT, SHIX(), cnst(e, it, n, loc), loc);
                ka[1] = vop_dyn(e, &a, SHIX(), loc);
                ka[2] = vop_dyn(e, &b, ia_bin(e, P_MINUS, it, SHIX(), cnst(e, it, n, loc), loc), loc);
                lanes[j] = ka[0] == TN || ka[1] == TN || ka[2] == TN ? TN : mk(e, TO_COND, a.et, loc, 0, 0, 0, ka, 3);
            } else {
                lanes[j] = vop_dyn(e, &a, SHIX(), loc);
            }
#undef SHIX
        }
        if (lanes[j] == TN)
            goto fail;
    }
    id = vec_make(e, rt, loc, lanes, nr, &setup);
fail:
    free(lanes);
done:
    vec_free(&setup);
    return id;
}

