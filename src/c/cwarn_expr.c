/* cwarn_expr.c - the checker's expression warnings: -Wlogical-op, the
 * -Wall/-Wextra family, -Wtautological-compare, -Wparentheses, -Warray-compare,
 * -Waddress, -Wsizeof-pointer-memaccess and -Wsizeof-pointer-div (split from
 * cexpr.c).  The shared readers are cexpr_int.h's. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* ---- -Wlogical-op ----------------------------------------------------------
 * c-family warn_logical_operator: a non-boolean constant operand, operands
 * that are the same test, and range tests on one expression that are always
 * false (&&) or always true (||). */

__extension__ typedef __int128 i128;

typedef struct {
    int op;          /* P_EQEQ, P_NE, P_LT, P_LE, P_GT, P_GE */
    uint32_t l, r;   /* r == NO_NODE: compared with zero */
} LForm;

typedef struct {
    int n;
    i128 lo[4], hi[4];
} IvSet;

static bool lg_cmp(int op)
{
    return op == P_EQEQ || op == P_NE || op == P_LT || op == P_LE ||
           op == P_GT || op == P_GE;
}

static int lg_swap(int op)
{
    switch (op) {
    case P_LT: return P_GT;
    case P_GT: return P_LT;
    case P_LE: return P_GE;
    case P_GE: return P_LE;
    default: return op;
    }
}

static int lg_inv(int op)
{
    switch (op) {
    case P_EQEQ: return P_NE;
    case P_NE: return P_EQEQ;
    case P_LT: return P_GE;
    case P_GE: return P_LT;
    case P_GT: return P_LE;
    default: return P_GT;
    }
}

/* e without the conversions fold removes from a comparison with zero. */
static uint32_t lg_strip(Checker *c, uint32_t e)
{
    for (;;) {
        uint32_t k[3], n;
        e = strip_paren(c, e);
        if (ntag(c, e) != N_CAST || (n = nkids(c, e, k, 3)) < 1)
            return e;
        if (!is_int(c, rvt(c, e)) || !is_int(c, rvt(c, k[n - 1])) ||
            int_bits(c, rvt(c, e)) < int_bits(c, rvt(c, k[n - 1])))
            return e;
        e = k[n - 1];
    }
}

/* A null pointer or integer zero constant. */
static bool lg_zero(Checker *c, uint32_t n)
{
    return is_npc(c, n) || (c->ck[n] == K_ADDR && !c->cb[n] && !c->cv[n]);
}

/* The operand as the comparison gcc builds from it (c_common_truthvalue_
 * conversion, with ! folded into the comparison).  False if not a test. */
static bool lg_form(Checker *c, uint32_t n, LForm *f)
{
    uint32_t k[3], cnt;
    n = strip_paren(c, n);
    cnt = nkids(c, n, k, 3);
    if (ntag(c, n) == N_BINARY && lg_cmp(npunct(c, n)) && cnt >= 2) {
        f->op = npunct(c, n);
        f->l = k[0];
        f->r = k[1];
        /* x == 0 and x != 0 are the truth conversions of x and !x */
        if (f->op == P_EQEQ || f->op == P_NE) {
            if (lg_zero(c, f->r) && !lg_zero(c, f->l)) {
                f->r = NO_NODE;
            } else if (lg_zero(c, f->l) && !lg_zero(c, f->r)) {
                f->l = f->r;
                f->r = NO_NODE;
            }
            if (f->r == NO_NODE)
                f->l = lg_strip(c, f->l);
        }
        return true;
    }
    if (ntag(c, n) == N_BINARY &&
        (npunct(c, n) == P_ANDAND || npunct(c, n) == P_OROR))
        return false;
    if (ntag(c, n) == N_UNARY && npunct(c, n) == P_BANG && cnt >= 1) {
        uint32_t in = strip_paren(c, k[0]);
        if (!lg_form(c, in, f))
            return false;
        if (f->r != NO_NODE && (is_flt(c, rvt(c, f->l)) || is_flt(c, rvt(c, f->r))))
            return false;
        f->op = lg_inv(f->op);
        return true;
    }
    if (ntag(c, n) == N_CAST && tkind(c, rvt(c, n)) == TY_BOOL && cnt >= 1)
        return lg_form(c, k[cnt - 1], f);
    f->op = P_NE;
    f->l = lg_strip(c, n);
    f->r = NO_NODE;
    return true;
}

static bool lg_eqn(Checker *c, uint32_t x, uint32_t y)
{
    if (x == NO_NODE || y == NO_NODE)
        return x == y;
    return opeq(c, x, y);
}

static bool lg_same(Checker *c, const LForm *a, const LForm *b)
{
    return (a->op == b->op && lg_eqn(c, a->l, b->l) && lg_eqn(c, a->r, b->r)) ||
           (a->r != NO_NODE && b->r != NO_NODE && a->op == lg_swap(b->op) &&
            lg_eqn(c, a->l, b->r) && lg_eqn(c, a->r, b->l));
}

/* A test gcc's warning looks at: a comparison, ! or integral value. */
static bool lg_eligible(Checker *c, uint32_t n)
{
    n = strip_paren(c, n);
    if (ntag(c, n) == N_BINARY && (lg_cmp(npunct(c, n)) ||
                                   npunct(c, n) == P_ANDAND ||
                                   npunct(c, n) == P_OROR))
        return true;
    if (ntag(c, n) == N_UNARY && npunct(c, n) == P_BANG)
        return true;
    return is_int(c, rvt(c, n));
}

static i128 lg_val(Checker *c, uint32_t n)
{
    bool neg;
    uint64_t mag;
    cst_parts(c, n, &neg, &mag);
    return neg ? -(i128)mag : (i128)mag;
}

static void iv_add(IvSet *s, i128 lo, i128 hi)
{
    if (lo <= hi && s->n < 4) {
        s->lo[s->n] = lo;
        s->hi[s->n++] = hi;
    }
}

/* The values v in [mn, mx] with 'v op k'. */
static void iv_cmp(IvSet *s, int op, i128 k, i128 mn, i128 mx)
{
    s->n = 0;
    switch (op) {
    case P_EQEQ: iv_add(s, k < mn ? mx + 1 : k, k > mx ? mn - 1 : k); break;
    case P_NE:
        iv_add(s, mn, k - 1 < mx ? k - 1 : mx);
        iv_add(s, k + 1 > mn ? k + 1 : mn, mx);
        if (k < mn || k > mx) {
            s->n = 0;
            iv_add(s, mn, mx);
        }
        break;
    case P_LT: iv_add(s, mn, k - 1 < mx ? k - 1 : mx); break;
    case P_LE: iv_add(s, mn, k < mx ? k : mx); break;
    case P_GT: iv_add(s, k + 1 > mn ? k + 1 : mn, mx); break;
    default: iv_add(s, k > mn ? k : mn, mx); break;
    }
}

static void iv_comp(IvSet *r, const IvSet *s, i128 mn, i128 mx)
{
    i128 at = mn;
    int i;
    r->n = 0;
    for (i = 0; i < s->n; i++) {
        iv_add(r, at, s->lo[i] - 1);
        at = s->hi[i] + 1;
    }
    iv_add(r, at, mx);
}

static void iv_and(IvSet *r, const IvSet *a, const IvSet *b)
{
    int i, j;
    r->n = 0;
    for (i = 0; i < a->n; i++)
        for (j = 0; j < b->n; j++)
            iv_add(r, a->lo[i] > b->lo[j] ? a->lo[i] : b->lo[j],
                   a->hi[i] < b->hi[j] ? a->hi[i] : b->hi[j]);
}

/* The expression a test constrains and the values that make it true. */
static bool lg_atom(Checker *c, const LForm *f, uint32_t *e, IvSet *s,
                    i128 *mn, i128 *mx)
{
    int op = f->op;
    i128 k = 0;
    TypeId t;
    unsigned bits;
    if (f->r == NO_NODE) {
        *e = f->l;
    } else if (is_intcst(c, f->r) && !is_intcst(c, f->l)) {
        *e = f->l;
        k = lg_val(c, f->r);
    } else if (is_intcst(c, f->l) && !is_intcst(c, f->r)) {
        *e = f->r;
        op = lg_swap(op);
        k = lg_val(c, f->l);
    } else {
        return false;
    }
    t = rvt(c, *e);
    if (is_ptr(c, t)) {
        *mn = 0;
        *mx = ((i128)1 << int_bits(c, t)) - 1;
    } else if (is_int(c, t)) {
        bits = tkind(c, t) == TY_BOOL ? 1 : int_bits(c, t);
        if (is_signed(c, t)) {
            *mn = -((i128)1 << (bits - 1));
            *mx = ((i128)1 << (bits - 1)) - 1;
        } else {
            *mn = 0;
            *mx = ((i128)1 << bits) - 1;
        }
    } else {
        return false;
    }
    iv_cmp(s, op, k, *mn, *mx);
    return true;
}

/* An operator expression from a macro (constants and names carry no location
 * in gcc's tree). */
static bool lg_macro(Checker *c, uint32_t n)
{
    return ntag(c, n) != N_IDENT && ntag(c, n) != N_NUMBER &&
           ntag(c, n) != N_CHAR && from_macro(c, c->nodes[n].tok);
}

static void logical_op_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    bool orop = op == P_OROR;
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b), ea, eb;
    SrcLoc loc = cnode_loc(c, i);
    LForm fa, fb;
    IvSet ia, ib, ca, cb, r;
    i128 mna, mxa, mnb, mxb;
    bool okb;
    if (!diag_enabled(c->diag, "logical-op") || inhibited(c, i, false) ||
        lg_macro(c, sa) || lg_macro(c, sb))
        return;
    if (is_intcst(c, sb) && is_int(c, rvt(c, sb)) && truth(c, sa, true) < 0 &&
        !(ntag(c, sb) == N_BINARY && npunct(c, sb) == P_COMMA) &&
        !is_intcst(c, sa)) {
        bool neg;
        uint64_t mag;
        int ta = ntag(c, a), pa = npunct(c, a);
        bool truthy = (ta == N_BINARY && (lg_cmp(pa) || pa == P_ANDAND ||
                                          pa == P_OROR)) ||
                      (ta == N_UNARY && pa == P_BANG);
        cst_parts(c, sb, &neg, &mag);
        if (!truthy && (neg || mag > 1)) {
            cwarn(c, loc, "logical-op", "logical '%s' applied to non-boolean "
                  "constant", orop ? "or" : "and");
            return;
        }
    }
    if (truth(c, sa, true) >= 0 || truth(c, sb, true) >= 0)
        return;
    if (!lg_form(c, sa, &fa) || !(okb = lg_form(c, sb, &fb)))
        return;
    (void)okb;
    /* the left operand is already a truth value, the right one is not */
    if (lg_eligible(c, sb) && lg_same(c, &fa, &fb)) {
        cwarn(c, loc, "logical-op", "logical '%s' of equal expressions",
              orop ? "or" : "and");
        return;
    }
    if (!lg_atom(c, &fa, &ea, &ia, &mna, &mxa) ||
        !lg_atom(c, &fb, &eb, &ib, &mnb, &mxb) || mna != mnb || mxa != mxb ||
        !opeq(c, ea, eb))
        return;
    iv_comp(&ca, &ia, mna, mxa);
    iv_comp(&cb, &ib, mna, mxa);
    if (orop ? (ca.n == 0 || cb.n == 0) : (ia.n == 0 || ib.n == 0))
        return;     /* one side alone is already always true / false */
    if (orop) {
        iv_and(&r, &ca, &cb);
        if (r.n == 0)
            cwarn(c, loc, "logical-op", "logical 'or' of collectively "
                  "exhaustive tests is always true");
    } else {
        iv_and(&r, &ia, &ib);
        if (r.n == 0)
            cwarn(c, loc, "logical-op", "logical 'and' of mutually exclusive "
                  "tests is always false");
    }
}

