/* cexpr.c - expressions: their types, lvalue-ness and constant values
 * (C99 6.5, 6.6), with gcc 13's diagnostics (c-typeck.cc build_binary_op,
 * build_unary_op, build_c_cast, build_component_ref, ...; c-parser.cc for
 * the order and locations).
 *
 * Each expression node gets, in the checker's per-node arrays:
 *   ty  its type before lvalue conversion (arrays and functions undecayed);
 *   ck  what kind of constant its value (after lvalue conversion) is, and
 *   cv / cb  that value: an integer (truncated to the type, sign-extended if
 *       signed), an index into Checker.fv, or an address (cb base, cv byte
 *       offset);
 *   ef  flags (lvalue, bit-field, null pointer constant, gcc's tree codes
 *       as far as constant folding and its warnings depend on them).
 *
 * gcc's constant kinds map to ours: an INTEGER_CST built at parse time from
 * integer constant operands is K_ICE (EF_OVERFLOW: TREE_OVERFLOW); a value
 * gcc only finds when it folds the full expression (c_fully_fold) is
 * K_FOLD; EF_INTOPS is C_MAYBE_CONST_EXPR_INT_OPERANDS. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"
#include "c/tast.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- context --------------------------------------------------------------- */

/* gcc's c_inhibit_evaluation_warnings at node i: inside sizeof, alignof,
 * typeof, a _Generic selector, or an operand not evaluated because of a
 * constant condition. */
bool inhibited(Checker *c, uint32_t i, bool fold)
{
    uint32_t ch = i, p;
    for (p = c->par[i]; p != NO_NODE; ch = p, p = c->par[p]) {
        uint32_t f;
        switch (c->nodes[p].tag) {
        case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
        case N_ALIGNOF_TYPE: case N_TYPEOF:
            return true;
        case N_GENERIC:
            if (ch == first_child(c, p))
                return true;
            break;
        case N_BINARY:
            if (ch == p - 1 && (npunct(c, p) == P_ANDAND ||
                                npunct(c, p) == P_OROR)) {
                f = first_child(c, p);
                if (f != NO_NODE && f != ch &&
                    truth(c, f, fold) == (npunct(c, p) == P_ANDAND ? 0 : 1))
                    return true;
            }
            break;
        case N_COND: {
            uint32_t k[3], n = nkids(c, p, k, 3);
            int tv;
            if (n < 2 || ch == k[0])
                break;
            tv = truth(c, k[0], fold);
            if (tv < 0)
                break;
            if (ch == k[n - 1] && tv == 1)
                return true;
            if (n == 3 && ch == k[1] && tv == 0)
                return true;
            break;
        }
        default:
            break;
        }
    }
    return false;
}

bool cexpr_cxx_compat(Checker *c, uint32_t node)
{
    return diag_enabled(c->diag, DW_CPP_COMPAT) &&
           (node == NO_NODE || !cexpr_in_extension(c, node));
}

bool cexpr_in_extension(Checker *c, uint32_t i)
{
    uint32_t p;
    for (p = i; p != NO_NODE; p = c->par[p]) {
        const Node *n = &c->nodes[p];
        if (n->tag == N_UNARY && tckw(c, n->tok) == CK_EXTENSION)
            return true;
        if ((n->tag == N_DECL || n->tag == N_FUNC_DEF ||
             n->tag == N_STATIC_ASSERT) &&
            (n->flags & NF_EXTENSION))
            return true;
    }
    return false;
}

/* A -Wpedantic pedwarn about node i (off under __extension__). */
Diagnostic *ped(Checker *c, uint32_t i, SrcLoc loc, const char *fmt,
                       ...)
{
    char buf[512];
    va_list ap;
    if (!c->opt.pedantic || cexpr_in_extension(c, i))
        return NULL;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return cpedantic(c, loc, "%s", buf);
}

/* A pedwarn under -Wpointer-arith (-pedantic enables it). */
void ped_arith(Checker *c, uint32_t i, SrcLoc loc, const char *msg)
{
    if (cexpr_in_extension(c, i))
        return;
    cpedwarn(c, loc, DW_POINTER_ARITH, "%s", msg);
}

/* ---- shared checks ----------------------------------------------------------- */

/* The element type of an array type, with the array's qualifiers. */
TypeId elem_of(Checker *c, TypeId t)
{
    const TypeEnt *e = type_ent(TT, t);
    unsigned q = TYPE_QUALS(t);
    while (e->kind == TY_TYPEDEF) {
        q |= TYPE_QUALS(e->base);
        e = type_ent(TT, e->base);
    }
    return type_qual(type_base(TT, t), q);
}

