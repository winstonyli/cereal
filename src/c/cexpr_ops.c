/* cexpr_ops.c - binary operators, vector operands, ?:, assignment
 * (split from cexpr.c; see its header comment). */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- binary operators ---------------------------------------------------------------- */

/* The value flags of a binary result whose integer value is v (ovf: it
 * overflowed in its type; int_const: gcc's int_const, false when the
 * operation is not a constant expression after all). */
void bin_value(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                      uint64_t v, bool ovf, bool int_const, SrcLoc loc)
{
    TypeId t = c->ty[i];
    bool prev = ((c->ef[a] | c->ef[b]) & EF_OVERFLOW) != 0;
    c->cv[i] = cexpr_trunc(c, t, v);
    if (is_intcst(c, a) && is_intcst(c, b)) {
        c->ef[i] |= EF_INTOPS;
        if (!int_const) {
            c->ck[i] = K_FOLD;
            return;
        }
        c->ck[i] = K_ICE;
        if (ovf || prev)
            c->ef[i] |= EF_OVERFLOW;
        if (ovf && !prev && !inhibited(c, i, false))
            cwarn(c, loc, DW_OVERFLOW, "integer overflow in expression of type "
                  "%s results in '%s'", type_q(TT, t), vstr(c, t, c->cv[i]));
        return;
    }
    c->ck[i] = K_FOLD;
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
    if (prev)
        c->ef[i] |= EF_OVERFLOW;
    else if (ovf) {
        c->ef[i] |= EF_OVERFLOW | EF_FOLDWARN;
        c->fold_pending++;
    }
}

void invalid_operands(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                             int op)
{
    const char *sp = punct_spelling[op];
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    /* the left operand of a compound assignment is no rvalue: it keeps its
     * qualifiers unless the promotions change it */
    if (ntag(c, i) == N_ASSIGN && mainv(c, ta) == mainv(c, c->ty[a])) {
        ta = c->ty[a];
        if (tquals(c, ta) & (TQ_VOLATILE | TQ_ATOMIC))
            ta = type_qual(unqual(c, ta),
                           tquals(c, ta) & ~(TQ_VOLATILE | TQ_ATOMIC));
    }
    cerror(c, cnode_loc(c, i), "invalid operands to binary %s (have %s and %s)",
           sp, type_q(TT, ta), type_q(TT, tb));
    set_err(c, i);
}

/* An operand of a binary operator used for its value: false (after an
 * error) for void and incomplete ones. */
static bool binop_operand_at(Checker *c, uint32_t a, SrcLoc loc)
{
    if (is_void(c, rvt(c, a))) {
        cerror(c, expr_loc(c, a), "void value not ignored as it ought to be");
        return false;
    }
    return rvalue_ok_at(c, a, loc);
}

static bool binop_operand(Checker *c, uint32_t a)
{
    return binop_operand_at(c, a, first_loc(c, a));
}

static bool truth_ok_at(Checker *c, uint32_t a, SrcLoc loc)
{
    TypeId t = rvt(c, a);
    if (is_record(c, t)) {
        cerror(c, loc, "used %s type value where scalar is "
               "required", tkind(c, t) == TY_UNION ? "union" : "struct");
        return false;
    }
    if (tkind(c, t) == TY_VECTOR) {
        cerror(c, loc, "used vector type where scalar is required");
        return false;
    }
    return true;
}

bool truth_ok(Checker *c, uint32_t a)
{
    return truth_ok_at(c, a, first_loc(c, a));
}

static void e_comma(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId t;
    if (node_err(c, a) || node_err(c, b)) {
        set_err(c, i);
        return;
    }
    /* c_parser_expression: both operands go through convert_lvalue_to_rvalue */
    if (!rvalue_ok(c, a) | !rvalue_ok(c, b)) {
        set_err(c, i);
        return;
    }
    if (!(c->ef[a] & EF_SIDE)) {
        uint32_t s = strip_paren(c, a), ck[3];
        bool voidcast;
        /* (void) a, (void) b, c: a COMPOUND_EXPR ending in a void cast */
        while (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
               nkids(c, s, ck, 3) == 2)
            s = strip_paren(c, ck[1]);
        voidcast = ntag(c, s) == N_CAST && is_void(c, c->ty[s]);
        if (!voidcast && !inhibited(c, i, false))
            cwarn(c, cnode_loc(c, i), DW_UNUSED_VALUE, "left-hand operand of "
                  "comma expression has no effect");
    }
    else if (!inhibited(c, i, false))
        unused_value(c, a, cnode_loc(c, i));    /* emit_side_effect_warnings */
    t = rvt(c, b);
    c->ty[i] = t;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    if (c->ef[a] & EF_SIDE)
        return;
    /* C99 DR031: a comma of integer constant operands may appear in an
     * unevaluated operand of an integer constant expression */
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
    switch (c->ck[b]) {
    case K_ICE: case K_FOLD:
        c->ck[i] = K_FOLD;
        c->cv[i] = c->cv[b];
        if (is_intcst(c, b))
            c->ef[i] |= EF_CST;
        c->ef[i] |= c->ef[b] & (EF_OVERFLOW | EF_NOPCST);
        break;
    case K_FLOAT: case K_ADDR:
        c->ck[i] = c->ck[b];
        c->cv[i] = c->cv[b];
        c->cb[i] = c->cb[b];
        break;
    default:
        break;
    }
}