void e_logical(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    bool andand = op == P_ANDAND, ok;
    int ta, tb, r = -1;
    /* gcc converts the left operand when it sees the operator and stops at an
     * error; a right operand of aggregate type is just an invalid operand */
    ok = truth_ok(c, a);
    if (ok && (is_record(c, rvt(c, b)) || tkind(c, rvt(c, b)) == TY_VECTOR)) {
        vec_invalid(c, i, a, b, op, TYPE_B(INT), 0);   /* a is already an int */
        return;
    }
    ok = ok && truth_ok(c, b);
    if (!ok) {
        set_err(c, i);
        return;
    }
    logical_op_warn(c, i, a, b, op);
    cexpr_truth_warn(c, a, first_loc(c, a));
    cexpr_truth_warn(c, b, cnode_loc(c, i));
    c->ty[i] = TYPE_B(INT);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    ta = truth(c, a, true);
    tb = truth(c, b, true);
    if (ta >= 0 && tb >= 0)
        r = andand ? (ta && tb) : (ta || tb);
    else if (ta >= 0 && ta == !andand)
        r = !andand;
    if (r < 0) {
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
        return;
    }
    c->cv[i] = (uint64_t)r;
    /* DR#031: an unevaluated integer-operand right side (2 || 1/0) is OK */
    if (is_intcst(c, a) && (is_intcst(c, b) ||
        (!(tb >= 0) && c->ck[a] == K_ICE && intops(c, b)))) {
        c->ck[i] = K_ICE;
        c->ef[i] |= EF_INTOPS | ((c->ef[a] | c->ef[b]) & EF_OVERFLOW);
        return;
    }
    c->ck[i] = K_FOLD;
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
}

bool is_npc(Checker *c, uint32_t n)
{
    return (c->ef[n] & EF_NPC) != 0;
}

bool extra_on(Checker *c)
{
    return diag_enabled(c->diag, "sign-compare");
}

/* Is the node a zero character constant (for -Wpointer-compare)? */
static bool char_zero(Checker *c, uint32_t n)
{
    TypeId t;
    if (!is_npc(c, n))
        return false;
    if (ntag(c, strip_paren(c, n)) == N_CHAR)
        return true;
    t = mainv(c, rvt(c, n));
    return t == TYPE_B(CHAR) || t == TYPE_B(SCHAR) || t == TYPE_B(UCHAR);
}

static bool addr_val(Checker *c, uint32_t n, uint32_t *cb, uint64_t *cv)
{
    if (c->ck[n] == K_ADDR) {
        *cb = c->cb[n];
        *cv = c->cv[n];
        return true;
    }
    if (has_ival(c, n)) {
        *cb = 0;
        *cv = c->cv[n];
        return true;
    }
    return false;
}

/* A base symbol whose address may equal another's (weak) or is not
 * an object or function. */
static bool addr_weak(Checker *c, uint32_t cb)
{
    CSym *s = csym(c, cb - 1);
    return (s->kind != CS_OBJ && s->kind != CS_FUNC) ||
           (s->flags & CSF_WEAK);
}

/* ---- -Wall / -Wextra expression warnings -------------------------------------
 * -Wsign-compare, -Wtype-limits, -Wbool-compare, -Wtautological-compare,
 * -Wparentheses, -Wlogical-not-parentheses, -Warray-compare, -Waddress and
 * -Wsizeof-pointer-div (gcc's shorten_compare, warn_for_sign_compare,
 * warn_about_parentheses, maybe_warn_for_null_address, ...). */

bool is_cmp_op(int op)
{
    return op == P_EQEQ || op == P_NE || op == P_LT || op == P_GT ||
           op == P_LE || op == P_GE;
}

static const char *cmp_str(int op)
{
    switch (op) {
    case P_EQEQ: return "==";
    case P_NE: return "!=";
    case P_LT: return "<";
    case P_GT: return ">";
    case P_LE: return "<=";
    default: return ">=";
    }
}

static int swap_cmp(int op)
{
    switch (op) {
    case P_LT: return P_GT;
    case P_GT: return P_LT;
    case P_LE: return P_GE;
    case P_GE: return P_LE;
    default: return op;
    }
}

/* CONSTANT_CLASS_P of the folded operand. */
static bool cst_class(Checker *c, uint32_t n)
{
    return c->ck[n] == K_ICE || c->ck[n] == K_FOLD || c->ck[n] == K_FLOAT;
}

/* gcc's truth_value_p of the operand's tree code. */
static bool truth_expr(Checker *c, uint32_t n)
{
    n = strip_paren(c, n);
    if (cst_class(c, n))
        return false;
    if (ntag(c, n) == N_BINARY) {
        switch (npunct(c, n)) {
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR:
            return true;
        default:
            return false;
        }
    }
    return ntag(c, n) == N_UNARY && npunct(c, n) == P_BANG;
}

bool is_boolish(Checker *c, uint32_t n)
{
    return tkind(c, rvt(c, n)) == TY_BOOL || truth_expr(c, n);
}

/* The node's value when it is an integer constant: *neg, the magnitude. */
void cst_parts(Checker *c, uint32_t n, bool *neg, uint64_t *mag)
{
    TypeId t = rvt(c, n);
    uint64_t v = c->cv[n];
    *neg = ival_neg(c, t, v);
    *mag = *neg ? (uint64_t)0 - v : v;
}

/* The precision of n before its widening conversions when it is an unsigned
 * integer type, else 0 (tree_binary_nonnegative_warnv_p's zero-extension). */
static unsigned zext_bits(Checker *c, uint32_t n)
{
    TypeId t;
    n = strip_paren(c, n);
    if (ntag(c, n) == N_CAST) {
        uint32_t k[3], cnt = nkids(c, n, k, 3);
        if (cnt >= 1)
            n = strip_paren(c, k[cnt - 1]);
    }
    t = rvt(c, n);
    return is_int(c, t) && tkind(c, t) != TY_BOOL && !is_signed(c, t)
           ? int_bits(c, t) : 0;
}

/* tree_expr_nonnegative_p of a (signed) operand. */
bool nonneg(Checker *c, uint32_t n)
{
    TypeId t;
    uint32_t k[3], cnt;
    n = strip_paren(c, n);
    t = rvt(c, n);
    if (is_intcst(c, n))
        return !ival_neg(c, t, c->cv[n]);
    if (!is_int(c, t))
        return false;
    if (!is_signed(c, t))
        return true;
    switch (ntag(c, n)) {
    case N_CAST: {
        uint32_t in;
        TypeId it;
        cnt = nkids(c, n, k, 3);
        if (cnt < 1)
            return false;
        in = k[cnt - 1];
        it = rvt(c, in);
        if (!is_int(c, it))
            return false;
        if (!is_signed(c, it))
            return int_bits(c, it) < int_bits(c, t);
        return int_bits(c, it) <= int_bits(c, t) && nonneg(c, in);
    }
    case N_BINARY:
        if (nkids(c, n, k, 3) < 2)
            return false;
        switch (npunct(c, n)) {
        case P_AMP: return nonneg(c, k[0]) || nonneg(c, k[1]);
        case P_PIPE: case P_CARET: case P_SLASH:
            return nonneg(c, k[0]) && nonneg(c, k[1]);
        case P_PERCENT: case P_SHR: return nonneg(c, k[0]);
        case P_COMMA: return nonneg(c, k[1]);
        case P_STAR: /* signed overflow is undefined */
            return nonneg(c, k[0]) && nonneg(c, k[1]);
        case P_PLUS: { /* two zero-extended operands cannot overflow */
            unsigned x = zext_bits(c, k[0]), y = zext_bits(c, k[1]);
            return x && y && (x > y ? x : y) + 1 < int_bits(c, t);
        }
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR:
            return true;
        default: return false;
        }
    case N_ASSIGN:
        return npunct(c, n) == P_ASSIGN && nkids(c, n, k, 3) >= 2 &&
               nonneg(c, k[1]);
    case N_STMT_EXPR:
        return (cnt = stmt_expr_value(c, n)) != NO_NODE && nonneg(c, cnt);
    case N_UNARY:
        if (nkids(c, n, k, 3) < 1)
            return false;
        if (npunct(c, n) == P_BANG)
            return true;
        return npunct(c, n) == P_PLUS && nonneg(c, k[0]);
    case N_COND:
        cnt = nkids(c, n, k, 3);
        if (cnt == 3)
            return nonneg(c, k[1]) && nonneg(c, k[2]);
        return cnt == 2 && nonneg(c, k[0]) && nonneg(c, k[1]);
    default:
        return false;
    }
}

/* The declared width of the bit-field node n (0: not a bit-field). */
unsigned bf_width(Checker *c, uint32_t n)
{
    uint32_t d;
    TypeId rec;
    const Field *f;
    uint64_t off = 0;
    unsigned q = 0;
    n = strip_paren(c, n);
    if (!(c->ef[n] & EF_BITFIELD) || ntag(c, n) != N_MEMBER_EXPR ||
        (d = first_child(c, n)) == NO_NODE)
        return 0;
    rec = (c->nodes[n].flags & NF_ARROW) ? pointee(c, rvt(c, d)) : c->ty[d];
    f = find_field(c, rec, cnode_ident(c, n), &off, &q);
    return f && (f->flags & FF_BITFIELD) ? f->width : 0;
}

/* The type the operand n has in the sign comparison: an unsigned bit-field
 * narrower than int has been promoted to int. */
static TypeId cmp_ty(Checker *c, uint32_t n)
{
    if (c->ef[strip_paren(c, n)] & EF_BFPROMOTE)
        return TYPE_B(INT);
    return rvt(c, n);
}

/* The type of n as gcc names it in -Wsign-compare (a bit-field has a type
 * of its own width: 'signed char:4'). */
const char *bf_tstr(Checker *c, TypeId t, unsigned w)
{
    bool sg = is_signed(c, t);
    if (w && is_int(c, t) && tkind(c, t) != TY_BOOL && w < int_bits(c, t)) {
        const char *base = w <= 8 ? (sg ? "signed char" : "unsigned char")
                         : w <= 16 ? (sg ? "short int" : "short unsigned int")
                         : w <= 32 ? (sg ? "int" : "unsigned int")
                                   : (sg ? "long int" : "long unsigned int");
        StrBuf *sb = &c->esb[c->enext++ & 1];
        sb->len = 0;
        sb_printf(sb, "'%s:%u'", base, w);
        return sb_cstr(sb);
    }
    return type_q(TT, t);
}

const char *cmp_tstr(Checker *c, uint32_t n)
{
    return bf_tstr(c, rvt(c, n), bf_width(c, n));
}

/* warn_for_sign_compare */
static void sign_compare(Checker *c, SrcLoc loc, uint32_t a, uint32_t b,
                         int op, TypeId rt)
{
    TypeId ta = cmp_ty(c, a), tb = cmp_ty(c, b);
    bool sa, sb;
    uint32_t sop, uop;
    /* a complex integer type counts by its component type */
    if (is_complex(c, ta))
        ta = cplx_comp(c, ta);
    if (is_complex(c, tb))
        tb = cplx_comp(c, tb);
    if (is_complex(c, rt))
        rt = cplx_comp(c, rt);
    if (is_signed(c, rt) || !is_int(c, ta) || !is_int(c, tb))
        return;
    sa = is_signed(c, ta);
    sb = is_signed(c, tb);
    if (sa == sb)
        return;
    sop = sa ? a : b;
    uop = sa ? b : a;
    if (nonneg(c, sop))
        return;
    if ((op == P_EQEQ || op == P_NE) && is_intcst(c, uop)) {
        unsigned bits = int_bits(c, rt);
        uint64_t smax = bits >= 64 ? INT64_MAX : (UINT64_C(1) << (bits - 1)) - 1;
        if (c->cv[uop] <= smax)
            return;
    }
    cwarn(c, loc, "sign-compare", "comparison of integer expressions of "
          "different signedness: %s and %s", cmp_tstr(c, a), cmp_tstr(c, b));
}

/* c_common_get_narrower: strip the widening conversions of n.  *bf: a
 * bit-field (not narrowed here). */
uint32_t narrower(Checker *c, uint32_t n, TypeId *ty, unsigned *prec)
{
    *prec = 0;
    for (;;) {
        uint32_t s = strip_paren(c, n), k[3], cnt;
        if (is_intcst(c, s))
            break;
        if (c->ef[s] & EF_BITFIELD) {
            unsigned w = bf_width(c, s);
            TypeId bt = rvt(c, s);
            /* a signed bit-field narrows to its width; an unsigned one
             * narrower than int stays the int it was promoted to */
            if (w && is_int(c, bt) && w < int_bits(c, bt) &&
                !((c->ef[s] & EF_BFPROMOTE) && !is_signed(c, bt)))
                *prec = w;
            else if (c->ef[s] & EF_BFPROMOTE) {
                *ty = TYPE_B(INT);
                return s;
            }
            break;
        }
        if (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
            nkids(c, s, k, 3) == 2) {
            n = k[1];
            continue;
        }
        if (ntag(c, s) == N_CAST) {
            TypeId ot = mainv(c, c->ty[s]), it;
            cnt = nkids(c, s, k, 3);
            if (cnt >= 1 && is_int(c, ot)) {
                it = rvt(c, k[cnt - 1]);
                if (is_int(c, it) && int_bits(c, it) < int_bits(c, ot) &&
                    !(!is_signed(c, ot) && is_signed(c, it))) {
                    n = k[cnt - 1];
                    continue;
                }
            }
        }
        break;
    }
    n = strip_paren(c, n);
    *ty = rvt(c, n);
    return n;
}

