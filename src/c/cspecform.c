/* cspecform.c - the expression forms only a spec has: `return` (the function's
 * value, in an ensures), $len(p) and the slice p[lo:hi]
 * (docs/SPEC_DIRECTIVES_DESIGN.md).  The parser makes them only inside a
 * #pragma cereal line; everything else in a spec is ordinary C. */
#include "c/cexpr_int.h"

/* The spec kind (SPEC_*) of the pragma that holds expression node i. */
static int spec_of(const Checker *c, uint32_t i)
{
    while (i != NO_NODE && ntag(c, i) != N_PRAGMA)
        i = c->par[i];
    return i == NO_NODE ? SPEC_NONE : c->nodes[i].aux;
}

void e_spec_result(Checker *c, uint32_t i)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId fty, vt;
    if (spec_of(c, i) != SPEC_ENSURES) {
        cerror(c, loc, "'return' is only available in an 'ensures' spec");
        set_err(c, i);
        return;
    }
    if (c->func_sym == SYM_NONE) {
        set_err(c, i);
        return;
    }
    fty = type_canon(TT, csym(c, c->func_sym)->ty);
    if (type_ckind(TT, fty) != TY_FUNC) {
        set_err(c, i);
        return;
    }
    vt = type_base(TT, fty);
    if (type_ckind(TT, vt) == TY_VOID) {
        cerror(c, loc, "'return' in an 'ensures' of a function returning void");
        set_err(c, i);
        return;
    }
    c->ty[i] = vt;
}

void e_spec_len(Checker *c, uint32_t i)
{
    uint32_t k;
    if (nkids(c, i, &k, 1) < 1 || node_err(c, k)) {
        set_err(c, i);
        return;
    }
    if (!is_ptr(c, rvt(c, k))) {
        cerror(c, cnode_loc(c, i), "'$len' of a value that is not a pointer or array");
        set_err(c, i);
        return;
    }
    c->ty[i] = size_type(c);
    c->ef[i] = c->ef[k] & EF_PROP;
}

void e_slice(Checker *c, uint32_t i)
{
    uint32_t k[3], p = c->par[i];
    SrcLoc loc = cnode_loc(c, i);
    TypeId pt, el;
    int j;
    if (nkids(c, i, k, 3) < 3 || node_err(c, k[0]) || node_err(c, k[1]) ||
        node_err(c, k[2])) {
        set_err(c, i);
        return;
    }
    pt = rvt(c, k[0]);
    if (!is_ptr(c, pt)) {
        cerror(c, loc, "slice of a value that is not a pointer or array");
        set_err(c, i);
        return;
    }
    el = pointee(c, pt);
    if (is_func(c, el) || type_ckind(TT, type_canon(TT, el)) == TY_VOID) {
        cerror(c, loc, "slice of a pointer to a function or void");
        set_err(c, i);
        return;
    }
    for (j = 1; j < 3; j++)
        if (!is_int(c, rvt(c, k[j]))) {
            cerror(c, loc, "slice bound is not an integer");
            set_err(c, i);
            return;
        }
    if (p == NO_NODE || ntag(c, p) != N_BINARY || c->nodes[p].aux ||
        (npunct(c, p) != P_EQEQ && npunct(c, p) != P_NE)) {
        cerror(c, loc, "a slice is only an operand of '==' or '!='");
        set_err(c, i);
        return;
    }
    c->ty[i] = pt;
    c->ef[i] = (c->ef[k[0]] | c->ef[k[1]] | c->ef[k[2]]) & EF_PROP;
}