/* ---- vector operands (c-typeck.cc build_binary_op, c-common.cc scalar_to_vector) ---- */

TypeId vec_elem(Checker *c, TypeId vt)
{
    return type_base(TT, type_canon(TT, vt));
}

unsigned vec_esize(Checker *c, TypeId el)
{
    bool ok;
    return (unsigned)type_size(TT, el, &ok);
}

/* gcc's INTEGER_TYPE: no _Bool, no enum. */
static bool int_type(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return is_int(c, t) && k != TY_BOOL && k != TY_ENUM;
}

/* unsafe_conversion_p of scalar operand s (type st) to vector element el. */
static bool vec_unsafe(Checker *c, uint32_t s, TypeId st, TypeId el)
{
    unsigned esz = vec_esize(c, el);
    long double f;
    if (is_int(c, el)) {
        unsigned p = int_bits(c, el), sp = int_bits(c, st);
        if (p >= sp)
            return false;
        if (has_ival(c, s)) {
            bool neg = ival_neg(c, st, c->cv[s]);
            int64_t v = (int64_t)c->cv[s];
            if (!neg && p < 64 && c->cv[s] >> (p - (is_signed(c, el) ? 1 : 0)))
                return true;
            if (neg && p < 64 && v < -((int64_t)1 << (p - 1)))
                return true;
            return false;
        }
        return true;
    }
    if (fval(c, s, &f)) {
        if (f != f)
            return false;
        if (esz == 4)
            return (long double)(float)f != f;
        if (esz == 8)
            return (long double)(double)f != f;
        return false;
    }
    if (is_int(c, st)) {
        unsigned mant = esz == 4 ? 24 : esz == 8 ? 53 : 64;
        return int_bits(c, st) > mant;
    }
    return vec_esize(c, st) > esz;
}

/* scalar_to_vector: operand sn (type st) of a binary operator with vector
 * type vt is converted to vt's element type.  1: converted, 0: not a scalar
 * it converts, -1: an error was reported. */
int vec_scalar(Checker *c, uint32_t i, uint32_t sn, TypeId st, TypeId vt,
                      bool strict_int)
{
    TypeId el = vec_elem(c, vt);
    bool isc = int_type(c, st), fsc = is_flt(c, st);
    if (strict_int ? !isc : (!isc && !fsc))
        return 0;
    if (is_int(c, el) && fsc)
        return 0;
    if (vec_unsafe(c, sn, st, el)) {
        cerror(c, cnode_loc(c, i), "conversion of scalar %s to vector %s "
               "involves truncation", type_q(TT, st), type_q(TT, vt));
        set_err(c, i);
        return -1;
    }
    return 1;
}

/* "invalid operands": the scalar keeps its own type here, not the promoted
 * one; la/lb override an operand's type once it was converted. */
void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId la, TypeId lb)
{
    cerror(c, cnode_loc(c, i), "invalid operands to binary %s (have %s and %s)",
           punct_spelling[op], type_q(TT, la ? la : unqual(c, rvt(c, a))),
           type_q(TT, lb ? lb : unqual(c, rvt(c, b))));
    set_err(c, i);
}

/* gcc's rules for a binary operator with a vector operand: both vectors of
 * the same shape, or a vector and a scalar converted to the element type.
 * False (after an error) when they do not apply. */
static bool vec_binop(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                      bool shift)
{
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool va = tkind(c, ta) == TY_VECTOR, vb = tkind(c, tb) == TY_VECTOR;
    TypeId vt = va ? ta : tb, el = vec_elem(c, vt), la = ta, lb = tb;
    bool need_int = shift || op == P_PERCENT || op == P_AMP || op == P_PIPE ||
                    op == P_CARET;
    bool ok = true;
    /* & | ^ and shifts check the element type before converting a scalar; %
     * converts first */
    if ((shift || op == P_AMP || op == P_PIPE || op == P_CARET) &&
        !is_int(c, el))
        ok = false;
    else if (va && vb) {
        TypeId e2 = vec_elem(c, tb);
        ok = type_ent(TT, type_canon(TT, ta))->n ==
                 type_ent(TT, type_canon(TT, tb))->n &&
             vec_esize(c, el) == vec_esize(c, e2) &&
             ((is_int(c, el) && is_int(c, e2)) ||
              (is_flt(c, el) && is_flt(c, e2)));
    } else {
        uint32_t sn = va ? b : a;
        TypeId st = unqual(c, rvt(c, sn));
        int r = 0;
        if (!shift || !va)    /* a vector shifted by a scalar converts nothing */
            r = vec_scalar(c, i, sn, st, vt, shift);
        if (r < 0)
            return false;
        if (r > 0) {
            if (va)
                lb = vt;
            else
                la = vt;
        } else if (!(shift && va && int_type(c, st) && is_int(c, el))) {
            if (!shift && !need_int && is_int(c, el) && is_flt(c, st)) {
                /* error() at gcc's input_location: the parser is past the
                 * right operand */
                cerror(c, cdecl_iloc(c, last_tok(c, i) + 1), "cannot convert "
                       "value to a vector");
                set_err(c, i);
                return false;
            }
            ok = false;
        }
    }
    if (ok && need_int && !is_int(c, el))
        ok = false;
    if (!ok) {
        vec_invalid(c, i, a, b, op, la == ta ? 0 : la, lb == tb ? 0 : lb);
        return false;
    }
    c->ty[i] = vt;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    return true;
}