/* The value v of a constant converted to a type of width bits. */
static uint64_t norm_val(uint64_t v, unsigned bits, bool sgn)
{
    if (bits < 64) {
        v &= (UINT64_C(1) << bits) - 1;
        if (sgn && (v >> (bits - 1)))
            v |= ~((UINT64_C(1) << bits) - 1);
    }
    return v;
}

static bool val_lt(uint64_t a, uint64_t b, bool sgn)
{
    return sgn ? (int64_t)a < (int64_t)b : a < b;
}

/* The ~E of a comparison operand x, E an unsigned value narrower than the
 * type x is compared in: *w is that type's width, *p the width of E before
 * its promotion (-Wsign-compare's "promoted bitwise complement"). */
static bool bitnot_operand(Checker *c, uint32_t x, unsigned *w, unsigned *p)
{
    uint32_t u = strip_paren(c, x), k[3];
    TypeId t;
    unsigned prec;
    if (u == NO_NODE)
        return false;
    *w = int_bits(c, rvt(c, u));
    if (ntag(c, u) == N_CAST) {
        unsigned n = nkids(c, u, k, 3);
        if (!n || !is_int(c, c->ty[u]))
            return false;
        *w = int_bits(c, c->ty[u]);
        u = strip_paren(c, k[n - 1]);
    }
    if (u == NO_NODE || ntag(c, u) != N_UNARY || npunct(c, u) != P_TILDE ||
        nkids(c, u, k, 2) < 1)
        return false;
    narrower(c, k[0], &t, &prec);
    if (!is_int(c, t) || is_signed(c, t) || tkind(c, t) == TY_BOOL)
        return false;
    *p = prec ? prec : int_bits(c, t);
    return *p < *w;
}

/* The "promoted bitwise complement of an unsigned value" warnings: ~E can
 * only equal values with ones above E's width. */
static bool bitnot_cmp(Checker *c, SrcLoc loc, uint32_t a, uint32_t b)
{
    unsigned w, p, i;
    for (i = 0; i < 2; i++) {
        uint32_t x = i ? b : a, y = strip_paren(c, i ? a : b);
        if (!bitnot_operand(c, x, &w, &p))
            continue;
        if (is_intcst(c, y)) {
            uint64_t m = w >= 64 ? ~UINT64_C(0) : (UINT64_C(1) << w) - 1;
            uint64_t ones = m & ~((UINT64_C(1) << p) - 1);
            uint64_t v = c->cv[y] & m;
            if ((v & ones) == ones)
                return false;
            cwarn(c, loc, "sign-compare", c->cv[y] == 0 ?
                  "promoted bitwise complement of an unsigned value is always "
                  "nonzero" : "comparison of promoted bitwise complement of an "
                  "unsigned value with constant");
            return true;
        } else {
            TypeId t;
            unsigned prec;
            narrower(c, y, &t, &prec);
            if (!is_int(c, t) || is_signed(c, t) || tkind(c, t) == TY_BOOL ||
                (prec ? prec : int_bits(c, t)) >= w)
                return false;
            cwarn(c, loc, "sign-compare", "comparison of promoted bitwise "
                  "complement of an unsigned value with unsigned");
            return true;
        }
    }
    return false;
}

/* shorten_compare's diagnostics.  True when gcc folds the comparison (and
 * so skips the sign-compare check). */
static bool type_limits(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId rt, SrcLoc loc)
{
    TypeId t0, t1, tx;
    unsigned pr0, pr1, prx;
    bool rts, warn;
    uint32_t p0 = narrower(c, a, &t0, &pr0), p1 = narrower(c, b, &t1, &pr1), y;
    int code = op;
    const char *why = NULL;
    if (!is_int(c, rt) || is_flt(c, rvt(c, a)) || is_flt(c, rvt(c, b)) ||
        int_bits(c, rt) > 64)
        return false;
    if (is_intcst(c, p0) && is_intcst(c, p1))
        return false;
    y = p1;
    tx = t0;
    prx = pr0;
    if (is_intcst(c, p0)) {
        y = p0;
        tx = t1;
        prx = pr1;
        code = swap_cmp(op);
    }
    if (!is_intcst(c, y))
        return false;
    warn = !inhibited(c, i, false) && !from_macro(c, c->nodes[i].tok);
    rts = is_signed(c, rt);
    /* constant on the left of a narrower operand: only the range message */
    if (!rts && c->cv[y] == 0 && (code == P_GE || code == P_LT) &&
        !(y == p0 && (prx ? prx : int_bits(c, tx)) < int_bits(c, rt))) {
        if (warn && tkind(c, tx) != TY_ENUM)
            cwarn(c, loc, "type-limits", code == P_GE
                  ? "comparison of unsigned expression in '>= 0' is always true"
                  : "comparison of unsigned expression in '< 0' is always false");
        return true;
    }
    if (!is_int(c, tx) || tkind(c, tx) == TY_BOOL || tkind(c, tx) == TY_ENUM ||
        (prx ? prx : int_bits(c, tx)) >= int_bits(c, rt))
        return false;
    {
        unsigned p = prx ? prx : int_bits(c, tx), w = int_bits(c, rt);
        bool xs = is_signed(c, tx), sgn = rts || xs;
        uint64_t mn, mx, v, yv = c->cv[y];
        bool min_gt, max_gt, min_lt, max_lt;
        int val = 0;
        yv = norm_val(yv, int_bits(c, rvt(c, y)), is_signed(c, rvt(c, y)));
        v = norm_val(yv, w, sgn);
        if (xs) {
            mn = norm_val(norm_val(UINT64_C(1) << (p - 1), p, true), w, true);
            mx = (UINT64_C(1) << (p - 1)) - 1;
        } else {
            mn = 0;
            mx = (UINT64_C(1) << p) - 1;
        }
        min_gt = val_lt(v, mn, sgn);
        max_gt = val_lt(v, mx, sgn);
        min_lt = val_lt(mn, v, sgn);
        max_lt = val_lt(mx, v, sgn);
        switch (code) {
        case P_NE:
            if (max_lt || min_gt) val = 1;
            break;
        case P_EQEQ:
            if (max_lt || min_gt) val = -1;
            break;
        case P_LT:
            if (max_lt) val = 1;
            if (!min_lt) val = -1;
            break;
        case P_GT:
            if (min_gt) val = 1;
            if (!max_gt) val = -1;
            break;
        case P_LE:
            if (!max_gt) val = 1;
            if (min_gt) val = -1;
            break;
        default:
            if (!min_lt) val = 1;
            if (max_lt) val = -1;
            break;
        }
        if (!rts && xs && code != P_EQEQ && code != P_NE)
            val = 0;
        if (!val)
            return false;
        why = val > 0 ? "comparison is always true due to limited range of "
                        "data type"
                      : "comparison is always false due to limited range of "
                        "data type";
    }
    if (warn)
        cwarn(c, loc, "type-limits", "%s", why);
    return true;
}

/* maybe_warn_bool_compare */
static bool bool_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                         SrcLoc loc)
{
    bool ba = is_boolish(c, a), bb = is_boolish(c, b), neg;
    uint32_t k;
    uint64_t mag;
    int r[2], x;
    if (ba == bb || inhibited(c, i, false))
        return false;
    k = ba ? b : a;
    if (!is_intcst(c, k))
        return false;
    cst_parts(c, k, &neg, &mag);
    for (x = 0; x < 2; x++) {
        int cmp;    /* x <=> constant */
        if (neg)
            cmp = 1;
        else
            cmp = (uint64_t)x < mag ? -1 : (uint64_t)x > mag;
        if (!ba)
            cmp = -cmp;     /* the constant is the left operand */
        switch (op) {
        case P_EQEQ: r[x] = cmp == 0; break;
        case P_NE: r[x] = cmp != 0; break;
        case P_LT: r[x] = cmp < 0; break;
        case P_GT: r[x] = cmp > 0; break;
        case P_LE: r[x] = cmp <= 0; break;
        default: r[x] = cmp >= 0; break;
        }
    }
    if (r[0] != r[1])
        return false;
    cwarn(c, loc, "bool-compare", "comparison of constant '%s' with boolean "
          "expression is always %s", vstr(c, rvt(c, k), c->cv[k]),
          r[0] ? "true" : "false");
    return true;
}

/* ---- -Wtautological-compare ---- */

/* operand_equal_p (x, y, 0), for the expressions this checker can see. */
bool opeq(Checker *c, uint32_t x, uint32_t y)
{
    uint32_t kx[3], ky[3], nx, ny;
    x = strip_paren(c, x);
    y = strip_paren(c, y);
    if (ntag(c, x) == N_STMT_EXPR && stmt_expr_single(c, x) != NO_NODE)
        x = strip_paren(c, stmt_expr_single(c, x));
    if (ntag(c, y) == N_STMT_EXPR && stmt_expr_single(c, y) != NO_NODE)
        y = strip_paren(c, stmt_expr_single(c, y));
    if (ntag(c, x) != ntag(c, y) ||
        (ntag(c, x) != N_CALL && (c->ef[x] | c->ef[y]) & EF_SIDE))
        return false;
    if (tquals(c, c->ty[x]) & TQ_VOLATILE)
        return false;
    if (is_intcst(c, x) || is_intcst(c, y))
        return is_intcst(c, x) && is_intcst(c, y) &&
               mainv(c, c->ty[x]) == mainv(c, c->ty[y]) && c->cv[x] == c->cv[y];
    nx = nkids(c, x, kx, 3);
    ny = nkids(c, y, ky, 3);
    switch (ntag(c, x)) {
    case N_IDENT:
        return cnode_ident(c, x) == cnode_ident(c, y) &&
               lookup_ord(c, cnode_ident(c, x)) != SYM_NONE;
    case N_MEMBER_EXPR:
        return cnode_ident(c, x) == cnode_ident(c, y) &&
               (c->nodes[x].flags & NF_ARROW) == (c->nodes[y].flags & NF_ARROW) &&
               nx >= 1 && ny >= 1 && opeq(c, kx[0], ky[0]);
    case N_INDEX:
        return nx == 2 && ny == 2 && opeq(c, kx[0], ky[0]) &&
               opeq(c, kx[1], ky[1]);
    case N_UNARY:
        switch (npunct(c, x)) {
        case P_STAR: case P_AMP: case P_MINUS: case P_PLUS: case P_TILDE:
        case P_BANG:
            return npunct(c, x) == npunct(c, y) && nx >= 1 && ny >= 1 &&
                   opeq(c, kx[0], ky[0]);
        default:
            return false;
        }
    case N_BINARY: {
        int op = npunct(c, x);
        if (op != npunct(c, y) || op == P_COMMA || nx < 2 || ny < 2)
            return false;
        if (opeq(c, kx[0], ky[0]) && opeq(c, kx[1], ky[1]))
            return true;
        if (op == P_PLUS || op == P_STAR || op == P_AMP || op == P_PIPE ||
            op == P_CARET || op == P_EQEQ || op == P_NE)
            return opeq(c, kx[0], ky[1]) && opeq(c, kx[1], ky[0]);
        return false;
    }
    case N_CAST:
        return nx >= 1 && ny >= 1 && mainv(c, c->ty[x]) == mainv(c, c->ty[y]) &&
               opeq(c, kx[nx - 1], ky[ny - 1]);
    case N_CALL: { /* calls of a const or pure function with equal arguments */
        uint32_t ref, j;
        if (nx < 1 || nx != ny || ntag(c, strip_paren(c, kx[0])) != N_IDENT ||
            ntag(c, strip_paren(c, ky[0])) != N_IDENT || nx > 3)
            return false;
        ref = lookup_ord(c, cnode_ident(c, strip_paren(c, kx[0])));
        if (ref == SYM_NONE || ref != lookup_ord(c, cnode_ident(c, strip_paren(c, ky[0]))) ||
            !(csym(c, ref)->flags & (CSF_PURE | CSF_CONSTFN)))
            return false;
        for (j = 1; j < nx; j++)
            if (!opeq(c, kx[j], ky[j]))
                return false;
        return true;
    }
    default:
        return false;
    }
}

/* The first token of loc's line: where gcc reports a restrict clash it has no
 * better location for. */
static SrcLoc line_start_loc(Checker *c, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    uint32_t line, col, len, j = 0;
    const char *s;
    if (!f)
        return loc;
    srcmgr_linecol(f, loc, &line, &col);
    s = srcmgr_line_text(f, line, &len);
    while (j < len && (s[j] == 32 || s[j] == 9))
        j++;
    return j < col ? srcmgr_loc_of(f, line, j + 1) : loc;
}