/* gcc's EXPR_LOCATION of an expression, as far as it matters. */
SrcLoc expr_loc(Checker *c, uint32_t i)
{
    switch (ntag(c, i)) {
    case N_PAREN:
        return i > 0 && c->nodes[i].size > 1 ? expr_loc(c, i - 1)
                                             : cnode_loc(c, i);
    case N_CALL:
        return first_loc(c, i);
    case N_MEMBER_EXPR: case N_ADDR_LABEL:
        return ctok_loc(c, c->nodes[i].tok - 1);
    case N_UNARY:   /* __extension__ makes no node of its own in gcc */
        if (cexpr_is_extension(c, i) && i > 0)
            return expr_loc(c, i - 1);
        return cnode_loc(c, i);
    default:
        return cnode_loc(c, i);
    }
}

/* c_incomplete_type_error (value: a variable or parameter, else
 * NO_NODE). */
void incomplete_error(Checker *c, SrcLoc loc, uint32_t value, TypeId t)
{
    TypeId ct;
    if (is_err(c, t))
        return;
    if (value != NO_NODE && ntag(c, strip_paren(c, value)) == N_IDENT) {
        uint32_t v = strip_paren(c, value);
        uint32_t ref = lookup_ord(c, cnode_ident(c, v));
        if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ) {
            cerror(c, loc, "'%s' has an incomplete type %s",
                   cident(c, cnode_ident(c, v)), type_q(TT, t));
            return;
        }
    }
    for (;;) {
        ct = type_canon(TT, t);
        switch (tkind(c, ct)) {
        case TY_VOID:
            cerror(c, loc, "invalid use of void expression");
            return;
        case TY_ARRAY:
            if (type_ent(TT, ct)->flags & TF_INCOMPLETE) {
                cerror(c, loc, (type_ent(TT, ct)->flags & TF_FLEX) ?
                       "invalid use of flexible array member" :
                       "invalid use of array with unspecified bounds");
                return;
            }
            t = type_base(TT, ct);
            continue;
        default:
            break;
        }
        break;
    }
    if (type_ent(TT, TYPE_UNQUAL(t))->kind == TY_TYPEDEF)
        cerror(c, loc, "invalid use of incomplete typedef %s", type_q(TT, t));
    else
        cerror(c, loc, "invalid use of undefined type %s", type_q(TT, t));
}

bool complete(Checker *c, TypeId t)
{
    return type_is_complete(TT, t);
}

/* The size in bytes of what a pointer of type pt points to, for
 * arithmetic (void and functions: 1). */
uint64_t elem_size(Checker *c, TypeId pt)
{
    bool ok;
    TypeId b = pointee(c, pt);
    uint64_t n;
    if (is_void(c, b) || is_func(c, b))
        return 1;
    n = type_size(TT, b, &ok);
    return ok ? n : 1;
}

/* pointer_int_sum's checks of the pointer operand (loc: the operator);
 * false after an error. */
bool ptr_arith_ok(Checker *c, uint32_t i, SrcLoc loc, TypeId pt)
{
    TypeId b = pointee(c, pt);
    if (is_void(c, b)) {
        ped_arith(c, i, loc, "pointer of type 'void *' used in arithmetic");
        return true;
    }
    if (is_func(c, b)) {
        ped_arith(c, i, loc, "pointer to a function used in arithmetic");
        return true;
    }
    if (!complete(c, b)) {
        incomplete_error(c, loc, NO_NODE, b);
        return false;
    }
    return true;
}

/* An lvalue at a constant address: an rvalue constant too if it is an
 * array or a function (it decays to its address). */
void addr_rvalue(Checker *c, uint32_t i)
{
    if ((c->ef[i] & EF_ADDRLV) && (is_array(c, c->ty[i]) ||
                                    is_func(c, c->ty[i])) &&
        tkind(c, c->ty[i]) != TY_VLA)
        c->ck[i] = K_ADDR;
}

/* ---- the dispatcher ------------------------------------------------------------------------- */