static void e_shift(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool left = op == P_SHL, int_const = true, cnt_ok = false;
    unsigned prec;
    const char *dir = left ? "left" : "right";
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        /* a constant scalar count is checked against the element's width */
        if (vec_binop(c, i, a, b, op, true) && tkind(c, ta) == TY_VECTOR &&
            tkind(c, tb) != TY_VECTOR && has_ival(c, b) &&
            !inhibited(c, i, false)) {
            if (ival_neg(c, tb, c->cv[b]))
                cwarn(c, loc, DW_SHIFT_COUNT_NEGATIVE, "%s shift count is "
                      "negative", dir);
            else if (c->cv[b] >= int_bits(c, vec_elem(c, ta)))
                cwarn(c, loc, DW_SHIFT_COUNT_OVERFLOW, "%s shift count >= "
                      "width of vector element", dir);
        }
        return;
    }
    if (!is_int(c, ta) || !is_int(c, tb)) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    c->ty[i] = ta;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    prec = int_bits(c, ta);
    if (has_ival(c, b)) {
        if (ival_neg(c, tb, c->cv[b])) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, DW_SHIFT_COUNT_NEGATIVE, "%s shift count is "
                      "negative", dir);
        } else if (c->cv[b] >= prec) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, DW_SHIFT_COUNT_OVERFLOW, "%s shift count >= width "
                      "of type", dir);
        } else {
            cnt_ok = true;
        }
    }
    if (left && is_intcst(c, a) && is_signed(c, ta) &&
        ival_neg(c, ta, c->cv[a])) {
        int_const = false;
        if (!inhibited(c, i, false))
            cwarn(c, loc, DW_SHIFT_NEGATIVE_VALUE, "left shift of negative "
                  "value");
    }
    if (!(cnt_ok && has_ival(c, a) && prec <= 64)) {
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
        if (!cnt_ok && !int_const && is_intcst(c, a) && is_intcst(c, b) &&
            (!ival_neg(c, tb, c->cv[b]) || c->cv[a] == 0)) {
            /* (a negative count only for a zero operand, which fold
             * leaves a constant) a count past the width: not an ICE, but a constant to gcc's
             * initializers (a pedwarn there) */
            c->ck[i] = K_FOLD;
            c->cv[i] = 0;
        }
        return;
    }
    if (left && is_intcst(c, a) && is_intcst(c, b) && is_signed(c, ta)) {
        unsigned mp = min_prec_signed(cexpr_trunc(c, ta, c->cv[a])) +
                      (unsigned)c->cv[b];
        /* 1 << 31 only reaches the sign bit; a negative operand has no such
         * exemption */
        bool neg = ival_neg(c, ta, c->cv[a]);
        bool sign = mp == prec + 1 && !neg;
        if (sign)
            int_const = false;
        if (mp > prec + 1 || (mp == prec + 1 && neg) ||
            (sign && diag_option_level(c->diag, DW_SHIFT_OVERFLOW_V, 1) >= 2)) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, DW_SHIFT_OVERFLOW_V, "result of '%s' requires %u "
                      "bits to represent, but %s only has %u bits", estr(c, i),
                      mp, type_q(TT, ta), prec);
        }
    }
    {
        unsigned fl;
        uint64_t v = ia_binop(op, cx_ia_ty(c, ta), cexpr_trunc(c, ta, c->cv[a]), c->cv[b], &fl);
        bin_value(c, i, a, b, v, false, int_const, loc);
    }
}

/* -Wxor-used-as-pow */
static void xor_pow(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
    unsigned long long l, r;
    Diagnostic *d;
    char sug[64];
    int own;
    if (!(c->ef[a] & EF_DECIMAL) || !(c->ef[b] & EF_DECIMAL) ||
        !has_ival(c, a) || !has_ival(c, b) || ntag(c, sa) != N_NUMBER ||
        ntag(c, sb) != N_NUMBER || tfrom_macro(c, c->nodes[sa].tok) ||
        tfrom_macro(c, c->nodes[sb].tok))
        return;
    l = c->cv[a];
    r = c->cv[b];
    if (l == 2) {
        if (r <= 30)
            snprintf(sug, sizeof sug, "'1 << %llu' (%llu)", r,
                     (unsigned long long)1 << r);
        else if (r <= 62)
            snprintf(sug, sizeof sug, "'1LL << %llu'", r);
        else if (r <= 64)
            snprintf(sug, sizeof sug, "exponentiation");
        else
            return; /* gcc: the RHS is too large to suggest anything */
    } else if (l == 10) {
        snprintf(sug, sizeof sug, "'1e%llu'", r);
    } else {
        return;
    }
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), DW_XOR_USED_AS_POW,
                "result of '%llu^%llu' is %llu; did you mean %s?", l, r, l ^ r,
                sug);
    /* gcc: the base and the operator (a touching pair merges); no hint for
     * 2^63 and up.  The note's hint is the hexadecimal base. */
    if (l == 10 || r <= 62) {
        cfix_range(c, d, 0, first_tok(c, a), after_tok(c, a) - 1,
                   l == 10 || r <= 30 ? "1" : "1LL", FIX_MAYBE);
        cfix_range(c, d, 0, c->nodes[i].tok, c->nodes[i].tok,
                   l == 10 ? "e" : "<<", FIX_MAYBE);
    }
    own = d ? (int)d->notes.len + 1 : 0;
    cnote(c, d, first_loc(c, a), "you can silence this warning by using a "
          "hexadecimal constant (%s rather than %llu)", l == 2 ? "0x2" : "0xa",
          l);
    cfix_range(c, d, own, first_tok(c, a), after_tok(c, a) - 1,
               l == 2 ? "0x2" : "0xa", FIX_MAYBE);
}