/* The built-ins whose call with a zero size is not diagnosed. */
bool zero_size_ok(const char *name)
{
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    return !strcmp(name, "memcpy") || !strcmp(name, "strncpy") ||
           !strcmp(name, "strncat");
}

/* The address an argument stands for: casts, `&E[0]` and `E + 0` look through. */
static uint32_t restrict_base(Checker *c, uint32_t a)
{
    for (;;) {
        uint32_t k[3], n, in, ik[3];
        a = strip_paren(c, a);
        n = nkids(c, a, k, 3);
        if (ntag(c, a) == N_CAST && n >= 1 && is_ptr(c, c->ty[a])) {
            a = k[n - 1];
        } else if (ntag(c, a) == N_UNARY && npunct(c, a) == P_AMP && n == 1 &&
                   ntag(c, in = strip_paren(c, k[0])) == N_INDEX &&
                   nkids(c, in, ik, 3) == 2 && is_intcst(c, ik[1]) &&
                   c->cv[ik[1]] == 0 && is_ptr(c, c->ty[strip_paren(c, ik[0])])) {
            a = ik[0];
        } else if (ntag(c, a) == N_BINARY && npunct(c, a) == P_PLUS && n == 2 &&
                   is_ptr(c, rvt(c, k[0])) && is_intcst(c, k[1]) &&
                   c->cv[k[1]] == 0) {
            a = k[0];
        } else {
            return a;
        }
    }
}

/* c-family warn_for_restrict: two arguments of restrict-qualified parameters
 * that are the same address.  A built-in copy of zero bytes is fine. */
bool zero_size_ok(const char *name);
void check_restrict(Checker *c, const uint32_t *kv, uint32_t nk,
                           uint32_t parms, uint32_t nparm, SrcLoc loc,
                           bool builtin, bool is_bt)
{
    uint32_t i, j;
    uint64_t seen = 0;   /* arguments already named: gcc reports each once */
    /* each restrict parameter against every other argument (the variadic ones
     * too); a pair of restrict ones once, the lower first; one warning per
     * parameter lists all the arguments it aliases */
    for (i = 0; i < nparm && i + 1 < nk && i < 64; i++) {
        char list[160];
        size_t len = 0;
        unsigned cnt = 0;
        SrcLoc l;
        uint32_t a = kv[i + 1], first;
        if (!cparm_restrict(c, parms, i) || (seen >> i & 1))
            continue;
        list[0] = 0;
        for (j = 0; j + 1 < nk && j < 64; j++) {
            if (j == i || (seen >> j & 1) ||
                (j < i && j < nparm && cparm_restrict(c, parms, j)))
                continue;
            if (is_bt && j >= nparm)   /* the middle end handles those */
                continue;
            if (node_err(c, a) || node_err(c, kv[j + 1]) ||
                !is_ptr(c, rvt(c, a)) || !is_ptr(c, rvt(c, kv[j + 1])))
                continue;
            if (!opeq(c, restrict_base(c, a), restrict_base(c, kv[j + 1])))
                continue;
            if (builtin && nparm == 3 && nk > 3 && is_intcst(c, kv[3]) &&
                c->cv[kv[3]] == 0)
                continue;
            len += (size_t)snprintf(list + len, sizeof list - len, "%s%u",
                                    cnt ? ", " : "", j + 1);
            cnt++;
            seen |= (uint64_t)1 << j;
        }
        if (!cnt)
            continue;
        first = restrict_base(c, a);
        l = ntag(c, first) != N_IDENT || is_array(c, c->ty[first])
                ? expr_loc(c, a) : line_start_loc(c, loc);
        cwarn(c, l, "restrict", "passing argument %u to 'restrict'-"
              "qualified parameter aliases with argument%s %s", i + 1,
              cnt > 1 ? "s" : "", list);
    }
}

/* warn_tautological_cmp */
void tauto_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b), bit, cst, n;
    TypeId t;
    if (!is_cmp_op(op) || from_macro(c, c->nodes[i].tok))
        return;
    if (op == P_EQEQ || op == P_NE) {
        bit = NO_NODE;
        cst = NO_NODE;
        if (ntag(c, sa) == N_BINARY && (npunct(c, sa) == P_AMP ||
                                        npunct(c, sa) == P_PIPE) &&
            is_intcst(c, sb)) {
            bit = sa;
            cst = sb;
        } else if (ntag(c, sb) == N_BINARY && (npunct(c, sb) == P_AMP ||
                                               npunct(c, sb) == P_PIPE) &&
                   is_intcst(c, sa)) {
            bit = sb;
            cst = sa;
        }
        if (bit != NO_NODE) {
            uint32_t k[3], kc = NO_NODE;
            if (nkids(c, bit, k, 3) == 2) {
                if (is_intcst(c, k[0]))
                    kc = k[0];
                else if (is_intcst(c, k[1]))
                    kc = k[1];
            }
            if (kc != NO_NODE) {
                TypeId bt = rvt(c, kc);
                unsigned bits = int_bits(c, bt);
                uint64_t cv = norm_val(c->cv[cst], bits, is_signed(c, bt));
                uint64_t bv = norm_val(c->cv[kc], bits, is_signed(c, bt));
                uint64_t res = norm_val(npunct(c, bit) == P_AMP ? cv & bv : cv | bv,
                                        bits, is_signed(c, bt));
                if (res != cv && bits <= 64)
                    cwarn(c, loc, "tautological-compare", op == P_EQEQ
                          ? "bitwise comparison always evaluates to false"
                          : "bitwise comparison always evaluates to true");
            }
        }
    }
    if (cst_class(c, sa) || cst_class(c, sb))
        return;
    if (ntag(c, sa) == N_CAST || ntag(c, sb) == N_CAST)
        return;
    t = c->ty[sa];
    if (is_flt(c, t) || is_complex(c, t) || is_array(c, t) ||
        is_record(c, t) || tkind(c, t) == TY_VECTOR)
        return;
    for (n = sa; ntag(c, n) == N_INDEX || ntag(c, n) == N_MEMBER_EXPR;) {
        uint32_t k[3];      /* a constant index anywhere in the access chain */
        if (nkids(c, n, k, 3) < 1)
            break;
        if (ntag(c, n) == N_INDEX && nkids(c, n, k, 3) == 2 &&
            is_array(c, c->ty[strip_paren(c, k[0])]) && is_intcst(c, k[1]))
            return;
        n = strip_paren(c, k[0]);
    }
    if (!opeq(c, sa, sb))
        return;
    cwarn(c, loc, "tautological-compare", "self-comparison always evaluates "
          "to %s", (op == P_EQEQ || op == P_LE || op == P_GE) ? "true"
                                                              : "false");
}

/* ---- -Wparentheses ---- */

/* The operator of an unparenthesized binary operand (gcc's original_code). */
static int orig_op(Checker *c, uint32_t n)
{
    int op;
    if (ntag(c, n) != N_BINARY)
        return 0;
    op = npunct(c, n);
    return op == P_COMMA ? 0 : op;
}


/* `!a & b`, `!a | b`: the right operand is no truth value (gcc's original
 * code is not a comparison, && , || or !). */
static bool lognot_bitop(Checker *c, uint32_t a, uint32_t b, int op, SrcLoc la)
{
    uint32_t k[3], r;
    int rop;
    if (ntag(c, a) != N_UNARY || npunct(c, a) != P_BANG || nkids(c, a, k, 3) < 1)
        return false;
    r = strip_paren(c, b);
    rop = ntag(c, r) == N_BINARY ? npunct(c, r) : 0;
    if (is_cmp_op(rop) || rop == P_ANDAND || rop == P_OROR ||
        (ntag(c, r) == N_UNARY && npunct(c, r) == P_BANG))
        return false;
    PW(la, "suggest parentheses around operand of '!' or "
       "change '%s' to '%s' or '!' to '~'", op == P_AMP ? "&" : "|",
       op == P_AMP ? "&&" : "||");
    return true;
}

/* warn_about_parentheses */
void parens_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    int ca = orig_op(c, a), cb = orig_op(c, b);
    SrcLoc la = cst_class(c, a) ? loc : cnode_loc(c, a);
    SrcLoc lb = cst_class(c, b) ? loc : cnode_loc(c, b);
    switch (op) {
    case P_SHL: case P_SHR: {
        const char *s = op == P_SHL ? "<<" : ">>";
        if (ca == P_PLUS)
            PW(la, "suggest parentheses around '+' inside '%s'", s);
        else if (cb == P_PLUS)
            PW(lb, "suggest parentheses around '+' inside '%s'", s);
        else if (ca == P_MINUS)
            PW(la, "suggest parentheses around '-' inside '%s'", s);
        else if (cb == P_MINUS)
            PW(lb, "suggest parentheses around '-' inside '%s'", s);
        return;
    }
    case P_OROR:
        if (ca == P_ANDAND)
            PW(la, "suggest parentheses around '&&' within '||'");
        else if (cb == P_ANDAND)
            PW(lb, "suggest parentheses around '&&' within '||'");
        return;
    case P_PIPE:
        if (lognot_bitop(c, a, b, op, la))
            return;
        if (ca == P_AMP || ca == P_CARET || ca == P_PLUS || ca == P_MINUS)
            PW(la, "suggest parentheses around arithmetic in operand of '|'");
        else if (cb == P_AMP || cb == P_CARET || cb == P_PLUS || cb == P_MINUS)
            PW(lb, "suggest parentheses around arithmetic in operand of '|'");
        else if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '|'");
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '|'");
        return;
    case P_CARET:
        if (ca == P_AMP || ca == P_PLUS || ca == P_MINUS)
            PW(la, "suggest parentheses around arithmetic in operand of '^'");
        else if (cb == P_AMP || cb == P_PLUS || cb == P_MINUS)
            PW(lb, "suggest parentheses around arithmetic in operand of '^'");
        else if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '^'");
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '^'");
        return;
    case P_AMP:
        if (lognot_bitop(c, a, b, op, la))
            return;
        if (ca == P_PLUS)
            PW(la, "suggest parentheses around '+' in operand of '&'");
        else if (cb == P_PLUS)
            PW(lb, "suggest parentheses around '+' in operand of '&'");
        else if (ca == P_MINUS)
            PW(la, "suggest parentheses around '-' in operand of '&'");
        else if (cb == P_MINUS)
            PW(lb, "suggest parentheses around '-' in operand of '&'");
        else if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '&'");
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '&'");
        return;
    case P_EQEQ: case P_NE:
        if (is_cmp_op(ca))
            PW(la, "suggest parentheses around comparison in operand of '%s'",
               cmp_str(op));
        else if (is_cmp_op(cb))
            PW(lb, "suggest parentheses around comparison in operand of '%s'",
               cmp_str(op));
        return;
    case P_LT: case P_GT: case P_LE: case P_GE:
        if (is_cmp_op(ca))
            PW(la, "comparisons like 'X<=Y<=Z' do not have their mathematical "
                   "meaning");
        else if (is_cmp_op(cb))
            PW(lb, "comparisons like 'X<=Y<=Z' do not have their mathematical "
                   "meaning");
        return;
    default:
        return;
    }
}

/* A bit operation (| ^ & ~, int casts) whose leaves are all boolean. */
static bool boolish_bits(Checker *c, uint32_t n)
{
    uint32_t k[3];
    unsigned cnt;
    n = strip_paren(c, n);
    if (is_boolish(c, n))
        return true;
    cnt = nkids(c, n, k, 3);
    if (ntag(c, n) == N_CAST && tkind(c, mainv(c, c->ty[n])) == TY_INT && cnt)
        return boolish_bits(c, k[cnt - 1]);
    if (ntag(c, n) == N_UNARY && npunct(c, n) == P_TILDE && cnt == 1)
        return boolish_bits(c, k[0]);
    if (ntag(c, n) == N_BINARY && cnt == 2 &&
        (npunct(c, n) == P_PIPE || npunct(c, n) == P_CARET ||
         npunct(c, n) == P_AMP))
        return boolish_bits(c, k[0]) && boolish_bits(c, k[1]);
    return false;
}

/* warn_logical_not_parentheses (the caller of it in parser_build_binary_op
 * included). */