bool cexpr_is_expr(unsigned tag)
{
    switch (tag) {
    case N_IDENT: case N_NUMBER: case N_CHAR: case N_STRING: case N_PAREN:
    case N_CALL: case N_INDEX: case N_MEMBER_EXPR: case N_POSTFIX:
    case N_UNARY: case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
    case N_ALIGNOF_TYPE: case N_CAST: case N_COMPOUND_LIT: case N_BINARY:
    case N_ASSIGN: case N_COND: case N_STMT_EXPR: case N_VA_ARG:
    case N_OFFSETOF: case N_TYPES_COMPAT: case N_CONVERTVECTOR: case N_GENERIC:
    case N_GENERIC_ASSOC: case N_ADDR_LABEL: case N_HAS_ATTR:
        return true;
    default:
        return false;
    }
}

/* The overflow warnings gcc gives when it folds the whole expression rooted
 * at i (c_fully_fold). */
static void fold_flush(Checker *c, uint32_t i)
{
    uint32_t k;
    for (k = cfirst(c, i); k <= i && c->fold_pending; k++) {
        TypeId t;
        if (!(c->ef[k] & EF_FOLDWARN))
            continue;
        c->ef[k] &= ~EF_FOLDWARN;
        c->fold_pending--;
        if (inhibited(c, k, true))
            continue;
        t = c->ty[k];
        if (ntag(c, k) == N_UNARY) {
            uint32_t a = first_child(c, k);
            cwarn(c, cnode_loc(c, k), DW_OVERFLOW, "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, a),
                  type_q(TT, t), vstr(c, t, c->cv[k]));
        } else if (ntag(c, k) == N_BINARY) {
            uint32_t kk[2], x, y;
            size_t ol;
            const char *op;
            /* An operand folded from a binary expression is a bare constant
             * without a location (unlike a folded cast), so gcc prints its
             * value and reports at input_location. */
            if (nkids(c, k, kk, 2) == 2 &&
                (x = strip_paren(c, kk[0])) != NO_NODE &&
                (y = strip_paren(c, kk[1])) != NO_NODE &&
                has_ival(c, x) && has_ival(c, y) &&
                (ntag(c, x) == N_NUMBER || ntag(c, x) == N_BINARY) &&
                (ntag(c, y) == N_NUMBER || ntag(c, y) == N_BINARY) &&
                (ntag(c, x) == N_BINARY || ntag(c, y) == N_BINARY) &&
                (op = ttext(c, c->nodes[k].tok, &ol)) != NULL) {
                cwarn(c, after_loc(c, k), DW_OVERFLOW, "integer overflow in "
                      "expression '%s %.*s %s' of type %s results in '%s'",
                      ntag(c, x) == N_NUMBER ? estr(c, x)
                                             : vstr(c, c->ty[x], c->cv[x]),
                      (int)ol, op,
                      ntag(c, y) == N_NUMBER ? estr(c, y)
                                             : vstr(c, c->ty[y], c->cv[y]),
                      type_q(TT, t), vstr(c, t, c->cv[k]));
                continue;
            }
            cwarn(c, cnode_loc(c, k), DW_OVERFLOW, "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, k),
                  type_q(TT, t), vstr(c, t, c->cv[k]));
        } else {
            cwarn(c, cnode_loc(c, k), DW_OVERFLOW, "integer overflow in "
                  "expression of type %s results in '%s'", type_q(TT, t),
                  vstr(c, t, c->cv[k]));
        }
    }
}

/* gcc -O: c_fully_fold replaces a read of a const, non-volatile integer
 * variable that has a constant initializer by that value (decl_constant_
 * value), inside a function or an initializer.  Done where the identifier is an operand;
 * &a, a++, a = .. and the like keep the variable. */
static void fold_const_var(Checker *c, uint32_t i)
{
    uint32_t t = i, p = c->par[i], ref;
    const CSym *s;
    if (!in_function(c)) {
        /* file scope: only an initializer folds (gcc, in_init), not an array
         * bound or a declarator */
        uint32_t r = i;
        while (c->par[r] != NO_NODE && cexpr_is_expr(ntag(c, c->par[r])))
            r = c->par[r];
        if (c->par[r] == NO_NODE || ntag(c, c->par[r]) != N_INIT_DECL ||
            r != c->par[r] - 1)
            return;
    }
    while (p != NO_NODE && ntag(c, p) == N_PAREN) {
        t = p;
        p = c->par[t];
    }
    if (p == NO_NODE)
        return;
    switch (ntag(c, p)) {
    case N_BINARY: case N_COND: case N_CAST: case N_INIT_DECL:
        break;
    case N_UNARY:
        if (npunct(c, p) != P_PLUS && npunct(c, p) != P_MINUS &&
            npunct(c, p) != P_TILDE && npunct(c, p) != P_BANG)
            return;
        break;
    case N_ASSIGN:
        if (t != p - 1)
            return;
        break;
    default:
        return;
    }
    ref = lookup_ord(c, cnode_ident(c, i));
    if (ref == SYM_NONE)
        return;
    s = csym(c, ref);
    if (s->kind != CS_OBJ || !(s->flags & CSF_CONST_VAL) ||
        !is_int(c, c->ty[i]) || (tquals(c, s->ty) & TQ_VOLATILE))
        return;
    c->ck[i] = K_FOLD;
    c->cv[i] = s->val;
    c->cb[i] = 0;
    c->ef[i] = EF_CST;
}