/* pointer +/- integer */
static void ptr_int(Checker *c, uint32_t i, uint32_t p, uint32_t n, bool minus)
{
    TypeId pt = rvt(c, p);
    if (!ptr_arith_ok(c, i, cnode_loc(c, i), pt)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = pt;
    c->ef[i] = (c->ef[p] | c->ef[n]) & EF_PROP;
    if (c->ck[p] == K_ADDR && has_ival(c, n)) {
        uint64_t d = c->cv[n] * elem_size(c, pt);
        c->ck[i] = K_ADDR;
        c->cb[i] = c->cb[p];
        c->cv[i] = cexpr_trunc(c, pt, minus ? c->cv[p] - d : c->cv[p] + d);
    }
}

static bool sym_ptr(Checker *c, uint32_t n, uint32_t *var, int64_t *off);

/* The address of an lvalue reached through a pointer variable (`p->m`,
 * `p->a[2]`, `*p`): the variable and the byte offset from its value. */
static bool lval_addr(Checker *c, uint32_t n, uint32_t *var, int64_t *off)
{
    uint32_t k[2];
    n = strip_paren(c, n);
    if (n == NO_NODE || node_err(c, n))
        return false;
    switch (ntag(c, n)) {
    case N_UNARY:
        return npunct(c, n) == P_STAR && sym_ptr(c, first_child(c, n), var, off);
    case N_MEMBER_EXPR: {
        uint32_t ch = first_child(c, n);
        bool arrow = (c->nodes[n].flags & NF_ARROW) != 0;
        uint64_t fo = 0;
        unsigned q = 0;
        if (ch == NO_NODE || node_err(c, ch) ||
            !(arrow ? sym_ptr(c, ch, var, off) : lval_addr(c, ch, var, off)))
            return false;
        if (!find_field(c, type_canon(TT, arrow ? pointee(c, rvt(c, ch))
                                                : c->ty[ch]),
                        cnode_ident(c, n), &fo, &q))
            return false;
        *off += (int64_t)fo;
        return true;
    }
    case N_INDEX:
        if (nkids(c, n, k, 2) != 2 || !is_array(c, c->ty[k[0]]) ||
            !has_ival(c, k[1]) || !lval_addr(c, k[0], var, off))
            return false;
        *off += (int64_t)c->cv[k[1]] *
                (int64_t)elem_size(c, type_ptr(TT, elem_of(c, c->ty[k[0]])));
        return true;
    default:
        return false;
    }
}

/* A pointer value fold-const can compare: a plain (non-volatile) pointer
 * variable plus a constant byte offset, through casts and `&`.  Two such
 * values off the same variable subtract to a constant. */
static bool sym_ptr(Checker *c, uint32_t n, uint32_t *var, int64_t *off)
{
    uint32_t k[2];
    n = strip_paren(c, n);
    if (n == NO_NODE || node_err(c, n))
        return false;
    if (is_array(c, c->ty[n]))
        return lval_addr(c, n, var, off);
    if (!is_ptr(c, c->ty[n]))
        return false;
    switch (ntag(c, n)) {
    case N_IDENT:
        *var = lookup_ord(c, cnode_ident(c, n));
        *off = 0;
        return *var != SYM_NONE && !(TYPE_QUALS(c->ty[n]) & TQ_VOLATILE);
    case N_CAST:
        return nkids(c, n, k, 2) == 2 && sym_ptr(c, k[1], var, off);
    case N_UNARY:
        return npunct(c, n) == P_AMP && lval_addr(c, first_child(c, n), var, off);
    default:
        return false;
    }
}

static void ptr_diff(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId ta = rvt(c, a), tb = rvt(c, b);
    TypeId pa = pointee(c, ta), pb = pointee(c, tb);
    SrcLoc loc = cnode_loc(c, i);
    uint64_t sz;
    if (!targets_compat(c, loc, pa, pb)) {
        invalid_operands(c, i, a, b, P_MINUS);
        return;
    }
    if (is_void(c, pa))
        ped_arith(c, i, loc, "pointer of type 'void *' used in subtraction");
    if (is_func(c, pa))
        ped_arith(c, i, loc, "pointer to a function used in subtraction");
    if (!is_void(c, pa) && !is_func(c, pa) && !complete(c, pa)) {
        cerror(c, loc, "arithmetic on pointer to an incomplete type");
        set_err(c, i);
        return;
    }
    sz = elem_size(c, ta);
    if (!is_void(c, pa) && !is_func(c, pa) && sz == 0 && !var_size(c, pa)) {
        cerror(c, loc, "arithmetic on pointer to an empty aggregate");
        set_err(c, i);
        return;
    }
    c->ty[i] = TYPE_MK(c->tgt->ptrdiff_type, 0);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    if (c->ck[a] == K_ADDR && c->ck[b] == K_ADDR && sz > 0 &&
        !var_size(c, pa) && (c->cb[a] == c->cb[b] || same_addr_const(c, a, b))) {
        c->ck[i] = K_FOLD;
        c->cv[i] = c->cb[a] == c->cb[b]
            ? cexpr_trunc(c, c->ty[i], (uint64_t)((int64_t)(c->cv[a] - c->cv[b]) /
                                                  (int64_t)sz))
            : 0;
    } else if (sz > 0 && !var_size(c, pa)) {
        uint32_t va, vb;
        int64_t oa, ob;
        if (sym_ptr(c, a, &va, &oa) && sym_ptr(c, b, &vb, &ob) && va == vb &&
            (oa - ob) % (int64_t)sz == 0) {
            c->ck[i] = K_FOLD;
            c->cv[i] = cexpr_trunc(c, c->ty[i],
                                   (uint64_t)((oa - ob) / (int64_t)sz));
        }
    }
}

static bool zero_ice(Checker *c, uint32_t n)
{
    return is_intcst(c, n) && c->ck[n] == K_ICE && c->cv[n] == 0;
}

/* fold-const's x * 0, 0 * x, x & 0, 0 & x and x - x (x a plain variable) */
static bool fold_zero_ident(Checker *c, int op, uint32_t a, uint32_t b)
{
    if (((c->ef[a] | c->ef[b]) & EF_PROP) || !is_int(c, rvt(c, a)) ||
        !is_int(c, rvt(c, b)))
        return false;
    if (op == P_STAR || op == P_AMP)
        return zero_ice(c, a) || zero_ice(c, b);
    if (op == P_MINUS) {
        uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
        return sa != NO_NODE && sb != NO_NODE && ntag(c, sa) == N_IDENT &&
               ntag(c, sb) == N_IDENT &&
               cnode_ident(c, sa) == cnode_ident(c, sb) &&
               lookup_ord(c, cnode_ident(c, sa)) != SYM_NONE &&
               !(TYPE_QUALS(rvt(c, a)) & TQ_VOLATILE);
    }
    return false;
}

static void e_arith(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b), rt;
    bool need_int = op == P_PERCENT || op == P_AMP || op == P_PIPE ||
                    op == P_CARET;
    bool zero_div = false;
    long double fa, fb;
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        vec_binop(c, i, a, b, op, false);
        return;
    }
    if (op == P_PLUS) {
        if (is_ptr(c, ta) && is_int(c, tb)) {
            ptr_int(c, i, a, b, false);
            return;
        }
        if (is_int(c, ta) && is_ptr(c, tb)) {
            ptr_int(c, i, b, a, false);
            return;
        }
    } else if (op == P_MINUS) {
        if (is_ptr(c, ta) && is_int(c, tb)) {
            ptr_int(c, i, a, b, true);
            return;
        }
        if (is_ptr(c, ta) && is_ptr(c, tb)) {
            ptr_diff(c, i, a, b);
            return;
        }
    }
    if (need_int ? !(is_int(c, ta) && is_int(c, tb))
                 : !(is_arith(c, ta) && is_arith(c, tb))) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    if (dec_mix(c, i, ta, tb))
        return;
    rt = common_type(c, ta, tb);
    c->ty[i] = rt;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    double_promo(c, i, loc, ta, rt, "to match other operand of binary "
                 "expression");
    double_promo(c, i, loc, tb, rt, "to match other operand of binary "
                 "expression");
    if (!is_complex(c, rt) && is_arith(c, rt)) {
        conv_operand(c, loc, rt, a, false);
        conv_operand(c, loc, rt, b, false);
    }
    if (op == P_CARET)
        xor_pow(c, i, a, b);
    if ((op == P_SLASH || op == P_PERCENT) && is_int(c, tb) &&
        is_intcst(c, b) && c->cv[b] == 0) {
        zero_div = true;
        c->ef[i] |= EF_ZDIV;
        if (!inhibited(c, i, false))
            cwarn(c, loc, DW_DIV_BY_ZERO, "division by zero");
    }
    if (is_complex(c, rt)) {
        long double ar, ai, br, bi, rr, ri;
        if ((op == P_PLUS || op == P_MINUS || op == P_STAR || op == P_SLASH) &&
            cplx_conv(c, a, rt, &ar, &ai) && cplx_conv(c, b, rt, &br, &bi) &&
            cplx_arith(c, op, cplx_comp(c, rt), ar, ai, br, bi, &rr, &ri))
            cplx_set(c, i, rr, ri);
        return;
    }
    if (is_int(c, rt)) {
        if (!zero_div && int_bits(c, rt) <= 64 && has_ival(c, a) &&
            has_ival(c, b)) {
            unsigned fl;
            uint64_t v = ia_binop(op, cx_ia_ty(c, rt), cexpr_trunc(c, rt, c->cv[a]),
                                  cexpr_trunc(c, rt, c->cv[b]), &fl);
            bin_value(c, i, a, b, v, fl & IA_OVF, true, loc);
            return;
        }
        /* fold's identities make x * 0, 0 & x and x - x constants (not
         * integer constant expressions) when x has no side effects */
        if (int_bits(c, rt) <= 64 && fold_zero_ident(c, op, a, b)) {
            c->cv[i] = 0;
            c->ck[i] = K_FOLD;
            return;
        }
    } else if (!is_decimal_flt(c, rt) && !zero_div && fval(c, a, &fa) &&
               fval(c, b, &fb) && !((op == P_SLASH) && fb == 0)) {
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, fl_binop(op, fp_ty(c, rt), fa, fb));
        return;
    }
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
}