void lognot_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    uint32_t k[3], x, y;
    Diagnostic *d;
    if (!is_cmp_op(op) || ntag(c, a) != N_UNARY || npunct(c, a) != P_BANG ||
        (ntag(c, b) == N_UNARY && npunct(c, b) == P_BANG) ||
        nkids(c, a, k, 3) < 1)
        return;
    x = strip_paren(c, k[0]);
    if (ntag(c, x) == N_UNARY && npunct(c, x) == P_BANG && !cst_class(c, x))
        return;
    if (ntag(c, x) == N_BINARY && npunct(c, x) == P_EQEQ &&
        nkids(c, x, k, 3) == 2 && is_intcst(c, k[1]) && c->cv[k[1]] == 0)
        return;
    y = x;
    if (tkind(c, rvt(c, y)) == TY_INT) {
        while (ntag(c, y) == N_CAST && tkind(c, mainv(c, c->ty[y])) == TY_INT &&
               nkids(c, y, k, 3) >= 1)
            y = strip_paren(c, k[nkids(c, y, k, 3) - 1]);
    }
    if (tkind(c, rvt(c, y)) == TY_BOOL)
        return;
    if (tkind(c, rvt(c, b)) == TY_BOOL || truth_expr(c, b) ||
        (is_intcst(c, b) && c->cv[b] == 0 && (op == P_EQEQ || op == P_NE)) ||
        boolish_bits(c, b))
        return;
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "logical-not-parentheses",
                "logical not is only applied to the left hand side of "
                "comparison");
    cnote(c, d, first_loc(c, a), "add parentheses around left hand side "
          "expression to silence this warning");
}

/* ---- -Warray-compare ---- */

void array_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    Diagnostic *d;
    SrcLoc loc = cnode_loc(c, i);
    if (!is_cmp_op(op) || ntag(c, a) != N_IDENT || ntag(c, b) != N_IDENT ||
        !is_array(c, c->ty[a]) || !is_array(c, c->ty[b]))
        return;
    d = cwarn_d(c, DL_WARNING, loc, "array-compare", "comparison between two "
                "arrays");
    cnote(c, d, loc, "use '&%s[0] %s &%s[0]' to compare the addresses",
          cident(c, cnode_ident(c, a)), cmp_str(op),
          cident(c, cnode_ident(c, b)));
}

/* ---- -Waddress (the address of an object is never null) ---- */

typedef struct AddrInfo {
    const char *name;
    SrcLoc dloc;
    bool direct;         /* the address of a declared object, itself */
    char buf[96];        /* storage for a '*p' name */
    bool label;          /* &&label */
    uint32_t ref;        /* its symbol, SYM_NONE for a member */
} AddrInfo;

/* decl_with_nonnull_addr_p: a weak declaration may be null, a weak definition
 * (not a weakref) cannot be. */
static bool weak_maybe_null(const CSym *s)
{
    return (s->flags & CSF_WEAK) &&
           ((s->flags & CSF_WEAKREF) || (s->flags & CSF_DECL_EXTERNAL));
}

/* '*p' for an identifier p that points to an array. */
static bool deref_name(Checker *c, uint32_t id, AddrInfo *ai)
{
    uint32_t ref;
    id = strip_paren(c, id);
    if (ntag(c, id) != N_IDENT || !is_ptr(c, rvt(c, id)) ||
        !is_array(c, pointee(c, rvt(c, id))))
        return false;
    ref = lookup_ord(c, cnode_ident(c, id));
    if (ref == SYM_NONE)
        return false;
    snprintf(ai->buf, sizeof ai->buf, "*%s", cident(c, csym(c, ref)->name));
    ai->name = ai->buf;
    ai->dloc = csym(c, ref)->loc;
    return true;
}

/* The object whose address the pointer-valued expression n is, if it is one
 * gcc's decl_with_nonnull_addr_p accepts. */
static bool addr_target(Checker *c, uint32_t n, AddrInfo *ai)
{
    uint32_t k[3], e;
    bool deref = false;
    n = strip_paren(c, n);
    memset(ai, 0, sizeof *ai);
    ai->ref = SYM_NONE;
    if (ntag(c, n) == N_ADDR_LABEL) { /* a label is never null */
        ai->name = cident(c, cnode_ident(c, n));
        ai->direct = true;
        ai->label = true;
        return true;
    }
    if (ntag(c, n) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, n));
        CSym *s;
        if (ref == SYM_NONE) {
            const char *nm = cident(c, cnode_ident(c, n));
            if (!is_func(c, c->ty[n]) || strncmp(nm, "__builtin_", 10))
                return false;
            ai->name = nm; /* a built-in function has no symbol */
            ai->direct = true;
            return true;
        }
        s = csym(c, ref);
        if (!((s->kind == CS_FUNC) ||
              (s->kind == CS_OBJ && is_array(c, c->ty[n]))) ||
            weak_maybe_null(s))
            return false;
        ai->name = cident(c, s->name);
        ai->dloc = s->loc;
        ai->direct = true;
        ai->ref = ref;
        return true;
    }
    if ((ntag(c, n) == N_MEMBER_EXPR || ntag(c, n) == N_INDEX) &&
        is_array(c, c->ty[n])) {
        e = n; /* an array decaying to the address of its element */
    } else {
        if (ntag(c, n) != N_UNARY || npunct(c, n) != P_AMP ||
            nkids(c, n, k, 3) < 1)
            return false;
        e = strip_paren(c, k[0]);
        if (ntag(c, e) == N_UNARY && (tckw(c, c->nodes[e].tok) == CK_REAL ||
                                      tckw(c, c->nodes[e].tok) == CK_IMAG) &&
            nkids(c, e, k, 3) >= 1) {
            /* &__real__ x: never null; named '__real__ x' */
            uint32_t in = strip_paren(c, k[0]);
            StrBuf sb = {0};
            if (ntag(c, in) == N_CALL ||
                (ntag(c, in) == N_UNARY && npunct(c, in) != P_STAR))
                return false;
            sb_puts(&sb, tckw(c, c->nodes[e].tok) == CK_REAL ? "__real__ "
                                                              : "__imag__ ");
            pexpr(c, &sb, in, PR_UNARY);
            snprintf(ai->buf, sizeof ai->buf, "%s", sb_cstr(&sb));
            sb_free(&sb);
            {   /* gcc prints a call as f() */
                char *sp = strstr(ai->buf, " ()");
                if (sp)
                    memmove(sp, sp + 1, strlen(sp));
            }
            ai->name = ai->buf;
            return true;
        }
    }
    for (;;) {
        if (ntag(c, e) == N_MEMBER_EXPR) {
            uint32_t d = first_child(c, e);
            bool arrow = (c->nodes[e].flags & NF_ARROW) != 0;
            TypeId rec;
            const Field *f;
            uint64_t off = 0;
            unsigned q = 0;
            if (d == NO_NODE)
                return false;
            rec = arrow ? pointee(c, rvt(c, d)) : c->ty[d];
            f = find_field(c, rec, cnode_ident(c, e), &off, &q);
            if (!f || !f->name)
                return false;
            ai->name = cident(c, f->name);
            ai->dloc = f->loc;
            return true;
        }
        if (ntag(c, e) == N_INDEX) {
            uint32_t base;
            if (nkids(c, e, k, 3) < 1)
                return false;
            base = strip_paren(c, k[0]);
            if (!is_array(c, c->ty[base]))
                return deref_name(c, base, ai);
            e = base;
            deref = true;
            continue;
        }
        break;
    }
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_STAR && is_array(c, c->ty[e]) &&
        nkids(c, e, k, 3) >= 1)
        return deref_name(c, k[0], ai);
    if (ntag(c, e) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, e));
        CSym *s;
        if (ref == SYM_NONE)
            return false;
        s = csym(c, ref);
        if ((s->kind != CS_OBJ && s->kind != CS_FUNC) || weak_maybe_null(s))
            return false;
        ai->name = cident(c, s->name);
        ai->dloc = s->loc;
        ai->direct = !deref;
        ai->ref = ref;
        return true;
    }
    return false;
}

/* x is 'ptr +- int' or '&ptr[int]': the pointer, the integer and the sign. */
static bool plus_split(Checker *c, uint32_t x, uint32_t *pn, uint32_t *in,
                       int *op)
{
    uint32_t k[3];
    x = strip_paren(c, x);
    if (ntag(c, x) == N_UNARY && npunct(c, x) == P_AMP &&
        nkids(c, x, k, 3) >= 1) {
        uint32_t e = strip_paren(c, k[0]), ek[3];
        if (ntag(c, e) != N_INDEX || nkids(c, e, ek, 3) != 2)
            return false;
        if (is_ptr(c, rvt(c, ek[0])) && !is_array(c, c->ty[strip_paren(c, ek[0])]) &&
            is_int(c, rvt(c, ek[1]))) {
            *pn = ek[0];
            *in = ek[1];
            *op = P_PLUS;
            return true;
        }
        return false;
    }
    if (ntag(c, x) == N_INDEX && is_array(c, c->ty[x]) && nkids(c, x, k, 3) == 2 &&
        is_ptr(c, rvt(c, k[0])) && !is_array(c, c->ty[strip_paren(c, k[0])]) &&
        is_int(c, rvt(c, k[1]))) {
        *pn = k[0];
        *in = k[1];
        *op = P_PLUS;
        return true;
    }
    if (ntag(c, x) != N_BINARY || nkids(c, x, k, 3) != 2)
        return false;
    *op = npunct(c, x);
    if (*op != P_PLUS && *op != P_MINUS)
        return false;
    if (is_ptr(c, rvt(c, k[0])) && is_int(c, rvt(c, k[1]))) {
        *pn = k[0];
        *in = k[1];
        return true;
    }
    if (*op == P_PLUS && is_ptr(c, rvt(c, k[1])) && is_int(c, rvt(c, k[0]))) {
        *pn = k[1];
        *in = k[0];
        return true;
    }
    return false;
}

/* maybe_warn_for_null_address's POINTER_PLUS_EXPR case: x is 'ptr +- N'.
 * Prints the sum as gcc's folded tree: base + (sizetype)((unsigned long)N * size).
 * Returns true when x is such a sum (warned or not). */
static bool ptr_plus_warn(Checker *c, SrcLoc loc, uint32_t x, int code)
{
    uint32_t pn, in, pn2, in2;
    int op, op2;
    int64_t k = 0;
    uint32_t var = NO_NODE;
    bool vneg = false;
    uint64_t sz;
    TypeId pt;
    StrBuf sb = {0};
    if (!plus_split(c, x, &pn, &in, &op))
        return false;
    pt = rvt(c, x);
    if (!is_ptr(c, pt) || is_void(c, pointee(c, pt)))
        return true;
    sz = elem_size(c, is_array(c, c->ty[strip_paren(c, pn)]) ? pt : rvt(c, pn));
    for (;;) {
        uint32_t ik[3];
        /* gcc distributes a constant: p + (i + 1) is p + i + 1 */
        in = strip_paren(c, in);
        if (!prints_value(c, in) && ntag(c, in) == N_BINARY &&
            (npunct(c, in) == P_PLUS || npunct(c, in) == P_MINUS) &&
            nkids(c, in, ik, 3) == 2 && prints_value(c, ik[1]) &&
            !prints_value(c, ik[0])) {
            int64_t v = (int64_t)cexpr_trunc(c, rvt(c, ik[1]), c->cv[ik[1]]);
            k += (op == P_MINUS) == (npunct(c, in) == P_MINUS) ? v : -v;
            in = ik[0];
        }
        if (prints_value(c, in)) {
            int64_t v = (int64_t)cexpr_trunc(c, rvt(c, in), c->cv[in]);
            k += op == P_MINUS ? -v : v;
        } else {
            if (var != NO_NODE)
                return true;
            var = in;
            vneg = op == P_MINUS;
        }
        pn = strip_paren(c, pn);
        if (!is_array(c, c->ty[pn]) && plus_split(c, pn, &pn2, &in2, &op2)) {
            pn = pn2;
            in = in2;
            op = op2;
            continue;
        }
        break;
    }
    if (ntag(c, pn) == N_UNARY && npunct(c, pn) == P_AMP) { /* &*p is p */
        uint32_t uk[3], inner;
        if (nkids(c, pn, uk, 3) >= 1) {
            inner = strip_paren(c, uk[0]);
            if (ntag(c, inner) == N_UNARY && npunct(c, inner) == P_STAR &&
                nkids(c, inner, uk, 3) >= 1 && !is_array(c, c->ty[inner]))
                pn = strip_paren(c, uk[0]);
        }
    }
    if (var == NO_NODE && k == 0) /* folds to the pointer itself */
        return true;
    {   /* a cast of an array: (char *)&a */
        uint32_t ck[3];
        if (ntag(c, pn) == N_CAST && is_ptr(c, rvt(c, pn)) &&
            nkids(c, pn, ck, 3) >= 1 &&
            is_array(c, c->ty[strip_paren(c, ck[nkids(c, pn, ck, 3) - 1])])) {
            pt = rvt(c, pn);
            pn = strip_paren(c, ck[nkids(c, pn, ck, 3) - 1]);
        }
    }
    if (is_array(c, c->ty[pn])) {
        sb_putc(&sb, '(');
        type_print(TT, &sb, pt);
        sb_putc(&sb, ')');
        if (ntag(c, pn) == N_UNARY && npunct(c, pn) == P_STAR) {
            uint32_t dk[3];
            if (nkids(c, pn, dk, 3) >= 1)
                pexpr(c, &sb, strip_paren(c, dk[0]), PR_UNARY);
        } else {
            sb_putc(&sb, '&');
            pexpr(c, &sb, pn, PR_UNARY);
        }
    } else {
        pexpr(c, &sb, pn, PR_ADD);
    }
    if (var == NO_NODE) {
        sb_printf(&sb, " + %lld", (long long)(k * (int64_t)sz));
    } else {
        TypeId vt = rvt(c, var);
        bool ul = !is_signed(c, vt) && type_size(TT, vt, NULL) == 8;
        StrBuf v = {0};
        if (sz == 1) {
            sb_puts(&v, "(sizetype)");
            pexpr(c, &v, var, PR_UNARY);
        } else {
            if (k) {
                sb_puts(&v, "((sizetype)");
                pexpr(c, &v, var, PR_UNARY);
                sb_printf(&v, " + %lld) * %llu", (long long)k,
                          (unsigned long long)sz);
            } else {
                sb_puts(&v, "(sizetype)(");
                if (!ul)
                    sb_puts(&v, "(long unsigned int)");
                pexpr(c, &v, var, PR_UNARY);
                sb_printf(&v, " * %llu)", (unsigned long long)sz);
            }
        }
        sb_printf(&sb, " + %s%s", vneg ? "-" : "", sb_cstr(&v));
        if (sz == 1 && k)
            sb_printf(&sb, " + %lld", (long long)k);
        sb_free(&v);
    }
    cwarn(c, loc, "address", "the comparison will always evaluate as '%s' for "
          "the pointer operand in '%s' must not be NULL",
          code == P_EQEQ ? "false" : "true", sb_cstr(&sb));
    sb_free(&sb);
    return true;
}