void cexpr_node(Checker *c, uint32_t i)
{
    uint32_t p;
    switch (ntag(c, i)) {
    case N_IDENT:
        e_ident(c, i);
        if (c->opt.opt_level && c->opt.opt_level != '0' && c->ck[i] != K_ERR &&
            c->ck[i] != K_ICE && !(c->ef[i] & EF_SIDE))
            fold_const_var(c, i);
        break;
    case N_NUMBER: e_number(c, i); break;
    case N_CHAR: e_char(c, i); break;
    case N_STRING: e_string(c, i); break;
    case N_PAREN: e_paren(c, i); break;
    case N_CALL: e_call(c, i); break;
    case N_INDEX: {
        /* a subscript cut short by a syntax error is checked after it */
        DiagOrd o0 = diag_ord(c->diag, c->nodes[i].flags & NF_CUT ? ORD_CUT
                                                                  : ORD_NORMAL);
        e_index(c, i);
        diag_ord(c->diag, o0);
        break;
    }
    case N_MEMBER_EXPR: e_member(c, i); break;
    case N_POSTFIX: e_postfix(c, i); break;
    case N_UNARY: e_unary(c, i); break;
    case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: e_sizeof(c, i, false); break;
    case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE: e_sizeof(c, i, true); break;
    case N_CAST: e_cast(c, i); break;
    case N_COMPOUND_LIT: e_complit(c, i); break;
    case N_BINARY: e_binary(c, i); break;
    case N_ASSIGN: e_assign(c, i); break;
    case N_COND: e_cond(c, i); break;
    case N_STMT_EXPR: e_stmt_expr(c, i); break;
    case N_VA_ARG: e_va_arg(c, i); break;
    case N_OFFSETOF: e_offsetof(c, i); break;
    case N_TYPES_COMPAT: e_types_compat(c, i); break;
    case N_HAS_ATTR: e_has_attr(c, i); break;
    case N_CONVERTVECTOR: e_convertvector(c, i); break;
    case N_GENERIC: e_generic(c, i); break;
    case N_GENERIC_ASSOC: e_generic_assoc(c, i); break;
    case N_ADDR_LABEL: e_addr_label(c, i); break;
    default: set_err(c, i); break;
    }
    if (ntag(c, i) == N_MEMBER_EXPR || ntag(c, i) == N_INDEX)
        sso_decay(c, i);
    p = c->par[i];
    if (p != NO_NODE && !cexpr_is_expr(ntag(c, p))) {
        switch (ntag(c, p)) {
        case N_EXPR_STMT: case N_IF: case N_WHILE: case N_DO: case N_FOR:
        case N_RETURN: case N_SWITCH: case N_INIT_DECL: case N_INIT_LIST:
            sq_check(c, i, ntag(c, p) == N_IF || ntag(c, p) == N_WHILE ||
                           ntag(c, p) == N_DO ||
                           (ntag(c, p) == N_FOR && sq_for_cond(c, p, i)));
            break;
        default:
            break;
        }
    }
    /* a null pointer constant: an integer constant expression with value 0 */
    if (is_intcst(c, i) && !(c->ef[i] & EF_OVERFLOW) && c->cv[i] == 0 &&
        is_int(c, c->ty[i]) && !constvar_in(c, i) &&
        !(ntag(c, strip_paren(c, i)) == N_BINARY &&
          npunct(c, strip_paren(c, i)) == P_COMMA))
        c->ef[i] |= EF_NPC;
    p = c->par[i];
    if (c->fold_pending && (p == NO_NODE || !cexpr_is_expr(ntag(c, p))))
        fold_flush(c, i);
}