void e_binary(Checker *c, uint32_t i)
{
    uint32_t k[2];
    int op = npunct(c, i);
    bool ok;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    if (op == P_COMMA) {
        e_comma(c, i, k[0], k[1]);
        return;
    }
    if (node_err(c, k[0]) || node_err(c, k[1])) {
        set_err(c, i);
        return;
    }
    /* gcc reports the right operand at its operator */
    ok = binop_operand(c, k[0]);
    ok = binop_operand_at(c, k[1], cnode_loc(c, i)) && ok;
    if (!ok) {
        set_err(c, i);
        return;
    }
    if (op == P_SLASH)
        sizeof_div(c, i, k[0], k[1]);
    parens_warn(c, i, k[0], k[1], op);
    if (punct_is_cmp(op)) {
        lognot_warn(c, i, k[0], k[1], op);
        tauto_warn(c, i, k[0], k[1], op);
        array_compare(c, i, k[0], k[1], op);
    }
    switch (op) {
    case P_ANDAND: case P_OROR:
        e_logical(c, i, k[0], k[1], op);
        break;
    case P_LT: case P_GT: case P_LE: case P_GE: case P_EQEQ: case P_NE:
        e_compare(c, i, k[0], k[1], op);
        break;
    case P_SHL: case P_SHR:
        e_shift(c, i, k[0], k[1], op);
        break;
    default:
        e_arith(c, i, k[0], k[1], op);
        break;
    }
}