static void null_addr_msg(Checker *c, SrcLoc loc, const AddrInfo *ai, int code)
{
    Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "address", "the comparison will "
                            "always evaluate as '%s' for the address of '%s' "
                            "will never be NULL",
                            code == P_EQEQ ? "false" : "true", ai->name);
    if (ai->dloc)
        cnote(c, d, ai->dloc, "'%s' declared here", ai->name);
}

/* maybe_warn_for_null_address */
static bool null_addr_warn(Checker *c, SrcLoc loc, uint32_t x, int code,
                           uint32_t cmp)
{
    AddrInfo ai;
    uint32_t k[3];
    x = strip_paren(c, x);
    if (ntag(c, x) == N_CAST) {
        if (is_int(c, mainv(c, c->ty[x])) || nkids(c, x, k, 3) < 1)
            return false;
        x = strip_paren(c, k[nkids(c, x, k, 3) - 1]);
    }
    if (ntag(c, x) == N_BINARY && npunct(c, x) == P_PLUS &&
        nkids(c, x, k, 3) == 2 && is_ptr(c, rvt(c, k[0])) &&
        prints_value(c, k[1]) && c->cv[k[1]] == 0) /* folds to the pointer */
        x = strip_paren(c, k[0]);
    /* judged by the comparison: G (*p, i) != 0 and 'malloc != 0' (malloc a
     * macro) are user code, F (p) with the comparison inside F is not */
    if (inhibited(c, x, false) || from_macro(c, c->nodes[cmp].tok))
        return false;
    if (ptr_plus_warn(c, loc, x, code))
        return true;
    if (!addr_target(c, x, &ai))
        return false;
    null_addr_msg(c, loc, &ai, code);
    return true;
}

/* c_common_truthvalue_conversion's -Waddress checks of the truth-value n
 * (the diagnostic location loc). */
/* Has the truth-value warning for label id already been given in this
 * function?  Records it if not. */
static bool label_warned(Checker *c, uint32_t id)
{
    uint32_t j;
    if (c->lbl_fn != c->func_sym) {
        c->lbl_fn = c->func_sym;
        c->lbl_n = 0;
    }
    for (j = 0; j < c->lbl_n; j++)
        if (c->lbl_ids[j] == id)
            return true;
    if (c->lbl_n < 64)
        c->lbl_ids[c->lbl_n++] = id;
    return false;
}

/* c_common_truthvalue_conversion's -Wint-in-bool-context: the expression n
 * is used as a truth value. */
bool no_int_bool;

static void int_bool_warn(Checker *c, uint32_t n)
{
    uint32_t s = n, k[3], cnt;
    TypeId t;
    if (no_int_bool || !diag_enabled(c->diag, "int-in-bool-context") ||
        node_err(c, n))
        return;
    for (;;) {      /* operations that keep zero-ness */
        s = strip_paren(c, s);
        cnt = nkids(c, s, k, 3);
        if (ntag(c, s) == N_UNARY && (npunct(c, s) == P_PLUS ||
                                      npunct(c, s) == P_MINUS) && cnt == 1) {
            s = k[0];
        } else if (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
                   cnt == 2) {
            s = k[1];
        } else if (ntag(c, s) == N_CAST && cnt >= 1 &&
                   is_arith(c, c->ty[s]) && tkind(c, c->ty[s]) != TY_BOOL &&
                   is_arith(c, rvt(c, k[cnt - 1])) &&
                   type_size(TT, c->ty[s], &(bool){0}) >=
                   type_size(TT, rvt(c, k[cnt - 1]), &(bool){0})) {
            s = k[cnt - 1];
        } else {
            break;
        }
    }
    if (is_intcst(c, s) || c->ck[s] == K_FLOAT || inhibited(c, s, false) ||
        node_err(c, s))
        return;
    t = rvt(c, s);
    if (ntag(c, s) == N_BINARY && npunct(c, s) == P_STAR && cnt == 2) {
        if (is_arith(c, t))
            cwarn(c, cnode_loc(c, s), "int-in-bool-context", "'*' in boolean "
                  "context, suggest '&&' instead");
    } else if (ntag(c, s) == N_BINARY && npunct(c, s) == P_SHL && cnt == 2) {
        if (is_int(c, t) && is_signed(c, type_int_promote(TT, rvt(c, k[0]))))
            cwarn(c, cnode_loc(c, s), "int-in-bool-context", "'<<' in boolean "
                  "context, did you mean '<'?");
    } else if (ntag(c, s) == N_COND && cnt == 3 && is_int(c, t)) {
        bool c1 = is_intcst(c, k[1]), c2 = is_intcst(c, k[2]);
        long long v1 = c1 ? (long long)c->cv[k[1]] : 0,
                  v2 = c2 ? (long long)c->cv[k[2]] : 0;
        bool nb1 = c1 && v1 != 0 && v1 != 1, nb2 = c2 && v2 != 0 && v2 != 1;
        if (!nb1 && !nb2)
            return;
        if (c1 && c2 && v1 != 0 && v2 != 0)
            cwarn(c, colon_loc(c, s, k[1], k[2]), "int-in-bool-context",
                  "'?:' using integer constants in boolean context, the "
                  "expression will always evaluate to 'true'");
        else
            cwarn(c, colon_loc(c, s, k[1], k[2]), "int-in-bool-context",
                  "'?:' using integer constants in boolean context");
    }
}

void cexpr_truth_warn(Checker *c, uint32_t n, SrcLoc loc)
{
    AddrInfo ai;
    uint32_t s = strip_paren(c, n), k[3];
    unsigned bw, bp;
    int_bool_warn(c, n);
    if (!node_err(c, n) && !inhibited(c, n, false) &&
        (is_flt(c, rvt(c, n)) || is_complex(c, rvt(c, n)))) {
        int rep = is_complex(c, rvt(c, n)) ? 2 : 1;     /* both parts */
        while (rep--)
            cwarn(c, loc, "float-equal", "comparing floating-point with "
                  "'==' or '!=' is unsafe");
    }
    if (diag_enabled(c->diag, "sign-compare") && !node_err(c, n) &&
        !inhibited(c, n, false) && ntag(c, s) == N_UNARY &&
        bitnot_operand(c, s, &bw, &bp)) {
        cwarn(c, loc, "sign-compare", "promoted bitwise complement of an "
              "unsigned value is always nonzero");
        return;
    }
    for (;;) { /* pointer conversions and '+ 0' fold away */
        if (ntag(c, s) == N_CAST && is_ptr(c, mainv(c, c->ty[s])) &&
            (nkids(c, s, k, 3) >= 1)) {
            s = strip_paren(c, k[nkids(c, s, k, 3) - 1]);
        } else if (ntag(c, s) == N_BINARY && npunct(c, s) == P_PLUS &&
                   nkids(c, s, k, 3) == 2 && is_npc(c, k[1]) &&
                   is_int(c, rvt(c, k[1]))) {
            s = strip_paren(c, k[0]);
        } else {
            break;
        }
    }
    if (node_err(c, n) || inhibited(c, n, false) || !is_ptr(c, rvt(c, n)))
        return;
    /* gcc stays silent when the operand itself begins inside a macro
     * expansion (if (ID (&p->b)), if (ADDR (p)); an outer parenthesis or an
     * operator spelled by the caller brings the warning back) */
    if (loc == first_loc(c, n) && from_macro(c, first_tok(c, n)))
        return;
    if (ptr_plus_warn(c, loc, s, P_NE))
        return;
    if (!addr_target(c, s, &ai))
        return;
    if (ai.label && label_warned(c, cnode_ident(c, s)))
        ai.label = false;       /* gcc says it once per label */
    if (ai.label || (ai.direct && ai.ref != SYM_NONE &&
        !(csym(c, ai.ref)->flags & CSF_ADDR_WARNED))) {
        if (!ai.label)
            csym(c, ai.ref)->flags |= CSF_ADDR_WARNED;
        else if (loc == ctok_loc(c, c->nodes[s].tok))
            loc = ctok_loc(c, c->nodes[s].tok - 1);   /* the && of &&label */
        cwarn(c, loc, "address", "the address of '%s' will always evaluate as "
              "'true'", ai.name);
        return;
    }
    null_addr_msg(c, loc, &ai, P_NE);
}

/* ---- -Wsizeof-pointer-memaccess ---- */

enum { MA_CMP = 1, MA_STR = 2, MA_COPY = 4 };
typedef struct {
    const char *name;
    signed char dst, src, len; /* argument positions, -1 for none */
    unsigned char flags;       /* compares / string functions / strn*cpy */
} MemAcc;

static const MemAcc memacc_tab[] = {
    {"strncmp", 0, 1, 2, MA_CMP | MA_STR},
    {"strncasecmp", 0, 1, 2, MA_CMP | MA_STR},
    {"strncpy", 0, 1, 2, MA_STR | MA_COPY},
    {"strncat", 0, 1, 2, MA_STR | MA_COPY},
    {"stpncpy", 0, 1, 2, MA_STR | MA_COPY},
    {"bcopy", 1, 0, 2, 0},
    {"memcpy", 0, 1, 2, 0},
    {"memmove", 0, 1, 2, 0},
    {"bcmp", 0, 1, 2, MA_CMP},
    {"memcmp", 0, 1, 2, MA_CMP},
    {"memset", 0, -1, 2, 0},
    {"bzero", 0, -1, 1, 0},
    {"memchr", -1, 0, 2, 0},
    {"snprintf", 0, -1, 1, MA_STR},
    {"vsnprintf", 0, -1, 1, MA_STR},
};

/* Is `a` (a call argument) the sizeof operand `x`?  `&*p` is `p` for gcc. */
static bool memacc_same(Checker *c, uint32_t a, uint32_t x)
{
    uint32_t k[3];
    a = strip_paren(c, a);
    x = strip_paren(c, x);
    if (ntag(c, a) == N_UNARY && npunct(c, a) == P_AMP &&
        nkids(c, a, k, 3) >= 1 && ntag(c, strip_paren(c, k[0])) == N_UNARY &&
        npunct(c, strip_paren(c, k[0])) == P_STAR &&
        nkids(c, strip_paren(c, k[0]), k, 3) >= 1)
        a = strip_paren(c, k[0]);
    if (ntag(c, a) == N_STRING && ntag(c, x) == N_STRING) {
        StrBuf s0 = {0}, s1 = {0};
        bool eq;
        pexpr(c, &s0, a, PR_UNARY);
        pexpr(c, &s1, x, PR_UNARY);
        eq = !strcmp(sb_cstr(&s0), sb_cstr(&s1));
        sb_free(&s0);
        sb_free(&s1);
        return eq;
    }
    return opeq(c, a, x);
}