/* ---- ?: -------------------------------------------------------------------------------- */

SrcLoc colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els)
{
    uint32_t t, from = (mid != NO_NODE ? last_tok(c, mid) : c->nodes[i].tok) + 1,
             to = first_tok(c, els);
    for (t = from; t < to; t++)
        if (tpunct(c, t) == P_COLON)
            return ctok_loc(c, t);
    return cnode_loc(c, i);
}

SrcLoc cexpr_colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els)
{
    return colon_loc(c, i, mid, els);
}

void e_cond(Checker *c, uint32_t i)
{
    uint32_t k[3], n = nkids(c, i, k, 3), cond, mid, els, ch;
    TypeId t1, t2, rt;
    char nb1[48], nb2[48];
    const char *n1, *n2;
    SrcLoc cl;
    bool ok, allint;
    int tv;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    cond = k[0];
    mid = n == 3 ? k[1] : NO_NODE;
    els = k[n - 1];
    if (node_err(c, cond) || node_err(c, els) ||
        (mid != NO_NODE && node_err(c, mid))) {
        set_err(c, i);
        return;
    }
    cl = colon_loc(c, i, mid, els);
    if (mid == NO_NODE) {
        uint32_t q[3], tc;
        size_t n0 = c->diag->all.len, at = c->dm[cfirst(c, els)];
        ped(c, i, cl, "ISO C forbids omitting the middle term of a '?:' "
                      "expression");
        for (tc = strip_paren(c, cond); ntag(c, tc) == N_BINARY &&
             npunct(c, tc) == P_COMMA && nkids(c, tc, q, 3) == 2;
             tc = strip_paren(c, q[1]))
            ;
        if (is_boolish(c, tc) && !node_err(c, tc))
            cwarn(c, cl, DW_PARENTHESES, "the omitted middle operand in '?:' "
                  "will always be 'true', suggest explicit middle operand");
        /* gcc's parser reports both before it reads the third operand */
        if (at < n0 && c->diag->all.len > n0) {
            Diagnostic **d = c->diag->all.data, *tmp[2];
            size_t nd = c->diag->all.len - n0;
            if (nd <= 2) {
                memcpy(tmp, d + n0, nd * sizeof *d);
                memmove(d + at + nd, d + at, (n0 - at) * sizeof *d);
                memcpy(d + at, tmp, nd * sizeof *d);
            }
        }
    }
    ok = binop_operand_at(c, cond, cnode_loc(c, i)) && truth_ok_at(c, cond, cnode_loc(c, i));
    if (ok) {
        no_int_bool = mid == NO_NODE;   /* the condition is also the value */
        cexpr_truth_warn(c, cond, cnode_loc(c, i));
        no_int_bool = false;
    }
    if (mid != NO_NODE && !is_void(c, rvt(c, mid)))
        ok = rvalue_ok(c, mid) && ok;
    if (!is_void(c, rvt(c, els)))
        ok = rvalue_ok(c, els) && ok;
    if (!ok) {
        set_err(c, i);
        return;
    }
    ch = mid != NO_NODE ? mid : cond;
    t1 = rvt(c, ch);
    t2 = rvt(c, els);
    n1 = fnref_builtin(c, ch, &t1, nb1, sizeof nb1);
    n2 = fnref_builtin(c, els, &t2, nb2, sizeof nb2);
    /* operands with side effects are compared later, with the ifs */
    if (mid != NO_NODE && c->func_node != NO_NODE &&
        !((c->ef[mid] | c->ef[els]) & EF_SIDE) &&
        diag_enabled(c->diag, DW_DUPLICATED_BRANCHES) &&
        cstmt_cond_identical(c, i, true))
        cwarn(c, cl, DW_DUPLICATED_BRANCHES, "this condition has identical "
              "branches");
    if (cexpr_cxx_compat(c, i)) {
        TypeId o1 = orig_type(c, ch), o2 = orig_type(c, els);
        if (tkind(c, o1) == TY_ENUM && tkind(c, o2) == TY_ENUM &&
            mainv(c, o1) != mainv(c, o2))
            cwarn(c, cl, DW_CPP_COMPAT, "different enum types in conditional "
                  "is invalid in C++: %s vs %s", type_q(TT, o1),
                  type_q(TT, o2));
    }
    c->ef[i] = (c->ef[cond] | c->ef[els] | (mid != NO_NODE ? c->ef[mid] : 0)) &
               EF_SIDE;
    if (mainv(c, t1) == mainv(c, t2) && !is_arith(c, t1)) {
        rt = unqual(c, t1);
    } else if (is_arith(c, t1) && is_arith(c, t2)) {
        if (dec_mix(c, i, promoted(c, ch), promoted(c, els)))
            return;
        rt = common_type(c, promoted(c, ch), promoted(c, els));
        double_promo(c, i, cl, promoted(c, ch), rt, "to match other result of "
                     "conditional");
        double_promo(c, i, cl, promoted(c, els), rt, "to match other result of "
                     "conditional");
        if (is_int(c, rt) && !is_signed(c, rt) &&
            is_int(c, promoted(c, ch)) && is_int(c, promoted(c, els)) &&
            is_signed(c, promoted(c, ch)) != is_signed(c, promoted(c, els)) &&
            diag_enabled(c->diag, DW_SIGN_COMPARE) && !inhibited(c, i, false)) {
            uint32_t sa = is_signed(c, promoted(c, ch)) ? ch : els;
            uint32_t oa = sa == ch ? els : ch;
            if (!nonneg(c, sa))
                cwarn(c, ctok_loc(c, first_tok(c, sa)), DW_SIGN_COMPARE,
                      "operand of '?:' changes signedness from %s to %s due "
                      "to unsignedness of other operand",
                      type_q(TT, rvt(c, sa)), type_q(TT, rvt(c, oa)));
        }
        if (!is_complex(c, rt)) {
            conv_operand(c, cl, rt, ch, true);
            conv_operand(c, cl, rt, els, true);
        }
    } else if (is_void(c, t1) || is_void(c, t2)) {
        ped(c, i, cl, "ISO C forbids conditional expr with only one void "
                      "side");
        rt = TYPE_B(VOID);
    } else if (is_ptr(c, t1) && is_ptr(c, t2)) {
        TypeId p1 = pointee(c, t1), p2 = pointee(c, t2);
        unsigned q = gq(c, p1) | gq(c, p2);
        if (targets_compat(c, cl, p1, p2)) {
            rt = type_ptr(TT, type_qual(type_composite(TT, mvt(c, p1),
                                                       mvt(c, p2)), q));
        } else if (is_npc(c, ch)) {
            rt = t2;
        } else if (is_npc(c, els)) {
            rt = t1;
        } else if ((is_void(c, p1) && !(tquals(c, p1) & TQ_ATOMIC)) ||
                   (is_void(c, p2) && !(tquals(c, p2) & TQ_ATOMIC))) {
            /* a pointer to atomic void is no void * here */
            TypeId vo = is_void(c, p1) && !(tquals(c, p1) & TQ_ATOMIC) ? p1 : p2,
                   ot = vo == p1 ? p2 : p1;
            if (is_array(c, ot) && (gq(c, ot) & ~TYPE_QUALS(vo)))
                cwarn(c, cl, DW_DISCARDED_ARRAY_QUALIFIERS, "pointer to array "
                      "loses qualifier in conditional expression");
            if (is_func(c, p1) || is_func(c, p2))
                ped(c, i, cl, "ISO C forbids conditional expr between 'void *' "
                              "and function pointer");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), (tquals(c, p1) | tquals(c, p2)) &
                                                   ~(unsigned)TQ_ATOMIC));
        } else {
            if (n1 && n2)
                cpedwarn(c, cl, DW_INCOMPATIBLE_POINTER_TYPES, "pointer type "
                         "mismatch between %s and %s of '%s' and '%s' in "
                         "conditional expression", type_q(TT, t1),
                         type_q(TT, t2), n1, n2);
            else
                cpedwarn(c, cl, DW_NONE, "pointer type mismatch in conditional "
                                    "expression");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), q));
        }
    } else if (tkind(c, t1) == TY_NULLPTR && is_ptr(c, t2)) {
        rt = t2;
    } else if (is_ptr(c, t1) && tkind(c, t2) == TY_NULLPTR) {
        rt = t1;
    } else if (is_ptr(c, t1) && is_int(c, t2)) {
        if (!is_npc(c, els))
            cpedwarn(c, cl, DW_NONE, "pointer/integer type mismatch in conditional "
                                "expression");
        rt = t1;
    } else if (is_int(c, t1) && is_ptr(c, t2)) {
        if (!is_npc(c, ch))
            cpedwarn(c, cl, DW_NONE, "pointer/integer type mismatch in conditional "
                                "expression");
        rt = t2;
    } else {
        cerror(c, cl, "type mismatch in conditional expression");
        set_err(c, i);
        return;
    }
    c->ty[i] = rt;
    allint = intops(c, cond) && intops(c, els) &&
             (mid == NO_NODE || intops(c, mid));
    if (allint)
        c->ef[i] |= EF_INTOPS;
    tv = truth(c, cond, true);
    if (tv >= 0 && (is_scalar(c, rt))) {
        uint32_t pick = tv ? ch : els;
        uint32_t keep = c->ef[i];
        if (c->ck[pick] == K_NONE || c->ck[pick] == K_ERR)
            return;
        conv_const(c, i, pick, rt);
        c->ef[i] |= keep;
        if (c->ck[i] == K_ICE || c->ck[i] == K_FOLD) {
            if (allint && c->ck[cond] == K_ICE && is_intcst(c, pick)) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS | (c->ef[pick] & EF_OVERFLOW);
            } else {
                c->ck[i] = K_FOLD;
                c->ef[i] &= ~(EF_CST | EF_NOPCST);
            }
        }
    }
}