/* An array whose size is known (a VLA included). */
static bool array_known(Checker *c, TypeId t)
{
    bool ok = false;
    if (tkind(c, t) == TY_VLA)
        return true;
    return tkind(c, t) == TY_ARRAY && (type_size(TT, t, &ok), ok);
}

/* integer_zerop of the converted operand: a null pointer constant, also
 * cast to another pointer type. */
static bool null_valued(Checker *c, uint32_t n)
{
    uint32_t k[3];
    n = strip_paren(c, n);
    while (is_npc(c, n) == false && ntag(c, n) == N_CAST &&
           is_ptr(c, c->ty[n]) && nkids(c, n, k, 3) >= 1)
        n = strip_paren(c, k[nkids(c, n, k, 3) - 1]);
    return is_npc(c, n);
}

/* A literal zero: one number or character token. */
static bool literal_zero(Checker *c, uint32_t a)
{
    uint32_t t;
    if (!is_intcst(c, a) || c->cv[a] != 0 || first_tok(c, a) != last_tok(c, a))
        return false;
    t = first_tok(c, a);
    return c->u->toks[t].t.kind == TK_PPNUM || c->u->toks[t].t.kind == TK_CHAR;
}

/* warn_for_memset: -Wmemset-transposed-args and -Wmemset-elt-size of a call
 * of memset (kv[0] the callee, then the arguments). */
void memset_args(Checker *c, const uint32_t *kv, uint32_t nk, const char *name)
{
    uint32_t a, ck[3], j;
    TypeId at, et;
    bool ok = false, ok2 = false;
    uint64_t sz, esz;
    if (strcmp(name, "memset") && strcmp(name, "__builtin_memset"))
        return;
    if (nk != 4)
        return;
    for (j = 1; j < 4; j++)
        if (node_err(c, kv[j]))
            return;
    if (diag_enabled(c->diag, "memset-transposed-args") &&
        literal_zero(c, kv[3]) && !literal_zero(c, kv[2]) &&
        is_intcst(c, kv[3]))
        cwarn(c, first_loc(c, kv[0]), "memset-transposed-args", "'memset' used "
              "with constant zero length parameter; this could be due to "
              "transposed parameters");
    if (!diag_enabled(c->diag, "memset-elt-size") || !is_intcst(c, kv[3]))
        return;
    a = strip_paren(c, kv[1]);
    while (ntag(c, a) == N_CAST && nkids(c, a, ck, 3) >= 1)
        a = strip_paren(c, ck[nkids(c, a, ck, 3) - 1]);
    if (ntag(c, a) == N_UNARY && npunct(c, a) == P_AMP &&
        nkids(c, a, ck, 3) == 1)
        a = strip_paren(c, ck[0]);
    at = c->ty[a];
    if (tkind(c, at) != TY_ARRAY)
        return;
    et = elem_of(c, at);
    sz = type_size(TT, at, &ok);
    esz = type_size(TT, et, &ok2);
    if (ok && ok2 && esz > 1 && sz / esz == (uint64_t)c->cv[kv[3]])
        cwarn(c, first_loc(c, kv[0]), "memset-elt-size", "'memset' used with "
              "length equal to number of elements without multiplication by "
              "element size");
}

void sizeof_memaccess(Checker *c, const uint32_t *kv, uint32_t nk,
                             const char *name)
{
    const char *nm = name;
    size_t j, len;
    const MemAcc *m = NULL;
    uint32_t sz, a, role;
    TypeId ty;
    bool is_expr, chk = false, legit = false;
    SrcLoc loc;
    if (!diag_enabled(c->diag, "sizeof-pointer-memaccess"))
        return;
    if (!strncmp(nm, "__builtin___", 12)) {
        nm += 12;
        chk = true;
    } else if (!strncmp(nm, "__builtin_", 10))
        nm += 10;
    len = strlen(nm);
    if (chk) {
        if (len < 5 || strcmp(nm + len - 4, "_chk"))
            return;
        len -= 4;
    }
    for (j = 0; j < sizeof memacc_tab / sizeof *memacc_tab; j++)
        if (strlen(memacc_tab[j].name) == len &&
            !strncmp(memacc_tab[j].name, nm, len))
            m = &memacc_tab[j];
    if (!m || nk < (uint32_t)m->len + 2 || nk < 3)
        return;
    sz = strip_paren(c, kv[1 + m->len]);
    if (ntag(c, sz) != N_SIZEOF_EXPR && ntag(c, sz) != N_SIZEOF_TYPE)
        return;
    a = first_child(c, sz);
    if (a == NO_NODE || node_err(c, a) || is_err(c, c->ty[a]))
        return;
    is_expr = ntag(c, sz) == N_SIZEOF_EXPR;
    ty = c->ty[a];
    loc = first_loc(c, a);
    for (role = 0; role < 2; role++) {
        int idx = role ? m->src : m->dst;
        if (idx >= 0 && node_err(c, kv[1 + idx]))
            return;
    }
    if (!is_ptr(c, ty)) {
        /* strncpy (d, s, sizeof s) with an array or literal s; an array
         * destination or a nonstring source is no mistake */
        uint32_t sa = (m->flags & MA_COPY) ? strip_paren(c, kv[1 + m->src])
                                           : NO_NODE;
        uint32_t sref = sa != NO_NODE && ntag(c, sa) == N_IDENT
                            ? lookup_ord(c, cnode_ident(c, sa)) : SYM_NONE;
        if ((m->flags & MA_COPY) && is_expr && is_array(c, ty) &&
            !array_known(c, c->ty[strip_paren(c, kv[1 + m->dst])]) &&
            !(sref != SYM_NONE &&
              cdecl_aset_has(c, csym(c, sref)->aset, "nonstring", NULL)) &&
            memacc_same(c, kv[1 + m->src], a) &&
            !memacc_same(c, kv[1 + m->dst], kv[1 + m->src]))
            cwarn(c, loc, "sizeof-pointer-memaccess", "argument to 'sizeof' "
                  "in '%s' call is the same expression as the source; did you "
                  "mean to use the size of the destination?", name);
        return;
    }
    /* memcpy (&p, q, sizeof p): an argument whose pointee is the sizeof type
     * is what the size is meant for (casts look through) */
    for (role = 0; role < 2; role++) {
        int idx = role ? m->src : m->dst;
        uint32_t arg;
        uint32_t ck[3];
        TypeId at;
        if (idx < 0)
            continue;
        arg = strip_paren(c, kv[1 + idx]);
        while (ntag(c, arg) == N_CAST && nkids(c, arg, ck, 3) >= 1)
            arg = strip_paren(c, ck[nkids(c, arg, ck, 3) - 1]);
        if (node_err(c, arg))
            continue;
        at = rvt(c, arg);
        if (is_ptr(c, at) && !is_void(c, pointee(c, at)) &&
            type_compatible(TT, mainv(c, pointee(c, at)), mainv(c, ty)))
            legit = true;
    }
    for (role = 0; role < 2; role++) {
        int idx = role ? m->src : m->dst;
        const char *what;
        uint32_t arg;
        TypeId at;
        bool same;
        if (idx < 0)
            continue;
        arg = strip_paren(c, kv[1 + idx]);
        if (node_err(c, arg) || ntag(c, arg) == N_CAST)
            continue;
        at = rvt(c, arg);
        if (!is_ptr(c, at))
            continue;
        what = role ? ((m->flags & MA_CMP) ? "second source" : "source")
                    : ((m->flags & MA_CMP) ? "first source" : "destination");
        if (m->dst < 0)
            what = "source";
        same = is_expr && memacc_same(c, arg, a);
        if (same) {
            const char *how;
            TypeId pt = pointee(c, at);
            uint32_t op = strip_paren(c, a);
            bool one = is_int(c, pt) && !is_void(c, pt);
            if (one) {
                bool ok = false;
                uint64_t sz1 = type_size(TT, pt, &ok);
                one = ok && sz1 == 1;
            }
            if (ntag(c, op) == N_UNARY && npunct(c, op) == P_AMP)
                how = "remove the addressof";
            else if ((m->flags & MA_STR) || one)
                how = "provide an explicit length";
            else
                how = "dereference it";
            cwarn(c, loc, "sizeof-pointer-memaccess", "argument to 'sizeof' "
                  "in '%s' call is the same expression as the %s; did you "
                  "mean to %s?", name, what, how);
            return;
        }
        if (!legit && !(m->flags & MA_STR) && !is_void(c, pointee(c, at)) &&
            tquals(c, ty) == tquals(c, at) &&
            type_compatible(TT, mainv(c, ty), mainv(c, at))) {
            char b0[256], b1[256];
            snprintf(b0, sizeof b0, "%s", type_q(TT, at));
            snprintf(b1, sizeof b1, "%s", type_q(TT, unqual(c, pointee(c, at))));
            cwarn(c, loc, "sizeof-pointer-memaccess", "argument to 'sizeof' "
                  "in '%s' call is the same pointer type %s as the %s; "
                  "expected %s or an explicit length", name, b0, what, b1);
            return;
        }
    }
}

/* ---- -Wsizeof-pointer-div ---- */

static bool sizeof_operand(Checker *c, uint32_t n, TypeId *ty, uint32_t *decl)
{
    uint32_t a;
    n = strip_paren(c, n);
    *decl = NO_NODE;
    if (ntag(c, n) != N_SIZEOF_TYPE && ntag(c, n) != N_SIZEOF_EXPR)
        return false;
    a = first_child(c, n);
    if (a == NO_NODE)
        return false;
    if (ntag(c, n) == N_SIZEOF_EXPR) {
        uint32_t s = strip_paren(c, a);
        if (node_err(c, a))
            return false;
        if (ntag(c, s) == N_IDENT)
            *decl = s;
    }
    *ty = c->ty[a];
    return !is_err(c, *ty);
}

void sizeof_div(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId t0, t1;
    uint32_t d0, d1;
    StrBuf s0 = {0}, s1 = {0};
    Diagnostic *d;
    if (!sizeof_operand(c, a, &t0, &d0) || !sizeof_operand(c, b, &t1, &d1) ||
        is_err(c, t1))
        return;
    if (tkind(c, t0) == TY_ARRAY) {     /* -Wsizeof-array-div */
        TypeId et = elem_of(c, t0);
        bool ok0 = false, ok1 = false;
        char n0[256], n1[256];
        if (strip_paren(c, b) != b || !diag_enabled(c->diag, "sizeof-array-div") ||
            tkind(c, et) == TY_CHAR || tkind(c, et) == TY_SCHAR ||
            tkind(c, et) == TY_UCHAR || tkind(c, et) == TY_ARRAY ||
            type_size(TT, et, &ok0) == type_size(TT, t1, &ok1) || !ok0 || !ok1)
            return;
        snprintf(n0, sizeof n0, "%s", type_q(TT, unqual(c, et)));
        snprintf(n1, sizeof n1, "%s", type_q(TT, t1));
        d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "sizeof-array-div",
                    "expression does not compute the number of elements in "
                    "this array; element type is %s, not %s", n0, n1);
        cnote(c, d, cnode_loc(c, i), "add parentheses around the second "
              "'sizeof' to silence this warning");
        if (d0 != NO_NODE) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, d0));
            if (ref != SYM_NONE)
                cnote(c, d, csym(c, ref)->loc, "array '%s' declared here",
                      cident(c, cnode_ident(c, d0)));
        }
        return;
    }
    if (!is_ptr(c, t0))
        return;
    if (d0 != NO_NODE) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, d0));
        if (ref != SYM_NONE && (csym(c, ref)->flags & CSF_ARRAY_PARM))
            return;
    }
    if (tquals(c, pointee(c, t0)) != tquals(c, t1) ||
        !type_compatible(TT, mainv(c, pointee(c, t0)), mainv(c, t1)))
        return;
    type_print(TT, &s0, t0);
    type_print(TT, &s1, t1);
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "sizeof-pointer-div",
                "division 'sizeof (%s) / sizeof (%s)' does not compute the "
                "number of array elements", sb_cstr(&s0), sb_cstr(&s1));
    if (d0 != NO_NODE) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, d0));
        if (ref != SYM_NONE)
            cnote(c, d, csym(c, ref)->loc, "first 'sizeof' operand was "
                  "declared here");
    }
    sb_free(&s0);
    sb_free(&s1);
}

/* The warnings of build_binary_op's comparison case that precede the
 * operand checks of gcc's sign-compare (arithmetic operands). */
static bool compare_limits(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                           int op, TypeId ta, TypeId tb)
{
    SrcLoc loc = cnode_loc(c, i);
    bool folded, bfold;
    TypeId rt;
    bfold = bool_compare(c, i, a, b, op, loc);
    if (is_complex(c, ta) || is_complex(c, tb)) {
        /* complex integers: only the sign comparison applies */
        TypeId ca = is_complex(c, ta) ? cplx_comp(c, ta) : ta;
        TypeId cb = is_complex(c, tb) ? cplx_comp(c, tb) : tb;
        if (is_int(c, ca) && is_int(c, cb) && !inhibited(c, i, false) &&
            diag_enabled(c->diag, "sign-compare"))
            sign_compare(c, loc, a, b, op, common_type(c, ta, tb));
        return bfold;
    }
    if (!is_int(c, ta) || !is_int(c, tb))
        return bfold;
    rt = common_type(c, ta, tb);
    {   /* a bit-field wider than int keeps its own width in the conversions */
        unsigned wa = bf_width(c, a), wb = bf_width(c, b);
        unsigned ba = int_bits(c, ta), bb = int_bits(c, tb);
        if (wa >= 32 && wa < ba)
            ba = wa;
        if (wb >= 32 && wb < bb)
            bb = wb;
        if (ba < int_bits(c, ta) || bb < int_bits(c, tb)) {
            if (ba > bb)
                rt = ta;
            else if (bb > ba)
                rt = tb;
        }
    }
    if (!inhibited(c, i, false) && diag_enabled(c->diag, "sign-compare") &&
        bitnot_cmp(c, loc, a, b))
        return bfold;
    folded = type_limits(c, i, a, b, op, rt, loc);
    if (!folded && !inhibited(c, i, false) && diag_enabled(c->diag, "sign-compare"))
        sign_compare(c, loc, a, b, op, rt);
    return folded || bfold;
}


TypeId vec_elem(Checker *c, TypeId vt);
unsigned vec_esize(Checker *c, TypeId el);
void e_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool eq = op == P_EQEQ || op == P_NE;
    bool pa = is_ptr(c, ta), pb = is_ptr(c, tb);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
    if (is_arith(c, ta) && is_arith(c, tb) && tkind(c, ta) != TY_VECTOR &&
        tkind(c, tb) != TY_VECTOR && dec_mix(c, i, ta, tb))
        return;
    if (is_arith(c, ta) && is_arith(c, tb) && tkind(c, ta) != TY_VECTOR &&
        tkind(c, tb) != TY_VECTOR && diag_enabled(c->diag, "double-promotion")) {
        TypeId rt = common_type(c, ta, tb);
        double_promo(c, i, loc, ta, rt, "to match other operand of binary "
                     "expression");
        double_promo(c, i, loc, tb, rt, "to match other operand of binary "
                     "expression");
    }
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        /* a vector comparison yields a signed integer vector of the same
         * lane size and count */
        TypeId vt = tkind(c, ta) == TY_VECTOR ? ta : tb, el, rt;
        bool ok;
        uint64_t esz;
        if (tkind(c, ta) == TY_VECTOR && tkind(c, tb) == TY_VECTOR) {
            TypeId e1 = vec_elem(c, ta), e2 = vec_elem(c, tb);
            if (vec_esize(c, e1) != vec_esize(c, e2) ||
                is_int(c, e1) != is_int(c, e2)) {
                cerror(c, loc, "comparing vectors with different element "
                       "types");
                set_err(c, i);
                return;
            }
            if (type_ent(TT, type_canon(TT, ta))->n !=
                type_ent(TT, type_canon(TT, tb))->n) {
                cerror(c, loc, "comparing vectors with different number of "
                       "elements");
                set_err(c, i);
                return;
            }
            if (is_int(c, e1) && is_signed(c, e1) != is_signed(c, e2) &&
                !inhibited(c, i, false))
                cwarn(c, loc, "sign-compare", "comparison between types %s "
                      "and %s", type_q(TT, rvt(c, a)), type_q(TT, rvt(c, b)));
        } else {
            bool lv = tkind(c, ta) == TY_VECTOR;
            int r = vec_scalar(c, i, lv ? b : a,
                               unqual(c, rvt(c, lv ? b : a)), vt, false);
            if (r < 0)
                return;
            if (!r) {
                vec_invalid(c, i, a, b, op, 0, 0);
                return;
            }
        }
        vt = type_canon(TT, vt);
        el = type_base(TT, vt);
        esz = type_size(TT, el, &ok);
        rt = esz == 1 ? TYPE_B(SCHAR) : esz == 2 ? TYPE_B(SHORT)
           : esz == 4 ? TYPE_B(INT) : esz == 8 ? TYPE_B(LONG) : 0;
        c->ty[i] = ok && rt ? type_vector(TT, rt, type_ent(TT, vt)->n)
                            : TYPE_B(INT);
        return;
    }
    if (!((is_arith(c, ta) && is_arith(c, tb)) || pa || pb) ||
        (!eq && (is_complex(c, ta) || is_complex(c, tb))) ||
        ((pa || pb) && !(pa ? (pb || is_int(c, tb)) : is_int(c, ta)))) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    c->ty[i] = TYPE_B(INT);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_PROP;
    {
        bool fold = false;
        if (is_arith(c, ta) && is_arith(c, tb))
            fold = compare_limits(c, i, a, b, op, ta, tb);
        else if (eq && pa && is_npc(c, b))
            fold = null_addr_warn(c, loc, a, op, i);
        else if (eq && pb && is_npc(c, a))
            fold = null_addr_warn(c, loc, b, op, i);
        if (!fold && !is_flt(c, ta) && !is_flt(c, tb) && opeq(c, a, b))
            fold = true; /* x == x and the like */
        if (fold)
            c->ef[i] |= EF_GCCFOLD;
    }
    if (eq && ((pa && char_zero(c, b)) || (pb && char_zero(c, a)))) {
        uint32_t p = pa ? a : b;
        Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "pointer-compare",
                                "comparison between pointer and zero "
                                "character constant");
        cnote(c, d, first_loc(c, p), "did you mean to dereference the pointer?");
    }
    if (pa && pb) {
        TypeId tta = pointee(c, ta), ttb = pointee(c, tb);
        bool compat = targets_compat(c, loc, tta, ttb);
        if (eq) {
            if (!compat) {
                if (is_void(c, tta) && !(tquals(c, tta) & TQ_ATOMIC)) {
                    if (is_func(c, ttb) && !is_npc(c, a))
                        ped(c, i, loc, "ISO C forbids comparison of 'void *' "
                                       "with function pointer");
                } else if (is_void(c, ttb) && !(tquals(c, ttb) & TQ_ATOMIC)) {
                    if (is_func(c, tta) && !is_npc(c, b))
                        ped(c, i, loc, "ISO C forbids comparison of 'void *' "
                                       "with function pointer");
                } else {
                    cpedwarn(c, loc, "", "comparison of distinct pointer types "
                             "lacks a cast");
                }
            }
        } else if (compat) {
            if (complete(c, tta) != complete(c, ttb))
                ped(c, i, loc, "comparison of complete and incomplete "
                               "pointers");
            else if (is_func(c, tta))
                ped(c, i, loc, "ISO C forbids ordered comparisons of pointers "
                               "to functions");
            else if (extra_on(c) && (is_npc(c, a) || is_npc(c, b)))
                cwarn(c, loc, "extra", "ordered comparison of pointer with null "
                                  "pointer");
        } else {
            cpedwarn(c, loc, "", "comparison of distinct pointer types lacks a "
                                "cast");
        }
    } else if (pa || pb) {
        uint32_t in = pa ? b : a;
        if (is_npc(c, in)) {
            if (!eq) {
                if (c->opt.pedantic && !cexpr_in_extension(c, i))
                    ped(c, i, loc, "ordered comparison of pointer with integer "
                                   "zero");
                else if (extra_on(c))
                    cwarn(c, loc, "extra", "ordered comparison of pointer with "
                                      "integer zero");
            }
        } else {
            cpedwarn(c, loc, "", "comparison between pointer and integer");
        }
    } else if (tkind(c, orig_type(c, a)) == TY_ENUM &&
               tkind(c, orig_type(c, b)) == TY_ENUM &&
               mainv(c, orig_type(c, a)) != mainv(c, orig_type(c, b))) {
        cwarn(c, loc, "enum-compare", "comparison between %s and %s",
              type_q(TT, unqual(c, orig_type(c, a))),
              type_q(TT, unqual(c, orig_type(c, b))));
    }
    if (eq && !inhibited(c, i, false) &&
        (is_flt(c, rvt(c, a)) || is_complex(c, rvt(c, a)) ||
         is_flt(c, rvt(c, b)) || is_complex(c, rvt(c, b))))
        cwarn(c, loc, "float-equal", "comparing floating-point with '==' or "
              "'!=' is unsafe");
    if (!inhibited(c, i, false) &&
        (eq ? (((c->ef[sa] & EF_STRING) && !null_valued(c, b)) ||
               ((c->ef[sb] & EF_STRING) && !null_valued(c, a)))
            : (((c->ef[sa] | c->ef[sb]) & EF_STRING) != 0)))
        cwarn(c, loc, "address", "comparison with string literal results in "
                                 "unspecified behavior");
    /* values */
    if (pa || pb) {
        uint32_t cba, cbb;
        uint64_t va, vb;
        int r = -1;
        if (addr_val(c, a, &cba, &va) && addr_val(c, b, &cbb, &vb)) {
            bool equal = cba == cbb && va == vb;
            if (cba == cbb) {
                int cmp = va < vb ? -1 : va > vb;
                switch (op) {
                case P_EQEQ: r = equal; break;
                case P_NE: r = !equal; break;
                case P_LT: r = cmp < 0; break;
                case P_GT: r = cmp > 0; break;
                case P_LE: r = cmp <= 0; break;
                default: r = cmp >= 0; break;
                }
            } else if (eq && ((cba == 0 && va == 0) || (cbb == 0 && vb == 0))) {
                r = op == P_NE;
            } else if (!eq && ((cba == 0 && va == 0 && cbb && !(cbb & CB_NODE) &&
                                !addr_weak(c, cbb)) ||
                               (cbb == 0 && vb == 0 && cba && !(cba & CB_NODE) &&
                                !addr_weak(c, cba)))) {
                int cmp = cba == 0 ? -1 : 1;    /* an object address > null */
                r = op == P_LT ? cmp < 0 : op == P_GT ? cmp > 0 :
                    op == P_LE ? cmp <= 0 : cmp >= 0;
            } else if (eq && cba && cbb && !(cba & CB_NODE) && !(cbb & CB_NODE) &&
                       !addr_weak(c, cba) && !addr_weak(c, cbb)) {
                r = op == P_NE;         /* distinct declared objects */
            }
        }
        if (r >= 0) {
            c->ck[i] = K_FOLD;
            c->cv[i] = (uint64_t)r;
        }
        return;
    }
    {
        TypeId rt = common_type(c, ta, tb);
        long double fa, fb;
        int r = -1;
        if (is_int(c, rt) && int_bits(c, rt) <= 64 && has_ival(c, a) &&
            has_ival(c, b)) {
            uint64_t x = cexpr_trunc(c, rt, c->cv[a]);
            uint64_t y = cexpr_trunc(c, rt, c->cv[b]);
            int cmp;
            if (is_signed(c, rt))
                cmp = (int64_t)x < (int64_t)y ? -1 : (int64_t)x > (int64_t)y;
            else
                cmp = x < y ? -1 : x > y;
            switch (op) {
            case P_EQEQ: r = cmp == 0; break;
            case P_NE: r = cmp != 0; break;
            case P_LT: r = cmp < 0; break;
            case P_GT: r = cmp > 0; break;
            case P_LE: r = cmp <= 0; break;
            default: r = cmp >= 0; break;
            }
            bin_value(c, i, a, b, (uint64_t)r, false, true, loc);
            return;
        }
        if (is_flt(c, rt) && !is_decimal_flt(c, rt) && fval(c, a, &fa) &&
            fval(c, b, &fb)) {
            switch (op) {
            case P_EQEQ: r = fa == fb; break;
            case P_NE: r = fa != fb; break;
            case P_LT: r = fa < fb; break;
            case P_GT: r = fa > fb; break;
            case P_LE: r = fa <= fb; break;
            default: r = fa >= fb; break;
            }
            c->ck[i] = K_FOLD;
            c->cv[i] = (uint64_t)r;
            return;
        }
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
    }
}

/* wi::min_precision (v, SIGNED) */
unsigned min_prec_signed(uint64_t v)
{
    unsigned n = 0;
    if ((int64_t)v < 0)
        v = ~v;
    while (v) {
        n++;
        v >>= 1;
    }
    return n + 1;
}