/* ---- assignment ------------------------------------------------------------------------- */

void e_assign(Checker *c, uint32_t i)
{
    uint32_t k[2], l, r;
    int op = npunct(c, i);
    SrcLoc loc = cnode_loc(c, i);
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    l = k[0];
    r = k[1];
    if (node_err(c, l) || node_err(c, r)) {
        set_err(c, i);
        return;
    }
    if (!(c->ef[l] & EF_LVALUE) || is_func(c, c->ty[l])) {
        cerror(c, loc, "lvalue required as left operand of assignment");
        set_err(c, i);
        return;
    }
    if (is_array(c, c->ty[l])) {
        if (!complete(c, c->ty[l]))
            incomplete_error(c, loc, l, c->ty[l]);
        else
            cerror(c, loc, "assignment to expression with array type");
        set_err(c, i);
        return;
    }
    if (op == P_ASSIGN) {
        bool bad = !rvalue_ok(c, r);
        TypeId lt = unqual(c, c->ty[l]);
        if (!is_void(c, lt) && !complete(c, lt)) {
            incomplete_error(c, loc, l, c->ty[l]);
            bad = true;
        }
        if (bad) {
            set_err(c, i);
            return;
        }
    }
    /* _Bool = a = b: the inner assignment is a truth value, unless it is
     * itself a _Bool (nothing is converted, `*r = *m = false`) */
    if (op == P_ASSIGN && tkind(c, unqual(c, c->ty[l])) == TY_BOOL &&
        ntag(c, r) == N_ASSIGN && npunct(c, r) == P_ASSIGN &&
        tkind(c, unqual(c, c->ty[r])) != TY_BOOL)
        PW(first_loc(c, i), "suggest parentheses around assignment used as "
           "truth value");
    {
        ConvInfo ci;
        TypeId lt = unqual(c, c->ty[l]);
        memset(&ci, 0, sizeof ci);
        ci.context = CONV_ASSIGN;
        ci.loc = loc;
        ci.lhs_bitfield = (c->ef[l] & EF_BITFIELD) != 0;
        ci.lhs_bits = bf_width(c, l);
        if (op != P_ASSIGN) {
            /* a op= b: the operation first, then the conversion of its
             * result */
            int bop = punct_assign_base(op);
            bool ok = binop_operand(c, l);
            ok = binop_operand(c, r) && ok;
            if (ok) {
                if (bop == P_SHL || bop == P_SHR)
                    e_shift(c, i, l, r, bop);
                else
                    e_arith(c, i, l, r, bop);
                ok = !node_err(c, i);
            }
            if (!ok) {
                /* gcc goes on to the read-only check */
                readonly_check(c, l, loc, 0);
                set_err(c, i);
                return;
            }
            c->ck[i] = K_NONE;
            c->cb[i] = 0;
            c->ef[i] = 0;
            ci.eloc = first_loc(c, r);
        }
        if (readonly_check(c, l, loc, 0)) {
            set_err(c, i);
            return;
        }
        if (is_array(c, c->ty[l])) {
            cerror(c, loc, "assignment to expression with array type");
            set_err(c, i);
            return;
        }
        if (!cexpr_assign_check(c, op != P_ASSIGN ? i : r, lt, &ci)) {
            set_err(c, i);
            return;
        }
    }
    c->ck[i] = K_NONE;
    c->ty[i] = unqual(c, c->ty[l]);
    c->ef[i] = EF_SIDE | (c->ef[strip_paren(c, l)] & EF_BFPROMOTE);
}

