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
#include "c/check_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)

enum {
    PR_COMMA, PR_ASSIGN, PR_COND, PR_LOR, PR_LAND, PR_OR, PR_XOR, PR_AND,
    PR_EQ, PR_REL, PR_SHIFT, PR_ADD, PR_MUL, PR_CAST, PR_UNARY, PR_POSTFIX,
    PR_PRIMARY
};

/* ---- small helpers ----------------------------------------------------- */

static unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static TypeKind tkind(Checker *c, TypeId t)
{
    return type_ckind(TT, t);
}

static bool is_err(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ERROR;
}

/* The punctuator of token tok, P_NONE if it is not one. */
static int tpunct(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return P_NONE;
    t = &c->u->toks[tok].t;
    return t->kind == TK_PUNCT ? t->punct : P_NONE;
}

static int npunct(const Checker *c, uint32_t i)
{
    return tpunct(c, c->nodes[i].tok);
}

/* The C keyword token tok spells (CK_NONE if none). */
static int tckw(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return CK_NONE;
    t = &c->u->toks[tok].t;
    if (t->kind != TK_IDENT)
        return CK_NONE;
    return ident_by_id(c->in, t->aux)->ckw & 0xFF;
}

static const char *ttext(const Checker *c, uint32_t tok, size_t *len)
{
    const Tok *t = &c->u->toks[tok].t;
    *len = t->kind == TK_IDENT ? ident_by_id(c->in, t->aux)->len : t->len;
    return tok_text_raw(c->sm, c->in, t);
}

/* Children of i: the first and the last. */
static uint32_t first_child(const Checker *c, uint32_t i)
{
    uint32_t k, f = cfirst(c, i);
    if (c->nodes[i].size <= 1)
        return NO_NODE;
    k = i - 1;
    while (cfirst(c, k) > f)
        k = cfirst(c, k) - 1;
    return k;
}

static uint32_t nkids(const Checker *c, uint32_t i, uint32_t *out,
                      uint32_t max)
{
    return node_children(c->nodes, i, out, max);
}

/* The last token of i's subtree (approximately: the largest token any of
 * its nodes names). */
static uint32_t last_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = 0;
    for (k = cfirst(c, i); k <= i; k++) {
        uint32_t t = c->nodes[k].tok;
        if (c->nodes[k].tag == N_STRING && c->nodes[k].aux)
            t += c->nodes[k].aux - 1u;
        if (t > m)
            m = t;
    }
    return m;
}

/* The first token of i's subtree. */
static uint32_t first_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = c->nodes[i].tok;
    for (k = cfirst(c, i); k < i; k++)
        if (c->nodes[k].tok < m)
            m = c->nodes[k].tok;
    return m;
}

static SrcLoc first_loc(const Checker *c, uint32_t i)
{
    return ctok_loc(c, first_tok(c, i));
}

/* gcc's input_location once the parser is past expression i. */
static SrcLoc after_loc(Checker *c, uint32_t i)
{
    return cinput_loc(c, last_tok(c, i) + 1);
}

bool cexpr_is_extension(Checker *c, uint32_t i)
{
    return ntag(c, i) == N_UNARY && tckw(c, c->nodes[i].tok) == CK_EXTENSION;
}

static bool in_function(const Checker *c)
{
    return c->func_sym != SYM_NONE;
}

static void set_err(Checker *c, uint32_t i)
{
    c->ty[i] = ERRT;
    c->ck[i] = K_ERR;
}

static bool node_err(Checker *c, uint32_t i)
{
    return i == NO_NODE || is_err(c, c->ty[i]) || c->ck[i] == K_ERR;
}

static void copy_node(Checker *c, uint32_t to, uint32_t from)
{
    c->ty[to] = c->ty[from];
    c->ck[to] = c->ck[from];
    c->cv[to] = c->cv[from];
    c->cb[to] = c->cb[from];
    c->ef[to] = c->ef[from];
}

/* Skips parentheses. */
static uint32_t strip_paren(const Checker *c, uint32_t i)
{
    while (i != NO_NODE && c->nodes[i].tag == N_PAREN)
        i = c->nodes[i].size > 1 ? i - 1 : NO_NODE;
    return i;
}

static uint32_t fpush(Checker *c, long double v)
{
    vec_push(&c->fv, v);
    return (uint32_t)c->fv.len - 1;
}

/* ---- types --------------------------------------------------------------- */

static bool is_int(Checker *c, TypeId t) { return type_is_integer(TT, t); }
static bool is_flt(Checker *c, TypeId t) { return type_is_float(TT, t); }
static bool is_arith(Checker *c, TypeId t) { return type_is_arith(TT, t); }
static bool is_ptr(Checker *c, TypeId t) { return tkind(c, t) == TY_PTR; }
static bool is_scalar(Checker *c, TypeId t) { return type_is_scalar(TT, t); }
static bool is_void(Checker *c, TypeId t) { return tkind(c, t) == TY_VOID; }
static bool is_func(Checker *c, TypeId t) { return tkind(c, t) == TY_FUNC; }
static bool is_record(Checker *c, TypeId t) { return type_is_record(TT, t); }

static bool is_array(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k == TY_ARRAY || k == TY_VLA;
}

static bool is_complex(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_COMPLEX;
}

static bool is_signed(Checker *c, TypeId t)
{
    return type_is_signed(TT, t);
}

/* The qualifiers t has, typedefs looked through. */
static unsigned tquals(Checker *c, TypeId t)
{
    return TYPE_QUALS(type_canon(TT, t));
}

/* The pointed-to type of a pointer (qualified). */
static TypeId pointee(Checker *c, TypeId t)
{
    return type_base(TT, t);
}

/* t without its top-level qualifiers, the typedef spelling kept when the
 * typedef does not itself add qualifiers. */
static TypeId unqual(Checker *c, TypeId t)
{
    t = TYPE_UNQUAL(t);
    if (type_ent(TT, t)->kind == TY_TYPEDEF && TYPE_QUALS(type_canon(TT, t)))
        t = TYPE_UNQUAL(type_canon(TT, t));
    return t;
}

/* gcc's TYPE_MAIN_VARIANT: the canonical type without qualifiers. */
static TypeId mainv(Checker *c, TypeId t)
{
    return TYPE_UNQUAL(type_canon(TT, t));
}

static unsigned int_bits(Checker *c, TypeId t)
{
    if (is_ptr(c, t))
        return c->tgt->ptr_size * 8u;
    return type_int_bits(TT, t);
}

static TypeId size_type(Checker *c)
{
    return TYPE_MK(c->tgt->size_type, 0);
}

static TypeId elem_of(Checker *c, TypeId t);

TypeId cexpr_rvalue_type(Checker *c, uint32_t i)
{
    TypeId t = c->ty[i];
    switch (tkind(c, t)) {
    case TY_ARRAY: case TY_VLA:
        return type_ptr(TT, elem_of(c, t));
    case TY_FUNC:
        return type_ptr(TT, t);
    default:
        return unqual(c, t);
    }
}

static TypeId rvt(Checker *c, uint32_t i)
{
    return cexpr_rvalue_type(c, i);
}

/* The type after the integer promotions (bit-fields included). */
static TypeId promoted(Checker *c, uint32_t i)
{
    TypeId t = rvt(c, i);
    if (!is_int(c, t))
        return t;
    if (c->ef[i] & EF_BFPROMOTE)
        return TYPE_B(INT);
    t = type_int_promote(TT, t);
    if (tkind(c, t) == TY_ENUM)
        t = TYPE_B(UINT);
    return TYPE_UNQUAL(type_canon(TT, t));
}

/* Floating types by precision (gcc's c_common_type). */
static int float_prec(Checker *c, TypeKind k)
{
    switch (k) {
    case TY_BF16: return 8;
    case TY_FLOAT16: return 11;
    case TY_FLOAT: case TY_FLOAT32: return 24;
    case TY_DOUBLE: case TY_FLOAT64: case TY_FLOAT32X: return 53;
    case TY_LDOUBLE:
        switch (c->tgt->long_double) {
        case LD_X87: return 64;
        case LD_IEEE64: return 53;
        case LD_IEEE128: return 113;
        default: return 106;
        }
    case TY_FLOAT64X: return 64;
    case TY_IBM128: return 106;
    case TY_FLOAT128: return 113;
    case TY_DEC32: return 7;
    case TY_DEC64: return 16;
    case TY_DEC128: return 34;
    default: return 0;
    }
}

/* Preference among floating types of one precision. */
static int fl_pref(TypeKind k)
{
    if (k == TY_FLOAT32X || k == TY_FLOAT64X)
        return 0;
    if (k == TY_FLOAT || k == TY_DOUBLE || k == TY_LDOUBLE)
        return 1;
    return 2;
}

/* The usual arithmetic conversions of two promoted arithmetic types. */
static TypeId common_type(Checker *c, TypeId a, TypeId b)
{
    TypeKind ka, kb;
    a = mainv(c, a);
    b = mainv(c, b);
    if (a == b)
        return a;
    if (is_complex(c, a) || is_complex(c, b)) {
        TypeId ra = is_complex(c, a) ? mainv(c, type_base(TT, a)) : a;
        TypeId rb = is_complex(c, b) ? mainv(c, type_base(TT, b)) : b;
        TypeId r = common_type(c, ra, rb);
        if (is_complex(c, a) && mainv(c, type_base(TT, a)) == r)
            return a;
        if (is_complex(c, b) && mainv(c, type_base(TT, b)) == r)
            return b;
        return type_complex(TT, r);
    }
    ka = tkind(c, a);
    kb = tkind(c, b);
    if (is_flt(c, a) || is_flt(c, b)) {
        if (!is_flt(c, b))
            return a;
        if (!is_flt(c, a))
            return b;
        if (float_prec(c, ka) != float_prec(c, kb))
            return float_prec(c, ka) > float_prec(c, kb) ? a : b;
        /* same precision: prefer _FloatN, then long double, double, and
         * last _FloatNx */
        if (fl_pref(ka) != fl_pref(kb))
            return fl_pref(ka) > fl_pref(kb) ? a : b;
        if (ka >= TY_FLOAT32 && ka <= TY_FLOAT64X)
            return a;
        if (ka == TY_LDOUBLE || kb == TY_LDOUBLE)
            return TYPE_B(LDOUBLE);
        if (ka == TY_DOUBLE || kb == TY_DOUBLE)
            return TYPE_B(DOUBLE);
        return a;
    }
    if (int_bits(c, a) != int_bits(c, b))
        return int_bits(c, a) > int_bits(c, b) ? a : b;
    if (ka == TY_ULLONG || kb == TY_ULLONG)
        return TYPE_B(ULLONG);
    if (ka == TY_LLONG || kb == TY_LLONG)
        return !is_signed(c, a) || !is_signed(c, b) ? TYPE_B(ULLONG)
                                                    : TYPE_B(LLONG);
    if (ka == TY_ULONG || kb == TY_ULONG)
        return TYPE_B(ULONG);
    if (ka == TY_LONG || kb == TY_LONG)
        return !is_signed(c, a) || !is_signed(c, b) ? TYPE_B(ULONG)
                                                    : TYPE_B(LONG);
    return !is_signed(c, a) ? a : b;
}

/* ---- integer values ------------------------------------------------------ */

uint64_t cexpr_trunc(Checker *c, TypeId t, uint64_t v)
{
    unsigned bits;
    if (tkind(c, t) == TY_BOOL)
        return v != 0;
    if (!is_int(c, t) && !is_ptr(c, t))
        return v;
    bits = int_bits(c, t);
    if (bits == 0 || bits >= 64)
        return v;
    v &= (UINT64_C(1) << bits) - 1;
    if (is_int(c, t) && is_signed(c, t) && (v >> (bits - 1)) & 1)
        v |= ~((UINT64_C(1) << bits) - 1);
    return v;
}

int64_t cexpr_sval(Checker *c, uint32_t i)
{
    return (int64_t)c->cv[i];
}

bool cexpr_fits(Checker *c, uint64_t v, TypeId from, TypeId to)
{
    bool neg = is_int(c, from) && is_signed(c, from) && (int64_t)v < 0;
    unsigned bits = int_bits(c, to);
    if (tkind(c, to) == TY_BOOL)
        return v <= 1 && !neg;
    if (bits == 0 || bits > 64)
        bits = 64;
    if (is_int(c, to) && is_signed(c, to)) {
        int64_t lo, hi;
        if (bits == 64) {
            /* an unsigned value above INT64_MAX does not fit */
            return neg || (int64_t)v >= 0;
        }
        hi = (int64_t)((UINT64_C(1) << (bits - 1)) - 1);
        lo = -hi - 1;
        if (!neg && v > (uint64_t)hi)
            return false;
        return (int64_t)v >= lo;
    }
    if (neg)
        return false;
    return bits == 64 || v < (UINT64_C(1) << bits);
}

/* Does v (per its type t) have the value zero / is it negative? */
static bool ival_neg(Checker *c, TypeId t, uint64_t v)
{
    return is_int(c, t) && is_signed(c, t) && (int64_t)v < 0;
}

/* Integer arithmetic in type t (promoted; at most 64 bits).  Sets *ovf on
 * signed overflow, *zdiv on division by zero. */
static uint64_t int_op(Checker *c, int op, TypeId t, uint64_t a, uint64_t b,
                       bool *ovf, bool *zdiv)
{
    bool sg = is_signed(c, t);
    unsigned bits = int_bits(c, t);
    uint64_t r = 0;
    int64_t sa = (int64_t)a, sb = (int64_t)b;
    *ovf = false;
    *zdiv = false;
    if (bits > 64 || bits == 0)
        bits = 64;
    switch (op) {
    case P_PLUS:
        r = a + b;
        if (sg && bits == 64)
            *ovf = (int64_t)((a ^ r) & (b ^ r)) < 0;
        break;
    case P_MINUS:
        r = a - b;
        if (sg && bits == 64)
            *ovf = (int64_t)((a ^ b) & (a ^ r)) < 0;
        break;
    case P_STAR:
        if (sg && bits == 64) {
            r = a * b;
            if (sa != 0 && sb != 0) {
                if ((sa == -1 && sb == INT64_MIN) ||
                    (sb == -1 && sa == INT64_MIN))
                    *ovf = true;
                else
                    *ovf = (int64_t)r / sb != sa;
            }
        } else if (sg) {
            r = (uint64_t)(sa * sb);   /* |a|, |b| < 2^32: exact */
        } else {
            r = a * b;
        }
        break;
    case P_SLASH: case P_PERCENT:
        if (b == 0) {
            *zdiv = true;
            return 0;
        }
        if (sg) {
            int64_t min = bits == 64 ? INT64_MIN : -(INT64_C(1) << (bits - 1));
            if (sa == min && sb == -1) {
                *ovf = true;
                r = op == P_SLASH ? (uint64_t)min : 0;
            } else {
                r = (uint64_t)(op == P_SLASH ? sa / sb : sa % sb);
            }
        } else {
            r = op == P_SLASH ? a / b : a % b;
        }
        break;
    case P_AMP: r = a & b; break;
    case P_PIPE: r = a | b; break;
    case P_CARET: r = a ^ b; break;
    case P_SHL:
        r = b >= 64 ? 0 : a << b;
        break;
    case P_SHR:
        if (b >= 64)
            r = sg && sa < 0 ? ~UINT64_C(0) : 0;
        else if (sg && sa < 0)
            r = ~(~a >> b);
        else
            r = a >> b;
        break;
    default:
        break;
    }
    if (bits < 64) {
        uint64_t tr = cexpr_trunc(c, t, r);
        if (sg && op != P_SHL && tr != r)
            *ovf = true;
        r = tr;
    }
    return r;
}

/* ---- floating values ----------------------------------------------------- */

static long double fround(Checker *c, TypeId t, long double v)
{
    switch (tkind(c, t)) {
    case TY_FLOAT: case TY_FLOAT32:
        return (float)v;
    case TY_DOUBLE: case TY_FLOAT64: case TY_FLOAT32X:
        return (double)v;
    case TY_LDOUBLE:
        if (c->tgt->long_double == LD_IEEE64)
            return (double)v;
        return v;
    default:
        return v;
    }
}

/* The value of node i as a long double, if it is an arithmetic constant. */
static bool fval(Checker *c, uint32_t i, long double *out)
{
    switch (c->ck[i]) {
    case K_FLOAT:
        *out = c->fv.data[c->cv[i]];
        return true;
    case K_ICE: case K_FOLD:
        if (!is_int(c, rvt(c, i)))
            return false;
        *out = ival_neg(c, rvt(c, i), c->cv[i]) ? (long double)(int64_t)c->cv[i]
                                               : (long double)c->cv[i];
        return true;
    default:
        return false;
    }
}

/* Does node i have a known integer value? */
static bool has_ival(Checker *c, uint32_t i)
{
    return (c->ck[i] == K_ICE || c->ck[i] == K_FOLD) && is_int(c, rvt(c, i));
}

/* gcc: an INTEGER_CST once C_MAYBE_CONST_EXPRs are removed. */
static bool is_intcst(Checker *c, uint32_t i)
{
    return c->ck[i] == K_ICE || (c->ck[i] == K_FOLD && (c->ef[i] & EF_CST));
}

/* EXPR_INT_CONST_OPERANDS. */
static bool intops(Checker *c, uint32_t i)
{
    return is_int(c, rvt(c, i)) &&
           (c->ck[i] == K_ICE || (c->ef[i] & EF_INTOPS));
}

/* The truth value of node i if known: 1 true, 0 false, -1 unknown.  fold:
 * as c_fully_fold sees it (folded values too), else as the parser does. */
static int truth(Checker *c, uint32_t i, bool fold)
{
    long double f;
    switch (c->ck[i]) {
    case K_ICE:
        return c->cv[i] != 0;
    case K_FOLD:
        if (!fold && !(c->ef[i] & EF_CST))
            return -1;
        return c->cv[i] != 0;
    case K_FLOAT:
        if (!fold && !(c->ef[i] & EF_REALCST))
            return -1;
        f = c->fv.data[c->cv[i]];
        return f != 0;
    case K_ADDR:
        if (c->cb[i])
            return 1;
        return fold ? c->cv[i] != 0 : -1;
    default:
        return -1;
    }
}

/* ---- context --------------------------------------------------------------- */

/* gcc's c_inhibit_evaluation_warnings at node i: inside sizeof, alignof,
 * typeof, a _Generic selector, or an operand not evaluated because of a
 * constant condition. */
static bool inhibited(Checker *c, uint32_t i, bool fold)
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
    return diag_enabled(c->diag, "c++-compat") &&
           (node == NO_NODE || !cexpr_in_extension(c, node));
}

bool cexpr_in_extension(Checker *c, uint32_t i)
{
    uint32_t p;
    for (p = i; p != NO_NODE; p = c->par[p]) {
        const Node *n = &c->nodes[p];
        if (n->tag == N_UNARY && tckw(c, n->tok) == CK_EXTENSION)
            return true;
        if ((n->tag == N_DECL || n->tag == N_FUNC_DEF) &&
            (n->flags & NF_EXTENSION))
            return true;
    }
    return false;
}

/* A -Wpedantic pedwarn about node i (off under __extension__). */
static Diagnostic *ped(Checker *c, uint32_t i, SrcLoc loc, const char *fmt,
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
static void ped_arith(Checker *c, uint32_t i, SrcLoc loc, const char *msg)
{
    if (cexpr_in_extension(c, i))
        return;
    cpedwarn(c, loc, "pointer-arith", "%s", msg);
}

/* ---- members ------------------------------------------------------------- */

/* The field name of record rec (anonymous members searched, depth first);
 * adds its offset to *off and the qualifiers of the anonymous members on
 * the way to *quals. */
static const Field *find_field(Checker *c, TypeId rec, uint32_t name,
                               uint64_t *off, unsigned *quals)
{
    const Record *r;
    uint32_t k;
    if (!is_record(c, rec))
        return NULL;
    r = type_record(TT, rec);
    if (!(r->flags & RF_COMPLETE) && !(r->flags & RF_DEFINING))
        return NULL;
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (f->name == name) {
            *off += f->off_bits;
            return f;
        }
    }
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (!f->name && is_record(c, f->ty)) {
            uint64_t o = *off + f->off_bits;
            unsigned q = *quals | tquals(c, f->ty);
            const Field *g = find_field(c, f->ty, name, &o, &q);
            if (g) {
                *off = o;
                *quals = q;
                return g;
            }
            r = type_record(TT, rec);   /* the table may have moved */
        }
    }
    return NULL;
}

bool cexpr_find_member(Checker *c, TypeId rec, uint32_t name, TypeId *ty,
                       uint64_t *off_bits, bool *bitfield)
{
    uint64_t off = 0;
    unsigned q = 0;
    const Field *f = find_field(c, rec, name, &off, &q);
    if (!f)
        return false;
    *ty = type_qual(f->ty, q);
    *off_bits += off;
    *bitfield = (f->flags & FF_BITFIELD) != 0;
    return true;
}

/* ---- spelling suggestions (gcc's spellcheck.cc) ---------------------------- */


/* A misspelled member name: the fields of rec, anonymous members'
 * included. */
static void fuzzy_fields(Checker *c, Best *b, TypeId rec)
{
    const Record *r;
    uint32_t k;
    if (!is_record(c, rec))
        return;
    r = type_record(TT, rec);
    for (k = 0; k < r->nfields; k++) {
        const Field *f = &c->tt.fields.data[r->fields + k];
        if (f->name)
            best_consider(b, cident(c, f->name));
        else
            fuzzy_fields(c, b, f->ty);
        r = type_record(TT, rec);
    }
}

/* The spelling suggestion for a misspelled member of rec (NULL: none). */
const char *cexpr_fuzzy_field(Checker *c, TypeId rec, uint32_t name)
{
    Best b;
    best_init(&b, cident(c, name), &c->fuzzy_work);
    fuzzy_fields(c, &b, rec);
    return best_get(&b);
}

/* ---- missing headers (gcc's known-headers.cc, C part) ---------------------- */

typedef struct StdName {
    const char *name, *header;
} StdName;

static const StdName std_names[] = {
    {"assert", "<assert.h>"}, {"errno", "<errno.h>"},
    {"CHAR_BIT", "<limits.h>"}, {"CHAR_MAX", "<limits.h>"},
    {"CHAR_MIN", "<limits.h>"}, {"INT_MAX", "<limits.h>"},
    {"INT_MIN", "<limits.h>"}, {"LLONG_MAX", "<limits.h>"},
    {"LLONG_MIN", "<limits.h>"}, {"LONG_MAX", "<limits.h>"},
    {"LONG_MIN", "<limits.h>"}, {"MB_LEN_MAX", "<limits.h>"},
    {"SCHAR_MAX", "<limits.h>"}, {"SCHAR_MIN", "<limits.h>"},
    {"SHRT_MAX", "<limits.h>"}, {"SHRT_MIN", "<limits.h>"},
    {"UCHAR_MAX", "<limits.h>"}, {"UINT_MAX", "<limits.h>"},
    {"ULLONG_MAX", "<limits.h>"}, {"ULONG_MAX", "<limits.h>"},
    {"USHRT_MAX", "<limits.h>"},
    {"DBL_MAX", "<float.h>"}, {"DBL_MIN", "<float.h>"},
    {"FLT_MAX", "<float.h>"}, {"FLT_MIN", "<float.h>"},
    {"LDBL_MAX", "<float.h>"}, {"LDBL_MIN", "<float.h>"},
    {"va_list", "<stdarg.h>"},
    {"NULL", "<stddef.h>"}, {"offsetof", "<stddef.h>"},
    {"size_t", "<stddef.h>"}, {"wchar_t", "<stddef.h>"},
    {"ptrdiff_t", "<stddef.h>"},
    {"BUFSIZ", "<stdio.h>"}, {"EOF", "<stdio.h>"}, {"FILE", "<stdio.h>"},
    {"FILENAME_MAX", "<stdio.h>"}, {"fopen", "<stdio.h>"},
    {"fpos_t", "<stdio.h>"}, {"getchar", "<stdio.h>"},
    {"printf", "<stdio.h>"}, {"snprintf", "<stdio.h>"},
    {"sprintf", "<stdio.h>"}, {"stderr", "<stdio.h>"},
    {"stdin", "<stdio.h>"}, {"stdout", "<stdio.h>"},
    {"EXIT_FAILURE", "<stdlib.h>"}, {"EXIT_SUCCESS", "<stdlib.h>"},
    {"abort", "<stdlib.h>"}, {"atexit", "<stdlib.h>"},
    {"calloc", "<stdlib.h>"}, {"exit", "<stdlib.h>"},
    {"free", "<stdlib.h>"}, {"getenv", "<stdlib.h>"},
    {"malloc", "<stdlib.h>"}, {"realloc", "<stdlib.h>"},
    {"memchr", "<string.h>"}, {"memcmp", "<string.h>"},
    {"memcpy", "<string.h>"}, {"memmove", "<string.h>"},
    {"memset", "<string.h>"}, {"strcat", "<string.h>"},
    {"strchr", "<string.h>"}, {"strcmp", "<string.h>"},
    {"strcpy", "<string.h>"}, {"strlen", "<string.h>"},
    {"strncat", "<string.h>"}, {"strncmp", "<string.h>"},
    {"strncpy", "<string.h>"}, {"strrchr", "<string.h>"},
    {"strspn", "<string.h>"}, {"strstr", "<string.h>"},
    {"PTRDIFF_MAX", "<stdint.h>"}, {"PTRDIFF_MIN", "<stdint.h>"},
    {"SIG_ATOMIC_MAX", "<stdint.h>"}, {"SIG_ATOMIC_MIN", "<stdint.h>"},
    {"SIZE_MAX", "<stdint.h>"}, {"WINT_MAX", "<stdint.h>"},
    {"WINT_MIN", "<stdint.h>"},
    {"INT8_MAX", "<stdint.h>"}, {"INT16_MAX", "<stdint.h>"},
    {"INT32_MAX", "<stdint.h>"}, {"INT64_MAX", "<stdint.h>"},
    {"INT8_MIN", "<stdint.h>"}, {"INT16_MIN", "<stdint.h>"},
    {"INT32_MIN", "<stdint.h>"}, {"INT64_MIN", "<stdint.h>"},
    {"UINT8_MAX", "<stdint.h>"}, {"UINT16_MAX", "<stdint.h>"},
    {"UINT32_MAX", "<stdint.h>"}, {"UINT64_MAX", "<stdint.h>"},
    {"int8_t", "<stdint.h>"}, {"int16_t", "<stdint.h>"},
    {"int32_t", "<stdint.h>"}, {"int64_t", "<stdint.h>"},
    {"intptr_t", "<stdint.h>"}, {"uint8_t", "<stdint.h>"},
    {"uint16_t", "<stdint.h>"}, {"uint32_t", "<stdint.h>"},
    {"uint64_t", "<stdint.h>"}, {"uintptr_t", "<stdint.h>"},
    {"asctime", "<time.h>"}, {"clock", "<time.h>"},
    {"clock_t", "<time.h>"}, {"ctime", "<time.h>"},
    {"difftime", "<time.h>"}, {"gmtime", "<time.h>"},
    {"localtime", "<time.h>"}, {"mktime", "<time.h>"},
    {"strftime", "<time.h>"}, {"time", "<time.h>"},
    {"time_t", "<time.h>"}, {"tm", "<time.h>"},
    {"WCHAR_MAX", "<wchar.h>"}, {"WCHAR_MIN", "<wchar.h>"},
    {"bool", "<stdbool.h>"}, {"true", "<stdbool.h>"},
    {"false", "<stdbool.h>"}
};

static const char *std_header(const char *name)
{
    size_t k;
    for (k = 0; k < sizeof std_names / sizeof *std_names; k++)
        if (!strcmp(std_names[k].name, name))
            return std_names[k].header;
    return NULL;
}

/* Where gcc suggests adding an #include: the line after the last #include
 * of the main file before loc, else its start. */
static SrcLoc include_loc(Checker *c, SrcLoc loc)
{
    uint32_t k, n = srcmgr_nfiles(c->sm), line, nl, lastinc = 0;
    SrcFile *f = NULL;
    for (k = 0; k < n; k++) {
        SrcFile *g = srcmgr_file(c->sm, k);
        if (g && g->kind == SF_USER && !g->system_header) {
            f = g;
            break;
        }
    }
    if (!f)
        return loc;
    srcmgr_linecol(f, f->base + f->size, &nl, &line);
    if (srcmgr_file_of(c->sm, loc) == f)
        srcmgr_linecol(f, loc, &nl, &line);
    for (line = 1; line < nl; line++) {
        uint32_t len, j = 0;
        const char *s = srcmgr_line_text(f, line, &len);
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (j >= len || s[j] != '#')
            continue;
        j++;
        while (j < len && (s[j] == ' ' || s[j] == '\t'))
            j++;
        if (len - j >= 7 && !memcmp(s + j, "include", 7))
            lastinc = line;
    }
    return srcmgr_loc_of(f, lastinc + 1, 1);
}

/* The location of a note suggesting header hdr: gcc puts the first one per
 * header where the #include would go (with a fix-it), later ones at the
 * use. */
static SrcLoc header_note_loc(Checker *c, SrcLoc loc, const char *hdr)
{
    static const char *const known[] = {
        "<assert.h>", "<complex.h>", "<ctype.h>", "<inttypes.h>", "<math.h>",
        "<stdarg.h>", "<stdbool.h>", "<stddef.h>", "<stdint.h>", "<stdio.h>",
        "<stdlib.h>", "<string.h>", "<time.h>", "<wchar.h>", "<errno.h>",
        "<limits.h>", "<float.h>", "<setjmp.h>", "<signal.h>", "<locale.h>"
    };
    size_t k;
    for (k = 0; k < sizeof known / sizeof *known; k++)
        if (!strcmp(known[k], hdr)) {
            uint64_t bit = UINT64_C(1) << k;
            bool seen = (c->hdr_noted & bit) != 0;
            c->hdr_noted |= bit;
            return seen ? loc : include_loc(c, loc);
        }
    return include_loc(c, loc);
}

static void header_note(Checker *c, Diagnostic *d, SrcLoc loc,
                        const char *name, const char *hdr)
{
    cnote(c, d, header_note_loc(c, loc, hdr),
          "'%s' is defined in header '%s'; did you forget to '#include %s'?",
          name, hdr, hdr);
}

#include "cbuiltin_tab.h"

typedef struct BTab {
    const char *name, *hdr;
    unsigned char mismatch;
    const char *sig;
    unsigned char gnu;       /* a built-in only outside -std=c99 */
} BTab;

/* any: the __builtin_ spelling, which exists in every mode */
static const BTab *bt_find(Checker *c, const char *name, bool any)
{
    size_t lo = 0, hi = sizeof cbuiltin_tab / sizeof *cbuiltin_tab;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, cbuiltin_tab[mid].name);
        if (!r)
            return cbuiltin_tab[mid].gnu && !c->opt.gnu && !any
                       ? NULL : (const BTab *)&cbuiltin_tab[mid];
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* A type as gcc prints it ("const char *", "long unsigned int"). */
static TypeId bt_type(Checker *c, const char *s, size_t n)
{
    TypeId t;
    bool cst = false;
    size_t k;
    while (n && s[n - 1] == ' ')
        n--;
    while (n && *s == ' ')
        s++, n--;
    if (n > 6 && !strncmp(s, "const ", 6)) {
        cst = true;
        s += 6;
        n -= 6;
    }
    if (n && s[n - 1] == '*') {
        TypeId b = bt_type(c, s, n - 1);
        if (cst)
            b = type_qual(b, TQ_CONST);
        return type_ptr(TT, b);
    }
    {
        static const struct { const char *n; TypeKind k; } base[] = {
            {"void", TY_VOID}, {"int", TY_INT}, {"char", TY_CHAR},
            {"long int", TY_LONG}, {"long long int", TY_LLONG},
            {"long unsigned int", TY_ULONG}, {"double", TY_DOUBLE},
            {"float", TY_FLOAT}, {"long double", TY_LDOUBLE}
        };
        t = ERRT;
        for (k = 0; k < sizeof base / sizeof *base; k++)
            if (strlen(base[k].n) == n && !strncmp(base[k].n, s, n))
                t = TYPE_MK(base[k].k, 0);
        if (is_err(c, t)) {
            if (n > 9 && !strncmp(s, "_Complex ", 9))
                t = type_complex(TT, bt_type(c, s + 9, n - 9));
            else if (n == 13 && !strncmp(s, "__va_list_tag", 13))
                t = type_base(TT, type_canon(TT, TT->va_list));
        }
    }
    if (cst)
        t = type_qual(t, TQ_CONST);
    return t;
}

/* The function type of a table entry. */
static TypeId bt_func_type(Checker *c, const BTab *b)
{
    const char *p = strchr(b->sig, '|'), *q;
    TypeId ps[8], ret;
    uint32_t n = 0, flags = 0;
    if (!p)
        return ERRT;
    ret = bt_type(c, b->sig, (size_t)(p - b->sig));
    p++;
    if (!strcmp(p, "?"))
        return type_func(TT, ret, NULL, 0, TF_NOPROTO);
    while (*p && n < 8) {
        q = strchr(p, '|');
        if (!q)
            q = p + strlen(p);
        if (q - p == 3 && !strncmp(p, "...", 3))
            flags |= TF_VARIADIC;
        else
            ps[n++] = bt_type(c, p, (size_t)(q - p));
        p = *q ? q + 1 : q;
    }
    return type_func(TT, ret, ps, n, flags);
}

/* __builtin_{s,u}{add,sub,mul}{,l,ll}_overflow: _Bool (T, T, T *). */
static TypeId overflow_func_type(Checker *c, const char *b)
{
    static const char *const op[] = {"add", "sub", "mul"};
    bool sg = *b == 's';
    TypeKind k;
    TypeId t, ps[3];
    size_t k2;
    int found = 0;
    if (*b != 's' && *b != 'u')
        return ERRT;
    for (k2 = 0; k2 < 3; k2++)
        if (!strncmp(b + 1, op[k2], 3))
            found = 1;
    if (!found)
        return ERRT;
    b += 4;
    if (!strncmp(b, "ll_overflow", 12))
        k = sg ? TY_LLONG : TY_ULLONG;
    else if (!strncmp(b, "l_overflow", 11))
        k = sg ? TY_LONG : TY_ULONG;
    else if (!strncmp(b, "_overflow", 10))
        k = sg ? TY_INT : TY_UINT;
    else
        return ERRT;
    t = TYPE_MK(k, 0);
    ps[0] = ps[1] = t;
    ps[2] = type_ptr(TT, t);
    return type_func(TT, TYPE_B(BOOL), ps, 3, 0);
}

/* gcc's builtin type declarations, candidates for misspelled names. */
static const char *const builtin_type_names[] = {
    "int", "char", "long int", "unsigned int", "long unsigned int",
    "long long int", "long long unsigned int", "short int",
    "short unsigned int", "signed char", "unsigned char", "__int128",
    "__int128 unsigned", "float", "double", "long double", "_Float16",
    "_Float32", "_Float64", "_Float128", "_Float32x", "_Float64x",
    "complex int", "complex float", "complex double", "complex long double",
    "void", "_Bool", "__bf16"
};

/* lookup_name_fuzzy: a visible name close to goal.  functions: only
 * functions and pointers to functions (an implicit declaration). */
static const char *fuzzy_name(Checker *c, const char *goal, bool functions)
{
    Best b;
    size_t k;
    bool res_ok = goal[0] == '_';
    best_init(&b, goal, &c->fuzzy_work);
    for (k = c->log.len; k-- > 0;) {
        const Bind *bd = &c->log.data[k];
        const char *s;
        if (c->top[bd->ns][bd->ident] == 0)
            continue;
        if (c->fuzzy_work > SC_BUDGET)
            return NULL;
        s = cident(c, bd->ident);
        if (!res_ok && reserved_name(s))
            continue;
        if (bd->ns == NS_ORD) {
            CSym *sym = csym(c, bd->ref);
            if (sym->flags & CSF_IMPLICIT)
                continue;
            if ((sym->flags & CSF_ERROR) && is_err(c, sym->ty))
                continue;
            if (functions) {
                if (sym->kind == CS_FUNC)
                    ;
                else if (sym->kind == CS_OBJ && is_ptr(c, sym->ty) &&
                         is_func(c, pointee(c, sym->ty)))
                    ;
                else
                    continue;
            }
        } else if (functions) {
            continue;
        }
        best_consider_n(&b, s, ident_by_id(c->in, bd->ident)->len);
    }
    if (!functions)
        for (k = sizeof builtin_type_names / sizeof *builtin_type_names;
             k-- > 0;)
            if (res_ok || !reserved_name(builtin_type_names[k]))
                best_consider(&b, builtin_type_names[k]);
    return best_get(&b);
}

/* ---- printing expressions (gcc's %E) ---------------------------------------- */

static void print_ival(Checker *c, StrBuf *sb, TypeId t, uint64_t v)
{
    if (is_int(c, t) && is_signed(c, t))
        sb_printf(sb, "%lld", (long long)(int64_t)v);
    else
        sb_printf(sb, "%llu", (unsigned long long)v);
}

static int bin_prec(int op)
{
    switch (op) {
    case P_COMMA: return PR_COMMA;
    case P_OROR: return PR_LOR;
    case P_ANDAND: return PR_LAND;
    case P_PIPE: return PR_OR;
    case P_CARET: return PR_XOR;
    case P_AMP: return PR_AND;
    case P_EQEQ: case P_NE: return PR_EQ;
    case P_LT: case P_GT: case P_LE: case P_GE: return PR_REL;
    case P_SHL: case P_SHR: return PR_SHIFT;
    case P_PLUS: case P_MINUS: return PR_ADD;
    default: return PR_MUL;
    }
}

/* Does node i print as its value (an INTEGER_CST)? */
static bool prints_value(Checker *c, uint32_t i)
{
    return has_ival(c, i) &&
           (c->ck[i] == K_ICE || (c->ef[i] & (EF_CST | EF_NOPCST)));
}

static bool pfloat(Checker *c, StrBuf *sb, const char *s, size_t len);
static void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec);
static void pcond_arm(Checker *c, StrBuf *sb, uint32_t arm, TypeId rt, int prec);
static void pcond_test(Checker *c, StrBuf *sb, uint32_t cond);

/* Operand i converted to type t (an implicit conversion prints as a
 * cast). */
static void pconv(Checker *c, StrBuf *sb, uint32_t i, TypeId t, int prec)
{
    i = strip_paren(c, i);
    if (i == NO_NODE)
        return;
    if (is_arith(c, t) && !is_err(c, t) && mainv(c, rvt(c, i)) != mainv(c, t)) {
        if (prints_value(c, i) && is_int(c, t)) {
            print_ival(c, sb, t, cexpr_trunc(c, t, c->cv[i]));
            return;
        }
        if (prec > PR_UNARY)
            sb_putc(sb, '(');
        sb_putc(sb, '(');
        type_print(TT, sb, mainv(c, t));
        sb_putc(sb, ')');
        pexpr(c, sb, i, PR_UNARY);
        if (prec > PR_UNARY)
            sb_putc(sb, ')');
        return;
    }
    pexpr(c, sb, i, prec);
}

/* gcc builds p[i] on a pointer as *(p + i * size) and prints that tree:
 * the byte offset of a constant index, (sizetype)(...) of a variable one.
 * Returns the base and index nodes when node i is such a subscript and its
 * spelling is one cereal reproduces (no folded sums of a signed index). */
static bool lowered_sub(Checker *c, uint32_t i, uint32_t *base, uint32_t *idx,
                        uint64_t *size)
{
    uint32_t k[3], b, x, sx;
    TypeId bt, pt;
    bool ov;
    if (ntag(c, i) != N_INDEX || nkids(c, i, k, 3) < 2)
        return false;
    b = strip_paren(c, k[0]);
    x = strip_paren(c, k[1]);
    if (b == NO_NODE || x == NO_NODE)
        return false;
    bt = type_canon(TT, c->ty[b]);
    if (type_ckind(TT, bt) != TY_PTR) {
        uint32_t t = b;
        b = x;
        x = t;
        bt = type_canon(TT, c->ty[b]);
        if (type_ckind(TT, bt) != TY_PTR)
            return false;
    }
    if (!is_int(c, rvt(c, x)))
        return false;
    pt = type_canon(TT, type_base(TT, bt));
    *size = type_size(TT, pt, &ov);
    if (!ov || !*size || tkind(c, pt) == TY_VLA)
        return false;
    if (c->ck[x] != K_ICE) {
        sx = x;
        /* sums and differences of a signed index are folded by gcc */
        if (ntag(c, sx) == N_BINARY && (npunct(c, sx) == P_PLUS ||
                                        npunct(c, sx) == P_MINUS) &&
            is_signed(c, rvt(c, sx))) {
            uint32_t sk[3];
            if (nkids(c, sx, sk, 3) >= 2 &&
                c->ck[strip_paren(c, sk[1])] == K_ICE &&
                (npunct(c, sx) != P_PLUS ||
                 cexpr_sval(c, strip_paren(c, sk[1])) <= 0 ||
                 c->ck[strip_paren(c, sk[0])] == K_ICE ||
                 type_size(TT, rvt(c, sx), &ov) != 4))
                return false;
        }
        if (ntag(c, sx) == N_COND || has_ival(c, sx))
            return false;
    }
    *base = b;
    *idx = x;
    return true;
}

static void plowered(Checker *c, StrBuf *sb, uint32_t b, uint32_t x,
                     uint64_t size)
{
    char num[32];
    sb_putc(sb, '*');
    if (c->ck[x] == K_ICE) {
        int64_t off = cexpr_sval(c, x) * (int64_t)size;
        if (off == 0) {
            pexpr(c, sb, b, PR_UNARY);
            return;
        }
        sb_putc(sb, '(');
        pexpr(c, sb, b, PR_ADD);
        snprintf(num, sizeof num, " + %lld)", (long long)off);
        sb_puts(sb, num);
        return;
    }
    sb_putc(sb, '(');
    pexpr(c, sb, b, PR_ADD);
    sb_puts(sb, " + ");
    {
        uint32_t sk[3], sx = strip_paren(c, x);
        if (ntag(c, sx) == N_BINARY && npunct(c, sx) == P_PLUS &&
            is_signed(c, rvt(c, sx)) && nkids(c, sx, sk, 3) >= 2 &&
            c->ck[strip_paren(c, sk[1])] == K_ICE) {
            /* gcc distributes: ((sizetype)i + 1) * size */
            snprintf(num, sizeof num, " + %lld)", (long long)
                     cexpr_sval(c, strip_paren(c, sk[1])));
            sb_puts(sb, "((sizetype)");
            pexpr(c, sb, sk[0], PR_UNARY);
            sb_puts(sb, num);
            if (size != 1) {
                snprintf(num, sizeof num, " * %llu", (unsigned long long)size);
                sb_puts(sb, num);
            }
            sb_putc(sb, ')');
            return;
        }
    }
    sb_puts(sb, "(sizetype)");
    {
        TypeId xt = type_canon(TT, rvt(c, x));
        bool wide_u = type_size(TT, xt, &(bool){0}) == 8 && !is_signed(c, xt);
        if (size == 1) {
            pexpr(c, sb, x, PR_UNARY);
        } else if (wide_u) {
            sb_putc(sb, '(');
            pexpr(c, sb, x, PR_MUL);
            snprintf(num, sizeof num, " * %llu)", (unsigned long long)size);
            sb_puts(sb, num);
        } else {
            sb_puts(sb, "((long unsigned int)");
            pexpr(c, sb, x, PR_UNARY);
            snprintf(num, sizeof num, " * %llu)", (unsigned long long)size);
            sb_puts(sb, num);
        }
    }
    sb_putc(sb, ')');
}

/* Whether the lvalue at node i is const through its own type or through a
 * const-qualified enclosing object (gcc's tree gives such members const
 * types). */
static bool const_through(Checker *c, uint32_t i)
{
    uint32_t b;
    while (i != NO_NODE && ntag(c, i) == N_PAREN)
        i = first_child(c, i);
    if (i == NO_NODE)
        return false;
    if (tquals(c, c->ty[i]) & TQ_CONST)
        return true;
    if (ntag(c, i) == N_MEMBER_EXPR) {
        b = first_child(c, i);
        if (b == NO_NODE)
            return false;
        if ((c->nodes[i].flags & NF_ARROW) && is_ptr(c, rvt(c, b)))
            return tquals(c, pointee(c, rvt(c, b))) & TQ_CONST;
        return const_through(c, b);
    }
    if (ntag(c, i) == N_INDEX) {
        b = first_child(c, i);
        return b != NO_NODE && is_array(c, c->ty[b]) && const_through(c, b);
    }
    return false;
}

/* The condition of `?:` as gcc prints it after truth-value conversion:
 * `(c) != 0`, `(d) != (0.0)`, `(a) < (b)`, `(c) == 0` for `!c`. */
static void pcond_test(Checker *c, StrBuf *sb, uint32_t cond)
{
    uint32_t e = strip_paren(c, cond), k[3];
    TypeId t;
    int op;
    if (e == NO_NODE || has_ival(c, e) || prints_value(c, e) ||
        c->ck[e] == K_ERR || is_err(c, c->ty[e])) {
        pexpr(c, sb, cond, PR_LOR);
        return;
    }
    t = rvt(c, e);
    if (ntag(c, e) == N_BINARY && nkids(c, e, k, 3) == 2) {
        op = npunct(c, e);
        if (op == P_ANDAND || op == P_OROR) {
            pcond_test(c, sb, k[0]);
            sb_puts(sb, op == P_ANDAND ? " && " : " || ");
            pcond_test(c, sb, k[1]);
            return;
        }
        if (bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL) {
            bool ar = is_arith(c, rvt(c, k[0])) && is_arith(c, rvt(c, k[1]));
            if (ar) {
                sb_putc(sb, '(');
                pexpr(c, sb, k[0], PR_COMMA);
                sb_putc(sb, ')');
            } else {
                pexpr(c, sb, k[0], bin_prec(op));
            }
            sb_printf(sb, " %s ", punct_spelling[op]);
            if (ar) {
                sb_putc(sb, '(');
                pexpr(c, sb, k[1], PR_COMMA);
                sb_putc(sb, ')');
            } else {
                pexpr(c, sb, k[1], bin_prec(op) + 1);
            }
            return;
        }
    }
    if (ntag(c, e) == N_UNARY && npunct(c, e) == P_BANG &&
        nkids(c, e, k, 3) == 1 && is_arith(c, rvt(c, k[0]))) {
        sb_putc(sb, '(');
        pexpr(c, sb, k[0], PR_COMMA);
        sb_puts(sb, ") == 0");
        return;
    }
    if (is_ptr(c, t)) {
        pexpr(c, sb, e, PR_EQ);
        sb_puts(sb, " != 0");
    } else if (is_arith(c, t)) {
        sb_putc(sb, '(');
        if (tkind(c, t) == TY_CHAR || tkind(c, t) == TY_SCHAR) {
            sb_puts(sb, "(signed char)");
            pexpr(c, sb, e, PR_UNARY);
        } else if (tkind(c, t) == TY_UCHAR) {
            sb_puts(sb, "(unsigned char)");
            pexpr(c, sb, e, PR_UNARY);
        } else {
            pexpr(c, sb, e, PR_COMMA);
        }
        sb_puts(sb, is_flt(c, t) ? ") != (0.0)" : ") != 0");
    } else {
        pexpr(c, sb, cond, PR_LOR);
    }
}

/* An arm of `?:` with a pointer result: gcc prints it converted to the
 * result type (an array arm as the address of the whole array). */
static void pcond_arm(Checker *c, StrBuf *sb, uint32_t arm, TypeId rt, int prec)
{
    TypeId at = c->ty[arm];
    bool arr = is_array(c, at);
    if (is_ptr(c, rt) && (arr || is_ptr(c, at)) && (arr || at != rt)) {
        sb_putc(sb, '(');
        type_print(TT, sb, rt);
        sb_putc(sb, ')');
        if (arr)
            sb_putc(sb, '&');
        pexpr(c, sb, arm, PR_UNARY);
        return;
    }
    pexpr(c, sb, arm, prec);
}

static void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec)
{
    uint32_t k[3], n, lb, lx;
    uint64_t lsz;
    int my = PR_PRIMARY, op;
    size_t len;
    const char *s;
    i = strip_paren(c, i);
    if (i == NO_NODE)
        return;
    if (prints_value(c, i)) {
        print_ival(c, sb, rvt(c, i), c->cv[i]);
        return;
    }
    n = nkids(c, i, k, 3);
    switch (ntag(c, i)) {
    case N_BINARY: my = bin_prec(npunct(c, i)); break;
    case N_ASSIGN: my = PR_ASSIGN; break;
    case N_COND: my = PR_COND; break;
    case N_CAST: case N_UNARY: case N_SIZEOF_EXPR: case N_SIZEOF_TYPE:
    case N_ALIGNOF_EXPR: case N_ALIGNOF_TYPE:
        my = PR_UNARY;
        break;
    case N_INDEX: case N_CALL: case N_MEMBER_EXPR: case N_POSTFIX:
        my = ntag(c, i) == N_INDEX && lowered_sub(c, i, &lb, &lx, &lsz)
                 ? PR_UNARY : PR_POSTFIX;
        break;
    default: break;
    }
    if (my < prec)
        sb_putc(sb, '(');
    switch (ntag(c, i)) {
    case N_IDENT: case N_NUMBER: case N_CHAR:
        s = ttext(c, c->nodes[i].tok, &len);
        if (ntag(c, i) == N_NUMBER && pfloat(c, sb, s, len))
            break;
        sb_putn(sb, s, len);
        break;
    case N_STRING: {
        uint32_t t;
        for (t = 0; t < (c->nodes[i].aux ? c->nodes[i].aux : 1u); t++) {
            if (t)
                sb_putc(sb, ' ');
            s = ttext(c, c->nodes[i].tok + t, &len);
            sb_putn(sb, s, len);
        }
        break;
    }
    case N_BINARY:
        if (n < 2)
            break;
        op = npunct(c, i);
        if (op == P_COMMA || op == P_ANDAND || op == P_OROR) {
            pexpr(c, sb, k[0], my);
            sb_puts(sb, op == P_COMMA ? ", " : op == P_ANDAND ? " && "
                                                              : " || ");
            pexpr(c, sb, k[1], my + 1);
        } else {
            TypeId t0 = rvt(c, i), t1 = rvt(c, i);
            if (op == P_SHL || op == P_SHR) {
                t0 = promoted(c, k[0]);
                t1 = promoted(c, k[1]);
            } else if (bin_prec(op) == PR_EQ || bin_prec(op) == PR_REL) {
                t0 = t1 = TYPE_B(ERROR);
                if (is_arith(c, rvt(c, k[0])) && is_arith(c, rvt(c, k[1])))
                    t0 = t1 = common_type(c, promoted(c, k[0]),
                                          promoted(c, k[1]));
            } else if (!is_arith(c, t0)) {
                t0 = t1 = TYPE_B(ERROR);
            }
            pconv(c, sb, k[0], t0, my);
            sb_printf(sb, " %s ", punct_spelling[op]);
            pconv(c, sb, k[1], t1, my + 1);
        }
        break;
    case N_ASSIGN:
        if (n < 2)
            break;
        pexpr(c, sb, k[0], PR_UNARY);
        sb_printf(sb, " %s ", punct_spelling[npunct(c, i)]);
        pexpr(c, sb, k[1], PR_ASSIGN);
        break;
    case N_COND:
        if (n < 2)
            break;
        pcond_test(c, sb, k[0]);
        sb_puts(sb, " ? ");
        if (n == 3)
            pcond_arm(c, sb, k[1], c->ty[i], PR_COMMA);
        sb_puts(sb, " : ");
        pcond_arm(c, sb, k[n - 1], c->ty[i], PR_COND);
        break;
    case N_CAST:
        if (n < 2)
            break;
        if (mainv(c, c->ty[i]) == mainv(c, rvt(c, k[1]))) {
            pexpr(c, sb, k[1], PR_UNARY);
            break;
        }
        sb_putc(sb, '(');
        type_print(TT, sb, c->ty[i]);
        sb_putc(sb, ')');
        pexpr(c, sb, k[1], PR_UNARY);
        break;
    case N_UNARY:
        if (n < 1)
            break;
        op = npunct(c, i);
        if (op == P_STAR && c->ty[k[0]] != ERRT && is_array(c, c->ty[k[0]])) {
            /* gcc prints a deref of an array as *(T *)&array */
            uint32_t x = k[0];
            TypeId et = elem_of(c, c->ty[x]);
            while (ntag(c, x) == N_UNARY && npunct(c, x) == P_STAR &&
                   nkids(c, x, k, 3) >= 1 && is_array(c, c->ty[k[0]]))
                x = k[0];
            while (is_array(c, et))
                et = elem_of(c, et);
            if (const_through(c, x))
                et = type_qual(et, TQ_CONST);
            sb_puts(sb, "*(");
            type_print(TT, sb, et);
            sb_puts(sb, " *)&");
            pexpr(c, sb, x, PR_UNARY);
            break;
        }
        if (op != P_NONE) {
            sb_puts(sb, punct_spelling[op]);
        } else {
            s = ttext(c, c->nodes[i].tok, &len);
            sb_putn(sb, s, len);
            sb_putc(sb, ' ');
        }
        if ((op == P_MINUS || op == P_PLUS || op == P_TILDE) &&
            is_arith(c, rvt(c, i)))
            pconv(c, sb, k[0], rvt(c, i), PR_UNARY);
        else
            pexpr(c, sb, k[0], PR_UNARY);
        break;
    case N_POSTFIX:
        if (n < 1)
            break;
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_puts(sb, punct_spelling[npunct(c, i)]);
        break;
    case N_SIZEOF_EXPR: case N_ALIGNOF_EXPR:
        s = ttext(c, c->nodes[i].tok, &len);
        sb_putn(sb, s, len);
        sb_putc(sb, ' ');
        if (n)
            pexpr(c, sb, k[0], PR_UNARY);
        break;
    case N_INDEX:
        if (n < 2)
            break;
        if (lowered_sub(c, i, &lb, &lx, &lsz)) {
            plowered(c, sb, lb, lx, lsz);
            break;
        }
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_putc(sb, '[');
        pexpr(c, sb, k[1], PR_COMMA);
        sb_putc(sb, ']');
        break;
    case N_MEMBER_EXPR:
        if (n < 1)
            break;
        pexpr(c, sb, k[0], PR_POSTFIX);
        sb_puts(sb, c->nodes[i].flags & NF_ARROW ? "->" : ".");
        if (!(c->nodes[i].flags & NF_ERROR)) {
            s = ttext(c, c->nodes[i].tok, &len);
            sb_putn(sb, s, len);
        }
        break;
    case N_CALL: {
        uint32_t a[64], m = nkids(c, i, a, 64), j;
        if (m == 0)
            break;
        pexpr(c, sb, a[0], PR_POSTFIX);
        sb_puts(sb, " (");
        for (j = 1; j < m && j < 64; j++) {
            if (j > 1)
                sb_puts(sb, ", ");
            pexpr(c, sb, a[j], PR_ASSIGN);
        }
        sb_putc(sb, ')');
        break;
    }
    default:
        sb_puts(sb, "...");
        break;
    }
    if (my < prec)
        sb_putc(sb, ')');
}

/* A floating literal as gcc prints it (real_to_decimal at the type's full
 * digit count, trailing zeros dropped: 'd.ddde+X', and the f/l suffix);
 * false for anything else. */
static bool pfloat(Checker *c, StrBuf *sb, const char *s, size_t len)
{
    char t[96], o[64], *e, *ep, *z;
    long double v;
    int ex;
    char sfx = 0;
    bool hex = len > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
    (void)c;
    if (len >= sizeof t || len == 0)
        return false;
    memcpy(t, s, len);
    t[len] = 0;
    if (t[len - 1] == 'f' || t[len - 1] == 'F' || t[len - 1] == 'l' ||
        t[len - 1] == 'L') {
        if (!hex || strpbrk(t, "pP")) {
            sfx = (char)tolower((unsigned char)t[len - 1]);
            t[--len] = 0;
        }
    }
    if (!strpbrk(t, hex ? ".pP" : ".eE"))
        return false;
    v = strtold(t, &e);
    if (*e)
        return false;
    if (sfx == 'f')
        snprintf(o, sizeof o, "%.8e", (double)(float)v);
    else if (sfx == 'l')
        snprintf(o, sizeof o, "%.20Le", v);
    else
        snprintf(o, sizeof o, "%.16e", (double)v);
    ep = strchr(o, 'e');
    ex = atoi(ep + 1);
    *ep = 0;
    for (z = ep - 1; z > o && *z == '0' && z[-1] != '.'; z--)
        *z = 0;
    sb_puts(sb, o);
    sb_printf(sb, "e%c%d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
    if (sfx)
        sb_putc(sb, sfx);
    return true;
}

/* %E of node i, in a buffer valid until the next call (two rotate). */
static const char *estr(Checker *c, uint32_t i)
{
    StrBuf *sb = &c->esb[c->enext++ & 1];
    sb->len = 0;
    pexpr(c, sb, i, PR_COMMA);
    return sb_cstr(sb);
}

const char *cexpr_str(Checker *c, uint32_t i)
{
    return estr(c, i);
}

static const char *vstr(Checker *c, TypeId t, uint64_t v)
{
    char *b = c->vbuf[c->vnext++ & 1];
    if (is_int(c, t) && is_signed(c, t))
        snprintf(b, sizeof c->vbuf[0], "%lld", (long long)(int64_t)v);
    else
        snprintf(b, sizeof c->vbuf[0], "%llu", (unsigned long long)v);
    return b;
}

/* ---- shared checks ----------------------------------------------------------- */

/* The element type of an array type, with the array's qualifiers. */
static TypeId elem_of(Checker *c, TypeId t)
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
static SrcLoc expr_loc(Checker *c, uint32_t i)
{
    switch (ntag(c, i)) {
    case N_PAREN:
        return i > 0 && c->nodes[i].size > 1 ? expr_loc(c, i - 1)
                                             : cnode_loc(c, i);
    case N_CALL:
        return first_loc(c, i);
    case N_MEMBER_EXPR:
        return ctok_loc(c, c->nodes[i].tok - 1);
    default:
        return cnode_loc(c, i);
    }
}

/* c_incomplete_type_error (value: a variable or parameter, else
 * NO_NODE). */
static void incomplete_error(Checker *c, SrcLoc loc, uint32_t value, TypeId t)
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
                cerror(c, loc, "invalid use of array with unspecified bounds");
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

static bool complete(Checker *c, TypeId t)
{
    return type_is_complete(TT, t);
}

/* The size in bytes of what a pointer of type pt points to, for
 * arithmetic (void and functions: 1). */
static uint64_t elem_size(Checker *c, TypeId pt)
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
static bool ptr_arith_ok(Checker *c, uint32_t i, SrcLoc loc, TypeId pt)
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
static void addr_rvalue(Checker *c, uint32_t i)
{
    if ((c->ef[i] & EF_ADDRLV) && (is_array(c, c->ty[i]) ||
                                    is_func(c, c->ty[i])) &&
        tkind(c, c->ty[i]) != TY_VLA)
        c->ck[i] = K_ADDR;
}

/* ---- literals -------------------------------------------------------------- */

static void lit_report(Checker *c, uint32_t i, const Lit *l)
{
    SrcLoc loc = cnode_loc(c, i);
    if (!l->msg[0])
        return;
    switch (l->level) {
    case 2:
        cerror(c, loc, "%s", l->msg);
        break;
    case 1:
        if (l->flags & (LIT_TOO_LARGE | LIT_UNSIGNED_WARN))
            cpedwarn(c, loc, l->id, "%s", l->msg);
        else
            cwarn(c, loc, l->id, "%s", l->msg);
        break;
    default:
        /* libcpp's pedwarns carry no option tag; interpret_float's
         * non-standard suffix pedwarn is c-family's: [-Wpedantic] at
         * input_location */
        if (c->opt.pedantic && !cexpr_in_extension(c, i)) {
            if (l->id && !strcmp(l->id, "inputloc"))
                cpedwarn(c, cinput_loc(c, c->nodes[i].tok), "pedantic", "%s",
                         l->msg);
            else
                cpedwarn(c, loc, "", "%s", l->msg);
        }
        break;
    }
}

static void e_number(Checker *c, uint32_t i)
{
    size_t len;
    const char *s = ttext(c, c->nodes[i].tok, &len);
    Lit l;
    TypeId t;
    lit_number(c->tgt, s, len, &l);
    lit_report(c, i, &l);
    if (l.flags & LIT_BAD) {
        set_err(c, i);
        return;
    }
    t = TYPE_MK(l.ty, 0);
    if (l.flags & LIT_IMAGINARY) {
        c->ty[i] = type_complex(TT, t);   /* its value: not tracked */
        return;
    }
    c->ty[i] = t;
    if (l.flags & LIT_FLOAT) {
        long double v = fround(c, t, l.f);
        size_t k;
        bool nonzero = false, hex = len > 1 && s[0] == '0' &&
                                    (s[1] == 'x' || s[1] == 'X');
        if (l.ty >= TY_DEC32 && l.ty <= TY_DEC128)
            return;
        for (k = hex ? 2 : 0; k < len; k++) {
            char ch = s[k];
            if ((!hex && (ch == 'e' || ch == 'E')) ||
                (hex && (ch == 'p' || ch == 'P')))
                break;
            if ((ch >= '1' && ch <= '9') ||
                (hex && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))))
                nonzero = true;
        }
        if (isinf(v))
            cwarn(c, cinput_loc(c, c->nodes[i].tok), "overflow",
                  "floating constant exceeds range of %s", type_q(TT, t));
        else if (v == 0 && nonzero)
            cwarn(c, cinput_loc(c, c->nodes[i].tok), "overflow",
                  "floating constant truncated to zero");
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, v);
        c->ef[i] = EF_REALCST;
        return;
    }
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, t, l.v);
    c->ef[i] = EF_INTOPS;
    if (len && s[0] >= '1' && s[0] <= '9')
        c->ef[i] |= EF_DECIMAL;
    else if (len == 1 || (len > 1 && s[1] != 'x' && s[1] != 'X' &&
                          s[1] != 'b' && s[1] != 'B' &&
                          !(s[1] >= '0' && s[1] <= '9')))
        c->ef[i] |= EF_DECIMAL;   /* 0, 0u */
}

static void e_char(Checker *c, uint32_t i)
{
    size_t len;
    const char *s = ttext(c, c->nodes[i].tok, &len);
    Lit l;
    lit_char(c->tgt, s, len, &l);
    lit_report(c, i, &l);
    if (l.flags & LIT_BAD) {
        set_err(c, i);
        return;
    }
    c->ty[i] = TYPE_MK(l.ty, 0);
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, c->ty[i], l.v);
    c->ef[i] = EF_INTOPS;
}

static void e_string(Checker *c, uint32_t i)
{
    uint32_t np = c->nodes[i].aux ? c->nodes[i].aux : 1, k;
    int prefix = 0;
    uint64_t units = 0;
    unsigned width;
    TypeKind ek = TY_CHAR;
    for (k = 0; k < np; k++) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        int p = lit_str_prefix(s, len);
        if (p && prefix && p != prefix) {
            /* gcc's lexer reports it twice, at the lookahead's line */
            SrcLoc il = cinput_loc(c, c->nodes[i].tok + np);
            cerror(c, il, "unsupported non-standard concatenation of string "
                   "literals");
            cerror(c, il, "unsupported non-standard concatenation of string "
                   "literals");
            set_err(c, i);
            return;
        }
        if (p)
            prefix = p;
    }
    if (prefix == '8')
        prefix = 0;
    switch (prefix) {
    case 'L': ek = c->tgt->wchar_type; break;
    case 'u': ek = c->tgt->char16_type; break;
    case 'U': ek = c->tgt->char32_type; break;
    default: break;
    }
    width = c->tgt->size[ek];
    for (k = 0; k < np; k++) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok + k, &len);
        lit_str_units(s, len, width, &units);
    }
    if (units > 4095 && !cexpr_in_extension(c, i))
        cpedwarn(c, cinput_loc(c, c->nodes[i].tok + np), "overlength-strings",
                 "string length '%llu' is greater than the length '%d' ISO "
                 "C99 compilers are required to support",
                 (unsigned long long)units, 4095);
    c->ty[i] = type_array(TT, TYPE_MK(ek, 0), units + 1);
    c->ef[i] = EF_LVALUE | EF_STRING | EF_ADDRLV;
    c->cb[i] = CB_NODE | i;
    c->ck[i] = K_ADDR;
}

/* ---- identifiers ------------------------------------------------------------- */

static bool builtin_name(const char *s)
{
    return !strncmp(s, "__builtin_", 10) || !strncmp(s, "__sync_", 7) ||
           !strncmp(s, "__atomic_", 9);
}

/* Is node i the callee of a call (not parenthesized)? */
static bool is_callee(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    return p != NO_NODE && ntag(c, p) == N_CALL && first_child(c, p) == i;
}

static void old_decl_note(Checker *c, Diagnostic *d, const CSym *s)
{
    const char *name = cident(c, s->name);
    if (s->flags & CSF_DEFINED)
        cnote(c, d, s->def_loc ? s->def_loc : s->loc,
              "previous definition of '%s' with type %s", name,
              type_q(TT, s->ty));
    else if (s->flags & CSF_IMPLICIT)
        cnote(c, d, s->loc, "previous implicit declaration of '%s' with type %s",
              name, type_q(TT, s->ty));
    else
        cnote(c, d, s->loc, "previous declaration of '%s' with type %s", name,
              type_q(TT, s->ty));
}

/* "incompatible implicit declaration of built-in function": the declaration
 * int() of a library built-in whose type differs. */
static void builtin_mismatch(Checker *c, SrcLoc loc, const BTab *bt)
{
    Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "builtin-declaration-mismatch",
                            "incompatible implicit declaration of built-in "
                            "function '%s'", bt->name);
    if (d && bt->hdr[0])
        cnote(c, d, loc, "include '%s' or provide a declaration of '%s'",
              bt->hdr, bt->name);
}

/* An implicit function declaration (C90; a C99 pedwarn). */
static void implicit_decl(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    uint32_t ref = id < c->nidents && c->ext[id] ? c->ext[id] - 1 : SYM_NONE;
    const BTab *bt = bt_find(c, name, false);
    csum_touch(c, SUM_EXT, id);
    CSym s;
    Diagnostic *d;
    if (ref != SYM_NONE && csym(c, ref)->kind == CS_FUNC) {
        CSym *o = csym(c, ref);
        if (!(o->flags & CSF_IMPLICIT)) {
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'", name);
            old_decl_note(c, d, o);
            o->flags |= CSF_IMPLICIT;
        } else if (bt && bt->mismatch)
            builtin_mismatch(c, loc, bt);
        cbind(c, NS_ORD, id, ref);
        o = csym(c, ref);
        c->ty[i] = o->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        return;
    }
    if (bt) {
        d = cpedwarn(c, loc, "implicit-function-declaration",
                     "implicit declaration of function '%s'", name);
        if (d && bt->hdr[0])
            cnote(c, d, header_note_loc(c, loc, bt->hdr), "include '%s' or "
                  "provide a declaration of '%s'", bt->hdr, name);
        if (bt->mismatch)
            builtin_mismatch(c, loc, bt);
    } else {
        const char *hdr = std_header(name);
        const char *sug = hdr ? NULL : fuzzy_name(c, name, true);
        if (sug)
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'; did you mean "
                         "'%s'?", name, sug);
        else
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'", name);
        if (hdr)
            header_note(c, d, loc, name, hdr);
    }
    memset(&s, 0, sizeof s);
    s.name = id;
    s.kind = CS_FUNC;
    s.sc = SC_EXTERN;
    s.linkage = LK_EXTERNAL;
    s.flags = CSF_IMPLICIT | CSF_USED;
    s.ty = bt && bt->mismatch ? bt_func_type(c, bt)
                              : type_func(TT, TYPE_B(INT), NULL, 0, TF_NOPROTO);
    s.loc = loc;
    ref = csym_new(c, true, &s);
    if (id < c->nidents && !c->ext[id])
        c->ext[id] = ref + 1;
    cbind(c, NS_ORD, id, ref);
    c->ty[i] = s.ty;
    c->ck[i] = K_ADDR;
    c->cb[i] = ref + 1;
}

static void undeclared(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    const char *hdr, *sug;
    Diagnostic *d;
    set_err(c, i);
    if (!in_function(c)) {
        CSym s;
        hdr = std_header(name);
        sug = hdr ? NULL : fuzzy_name(c, name, false);
        if (sug)
            d = cerror_d(c, loc, "'%s' undeclared here (not in a function); "
                         "did you mean '%s'?", name, sug);
        else
            d = cerror_d(c, loc, "'%s' undeclared here (not in a function)",
                         name);
        if (hdr)
            header_note(c, d, loc, name, hdr);
        memset(&s, 0, sizeof s);
        s.name = id;
        s.kind = CS_OBJ;
        s.flags = CSF_ERROR;
        s.ty = ERRT;
        s.loc = loc;
        cbind(c, NS_ORD, id, csym_new(c, cat_file_scope(c), &s));
        return;
    }
    {
        uint64_t key = ((uint64_t)c->u->first_tok * UINT64_C(0x100000001)) ^
                       c->func_sym;
        size_t k;
        if (key != c->undecl_key) {
            c->undecl_key = key;
            c->undecl.len = 0;
        }
        for (k = 0; k < c->undecl.len; k++)
            if (c->undecl.data[k] == id)
                return;
        vec_push(&c->undecl, id);
    }
    hdr = std_header(name);
    sug = hdr ? NULL : fuzzy_name(c, name, false);
    if (sug)
        d = cerror_d(c, loc, "'%s' undeclared (first use in this function); "
                     "did you mean '%s'?", name, sug);
    else
        d = cerror_d(c, loc, "'%s' undeclared (first use in this function)",
                     name);
    if (hdr)
        header_note(c, d, loc, name, hdr);
    if (d && !c->undecl_noted) {
        c->undecl_noted = true;
        cnote(c, d, loc, "each undeclared identifier is reported only once "
              "for each function it appears in");
    }
}

/* gcc's reject_gcc_builtin: a built-in function without a library fallback
 * (__builtin_trap, __sync_*, ...) may only be called, cast to void or
 * evaluated as a statement; anything that would take its address is an error.
 * The consumer of the value (through parentheses and '*') decides, and where
 * gcc reports: input_location, except an arithmetic, relational or equality
 * operand (the operand's own place for the left, the operator for the right)
 * and the right side of && and ||. */
static void reject_builtin(Checker *c, uint32_t i, const char *name)
{
    uint32_t n = i, p, k[2];
    SrcLoc loc = cinput_loc(c, c->nodes[i].tok);
    int op;
    if (bt_find(c, name + (!strncmp(name, "__builtin_", 10) ? 10 : 0), true) &&
        !strncmp(name, "__builtin_", 10))
        return;             /* has a library fallback */
    for (;;) {
        p = c->par[n];
        if (p == NO_NODE)
            return;
        if (ntag(c, p) == N_PAREN ||
            (ntag(c, p) == N_UNARY && npunct(c, p) == P_STAR)) {
            n = p;
            continue;
        }
        break;
    }
    switch (ntag(c, p)) {
    case N_CALL:
        if (first_child(c, p) == n)
            return;
        break;
    case N_CAST:
        if (nkids(c, p, k, 2) < 2 || k[1] != n || is_void(c, type_of_typename(c, k[0])))
            return;
        break;
    case N_BINARY:
        op = npunct(c, p);
        if (op == P_COMMA || nkids(c, p, k, 2) < 2)
            return;
        if (k[0] != n)
            loc = cnode_loc(c, p);
        else if (op != P_ANDAND && op != P_OROR)
            loc = cnode_loc(c, n);
        break;
    case N_ASSIGN:
        if (nkids(c, p, k, 2) < 2 || k[1] != n)
            return;
        if (npunct(c, p) != P_ASSIGN)
            loc = cnode_loc(c, n);
        break;
    case N_UNARY: case N_COND: case N_RETURN: case N_IF: case N_WHILE:
    case N_DO: case N_SWITCH: case N_FOR: case N_INIT_DECL:
    case N_INIT_LIST: case N_DESIGNATED:
        break;
    default:
        return;
    }
    cerror(c, loc, "built-in function '%s' must be directly called", name);
}

/* An operand of __builtin_has_attribute names a declaration without using it. */
static bool in_has_attr(const Checker *c, uint32_t i)
{
    unsigned depth;
    for (depth = 0; depth < 8; depth++) {
        uint32_t p = c->par[i];
        if (p == NO_NODE)
            return false;
        switch (ntag(c, p)) {
        case N_HAS_ATTR:
            return true;
        case N_PAREN: case N_MEMBER_EXPR: case N_INDEX: case N_UNARY:
        case N_CAST: case N_BINARY:
            i = p;
            break;
        default:
            return false;
        }
    }
    return false;
}

static void e_ident(Checker *c, uint32_t i)
{
    uint32_t id = cnode_ident(c, i), ref = lookup_ord(c, id);
    const char *name = cident(c, id);
    CSym *s;
    if (ref == SYM_NONE) {
        uint32_t p = c->par[i];
        if (!strcmp(name, "__func__") || !strcmp(name, "__FUNCTION__") ||
            !strcmp(name, "__PRETTY_FUNCTION__")) {
            size_t n = 0;
            if (in_function(c))
                n = strlen(cident(c, csym(c, c->func_sym)->name));
            else if (!strcmp(name, "__func__"))
                cpedwarn(c, cnode_loc(c, i), "",
                         "'%s' is not defined outside of function scope",
                         name);
            c->ty[i] = type_array(TT, type_qual(TYPE_B(CHAR), TQ_CONST), n + 1);
            c->ef[i] = EF_LVALUE | EF_ADDRLV;
            c->cb[i] = CB_NODE | i;
            c->ck[i] = K_ADDR;
            return;
        }
        if (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM) {
            size_t al;
            const char *an = ttext(c, c->nodes[p].tok, &al);
            static const char *const ex[] = {
                "nonnull", "aligned", "vector_size", "warn_if_not_aligned",
                "alloc_size", "alloc_align", "assume_aligned", "malloc"};
            size_t q;
            if (al > 4 && !strncmp(an, "__", 2) && !strncmp(an + al - 2, "__", 2)) {
                an += 2;
                al -= 4;
            }
            /* attributes whose arguments are expressions */
            for (q = 0; q < sizeof ex / sizeof *ex && strncmp(name, "__builtin_", 10); q++)
                if (strlen(ex[q]) == al && !strncmp(an, ex[q], al)) {
                    undeclared(c, i, id);
                    return;
                }
        }
        if (!strncmp(name, "__builtin_", 10)) {
            /* a built-in with a library counterpart has that function's type */
            const BTab *bt = bt_find(c, name + 10, true);
            TypeId ft = bt ? bt_func_type(c, bt) : overflow_func_type(c, name + 10);
            if (!is_err(c, ft)) {
                c->ty[i] = ft;
                c->ck[i] = K_ADDR;
                c->ef[i] = EF_ADDRLV;
                return;
            }
        }
        if (builtin_name(name) || (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM)) {
            if (builtin_name(name))
                reject_builtin(c, i, name);
            set_err(c, i);
            return;
        }
        if (is_callee(c, i)) {
            implicit_decl(c, i, id);
            return;
        }
        undeclared(c, i, id);
        return;
    }
    s = csym(c, ref);
    if (!in_has_attr(c, i))
        s->flags |= CSF_USED;
    if (c->func_sym != SYM_NONE && !(ref & SYM_LOCAL) &&
        (s->kind == CS_OBJ || s->kind == CS_FUNC) && s->linkage == LK_INTERNAL)
        cdecl_record_inline_static(c, ctok_loc(c, c->nodes[i].tok), s->name,
                                   false);
    cdep_use(c, cinput_loc(c, c->nodes[i].tok), s, &s->loc);
    switch (s->kind) {
    case CS_TYPEDEF:
        set_err(c, i);
        return;
    case CS_ENUMCONST:
        c->ty[i] = s->vty ? s->vty : s->ty;
        if (is_err(c, c->ty[i])) {
            set_err(c, i);
            return;
        }
        c->ck[i] = K_ICE;
        c->cv[i] = cexpr_trunc(c, c->ty[i], s->val);
        c->ef[i] = EF_INTOPS;
        return;
    case CS_FUNC:
        c->ty[i] = cbind_type(c, id) ? cbind_type(c, id) - 1 : s->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        c->ef[i] = EF_ADDRLV;
        return;
    default:
        break;
    }
    c->ty[i] = cbind_type(c, id) ? cbind_type(c, id) - 1 : s->ty;
    if (is_err(c, s->ty)) {
        set_err(c, i);
        return;
    }
    c->ef[i] = EF_LVALUE;
    if (s->sc == SC_REGISTER)
        c->ef[i] |= EF_REGISTER;
    if (tquals(c, s->ty) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (!(s->flags & CSF_THREAD) &&
        (!(ref & SYM_LOCAL) || s->sc == SC_STATIC || s->sc == SC_EXTERN ||
         s->linkage != LK_NONE)) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = ref + 1;
        addr_rvalue(c, i);
    }
}

/* ---- postfix operators --------------------------------------------------------- */

static void e_paren(Checker *c, uint32_t i)
{
    uint32_t k = first_child(c, i);
    if (k == NO_NODE || ntag(c, k) == N_ERROR) {
        set_err(c, i);
        return;
    }
    copy_node(c, i, k);
}

static bool is_const(Checker *c, uint32_t i)
{
    return c->ck[i] == K_ICE || c->ck[i] == K_FOLD || c->ck[i] == K_FLOAT ||
           c->ck[i] == K_ADDR;
}

static bool is_npc(Checker *c, uint32_t n);
static bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out);
static bool rvalue_ok(Checker *c, uint32_t i);
static bool rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc);
static TypeId vec_elem(Checker *c, TypeId vt);

/* ---- implicit conversions (gcc's convert_for_assignment) ----------------------------------- */

typedef struct Conv {
    Checker *c;
    const ConvInfo *ci;
    uint32_t expr;
    SrcLoc loc;          /* gcc's `location` */
    SrcLoc eloc;         /* gcc's `expr_loc` */
    TypeId type;         /* the target type (top-level qualifiers dropped) */
    TypeId rhstype;      /* the value's type after lvalue conversion */
    bool npc;            /* the value is a null pointer constant */
} Conv;

static void sp(char *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, 640, fmt, ap);
    va_end(ap);
}

/* %qv: the qualifiers in q. */
static const char *qual_str(char *b, unsigned q)
{
    b[0] = 0;
    if (q & TQ_CONST)
        strcat(b, "const");
    if (q & TQ_VOLATILE)
        strcat(b, b[0] ? " volatile" : "volatile");
    if (q & TQ_RESTRICT)
        strcat(b, b[0] ? " restrict" : "restrict");
    if (q & TQ_ATOMIC)
        strcat(b, b[0] ? " _Atomic" : "_Atomic");
    return b;
}

/* gcc's strip_array_types: the element type of (nested) arrays. */
static TypeId strip_arr(Checker *c, TypeId t)
{
    t = type_canon(TT, t);
    while (is_array(c, t))
        t = type_canon(TT, type_base(TT, t));
    return t;
}

/* gcc's TYPE_QUALS (arrays carry their element's qualifiers). */
static unsigned gq(Checker *c, TypeId t)
{
    return TYPE_QUALS(strip_arr(c, t));
}

/* TYPE_MAIN_VARIANT, keeping _Atomic (and leaving the elements of arrays
 * unqualified). */
static TypeId mvt(Checker *c, TypeId t)
{
    TypeId k = type_canon(TT, t);
    unsigned a = TYPE_QUALS(k) & TQ_ATOMIC;
    if (is_array(c, k)) {
        const TypeEnt *e = type_ent(TT, k);
        TypeId el = mvt(c, e->base);
        if (e->kind == TY_VLA)
            return type_vla(TT, el);
        if (e->flags & TF_INCOMPLETE)
            return type_array_incomplete(TT, el);
        return type_array(TT, el, e->n);
    }
    return TYPE_UNQUAL(k) | a;
}

/* c_common_signed_type / c_common_unsigned_type of a main variant. */
static void sign_map(Checker *c, TypeId t, TypeId *uns, TypeId *sgn)
{
    TypeId m = TYPE_UNQUAL(type_canon(TT, t));
    unsigned bits;
    *uns = *sgn = m;
    switch (tkind(c, m)) {
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR:
        *uns = TYPE_B(UCHAR);
        *sgn = TYPE_B(SCHAR);
        break;
    case TY_SHORT: case TY_USHORT:
        *uns = TYPE_B(USHORT);
        *sgn = TYPE_B(SHORT);
        break;
    case TY_INT: case TY_UINT:
        *uns = TYPE_B(UINT);
        *sgn = TYPE_B(INT);
        break;
    case TY_LONG: case TY_ULONG:
        *uns = TYPE_B(ULONG);
        *sgn = TYPE_B(LONG);
        break;
    case TY_LLONG: case TY_ULLONG:
        *uns = TYPE_B(ULLONG);
        *sgn = TYPE_B(LLONG);
        break;
    case TY_INT128: case TY_UINT128:
        *uns = TYPE_B(UINT128);
        *sgn = TYPE_B(INT128);
        break;
    case TY_ENUM:
        bits = int_bits(c, m);
        if (bits == 8) {
            *uns = TYPE_B(UCHAR);
            *sgn = TYPE_B(SCHAR);
        } else if (bits == 16) {
            *uns = TYPE_B(USHORT);
            *sgn = TYPE_B(SHORT);
        } else if (bits == 32) {
            *uns = TYPE_B(UINT);
            *sgn = TYPE_B(INT);
        } else if (bits == 64) {
            *uns = TYPE_B(ULONG);
            *sgn = TYPE_B(LONG);
        }
        /* a type already of the wanted signedness maps to itself */
        if (is_signed(c, m))
            *sgn = m;
        else
            *uns = m;
        break;
    default:
        break;
    }
}

/* gcc's INTEGER_TYPE: an integer that is neither _Bool nor an enumeration. */
static bool gcc_integer(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_CHAR && k <= TY_UINT128;
}

/* The parameter locations of the functions declared so far: gcc's
 * DECL_ARGUMENTS of the declaration it keeps (the definition's if seen, else
 * the first declaration's). */
typedef struct PLoc {
    uint32_t ref, n;
    bool def;
    SrcLoc *loc;
} PLoc;
typedef struct PLocVec {
    VEC(PLoc) v;
    uint32_t *idx;           /* symbol ref -> index + 1 into v */
    size_t idxcap;
} PLocVec;

void cexpr_free_params(Checker *c)
{
    PLocVec *v = c->plocs;
    size_t k;
    if (!v)
        return;
    for (k = 0; k < v->v.len; k++)
        free(v->v.data[k].loc);
    vec_free(&v->v);
    free(v->idx);
    free(v);
    c->plocs = NULL;
}

void cexpr_record_params(Checker *c, uint32_t declared, uint32_t ref, bool def)
{
    PLocVec *v = c->plocs;
    uint32_t fn = NO_NODE, kids[64], n, j, np = 0, m;
    PLoc *pl = NULL, nw;
    SrcLoc *locs;
    if (ref & SYM_LOCAL)
        return;
    if (!v) {
        v = xcalloc(1, sizeof *v);
        c->plocs = v;
    }
    if (ref < v->idxcap && v->idx[ref])
        pl = &v->v.data[v->idx[ref] - 1];
    if (pl && (pl->def || !def))
        return;
    /* the function declarator: the declared node's declarator sibling */
    for (j = cfirst(c, c->par[declared]); j < declared; j++)
        if (ntag(c, j) == N_FUNC)
            fn = j;
    if (fn == NO_NODE)
        return;
    n = nkids(c, fn, kids, 64);
    locs = xcalloc(n + 1, sizeof *locs);
    for (j = 0; j < n && j < 64; j++) {
        uint32_t q = kids[j];
        if (!is_real_param(c, q))
            continue;
        locs[np] = first_loc(c, q);
        for (m = cfirst(c, q); m <= q; m++)
            if (ntag(c, m) == N_NAME) {
                locs[np] = cnode_loc(c, m);
                break;
            }
        np++;
    }
    if (pl) {
        free(pl->loc);
        pl->loc = locs;
        pl->n = np;
        pl->def = def;
        return;
    }
    nw.ref = ref;
    nw.n = np;
    nw.def = def;
    nw.loc = locs;
    if (ref >= v->idxcap) {
        size_t nc = v->idxcap ? v->idxcap : 1024;
        while (nc <= ref)
            nc *= 2;
        v->idx = xrealloc(v->idx, nc * sizeof *v->idx);
        memset(v->idx + v->idxcap, 0, (nc - v->idxcap) * sizeof *v->idx);
        v->idxcap = nc;
    }
    vec_push(&v->v, nw);
    v->idx[ref] = (uint32_t)v->v.len;
}

static SrcLoc param_loc(Checker *c, uint32_t ref, uint32_t idx, uint32_t at,
                        SrcLoc dflt)
{
    const PLocVec *v = c->plocs;
    (void)at;
    if (v && ref < v->idxcap && v->idx[ref]) {
        const PLoc *p = &v->v.data[v->idx[ref] - 1];
        return idx < p->n ? p->loc[idx] : dflt;
    }
    return dflt;
}

static SrcLoc conv_note_loc(const Conv *x)
{
    if (x->ci->note_loc)
        return x->ci->note_loc;
    if (x->ci->fsym)
        return param_loc(x->c, x->ci->fsym - 1, (uint32_t)x->ci->parmnum - 1,
                         x->expr, x->eloc);
    return x->eloc;
}

enum { RK_PED, RK_WARN };

/* One diagnostic of a conversion, the text per context in m[] (indexed by
 * CONV_*); the notes gcc adds follow a reported one. */
static void conv_diag(Conv *x, int rk, const char *opt, char (*m)[640],
                      bool near)
{
    Checker *c = x->c;
    int ctx = x->ci->context;
    SrcLoc l = ctx == CONV_ARG ? x->eloc : x->loc;
    Diagnostic *d;
    if (rk == RK_PED)
        d = cpedwarn(c, l, opt, "%s", m[ctx]);
    else
        d = cwarn_d(c, DL_WARNING, l, opt, "%s", m[ctx]);
    if (!d)
        return;
    if (ctx == CONV_ARG)
        cnote(c, d, conv_note_loc(x), "expected %s but argument is of type %s",
              type_q(TT, x->type), type_q(TT, x->rhstype));
    else if (ctx == CONV_INIT && near && x->ci->near && *x->ci->near)
        cnote(c, d, l, "(near initialization for '%s')", x->ci->near);
}

/* comptypes_internal's enum_and_int_p: compatible types that differ in
 * being an enum on one side and an integer type on the other. */
bool cexpr_enum_int_mix(Checker *c, TypeId a, TypeId b, int depth)
{
    TypeKind ka, kb;
    a = type_canon(TT, a);
    b = type_canon(TT, b);
    ka = tkind(c, a);
    kb = tkind(c, b);
    if ((ka == TY_ENUM) != (kb == TY_ENUM))
        return type_is_integer(TT, a) && type_is_integer(TT, b);
    if (ka != kb || depth > 8)
        return false;
    if (ka == TY_PTR || ka == TY_ARRAY)
        return cexpr_enum_int_mix(c, type_base(TT, a), type_base(TT, b), depth + 1);
    if (ka == TY_FUNC) {
        uint32_t k, n = type_ent(TT, a)->n;
        if (n != type_ent(TT, b)->n)
            return false;
        if (cexpr_enum_int_mix(c, type_base(TT, a), type_base(TT, b), depth + 1))
            return true;
        for (k = 0; k < n; k++)
            if (cexpr_enum_int_mix(c, type_params(TT, a)[k], type_params(TT, b)[k],
                             depth + 1))
                return true;
    }
    return false;
}

/* comp_target_types of two pointer types (a pedantic note for arrays of
 * differently qualified elements). */
static int comp_target(Conv *x, TypeId lt, TypeId rt)
{
    Checker *c = x->c;
    TypeId mvl = type_base(TT, type_canon(TT, lt));
    TypeId mvr = type_base(TT, type_canon(TT, rt));
    bool val_ped = true, val;
    if (is_array(c, mvl) && is_array(c, mvr))
        val_ped = type_compatible(TT, mvl, mvr);
    val = type_compatible(TT, mvt(c, mvl), mvt(c, mvr));
    if (val && cexpr_cxx_compat(c, x->expr) &&
        cexpr_enum_int_mix(c, mvl, mvr, 0))
        cwarn(c, x->loc, "c++-compat", "pointer target types incompatible in "
              "C++");
    if (val && !val_ped)
        cpedantic(c, x->loc, "invalid use of pointers to arrays with "
                  "different qualifiers in ISO C before C2X");
    return val;
}

/* comp_target_types of two pointer targets (-, comparison, ?:): arrays of
 * differently qualified elements are compatible, with a pedantic note. */
static bool targets_compat(Checker *c, SrcLoc loc, TypeId pa, TypeId pb)
{
    TypeId a = type_canon(TT, pa), b = type_canon(TT, pb);
    bool ped = !(is_array(c, a) && is_array(c, b)) || type_compatible(TT, a, b);
    bool val = type_compatible(TT, mvt(c, a), mvt(c, b));
    if (val && !ped)
        cpedantic(c, loc, "invalid use of pointers to arrays with different "
                  "qualifiers in ISO C before C2X");
    return val;
}

/* The location of gcc's built-in declarations ("<built-in>"). */
static SrcLoc builtin_loc(Checker *c)
{
    uint32_t k, n = srcmgr_nfiles(c->sm);
    for (k = 0; k < n; k++) {
        SrcFile *f = srcmgr_file(c->sm, k);
        if (f && f->kind == SF_VIRTUAL && !strcmp(f->name, "<built-in>"))
            return f->base;
    }
    return 0;
}

/* maybe_warn_builtin_no_proto_arg: an argument of a call to a built-in
 * declared without a prototype that does not promote to the built-in's
 * parameter type. */
static void builtin_noproto_arg(Conv *x)
{
    Checker *c = x->c;
    TypeId pt = x->type, at = x->rhstype, pr = TYPE_UNQUAL(at);
    TypeKind pk = tkind(c, pt), ak = tkind(c, at);
    int pc, ac;
    Diagnostic *d;
    if (is_int(c, at)) {
        /* gcc keeps the argument's own type node (typedef and all) when the
         * promotion changes nothing; an enum as wide as int stays itself */
        TypeId pm = type_int_promote(TT, at);
        if (ak == TY_ENUM && int_bits(c, at) < int_bits(c, TYPE_B(INT)))
            pr = TYPE_B(INT);
        else if (ak != TY_ENUM && mainv(c, pm) != mainv(c, at))
            pr = mainv(c, pm);
    } else if (is_flt(c, at) && ak != TY_DOUBLE && ak != TY_LDOUBLE &&
               float_prec(c, ak) <= 53 && ak >= TY_FLOAT16 && ak <= TY_FLOAT)
        pr = TYPE_B(DOUBLE);
    pc = gcc_integer(c, pt) ? 1 : pk == TY_ENUM ? 2 : pk == TY_BOOL ? 3
         : is_flt(c, pt) ? 4 : 5;
    ac = gcc_integer(c, at) ? 1 : ak == TY_ENUM ? 2 : ak == TY_BOOL ? 3
         : is_flt(c, at) ? 4 : 5;
    if (pc == 1 && ac == 2 && int_bits(c, pt) == int_bits(c, at))
        return;
    if ((pc == ac || (pc == 1 && ac == 2)) &&
        TYPE_UNQUAL(type_canon(TT, pt)) == TYPE_UNQUAL(type_canon(TT, pr)))
        return;
    d = cwarn_d(c, DL_WARNING, x->eloc, "builtin-declaration-mismatch",
                pr == mainv(c, at)
                ? "'%s' argument %d type is %s where %s is expected in a call "
                  "to built-in function declared without prototype"
                : "'%s' argument %d promotes to %s where %s is expected in a "
                  "call to built-in function declared without prototype",
                x->ci->fname, x->ci->parmnum, type_q(TT, pr), type_q(TT, pt));
    if (d)
        cnote(c, d, builtin_loc(c), "built-in '%s' declared here",
              x->ci->fname);
}

/* -Woverflow for a constant stored into a bit-field of width w: gcc converts
 * to the bit-field's own (narrower) type, which prints as 'unsigned char:3'.
 * True when it warned. */
bool cexpr_bf_overflow(Checker *c, SrcLoc loc, uint32_t n, TypeId ft,
                       TypeId rt, unsigned w)
{
    unsigned su_bits;
    bool tu, su;
    int64_t sv;
    uint64_t uv, mask, r;
    const char *base;
    char tname[64], vs[32], rs[32];
    if (!type_is_integer(TT, rt) || type_int_bits(TT, rt) > 64)
        return false;
    su_bits = type_int_bits(TT, ft);
    if (w >= su_bits)
        return false;
    tu = !type_is_signed(TT, ft);
    su = !type_is_signed(TT, rt);
    sv = cexpr_sval(c, n);
    uv = (uint64_t)sv;
    mask = ((uint64_t)1 << w) - 1;
    {
        bool fits;
        if (su)
            fits = tu ? uv <= mask : uv <= (mask >> 1);
        else if (tu)
            fits = sv >= 0 && uv <= mask;
        else
            fits = sv >= -(int64_t)(mask >> 1) - 1 && sv <= (int64_t)(mask >> 1);
        if (fits)
            return false;
    }
    r = uv & mask;
    if (!tu && (r >> (w - 1)))
        r |= ~mask;
    base = w <= 8 ? (tu ? "unsigned char" : "signed char")
         : w <= 16 ? (tu ? "short unsigned int" : "short int")
         : w <= 32 ? (tu ? "unsigned int" : "int")
                   : (tu ? "long unsigned int" : "long int");
    snprintf(tname, sizeof tname, "'%s:%u'", base, w);
    if (su)
        snprintf(vs, sizeof vs, "%" PRIu64, uv);
    else
        snprintf(vs, sizeof vs, "%" PRId64, sv);
    if (tu)
        snprintf(rs, sizeof rs, "%" PRIu64, r);
    else
        snprintf(rs, sizeof rs, "%" PRId64, (int64_t)r);
    if (su) {
        cwarn(c, loc, "overflow", "conversion from %s to %s "
              "changes value from '%s' to '%s'", type_q(TT, rt), tname, vs, rs);
        return true;
    } else if (tu) {
        bool sfit = sv >= -(int64_t)(mask >> 1) - 1 && sv <= (int64_t)(mask >> 1);
        if (!sfit) {
            cwarn(c, loc, "overflow", "unsigned conversion from "
                  "%s to %s changes value from '%s' to '%s'", type_q(TT, rt),
                  tname, vs, rs);
            return true;
        }
    } else {
        bool ufit = sv >= 0 && uv <= mask;
        if (!ufit || c->opt.pedantic) {
            cwarn(c, loc, "overflow", "overflow in conversion "
                  "from %s to %s changes value from '%s' to '%s'",
                  type_q(TT, rt), tname, vs, rs);
            return true;
        }
    }
    return false;
}

/* ---- -Wconversion: gcc's unsafe_conversion_p and conversion_warning ------- */

enum { UC_SAFE, UC_OTHER, UC_SIGN, UC_REAL, UC_IMAG };

static unsigned bf_width(Checker *c, uint32_t n);

static unsigned uc_bw;    /* the width of the bit-field being assigned, or 0 */
static const char *uc_whole;   /* a conditional that folds to this constant */

/* The target type as gcc prints it: a bit-field's own type is 'signed char:1'. */
static const char *tgt_name(Checker *c, TypeId lt, char *buf)
{
    bool tu = !is_signed(c, lt);
    unsigned w = uc_bw;
    if (!w)
        return type_q(TT, lt);
    snprintf(buf, 64, "'%s:%u'", w <= 8 ? (tu ? "unsigned char" : "signed char")
             : w <= 16 ? (tu ? "short unsigned int" : "short int")
             : w <= 32 ? (tu ? "unsigned int" : "int")
                       : (tu ? "long unsigned int" : "long int"), w);
    return buf;
}

static unsigned tgt_bits(Checker *c, TypeId lt)
{
    return uc_bw ? uc_bw : int_bits(c, lt);
}

static bool tgt_fits(Checker *c, uint64_t v, TypeId et, TypeId lt)
{
    bool neg;
    uint64_t mask;
    if (!uc_bw)
        return cexpr_fits(c, v, et, lt);
    neg = is_signed(c, et) && (int64_t)v < 0;
    mask = ((uint64_t)1 << uc_bw) - 1;
    if (!is_signed(c, lt))
        return !neg && v <= mask;
    if (neg)
        return (int64_t)v >= -(int64_t)(mask >> 1) - 1;
    return v <= (mask >> 1);
}

static uint64_t tgt_trunc(Checker *c, TypeId lt, uint64_t v)
{
    uint64_t mask, r;
    if (!uc_bw)
        return cexpr_trunc(c, lt, v);
    mask = ((uint64_t)1 << uc_bw) - 1;
    r = v & mask;
    if (is_signed(c, lt) && (r >> (uc_bw - 1)))
        r |= ~mask;
    return r;
}

/* gcc's REAL_TYPE (decimal floats and complex types excluded). */
static bool gcc_real(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_FLOAT16 && k <= TY_IBM128;
}

/* TYPE_PRECISION for ordering integer or real types. */
static unsigned uc_prec(Checker *c, TypeId t)
{
    return is_int(c, t) ? int_bits(c, t) : (unsigned)float_prec(c, tkind(c, t));
}

/* A REAL_CST as gcc prints it in a warning: enough digits to round-trip the
 * type, a suffix for float and long double. */
static const char *real_cst_str(Checker *c, char *out, long double f, TypeId t)
{
    char b[160], *e, *p;
    int ex, dig = (int)(uc_prec(c, t) * 0.30103 + 1.0) + 1;
    TypeKind k = tkind(c, t);
    const char *suf = k == TY_FLOAT || k == TY_FLOAT32 ? "f"
                      : k == TY_LDOUBLE ? "l" : "";
    if (f != f) {
        snprintf(out, 160, "+QNaN%s", suf);
        return out;
    }
    if (f > 1.0e4900L || f < -1.0e4900L || f == (long double)HUGE_VALL ||
        f == -(long double)HUGE_VALL) {
        snprintf(out, 160, "%cInf%s", f < 0 ? '-' : '+', suf);
        return out;
    }
    snprintf(b, sizeof b, "%.*Le", dig - 1, f);
    e = strchr(b, 'e');
    if (!e) {
        snprintf(out, 160, "%s%s", b, suf);
        return out;
    }
    ex = atoi(e + 1);
    *e = 0;
    p = b + strlen(b);
    while (p > b && p[-1] == '0')
        *--p = 0;
    if (p > b && p[-1] == '.')
        *p++ = '0', *p = 0;
    snprintf(out, 160, "%se%+d%s", b, ex, suf);
    return out;
}

/* f rounded to the real type t (an overflow gives infinity). */
static long double real_round(Checker *c, long double f, TypeId t)
{
    TypeKind k = tkind(c, t);
    if (k == TY_FLOAT || k == TY_FLOAT32)
        return (float)f;
    if (k == TY_DOUBLE || k == TY_FLOAT64 || k == TY_FLOAT32X ||
        (k == TY_LDOUBLE && c->tgt->long_double == LD_IEEE64))
        return (double)f;
    return f;
}

static uint32_t uc_cast_operand(Checker *c, uint32_t e)
{
    uint32_t k[3], n = nkids(c, e, k, 3);
    return n ? k[n - 1] : NO_NODE;
}

/* gcc's get_unwidened (e, 0): the expression and type e had before widening
 * conversions (explicit casts only; the promotions are not nodes here). */
static uint32_t unwidened(Checker *c, uint32_t e, TypeId *ty)
{
    unsigned final = int_bits(c, rvt(c, e));
    uint32_t win = e, op = strip_paren(c, e);
    bool uns = false;
    while (op != NO_NODE && ntag(c, op) == N_CAST) {
        uint32_t in = strip_paren(c, uc_cast_operand(c, op));
        TypeId ot = rvt(c, op), it;
        int bc;
        if (in == NO_NODE)
            break;
        it = rvt(c, in);
        if (!is_int(c, it) || !is_int(c, ot))
            break;
        bc = (int)int_bits(c, ot) - (int)int_bits(c, it);
        if (bc < 0 && final > int_bits(c, ot))
            break;
        op = in;
        if (bc > 0) {
            if (!uns || final <= int_bits(c, it))
                win = in;
            if ((uns || ntag(c, op) == N_CAST) && !is_signed(c, it)) {
                uns = true;
                win = in;
            }
        }
    }
    *ty = rvt(c, win);
    return win;
}

static bool bitwise_op(int p)
{
    return p == P_AMP || p == P_PIPE || p == P_CARET;
}

/* Does node n, an integer constant converted to type as, fit type t? */
static bool const_fits(Checker *c, uint32_t n, TypeId as, TypeId t)
{
    return has_ival(c, n) && int_bits(c, rvt(c, n)) <= 64 &&
           int_bits(c, as) <= 64 &&
           cexpr_fits(c, cexpr_trunc(c, as, c->cv[n]), as, t);
}

/* build_binary_op narrows a division, a modulus or a right shift to its
 * operands' narrower type: wt is that type when it did. */
static bool shorten_divshift(Checker *c, uint32_t ws, TypeId *wt)
{
    uint32_t k[2], s1;
    TypeId t0, t1;
    unsigned p = npunct(c, ws);
    bool div = p == P_SLASH || p == P_PERCENT, cst;
    if ((!div && p != P_SHR) || nkids(c, ws, k, 2) != 2)
        return false;
    unwidened(c, k[0], &t0);
    s1 = strip_paren(c, unwidened(c, k[1], &t1));
    if (int_bits(c, t0) >= int_bits(c, rvt(c, ws)))
        return false;
    cst = has_ival(c, s1);
    if (div) {
        if (cst && is_signed(c, rvt(c, s1)) && (int64_t)c->cv[s1] == -1)
            cst = false, t1 = 0;
        if (!(!is_signed(c, rvt(c, k[0])) || cst))
            return false;
        if (cst ? !const_fits(c, s1, rvt(c, s1), t0)
                : !t1 || mainv(c, t0) != mainv(c, t1))
            return false;
    } else if (!cst || (is_signed(c, rvt(c, s1)) && (int64_t)c->cv[s1] < 0) ||
               c->cv[s1] >= int_bits(c, t0)) {
        return false;
    }
    *wt = t0;
    return true;
}

/* The narrowed type of a bitwise node's operands (shorten_binary_op) and
 * gcc's BIT_AND special cases: true when the result surely fits lt. */
static bool shorten_bitwise(Checker *c, uint32_t ws, TypeId lt, TypeId et,
                            TypeId *wt)
{
    uint32_t k[2], o0, o1, s0, s1;
    TypeId t0, t1;
    bool c0, c1;
    if (nkids(c, ws, k, 2) != 2)
        return false;
    o0 = unwidened(c, k[0], &t0);
    o1 = unwidened(c, k[1], &t1);
    s0 = strip_paren(c, o0);
    s1 = strip_paren(c, o1);
    c0 = has_ival(c, s0);
    c1 = has_ival(c, s1);
    if (!c0 && !c1) {
        unsigned b0 = int_bits(c, t0), b1 = int_bits(c, t1);
        *wt = b0 > b1 ? t0 : b1 > b0 ? t1
              : is_signed(c, t0) ? t0 : t1;
        if (int_bits(c, *wt) > int_bits(c, et))
            *wt = et;
    } else if (c0 != c1) {
        TypeId vt = c0 ? t1 : t0;
        if (int_bits(c, vt) < int_bits(c, *wt))
            *wt = vt;
    }
    if (npunct(c, ws) == P_AMP) {
        TypeId sg, us, d;
        sign_map(c, lt, &us, &d);
        sign_map(c, lt, &d, &sg);
        TypeId ot = rvt(c, ws);        /* the operands' converted type */
        if ((c0 && const_fits(c, s0, ot, sg) && const_fits(c, s0, ot, us)) ||
            (c1 && const_fits(c, s1, ot, sg) && const_fits(c, s1, ot, us)))
            return true;
        if ((c0 && !is_signed(c, ot) && const_fits(c, s0, ot, lt)) ||
            (c1 && !is_signed(c, ot) && const_fits(c, s1, ot, lt)))
            return true;
    }
    return false;
}

/* unsafe_conversion_p: e (of type et) converted to lt. */
static int unsafe_conv_t(Checker *c, TypeId lt, uint32_t e, TypeId et,
                         bool check_sign)
{
    uint32_t s = strip_paren(c, e);
    if (s == NO_NODE)
        return UC_SAFE;
    if (has_ival(c, s) && int_bits(c, et) <= 64) {
        if (!gcc_integer(c, et))
            return UC_SAFE;
        if (gcc_integer(c, lt) && int_bits(c, lt) <= 64) {
            uint64_t v = c->cv[s];
            if (tgt_fits(c, v, et, lt))
                return UC_SAFE;
            if (!is_signed(c, lt) && is_signed(c, et) && (int64_t)v < 0)
                return check_sign ? UC_SIGN : UC_SAFE;
            if (is_signed(c, lt) && !is_signed(c, et))
                return check_sign ? UC_SIGN : UC_SAFE;
            return UC_OTHER;
        }
        if (gcc_real(c, lt)) {
            long double f = is_signed(c, et) ? (long double)(int64_t)c->cv[s]
                                             : (long double)c->cv[s];
            return real_round(c, f, lt) == f ? UC_SAFE : UC_REAL;
        }
        return UC_SAFE;
    }
    if (c->ck[s] == K_FLOAT && gcc_real(c, et)) {
        long double f = c->fv.data[c->cv[s]];
        if (gcc_integer(c, lt))
            return f == truncl(f) ? UC_SAFE : UC_REAL;
        if (gcc_real(c, lt) && uc_prec(c, lt) < uc_prec(c, et))
            return real_round(c, f, lt) == f ? UC_SAFE : UC_REAL;
        return UC_SAFE;
    }
    if (is_complex(c, et)) {
        TypeId ef = mainv(c, type_base(TT, type_canon(TT, et))), tf;
        if (!is_complex(c, lt))
            return UC_IMAG;
        tf = mainv(c, type_base(TT, type_canon(TT, lt)));
        if (gcc_real(c, ef) && gcc_integer(c, tf))
            return UC_REAL;
        if (gcc_real(c, ef) && gcc_real(c, tf))
            return uc_prec(c, tf) < uc_prec(c, ef) ? UC_REAL : UC_SAFE;
        if (gcc_integer(c, ef) && gcc_integer(c, tf)) {
            bool eu = !is_signed(c, ef), tu = !is_signed(c, tf);
            if (int_bits(c, tf) < int_bits(c, ef))
                return UC_OTHER;
            if (check_sign && ((int_bits(c, tf) == int_bits(c, ef) && eu != tu) ||
                               (tu && !eu)))
                return UC_SIGN;
            return UC_SAFE;
        }
        if (gcc_integer(c, ef) && gcc_real(c, tf))
            return int_bits(c, ef) - (is_signed(c, ef) ? 1u : 0u) <=
                   uc_prec(c, tf) ? UC_SAFE : UC_OTHER;
        return UC_SAFE;
    }
    if (gcc_real(c, et) && gcc_integer(c, lt))
        return UC_REAL;
    if (gcc_integer(c, et) && gcc_integer(c, lt)) {
        TypeId wt;
        uint32_t w = unwidened(c, s, &wt), ws = strip_paren(c, w);
        unsigned tp = tgt_bits(c, lt), wb;
        bool eu, tu = !is_signed(c, lt);
        if (ws != NO_NODE && ntag(c, ws) == N_BINARY &&
            bitwise_op(npunct(c, ws)) &&
            shorten_bitwise(c, ws, lt, et, &wt))
            return UC_SAFE;
        if (ws != NO_NODE && ntag(c, ws) == N_BINARY)
            shorten_divshift(c, ws, &wt);
        unsigned bw = ws != NO_NODE ? bf_width(c, ws) : 0;
        wb = int_bits(c, wt);
        if (bw && bw < wb)
            wb = bw;            /* a bit-field has its own narrow type */
        eu = !is_signed(c, wt);
        if (tp < wb)
            return UC_OTHER;
        if (check_sign && ((tp == wb && eu != tu) || (tu && !eu)))
            return UC_SIGN;
        return UC_SAFE;
    }
    if (gcc_integer(c, et) && gcc_real(c, lt)) {
        TypeId wt;
        unsigned fp;
        uint32_t w = unwidened(c, s, &wt), bw = bf_width(c, w);
        if (w != NO_NODE && ntag(c, strip_paren(c, w)) == N_BINARY)
            shorten_divshift(c, strip_paren(c, w), &wt);
        fp = int_bits(c, wt);
        if (bw && bw < fp)
            fp = bw;
        fp -= is_signed(c, wt) ? 1u : 0u;
        return fp <= uc_prec(c, lt) ? UC_SAFE : UC_OTHER;
    }
    if (gcc_real(c, et) && gcc_real(c, lt) && uc_prec(c, lt) < uc_prec(c, et))
        return UC_REAL;
    return UC_SAFE;
}

static int unsafe_conv(Checker *c, TypeId lt, uint32_t e, bool check_sign)
{
    uint32_t s = strip_paren(c, e);
    return s == NO_NODE ? UC_SAFE
                        : unsafe_conv_t(c, lt, s, rvt(c, s), check_sign);
}

/* The operands of an arithmetic node as conversion_warning treats them
 * (arith_ops); 0 for other nodes. */
static int arith_operands(Checker *c, uint32_t s, uint32_t *k)
{
    int p;
    switch (ntag(c, s)) {
    case N_BINARY:
        p = npunct(c, s);
        if ((p == P_PLUS || p == P_MINUS || p == P_STAR || p == P_SLASH ||
             p == P_PERCENT || p == P_SHL || p == P_SHR || bitwise_op(p)) &&
            nkids(c, s, k, 2) == 2)
            return 2;
        return 0;
    case N_UNARY:
        p = npunct(c, s);
        if ((p == P_MINUS || p == P_TILDE) && nkids(c, s, k, 1) == 1)
            return 1;
        return 0;
    case N_ASSIGN:        /* a compound assignment converts a op b */
        p = npunct(c, s);
        if (p != P_ASSIGN && nkids(c, s, k, 2) == 2)
            return 2;
        return 0;
    default:
        return 0;
    }
}

/* A comparison or logical operator: its value is a boolean. */
static bool bool_valued(Checker *c, uint32_t s)
{
    int p;
    if (ntag(c, s) == N_UNARY)
        return npunct(c, s) == P_BANG;
    if (ntag(c, s) == N_COND) {        /* fold turns c ? 1 : 0 into (int)c */
        uint32_t k[3], a0, a1;
        if (nkids(c, s, k, 3) != 3)
            return false;
        a0 = strip_paren(c, k[1]);
        a1 = strip_paren(c, k[2]);
        return has_ival(c, a0) && has_ival(c, a1) &&
               ((c->cv[a0] == 1 && c->cv[a1] == 0) ||
                (c->cv[a0] == 0 && c->cv[a1] == 1)) &&
               !has_ival(c, s);
    }
    if (ntag(c, s) != N_BINARY)
        return false;
    p = npunct(c, s);
    return p == P_EQEQ || p == P_NE || p == P_LT || p == P_GT || p == P_LE ||
           p == P_GE || p == P_ANDAND || p == P_OROR;
}

/* An expression built only from numeric literals (a complex constant). */
static bool opeq(Checker *c, uint32_t x, uint32_t y);

/* gcc folds x - x and x ^ x (also as -= and ^=) of integers to 0. */
static bool folds_to_zero(Checker *c, uint32_t n)
{
    uint32_t k[3];
    unsigned p;
    if ((ntag(c, n) != N_BINARY && ntag(c, n) != N_ASSIGN) ||
        !gcc_integer(c, rvt(c, n)))
        return false;
    p = npunct(c, n);
    return (p == P_MINUS || p == P_CARET || p == P_SUB_ASSIGN ||
            p == P_XOR_ASSIGN) &&
           nkids(c, n, k, 3) == 2 && opeq(c, k[0], k[1]);
}

static bool cplx_const(Checker *c, uint32_t n)
{
    uint32_t k[3], m, i;
    n = strip_paren(c, n);
    if (n == NO_NODE)
        return false;
    switch (ntag(c, n)) {
    case N_NUMBER:
        return true;
    case N_UNARY: case N_BINARY:
        m = nkids(c, n, k, 3);
        for (i = 0; i < m; i++)
            if (!cplx_const(c, k[i]))
                return false;
        return m > 0;
    default:
        return false;
    }
}

/* conversion_warning: e converted to lt.  top: e is the converted expression,
 * not an arm of a conditional. */
static void conversion_warning(Checker *c, SrcLoc l, TypeId lt, uint32_t e,
                               TypeId et, bool top)
{
    uint32_t s = strip_paren(c, e), k[3];
    int kind;
    const char *opt;
    bool cst;
    if (s == NO_NODE || node_err(c, s) || folds_to_zero(c, s))
        return;
    cst = has_ival(c, s) || c->ck[s] == K_FLOAT;
    if (ntag(c, s) == N_COND && !cst && !bool_valued(c, s)) {
        uint32_t n = nkids(c, s, k, 3);
        if (n >= 2) {
            /* the arms were converted to the conditional's type */
            TypeId ct = rvt(c, s);
            int j;
            char wb[40];
            const char *saved = uc_whole;
            uc_whole = NULL;
            /* arms that convert to one value make the whole a constant */
            if (gcc_integer(c, lt) && int_bits(c, lt) <= 64 &&
                has_ival(c, strip_paren(c, k[n - 2])) &&
                has_ival(c, strip_paren(c, k[n - 1]))) {
                uint64_t r0 = tgt_trunc(c, lt, c->cv[strip_paren(c, k[n - 2])]),
                         r1 = tgt_trunc(c, lt, c->cv[strip_paren(c, k[n - 1])]);
                if (r0 == r1) {
                    snprintf(wb, sizeof wb, "%s", vstr(c, lt, r0));
                    uc_whole = wb;
                }
            }
            for (j = n - 2; j < (int)n; j++) {
                bool ar = is_arith(c, ct) && !is_complex(c, ct);
                /* gcc warned about an arm's conversion to ct already */
                if (ar && mainv(c, promoted(c, k[j])) != mainv(c, ct) &&
                    unsafe_conv_t(c, ct, k[j], promoted(c, k[j]), true) !=
                    UC_SAFE)
                    continue;
                conversion_warning(c, l, lt, k[j], ar ? ct : rvt(c, k[j]),
                                   false);
            }
            uc_whole = saved;
        }
        return;
    }
    if (!(gcc_integer(c, et) || gcc_real(c, et) || is_complex(c, et)) ||
        !(gcc_integer(c, lt) || gcc_real(c, lt) || is_complex(c, lt)))
        return;
    if (is_complex(c, et) && cplx_const(c, s))
        return;      /* gcc prints complex constants in a form we lack */
    if (!cst && bool_valued(c, s)) {
        char tb[64];
        if (uc_bw == 1 && is_signed(c, lt) && diag_enabled(c->diag, "conversion"))
            cwarn(c, l, "conversion", "conversion to %s from boolean "
                  "expression", tgt_name(c, lt, tb));
        return;
    }
    kind = unsafe_conv_t(c, lt, s, et, true);
    if (kind == UC_SAFE)
        return;
    opt = kind == UC_REAL ? "float-conversion"
          : kind == UC_SIGN ? "sign-conversion" : "conversion";
    if (kind == UC_IMAG) {
        if (diag_enabled(c->diag, opt)) {
            char tb[64];
            cwarn(c, l, opt, "conversion from %s to %s discards imaginary "
                  "component", type_q(TT, et), tgt_name(c, lt, tb));
        }
        return;
    }
    if (kind != UC_REAL && !cst) {
        int i, n = arith_operands(c, s, k);
        bool safe = n > 0;
        for (i = 0; i < n && safe; i++) {
            bool cs = true;
            uint32_t o = strip_paren(c, k[i]);
            if (i == 1 && npunct(c, s) == P_PLUS && !is_signed(c, lt) &&
                has_ival(c, o) && is_signed(c, rvt(c, o)) &&
                (int64_t)c->cv[o] < 0)
                cs = false;
            if (unsafe_conv(c, lt, k[i], cs) != UC_SAFE)
                safe = false;
        }
        if (safe)
            opt = "arith-conversion";        /* off by default */
    }
    if (!diag_enabled(c->diag, opt))
        return;
    if (cst) {
        char a[160], b[160], tb[64];
        const char *from, *to, *tn = tgt_name(c, lt, tb);
        if (has_ival(c, s)) {
            uint64_t v = c->cv[s];
            from = vstr(c, et, v);
            if (gcc_integer(c, lt)) {
                to = vstr(c, lt, tgt_trunc(c, lt, v));
            } else {
                long double f = is_signed(c, et) ? (long double)(int64_t)v
                                                 : (long double)v;
                to = real_cst_str(c, b, real_round(c, f, lt), lt);
            }
        } else {
            long double f = c->fv.data[c->cv[s]];
            uint64_t r;
            from = real_cst_str(c, a, f, et);
            if (gcc_integer(c, lt)) {
                if (!float_to_int(c, f, lt, &r))
                    return;
                to = vstr(c, lt, r);
            } else
                to = real_cst_str(c, b, real_round(c, f, lt), lt);
        }
        if (kind == UC_SIGN) {
            const char *w = is_signed(c, lt) ? "signed" : "unsigned";
            if (top || uc_whole)
                cwarn(c, l, opt, "%s conversion from %s to %s changes value "
                      "from '%s' to '%s'", w, type_q(TT, et), tn, from,
                      uc_whole ? uc_whole : to);
            else
                cwarn(c, l, opt, "%s conversion from %s to %s changes the "
                      "value of '%s'", w, type_q(TT, et), tn, from);
        } else if (top || uc_whole) {
            cwarn(c, l, opt, "conversion from %s to %s changes value from "
                  "'%s' to '%s'", type_q(TT, et), tn, from,
                  uc_whole ? uc_whole : to);
        } else {
            cwarn(c, l, opt, "conversion from %s to %s changes the value of "
                  "'%s'", type_q(TT, et), tn, from);
        }
        return;
    }
    {
        char tb[64];
        if (kind == UC_SIGN)
            cwarn(c, l, opt, "conversion to %s from %s may change the sign of "
                  "the result", tgt_name(c, lt, tb), type_q(TT, et));
        else
            cwarn(c, l, opt, "conversion from %s to %s may change value",
                  type_q(TT, et), tgt_name(c, lt, tb));
    }
}

/* convert_and_check of arithmetic types: -Woverflow for a constant.  True
 * when gcc does not go on to conversion_warning. */
static SrcLoc conv_loc(Conv *x)
{
    Checker *c = x->c;
    SrcLoc l = x->eloc ? x->eloc : x->loc;
    if (x->ci->context == CONV_ARG && !x->ci->loc) {    /* the argument's start,
                                                        * at the macro use in a
                                                        * system header */
        uint32_t t = first_tok(c, x->expr);
        SrcFile *sf;
        l = ctok_loc(c, t);
        sf = srcmgr_file_of(c->sm, l);
        if (sf && sf->system_header && c->u->toks[t].exp)
            l = c->u->toks[t].exp;
    } else {                /* a macro of a system header: where it is used */
        SrcFile *sf = srcmgr_file_of(c->sm, l);
        uint32_t t = first_tok(c, x->expr);
        if (sf && sf->system_header && c->u->toks[t].exp)
            l = c->u->toks[t].exp;
    }
    return l;
}

static bool conv_overflow(Conv *x)
{
    Checker *c = x->c;
    uint32_t e = x->expr;
    TypeId lt = x->type, rt = x->rhstype;
    SrcLoc l = conv_loc(x);
    if (inhibited(c, e, false) || (c->ef[e] & EF_OVERFLOW))
        return true;
    if (!is_int(c, lt) || tkind(c, lt) == TY_BOOL)
        return false;
    if (has_ival(c, e) && int_bits(c, rt) <= 64 && int_bits(c, lt) <= 64) {
        uint64_t v = c->cv[e], r = cexpr_trunc(c, lt, v);
        TypeId us, sg, dummy;
        bool warn = false;
        if (cexpr_fits(c, v, rt, lt))
            return false;
        sign_map(c, lt, &us, &dummy);
        sign_map(c, lt, &dummy, &sg);
        if (!is_signed(c, lt)) {
            if (!cexpr_fits(c, v, rt, sg)) {
                cwarn(c, l, "overflow", "%sconversion from %s to %s "
                      "changes value from '%s' to '%s'",
                      is_signed(c, rt) ? "unsigned " : "", type_q(TT, rt),
                      type_q(TT, lt), vstr(c, rt, v), vstr(c, lt, r));
                return true;
            }
            return false;
        }
        if (!cexpr_fits(c, v, rt, us))
            warn = true;
        else if (c->opt.pedantic &&
                 (!gcc_integer(c, rt) || int_bits(c, rt) != int_bits(c, lt)))
            warn = true;
        if (warn)
            cwarn(c, l, "overflow", "overflow in conversion from %s to %s "
                  "changes value from '%s' to '%s'", type_q(TT, rt),
                  type_q(TT, lt), vstr(c, rt, v), vstr(c, lt, r));
        return warn;
    }
    if (c->ck[e] == K_FLOAT && is_flt(c, rt) && int_bits(c, lt) <= 64) {
        long double f = c->fv.data[c->cv[e]];
        unsigned bits = int_bits(c, lt);
        uint64_t r;
        bool ovf;
        char rb[160];
        if (f != f)
            return false;
        {
            long double hi = 1.0L;
            unsigned k;
            for (k = 0; k < bits - (is_signed(c, lt) ? 1u : 0u); k++)
                hi *= 2.0L;
            /* trunc(f) >= hi, or trunc(f) < lo */
            if (is_signed(c, lt))
                ovf = f >= hi || f <= -hi - 1.0L;
            else
                ovf = f >= hi || f <= -1.0L;
        }
        if (!ovf || !float_to_int(c, f, lt, &r))
            return false;
        cwarn(c, l, "overflow", "overflow in conversion from %s to %s changes "
              "value from '%s' to '%s'", type_q(TT, rt), type_q(TT, lt),
              real_cst_str(c, rb, f, rt), vstr(c, lt, r));
        return true;
    }
    return false;
}

static void conv_arith(Conv *x)
{
    Checker *c = x->c;
    unsigned w = x->ci->lhs_bits;
    if (w && gcc_integer(c, x->type) && w < int_bits(c, x->type) &&
        gcc_integer(c, x->rhstype) && !inhibited(c, x->expr, false) &&
        !(c->ef[x->expr] & EF_OVERFLOW)) {
        /* a bit-field is converted to its own narrow type */
        uint32_t s = strip_paren(c, x->expr);
        bool done = s != NO_NODE && has_ival(c, s) &&
                    int_bits(c, rvt(c, s)) <= 64 &&
                    cexpr_bf_overflow(c, conv_loc(x), s, x->type,
                                      x->rhstype, w);
        if (!done) {
            uc_bw = w;
            conversion_warning(c, conv_loc(x), x->type, x->expr, x->rhstype,
                               true);
            uc_bw = 0;
        }
        return;
    }
    if (!conv_overflow(x))
        conversion_warning(x->c, conv_loc(x), x->type, x->expr, x->rhstype,
                           true);
}

/* convert_and_check of an operand to the type of its operation (build_binary_op,
 * build_conditional_expr); prom: the operand was promoted first. */
static void conv_operand(Checker *c, SrcLoc l, TypeId lt, uint32_t a, bool prom)
{
    ConvInfo ci;
    Conv x;
    TypeId at = prom ? promoted(c, a) : rvt(c, a);
    if (!is_arith(c, at) || is_complex(c, at) || mainv(c, at) == mainv(c, lt))
        return;
    memset(&ci, 0, sizeof ci);
    x.c = c;
    x.ci = &ci;
    x.expr = a;
    x.loc = x.eloc = l;
    x.type = lt;
    x.rhstype = at;
    x.npc = false;
    conv_arith(&x);
}

/* The declared enum type of an enumerator, else the value's type (gcc's
 * original type of the expression). */
static TypeId orig_type(Checker *c, uint32_t e)
{
    uint32_t s = strip_paren(c, e);
    if (s != NO_NODE && ntag(c, s) == N_COND) {
        /* c_parser_conditional_expression: both arms' original types, when
         * they agree */
        uint32_t k[3], n = nkids(c, s, k, 3);
        if (n >= 2) {
            TypeId t1 = orig_type(c, k[n - 2]), t2 = orig_type(c, k[n - 1]);
            if (mainv(c, t1) == mainv(c, t2))
                return t1;
        }
        return rvt(c, s);
    }
    if (s != NO_NODE && ntag(c, s) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        if (ref != SYM_NONE && csym(c, ref)->kind == CS_ENUMCONST)
            return csym(c, ref)->ty;
    }
    return rvt(c, e);
}

bool cexpr_assign_check(Checker *c, uint32_t expr, TypeId lhs,
                        const ConvInfo *ci)
{
    Conv x;
    char m[4][640], q[48];
    const char *T, *R, *fn = ci->fname ? ci->fname : "";
    int ctx = ci->context, pn = ci->parmnum;
    TypeId lt, rt, cl, cr;
    TypeKind kl, kr;
    uint32_t p;
    if (expr == NO_NODE || node_err(c, expr) || is_err(c, lhs))
        return false;
    lt = unqual(c, lhs);
    rt = rvt(c, expr);
    if (is_err(c, rt))
        return false;
    x.c = c;
    x.ci = ci;
    x.expr = expr;
    x.type = lt;
    x.rhstype = rt;
    x.npc = is_npc(c, expr);
    p = c->par[expr];
    switch (ctx) {
    case CONV_ASSIGN:
        x.loc = ci->loc ? ci->loc
                        : p != NO_NODE && ntag(c, p) == N_ASSIGN
                              ? cnode_loc(c, p) : expr_loc(c, expr);
        x.eloc = ci->eloc ? ci->eloc : first_loc(c, expr);
        break;
    case CONV_ARG:
        x.loc = x.eloc = ci->loc ? ci->loc : expr_loc(c, expr);
        break;
    default:
        x.loc = x.eloc = ci->loc ? ci->loc : expr_loc(c, expr);
        break;
    }
    cl = type_canon(TT, lt);
    cr = type_canon(TT, rt);
    kl = tkind(c, cl);
    kr = tkind(c, cr);

    if (cexpr_cxx_compat(c, expr) && kl == TY_ENUM &&
        mainv(c, orig_type(c, expr)) != mainv(c, lt)) {
        sp(m[CONV_ARG], "enum conversion when passing argument %d of '%s' is "
           "invalid in C++", pn, fn);
        if (ci->lhs_bitfield) {
            sp(m[CONV_ASSIGN], "enum conversion in assignment is invalid in "
               "C++");
            sp(m[CONV_INIT], "enum conversion in initialization is invalid in "
               "C++");
        } else {
            sp(m[CONV_ASSIGN], "enum conversion from %s to %s in assignment "
               "is invalid in C++", type_q(TT, rt), type_q(TT, lt));
            sp(m[CONV_INIT], "enum conversion from %s to %s in initialization "
               "is invalid in C++", type_q(TT, rt), type_q(TT, lt));
        }
        sp(m[CONV_RETURN], "enum conversion from %s to %s in return is "
           "invalid in C++", type_q(TT, rt), type_q(TT, lt));
        conv_diag(&x, RK_PED, "c++-compat", m, true);
    }
    if (diag_enabled(c->diag, "enum-conversion")) {
        TypeId ot = orig_type(c, expr);
        if (tkind(c, ot) == TY_ENUM && kl == TY_ENUM &&
            mainv(c, ot) != mainv(c, lt))
            cwarn(c, x.loc, "enum-conversion", "implicit conversion from %s "
                  "to %s", type_q(TT, ot), type_q(TT, lt));
    }
    if (mainv(c, lt) == mainv(c, rt)) {
        if (ci->lhs_bits && gcc_integer(c, lt))
            conv_arith(&x);        /* a bit-field is narrower than its type */
        return true;
    }
    if (kr == TY_VOID) {
        if (ci->warnopt)
            cwarn(c, x.loc, ci->warnopt, "void value not ignored as it ought "
                  "to be");
        else
            cerror(c, x.loc, ci->context == CONV_ARG
                   ? "invalid use of void expression"
                   : "void value not ignored as it ought to be");
        return false;
    }
    if (!complete(c, rt)) {
        incomplete_error(c, x.loc, expr, rt);
        return false;
    }
    if (type_is_arith(TT, cl) && type_is_arith(TT, cr) &&
        kl != TY_VECTOR && kr != TY_VECTOR) {
        if (ci->warnopt && ctx == CONV_ARG)
            builtin_noproto_arg(&x);
        conv_arith(&x);
        return true;
    }
    if (kl == TY_VECTOR && kr == TY_VECTOR)
        return true;
    if (kl == TY_UNION && kr != TY_UNION &&
        (type_record(TT, cl)->flags & RF_TRANSPARENT)) {
        /* gcc: a transparent union accepts any of its members' types
         * (pointer members also take void * and null pointer constants) */
        const Record *tr = type_record(TT, cl);
        for (uint32_t k = 0; k < tr->nfields; k++) {
            TypeId mt = c->tt.fields.data[tr->fields + k].ty;
            TypeId mc = type_canon(TT, mt);
            bool ok = type_compatible(TT, mvt(c, mt), mvt(c, rt));
            if (!ok && tkind(c, mc) == TY_PTR) {
                if (kr == TY_PTR) {
                    TypeId ml = type_canon(TT, type_base(TT, mc));
                    TypeId mr = type_canon(TT, type_base(TT, cr));
                    ok = tkind(c, ml) == TY_VOID || tkind(c, mr) == TY_VOID ||
                         type_compatible(TT, mvt(c, ml), mvt(c, mr));
                } else if (x.npc) {
                    ok = true;
                }
            }
            if (!ok)
                continue;
            if (c->opt.pedantic) {
                SrcFile *sf = ci->fsym ? srcmgr_file_of(c->sm,
                                   csym(c, ci->fsym - 1)->loc) : NULL;
                if (!(sf && sf->system_header))
                    cpedantic(c, x.loc, "ISO C prohibits argument conversion "
                              "to union type");
            }
            return true;
        }
    }
    if ((kl == TY_STRUCT || kl == TY_UNION) && kl == kr &&
        type_compatible(TT, mainv(c, lt), mainv(c, rt)))
        return true;

    if (kl == TY_PTR && kr == TY_PTR) {
        TypeId ttl = type_canon(TT, type_base(TT, cl));
        TypeId ttr = type_canon(TT, type_base(TT, cr));
        TypeId mvl = mvt(c, ttl), mvr = mvt(c, ttr);
        TypeId ul, sl, ur, sr;
        bool opaque = tkind(c, ttl) == TY_VECTOR && tkind(c, ttr) == TY_VECTOR &&
                      type_ent(TT, ttl)->n == type_ent(TT, ttr)->n;
        int target_cmp = 0;
        bool lvoid = is_void(c, ttl) && !(TYPE_QUALS(ttl) & TQ_ATOMIC);
        bool rvoid = is_void(c, ttr) && !(TYPE_QUALS(ttr) & TQ_ATOMIC);
        bool ok = lvoid || rvoid;
        T = type_q(TT, lt);
        R = type_q(TT, rt);
        if (!ok)
            ok = (target_cmp = comp_target(&x, cl, cr)) != 0 || opaque;
        if (!ok) {
            sign_map(c, mvl, &ul, &sl);
            sign_map(c, mvr, &ur, &sr);
            ok = ul == ur && sl == sr &&
                 (TYPE_QUALS(mvl) & TQ_ATOMIC) == (TYPE_QUALS(mvr) & TQ_ATOMIC);
        }
        if (ok) {
            unsigned qnoat = TQ_CONST | TQ_VOLATILE | TQ_RESTRICT;
            if (is_array(c, ttr)) {
                TypeId sr2 = strip_arr(c, ttr), sl2 = strip_arr(c, ttl);
                unsigned miss = TYPE_QUALS(sr2) & ~TYPE_QUALS(sl2);
                if (miss & qnoat) {
                    const char *qs = qual_str(q, miss);
                    sp(m[CONV_ARG], "passing argument %d of '%s' discards '%s' "
                       "qualifier from pointer target type", pn, fn, qs);
                    sp(m[CONV_ASSIGN], "assignment discards '%s' qualifier from "
                       "pointer target type", qs);
                    sp(m[CONV_INIT], "initialization discards '%s' qualifier "
                       "from pointer target type", qs);
                    sp(m[CONV_RETURN], "return discards '%s' qualifier from "
                       "pointer target type", qs);
                    conv_diag(&x, RK_WARN, "discarded-array-qualifiers", m,
                              false);
                }
            } else if (c->opt.pedantic &&
                       ((is_void(c, ttl) && is_func(c, ttr)) ||
                        (is_void(c, ttr) && !x.npc && is_func(c, ttl)))) {
                sp(m[CONV_ARG], "ISO C forbids passing argument %d of '%s' "
                   "between function pointer and 'void *'", pn, fn);
                sp(m[CONV_ASSIGN], "ISO C forbids assignment between function "
                   "pointer and 'void *'");
                sp(m[CONV_INIT], "ISO C forbids initialization between "
                   "function pointer and 'void *'");
                sp(m[CONV_RETURN], "ISO C forbids return between function "
                   "pointer and 'void *'");
                conv_diag(&x, RK_PED, "pedantic", m, true);
            } else if (!is_func(c, ttr) && !is_func(c, ttl)) {
                unsigned warn_ped = TYPE_QUALS(ttr) & qnoat & ~TYPE_QUALS(ttl);
                unsigned warn_q = TYPE_QUALS(ttr) & qnoat &
                                  ~gq(c, ttl);
                /* the qualifiers of an array target live on its elements */
                unsigned miss = gq(c, ttr) & ~gq(c, ttl);
                if (is_array(c, ttl))
                    warn_ped = gq(c, ttr) & qnoat & ~gq(c, ttl);
                /* gcc compares a void source with the array type's own
                 * qualifiers: const void * -> const T (*)[N] discards */
                if (rvoid && is_array(c, ttl) && !lvoid) {
                    miss = TYPE_QUALS(ttr);
                    warn_q = warn_ped = miss & qnoat;
                }
                if (warn_q || (warn_ped && c->opt.pedantic)) {
                    const char *qs = qual_str(q, miss);
                    sp(m[CONV_ARG], "passing argument %d of '%s' discards '%s' "
                       "qualifier from pointer target type", pn, fn, qs);
                    sp(m[CONV_ASSIGN], "assignment discards '%s' qualifier from "
                       "pointer target type", qs);
                    sp(m[CONV_INIT], "initialization discards '%s' qualifier "
                       "from pointer target type", qs);
                    sp(m[CONV_RETURN], "return discards '%s' qualifier from "
                       "pointer target type", qs);
                    conv_diag(&x, RK_PED, "discarded-qualifiers", m, true);
                } else if (warn_ped) {
                    /* pedwarn_c11 before C2X: only when pedantic (above) */
                } else if (is_void(c, ttl) || is_void(c, ttr) || target_cmp) {
                    /* a mismatch in signedness is ignored */
                } else if (diag_enabled(c->diag, "pointer-sign")) {
                    sp(m[CONV_ARG], "pointer targets in passing argument %d of "
                       "'%s' differ in signedness", pn, fn);
                    sp(m[CONV_ASSIGN], "pointer targets in assignment from %s to "
                       "%s differ in signedness", R, T);
                    sp(m[CONV_INIT], "pointer targets in initialization of %s "
                       "from %s differ in signedness", T, R);
                    sp(m[CONV_RETURN], "pointer targets in returning %s from a "
                       "function with return type %s differ in signedness", R, T);
                    conv_diag(&x, RK_PED, "pointer-sign", m, true);
                }
            } else if (is_func(c, ttl) && is_func(c, ttr)) {
                unsigned lq = TYPE_QUALS(ttl) & qnoat, rq = TYPE_QUALS(ttr) & qnoat;
                if (lq & ~rq) {
                    const char *qs = qual_str(q, lq & ~rq);
                    sp(m[CONV_ARG], "passing argument %d of '%s' makes '%s' "
                       "qualified function pointer from unqualified", pn, fn, qs);
                    sp(m[CONV_ASSIGN], "assignment makes '%s' qualified "
                       "function pointer from unqualified", qs);
                    sp(m[CONV_INIT], "initialization makes '%s' qualified "
                       "function pointer from unqualified", qs);
                    sp(m[CONV_RETURN], "return makes '%s' qualified function "
                       "pointer from unqualified", qs);
                    conv_diag(&x, RK_PED, "discarded-qualifiers", m, true);
                }
            }
            /* C++ has no implicit void * -> T *; a null pointer constant
             * (NULL is usually (void *) 0) is tolerated */
            if (rvoid && !x.npc && !lvoid && cexpr_cxx_compat(c, expr))
                cwarn(c, x.loc, "c++-compat", "request for implicit conversion "
                      "from %s to %s not permitted in C++", type_q(TT, rt),
                      type_q(TT, lt));
        } else {
            T = type_q(TT, lt);
            R = type_q(TT, rt);
            sp(m[CONV_ARG], "passing argument %d of '%s' from incompatible "
               "pointer type", pn, fn);
            sp(m[CONV_ASSIGN], "assignment to %s from incompatible pointer "
               "type %s", T, R);
            sp(m[CONV_INIT], "initialization of %s from incompatible pointer "
               "type %s", T, R);
            sp(m[CONV_RETURN], "returning %s from a function with "
               "incompatible return type %s", R, T);
            conv_diag(&x, RK_PED, "incompatible-pointer-types", m, true);
        }
        return true;
    }
    if (kl == TY_PTR && gcc_integer(c, cr)) {
        if (!x.npc) {
            T = type_q(TT, lt);
            R = type_q(TT, rt);
            sp(m[CONV_ARG], "passing argument %d of '%s' makes pointer from "
               "integer without a cast", pn, fn);
            sp(m[CONV_ASSIGN], "assignment to %s from %s makes pointer from "
               "integer without a cast", T, R);
            sp(m[CONV_INIT], "initialization of %s from %s makes pointer from "
               "integer without a cast", T, R);
            sp(m[CONV_RETURN], "returning %s from a function with return type "
               "%s makes pointer from integer without a cast", R, T);
            conv_diag(&x, RK_PED, "int-conversion", m, true);
        }
        return true;
    }
    if (gcc_integer(c, cl) && kr == TY_PTR) {
        T = type_q(TT, lt);
        R = type_q(TT, rt);
        sp(m[CONV_ARG], "passing argument %d of '%s' makes integer from "
           "pointer without a cast", pn, fn);
        sp(m[CONV_ASSIGN], "assignment to %s from %s makes integer from "
           "pointer without a cast", T, R);
        sp(m[CONV_INIT], "initialization of %s from %s makes integer from "
           "pointer without a cast", T, R);
        sp(m[CONV_RETURN], "returning %s from a function with return type %s "
           "makes integer from pointer without a cast", R, T);
        conv_diag(&x, RK_PED, "int-conversion", m, true);
        return true;
    }
    if (kl == TY_BOOL && kr == TY_PTR)
        return true;

    T = type_q(TT, lt);
    R = type_q(TT, rt);
    switch (ctx) {
    case CONV_ARG: {
        Diagnostic *d;
        if (ci->warnopt)
            d = cwarn_d(c, DL_WARNING, x.eloc, ci->warnopt, "incompatible type "
                        "for argument %d of '%s'", pn, fn);
        else
            d = cerror_d(c, x.eloc, "incompatible type for argument %d of '%s'",
                         pn, fn);
        cnote(c, d, conv_note_loc(&x), "expected %s but argument is of type %s",
              type_q(TT, lt), type_q(TT, rt));
        break;
    }
    case CONV_ASSIGN:
        if (ci->warnopt)
            cwarn(c, x.eloc, "", "incompatible types when assigning to type %s "
                  "from type %s", T, R);
        else
            cerror(c, x.eloc, "incompatible types when assigning to type %s "
                   "from type %s", T, R);
        break;
    case CONV_INIT:
        if (ci->warnopt)
            cwarn(c, x.loc, "", "incompatible types when initializing type %s "
                  "using type %s", T, R);
        else
            cerror(c, x.loc, "incompatible types when initializing type %s "
                   "using type %s", T, R);
        break;
    default:
        if (ci->warnopt)
            cwarn(c, x.loc, "", "incompatible types when returning type %s but "
                  "%s was expected", R, T);
        else
            cerror(c, x.loc, "incompatible types when returning type %s but "
                   "%s was expected", R, T);
        break;
    }
    return false;
}

/* gcc's location of a call: EXPR_LOC_OR_LOC of the primary expression the
 * postfix operators start from. */
static SrcLoc call_loc(Checker *c, uint32_t f)
{
    for (;;) {
        unsigned tg = ntag(c, f);
        uint32_t k;
        if (tg != N_CALL && tg != N_INDEX && tg != N_MEMBER_EXPR &&
            tg != N_POSTFIX)
            break;
        k = first_child(c, f);
        if (k == NO_NODE)
            break;
        f = k;
    }
    if (ntag(c, f) == N_PAREN) {
        uint32_t in = strip_paren(c, f);
        if (in != NO_NODE && ntag(c, in) != N_IDENT && ntag(c, in) != N_NUMBER &&
            ntag(c, in) != N_CHAR && ntag(c, in) != N_STRING)
            return expr_loc(c, in);
        return cnode_loc(c, f);
    }
    return cnode_loc(c, f);
}

/* The position of the format argument of the printf-like library functions
 * (gcc's built-in attributes); 0: not one. */
static uint32_t builtin_format_pos(const char *name)
{
    static const struct { const char *n; uint32_t pos; } t[] = {
        {"printf", 1}, {"fprintf", 2}, {"sprintf", 2}, {"snprintf", 3},
        {"dprintf", 2}, {"printf_unlocked", 1}, {"fprintf_unlocked", 2}};
    size_t m;
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    for (m = 0; m < sizeof t / sizeof *t; m++)
        if (!strcmp(name, t[m].n))
            return t[m].pos;
    return 0;
}

/* ---- -Wformat: the conversions of a printf-style literal against its
 * arguments (gcc's check_format_info for gnu_printf). ---- */

enum { FW_INT, FW_FLT, FW_LDBL, FW_PCHAR, FW_PVOID, FW_PINT, FW_ANY,
       FW_PFLT, FW_PPCHAR, FW_PPVOID };
enum { FL_NONE, FL_HH, FL_H, FL_L, FL_LL, FL_BIGL, FL_Z, FL_T, FL_J };

typedef struct FmtWant {
    int shape;
    int cls;                    /* FW_INT, FW_PINT: fmt_cls of the integer */
    char name[40];              /* as the message spells the wanted type */
    bool write;                 /* the argument is written through */
    bool uns;                   /* FW_PINT: an unsigned integer */
} FmtWant;

/* An integer type's width class, signedness ignored (0: not an integer). */
static int fmt_cls(TypeKind k)
{
    switch (k) {
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 1;
    case TY_SHORT: case TY_USHORT: return 2;
    case TY_INT: case TY_UINT: return 3;
    case TY_LONG: case TY_ULONG: return 4;
    case TY_LLONG: case TY_ULLONG: return 5;
    case TY_INT128: case TY_UINT128: return 6;
    default: return 0;
    }
}

static bool fmt_unsigned_kind(TypeKind k)
{
    return k == TY_UCHAR || k == TY_USHORT || k == TY_UINT || k == TY_ULONG ||
           k == TY_ULLONG || k == TY_UINT128;
}

/* What conversion `conv` with length `len` takes; false: no argument.  scan:
 * scanf's (pointers to the objects), mflag: its 'm' allocation flag. */
static bool fmt_want(Checker *c, char conv, int len, bool scan, bool mflag,
                     FmtWant *w)
{
    bool uns = conv == 'o' || conv == 'u' || conv == 'x' || conv == 'X';
    bool ptr = conv == 'n' || scan;
    const char *nm;
    w->cls = 3;
    w->write = ptr;
    w->uns = uns && scan;
    switch (conv) {
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'n':
        w->shape = ptr ? FW_PINT : FW_INT;
        switch (len) {
        case FL_NONE: nm = uns ? "unsigned int" : "int"; break;
        case FL_HH: case FL_H:
            if (ptr) {
                w->cls = len == FL_HH ? 1 : 2;
                nm = len == FL_HH ? (uns ? "unsigned char" : "signed char")
                                  : (uns ? "short unsigned int" : "short int");
            } else {
                nm = "int";
            }
            break;
        case FL_L: w->cls = 4; nm = uns ? "long unsigned int" : "long int";
            break;
        case FL_LL: case FL_BIGL: w->cls = 5;
            nm = uns ? "long long unsigned int" : "long long int";
            break;
        case FL_Z: w->cls = fmt_cls(c->tgt->size_type);
            nm = uns ? "size_t" : "signed size_t"; break;
        case FL_T: w->cls = fmt_cls(c->tgt->ptrdiff_type);
            nm = uns ? "unsigned ptrdiff_t" : "ptrdiff_t"; break;
        default: w->cls = fmt_cls(c->tgt->intmax_type);
            nm = uns ? "uintmax_t" : "intmax_t"; break;
        }
        snprintf(w->name, sizeof w->name, "%s%s", nm, ptr ? " *" : "");
        return true;
    case 'c': case 'C': case '[':
        if (len == FL_L || conv == 'C') {
            w->shape = scan ? FW_PINT : FW_INT;
            w->cls = fmt_cls(scan ? c->tgt->wchar_type : c->tgt->wint_type);
            snprintf(w->name, sizeof w->name, scan ? "wchar_t *" : "wint_t");
        } else if (scan) {
            w->shape = mflag ? FW_PPCHAR : FW_PCHAR;
            snprintf(w->name, sizeof w->name, mflag ? "char **" : "char *");
        } else {
            w->shape = FW_INT;
            snprintf(w->name, sizeof w->name, "int");
        }
        return true;
    case 's': case 'S':
        if (len == FL_L || conv == 'S') {
            w->shape = FW_PINT;
            w->cls = fmt_cls(c->tgt->wchar_type);
            snprintf(w->name, sizeof w->name, "wchar_t *");
        } else {
            w->shape = mflag ? FW_PPCHAR : FW_PCHAR;
            snprintf(w->name, sizeof w->name, mflag ? "char **" : "char *");
        }
        return true;
    case 'p':
        w->shape = scan ? FW_PPVOID : FW_PVOID;
        snprintf(w->name, sizeof w->name, scan ? "void **" : "void *");
        return true;
    case 'm':
        return false;
    default:
        if (scan) {
            w->shape = FW_PFLT;
            w->cls = len == FL_BIGL ? 3 : len == FL_L ? 2 : 1;
            snprintf(w->name, sizeof w->name, "%s *",
                     w->cls == 3 ? "long double" : w->cls == 2 ? "double"
                                                                : "float");
            return true;
        }
        w->shape = len == FL_BIGL ? FW_LDBL : FW_FLT;
        snprintf(w->name, sizeof w->name, "%s",
                 len == FL_BIGL ? "long double" : "double");
        return true;
    }
}

/* The type an argument has when passed: arrays decayed, the default
 * promotions applied (the typedef spelling kept when they change nothing). */
static TypeId fmt_argtype(Checker *c, uint32_t a)
{
    TypeId t = rvt(c, a), p;
    if (type_ckind(TT, t) == TY_FLOAT)
        return TYPE_B(DOUBLE);
    if (is_int(c, t)) {
        p = promoted(c, a);
        if (p != TYPE_UNQUAL(type_canon(TT, t)))
            return p;
    }
    return unqual(c, t);
}

/* Does an argument of type t (from fmt_argtype) satisfy w? */
static bool fmt_arg_ok(Checker *c, const FmtWant *w, TypeId t)
{
    TypeKind k = type_ckind(TT, t), pk, qk;
    TypeId ct, pt;
    switch (w->shape) {
    case FW_INT: return fmt_cls(k) == w->cls;
    case FW_FLT: return k == TY_DOUBLE;
    case FW_LDBL: return k == TY_LDOUBLE;
    case FW_ANY: return true;
    default: break;
    }
    if (k != TY_PTR)
        return false;
    ct = type_canon(TT, t);
    pt = type_canon(TT, type_base(TT, ct));
    pk = type_kind(TT, TYPE_UNQUAL(pt));
    switch (w->shape) {
    case FW_PCHAR: return fmt_cls(pk) == 1;
    case FW_PVOID: return !c->opt.pedantic || pk == TY_VOID || fmt_cls(pk) == 1;
    case FW_PFLT:
        return pk == (w->cls == 3 ? TY_LDOUBLE : w->cls == 2 ? TY_DOUBLE
                                                             : TY_FLOAT);
    case FW_PPCHAR: case FW_PPVOID:
        if (pk != TY_PTR)
            return false;
        qk = type_kind(TT, TYPE_UNQUAL(type_canon(TT, type_base(TT, pt))));
        return w->shape == FW_PPVOID ? qk == TY_VOID : fmt_cls(qk) == 1;
    default:
        if (fmt_cls(pk) != w->cls)
            return false;
        if (!c->opt.pedantic)
            return true;
        if (w->cls == 1)
            return w->uns ? pk == TY_UCHAR : pk == TY_SCHAR;
        return w->uns == fmt_unsigned_kind(pk);
    }
}

typedef struct FmtCtx {
    Checker *c;
    const uint32_t *kv;
    uint32_t nk, ai;            /* the next argument to take (an index in kv) */
    SrcLoc whole, base, call;   /* the literal; its first byte (exact); the call */
    const uint32_t *off;        /* byte offsets in the spelling, if exact */
    bool exact;
    bool va;                    /* a va_list: arguments not checked */
} FmtCtx;

static SrcLoc fmt_loc(const FmtCtx *x, size_t i)
{
    return x->exact ? x->base + x->off[i] : x->whole;
}

/* Takes the next argument for `what` (a description such as "format '%d'"),
 * checking it against w. */
static void fmt_take(FmtCtx *x, const FmtWant *w, SrcLoc loc, const char *what)
{
    Checker *c = x->c;
    uint32_t a;
    TypeId t;
    TypeKind k;
    if (x->va)
        return;
    if (x->ai >= x->nk) {
        cwarn(c, loc, "format=", "%s expects a matching '%s' argument", what,
              w->name);
        return;
    }
    a = x->kv[x->ai];
    if (node_err(c, a) || c->ty[a] == ERRT) {
        x->ai++;
        return;
    }
    t = fmt_argtype(c, a);
    k = type_ckind(TT, t);
    if (w->write && k == TY_PTR &&
        (tquals(c, type_base(TT, type_canon(TT, t))) & TQ_CONST))
        cwarn(c, x->call, "format=", "writing into constant object "
              "(argument %u)", x->ai);
    if ((fmt_cls(k) || k == TY_DOUBLE || k == TY_LDOUBLE || k == TY_PTR ||
         is_record(c, t)) && !fmt_arg_ok(c, w, t))
        cwarn(c, loc, "format=", "%s expects argument of type '%s', but "
              "argument %u has type %s", what, w->name, x->ai, type_q(TT, t));
    x->ai++;
}

/* The bytes of a format literal (its pieces concatenated and unescaped),
 * with each byte's offset in its piece's spelling when the literal is a
 * single piece written in the file. */
static bool fmt_decode(Checker *c, uint32_t s, char **buf, uint32_t **off,
                       size_t *n, bool *exact)
{
    uint32_t np = c->nodes[s].aux ? c->nodes[s].aux : 1, t;
    size_t cap = 1, m = 0, len, j;
    const char *tx;
    for (t = 0; t < np; t++) {
        tx = ttext(c, c->nodes[s].tok + t, &len);
        cap += len;
    }
    *buf = malloc(cap);
    *off = malloc(cap * sizeof **off);
    /* a token a macro expansion made has no substring location in gcc */
    *exact = np == 1 && (!c->u->toks[c->nodes[s].tok].exp ||
                         c->u->toks[c->nodes[s].tok].exp ==
                             c->u->toks[c->nodes[s].tok].t.loc);
    for (t = 0; t < np; t++) {
        tx = ttext(c, c->nodes[s].tok + t, &len);
        if (lit_str_prefix(tx, len) || len < 2)
            goto bad;
        for (j = 1; j + 1 < len; j++) {
            unsigned char ch = (unsigned char)tx[j];
            size_t at = j;
            if (ch >= 0x80 || ch == '\t')
                *exact = false;
            if (ch == '\\') {
                ch = (unsigned char)tx[++j];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'a': ch = '\a'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'v': ch = '\v'; break;
                case 'e': ch = 27; break;
                case '\\': case '\'': case '"': case '?': break;
                case 'x': {
                    unsigned v = 0;
                    while (j + 2 < len && isxdigit((unsigned char)tx[j + 1])) {
                        char d = tx[++j];
                        v = v * 16 + (unsigned)(d <= '9' ? d - '0'
                                                : (d | 32) - 'a' + 10);
                    }
                    ch = (unsigned char)v;
                    break;
                }
                case '0': case '1': case '2': case '3': case '4': case '5':
                case '6': case '7': {
                    unsigned v = (unsigned)(ch - '0'), d = 1;
                    while (d < 3 && j + 2 < len && tx[j + 1] >= '0' &&
                           tx[j + 1] <= '7') {
                        v = v * 8 + (unsigned)(tx[++j] - '0');
                        d++;
                    }
                    ch = (unsigned char)v;
                    break;
                }
                default: goto bad;
                }
            }
            (*buf)[m] = (char)ch;
            (*off)[m++] = (uint32_t)at;
        }
    }
    *n = m;
    return true;
bad:
    free(*buf);
    free(*off);
    return false;
}

/* The conversions of a literal format s of a printf-like (or, scan, scanf-like)
 * function against the arguments kv[first..nk) (first 0: a va_list, the
 * arguments are not checked). */
static void fmt_check(Checker *c, const uint32_t *kv, uint32_t nk,
                      uint32_t first, bool scan, uint32_t s, SrcLoc whole,
                      SrcLoc call)
{
    static const struct { char conv; const char *flags; } ft[] = {
        {'d', "-+ 0'I"}, {'i', "-+ 0'I"}, {'o', "-0#"}, {'x', "-0#"},
        {'X', "-0#"}, {'u', "-0'I"}, {'f', "-0 +#'I"}, {'g', "-0 +#'I"},
        {'G', "-0 +#'I"}, {'e', "-0 +#I"}, {'E', "-0 +#I"}, {'a', "-0 +#I"},
        {'A', "-0 +#I"}, {'F', "-0 +#I"}, {'c', "-"}, {'C', "-"}, {'s', "-"},
        {'S', "-"}, {'p', "-"}, {'n', ""}};
    const char *kname = scan ? "gnu_scanf" : "gnu_printf";
    const char *convs = scan ? "diouxXaAeEfFgGcspnCS[" : "diouxXfFeEgGaAcsCSpnm";
    FmtCtx x;
    char *f;
    uint32_t *off;
    size_t n, i = 0, t, st;
    bool exact, dollar = false;
    if (!diag_enabled(c->diag, "format="))
        return;
    if (!fmt_decode(c, s, &f, &off, &n, &exact))
        return;
    x.c = c;
    x.kv = kv;
    x.nk = nk;
    x.va = first == 0;
    x.ai = first ? first : nk;
    x.whole = whole;
    x.call = call;
    x.exact = false;
    x.base = 0;
    if (exact) {
        size_t len;
        const char *tx = ttext(c, c->nodes[s].tok, &len);
        SrcLoc b = ctok_loc(c, c->nodes[s].tok);
        if (!memcmp(srcmgr_ptr(c->sm, b), tx, len)) {
            x.exact = true;
            x.base = b;
        }
    }
    x.off = off;
    for (t = 0; t < n; t++)
        if (!f[t]) {
            cwarn(c, fmt_loc(&x, t), "format-contains-nul",
                  "embedded '\\0' in format");
            n = t;
            break;
        }
    if (!n) {
        cwarn(c, whole, "format-zero-length", "zero-length %s format string",
              kname);
        goto out;
    }
    while (i < n && !dollar) {
        char seen[128] = {0}, conv, flags[16];
        unsigned nf = 0;
        bool width = false, prec = false, supp = false, mflag = false;
        int len = FL_NONE;
        bool badlen = false;
        char lsp[3] = {0, 0, 0};
        FmtWant w;
        char what[48];
        if (f[i] != '%') {
            i++;
            continue;
        }
        st = i++;
        if (i >= n) {
            cwarn(c, fmt_loc(&x, st), "format=",
                  "spurious trailing '%%' in format");
            break;
        }
        while (i < n && strchr(scan ? "*'m" : "-+ #0'I", f[i])) {
            if (seen[(int)f[i]])
                cwarn(c, fmt_loc(&x, i), "format=",
                      "repeated '%c' flag in format", f[i]);
            else if (nf < sizeof flags - 1)
                flags[nf++] = f[i];
            if (c->opt.pedantic && (f[i] == '\'' || f[i] == 'I'))
                cwarn(c, whole, "format=", "ISO C does not support the '%c' "
                      "%s flag", f[i], scan ? "scanf" : "printf");
            seen[(int)f[i]] = 1;
            supp |= scan && f[i] == '*';
            mflag |= scan && f[i] == 'm';
            i++;
        }
        flags[nf] = 0;
        if (!scan && i < n && f[i] == '*') {
            FmtWant iw = {FW_INT, 3, "int", false, false};
            fmt_take(&x, &iw, fmt_loc(&x, i), "field width specifier '*'");
            width = true;
            i++;
        } else {
            while (i < n && isdigit((unsigned char)f[i])) {
                width = true;
                i++;
            }
            if (width && i < n && f[i] == '$') {
                if (c->opt.pedantic)
                    cwarn(c, call, "format=", "ISO C does not support %%n$ "
                          "operand number formats");
                dollar = true;
                break;
            }
        }
        if (!scan && i < n && f[i] == '.') {
            i++;
            prec = true;
            if (i < n && f[i] == '*') {
                FmtWant iw = {FW_INT, 3, "int", false, false};
                fmt_take(&x, &iw, fmt_loc(&x, i),
                         "field precision specifier '.*'");
                i++;
            } else {
                while (i < n && isdigit((unsigned char)f[i]))
                    i++;
            }
        }
        if (i < n) {
            switch (f[i]) {
            case 'h': len = i + 1 < n && f[i + 1] == 'h' ? FL_HH : FL_H; break;
            case 'l': len = i + 1 < n && f[i + 1] == 'l' ? FL_LL : FL_L; break;
            case 'L': len = FL_BIGL; break;
            case 'q': len = FL_LL; break;
            case 'z': case 'Z': len = FL_Z; break;
            case 't': len = FL_T; break;
            case 'j': len = FL_J; break;
            default: break;
            }
            if (len != FL_NONE) {
                lsp[0] = f[i];
                lsp[1] = len == FL_HH || (len == FL_LL && f[i] == 'l') ? f[i] : 0;
                i += lsp[1] ? 2 : 1;
                if ((f[i - 1] == 'q' || f[i - 1] == 'Z') && c->opt.pedantic)
                    cwarn(c, whole, "format=", "ISO C does not support the "
                          "'%c' %s length modifier", f[i - 1], kname);
            }
        }
        if (i >= n) {
            cwarn(c, fmt_loc(&x, i - 1), "format=",
                  "conversion lacks type at end of format");
            break;
        }
        conv = f[i];
        if (conv == '%') {
            if (i - 1 > st) {
                cwarn(c, fmt_loc(&x, i - 1), "format=",
                      "conversion lacks type at end of format");
                continue;       /* the '%' starts the next conversion */
            }
            i++;
            continue;
        }
        if (!conv || !strchr(convs, conv)) {
            if (isprint((unsigned char)conv))
                cwarn(c, fmt_loc(&x, i), "format=",
                      "unknown conversion type character '%c' in format",
                      conv);
            else
                cwarn(c, fmt_loc(&x, i), "format=", "unknown conversion type "
                      "character '\\x%02x' in format", (unsigned char)conv);
            i++;
            continue;
        }
        snprintf(what, sizeof what, "format '%%%s%s%c'", mflag ? "m" : "", lsp,
                 conv);
        if (mflag && c->opt.pedantic)
            cwarn(c, whole, "format=", "ISO C does not support the 'm' scanf "
                  "flag");
        if (conv == '[') {
            /* a scan set: the conversion is located at its last character */
            size_t j = i + 1, e;
            if (j < n && f[j] == '^')
                j++;
            if (j < n && f[j] == ']')
                j++;
            while (j < n && f[j] != ']')
                j++;
            e = j < n ? j - 1 : n - 1;
            if (j >= n)
                cwarn(c, fmt_loc(&x, e), "format=",
                      "no closing ']' for '%%[' format");
            snprintf(what, sizeof what, "format '%%%s%.*s'", lsp,
                     (int)(e - i + 1), f + i);
            i = e;
        }
        if (!scan) {
            for (t = 0; t < sizeof ft / sizeof *ft; t++)
                if (ft[t].conv == conv)
                    break;
            if (t < sizeof ft / sizeof *ft) {
                bool intc = strchr("diouxX", conv) != NULL;
                if (seen[' '] && seen['+'])
                    cwarn(c, whole, "format=", "' ' flag ignored with '+' flag "
                          "in %s format", kname);
                if (seen['0'] && seen['-'])
                    cwarn(c, whole, "format=", "'0' flag ignored with '-' flag "
                          "in %s format", kname);
                if (seen['0'] && prec && intc)
                    cwarn(c, whole, "format=", "'0' flag ignored with "
                          "precision and '%%%c' %s format", conv, kname);
                for (st = 0; flags[st]; st++)
                    if (!strchr(ft[t].flags, flags[st]))
                        cwarn(c, fmt_loc(&x, i), "format=", "'%c' flag used "
                              "with '%%%c' %s format", flags[st], conv, kname);
                if (width && conv == 'n')
                    cwarn(c, fmt_loc(&x, i), "format=", "field width used "
                          "with '%%%c' %s format", conv, kname);
                if (prec && !strchr("diouxXfFeEgGaAsSn", conv))
                    cwarn(c, fmt_loc(&x, i), "format=", "precision used with "
                          "'%%%c' %s format", conv, kname);
            }
        }
        {
            bool intc = strchr("diouxX", conv) != NULL, lenok;
            if (len == FL_NONE || intc || conv == 'n')
                lenok = true;
            else if (conv == 'c' || conv == 's' || conv == '[')
                lenok = len == FL_L;
            else if (strchr("fFeEgGaA", conv))
                lenok = len == FL_BIGL || len == FL_L;
            else
                lenok = conv == 'C' || conv == 'S';
            if (!lenok) {
                cwarn(c, fmt_loc(&x, i), "format=", "use of '%s' length "
                      "modifier with '%c' type character has either no "
                      "effect or undefined behavior", lsp, conv);
                len = FL_NONE;
                lsp[0] = 0;
                badlen = true;
                snprintf(what, sizeof what, "format '%%%c'", conv);
            }
            if (c->opt.pedantic && (strchr(scan ? "CS" : "mCS", conv) ||
                                    (len == FL_BIGL && intc)))
                cwarn(c, fmt_loc(&x, i), "format=", "ISO C does not support "
                      "the '%%%s%c' %s format", lsp, conv, kname);
        }
        if (supp) {
            i++;
            continue;           /* assignment suppressed: no argument */
        }
        if (fmt_want(c, conv, len, scan, mflag, &w)) {
            if (badlen)
                w.shape = FW_ANY;   /* gcc has no type for it */
            fmt_take(&x, &w, fmt_loc(&x, i), what);
        }
        i++;
    }
    if (!dollar && !x.va && x.ai < nk)
        cwarn(c, whole, "format-extra-args", "too many arguments for format");
out:
    free(f);
    free(off);
}

/* The position of the format argument of the scanf-like library functions. */
static uint32_t builtin_scanf_pos(const char *name)
{
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    if (!strcmp(name, "scanf"))
        return 1;
    return !strcmp(name, "fscanf") || !strcmp(name, "sscanf") ? 2 : 0;
}

/* check_format_info's complaint about a format that is not a string
 * literal: -Wformat-security (or -Wformat-nonliteral) with no arguments to
 * check, -Wformat-nonliteral with some. */
static void check_format_literal(Checker *c, const uint32_t *kv, uint32_t nk,
                                 const CSym *sy, const char *name, SrcLoc loc)
{
    uint32_t pos = 0, first = 0, a, s;
    SrcLoc where = loc;
    bool nonlit = false, scan = false;
    if (sy && sy->fmt) {
        pos = (sy->fmt >> 12) & 0xfff;
        first = sy->fmt & 0xfff;
        scan = (sy->fmt >> 24) == 2;
    } else if ((pos = builtin_format_pos(name))) {
        first = pos + 1;
    } else if ((pos = builtin_scanf_pos(name))) {
        first = pos + 1;
        scan = true;
    }
    if (!pos || nk - 1 < pos)
        return;
    a = kv[pos];
    /* gcc's input_location: the line of the token after the call's ')' */
    where = cinput_loc(c, last_tok(c, kv[nk - 1]) + 2);
    s = strip_paren(c, a);
    if (s == NO_NODE || node_err(c, a))
        return;
    if (ntag(c, s) == N_STRING) {
        fmt_check(c, kv, nk, first, scan, s, expr_loc(c, a), loc);
        return;
    }
    if (type_ckind(TT, c->ty[s]) == TY_ARRAY && c->ck[s] != K_ERR) {
        /* a writable array: its address has a location of its own; a
         * const one is read through its initializer */
        if (!(TYPE_QUALS(type_base(TT, type_canon(TT, c->ty[s]))) & TQ_CONST) &&
            !(c->ck[s] == K_ADDR && (c->cb[s] & CB_NODE))) {
            nonlit = true;
            where = expr_loc(c, a);
        }
    } else if (type_ckind(TT, c->ty[s]) == TY_VLA && c->ck[s] != K_ERR) {
        nonlit = true;          /* never read through an initializer */
        where = expr_loc(c, a);
    } else if (c->ck[s] == K_NONE) {
        nonlit = true;
    }
    if (!nonlit)
        return;
    if (first && nk <= first) {
        const char *opt = !scan && diag_enabled(c->diag, "format-security")
                          ? "format-security" : "format-nonliteral";
        cwarn(c, where, opt, "format not a string literal and no format "
              "arguments");
    } else {
        cwarn(c, where, "format-nonliteral", "format not a string literal, "
              "argument types not checked");
    }
}

/* gcc 13's built-in library functions that carry the nonnull attribute:
 * the argument numbers, one digit each (probed from gcc itself). */
static uint64_t builtin_nonnull(const char *name)
{
    static const struct { const char *n, *pos; } t[] = {
        {"bcmp", "12"}, {"bcopy", "12"}, {"bzero", "1"}, {"fprintf", "12"},
        {"fputc", "2"}, {"fputs", "12"}, {"fscanf", "2"}, {"fwrite", "14"},
        {"index", "1"}, {"memchr", "1"}, {"memcmp", "12"}, {"memcpy", "12"},
        {"memmove", "12"}, {"mempcpy", "12"}, {"memset", "1"}, {"nan", "1"},
        {"nanf", "1"}, {"nanl", "1"}, {"nans", "1"}, {"nansf", "1"},
        {"nansl", "1"}, {"printf", "1"}, {"putc", "2"},
        {"puts_unlocked", "1"}, {"fputc_unlocked", "2"},
        {"fputs_unlocked", "12"}, {"fwrite_unlocked", "14"},
        {"printf_unlocked", "1"}, {"fprintf_unlocked", "12"},
        {"putc_unlocked", "2"},
        {"puts", "1"}, {"rindex", "1"}, {"scanf", "1"}, {"snprintf", "3"},
        {"sprintf", "12"}, {"sscanf", "2"}, {"stpcpy", "12"},
        {"stpncpy", "12"}, {"strcasecmp", "12"}, {"strcat", "12"},
        {"strchr", "1"}, {"strcmp", "12"}, {"strcpy", "12"},
        {"strcspn", "12"}, {"strdup", "1"}, {"strftime", "3"},
        {"strlen", "1"}, {"strncasecmp", "12"}, {"strncat", "12"},
        {"strncmp", "12"}, {"strncpy", "12"}, {"strndup", "1"},
        {"strpbrk", "12"}, {"strrchr", "1"}, {"strspn", "12"},
        {"strstr", "12"}};
    size_t k;
    uint64_t m = 0;
    const char *p;
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    /* {s,u}{add,sub,mul}{,l,ll}_overflow: the result pointer */
    if ((*name == 's' || *name == 'u') &&
        (!strncmp(name + 1, "add", 3) || !strncmp(name + 1, "sub", 3) ||
         !strncmp(name + 1, "mul", 3))) {
        const char *q = name + 4;
        if (!strncmp(q, "ll", 2))
            q += 2;
        else if (*q == 'l')
            q++;
        if (!strcmp(q, "_overflow"))
            return 4;
    }
    for (k = 0; k < sizeof t / sizeof *t; k++)
        if (*name == *t[k].n && !strcmp(name, t[k].n)) {
            for (p = t[k].pos; *p; p++)
                m |= (uint64_t)1 << (*p - '1');
            break;
        }
    return m;
}

/* gcc's match_builtin_function_types, as probed from gcc 13.  Compare the
 * declared type n with the built-in's o in one position (ret: the return
 * type): 0 equal, 1 a mismatch gcc only warns about ("mismatch in argument N
 * type"), 2 a conflict.  A FILE * parameter of the table is a void * that
 * accepts any pointer. */
static int bt_cmp(Checker *c, TypeId o, TypeId n, bool ret, bool file)
{
    TypeId uo = unqual(c, o), un = unqual(c, n);
    if (is_ptr(c, o) || is_ptr(c, n)) {
        TypeId po, pn;
        if (!is_ptr(c, o) || !is_ptr(c, n))
            return 2;
        if (file)
            return 0;
        po = pointee(c, o);
        pn = pointee(c, n);
        if (type_compatible(TT, po, pn))
            return 0;
        if (ret || type_compatible(TT, unqual(c, po), unqual(c, pn)))
            return 1;
        return 2;
    }
    if (type_compatible(TT, uo, un))
        return 0;
    if (is_int(c, o) && is_int(c, n) && tkind(c, uo) != TY_BOOL &&
        tkind(c, un) != TY_BOOL) {
        bool ok1 = true, ok2 = true;
        return type_size(TT, uo, &ok1) == type_size(TT, un, &ok2) ? 1 : 2;
    }
    return 2;
}

/* Is parameter j (0-based) of the library built-in a FILE * or struct tm *
 * (a void * to gcc until the type is declared)? */
static bool extra_on(Checker *c);
static bool bt_file_param(const char *name, uint32_t j)
{
    static const struct { const char *n; unsigned char j; } t[] = {
        {"fprintf", 0}, {"fscanf", 0}, {"vfprintf", 0}, {"vfscanf", 0},
        {"fputc", 1}, {"fputs", 1}, {"putc", 1}, {"fwrite", 3},
        {"fprintf_unlocked", 0}, {"fputc_unlocked", 1},
        {"fputs_unlocked", 1}, {"fwrite_unlocked", 3},
        {"putc_unlocked", 1}, {"strftime", 3}};
    size_t k;
    for (k = 0; k < sizeof t / sizeof *t; k++)
        if (t[k].j == j && !strcmp(t[k].n, name))
            return true;
    return false;
}

typedef struct BtMatch {
    bool conflict;           /* gcc: "conflicting types for built-in function" */
    int soft;                /* the first soft mismatch: 0 none, 1 return, j + 2 */
    TypeId bft, dft;
} BtMatch;

static BtMatch bt_match(Checker *c, const BTab *bt, TypeId declty)
{
    BtMatch m;
    uint32_t n, j;
    const TypeId *bp, *dp;
    int r;
    memset(&m, 0, sizeof m);
    m.dft = type_canon(TT, declty);
    m.bft = type_canon(TT, bt_func_type(c, bt));
    if (type_ent(TT, m.dft)->kind != TY_FUNC ||
        type_ent(TT, m.bft)->kind != TY_FUNC)
        return m;
    r = bt_cmp(c, type_base(TT, m.bft), type_base(TT, m.dft), true, false);
    if (r == 2) {
        m.conflict = true;
        return m;
    }
    if (r)
        m.soft = 1;
    if (type_ent(TT, m.dft)->flags & TF_NOPROTO) {
        m.conflict = (type_ent(TT, m.bft)->flags & TF_VARIADIC) != 0;
        bp = type_params(TT, m.bft);
        for (j = 0; j < (uint32_t)type_ent(TT, m.bft)->n; j++) {
            TypeId pj = unqual(c, bp[j]);
            if ((is_flt(c, pj) && tkind(c, pj) == TY_FLOAT) ||
                (is_int(c, pj) && mainv(c, type_int_promote(TT, pj)) != mainv(c, pj)))
                m.conflict = true;     /* the argument would be promoted */
        }
        return m;
    }
    n = (uint32_t)type_ent(TT, m.dft)->n;
    if (n != (uint32_t)type_ent(TT, m.bft)->n ||
        (type_ent(TT, m.dft)->flags & TF_VARIADIC) !=
            (type_ent(TT, m.bft)->flags & TF_VARIADIC)) {
        m.conflict = true;
        return m;
    }
    dp = type_params(TT, m.dft);
    bp = type_params(TT, m.bft);
    for (j = 0; j < n; j++) {
        r = bt_cmp(c, bp[j], dp[j], false, bt_file_param(bt->name, j));
        if (r == 2) {
            m.conflict = true;
            return m;
        }
        if (r && !m.soft)
            m.soft = (int)j + 2;
    }
    return m;
}

static const BTab *bt_for_decl(Checker *c, const CSym *s)
{
    const char *n = cident(c, s->name);
    return bt_find(c, !strncmp(n, "__builtin_", 10) ? n + 10 : n,
                   !strncmp(n, "__builtin_", 10));
}

static bool builtin_decl_ok(Checker *c, const CSym *s)
{
    const BTab *bt = bt_for_decl(c, s);
    if (!bt || !strcmp(strchr(bt->sig, '|') + 1, "?") || (s->flags & CSF_IMPLICIT))
        return true;
    return !bt_match(c, bt, s->ty).conflict;
}

/* gcc prints a built-in's type: 'ret(a, b)', with a second space after an
 * argument that is not a pointer. */
static void bt_sig_print(StrBuf *sb, const char *sig)
{
    const char *p = strchr(sig, '|'), *q;
    bool first = true, prev_ptr = true;
    sb_putn(sb, sig, (size_t)(p - sig));
    sb_putc(sb, '(');
    for (p++; *p; p = *q ? q + 1 : q) {
        q = strchr(p, '|');
        if (!q)
            q = p + strlen(p);
        if (!first)
            sb_puts(sb, prev_ptr ? ", " : ",  ");
        sb_putn(sb, p, (size_t)(q - p));
        prev_ptr = q > p && q[-1] == '*';
        first = false;
    }
    if (first)
        sb_puts(sb, "void");
    sb_putc(sb, ')');
}

/* -Wbuiltin-declaration-mismatch for the first declaration of a library
 * built-in (gcc's diagnose_mismatched_decls on the undeclared built-in). */
void cexpr_builtin_decl(Checker *c, const CSym *s)
{
    const BTab *bt = bt_for_decl(c, s);
    const char *dn = cident(c, s->name);
    BtMatch m;
    StrBuf sb;
    const char *p;
    Diagnostic *d = NULL;
    if (!bt || !strcmp(strchr(bt->sig, '|') + 1, "?") || (s->flags & CSF_IMPLICIT) ||
        !diag_enabled(c->diag, "builtin-declaration-mismatch"))
        return;
    m = bt_match(c, bt, s->ty);
    memset(&sb, 0, sizeof sb);
    if (m.conflict) {
        bt_sig_print(&sb, bt->sig);
        d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                    "conflicting types for built-in function '%s'; expected "
                    "'%s'", dn, sb_cstr(&sb));
    } else if (m.soft && extra_on(c)) {
        uint32_t j = (uint32_t)m.soft - 2;
        p = strchr(bt->sig, '|');
        if (m.soft == 1) {
            sb_putn(&sb, bt->sig, (size_t)(p - bt->sig));
            d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                        "mismatch in return type of built-in function '%s'; "
                        "expected '%s'", dn, sb_cstr(&sb));
        } else {
            const char *q;
            for (p++; j; j--)
                p = strchr(p, '|') + 1;
            q = strchr(p, '|');
            sb_putn(&sb, p, q ? (size_t)(q - p) : strlen(p));
            d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                        "mismatch in argument %d type of built-in function "
                        "'%s'; expected '%s'", m.soft - 1, dn,
                        sb_cstr(&sb));
        }
    } else if ((type_ent(TT, m.dft)->flags & TF_NOPROTO) && extra_on(c) &&
               type_ent(TT, m.dft)->kind == TY_FUNC && type_ent(TT, m.bft)->n) {
        bt_sig_print(&sb, bt->sig);
        d = cwarn_d(c, DL_WARNING, s->loc, "builtin-declaration-mismatch",
                    "declaration of built-in function '%s' without a "
                    "prototype; expected '%s'", dn, sb_cstr(&sb));
    }
    if (d && bt->hdr[0])
        cnote(c, d, header_note_loc(c, s->loc, bt->hdr), "'%s' is declared in "
              "header '%s'", bt->name, bt->hdr);
    sb_free(&sb);
}

/* A constant null pointer as gcc's integer_zerop sees it: argument a of a
 * nonnull parameter, looking into the arms of ?: and the value of a comma. */
static void nonnull_arg(Checker *c, uint32_t a, uint32_t parm, bool ptr, SrcLoc loc)
{
    uint32_t k[3], n;
    a = strip_paren(c, a);
    if (a == NO_NODE || node_err(c, a))
        return;
    if (((c->ef[a] & EF_NPC) && (ptr || is_ptr(c, c->ty[a]))) ||
        (c->ck[a] == K_ADDR && !c->cb[a] && c->cv[a] == 0 && is_ptr(c, c->ty[a])) ||
        (ptr && has_ival(c, a) && c->cv[a] == 0 &&
         !(ntag(c, a) == N_BINARY && npunct(c, a) == P_COMMA))) {
        cwarn(c, loc, "nonnull", "argument %u null where non-null expected",
              parm);
        return;
    }
    switch (ntag(c, a)) {
    case N_COND:
        n = nkids(c, a, k, 3);
        if (n < 2)
            return;
        if (has_ival(c, k[0])) {
            if (c->cv[k[0]] != 0)
                nonnull_arg(c, n == 3 ? k[1] : k[0], parm, ptr, loc);
            else
                nonnull_arg(c, k[n - 1], parm, ptr, loc);
        } else {
            nonnull_arg(c, n == 3 ? k[1] : k[0], parm, ptr, loc);
            nonnull_arg(c, k[n - 1], parm, ptr, loc);
        }
        break;
    case N_BINARY:
        if (npunct(c, a) == P_COMMA && nkids(c, a, k, 2) == 2)
            nonnull_arg(c, k[1], parm, ptr && is_ptr(c, c->ty[a]), loc);
        break;
    case N_CAST:
        if (is_ptr(c, c->ty[a]) && nkids(c, a, k, 2) == 2)
            nonnull_arg(c, k[1], parm, false, loc);
        break;
    default:
        break;
    }
}

/* -Wnonnull for a call's arguments kv[1..nk) against the mask (a declared
 * attribute or a built-in's); pt/nparm: the prototype, if any. */
static void check_nonnull(Checker *c, const uint32_t *kv, uint32_t nk,
                          uint64_t mask, const TypeId *pt, uint32_t nparm,
                          bool proto, bool builtin, SrcLoc loc)
{
    uint32_t j;
    for (j = 0; j + 1 < nk && j < 63; j++) {
        uint32_t a = kv[j + 1];
        bool ptr;
        if (node_err(c, a))
            continue;
        ptr = builtin || (proto && j < nparm ? is_ptr(c, pt[j]) : is_ptr(c, rvt(c, a)));
        if (!((mask >> j) & 1) && !((mask & NN_ALL) && ptr))
            continue;
        nonnull_arg(c, a, j + 1, ptr, loc);
    }
}

static bool zero_size_ok(const char *name);
static void check_restrict(Checker *c, const uint32_t *kv, uint32_t nk,
                           uint32_t parms, uint32_t nparm, SrcLoc loc,
                           bool builtin);

/* convert_arguments: the arguments of call i (callee node fn, of pointer to
 * function type ft) against the prototype.  False if the call is erroneous. */
static bool call_args(Checker *c, uint32_t i, uint32_t fn, TypeId ft)
{
    uint32_t buf[32], *kv = buf, nk = nkids(c, i, buf, 32), j;
    TypeId fty = type_canon(TT, pointee(c, ft));
    const TypeEnt *fe = type_ent(TT, fty);
    bool proto = !(fe->flags & TF_NOPROTO), variadic = (fe->flags & TF_VARIADIC) != 0;
    bool bad = false, too_many = false;
    uint32_t fnode = strip_paren(c, fn), fref = SYM_NONE, nparm = (uint32_t)fe->n;
    TypeId ufty = pointee(c, ft);
    const TypeId *pt, *bpt = NULL;
    uint32_t bn = NO_NODE;
    char fname[256];
    SrcLoc loc = call_loc(c, fn);
    /* the declared parameter types keep their typedef spellings */
    while (type_ent(TT, ufty)->kind == TY_TYPEDEF)
        ufty = type_ent(TT, ufty)->base;
    pt = type_params(TT, type_ent(TT, ufty)->kind == TY_FUNC ? ufty : fty);
    if (nk > 32) {
        kv = xmalloc(nk * sizeof *kv);
        nkids(c, i, kv, nk);
    }
    if (fnode != NO_NODE && ntag(c, fnode) == N_IDENT && c->ck[fnode] == K_ADDR &&
        c->cb[fnode] && !(c->cb[fnode] & CB_NODE) &&
        csym(c, c->cb[fnode] - 1)->kind == CS_FUNC)
        fref = c->cb[fnode] - 1;
    {
        uint32_t show = fnode;
        if (show != NO_NODE && ntag(c, show) == N_UNARY &&
            npunct(c, show) == P_AMP && first_child(c, show) != NO_NODE &&
            is_func(c, c->ty[first_child(c, show)]))
            show = strip_paren(c, first_child(c, show));
        snprintf(fname, sizeof fname, "%s", show == NO_NODE ? "" : estr(c, show));
    }
    if (!proto && fref != SYM_NONE) {
        bool impl = (csym(c, fref)->flags & CSF_IMPLICIT) != 0;
        const BTab *bt = bt_find(c, cident(c, csym(c, fref)->name), false);
        if (bt && (impl ? !bt->mismatch : builtin_decl_ok(c, csym(c, fref))) &&
            strcmp(strchr(bt->sig, '|') + 1, "?")) {
            TypeId bft = type_canon(TT, bt_func_type(c, bt));
            bn = (uint32_t)type_ent(TT, bft)->n;
            bpt = type_params(TT, bft);
        }
    }
    for (j = 0; j + 1 < nk; j++) {
        uint32_t a = kv[j + 1];
        bool have = proto && j < nparm;
        if (!have && proto && !variadic) {
            Diagnostic *d = cerror_d(c, loc, "too many arguments to function "
                                     "'%s'", fname);
            if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
                cnote(c, d, csym(c, fref)->loc, "declared here");
            too_many = true;
            break;
        }
        if (node_err(c, a)) {
            bad = true;
            continue;
        }
        if (!rvalue_ok(c, a)) {
            bad = true;
            continue;
        }
        if (have) {
            ConvInfo ci;
            if (!complete(c, pt[j])) {
                cerror(c, expr_loc(c, a), "type of formal parameter %u is "
                       "incomplete", j + 1);
                continue;
            }
            memset(&ci, 0, sizeof ci);
            ci.context = CONV_ARG;
            ci.fname = fname;
            ci.parmnum = (int)j + 1;
            if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
                ci.fsym = fref + 1;
            if (!cexpr_assign_check(c, a, pt[j], &ci))
                bad = true;
        } else if (is_void(c, rvt(c, a))) {
            cerror(c, expr_loc(c, a), "invalid use of void expression");
            bad = true;
        } else if (bn != NO_NODE && bn != 0xFFFFFFFEu) {
            /* a built-in declared without a prototype: the arguments are
             * matched against the built-in's own parameters */
            if (j >= bn) {
                cwarn(c, loc, "builtin-declaration-mismatch", "too many "
                      "arguments to built-in function '%s' expecting %u",
                      fname, bn);
                bn = 0xFFFFFFFEu;
            } else {
                ConvInfo ci;
                memset(&ci, 0, sizeof ci);
                ci.context = CONV_ARG;
                ci.fname = fname;
                ci.parmnum = (int)j + 1;
                ci.warnopt = "builtin-declaration-mismatch";
                (void)cexpr_assign_check(c, a, bpt[j], &ci);
            }
        }
    }
    if (!too_many && !bad && fref != SYM_NONE) {
        uint64_t mask = csym(c, fref)->nonnull;
        if (builtin_decl_ok(c, csym(c, fref)))
            mask |= builtin_nonnull(cident(c, csym(c, fref)->name));
        /* (types made meanwhile may have moved the parameter arrays) */
        pt = type_params(TT, type_ent(TT, ufty)->kind == TY_FUNC ? ufty : fty);
        if (mask)
            check_nonnull(c, kv, nk, mask, pt, nparm, proto, false, loc);
        if (csym(c, fref)->fmt || builtin_decl_ok(c, csym(c, fref)))
            check_format_literal(c, kv, nk, csym(c, fref),
                                 cident(c, csym(c, fref)->name), loc);
    }
    if (!too_many && !bad && proto && nparm > 1 && fref != SYM_NONE &&
        csym(c, fref)->parms)
        check_restrict(c, kv, nk, csym(c, fref)->parms, nparm, loc,
                       builtin_decl_ok(c, csym(c, fref)) &&
                       zero_size_ok(cident(c, csym(c, fref)->name)));
    if (!too_many && !proto && bn != NO_NODE && bn != 0xFFFFFFFEu &&
        nk - 1 < bn)
        cwarn(c, loc, "builtin-declaration-mismatch", "too few arguments to "
              "built-in function '%s' expecting %u", fname, bn);
    if (!too_many && proto && nk - 1 < nparm) {
        Diagnostic *d = cerror_d(c, loc, "too few arguments to function '%s'",
                                 fname);
        if (fref != SYM_NONE && !(csym(c, fref)->flags & CSF_IMPLICIT))
            cnote(c, d, csym(c, fref)->loc, "declared here");
        bad = true;
    }
    if (kv != buf)
        free(kv);
    return !bad;
}

/* handle_cleanup_attribute: the call fn(&var) of the cleanup function fsym on
 * an automatic variable of type vty.  dloc is the declarator, iloc gcc's
 * input_location. */
void cexpr_cleanup_call(Checker *c, uint32_t fsym, TypeId vty, SrcLoc dloc,
                        SrcLoc il)
{
    CSym *f = csym(c, fsym);
    TypeId fty = type_canon(TT, f->ty), at, pt, cp, ca;
    const TypeEnt *fe;
    char fname[256];
    Diagnostic *d;
    if (tkind(c, fty) != TY_FUNC)
        return;
    fe = type_ent(TT, fty);
    if (fe->flags & TF_NOPROTO)
        return;
    snprintf(fname, sizeof fname, "%s", cident(c, f->name));
    if (fe->n == 0 || (fe->n > 1 && !(fe->flags & TF_VARIADIC))) {
        d = cerror_d(c, dloc, fe->n ? "too few arguments to function '%s'"
                                    : "too many arguments to function '%s'",
                     fname);
        cnote(c, d, f->loc, "declared here");
        return;
    }
    pt = type_params(TT, fty)[0];
    at = type_ptr(TT, vty);
    cp = type_canon(TT, unqual(c, pt));
    ca = type_canon(TT, at);
    if (mainv(c, cp) == mainv(c, ca))
        return;
    if (tkind(c, cp) == TY_PTR) {
        TypeId tl = type_canon(TT, type_base(TT, cp));
        if (is_void(c, tl) || type_compatible(TT, mvt(c, tl),
                mvt(c, type_canon(TT, type_base(TT, ca)))))
            return;
        d = cwarn_d(c, DL_WARNING, il, "incompatible-pointer-types", "passing "
                    "argument 1 of '%s' from incompatible pointer type", fname);
    } else if (gcc_integer(c, cp))
        d = cwarn_d(c, DL_WARNING, il, "int-conversion", "passing argument 1 "
                    "of '%s' makes integer from pointer without a cast", fname);
    else
        return;
    if (d)
        cnote(c, d, param_loc(c, fsym, 0, NO_NODE, il),
              "expected %s but argument is of type %s", type_q(TT, pt),
              type_q(TT, at));
}

/* ARG_LOCATION: the expression's location, at the macro use when its first
 * token comes from an expansion. */
static SrcLoc arg_loc(Checker *c, uint32_t a)
{
    uint32_t t = first_tok(c, a);
    return c->u->toks[t].exp ? c->u->toks[t].exp : expr_loc(c, a);
}

/* The argument count of an __atomic_* built-in (sized variants _1.._16
 * included); 0 for the others. */
static uint32_t atomic_argc(const char *name)
{
    static const struct { const char *n; uint32_t argc; } t[] = {
        {"load_n", 2}, {"load", 3}, {"store_n", 3}, {"store", 3},
        {"exchange_n", 3}, {"exchange", 4}, {"compare_exchange_n", 6},
        {"compare_exchange", 6}, {"add_fetch", 3}, {"sub_fetch", 3},
        {"and_fetch", 3}, {"xor_fetch", 3}, {"or_fetch", 3},
        {"nand_fetch", 3}, {"fetch_add", 3}, {"fetch_sub", 3},
        {"fetch_and", 3}, {"fetch_xor", 3}, {"fetch_or", 3},
        {"fetch_nand", 3}, {"test_and_set", 2}, {"clear", 2},
        {"thread_fence", 1}, {"signal_fence", 1}};
    char b[32];
    size_t n, len;
    if (strncmp(name, "__atomic_", 9))
        return 0;
    snprintf(b, sizeof b, "%s", name + 9);
    len = strlen(b);
    while (len && b[len - 1] >= '0' && b[len - 1] <= '9')
        b[--len] = 0;
    if (len < strlen(name + 9) && len && b[len - 1] == '_')
        b[--len] = 0;
    for (n = 0; n < sizeof t / sizeof *t; n++)
        if (!strcmp(b, t[n].n))
            return t[n].argc;
    return 0;
}

/* The __atomic_ and __sync_ built-ins gcc resolves itself: 1 the generic
 * (void *-based) __atomic_load/store/exchange/compare_exchange, 2 the _n and
 * __sync ones sized by the first argument, 3 those same for fetch operations
 * (a _Bool operand is refused); 0 the others. */
static int atomic_kind(const char *name)
{
    static const char *const ops[] = {"add", "sub", "and", "nand", "xor", "or"};
    const char *b;
    size_t k, n;
    if (!strncmp(name, "__sync_", 7)) {
        b = name + 7;
        if (!strcmp(b, "bool_compare_and_swap") ||
            !strcmp(b, "val_compare_and_swap") ||
            !strcmp(b, "lock_test_and_set") || !strcmp(b, "lock_release"))
            return 2;
        for (k = 0; k < sizeof ops / sizeof *ops; k++) {
            n = strlen(ops[k]);
            if (!strncmp(b, "fetch_and_", 10) && !strcmp(b + 10, ops[k]))
                return 3;
            if (!strncmp(b, ops[k], n) && !strcmp(b + n, "_and_fetch"))
                return 3;
        }
        return 0;
    }
    if (strncmp(name, "__atomic_", 9))
        return 0;
    b = name + 9;
    if (!strcmp(b, "load") || !strcmp(b, "store") || !strcmp(b, "exchange") ||
        !strcmp(b, "compare_exchange"))
        return 1;
    if (!strcmp(b, "load_n") || !strcmp(b, "store_n") ||
        !strcmp(b, "exchange_n") || !strcmp(b, "compare_exchange_n"))
        return 2;
    for (k = 0; k < sizeof ops / sizeof *ops; k++) {
        n = strlen(ops[k]);
        if (!strncmp(b, "fetch_", 6) && !strcmp(b + 6, ops[k]))
            return 3;
        if (!strncmp(b, ops[k], n) && !strcmp(b + n, "_fetch"))
            return 3;
    }
    return 0;
}

static bool vla_pointee(Checker *c, TypeId e)
{
    e = type_canon(TT, e);
    while (tkind(c, e) == TY_ARRAY)
        e = type_canon(TT, type_base(TT, e));
    return tkind(c, e) == TY_VLA ||
           ((tkind(c, e) == TY_STRUCT || tkind(c, e) == TY_UNION) &&
            (type_record(TT, e)->flags & RF_VLA));
}

/* sync_resolve_size / get_atomic_generic_size: the argument checks gcc makes
 * while resolving an overloaded atomic or sync built-in.  a: the
 * arguments, n their count.  False after an error. */
static bool atomic_args_ok(Checker *c, uint32_t i, const uint32_t *a,
                           uint32_t n, const char *name, SrcLoc loc)
{
    int kind = atomic_kind(name);
    uint32_t k;
    TypeId t0, e0;
    bool ok;
    uint64_t sz0;
    if (!kind || n == 0)
        return true;
    for (k = 0; k < n; k++)
        if (node_err(c, a[k]))
            return true;
    t0 = rvt(c, a[0]);
    if (kind != 1) {
        TypeId e = is_ptr(c, t0) ? pointee(c, t0) : ERRT;
        uint64_t sz = 0;
        bool good = is_ptr(c, t0) &&
                    (is_int(c, e) || is_ptr(c, e)) &&
                    type_is_complete(TT, e) &&
                    !(kind == 3 && tkind(c, e) == TY_BOOL);
        if (good) {
            sz = type_size(TT, e, &ok);
            good = ok && (sz == 1 || sz == 2 || sz == 4 || sz == 8 || sz == 16);
        }
        if (!good) {
            cerror(c, cdecl_iloc(c, last_tok(c, i) + 1), "operand type %s is incompatible with "
                   "argument 1 of '%s'", type_q(TT, t0), name);
            return false;
        }
        return true;
    }
    /* the generic functions */
    if (!is_ptr(c, t0) || is_void(c, pointee(c, t0))) {
        cerror(c, loc, "argument 1 of '%s' must be a non-void pointer type",
               name);
        return false;
    }
    e0 = pointee(c, t0);
    if (!type_is_complete(TT, e0)) {
        cerror(c, loc, "argument 1 of '%s' must be a pointer to a complete "
               "type", name);
        return false;
    }
    if (vla_pointee(c, e0)) {
        cerror(c, loc, "argument 1 of '%s' must be a pointer to a constant "
               "size type", name);
        return false;
    }
    sz0 = type_size(TT, e0, &ok);
    if (!sz0) {
        cerror(c, loc, "argument 1 of '%s' must be a pointer to a nonzero "
               "size object", name);
        return false;
    }
    {
        const char *b = name + 9;
        unsigned nparam = !strcmp(b, "exchange") ? 4 :
                          !strcmp(b, "compare_exchange") ? 6 : 3;
        unsigned nmodel = nparam == 6 ? 2 : 1;
        unsigned outputs = !strcmp(b, "exchange") ? 5 :
                           !strcmp(b, "load") ? 2 :
                           !strcmp(b, "store") ? 1 : 3;
        unsigned x;
        for (x = 0; x < nparam - nmodel; x++) {
            TypeId t = rvt(c, a[x]), e;
            uint64_t sz;
            unsigned q;
            if (nparam == 6 && x == 3)
                continue;
            if (!is_ptr(c, t)) {
                cerror(c, loc, "argument %u of '%s' must be a pointer type",
                       x + 1, name);
                return false;
            }
            e = pointee(c, t);
            if (vla_pointee(c, e)) {
                cerror(c, loc, "argument %u of '%s' must be a pointer to a "
                       "constant size type", x + 1, name);
                return false;
            }
            if (is_func(c, e)) {
                cerror(c, loc, "argument %u of '%s' must not be a pointer to "
                       "a function", x + 1, name);
                return false;
            }
            sz = type_is_complete(TT, e) ? type_size(TT, e, &ok) : 0;
            if (sz != sz0) {
                cerror(c, loc, "size mismatch in argument %u of '%s'", x + 1,
                       name);
                return false;
            }
            q = tquals(c, e);
            if ((outputs & (1u << x)) && (q & TQ_CONST))
                cwarn(c, loc, "incompatible-pointer-types", "argument %u of "
                      "'%s' discards 'const' qualifier", x + 1, name);
            if (x > 0 && (q & TQ_VOLATILE))
                cwarn(c, loc, "incompatible-pointer-types", "argument %u of "
                      "'%s' discards 'volatile' qualifier", x + 1, name);
        }
        for (x = nparam - nmodel; x < nparam; x++) {
            if (!is_int(c, rvt(c, a[x]))) {
                cerror(c, loc, "non-integer memory model argument %u of '%s'",
                       x + 1, name);
                return false;
            }
            if (c->ck[a[x]] == K_ICE && (c->cv[a[x]] & 0xffff) >= 6)
                cwarn(c, loc, "invalid-memory-model", "invalid memory model "
                      "argument %u of '%s'", x + 1, name);
        }
    }
    return true;
}

/* check_builtin_function_arguments, for the built-ins gcc validates itself:
 * argument counts and the argument kinds.  False after an error. */
static bool builtin_args_ok(Checker *c, uint32_t i, uint32_t fn,
                            const char *name)
{
    uint32_t all[16], *a = all + 1, n = nkids(c, i, all, 16), k, want = 0;
    const char *b = name + 10;
    SrcLoc loc = call_loc(c, fn);
    bool ovf = false, ovfp = false, fp1 = false, cmp = false;
    if (n == 0 || n > 16)
        return true;
    n--;
    if ((want = atomic_argc(name)) != 0) {
        if (n != want) {
            if (atomic_kind(name) == 1)
                cerror(c, loc, "incorrect number of arguments to function "
                       "'%s'", name);
            else
                cerror(c, loc, n < want ? "too few arguments to function "
                       "'%s'" : "too many arguments to function '%s'",
                       name);
            return false;
        }
        return atomic_args_ok(c, i, a, n, name, loc);
    }
    if (!strncmp(name, "__sync_", 7))
        return atomic_args_ok(c, i, a, n, name, loc);
    if (strncmp(name, "__builtin_", 10))
        return true;
    if (!strcmp(b, "constant_p")) {
        want = 1;
    } else if (!strcmp(b, "alloca_with_align")) {
        want = 2;
    } else if (!strcmp(b, "alloca_with_align_and_max")) {
        want = 3;
    } else if (!strcmp(b, "assume_aligned")) {
        want = n > 2 ? 3 : 2;
    } else if (!strcmp(b, "fpclassify")) {
        want = 6;
    } else if (!strcmp(b, "va_start")) {
        if (n == 0) {
            cerror(c, loc, "too few arguments to function '%s'", name);
            return false;
        }
        return true;
    } else if (!strcmp(b, "isfinite") || !strcmp(b, "isinf_sign") ||
               !strcmp(b, "isinf") || !strcmp(b, "isnan") ||
               !strcmp(b, "isnormal") || !strcmp(b, "signbit")) {
        want = 1;
        fp1 = true;
    } else if (!strcmp(b, "isgreater") || !strcmp(b, "isgreaterequal") ||
               !strcmp(b, "isless") || !strcmp(b, "islessequal") ||
               !strcmp(b, "islessgreater") || !strcmp(b, "isunordered")) {
        want = 2;
        cmp = true;
    } else if (!strcmp(b, "add_overflow") || !strcmp(b, "sub_overflow") ||
               !strcmp(b, "mul_overflow")) {
        want = 3;
        ovf = true;
    } else if (!strcmp(b, "add_overflow_p") || !strcmp(b, "sub_overflow_p") ||
               !strcmp(b, "mul_overflow_p")) {
        want = 3;
        ovfp = true;
    } else {
        return true;
    }
    if (n != want) {
        cerror(c, loc, n < want ? "too few arguments to function '%s'"
                                : "too many arguments to function '%s'", name);
        return false;
    }
    for (k = 0; k < n; k++)
        if (node_err(c, a[k]))
            return true;
    if (!strcmp(b, "alloca_with_align")) {
        bool ok = c->ck[a[1]] == K_ICE && !(c->ef[a[1]] & EF_OVERFLOW);
        uint64_t v = c->cv[a[1]];
        if (ok && (v < 8 || v > 2147483648u || (v & (v - 1))))
            ok = false;
        if (!ok) {
            cerror(c, arg_loc(c, a[1]), "second argument to function '%s' "
                   "must be a constant integer power of 2 between '8' and "
                   "'2147483648' bits", name);
            return false;
        }
    } else if (!strcmp(b, "assume_aligned")) {
        if (n == 3 && !is_int(c, rvt(c, a[2]))) {
            cerror(c, arg_loc(c, a[2]), "non-integer argument 3 in call to "
                   "function '%s'", name);
            return false;
        }
    } else if (!strcmp(b, "fpclassify")) {
        for (k = 0; k < 5; k++)
            if (c->ck[a[k]] != K_ICE) {
                cerror(c, arg_loc(c, a[k]), "non-const integer argument %u "
                       "in call to function '%s'", k + 1, name);
                return false;
            }
        if (!is_flt(c, rvt(c, a[5]))) {
            cerror(c, arg_loc(c, a[5]), "non-floating-point argument in call "
                   "to function '%s'", name);
            return false;
        }
    } else if (fp1) {
        if (!is_flt(c, rvt(c, a[0]))) {
            cerror(c, arg_loc(c, a[0]), "non-floating-point argument in call "
                   "to function '%s'", name);
            return false;
        }
    } else if (cmp) {
        TypeId t0 = rvt(c, a[0]), t1 = rvt(c, a[1]);
        if (!(is_flt(c, t0) || is_flt(c, t1)) ||
            !(is_flt(c, t0) || is_int(c, t0)) ||
            !(is_flt(c, t1) || is_int(c, t1))) {
            cerror(c, loc, "non-floating-point arguments in call to function "
                   "'%s'", name);
            return false;
        }
    } else if (ovf || ovfp) {
        for (k = 0; k < (ovf ? 2u : 3u); k++)
            if (!is_int(c, rvt(c, a[k]))) {
                cerror(c, arg_loc(c, a[k]), "argument %u in call to function "
                       "'%s' does not have integral type", k + 1, name);
                return false;
            }
        if (ovf) {
            TypeId t = rvt(c, a[2]), e;
            if (!is_ptr(c, t) || !is_int(c, e = pointee(c, t))) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' does not have pointer to integral type", name);
                return false;
            }
            if (tkind(c, e) == TY_ENUM) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to enumerated type", name);
                return false;
            }
            if (tkind(c, e) == TY_BOOL) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to boolean type", name);
                return false;
            }
            if (tquals(c, e) & TQ_CONST) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to 'const' type (%s)", name,
                       type_q(TT, t));
                return false;
            }
            if (tquals(c, e) & TQ_ATOMIC) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has pointer to '_Atomic' type (%s)", name,
                       type_q(TT, t));
                return false;
            }
        } else {
            TypeId t = rvt(c, a[2]);
            if (tkind(c, t) == TY_ENUM || tkind(c, t) == TY_BOOL) {
                cerror(c, arg_loc(c, a[2]), "argument 3 in call to function "
                       "'%s' has %s type", name,
                       tkind(c, t) == TY_ENUM ? "enumerated" : "boolean");
                return false;
            }
        }
    }
    return true;
}

/* A function designator or array operand takes the builtin's location when
 * converted to a pointer. */
static SrcLoc tg_loc(Checker *c, uint32_t n, SrcLoc loc)
{
    TypeId t = type_canon(TT, c->ty[n]);
    return is_func(c, t) || type_ckind(TT, t) == TY_ARRAY ? loc : cnode_loc(c, n);
}

/* check_tgmath_function: the parameter count of function-pointer argument
 * a (position pos), or 0 after an error. */
static uint32_t tgmath_function(Checker *c, uint32_t a, unsigned pos,
                                SrcLoc loc)
{
    TypeId t = rvt(c, a), f = 0;
    SrcLoc l = tg_loc(c, a, loc);
    const char *why;
    if (!is_ptr(c, t) || !is_func(c, pointee(c, t)))
        why = "is not a function pointer";
    else if ((type_ent(TT, f = type_canon(TT, pointee(c, t)))->flags &
              TF_NOPROTO))
        why = "is unprototyped";
    else if (type_ent(TT, f)->flags & TF_VARIADIC)
        why = "has variable arguments";
    else if (!type_ent(TT, f)->n)
        why = "has no arguments";
    else
        return (uint32_t)type_ent(TT, f)->n;
    cerror(c, l, "argument %u of '__builtin_tgmath' %s", pos, why);
    return 0;
}

/* common_type of two real floating types: the wider; of equal value sets,
 * _FloatN over _FloatNx over the standard type.  True if a wins over b. */
static int tg_rank(Checker *c, TypeId t)
{
    switch (tkind(c, t)) {
    case TY_FLOAT16: case TY_FLOAT32: case TY_FLOAT64: case TY_FLOAT128:
        return 2;
    case TY_FLOAT32X: case TY_FLOAT64X:
        return 1;
    default:
        return 0;
    }
}

static bool tg_better(Checker *c, TypeId a, TypeId b)
{
    int pa = float_prec(c, tkind(c, a)), pb = float_prec(c, tkind(c, b));
    return pa > pb || (pa == pb && tg_rank(c, a) > tg_rank(c, b));
}

static TypeId tg_mv(Checker *c, TypeId t)
{
    return unqual(c, type_canon(TT, t));
}

static bool tg_fl(Checker *c, TypeId t)
{
    return is_flt(c, t) ||
           (is_complex(c, t) && is_flt(c, type_base(TT, type_canon(TT, t))));
}

/* The checks of c_parser_postfix_expression on how the functions' return and
 * parameter types vary; false after an error. */
static bool tgmath_variation(Checker *c, const uint32_t *a, const TypeId *ft,
                             uint32_t nf, uint32_t nargs, SrcLoc loc,
                             int *kind, TypeId *tg)
{
    TypeId pf[17];
    bool pcx[17] = {false}, pvar[17] = {false};
    int maxv = 0, tgarg = 0;
    uint32_t j, m, u;
    pf[0] = tg_mv(c, type_base(TT, ft[0]));
    pcx[0] = is_complex(c, pf[0]);
    for (m = 0; m < nargs; m++) {
        pf[m + 1] = tg_mv(c, type_params(TT, ft[0])[m]);
        pcx[m + 1] = is_complex(c, pf[m + 1]);
    }
    for (j = 1; j < nf; j++) {
        TypeId ret = tg_mv(c, type_base(TT, ft[j]));
        if (ret != pf[0]) {
            pvar[0] = true;
            if (!tg_fl(c, pf[0])) {
                cerror(c, tg_loc(c, a[0], loc), "invalid type-generic return "
                       "type for argument 1 of '__builtin_tgmath'");
                return false;
            }
            if (!tg_fl(c, ret)) {
                cerror(c, tg_loc(c, a[j], loc), "invalid type-generic return "
                       "type for argument %u of '__builtin_tgmath'", j + 1);
                return false;
            }
        }
        if (is_complex(c, ret))
            pcx[0] = true;
        for (m = 0; m < nargs; m++) {
            TypeId t = tg_mv(c, type_params(TT, ft[j])[m]);
            if (t != pf[m + 1]) {
                pvar[m + 1] = true;
                if (!tg_fl(c, pf[m + 1])) {
                    cerror(c, tg_loc(c, a[0], loc), "invalid type-generic type "
                           "for argument %u of argument %u of "
                           "'__builtin_tgmath'", m + 1, 1);
                    return false;
                }
                if (!tg_fl(c, t)) {
                    cerror(c, tg_loc(c, a[j], loc), "invalid type-generic type "
                           "for argument %u of argument %u of "
                           "'__builtin_tgmath'", m + 1, j + 1);
                    return false;
                }
            }
            if (is_complex(c, t))
                pcx[m + 1] = true;
        }
    }
    for (j = 0; j <= nargs; j++) {          /* 0 fixed, 1 real, 2 complex */
        if (!pvar[j])
            kind[j] = 0;
        else if (pcx[j])
            maxv = kind[j] = 2;
        else {
            kind[j] = 1;
            if (maxv != 2)
                maxv = 1;
        }
    }
    if (!maxv) {
        cerror(c, loc, "function arguments of '__builtin_tgmath' all have "
               "the same type");
        return false;
    }
    for (j = 1; j <= nargs && !tgarg; j++)
        if (kind[j] == maxv)
            tgarg = (int)j;
    if (!tgarg) {
        cerror(c, loc, "function arguments of '__builtin_tgmath' lack "
               "type-generic parameter");
        return false;
    }
    for (j = 0; j < nf; j++) {
        tg[j] = tg_mv(c, type_params(TT, ft[j])[tgarg - 1]);
        for (u = 0; u < j; u++)
            if (tg[u] == tg[j]) {
                cerror(c, tg_loc(c, a[j], loc), "duplicate type-generic "
                       "parameter type for function argument %u of "
                       "'__builtin_tgmath'", j + 1);
                return false;
            }
    }
    for (j = 0; j < nf; j++) {
        TypeId et = tg[j], er = is_complex(c, et)
                    ? tg_mv(c, type_base(TT, type_canon(TT, et))) : et;
        TypeId ret = tg_mv(c, type_base(TT, ft[j]));
        if ((kind[0] == 2 && ret != et) || (kind[0] == 1 && ret != er)) {
            cerror(c, tg_loc(c, a[j], loc), "bad return type for function "
                   "argument %u of '__builtin_tgmath'", j + 1);
            return false;
        }
        for (m = 0; m < nargs; m++) {
            TypeId t = tg_mv(c, type_params(TT, ft[j])[m]);
            if ((kind[m + 1] == 2 && t != et) ||
                (kind[m + 1] == 1 && t != er)) {
                cerror(c, tg_loc(c, a[j], loc), "bad type for argument %u of "
                       "function argument %u of '__builtin_tgmath'", m + 1,
                       j + 1);
                return false;
            }
        }
    }
    for (m = 0; m < nargs; m++) {
        uint32_t ar = a[nf + m];
        TypeId t;
        if (!kind[m + 1])
            continue;
        t = rvt(c, ar);
        if (!is_int(c, t) && !is_flt(c, t) && !is_complex(c, t)) {
            cerror(c, cnode_loc(c, ar), "invalid type of argument %u of "
                   "type-generic function", m + 1);
            return false;
        }
    }
    return true;
}

/* __builtin_tgmath (functions..., arguments...): the call of the function
 * whose generic parameter type fits the arguments.  Sets i's type to its
 * return type; false (no diagnostic) if the call is malformed. */
static bool e_tgmath(Checker *c, uint32_t i)
{
    uint32_t all[16], n = nkids(c, i, all, 16), na, nf, nargs, j, m, sel;
    uint32_t *a = all + 1;
    TypeId rt, ft[16], tg[16], areal = 0;
    int kind[17];
    bool arg_cx, floatnx = false;
    SrcLoc loc;
    if (n < 1 || n > 16)
        return false;
    loc = cnode_loc(c, all[0]);
    na = n - 1;
    for (j = 0; j < na; j++)
        if (node_err(c, a[j]))
            return false;
    if (na < 3) {
        cerror(c, loc, "too few arguments to '__builtin_tgmath'");
        return false;
    }
    nargs = tgmath_function(c, a[0], 1, loc);
    if (!nargs)
        return false;
    if (na < nargs || na - nargs < 2) {
        cerror(c, loc, "too few arguments to '__builtin_tgmath'");
        return false;
    }
    nf = na - nargs;
    sel = nf;
    ft[0] = type_canon(TT, pointee(c, rvt(c, a[0])));
    for (j = 1; j < nf; j++) {
        uint32_t tn = tgmath_function(c, a[j], j + 1, loc);
        if (!tn)
            return false;
        if (tn != nargs) {
            cerror(c, tg_loc(c, a[j], loc), "argument %u of '__builtin_tgmath' "
                   "has wrong number of arguments", j + 1);
            return false;
        }
        ft[j] = type_canon(TT, pointee(c, rvt(c, a[j])));
    }
    if (!tgmath_variation(c, a, ft, nf, nargs, loc, kind, tg))
        return false;
    arg_cx = true;
    for (j = 0; j < nf; j++)
        if (!is_complex(c, tg[j]))
            arg_cx = false;
    for (m = 0; m < nargs; m++) {       /* integers become _Float32x if any
                                         * generic argument is _FloatNx */
        TypeId t;
        if (!kind[m + 1])
            continue;
        t = tg_mv(c, rvt(c, a[nf + m]));
        if (is_complex(c, t))
            t = tg_mv(c, type_base(TT, type_canon(TT, t)));
        if (tkind(c, t) == TY_FLOAT32X || tkind(c, t) == TY_FLOAT64X)
            floatnx = true;
    }
    for (m = 0; m < nargs; m++) {
        TypeId t;
        if (!kind[m + 1])
            continue;
        t = tg_mv(c, rvt(c, a[nf + m]));
        if (is_complex(c, t)) {
            arg_cx = true;
            t = tg_mv(c, type_base(TT, type_canon(TT, t)));
        }
        if (is_int(c, t))
            t = floatnx ? TYPE_B(FLOAT32X) : TYPE_B(DOUBLE);
        if (!areal || tg_better(c, t, areal))
            areal = t;
    }
    for (j = 0; j < nf && sel == nf; j++) {
        TypeId t = is_complex(c, tg[j]) ? tg_mv(c, type_base(TT, type_canon(TT, tg[j])))
                                        : tg[j];
        if (is_complex(c, tg[j]) == arg_cx && t == areal)
            sel = j;
    }
    if (sel == nf && !kind[0] && is_flt(c, tg_mv(c, type_base(TT, ft[0]))))
        for (j = 0; j < nf && sel == nf; j++) {
            TypeId t = is_complex(c, tg[j])
                       ? tg_mv(c, type_base(TT, type_canon(TT, tg[j]))) : tg[j];
            if (is_complex(c, tg[j]) == arg_cx && areal && is_flt(c, t) &&
                float_prec(c, tkind(c, areal)) <= float_prec(c, tkind(c, t)))
                sel = j;
        }
    if (sel == nf) {
        cerror(c, loc, "no matching function for type-generic call");
        return false;
    }
    rt = unqual(c, type_base(TT, ft[sel]));
    c->ty[i] = rt;
    c->ef[i] = EF_SIDE;
    return true;
}

/* The type of a call of an overloaded atomic or sync built-in; ERRT for the
 * others. */
static TypeId atomic_result(Checker *c, uint32_t i, const char *name)
{
    int kind = atomic_kind(name);
    const char *b = !strncmp(name, "__sync_", 7) ? name + 7 : name + 9;
    uint32_t all[16], n = nkids(c, i, all, 16);
    TypeId t;
    if (!kind || n < 2 || n > 16 || node_err(c, all[1]))
        return ERRT;
    if (kind == 1)
        return !strcmp(b, "compare_exchange") ? TYPE_B(BOOL) : TYPE_B(VOID);
    t = rvt(c, all[1]);
    if (!is_ptr(c, t))
        return ERRT;
    if (!strcmp(b, "bool_compare_and_swap") ||
        !strcmp(b, "compare_exchange_n"))
        return TYPE_B(BOOL);
    if (!strcmp(b, "lock_release") || !strcmp(b, "store_n"))
        return TYPE_B(VOID);
    return unqual(c, pointee(c, t));
}

/* -Wabsolute-value: warn_for_abs of c-parser.cc, for a call of a library
 * absolute value function with one argument of an unsuitable type. */
static void warn_for_abs(Checker *c, uint32_t i, const uint32_t *k, uint32_t n)
{
    uint32_t fn = strip_paren(c, k[0]), a = n == 2 ? k[1] : NO_NODE;
    const CSym *sy = NULL;
    const BTab *bt = NULL;
    const char *nm, *base;
    TypeId at, ft = 0, bft;
    int fam;
    bool integ, flt, cpx;
    if (a == NO_NODE || fn == NO_NODE || ntag(c, fn) != N_IDENT ||
        !diag_enabled(c->diag, "absolute-value") || node_err(c, a) ||
        inhibited(c, i, false))
        return;
    if (c->ck[fn] == K_ADDR && c->cb[fn] && !(c->cb[fn] & CB_NODE)) {
        sy = csym(c, c->cb[fn] - 1);
        if (sy->kind != CS_FUNC)
            return;
        nm = cident(c, sy->name);
    } else
        nm = cident(c, cnode_ident(c, fn));
    if (strncmp(nm, "__builtin_", 10) && !sy)
        return;
    base = !strncmp(nm, "__builtin_", 10) ? nm + 10 : nm;
    if (!strcmp(base, "abs") || !strcmp(base, "labs") ||
        !strcmp(base, "llabs") || !strcmp(base, "imaxabs"))
        fam = 0;
    else if (!strcmp(base, "fabs") || !strcmp(base, "fabsf") ||
             !strcmp(base, "fabsl"))
        fam = 1;
    else if (!strcmp(base, "cabs") || !strcmp(base, "cabsf") ||
             !strcmp(base, "cabsl")) {
        fam = 2;   /* not in the built-in table: the parameter is a complex */
        ft = !strcmp(base, "cabs") ? TYPE_B(DOUBLE)
           : !strcmp(base, "cabsf") ? TYPE_B(FLOAT) : TYPE_B(LDOUBLE);
    } else
        return;
    if (fam != 2) {
        bt = sy ? bt_for_decl(c, sy)
                : bt_find(c, nm + 10, true);
        if (!bt || (sy && !builtin_decl_ok(c, sy)))
            return;
    }
    at = unqual(c, rvt(c, a));
    if (is_err(c, at))
        return;
    integ = is_int(c, at);
    flt = is_flt(c, at);
    cpx = is_complex(c, at);
    if (!integ && !flt && !cpx)
        return;
    if (fam == 0 && !integ) {
        cwarn(c, call_loc(c, k[0]), "absolute-value", "using integer absolute "
              "value function '%s' when argument is of %s type %s", nm,
              flt ? "floating-point" : "complex", type_q(TT, at));
        return;
    }
    if (fam == 1 && !flt) {
        cwarn(c, call_loc(c, k[0]), "absolute-value", "using floating-point "
              "absolute value function '%s' when argument is of %s type %s",
              nm, integ ? "integer" : "complex", type_q(TT, at));
        return;
    }
    if (fam == 2 && !cpx) {
        cwarn(c, call_loc(c, k[0]), "absolute-value", "using complex absolute "
              "value function '%s' when argument is of %s type %s", nm,
              integ ? "integer" : "floating-point", type_q(TT, at));
        return;
    }
    if (fam == 0 && integ && !type_is_signed(TT, at) && tkind(c, at) != TY_BOOL)
        cwarn(c, call_loc(c, k[0]), "absolute-value", "taking the absolute "
              "value of unsigned type %s has no effect", type_q(TT, at));
    if (fam != 2) {
        bft = type_canon(TT, bt_func_type(c, bt));
        if (!type_ent(TT, bft)->n)
            return;
        ft = type_params(TT, bft)[0];
    }
    if (cpx) {
        at = type_canon(TT, type_base(TT, type_canon(TT, at)));
        if (fam != 2)
            ft = type_canon(TT, type_base(TT, type_canon(TT, ft)));
    }
    {
        bool ok1, ok2;
        uint64_t sa = type_size(TT, at, &ok1), sf = type_size(TT, ft, &ok2);
        if (ok1 && ok2 && sf < sa)
            cwarn(c, call_loc(c, k[0]), "absolute-value", "absolute value "
                  "function '%s' given an argument of type %s but has "
                  "parameter of type %s which may cause truncation of value",
                  nm, type_q(TT, at), type_q(TT, ft));
    }
}

static void e_call(Checker *c, uint32_t i)
{
    uint32_t k[3], n = nkids(c, i, k, 3), f;
    TypeId t;
    if (n == 0) {
        set_err(c, i);
        return;
    }
    f = strip_paren(c, k[0]);
    if (f != NO_NODE && ntag(c, f) == N_IDENT && f == k[0] &&
        lookup_ord(c, cnode_ident(c, f)) == SYM_NONE) {
        const char *name = cident(c, cnode_ident(c, f));
        if (!builtin_args_ok(c, i, k[0], name)) {
            set_err(c, i);
            return;
        }
        if (!strncmp(name, "__builtin_", 10) && builtin_nonnull(name)) {
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an <= 32)
                check_nonnull(c, av, an, builtin_nonnull(name), NULL, 0, false,
                              true, call_loc(c, k[0]));
        }
        if (!strncmp(name, "__builtin_", 10) &&
            (builtin_format_pos(name) || builtin_scanf_pos(name))) {
            uint32_t av[32], an = nkids(c, i, av, 32);
            if (an <= 32)
                check_format_literal(c, av, an, NULL, name, call_loc(c, k[0]));
        }
        {
            TypeId rt = atomic_result(c, i, name);
            if (!is_err(c, rt)) {
                c->ty[i] = rt;
                c->ef[i] = EF_SIDE;
                return;
            }
        }
        if (!strcmp(name, "__builtin_choose_expr") ||
            !strcmp(name, "__builtin_call_with_static_chain")) {
            uint32_t av[4], an = nkids(c, i, av, 4), j;
            bool choose = name[10] == 'c' && name[11] == 'h';
            SrcLoc bl = call_loc(c, k[0]);
            if (an > 4 || an - 1 != (choose ? 3u : 2u)) {
                cerror(c, bl, "wrong number of arguments to '%s'", name);
                set_err(c, i);
                return;
            }
            for (j = 1; j < an; j++)
                if (node_err(c, av[j])) {
                    set_err(c, i);
                    return;
                }
            if (choose) {
                if (c->ck[av[1]] != K_ICE || !is_int(c, rvt(c, av[1])))
                    cerror(c, bl, "first argument to '__builtin_choose_expr' "
                           "not a constant");
                copy_node(c, i, c->cv[av[1]] && c->ck[av[1]] == K_ICE
                                    ? av[2] : av[3]);
                return;
            }
            {
                uint32_t a0 = strip_paren(c, av[1]);
                if (a0 == NO_NODE || ntag(c, a0) != N_CALL)
                    cerror(c, bl, "first argument to "
                           "'__builtin_call_with_static_chain' must be a call "
                           "expression");
                else if (!is_ptr(c, rvt(c, av[2])))
                    cerror(c, bl, "second argument to "
                           "'__builtin_call_with_static_chain' must be a "
                           "pointer type");
            }
            copy_node(c, i, av[1]);
            return;
        }
        if (!strcmp(name, "__builtin_tgmath")) {
            if (!e_tgmath(c, i))
                set_err(c, i);
            return;
        }
        if (!strcmp(name, "__builtin_constant_p") && n >= 2) {
            c->ty[i] = TYPE_B(INT);
            if (c->ck[k[1]] == K_ICE && !(c->ef[k[1]] & EF_OVERFLOW)) {
                c->ck[i] = K_ICE;
                c->cv[i] = 1;
                c->ef[i] = EF_INTOPS;
            } else if (is_const(c, k[1])) {
                c->ck[i] = K_FOLD;
                c->cv[i] = 1;
                c->ef[i] = EF_CST;
            } else if (!in_function(c)) {
                c->ck[i] = K_FOLD;
                c->ef[i] = EF_CST;
            }
            return;
        }
        if (!strcmp(name, "__builtin_expect") && n >= 2) {
            c->ty[i] = TYPE_B(LONG);
            if (has_ival(c, k[1])) {
                c->ck[i] = K_FOLD;
                c->cv[i] = cexpr_trunc(c, c->ty[i], c->cv[k[1]]);
                c->ef[i] = EF_CST;
            }
            return;
        }
    }
    if (node_err(c, k[0])) {
        set_err(c, i);
        return;
    }
    t = rvt(c, k[0]);
    if (!is_ptr(c, t) || !is_func(c, pointee(c, t))) {
        uint32_t ref = f != NO_NODE && ntag(c, f) == N_IDENT
                           ? lookup_ord(c, cnode_ident(c, f)) : SYM_NONE;
        Diagnostic *d = ref != SYM_NONE
            ? cerror_d(c, call_loc(c, k[0]), "called object '%s' is not a "
                       "function or function pointer", estr(c, k[0]))
            : cerror_d(c, call_loc(c, k[0]), "called object is not a "
                       "function or function pointer");
        if (ref != SYM_NONE)
            cnote(c, d, csym(c, ref)->loc, "declared here");
        set_err(c, i);
        return;
    }
    warn_for_abs(c, i, k, n);
    if (!call_args(c, i, k[0], t)) {
        set_err(c, i);
        return;
    }
    {
        TypeId ret = type_base(TT, pointee(c, t));
        SrcLoc cl = call_loc(c, k[0]);
        uint32_t cf = strip_paren(c, k[0]);
        bool qv = is_void(c, ret) && tquals(c, ret) != 0;
        /* a function designator cast to an incompatible function type */
        if (cf != NO_NODE && ntag(c, cf) == N_CAST) {
            uint32_t ck2[2], op;
            if (nkids(c, cf, ck2, 2) == 2 &&
                (op = strip_paren(c, ck2[1])) != NO_NODE &&
                ntag(c, op) == N_IDENT && is_func(c, c->ty[op]) &&
                !type_compatible(TT, mvt(c, c->ty[op]),
                                 mvt(c, pointee(c, t)))) {
                cwarn(c, cl, "", "function called through a non-compatible "
                      "type");
                if (qv)
                    cpedwarn(c, cl, "", "function with qualified void return "
                             "type called");
            }
        }
        if (is_void(c, ret)) {
            if (qv)
                cpedwarn(c, cl, "", "function with qualified void return type "
                         "called");
        } else if (!complete(c, ret)) {
            incomplete_error(c, cl, NO_NODE, ret);
            set_err(c, i);
            return;
        }
    }
    c->ty[i] = unqual(c, type_base(TT, pointee(c, t)));
    c->ef[i] = EF_SIDE;
}

static void e_index(Checker *c, uint32_t i)
{
    uint32_t k[2], a, x;
    SrcLoc loc = cnode_loc(c, i);
    TypeId pt, et;
    bool swapped = false;
    if (nkids(c, i, k, 2) < 2 || node_err(c, k[0]) || node_err(c, k[1])) {
        set_err(c, i);
        return;
    }
    a = k[0];
    x = k[1];
    if (tkind(c, c->ty[a]) == TY_VECTOR) {
        if (!is_int(c, rvt(c, x))) {
            cerror(c, loc, "array subscript is not an integer");
            set_err(c, i);
            return;
        }
        c->ty[i] = type_base(TT, type_canon(TT, c->ty[a])) |
                   TYPE_QUALS(c->ty[a]);
        c->ef[i] = (c->ef[a] & EF_LVALUE) | ((c->ef[a] | c->ef[x]) & EF_SIDE);
        return;
    }
    if (!is_array(c, c->ty[a]) && !is_ptr(c, c->ty[a])) {
        if (!is_array(c, c->ty[x]) && !is_ptr(c, c->ty[x])) {
            cerror(c, loc, "subscripted value is neither array nor pointer "
                   "nor vector");
            set_err(c, i);
            return;
        }
        a = k[1];
        x = k[0];
        swapped = true;
    }
    /* require_complete_type at gcc's input_location: the line's first token */
    if (!rvalue_ok_at(c, x, cinput_loc(c, c->nodes[i].tok))) {
        set_err(c, i);
        return;
    }
    if (!is_int(c, rvt(c, x))) {
        cerror(c, loc, "array subscript is not an integer");
        set_err(c, i);
        return;
    }
    pt = rvt(c, a);
    if (is_func(c, pointee(c, pt))) {
        cerror(c, loc, "subscripted value is pointer to function");
        set_err(c, i);
        return;
    }
    if (!swapped && mainv(c, rvt(c, x)) == TYPE_B(CHAR) && !is_intcst(c, x))
        cwarn(c, expr_loc(c, x), "char-subscripts",
              "array subscript has type 'char'");
    if (is_array(c, c->ty[a])) {
        uint32_t v = strip_paren(c, a);
        while (v != NO_NODE && ntag(c, v) == N_MEMBER_EXPR &&
               !(c->nodes[v].flags & NF_ARROW))
            v = strip_paren(c, first_child(c, v));
        if (v != NO_NODE && ntag(c, v) == N_IDENT &&
            (c->ef[v] & EF_REGISTER))
            ped(c, i, loc, "ISO C forbids subscripting 'register' array");
        et = elem_of(c, c->ty[a]);
    } else {
        if (!ptr_arith_ok(c, i, loc, pt)) {
            set_err(c, i);
            return;
        }
        et = pointee(c, pt);
        if (is_void(c, et) && !inhibited(c, i, false))
            cwarn(c, loc, "", "dereferencing 'void *' pointer");
    }
    c->ty[i] = et;
    c->ef[i] = EF_LVALUE | ((c->ef[a] | c->ef[x]) & EF_SIDE);
    if (tquals(c, et) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (c->ck[a] == K_ADDR && has_ival(c, x)) {
        int64_t idx = ival_neg(c, rvt(c, x), c->cv[x]) ? (int64_t)c->cv[x]
                                                       : (int64_t)c->cv[x];
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = c->cb[a];
        c->cv[i] = c->cv[a] + (uint64_t)idx * elem_size(c, pt);
        addr_rvalue(c, i);
    }
}

/* The record a member access applies to: *rec, false after an error. */
static bool member_datum(Checker *c, uint32_t i, uint32_t d, TypeId *rec)
{
    SrcLoc loc = ctok_loc(c, c->nodes[i].tok - 1);
    if (c->nodes[i].flags & NF_ARROW) {
        TypeId t = rvt(c, d);
        if (!is_ptr(c, t)) {
            cerror(c, loc, "invalid type argument of '->' (have %s)",
                   type_q(TT, t));
            return false;
        }
        *rec = pointee(c, t);
        if (is_void(c, *rec) && !inhibited(c, i, false))
            cwarn(c, loc, "", "dereferencing 'void *' pointer");
        return true;
    }
    *rec = c->ty[d];
    return true;
}

static void e_member(Checker *c, uint32_t i)
{
    uint32_t d = first_child(c, i), name = cnode_ident(c, i);
    SrcLoc loc;
    TypeId rec, ft;
    const Field *f;
    uint64_t off = 0;
    unsigned q = 0;
    bool arrow = (c->nodes[i].flags & NF_ARROW) != 0;
    if (d == NO_NODE || node_err(c, d) || (c->nodes[i].flags & NF_ERROR) ||
        !name) {
        set_err(c, i);
        return;
    }
    loc = ctok_loc(c, c->nodes[i].tok - 1);
    if (!member_datum(c, i, d, &rec)) {
        set_err(c, i);
        return;
    }
    if (!is_record(c, rec)) {
        TypeId r = rec;
        if (is_ptr(c, r) && is_record(c, pointee(c, r))) {
            if (arrow)
                cerror(c, loc, "'%s' is a pointer to pointer; did you mean to "
                       "dereference it before applying '->' to it?",
                       estr(c, d));
            else
                cerror(c, loc, "'%s' is a pointer; did you mean to use '->'?",
                       estr(c, d));
        } else if (!is_err(c, r)) {
            cerror(c, loc, "request for member '%s' in something not a "
                   "structure or union", cident(c, name));
        }
        set_err(c, i);
        return;
    }
    if (!complete(c, rec)) {
        incomplete_error(c, loc, NO_NODE, rec);
        set_err(c, i);
        return;
    }
    f = find_field(c, rec, name, &off, &q);
    if (!f) {
        Best b;
        const char *sug;
        best_init(&b, cident(c, name), &c->fuzzy_work);
        fuzzy_fields(c, &b, rec);
        sug = best_get(&b);
        if (sug)
            cerror(c, cnode_loc(c, i), "%s has no member named '%s'; did you "
                   "mean '%s'?", type_q(TT, rec), cident(c, name), sug);
        else
            cerror(c, loc, "%s has no member named '%s'", type_q(TT, rec),
                   cident(c, name));
        set_err(c, i);
        return;
    }
    cdep_report(c, cinput_loc(c, c->nodes[i].tok), f->name, f->dep, f->dmsg,
                &f->loc);
    ft = type_qual(f->ty, q | tquals(c, rec));
    c->ty[i] = ft;
    c->ef[i] = c->ef[d] & EF_SIDE;
    if (tquals(c, ft) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (arrow || (c->ef[d] & EF_LVALUE))
        c->ef[i] |= EF_LVALUE;
    if (f->flags & FF_BITFIELD) {
        c->ef[i] |= EF_BITFIELD;
        if (f->width < type_int_bits(TT, TYPE_B(INT)))
            c->ef[i] |= EF_BFPROMOTE;
        return;
    }
    if (arrow ? c->ck[d] == K_ADDR : (c->ef[d] & EF_ADDRLV) != 0) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = c->cb[d];
        c->cv[i] = c->cv[d] + off / 8;
        addr_rvalue(c, i);
    }
}

/* ---- unary operators ------------------------------------------------------------ */

/* convert_lvalue_to_rvalue's check: an operand used for its value must not
 * have an incomplete (non-void) type.  Reported at its first token. */
static bool rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc)
{
    TypeId t = rvt(c, i);
    if (is_void(c, t) || complete(c, t))
        return true;
    incomplete_error(c, loc, i, t);
    return false;
}

static bool rvalue_ok(Checker *c, uint32_t i)
{
    return rvalue_ok_at(c, i, first_loc(c, i));
}

bool cexpr_rvalue_ok(Checker *c, uint32_t i)
{
    return rvalue_ok(c, i);
}

bool cexpr_rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc)
{
    return rvalue_ok_at(c, i, loc);
}

/* Value flags of a unary result from its operand a: see the file
 * comment. */
static void unary_value(Checker *c, uint32_t i, uint32_t a, uint64_t v,
                        bool ovf)
{
    TypeId t = c->ty[i];
    c->cv[i] = cexpr_trunc(c, t, v);
    if (c->ck[a] == K_ICE) {
        c->ck[i] = K_ICE;
        c->ef[i] |= EF_INTOPS;
        if (ovf || (c->ef[a] & EF_OVERFLOW))
            c->ef[i] |= EF_OVERFLOW;
        if (ovf && !(c->ef[a] & EF_OVERFLOW) && !inhibited(c, i, false))
            cwarn(c, cnode_loc(c, i), "overflow", "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, a),
                  type_q(TT, t), vstr(c, t, c->cv[i]));
        return;
    }
    c->ck[i] = K_FOLD;
    if (c->ef[a] & (EF_CST | EF_NOPCST | EF_REALCST))
        c->ef[i] |= EF_NOPCST;
    else if (c->ef[a] & EF_INTOPS)
        c->ef[i] |= EF_INTOPS;
    if (ovf && !(c->ef[a] & EF_OVERFLOW)) {
        c->ef[i] |= EF_OVERFLOW | EF_FOLDWARN;
        c->fold_pending++;
    }
}

/* gcc's readonly_error for lvalue a: the diagnostic (use 0: assignment, 1:
 * increment, 2: decrement) when a is read-only; true if one was given. */
static bool readonly_check(Checker *c, uint32_t a, SrcLoc loc, int use)
{
    static const char *const verb[] = { "assignment", "increment", "decrement" };
    TypeId t = c->ty[a];
    uint32_t s = strip_paren(c, a);
    bool ro = (tquals(c, t) & TQ_CONST) != 0;
    if (!ro && is_record(c, t)) {
        const Record *r = type_record(TT, t);
        ro = r && (r->flags & RF_CONST_MEMBER);
    }
    if (!ro)
        return false;
    if (s != NO_NODE && ntag(c, s) == N_MEMBER_EXPR) {
        uint32_t base = first_child(c, s), mid = cnode_ident(c, s);
        TypeId bt = base == NO_NODE ? ERRT : c->ty[base];
        bool arrow = (c->nodes[s].flags & NF_ARROW) != 0;
        if (base != NO_NODE && arrow && is_ptr(c, rvt(c, base)))
            bt = pointee(c, rvt(c, base));
        if (mid) {
            const char *nm = cident(c, mid);
            if (tquals(c, bt) & TQ_CONST)
                cerror(c, loc, "%s of member '%s' in read-only object",
                       verb[use], nm);
            else
                cerror(c, loc, "%s of read-only member '%s'", verb[use], nm);
            return true;
        }
    }
    if (s != NO_NODE && ntag(c, s) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        if (ref != SYM_NONE && csym(c, ref)->kind != CS_FUNC) {
            cerror(c, loc, "%s of read-only %s '%s'", verb[use],
                   csym(c, ref)->flags & CSF_PARAM ? "parameter" : "variable",
                   cident(c, cnode_ident(c, s)));
            return true;
        }
    }
    cerror(c, loc, "%s of read-only location '%s'", verb[use], estr(c, a));
    return true;
}

static void incdec(Checker *c, uint32_t i, uint32_t a, bool inc)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId t;
    if (node_err(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (!is_void(c, t) && !complete(c, t)) {
        incomplete_error(c, loc, a, t);
        set_err(c, i);
        return;
    }
    if (is_void(c, t)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (!(c->ef[a] & EF_LVALUE) || is_array(c, c->ty[a]) ||
        is_func(c, c->ty[a])) {
        cerror(c, loc, inc ? "lvalue required as increment operand"
                           : "lvalue required as decrement operand");
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ENUM && cexpr_cxx_compat(c, i))
        cwarn(c, loc, "c++-compat", inc ? "increment of enumeration value is "
              "invalid in C++" : "decrement of enumeration value is invalid in "
              "C++");
    if (tkind(c, t) == TY_BOOL)
        cwarn(c, loc, "bool-operation", inc ? "increment of a boolean "
              "expression" : "decrement of a boolean expression");
    if (is_complex(c, t))
        ped(c, i, loc, "ISO C does not support '++' and '--' on complex types");
    else if (!is_ptr(c, t) && !is_int(c, t) && !is_flt(c, t) &&
             tkind(c, t) != TY_VECTOR) {
        cerror(c, loc, inc ? "wrong type argument to increment"
                           : "wrong type argument to decrement");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, t)) {
        TypeId b = pointee(c, t);
        if (!is_void(c, b) && !is_func(c, b) && !complete(c, b))
            cerror(c, loc, inc ? "increment of pointer to an incomplete type "
                   "%s" : "decrement of pointer to an incomplete type %s",
                   type_q(TT, b));
        else if ((is_func(c, b) || is_void(c, b)) &&
                 !cexpr_in_extension(c, i))
            cpedwarn(c, loc, "pointer-arith", inc ? "wrong type argument to "
                     "increment" : "wrong type argument to decrement");
    }
    if (readonly_check(c, a, loc, inc ? 1 : 2)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = EF_SIDE;
}

static void addr_of(Checker *c, uint32_t i, uint32_t a)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t s = strip_paren(c, a);
    TypeId t = c->ty[a];
    if (node_err(c, a)) {
        set_err(c, i);
        return;
    }
    if (is_void(c, t) && !tquals(c, t) &&
        !(ntag(c, s) == N_UNARY && npunct(c, s) == P_STAR))
        cpedwarn(c, loc, "", "taking address of expression of type 'void'");
    if (ntag(c, s) == N_UNARY && npunct(c, s) == P_STAR) {
        uint32_t p = first_child(c, s);
        c->ty[i] = rvt(c, p);
        c->ck[i] = c->ck[p];
        c->cv[i] = c->cv[p];
        c->cb[i] = c->cb[p];
        c->ef[i] = c->ef[p] & (EF_SIDE | EF_INTOPS | EF_NOPCST | EF_CST);
        return;
    }
    if (!is_func(c, t) && !(c->ef[a] & EF_LVALUE)) {
        cerror(c, loc, "lvalue required as unary '&' operand");
        set_err(c, i);
        return;
    }
    if (c->ef[a] & EF_BITFIELD) {
        uint32_t m = s;
        while (m != NO_NODE && ntag(c, m) == N_UNARY &&
               tckw(c, c->nodes[m].tok) == CK_EXTENSION)
            m = strip_paren(c, first_child(c, m));
        cerror(c, loc, "cannot take address of bit-field '%s'",
               m != NO_NODE && ntag(c, m) == N_MEMBER_EXPR
                   ? cident(c, cnode_ident(c, m)) : "");
        set_err(c, i);
        return;
    }
    if (c->ef[a] & EF_REGISTER) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, s));
        bool global = ref != SYM_NONE && !(ref & SYM_LOCAL);
        cerror(c, cinput_loc(c, last_tok(c, i) + 1),
               global ? "address of global register variable '%s' requested"
                      : "address of register variable '%s' requested",
               cident(c, cnode_ident(c, s)));
        set_err(c, i);
        return;
    }
    c->ty[i] = type_ptr(TT, t);
    c->ef[i] = c->ef[a] & EF_SIDE;
    if (c->ef[a] & EF_ADDRLV) {
        c->ck[i] = K_ADDR;
        c->cb[i] = c->cb[a];
        c->cv[i] = c->cv[a];
    }
}

static void deref(Checker *c, uint32_t i, uint32_t a)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t s = strip_paren(c, a);
    TypeId t, b;
    if (node_err(c, a) || !rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (!is_ptr(c, t)) {
        cerror(c, loc, "invalid type argument of unary '*' (have %s)",
               type_q(TT, t));
        set_err(c, i);
        return;
    }
    b = pointee(c, t);
    if (ntag(c, s) == N_UNARY && npunct(c, s) == P_AMP) {
        uint32_t x = first_child(c, s);
        if (x != NO_NODE && c->ty[x] == b) {
            copy_node(c, i, x);
            return;
        }
    }
    if (is_void(c, b) && !inhibited(c, i, false))
        cwarn(c, loc, "", "dereferencing 'void *' pointer");
    c->ty[i] = b;
    c->ef[i] = EF_LVALUE | (c->ef[a] & EF_SIDE);
    if (tquals(c, b) & TQ_VOLATILE)
        c->ef[i] |= EF_SIDE;
    if (c->ck[a] == K_ADDR) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = c->cb[a];
        c->cv[i] = c->cv[a];
        addr_rvalue(c, i);
    }
}

static void arith_unary(Checker *c, uint32_t i, uint32_t a, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId t;
    if (node_err(c, a) || !rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (is_void(c, t)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_VECTOR && op == P_TILDE &&
        is_flt(c, vec_elem(c, t))) {
        cerror(c, loc, "wrong type argument to bit-complement");
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_VECTOR && op != P_BANG) {
        /* vector operands: the result has the vector's type */
        c->ty[i] = TYPE_UNQUAL(type_canon(TT, t));
        c->ef[i] = c->ef[a] & EF_SIDE;
        return;
    }
    switch (op) {
    case P_PLUS: case P_MINUS:
        if (!is_arith(c, t)) {
            cerror(c, loc, op == P_PLUS ? "wrong type argument to unary plus"
                                        : "wrong type argument to unary minus");
            set_err(c, i);
            return;
        }
        break;
    case P_TILDE:
        if (is_complex(c, t)) {
            ped(c, i, loc, "ISO C does not support '~' for complex conjugation");
        } else if (!is_int(c, t)) {
            cerror(c, loc, "wrong type argument to bit-complement");
            set_err(c, i);
            return;
        } else if (tkind(c, t) == TY_BOOL) {
            /* -Wbool-operation (-Wall) */
            Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "bool-operation",
                                    "'~' on a boolean expression");
            cnote(c, d, loc, "did you mean to use logical not?");
        }
        break;
    default: /* '!' */
        if (!is_scalar(c, t)) {
            cerror(c, loc, "wrong type argument to unary exclamation mark");
            set_err(c, i);
            return;
        }
        cexpr_truth_warn(c, a, loc);
        c->ty[i] = TYPE_B(INT);
        c->ef[i] = c->ef[a] & EF_SIDE;
        {
            int tv = truth(c, a, true);
            if (tv < 0) {
                if (c->ef[a] & EF_INTOPS)
                    c->ef[i] |= EF_INTOPS;
                return;
            }
            if (c->ck[a] == K_ICE) {
                c->ck[i] = K_ICE;
                c->cv[i] = !tv;
                c->ef[i] |= EF_INTOPS;
                if (c->ef[a] & EF_OVERFLOW)
                    c->ef[i] |= EF_OVERFLOW;
                return;
            }
            c->ck[i] = K_FOLD;
            c->cv[i] = !tv;
            if (truth(c, a, false) >= 0)
                c->ef[i] |= EF_NOPCST;
            else if (c->ef[a] & EF_INTOPS)
                c->ef[i] |= EF_INTOPS;
        }
        return;
    }
    c->ty[i] = promoted(c, a);
    c->ef[i] = c->ef[a] & EF_SIDE;
    if (c->ck[a] == K_FLOAT) {
        long double f = c->fv.data[c->cv[a]];
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, op == P_MINUS ? -f : f);
        return;
    }
    if (has_ival(c, a)) {
        uint64_t v = c->cv[a];
        bool ovf = false;
        TypeId pt = c->ty[i];
        if (op == P_MINUS) {
            unsigned bits = int_bits(c, pt);
            if (is_signed(c, pt) && bits <= 64 && bits > 0 &&
                v == cexpr_trunc(c, pt, UINT64_C(1) << (bits - 1)))
                ovf = true;
            v = 0 - v;
        } else if (op == P_TILDE) {
            v = ~v;
        }
        if (op == P_PLUS) {
            /* no operation: the operand's value (not an lvalue) */
            c->ck[i] = c->ck[a];
            c->cv[i] = cexpr_trunc(c, pt, v);
            c->ef[i] |= c->ef[a] & (EF_INTOPS | EF_CST | EF_NOPCST |
                                    EF_OVERFLOW);
            return;
        }
        unary_value(c, i, a, v, ovf);
        return;
    }
    if (c->ef[a] & EF_INTOPS)
        c->ef[i] |= EF_INTOPS;
}

static void real_imag(Checker *c, uint32_t i, uint32_t a, bool real)
{
    TypeId t;
    if (node_err(c, a)) {
        set_err(c, i);
        return;
    }
    t = rvt(c, a);
    if (is_complex(c, t)) {
        c->ty[i] = type_qual(type_base(TT, type_canon(TT, t)),
                             tquals(c, c->ty[a]));
        c->ef[i] = c->ef[a] & (EF_LVALUE | EF_SIDE);
        return;
    }
    if (!is_int(c, t) && !is_flt(c, t)) {
        cerror(c, cnode_loc(c, i), "wrong type argument to %s",
               real ? "__real" : "__imag");
        set_err(c, i);
        return;
    }
    if (real) {
        copy_node(c, i, a);
        c->ty[i] = t;
        c->ef[i] &= ~(EF_LVALUE | EF_ADDRLV | EF_BITFIELD);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = c->ef[a] & EF_SIDE;
    if (is_flt(c, t)) {
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, 0);
    } else {
        c->ck[i] = c->ck[a] == K_ICE ? K_ICE : K_FOLD;
        c->cv[i] = 0;
        c->ef[i] |= c->ck[a] == K_ICE ? EF_INTOPS : EF_NOPCST;
    }
}

static void e_unary(Checker *c, uint32_t i)
{
    uint32_t a = first_child(c, i);
    int op = npunct(c, i);
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    switch (op) {
    case P_INC: case P_DEC:
        incdec(c, i, a, op == P_INC);
        return;
    case P_AMP:
        addr_of(c, i, a);
        return;
    case P_STAR:
        deref(c, i, a);
        return;
    case P_PLUS: case P_MINUS: case P_TILDE: case P_BANG:
        arith_unary(c, i, a, op);
        return;
    default:
        break;
    }
    switch (tckw(c, c->nodes[i].tok)) {
    case CK_EXTENSION:
        copy_node(c, i, a);
        return;
    case CK_REAL:
        real_imag(c, i, a, true);
        return;
    case CK_IMAG:
        real_imag(c, i, a, false);
        return;
    default:
        set_err(c, i);
        return;
    }
}

static void e_postfix(Checker *c, uint32_t i)
{
    uint32_t a = first_child(c, i);
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    incdec(c, i, a, npunct(c, i) == P_INC);
}

/* ---- constants through conversions ---------------------------------------------- */

static bool is_decimal_flt(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k >= TY_DEC32 && k <= TY_DEC128;
}

/* A floating value converted to integer type t (saturating). */
static bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out)
{
    unsigned bits = int_bits(c, t);
    if (bits == 0 || bits > 64)
        return false;
    if (f != f) {
        *out = 0;
        return true;
    }
    if (tkind(c, t) == TY_BOOL) {
        *out = f != 0;
        return true;
    }
    if (is_signed(c, t)) {
        int64_t max = bits == 64 ? INT64_MAX : (INT64_C(1) << (bits - 1)) - 1;
        int64_t min = -max - 1;
        if (f >= (long double)max)
            *out = (uint64_t)max;
        else if (f <= (long double)min)
            *out = (uint64_t)min;
        else
            *out = (uint64_t)(int64_t)f;
    } else {
        uint64_t max = bits == 64 ? ~UINT64_C(0) : (UINT64_C(1) << bits) - 1;
        if (f <= 0)
            *out = 0;
        else if (f >= (long double)max)
            *out = max;
        else
            *out = (uint64_t)f;
    }
    return true;
}

/* Node i (whose type is already set to `to`) gets the value of node a
 * converted to `to`, as a cast or the usual conversions would. */
static void conv_const(Checker *c, uint32_t i, uint32_t a, TypeId to)
{
    TypeId from = rvt(c, a);
    long double f;
    c->ck[i] = K_NONE;
    c->cv[i] = 0;
    c->cb[i] = 0;
    if (is_int(c, to)) {
        if (has_ival(c, a) && int_bits(c, to) <= 64 && int_bits(c, from) <= 64) {
            uint64_t v = tkind(c, to) == TY_BOOL ? c->cv[a] != 0
                                                 : cexpr_trunc(c, to, c->cv[a]);
            c->cv[i] = v;
            if (c->ck[a] == K_ICE) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS | (c->ef[a] & EF_OVERFLOW);
            } else {
                c->ck[i] = K_FOLD;
                if (c->ef[a] & (EF_CST | EF_NOPCST | EF_REALCST))
                    c->ef[i] |= EF_NOPCST;
                else if (c->ef[a] & EF_INTOPS)
                    c->ef[i] |= EF_INTOPS;
                c->ef[i] |= c->ef[a] & EF_OVERFLOW;
            }
        } else if (c->ck[a] == K_FLOAT &&
                   float_to_int(c, c->fv.data[c->cv[a]], to, &c->cv[i])) {
            if (c->ef[a] & EF_REALCST) {
                c->ck[i] = K_ICE;
                c->ef[i] |= EF_INTOPS;
            } else {
                c->ck[i] = K_FOLD;
                c->ef[i] |= EF_NOPCST;
            }
        } else if (c->ck[a] == K_ADDR && is_ptr(c, from)) {
            if (c->cb[a] == 0) {
                c->ck[i] = K_FOLD;
                c->cv[i] = tkind(c, to) == TY_BOOL ? c->cv[a] != 0
                                                   : cexpr_trunc(c, to, c->cv[a]);
                c->ef[i] |= EF_NOPCST;
            } else if (int_bits(c, from) == int_bits(c, to) &&
                       tkind(c, to) != TY_BOOL) {
                c->ck[i] = K_ADDR;
                c->cb[i] = c->cb[a];
                c->cv[i] = c->cv[a];
            }
        }
        return;
    }
    if (is_flt(c, to)) {
        if (!is_decimal_flt(c, to) && !is_decimal_flt(c, from) &&
            fval(c, a, &f)) {
            c->ck[i] = K_FLOAT;
            c->cv[i] = fpush(c, fround(c, to, f));
        }
        return;
    }
    if (is_ptr(c, to)) {
        if (has_ival(c, a) && int_bits(c, from) <= 64) {
            c->ck[i] = K_ADDR;
            c->cv[i] = cexpr_trunc(c, to, c->cv[a]);
        } else if (c->ck[a] == K_ADDR &&
                   (is_ptr(c, from) ||
                    (is_int(c, from) && int_bits(c, from) == int_bits(c, to)))) {
            c->ck[i] = K_ADDR;
            c->cb[i] = c->cb[a];
            c->cv[i] = c->cv[a];
        }
    }
}

/* ---- sizeof, _Alignof ------------------------------------------------------------ */

/* Is t variably modified in a way that makes its size a run-time value? */
static bool var_size(Checker *c, TypeId t)
{
    for (;;) {
        TypeId ct = type_canon(TT, t);
        TypeKind k = tkind(c, ct);
        if (k == TY_VLA)
            return true;
        if (k == TY_ARRAY) {
            t = type_base(TT, ct);
            continue;
        }
        if (k == TY_STRUCT || k == TY_UNION)
            return (type_record(TT, ct)->flags & RF_VLA) != 0;
        return false;
    }
}

static void set_ice(Checker *c, uint32_t i, TypeId t, uint64_t v)
{
    c->ty[i] = t;
    c->ck[i] = K_ICE;
    c->cv[i] = cexpr_trunc(c, t, v);
    c->ef[i] = EF_INTOPS;
}

static const Field *member_field_of(Checker *c, uint32_t x, TypeId *recp);

/* Whether x (parentheses ignored) names an object. */
static bool obj_ident(Checker *c, uint32_t x)
{
    uint32_t s = strip_paren(c, x), ref;
    if (s == NO_NODE || ntag(c, s) != N_IDENT)
        return false;
    ref = lookup_ord(c, cnode_ident(c, s));
    return ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ;
}

static void e_sizeof(Checker *c, uint32_t i, bool align)
{
    uint32_t a = first_child(c, i);
    unsigned tag = ntag(c, i);
    bool is_type = tag == N_SIZEOF_TYPE || tag == N_ALIGNOF_TYPE;
    const char *op = align ? "__alignof__" : "sizeof";
    SrcLoc loc;
    TypeId t;
    uint64_t v = 1;
    bool ok;
    if (a == NO_NODE) {
        set_err(c, i);
        return;
    }
    if (align && !cexpr_in_extension(c, i)) {
        size_t len;
        const char *s = ttext(c, c->nodes[i].tok, &len);
        if (len == 8 && !memcmp(s, "_Alignof", 8)) {
            cped11(c, cnode_loc(c, i), "ISO C99 does not support '_Alignof'");
            if (!is_type)
                cpedantic(c, cnode_loc(c, i),
                          "ISO C does not allow '_Alignof (expression)'");
        }
    }
    if (is_type) {
        t = type_of_typename(c, a);
        loc = first_loc(c, a);
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
    } else {
        if (node_err(c, a)) {
            /* c_alignof_expr of an erroneous operand is a constant 1 */
            if (align)
                set_ice(c, i, size_type(c), 1);
            else
                set_err(c, i);
            return;
        }
        t = c->ty[a];
        loc = align ? cnode_loc(c, i) : first_loc(c, a);
        if (c->ef[a] & EF_BITFIELD) {
            if (align) {
                cerror(c, loc, "'__alignof' applied to a bit-field");
                set_ice(c, i, size_type(c), 1);
                return;
            }
            cerror(c, loc, "'sizeof' applied to a bit-field");
        }
    }
    if (!is_type && !align) {
        uint32_t s = strip_paren(c, a);
        if (s != NO_NODE && ntag(c, s) == N_IDENT) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, s));
            if (ref != SYM_NONE && (csym(c, ref)->flags & CSF_ARRAY_PARM)) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc,
                                        "sizeof-array-argument", "'sizeof' on "
                                        "array function parameter '%s' will "
                                        "return size of %s",
                                        cident(c, cnode_ident(c, s)),
                                        type_q(TT, t));
                cnote(c, d, csym(c, ref)->loc, "declared here");
            }
        }
    }
    if (is_func(c, t)) {
        if (!align)
            ped_arith(c, i, loc, "invalid application of 'sizeof' to a function "
                                 "type");
        else
            ped(c, i, loc, "ISO C does not permit '_Alignof' applied to a "
                           "function type");
        v = align ? 8 : 1;
    } else if (is_void(c, t)) {
        char buf[96];
        snprintf(buf, sizeof buf, "invalid application of '%s' to a void type",
                 op);
        ped_arith(c, i, loc, buf);
    } else if (align && !is_type && !complete(c, t) && obj_ident(c, a)) {
        /* c_alignof_expr takes a variable's DECL_ALIGN: an incomplete object
         * is accepted */
        v = tkind(c, type_canon(TT, t)) == TY_ARRAY ? type_align(TT, t) : 1;
    } else if (!complete(c, t)) {
        cerror(c, loc, "invalid application of '%s' to incomplete type %s", op,
               type_q(TT, t));
        set_err(c, i);
        return;
    } else if (!align) {
        if (var_size(c, t)) {
            c->ty[i] = size_type(c);
            return;
        }
        v = type_size(TT, t, &ok);
        if (!ok)
            v = 0;
    } else {
        v = type_align(TT, t);
        if (!is_type) {
            uint32_t s = strip_paren(c, a);
            if (s != NO_NODE && ntag(c, s) == N_IDENT) {
                uint32_t ref = lookup_ord(c, cnode_ident(c, s));
                /* aligned (n) sets a variable's alignment, even below the type's */
                if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ &&
                    csym(c, ref)->align)
                    v = csym(c, ref)->align;
            } else if (s != NO_NODE && ntag(c, s) == N_MEMBER_EXPR) {
                TypeId rec = 0;
                const Field *f = member_field_of(c, s, &rec);
                if (f) {
                    bool pk = (f->flags & FF_PACKED) ||
                              (type_record(TT, rec)->flags & RF_PACKED);
                    if (pk)
                        v = f->align ? f->align : 1;
                    else if (f->align > v)
                        v = f->align;
                }
            }
        }
    }
    set_ice(c, i, size_type(c), v);
}

/* ---- casts ----------------------------------------------------------------------- */


/* gcc's TREE_CODE class of a type for -Wbad-function-cast: all integer
 * types but _Bool and enums share INTEGER_TYPE. */
static int cast_class(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    if (k == TY_BOOL || k == TY_ENUM)
        return (int)k;
    if (is_int(c, t))
        return -1;
    if (is_flt(c, t))
        return -2;
    if (is_complex(c, t))
        return -3;
    return (int)k;
}

static void e_cast(Checker *c, uint32_t i)
{
    uint32_t k[2], a;
    SrcLoc loc = cnode_loc(c, i);
    TypeId t, ot;
    TypeKind tk;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    a = k[1];
    t = type_of_typename(c, k[0]);
    if (is_err(c, t) || node_err(c, a)) {
        set_err(c, i);
        return;
    }
    tk = tkind(c, t);
    if (tk == TY_ARRAY || tk == TY_VLA) {
        cerror(c, loc, "cast specifies array type");
        set_err(c, i);
        return;
    }
    if (tk == TY_FUNC) {
        cerror(c, loc, "cast specifies function type");
        set_err(c, i);
        return;
    }
    if (tk == TY_VOID) {
        if (!rvalue_ok(c, a)) {
            set_err(c, i);
            return;
        }
        c->ty[i] = unqual(c, t);
        c->ef[i] = c->ef[a] & EF_SIDE;
        return;
    }
    ot = rvt(c, a);
    if (is_void(c, ot)) {
        cerror(c, loc, "invalid use of void expression");
        set_err(c, i);
        return;
    }
    if (!rvalue_ok(c, a)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = unqual(c, t);
    c->ef[i] = c->ef[a] & EF_SIDE;
    if (tk == TY_VECTOR || (tkind(c, ot) == TY_VECTOR && tk != TY_UNION))
        return;
    if (is_record(c, t) || tk == TY_UNION) {
        if (mainv(c, t) == mainv(c, ot)) {
            ped(c, i, loc, "ISO C forbids casting nonscalar to the same type");
            return;
        }
        if (tk == TY_UNION) {
            const Record *r = type_record(TT, type_canon(TT, t));
            uint32_t f;
            for (f = 0; f < r->nfields; f++) {
                const Field *fl = &c->tt.fields.data[r->fields + f];
                if (type_compatible(TT, mainv(c, fl->ty), mainv(c, ot)))
                    break;
            }
            if (f == r->nfields) {
                cerror(c, loc, "cast to union type from type not present in "
                               "union");
                set_err(c, i);
                return;
            }
            ped(c, i, loc, "ISO C forbids casts to union type");
            return;
        }
        {
            SrcLoc el = after_loc(c, i);
            uint32_t m;
            /* gcc points at the tag of a struct/union type name */
            for (m = cfirst(c, k[0]); m < k[0]; m++)
                if (ntag(c, m) == N_TAG && c->par[m] != NO_NODE &&
                    ntag(c, c->par[m]) == N_STRUCT &&
                    c->par[c->par[m]] != NO_NODE &&
                    ntag(c, c->par[c->par[m]]) == N_SPECS &&
                    c->par[c->par[c->par[m]]] == k[0])
                    el = cnode_loc(c, m);
            cerror(c, el, "conversion to non-scalar type requested");
        }
        set_err(c, i);
        return;
    }
    if (is_record(c, ot)) {
        if (tk == TY_BOOL) {
            cerror(c, after_loc(c, i), "used %s type value where scalar is "
                   "required", tkind(c, ot) == TY_UNION ? "union" : "struct");
            set_err(c, i);
            return;
        }
        if (is_ptr(c, t)) {
            /* the cast keeps its type after the error */
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
            c->ty[i] = type_canon(TT, c->ty[i]);
            return;
        }
        if (is_int(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where an integer was "
                           "expected");
        else if (is_flt(c, t))
            cerror(c, after_loc(c, i), "aggregate value used where a floating-point was "
                           "expected");
        else if (is_ptr(c, t))
            cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        else
            cerror(c, after_loc(c, i), "aggregate value used where a complex was "
                           "expected");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, ot) && (is_flt(c, t) || is_complex(c, t))) {
        cerror(c, after_loc(c, i), is_flt(c, t) ? "pointer value used where a "
               "floating-point was expected" : "pointer value used where a "
               "complex was expected");
        set_err(c, i);
        return;
    }
    if (is_ptr(c, t) && !is_ptr(c, ot) && !is_int(c, ot)) {
        cerror(c, after_loc(c, i), "cannot convert to a pointer type");
        c->ty[i] = type_canon(TT, c->ty[i]);
        return;
    }
    if (is_ptr(c, t) && is_ptr(c, ot)) {
        TypeId pt = pointee(c, t), po = pointee(c, ot);
        if (is_func(c, pt) && !is_func(c, po) && !(c->ef[a] & EF_NPC))
            ped(c, i, loc, "ISO C forbids conversion of object pointer to "
                           "function pointer type");
        else if (!is_func(c, pt) && is_func(c, po))
            ped(c, i, loc, "ISO C forbids conversion of function pointer to "
                           "object pointer type");
    } else if (is_ptr(c, ot) && is_int(c, t) &&
               tk != TY_BOOL && tk != TY_ENUM &&
               int_bits(c, t) != int_bits(c, ot)) {
        cwarn(c, loc, "pointer-to-int-cast",
              "cast from pointer to integer of different size");
    }
    /* -Wbad-function-cast: a call cast to a type of another tree code */
    if (ntag(c, strip_paren(c, a)) == N_CALL && diag_enabled(c->diag, "bad-function-cast") &&
        cast_class(c, t) != cast_class(c, ot))
        cwarn(c, loc, "bad-function-cast", "cast from function call of type "
              "%s to non-matching type %s", type_q(TT, ot), type_q(TT, t));
    if (!(is_ptr(c, t) && is_ptr(c, ot)) &&
        is_ptr(c, t) && is_int(c, ot) && tkind(c, ot) != TY_BOOL &&
        tkind(c, ot) != TY_ENUM && int_bits(c, t) != int_bits(c, ot) &&
        c->ck[a] != K_ICE && c->ck[a] != K_FOLD) {
        cwarn(c, loc, "int-to-pointer-cast",
              "cast to pointer from integer of different size");
    }
    conv_const(c, i, a, c->ty[i]);
    if (is_ptr(c, t) && c->ck[i] == K_ADDR && c->cb[i] == 0 &&
        c->cv[i] == 0 && is_intcst(c, a) && !(c->ef[a] & EF_OVERFLOW) &&
        !(ntag(c, strip_paren(c, a)) == N_BINARY &&
          npunct(c, strip_paren(c, a)) == P_COMMA) &&
        is_void(c, pointee(c, t)) && tquals(c, pointee(c, t)) == 0)
        c->ef[i] |= EF_NPC;
}

/* ---- compound literals, statement expressions --------------------------------------- */

static void e_complit(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[0]);
    if (is_err(c, t)) {
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ARRAY &&
        (type_ent(TT, type_canon(TT, t))->flags & TF_INCOMPLETE)) {
        /* cinit.c left the element count in the list's cv */
        uint64_t n = c->ck[k[1]] == K_ICE ? c->cv[k[1]] : 1;
        t = type_array(TT, elem_of(c, t), n);
    }
    if (c->ck[k[1]] == K_ERR) {
        set_err(c, i);
        return;
    }
    if (!type_is_complete(TT, t)) {
        incomplete_error(c, cnode_loc(c, k[1]), NO_NODE, t);
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = EF_LVALUE;
    /* P2c: the initializer's contents */
    if (!in_function(c) && tkind(c, t) != TY_VLA) {
        c->ef[i] |= EF_ADDRLV;
        c->cb[i] = CB_NODE | i;
        addr_rvalue(c, i);
    }
}

static void e_stmt_expr(Checker *c, uint32_t i)
{
    uint32_t k = i - 1;
    SrcLoc loc = cnode_loc(c, i);
    if (!in_function(c)) {
        cerror(c, loc, "braced-group within expression allowed only inside a "
                       "function");
        set_err(c, i);
        return;
    }
    ped(c, i, loc, "ISO C forbids braced-groups within expressions");
    c->ty[i] = TYPE_B(VOID);
    c->ef[i] = EF_SIDE;
    if (c->nodes[i].size >= 4 && k >= 3 && ntag(c, k - 1) == N_SCOPE_END &&
        ntag(c, k - 2) == N_EXPR_STMT && c->nodes[k - 2].size > 1) {
        uint32_t e = k - 3;
        if (node_err(c, e)) {
            set_err(c, i);
            return;
        }
        c->ty[i] = rvt(c, e);
    }
}

/* ---- __builtin_va_arg, __builtin_convertvector, offsetof, types_compatible_p ----------- */

static void e_va_arg(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t, vl = c->tt.va_list, et;
    bool ok = false;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[1]);
    if (node_err(c, k[0]) || is_err(c, t)) {
        set_err(c, i);
        return;
    }
    et = c->ty[k[0]];
    if (vl && type_compatible(TT, mainv(c, et), mainv(c, vl)))
        ok = true;
    else if (vl && is_array(c, vl) && is_ptr(c, rvt(c, k[0])) &&
             type_compatible(TT, mainv(c, rvt(c, k[0])),
                             type_ptr(TT, mainv(c, elem_of(c, vl)))))
        ok = true;
    if (!ok) {
        cerror(c, expr_loc(c, k[0]), "first argument to 'va_arg' not of type "
                                     "'va_list'");
        set_err(c, i);
        return;
    }
    if (!complete(c, t)) {
        cerror(c, first_loc(c, k[1]), "second argument to 'va_arg' is of "
               "incomplete type %s", type_q(TT, t));
        set_err(c, i);
        return;
    }
    if (tkind(c, t) == TY_ENUM && cexpr_cxx_compat(c, i))
        cwarn(c, first_loc(c, k[1]), "c++-compat", "C++ requires promoted "
              "type, not enum type, in 'va_arg'");
    c->ty[i] = t;
    c->ef[i] = EF_SIDE;
}

static void e_convertvector(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId t;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[1]);
    if (node_err(c, k[0]) || is_err(c, t)) {
        set_err(c, i);
        return;
    }
    c->ty[i] = t;
    c->ef[i] = c->ef[k[0]] & EF_SIDE;
}

static void e_offsetof(Checker *c, uint32_t i)
{
    uint32_t k[64], n = nkids(c, i, k, 64), j;
    TypeId t, cur;
    uint64_t off = 0;
    bool konst = true;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    t = type_of_typename(c, k[0]);
    if (is_err(c, t)) {
        set_err(c, i);
        return;
    }
    cur = t;
    for (j = 1; j < n; j++) {
        unsigned tag = ntag(c, k[j]);
        if (tag == N_NAME || tag == N_DESIG_FIELD) {
            uint32_t name = cnode_ident(c, k[j]);
            SrcLoc loc = cnode_loc(c, k[j]);
            const Field *f;
            uint64_t o = 0;
            unsigned q = 0;
            if (tag == N_DESIG_FIELD && c->nodes[k[j]].flags && is_array(c, cur))
                cur = elem_of(c, cur);
            if (!is_record(c, cur)) {
                if (!is_err(c, cur))
                    cerror(c, loc, "request for member '%s' in something not a "
                           "structure or union", cident(c, name));
                set_err(c, i);
                return;
            }
            if (!complete(c, cur)) {
                incomplete_error(c, loc, NO_NODE, cur);
                set_err(c, i);
                return;
            }
            f = find_field(c, cur, name, &o, &q);
            if (!f) {
                cerror(c, loc, "%s has no member named '%s'", type_q(TT, cur),
                       cident(c, name));
                set_err(c, i);
                return;
            }
            if (f->flags & FF_BITFIELD) {
                cerror(c, loc, "cannot take address of bit-field '%s'",
                       cident(c, name));
                set_err(c, i);
                return;
            }
            off += o / 8;
            cur = f->ty;
        } else if (tag == N_DESIG_INDEX) {
            uint32_t e = first_child(c, k[j]);
            if (e == NO_NODE || node_err(c, e)) {
                set_err(c, i);
                return;
            }
            if (!is_array(c, cur)) {
                cerror(c, cnode_loc(c, k[j]), "subscripted value is neither "
                       "array nor pointer nor vector");
                set_err(c, i);
                return;
            }
            if (!is_int(c, rvt(c, e))) {
                cerror(c, cnode_loc(c, k[j]), "array subscript is not an "
                       "integer");
                set_err(c, i);
                return;
            }
            if (has_ival(c, e) && !var_size(c, elem_of(c, cur)))
                off += (uint64_t)(int64_t)c->cv[e] * elem_size(c, type_ptr(TT,
                                                                 elem_of(c, cur)));
            else
                konst = false;
            cur = elem_of(c, cur);
        }
    }
    c->ty[i] = size_type(c);
    if (konst)
        set_ice(c, i, size_type(c), off);
}

static void e_types_compat(Checker *c, uint32_t i)
{
    uint32_t k[2];
    TypeId a, b;
    if (nkids(c, i, k, 2) < 2) {
        set_err(c, i);
        return;
    }
    a = type_of_typename(c, k[0]);
    b = type_of_typename(c, k[1]);
    if (is_err(c, a) || is_err(c, b)) {
        set_err(c, i);
        return;
    }
    set_ice(c, i, TYPE_B(INT), type_compatible(TT, mainv(c, a), mainv(c, b)));
}

/* ---- __builtin_has_attribute ---------------------------------------------------------- */

/* The field a member-access node names (no diagnostics), and its record. */
static const Field *member_field_of(Checker *c, uint32_t x, TypeId *recp)
{
    uint32_t d = first_child(c, x);
    TypeId rt;
    uint64_t off;
    unsigned q;
    if (d == NO_NODE || node_err(c, d))
        return NULL;
    rt = c->ty[d];
    if (c->nodes[x].flags & NF_ARROW) {
        if (type_ckind(TT, rt) != TY_PTR && type_ckind(TT, rt) != TY_ARRAY)
            return NULL;
        rt = pointee(c, rt);
    }
    rt = type_canon(TT, rt);
    if (!is_record(c, rt))
        return NULL;
    if (recp)
        *recp = rt;
    return find_field(c, rt, cnode_ident(c, x), &off, &q);
}

/* The sets of attribute names that belong to the expression e: its declaration
 * (a symbol, a member), and its type's record.  strip: look through the
 * pointers and arrays of the type (what 'copy' does). */
unsigned cexpr_asets(Checker *c, uint32_t e, bool strip, uint32_t out[3])
{
    unsigned n = 0;
    uint32_t x = strip_paren(c, e);
    TypeId t;
    if (x == NO_NODE)
        return 0;
    if (ntag(c, x) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, x));
        if (ref != SYM_NONE && csym(c, ref)->aset)
            out[n++] = csym(c, ref)->aset;
    } else if (ntag(c, x) == N_MEMBER_EXPR) {
        const Field *f = member_field_of(c, x, NULL);
        if (f && f->aset)
            out[n++] = f->aset;
    }
    if (node_err(c, x))
        return n;
    t = c->ty[x];
    while (strip && (type_ckind(TT, t) == TY_PTR || type_ckind(TT, t) == TY_ARRAY))
        t = type_base(TT, t);
    t = type_canon(TT, t);
    if (is_record(c, t) && type_record(TT, t)->aset)
        out[n++] = type_record(TT, t)->aset;
    return n;
}

static void e_has_attr(Checker *c, uint32_t i)
{
    uint32_t k[2], sets[3], n = 0, j;
    char an[32];
    bool has = false;
    if (nkids(c, i, k, 2) < 2 || ntag(c, k[1]) != N_ATTR_ITEM) {
        set_err(c, i);
        return;
    }
    {
        size_t len;
        char raw[48];
        const char *tx = ttext(c, c->nodes[k[1]].tok, &len);
        if (len >= sizeof raw)
            len = sizeof raw - 1;
        memcpy(raw, tx, len);
        raw[len] = 0;
        cdecl_attr_name(raw, an, sizeof an);
    }
    if (ntag(c, k[0]) == N_TYPE_NAME) {
        TypeId t = type_of_typename(c, k[0]);
        uint32_t q, tmp = 0;
        if (is_err(c, t)) {
            set_err(c, i);
            return;
        }
        t = type_canon(TT, t);
        if (is_record(c, t))
            sets[n++] = type_record(TT, t)->aset;
        /* attributes written in the type name itself */
        for (q = cfirst(c, k[0]); q < k[0]; q++)
            if (ntag(c, q) == N_ATTRIBUTE)
                cdecl_attrs_names(c, q, &tmp);
        if (tmp)
            sets[n++] = tmp;
    } else {
        if (node_err(c, k[0])) {
            set_err(c, i);
            return;
        }
        n = cexpr_asets(c, k[0], false, sets);
    }
    for (j = 0; j < n; j++)
        has |= cdecl_aset_has(c, sets[j], an);
    set_ice(c, i, TYPE_B(INT), has);
}

/* ---- _Generic ------------------------------------------------------------------------ */

static void e_generic_assoc(Checker *c, uint32_t i)
{
    copy_node(c, i, i - 1);
}

static void e_generic(Checker *c, uint32_t i)
{
    uint32_t k[64], n = nkids(c, i, k, 64), j, match = NO_NODE, deflt = NO_NODE;
    TypeId sel, ts[64];
    bool bad = false;
    if (n < 2) {
        set_err(c, i);
        return;
    }
    if (!cexpr_in_extension(c, i))
        cped11(c, cnode_loc(c, i), "ISO C99 does not support '_Generic'");
    if (node_err(c, k[0])) {
        set_err(c, i);
        return;
    }
    sel = rvt(c, k[0]);
    sel = mainv(c, sel);
    for (j = 1; j < n; j++) {
        uint32_t tn = first_child(c, k[j]);
        SrcLoc loc;
        uint32_t m;
        ts[j] = 0;
        if (tn == NO_NODE)
            continue;
        if (ntag(c, tn) != N_TYPE_NAME) {
            if (deflt != NO_NODE) {
                Diagnostic *d = cerror_d(c, cnode_loc(c, k[j]), "duplicate "
                                         "'default' case in '_Generic'");
                cnote(c, d, cnode_loc(c, deflt), "original 'default' is here");
                bad = true;
            } else {
                deflt = k[j];
            }
            continue;
        }
        loc = first_loc(c, tn);
        ts[j] = type_of_typename(c, tn);
        if (is_err(c, ts[j])) {
            ts[j] = 0;
            bad = true;
            continue;
        }
        if (is_func(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has function type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        if (!complete(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has incomplete type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        if (var_size(c, ts[j])) {
            cerror(c, loc, "'_Generic' association has variable length type");
            bad = true;
            ts[j] = 0;
            continue;
        }
        for (m = 1; m < j; m++)
            if (ts[m] && type_compatible(TT, ts[m], ts[j])) {
                Diagnostic *d = cerror_d(c, loc, "'_Generic' specifies two "
                                         "compatible types");
                cnote(c, d, first_loc(c, first_child(c, k[m])),
                      "compatible type is here");
                bad = true;
                break;
            }
    }
    for (j = 1; j < n; j++) {
        if (!ts[j] || !type_compatible(TT, sel, ts[j]))
            continue;
        if (match != NO_NODE) {
            Diagnostic *d = cerror_d(c, cnode_loc(c, k[j]), "'_Generic' "
                                     "selector matches multiple associations");
            cnote(c, d, cnode_loc(c, match), "other match is here");
            bad = true;
        } else {
            match = k[j];
        }
    }
    if (match == NO_NODE)
        match = deflt;
    if (match == NO_NODE) {
        cerror(c, first_loc(c, k[0]), "'_Generic' selector of type %s is not "
               "compatible with any association", type_q(TT, sel));
        set_err(c, i);
        return;
    }
    if (bad) {
        set_err(c, i);
        return;
    }
    copy_node(c, i, match);
}

static void e_addr_label(Checker *c, uint32_t i)
{
    uint32_t tok = c->nodes[i].tok;
    if (!in_function(c)) {
        cerror(c, cinput_loc(c, tok + 1), "label '%s' referenced outside of "
               "any function", cident(c, cnode_ident(c, i)));
        c->ty[i] = type_ptr(TT, TYPE_B(VOID));
        c->ck[i] = K_ADDR;
        c->ef[i] = EF_NPC;
        return;
    }
    ped(c, i, cinput_loc(c, tok + 1), "taking the address of a "
                                                    "label is non-standard");
    c->ty[i] = type_ptr(TT, TYPE_B(VOID));
    c->ck[i] = K_ADDR;
    c->cb[i] = CB_NODE | i;
}

/* ---- binary operators ---------------------------------------------------------------- */

/* The value flags of a binary result whose integer value is v (ovf: it
 * overflowed in its type; int_const: gcc's int_const, false when the
 * operation is not a constant expression after all). */
static void bin_value(Checker *c, uint32_t i, uint32_t a, uint32_t b,
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
            cwarn(c, loc, "overflow", "integer overflow in expression of type "
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

static void invalid_operands(Checker *c, uint32_t i, uint32_t a, uint32_t b,
                             int op)
{
    const char *sp = punct_spelling[op];
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    /* the left operand of a compound assignment is no rvalue: it keeps its
     * qualifiers unless the promotions change it */
    if (ntag(c, i) == N_ASSIGN && mainv(c, ta) == mainv(c, c->ty[a])) {
        ta = c->ty[a];
        if (tquals(c, ta) & TQ_VOLATILE)
            ta = type_qual(unqual(c, ta), tquals(c, ta) & ~TQ_VOLATILE);
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

static void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId la, TypeId lb);

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

static bool truth_ok(Checker *c, uint32_t a)
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
    if (!(c->ef[a] & EF_SIDE)) {
        uint32_t s = strip_paren(c, a), ck[3];
        bool voidcast;
        /* (void) a, (void) b, c: a COMPOUND_EXPR ending in a void cast */
        while (ntag(c, s) == N_BINARY && npunct(c, s) == P_COMMA &&
               nkids(c, s, ck, 3) == 2)
            s = strip_paren(c, ck[1]);
        voidcast = ntag(c, s) == N_CAST && is_void(c, c->ty[s]);
        if (!voidcast && !inhibited(c, i, false))
            cwarn(c, cnode_loc(c, i), "unused-value", "left-hand operand of "
                  "comma expression has no effect");
    }
    t = rvt(c, b);
    c->ty[i] = t;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
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

static void e_logical(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
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
    cexpr_truth_warn(c, a, first_loc(c, a));
    cexpr_truth_warn(c, b, cnode_loc(c, i));
    c->ty[i] = TYPE_B(INT);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
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

static bool is_npc(Checker *c, uint32_t n)
{
    return (c->ef[n] & EF_NPC) != 0;
}

static bool extra_on(Checker *c)
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

static bool from_macro(Checker *c, uint32_t tok);

/* ---- -Wall / -Wextra expression warnings -------------------------------------
 * -Wsign-compare, -Wtype-limits, -Wbool-compare, -Wtautological-compare,
 * -Wparentheses, -Wlogical-not-parentheses, -Warray-compare, -Waddress and
 * -Wsizeof-pointer-div (gcc's shorten_compare, warn_for_sign_compare,
 * warn_about_parentheses, maybe_warn_for_null_address, ...). */

static bool is_cmp_op(int op)
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

static bool is_boolish(Checker *c, uint32_t n)
{
    return tkind(c, rvt(c, n)) == TY_BOOL || truth_expr(c, n);
}

/* The node's value when it is an integer constant: *neg, the magnitude. */
static void cst_parts(Checker *c, uint32_t n, bool *neg, uint64_t *mag)
{
    TypeId t = rvt(c, n);
    uint64_t v = c->cv[n];
    *neg = ival_neg(c, t, v);
    *mag = *neg ? (uint64_t)0 - v : v;
}

/* tree_expr_nonnegative_p of a (signed) operand. */
static bool nonneg(Checker *c, uint32_t n)
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
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR:
            return true;
        default: return false;
        }
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
static unsigned bf_width(Checker *c, uint32_t n)
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
static const char *cmp_tstr(Checker *c, uint32_t n)
{
    unsigned w = bf_width(c, n);
    TypeId t = rvt(c, n);
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

/* warn_for_sign_compare */
static void sign_compare(Checker *c, SrcLoc loc, uint32_t a, uint32_t b,
                         int op, TypeId rt)
{
    TypeId ta = cmp_ty(c, a), tb = cmp_ty(c, b);
    bool sa, sb;
    uint32_t sop, uop;
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
static uint32_t narrower(Checker *c, uint32_t n, TypeId *ty, unsigned *prec)
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
    if (!rts && c->cv[y] == 0 && (code == P_GE || code == P_LT)) {
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
static bool opeq(Checker *c, uint32_t x, uint32_t y)
{
    uint32_t kx[3], ky[3], nx, ny;
    x = strip_paren(c, x);
    y = strip_paren(c, y);
    if (ntag(c, x) != ntag(c, y) || (c->ef[x] | c->ef[y]) & EF_SIDE)
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
static bool zero_size_ok(const char *name)
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
static bool zero_size_ok(const char *name);
static void check_restrict(Checker *c, const uint32_t *kv, uint32_t nk,
                           uint32_t parms, uint32_t nparm, SrcLoc loc,
                           bool builtin)
{
    uint32_t i, j;
    for (i = 0; i < nparm && i + 1 < nk; i++) {
        if (!cparm_restrict(c, parms, i))
            continue;
        for (j = i + 1; j < nparm && j + 1 < nk; j++) {
            uint32_t a = kv[i + 1], first;
            SrcLoc l = line_start_loc(c, loc);
            if (!cparm_restrict(c, parms, j) || node_err(c, a) || node_err(c, kv[j + 1]))
                continue;
            if (!opeq(c, restrict_base(c, a), restrict_base(c, kv[j + 1])))
                continue;
            if (builtin && nparm == 3 && nk > 3 && is_intcst(c, kv[3]) &&
                c->cv[kv[3]] == 0)
                continue;
            first = strip_paren(c, a);
            if (ntag(c, first) != N_IDENT || is_array(c, c->ty[first]))
                l = expr_loc(c, a);
            cwarn(c, l, "restrict", "passing argument %u to 'restrict'-qualified "
                  "parameter aliases with argument %u", i + 1, j + 1);
        }
    }
}

/* warn_tautological_cmp */
static void tauto_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b), bit, cst;
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
    if (is_flt(c, t) || is_complex(c, t) || is_array(c, t) || is_func(c, t) ||
        is_record(c, t) || tkind(c, t) == TY_VECTOR)
        return;
    if (ntag(c, sa) == N_INDEX) {
        uint32_t k[3];
        if (nkids(c, sa, k, 3) == 2 && is_array(c, c->ty[strip_paren(c, k[0])]) &&
            is_intcst(c, k[1]))
            return;
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

#define PW(l, ...) cwarn(c, (l), "parentheses", __VA_ARGS__)

/* warn_about_parentheses */
static void parens_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
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

/* warn_logical_not_parentheses (the caller of it in parser_build_binary_op
 * included). */
static void lognot_warn(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    uint32_t k[3], x, y;
    Diagnostic *d;
    if (!is_cmp_op(op) || ntag(c, a) != N_UNARY || npunct(c, a) != P_BANG ||
        (ntag(c, b) == N_UNARY && npunct(c, b) == P_BANG) ||
        nkids(c, a, k, 3) < 1)
        return;
    x = strip_paren(c, k[0]);
    if (ntag(c, x) == N_UNARY && npunct(c, x) == P_BANG)
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
        (is_intcst(c, b) && c->cv[b] == 0))
        return;
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "logical-not-parentheses",
                "logical not is only applied to the left hand side of "
                "comparison");
    cnote(c, d, first_loc(c, a), "add parentheses around left hand side "
          "expression to silence this warning");
}

/* ---- -Warray-compare ---- */

static void array_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
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
    uint32_t ref;        /* its symbol, SYM_NONE for a member */
} AddrInfo;

/* The object whose address the pointer-valued expression n is, if it is one
 * gcc's decl_with_nonnull_addr_p accepts. */
static bool addr_target(Checker *c, uint32_t n, AddrInfo *ai)
{
    uint32_t k[3], e;
    bool deref = false;
    n = strip_paren(c, n);
    memset(ai, 0, sizeof *ai);
    ai->ref = SYM_NONE;
    if (ntag(c, n) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, n));
        CSym *s;
        if (ref == SYM_NONE)
            return false;
        s = csym(c, ref);
        if (!((s->kind == CS_FUNC) ||
              (s->kind == CS_OBJ && is_array(c, c->ty[n]))) ||
            (s->flags & CSF_WEAK))
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
                return false;
            e = base;
            deref = true;
            continue;
        }
        break;
    }
    if (ntag(c, e) == N_IDENT) {
        uint32_t ref = lookup_ord(c, cnode_ident(c, e));
        CSym *s;
        if (ref == SYM_NONE)
            return false;
        s = csym(c, ref);
        if ((s->kind != CS_OBJ && s->kind != CS_FUNC) || (s->flags & CSF_WEAK))
            return false;
        ai->name = cident(c, s->name);
        ai->dloc = s->loc;
        ai->direct = !deref;
        ai->ref = ref;
        return true;
    }
    return false;
}

/* maybe_warn_for_null_address's POINTER_PLUS_EXPR case: x is 'ptr +- N'
 * with a constant N.  Returns true when x is such a sum (warned or not). */
static bool ptr_plus_warn(Checker *c, SrcLoc loc, uint32_t x, int code)
{
    uint32_t k[3], pn, in;
    int op;
    int64_t off;
    TypeId pt;
    StrBuf sb = {0};
    if (ntag(c, x) != N_BINARY || nkids(c, x, k, 3) != 2)
        return false;
    op = npunct(c, x);
    if (op != P_PLUS && op != P_MINUS)
        return false;
    if (is_ptr(c, rvt(c, k[0])) && is_int(c, rvt(c, k[1]))) {
        pn = k[0];
        in = k[1];
    } else if (op == P_PLUS && is_ptr(c, rvt(c, k[1])) &&
               is_int(c, rvt(c, k[0]))) {
        pn = k[1];
        in = k[0];
    } else {
        return false;
    }
    pt = rvt(c, x);
    if (!is_ptr(c, pt) || is_void(c, pointee(c, pt)))
        return true;
    if (!prints_value(c, in))
        return true;
    off = (int64_t)cexpr_trunc(c, rvt(c, in), c->cv[in]) *
          (int64_t)elem_size(c, pt);
    if (op == P_MINUS)
        off = -off;
    pn = strip_paren(c, pn);
    if (is_array(c, c->ty[pn])) {
        sb_putc(&sb, '(');
        type_print(TT, &sb, rvt(c, pn));
        sb_puts(&sb, ")&");
        pexpr(c, &sb, pn, PR_UNARY);
    } else {
        pexpr(c, &sb, pn, PR_ADD);
    }
    sb_printf(&sb, " + %lld", (long long)off);
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
    cnote(c, d, ai->dloc, "'%s' declared here", ai->name);
}

/* maybe_warn_for_null_address */
static bool null_addr_warn(Checker *c, SrcLoc loc, uint32_t x, int code)
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
    if (inhibited(c, x, false) || from_macro(c, c->nodes[strip_paren(c, x)].tok))
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
void cexpr_truth_warn(Checker *c, uint32_t n, SrcLoc loc)
{
    AddrInfo ai;
    uint32_t s = strip_paren(c, n), k[3];
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
    if (ptr_plus_warn(c, loc, s, P_NE))
        return;
    if (!addr_target(c, s, &ai))
        return;
    if (ai.direct && ai.ref != SYM_NONE &&
        !(csym(c, ai.ref)->flags & CSF_ADDR_WARNED)) {
        csym(c, ai.ref)->flags |= CSF_ADDR_WARNED;
        cwarn(c, loc, "address", "the address of '%s' will always evaluate as "
              "'true'", ai.name);
        return;
    }
    null_addr_msg(c, loc, &ai, P_NE);
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

static void sizeof_div(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId t0, t1;
    uint32_t d0, d1;
    StrBuf s0 = {0}, s1 = {0};
    Diagnostic *d;
    if (!sizeof_operand(c, a, &t0, &d0) || !sizeof_operand(c, b, &t1, &d1) ||
        !is_ptr(c, t0) || is_err(c, t1))
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
    folded = type_limits(c, i, a, b, op, rt, loc);
    if (!folded && !inhibited(c, i, false) && diag_enabled(c->diag, "sign-compare"))
        sign_compare(c, loc, a, b, op, rt);
    return folded || bfold;
}


static TypeId vec_elem(Checker *c, TypeId vt);
static unsigned vec_esize(Checker *c, TypeId el);
static int vec_scalar(Checker *c, uint32_t i, uint32_t sn, TypeId st, TypeId vt,
                      bool strict_int);
static void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
                        TypeId la, TypeId lb);

static void e_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool eq = op == P_EQEQ || op == P_NE;
    bool pa = is_ptr(c, ta), pb = is_ptr(c, tb);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
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
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
    {
        bool fold = false;
        if (is_arith(c, ta) && is_arith(c, tb))
            fold = compare_limits(c, i, a, b, op, ta, tb);
        else if (eq && pa && is_npc(c, b))
            fold = null_addr_warn(c, loc, a, op);
        else if (eq && pb && is_npc(c, a))
            fold = null_addr_warn(c, loc, b, op);
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
                cwarn(c, loc, "", "ordered comparison of pointer with null "
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
                    cwarn(c, loc, "", "ordered comparison of pointer with "
                                      "integer zero");
            }
        } else {
            cpedwarn(c, loc, "", "comparison between pointer and integer");
        }
    } else if (tkind(c, rvt(c, a)) == TY_ENUM && tkind(c, rvt(c, b)) == TY_ENUM &&
               mainv(c, rvt(c, a)) != mainv(c, rvt(c, b))) {
        cwarn(c, loc, "enum-compare", "comparison between %s and %s",
              type_q(TT, unqual(c, rvt(c, a))), type_q(TT, unqual(c, rvt(c, b))));
    }
    if (!inhibited(c, i, false) &&
        (eq ? (((c->ef[sa] & EF_STRING) && !is_npc(c, b)) ||
               ((c->ef[sb] & EF_STRING) && !is_npc(c, a)))
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
static unsigned min_prec_signed(uint64_t v)
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

/* ---- vector operands (c-typeck.cc build_binary_op, c-common.cc scalar_to_vector) ---- */

static TypeId vec_elem(Checker *c, TypeId vt)
{
    return type_base(TT, type_canon(TT, vt));
}

static unsigned vec_esize(Checker *c, TypeId el)
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
static int vec_scalar(Checker *c, uint32_t i, uint32_t sn, TypeId st, TypeId vt,
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
static void vec_invalid(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op,
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
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
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
        vec_binop(c, i, a, b, op, true);
        return;
    }
    if (!is_int(c, ta) || !is_int(c, tb)) {
        invalid_operands(c, i, a, b, op);
        return;
    }
    c->ty[i] = ta;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
    prec = int_bits(c, ta);
    if (has_ival(c, b)) {
        if (ival_neg(c, tb, c->cv[b])) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, "shift-count-negative", "%s shift count is "
                      "negative", dir);
        } else if (c->cv[b] >= prec) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, "shift-count-overflow", "%s shift count >= width "
                      "of type", dir);
        } else {
            cnt_ok = true;
        }
    }
    if (left && is_intcst(c, a) && is_signed(c, ta) &&
        ival_neg(c, ta, c->cv[a])) {
        int_const = false;
        if (!inhibited(c, i, false))
            cwarn(c, loc, "shift-negative-value", "left shift of negative "
                  "value");
    }
    if (!(cnt_ok && has_ival(c, a) && prec <= 64)) {
        if (intops(c, a) && intops(c, b))
            c->ef[i] |= EF_INTOPS;
        return;
    }
    if (left && is_intcst(c, a) && is_intcst(c, b) && is_signed(c, ta)) {
        unsigned mp = min_prec_signed(cexpr_trunc(c, ta, c->cv[a])) +
                      (unsigned)c->cv[b];
        if (mp == prec + 1) {
            int_const = false;
        } else if (mp > prec + 1) {
            int_const = false;
            if (!inhibited(c, i, false))
                cwarn(c, loc, "shift-overflow=", "result of '%s' requires %u "
                      "bits to represent, but %s only has %u bits", estr(c, i),
                      mp, type_q(TT, ta), prec);
        }
    }
    {
        bool ovf, zdiv;
        uint64_t v = int_op(c, op, ta, cexpr_trunc(c, ta, c->cv[a]), c->cv[b],
                            &ovf, &zdiv);
        bin_value(c, i, a, b, v, false, int_const, loc);
    }
}

/* Is token tok the result of a macro expansion? */
static bool from_macro(Checker *c, uint32_t tok)
{
    const PTok *t = &c->u->toks[tok];
    return t->exp && t->exp != t->t.loc;
}

/* -Wxor-used-as-pow */
static void xor_pow(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
    unsigned long long l, r;
    Diagnostic *d;
    char sug[64];
    if (!(c->ef[a] & EF_DECIMAL) || !(c->ef[b] & EF_DECIMAL) ||
        !has_ival(c, a) || !has_ival(c, b) || ntag(c, sa) != N_NUMBER ||
        ntag(c, sb) != N_NUMBER || from_macro(c, c->nodes[sa].tok) ||
        from_macro(c, c->nodes[sb].tok))
        return;
    l = c->cv[a];
    r = c->cv[b];
    if (l == 2) {
        if (r <= 30)
            snprintf(sug, sizeof sug, "'1 << %llu' (%llu)", r,
                     (unsigned long long)1 << r);
        else if (r <= 62)
            snprintf(sug, sizeof sug, "'1LL << %llu'", r);
        else
            snprintf(sug, sizeof sug, "exponentiation");
    } else if (l == 10) {
        snprintf(sug, sizeof sug, "'1e%llu'", r);
    } else {
        return;
    }
    d = cwarn_d(c, DL_WARNING, cnode_loc(c, i), "xor-used-as-pow",
                "result of '%llu^%llu' is %llu; did you mean %s?", l, r, l ^ r,
                sug);
    cnote(c, d, first_loc(c, a), "you can silence this warning by using a "
          "hexadecimal constant (%s rather than %llu)", l == 2 ? "0x2" : "0xa",
          l);
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
    c->ef[i] = (c->ef[p] | c->ef[n]) & EF_SIDE;
    if (c->ck[p] == K_ADDR && has_ival(c, n)) {
        uint64_t d = c->cv[n] * elem_size(c, pt);
        c->ck[i] = K_ADDR;
        c->cb[i] = c->cb[p];
        c->cv[i] = cexpr_trunc(c, pt, minus ? c->cv[p] - d : c->cv[p] + d);
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
    c->ty[i] = TYPE_MK(c->tgt->ptrdiff_type, 0);
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
    sz = elem_size(c, ta);
    if (c->ck[a] == K_ADDR && c->ck[b] == K_ADDR && c->cb[a] == c->cb[b] &&
        sz > 0 && !var_size(c, pa)) {
        c->ck[i] = K_FOLD;
        c->cv[i] = cexpr_trunc(c, c->ty[i],
                               (uint64_t)((int64_t)(c->cv[a] - c->cv[b]) /
                                          (int64_t)sz));
    }
}

static bool zero_ice(Checker *c, uint32_t n)
{
    return is_intcst(c, n) && c->ck[n] == K_ICE && c->cv[n] == 0;
}

/* fold-const's x * 0, 0 * x, x & 0, 0 & x and x - x (x a plain variable) */
static bool fold_zero_ident(Checker *c, int op, uint32_t a, uint32_t b)
{
    if (((c->ef[a] | c->ef[b]) & EF_SIDE) || !is_int(c, rvt(c, a)) ||
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
    rt = common_type(c, ta, tb);
    c->ty[i] = rt;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
    if (!is_complex(c, rt) && is_arith(c, rt)) {
        conv_operand(c, loc, rt, a, false);
        conv_operand(c, loc, rt, b, false);
    }
    if (op == P_CARET)
        xor_pow(c, i, a, b);
    if ((op == P_SLASH || op == P_PERCENT) && is_int(c, tb) &&
        is_intcst(c, b) && c->cv[b] == 0) {
        zero_div = true;
        if (!inhibited(c, i, false))
            cwarn(c, loc, "div-by-zero", "division by zero");
    }
    if (is_complex(c, rt))
        return;
    if (is_int(c, rt)) {
        if (!zero_div && int_bits(c, rt) <= 64 && has_ival(c, a) &&
            has_ival(c, b)) {
            bool ovf, zd;
            uint64_t v = int_op(c, op, rt, cexpr_trunc(c, rt, c->cv[a]),
                                cexpr_trunc(c, rt, c->cv[b]), &ovf, &zd);
            bin_value(c, i, a, b, v, ovf, true, loc);
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
        long double r = 0;
        bool cst = c->ck[a] != K_ADDR && c->ck[b] != K_ADDR;
        switch (op) {
        case P_PLUS: r = fa + fb; break;
        case P_MINUS: r = fa - fb; break;
        case P_STAR: r = fa * fb; break;
        default: r = fa / fb; break;
        }
        c->ck[i] = K_FLOAT;
        c->cv[i] = fpush(c, fround(c, rt, r));
        if (cst)
            c->ef[i] |= EF_REALCST;
        return;
    }
    if (intops(c, a) && intops(c, b))
        c->ef[i] |= EF_INTOPS;
}

static void e_binary(Checker *c, uint32_t i)
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
    if (is_cmp_op(op)) {
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

static SrcLoc colon_loc(Checker *c, uint32_t i, uint32_t mid, uint32_t els)
{
    uint32_t t, from = (mid != NO_NODE ? last_tok(c, mid) : c->nodes[i].tok) + 1,
             to = first_tok(c, els);
    for (t = from; t < to; t++)
        if (tpunct(c, t) == P_COLON)
            return ctok_loc(c, t);
    return cnode_loc(c, i);
}

static void e_cond(Checker *c, uint32_t i)
{
    uint32_t k[3], n = nkids(c, i, k, 3), cond, mid, els, ch;
    TypeId t1, t2, rt;
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
    if (mid == NO_NODE)
        ped(c, i, cl, "ISO C forbids omitting the middle term of a '?:' "
                      "expression");
    ok = binop_operand_at(c, cond, cnode_loc(c, i)) && truth_ok_at(c, cond, cnode_loc(c, i));
    if (ok)
        cexpr_truth_warn(c, cond, cnode_loc(c, i));
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
    if (cexpr_cxx_compat(c, i)) {
        TypeId o1 = orig_type(c, ch), o2 = orig_type(c, els);
        if (tkind(c, o1) == TY_ENUM && tkind(c, o2) == TY_ENUM &&
            mainv(c, o1) != mainv(c, o2))
            cwarn(c, cl, "c++-compat", "different enum types in conditional "
                  "is invalid in C++: %s vs %s", type_q(TT, o1),
                  type_q(TT, o2));
    }
    c->ef[i] = (c->ef[cond] | c->ef[els] | (mid != NO_NODE ? c->ef[mid] : 0)) &
               EF_SIDE;
    if (mainv(c, t1) == mainv(c, t2) && !is_arith(c, t1)) {
        rt = unqual(c, t1);
    } else if (is_arith(c, t1) && is_arith(c, t2)) {
        rt = common_type(c, promoted(c, ch), promoted(c, els));
        if (is_int(c, rt) && !is_signed(c, rt) &&
            is_int(c, promoted(c, ch)) && is_int(c, promoted(c, els)) &&
            is_signed(c, promoted(c, ch)) != is_signed(c, promoted(c, els)) &&
            diag_enabled(c->diag, "sign-compare") && !inhibited(c, i, false)) {
            uint32_t sa = is_signed(c, promoted(c, ch)) ? ch : els;
            uint32_t oa = sa == ch ? els : ch;
            if (!nonneg(c, sa))
                cwarn(c, ctok_loc(c, first_tok(c, sa)), "sign-compare",
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
        } else if (is_void(c, p1) || is_void(c, p2)) {
            TypeId vo = is_void(c, p1) ? p1 : p2, ot = vo == p1 ? p2 : p1;
            if (is_array(c, ot) && (gq(c, ot) & ~TYPE_QUALS(vo)))
                cwarn(c, cl, "discarded-array-qualifiers", "pointer to array "
                      "loses qualifier in conditional expression");
            if (is_func(c, p1) || is_func(c, p2))
                ped(c, i, cl, "ISO C forbids conditional expr between 'void *' "
                              "and function pointer");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), tquals(c, p1) | tquals(c, p2)));
        } else {
            cpedwarn(c, cl, "", "pointer type mismatch in conditional "
                                "expression");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), q));
        }
    } else if (is_ptr(c, t1) && is_int(c, t2)) {
        if (!is_npc(c, els))
            cpedwarn(c, cl, "", "pointer/integer type mismatch in conditional "
                                "expression");
        rt = t1;
    } else if (is_int(c, t1) && is_ptr(c, t2)) {
        if (!is_npc(c, ch))
            cpedwarn(c, cl, "", "pointer/integer type mismatch in conditional "
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
        uint16_t keep = c->ef[i];
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

static int assign_binop(int op)
{
    switch (op) {
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
    default: return op;
    }
}

static void e_assign(Checker *c, uint32_t i)
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
            int bop = assign_binop(op);
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
    c->ef[i] = EF_SIDE;
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
            cwarn(c, cnode_loc(c, k), "overflow", "integer overflow in "
                  "expression '%s' of type %s results in '%s'", estr(c, a),
                  type_q(TT, t), vstr(c, t, c->cv[k]));
        } else {
            cwarn(c, cnode_loc(c, k), "overflow", "integer overflow in "
                  "expression of type %s results in '%s'", type_q(TT, t),
                  vstr(c, t, c->cv[k]));
        }
    }
}

/* ---- asm statements ------------------------------------------------------------------- */

/* The characters of the string literal node n (adjacent literals joined;
 * simple escapes decoded). */
static size_t asm_string(Checker *c, uint32_t n, char *out, size_t cap)
{
    uint32_t np = c->nodes[n].aux ? c->nodes[n].aux : 1, t;
    size_t o = 0;
    for (t = 0; t < np; t++) {
        size_t len, j;
        const char *s = ttext(c, c->nodes[n].tok + t, &len);
        for (j = 0; j < len && s[j] != '"'; j++)
            ;
        for (j++; j + 1 < len && o + 1 < cap; j++) {
            char ch = s[j];
            if (ch == '\\' && j + 2 < len) {
                ch = s[++j];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'u': case 'U': ch = '\1'; break;  /* UCN: not decoded */
                case 'x': {
                    int v = 0;
                    while (j + 2 < len && isxdigit((unsigned char)s[j + 1])) {
                        char h = s[++j];
                        v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                    }
                    ch = (char)v;
                    break;
                }
                default:
                    if (ch >= '0' && ch <= '7') {
                        int v = ch - '0', k = 0;
                        while (k++ < 2 && j + 2 < len && s[j + 1] >= '0' &&
                               s[j + 1] <= '7')
                            v = v * 8 + (s[++j] - '0');
                        ch = (char)v;
                    }
                    break;
                }
            }
            out[o++] = ch;
        }
    }
    out[o] = 0;
    return o;
}

static const char *asm_chr(char ch, char *buf)
{
    if (ch >= 32 && ch < 127)
        snprintf(buf, 8, "%c", ch);
    else
        snprintf(buf, 8, "\\x%02x", (unsigned char)ch);
    return buf;
}

/* parse_output_constraint / parse_input_constraint, as far as they report
 * errors; false after one. */
static bool asm_constraint(Checker *c, SrcLoc loc, const char *k, bool out,
                           bool last, bool *reg, bool *mem)
{
    const char *p;
    char b[8];
    *reg = *mem = false;
    if (out && k[0] != '=' && k[0] != '+') {
        cerror(c, loc, "output operand constraint lacks '='");
        return false;
    }
    for (p = k; *p; p++) {
        switch (*p) {
        case '=': case '+':
            if (!out) {
                cerror(c, loc, "input operand constraint contains '%c'", *p);
                return false;
            }
            if (p != k) {
                cerror(c, loc, "operand constraint contains incorrectly "
                       "positioned '+' or '='");
                return false;
            }
            break;
        case '&':
            if (!out) {
                cerror(c, loc, "input operand constraint contains '&'");
                return false;
            }
            break;
        case '%':
            if (last) {
                cerror(c, loc, "'%%' constraint used with last operand");
                return false;
            }
            break;
        case '?': case '!': case '*': case '#': case '$': case '^': case ',':
        case ' ': case '\t': case '<': case '>':
            break;
        case 'V': case 'm': case 'o':
            *mem = true;
            break;
        case 'g': case 'X':
            *reg = *mem = true;
            break;
        case 'r': case 'p': case 'a': case 'b': case 'c': case 'd': case 'S':
        case 'D': case 'q': case 'Q': case 'R': case 'l': case 'A': case 'f':
        case 't': case 'u': case 'y': case 'x': case 'Y': case 'k': case 'v':
            *reg = true;
            break;
        case 'E': case 'F': case 'G': case 'H': case 's': case 'i': case 'n':
        case 'I': case 'J': case 'K': case 'L': case 'M': case 'N': case 'O':
        case 'P': case 'e': case 'Z': case 'B': case 'C': case 'T': case 'W':
            break;
        case '[':
            while (p[1] && p[1] != ']')
                p++;
            *reg = true;
            if (p[1])
                p++;
            break;
        default:
            if (*p >= '0' && *p <= '9') {
                if (out) {
                    cerror(c, loc, "matching constraint not valid in output "
                           "operand");
                    return false;
                }
                *reg = true;
            } else if (!isalpha((unsigned char)*p)) {
                cerror(c, loc, "invalid punctuation '%s' in constraint",
                       asm_chr(*p, b));
                return false;
            } else {
                *reg = *mem = true;     /* unknown: treat like "g" */
            }
            break;
        }
    }
    return true;
}

/* An asm operand's expression e (build_asm_expr). */
static void asm_operand(Checker *c, SrcLoc loc, uint32_t e, bool out, bool reg,
                        bool mem)
{
    uint32_t s;
    TypeId t;
    if (node_err(c, e))
        return;
    t = c->ty[e];
    if (out) {
        if (!(c->ef[e] & EF_LVALUE)) {
            cerror(c, loc, "lvalue required in 'asm' statement");
            return;
        }
        if (tquals(c, t) & TQ_CONST) {
            uint32_t v = strip_paren(c, e), ref = SYM_NONE;
            if (ntag(c, v) == N_IDENT)
                ref = lookup_ord(c, cnode_ident(c, v));
            if (ntag(c, v) == N_MEMBER_EXPR && cnode_ident(c, v))
                cerror(c, loc, "read-only member '%s' used as 'asm' output",
                       cident(c, cnode_ident(c, v)));
            else if (ref != SYM_NONE && csym(c, ref)->kind != CS_FUNC)
                cerror(c, loc, csym(c, ref)->flags & CSF_PARAM
                           ? "read-only parameter '%s' use as 'asm' output"
                           : "read-only variable '%s' used as 'asm' output",
                       cident(c, cnode_ident(c, v)));
            else
                cerror(c, loc, "read-only location '%s' used as 'asm' output",
                       estr(c, e));
            return;
        }
    }
    if (reg && (is_void(c, t) || (!out && !is_func(c, t) && tkind(c, t) != TY_ARRAY &&
                                   !type_is_complete(TT, rvt(c, e))))) {
        incomplete_error(c, loc, NO_NODE, rvt(c, e));
        return;
    }
    if (!reg && mem) {          /* c_mark_addressable */
        s = strip_paren(c, e);
        while (ntag(c, s) == N_CAST && s > 0)
            s = strip_paren(c, s - 1);
        if (ntag(c, s) == N_IDENT && (c->ef[s] & EF_REGISTER)) {
            uint32_t ref = lookup_ord(c, cnode_ident(c, s));
            cerror(c, loc, ref != SYM_NONE && !(ref & SYM_LOCAL)
                       ? "address of global register variable '%s' requested"
                       : "address of register variable '%s' requested",
                   cident(c, cnode_ident(c, s)));
        }
    }
}

uint32_t cdep_msg(Checker *c, uint32_t str_node)
{
    char buf[512];
    asm_string(c, str_node, buf, sizeof buf);
    vec_push(&c->dep_msgs, xstrdup(buf));
    return c->dep_msgs.len;
}

void cdep_use(Checker *c, SrcLoc loc, const CSym *s, const SrcLoc *note)
{
    cdep_report(c, loc, s->name, s->flags, s->dep_msg, note);
}

void cdep_report(Checker *c, SrcLoc loc, uint32_t nameid, uint32_t flags,
                 uint32_t dep_msg, const SrcLoc *note)
{
    const char *msg = NULL, *name;
    Diagnostic *d;
    if (!(flags & (CSF_DEPRECATED | CSF_UNAVAILABLE)) || !nameid)
        return;
    if (dep_msg && dep_msg <= c->dep_msgs.len)
        msg = c->dep_msgs.data[dep_msg - 1];
    name = cident(c, nameid);
    if (flags & CSF_UNAVAILABLE)
        d = msg ? cerror_d(c, loc, "'%s' is unavailable: %s", name, msg)
                : cerror_d(c, loc, "'%s' is unavailable", name);
    else
        d = msg ? cwarn_d(c, DL_WARNING, loc, "deprecated-declarations",
                          "'%s' is deprecated: %s", name, msg)
                : cwarn_d(c, DL_WARNING, loc, "deprecated-declarations",
                          "'%s' is deprecated", name);
    if (d && note)
        cnote(c, d, *note, "declared here");
}

typedef struct AsmOp {
    uint32_t e;
    bool out, reg, mem;
} AsmOp;

void cexpr_asm(Checker *c, uint32_t i)
{
    uint32_t kids[64], nk = node_children(c->nodes, i, kids, 64), k, j;
    SrcLoc loc = cnode_loc(c, i);
    char tmpl[1024], names[64][64];
    uint32_t nname = 0, nops = 0, tn = NO_NODE, nouts = 0, nins = 0;
    bool extended = false, ok = true;
    AsmOp ops[64];
    if (nk > 64)
        return;
    /* count the operands first (the last one may not use '%') */
    for (k = 0; k < nk; k++) {
        if (ntag(c, kids[k]) == N_STRING && tn == NO_NODE)
            tn = kids[k];
        if (ntag(c, kids[k]) == N_ASM_SECTION) {
            uint32_t sec = c->nodes[kids[k]].aux, oc[64];
            uint32_t n2 = node_children(c->nodes, kids[k], oc, 64);
            extended = true;
            for (j = 0; j < n2 && j < 64; j++)
                if (ntag(c, oc[j]) == N_ASM_OPERAND) {
                    if (sec == 1)
                        nouts++;
                    else if (sec == 2)
                        nins++;
                }
        }
    }
    for (k = 0; k < nk; k++) {
        uint32_t sec, n2, o, all[64];
        if (ntag(c, kids[k]) != N_ASM_SECTION)
            continue;
        sec = c->nodes[kids[k]].aux;
        n2 = node_children(c->nodes, kids[k], all, 64);
        if (sec > 2) {
            if (sec == 4)
                for (j = 0; j < n2 && j < 64; j++)
                    if (ntag(c, all[j]) == N_NAME && nname < 64) {
                        size_t len;
                        const char *s = ttext(c, c->nodes[all[j]].tok, &len);
                        snprintf(names[nname++], 64, "%.*s", (int)len, s);
                    }
            continue;
        }
        for (o = 0; o < n2 && o < 64; o++) {
            uint32_t oper = all[o], pc, st = NO_NODE, ex = NO_NODE;
            uint32_t nm = NO_NODE, ch[8];
            char con[256];
            bool reg, mem;
            if (ntag(c, oper) != N_ASM_OPERAND)
                continue;
            pc = node_children(c->nodes, oper, ch, 8);
            for (j = 0; j < pc && j < 8; j++) {
                if (ntag(c, ch[j]) == N_NAME)
                    nm = ch[j];
                else if (ntag(c, ch[j]) == N_STRING)
                    st = ch[j];
                else
                    ex = ch[j];
            }
            if (nm != NO_NODE && nname < 64) {
                size_t len, q;
                const char *s = ttext(c, c->nodes[nm].tok, &len);
                char nb[64];
                snprintf(nb, sizeof nb, "%.*s", (int)len, s);
                for (q = 0; q < nname; q++)
                    if (!strcmp(names[q], nb)) {
                        cerror(c, loc, "duplicate 'asm' operand name '%s'", nb);
                        ok = false;
                        break;
                    }
                strcpy(names[nname++], nb);
            }
            if (st == NO_NODE || ex == NO_NODE)
                continue;
            asm_string(c, st, con, sizeof con);
            if (asm_constraint(c, loc, con, sec == 1,
                               nops + 1 == nouts + nins, &reg, &mem) &&
                nops < 64) {
                ops[nops].e = ex;
                ops[nops].out = sec == 1;
                ops[nops].reg = reg;
                ops[nops].mem = mem;
                nops++;
            }
        }
    }
    for (j = 0; j < nops; j++)
        asm_operand(c, loc, ops[j].e, ops[j].out, ops[j].reg, ops[j].mem);
    if (!extended || tn == NO_NODE || !ok)
        return;
    /* %[name] in the template must name an operand */
    asm_string(c, tn, tmpl, sizeof tmpl);
    {
        const char *p;
        for (p = tmpl; *p; p++) {
            const char *q;
            size_t len, m;
            bool found = false;
            char nb[64];
            if (*p != '%')
                continue;
            q = p + 1;
            if (*q == '%') {
                p++;
                continue;
            }
            while (isalpha((unsigned char)*q) || *q == '=' || *q == '+' ||
                   *q == '-' || *q == '#' || *q == '*' || *q == '&')
                q++;
            if (*q != '[')
                continue;
            q++;
            len = strcspn(q, "]");
            if (!q[len])
                continue;
            snprintf(nb, sizeof nb, "%.*s", (int)len, q);
            for (m = 0; m < nname; m++)
                if (!strcmp(names[m], nb) || strchr(names[m], 92) ||
                    strchr(nb, 1) || (unsigned char)nb[0] >= 0x80)
                    found = true;
            if (!found) {
                cerror(c, loc, "undefined named operand '%s'", nb);
                return;
            }
            p = q + len;
        }
    }
}

/* ---- -Wsequence-point (a port of c-family/c-common.cc verify_tree) -------- */

/* A reference to an object: read when writer is NO_NODE, else written by
 * the node writer (an assignment or an increment). */
typedef struct TL {
    struct TL *next;
    uint32_t expr, writer;
} TL;

typedef struct SaveEnt {
    struct SaveEnt *next;
    uint32_t node;
    TL *before, *after;
} SaveEnt;

typedef struct {
    Checker *c;
    TL **pool;                  /* every allocated entry, freed at the end */
    size_t npool, cappool;
    TL *warned;
    SaveEnt *saves;
    uint32_t *saved;            /* nodes that gcc wraps in a SAVE_EXPR */
    uint32_t skip;
    size_t nsaved, capsaved;
    uint32_t *lnode;            /* locations gcc's fold moved to a comma */
    SrcLoc *lloc;
    size_t nloc, caploc;
} SeqCtx;

static TL *sq_new(SeqCtx *s, TL *next, uint32_t t, uint32_t writer)
{
    TL *l = malloc(sizeof *l);
    if (s->npool == s->cappool) {
        s->cappool = s->cappool ? s->cappool * 2 : 64;
        s->pool = realloc(s->pool, s->cappool * sizeof *s->pool);
    }
    s->pool[s->npool++] = l;
    l->next = next;
    l->expr = t;
    l->writer = writer;
    return l;
}

static bool sq_side(Checker *c, uint32_t x);

static uint32_t sq_strip(Checker *c, uint32_t x)
{
    while (x != NO_NODE && ntag(c, x) == N_PAREN)
        x = first_child(c, x);
    return x;
}

/* Whether the trees of x and y are the same object expression
 * (operand_equal_p): no side effects, same structure. */
static bool sq_eq(Checker *c, uint32_t x, uint32_t y)
{
    uint32_t kx[3], ky[3], nx, ny;
    x = sq_strip(c, x);
    y = sq_strip(c, y);
    if (x == y)
        return x != NO_NODE;
    if (x == NO_NODE || y == NO_NODE || ntag(c, x) != ntag(c, y))
        return false;
    switch (ntag(c, x)) {
    case N_IDENT:
        return cnode_ident(c, x) == cnode_ident(c, y) &&
               lookup_ord(c, cnode_ident(c, x)) != SYM_NONE;
    case N_NUMBER: case N_CHAR:
        return c->ck[x] == K_ICE && c->ck[y] == K_ICE && c->cv[x] == c->cv[y] &&
               c->ty[x] == c->ty[y];
    case N_UNARY:
        if (npunct(c, x) != npunct(c, y) || npunct(c, x) == P_INC ||
            npunct(c, x) == P_DEC)
            return false;
        break;
    case N_BINARY:
        if (npunct(c, x) != npunct(c, y) || npunct(c, x) == P_COMMA ||
            npunct(c, x) == P_ANDAND || npunct(c, x) == P_OROR)
            return false;
        break;
    case N_INDEX:
        break;
    case N_MEMBER_EXPR: {
        size_t lx, ly;
        const char *tx = ttext(c, c->nodes[x].tok, &lx);
        const char *ty = ttext(c, c->nodes[y].tok, &ly);
        if ((c->nodes[x].flags & NF_ARROW) != (c->nodes[y].flags & NF_ARROW) ||
            lx != ly || memcmp(tx, ty, lx))
            return false;
        break;
    }
    case N_CAST:
        if (c->ty[x] != c->ty[y])
            return false;
        break;
    default:
        return false;
    }
    nx = nkids(c, x, kx, 3);
    ny = nkids(c, y, ky, 3);
    if (nx != ny)
        return false;
    for (uint32_t k = ntag(c, x) == N_CAST ? 1 : 0; k < nx; k++)
        if (!sq_eq(c, kx[k], ky[k]))
            return false;
    return true;
}

static bool sq_same(Checker *c, uint32_t x, uint32_t y)
{
    return x == y || (x != NO_NODE && y != NO_NODE && sq_eq(c, x, y));
}

/* warning_candidate_p */
static bool sq_cand(Checker *c, uint32_t x)
{
    switch (ntag(c, x)) {
    case N_IDENT: case N_MEMBER_EXPR: case N_INDEX:
        break;
    case N_UNARY:
        if (npunct(c, x) != P_STAR)
            return false;
        break;
    default:
        return false;
    }
    return (c->ef[x] & EF_LVALUE) && !is_func(c, c->ty[x]) &&
           !is_array(c, c->ty[x]) &&
           !is_void(c, c->ty[x]) && !is_err(c, c->ty[x]);
}

static void sq_add(SeqCtx *s, TL **to, TL *add, uint32_t exclude, bool copy)
{
    while (add) {
        TL *next = add->next;
        if (!copy)
            add->next = *to;
        if (exclude == NO_NODE || !sq_same(s->c, add->writer, exclude))
            *to = copy ? sq_new(s, *to, add->expr, add->writer) : add;
        add = next;
    }
}

static void sq_merge(SeqCtx *s, TL **to, TL *add, bool copy)
{
    TL **end = to;
    while (*end)
        end = &(*end)->next;
    while (add) {
        bool found = false;
        TL *t2, *next = add->next;
        for (t2 = *to; t2; t2 = t2->next)
            if (sq_same(s->c, t2->expr, add->expr)) {
                found = true;
                if (t2->writer == NO_NODE)
                    t2->writer = add->writer;
            }
        if (!found) {
            *end = copy ? sq_new(s, NULL, add->expr, add->writer) : add;
            end = &(*end)->next;
            *end = NULL;
        }
        add = next;
    }
}

static SrcLoc sq_loc(SeqCtx *s, uint32_t n)
{
    for (size_t k = s->nloc; k-- > 0;)
        if (s->lnode[k] == n)
            return s->lloc[k];
    return cnode_loc(s->c, n);
}

static void sq_setloc(SeqCtx *s, uint32_t n, SrcLoc loc)
{
    if (s->nloc == s->caploc) {
        s->caploc = s->caploc ? s->caploc * 2 : 8;
        s->lnode = realloc(s->lnode, s->caploc * sizeof *s->lnode);
        s->lloc = realloc(s->lloc, s->caploc * sizeof *s->lloc);
    }
    s->lnode[s->nloc] = n;
    s->lloc[s->nloc++] = loc;
}

static void sq_collide1(SeqCtx *s, uint32_t written, uint32_t writer, TL *list,
                        bool only_writes)
{
    TL *t;
    for (t = s->warned; t; t = t->next)
        if (sq_same(s->c, t->expr, written))
            return;
    for (; list; list = list->next)
        if (sq_same(s->c, list->expr, written) &&
            !sq_same(s->c, list->writer, writer) &&
            (!only_writes || list->writer != NO_NODE)) {
            s->warned = sq_new(s, s->warned, written, NO_NODE);
            cwarn(s->c, sq_loc(s, writer), "sequence-point",
                  "operation on '%s' may be undefined",
                  estr(s->c, list->expr));
        }
}

static void sq_collide(SeqCtx *s, TL *list)
{
    TL *t;
    for (t = list; t; t = t->next)
        if (t->writer != NO_NODE)
            sq_collide1(s, t->expr, t->writer, list, false);
}

static void sq_verify(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno,
                      uint32_t writer);
static bool sq_truth_wraps(Checker *c, uint32_t e);

/* an operand that gcc truth-converts to `x != 0` first: one more level */
static void sq_verify_t(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno)
{
    if (sq_truth_wraps(s->c, x)) {
        TL *tb = NULL, *tn = NULL;
        sq_verify(s, x, &tb, &tn, NO_NODE);
        sq_merge(s, &tn, tb, false);
        sq_add(s, pno, tn, NO_NODE, false);
    } else {
        sq_verify(s, x, pbefore, pno, NO_NODE);
    }
}

/* verify_tree's default case: the operands of one expression, unordered. */
static void sq_ops(SeqCtx *s, const uint32_t *ops, uint32_t n, TL **pno)
{
    for (uint32_t k = 0; k < n; k++) {
        TL *tb = NULL, *tn = NULL;
        uint32_t o = sq_strip(s->c, ops[k]);
        sq_verify(s, ops[k], &tb, &tn, NO_NODE);
        /* fold hoists a comma operand's left side out: (A, B) + C => A, (B + C) */
        if (!(o != NO_NODE && ntag(s->c, o) == N_BINARY &&
              npunct(s->c, o) == P_COMMA))
            sq_merge(s, &tn, tb, false);
        sq_add(s, pno, tn, NO_NODE, false);
    }
}

static bool sq_saved(SeqCtx *s, uint32_t x)
{
    for (size_t k = 0; k < s->nsaved; k++)
        if (s->saved[k] == x)
            return true;
    return false;
}

static void sq_mark_saved(SeqCtx *s, uint32_t x)
{
    if (s->nsaved == s->capsaved) {
        s->capsaved = s->capsaved ? s->capsaved * 2 : 16;
        s->saved = realloc(s->saved, s->capsaved * sizeof *s->saved);
    }
    s->saved[s->nsaved++] = x;
}

/* Whether a subtree has side effects (TREE_SIDE_EFFECTS). */
static bool sq_side(Checker *c, uint32_t x)
{
    uint32_t k, f = cfirst(c, x);
    for (k = f; k <= x; k++) {
        unsigned t = ntag(c, k);
        if (t == N_CALL || t == N_ASSIGN || t == N_POSTFIX || t == N_STMT_EXPR ||
            (t == N_UNARY && (npunct(c, k) == P_INC || npunct(c, k) == P_DEC)))
            return true;
    }
    return false;
}

/* stabilize_reference: the side-effecting leaves of an lvalue become
 * SAVE_EXPRs, evaluated once although the tree mentions them twice. */
static void sq_stab1(SeqCtx *s, uint32_t x)
{
    Checker *c = s->c;
    uint32_t k[3], n;
    x = sq_strip(c, x);
    if (x == NO_NODE || !sq_side(c, x))
        return;
    switch (ntag(c, x)) {
    case N_BINARY:
        if (npunct(c, x) == P_COMMA || npunct(c, x) == P_ANDAND ||
            npunct(c, x) == P_OROR)
            break;
        n = nkids(c, x, k, 3);
        for (uint32_t j = 0; j < n; j++)
            sq_stab1(s, k[j]);
        return;
    case N_CAST:
        n = nkids(c, x, k, 3);
        if (n >= 2)
            sq_stab1(s, k[1]);
        return;
    case N_UNARY:
        if (npunct(c, x) != P_INC && npunct(c, x) != P_DEC &&
            npunct(c, x) != P_STAR && npunct(c, x) != P_AMP) {
            n = nkids(c, x, k, 3);
            if (n)
                sq_stab1(s, k[0]);
            return;
        }
        break;
    default:
        break;
    }
    sq_mark_saved(s, x);
}

static void sq_stab(SeqCtx *s, uint32_t x)
{
    Checker *c = s->c;
    uint32_t k[3], n;
    x = sq_strip(c, x);
    if (x == NO_NODE)
        return;
    n = nkids(c, x, k, 3);
    switch (ntag(c, x)) {
    case N_UNARY:
        if (npunct(c, x) == P_STAR && n)
            sq_stab1(s, k[0]);
        break;
    case N_INDEX:
        if (n >= 2) {
            if (is_array(c, c->ty[strip_paren(c, k[0])])) {
                sq_stab(s, k[0]);
                sq_stab1(s, k[1]);
            } else {
                sq_stab1(s, k[0]);
                sq_stab1(s, k[1]);
            }
        }
        break;
    case N_MEMBER_EXPR:
        if (n) {
            if (c->nodes[x].flags & NF_ARROW)
                sq_stab1(s, k[0]);
            else
                sq_stab(s, k[0]);
        }
        break;
    default:
        break;
    }
}

static void sq_verify(SeqCtx *s, uint32_t x, TL **pbefore, TL **pno,
                      uint32_t writer)
{
    Checker *c = s->c;
    TL *tmp_before, *tmp_nosp, *tmp_list2, *tmp_list3;
    uint32_t k[64], n;
restart:
    x = sq_strip(c, x);
    if (x == NO_NODE || node_err(c, x))
        return;
    if (sq_saved(s, x) && s->skip != x) {
        SaveEnt *t;
        for (t = s->saves; t; t = t->next)
            if (t->node == x)
                break;
        if (!t) {
            t = malloc(sizeof *t);
            t->next = s->saves;
            s->saves = t;
            t->node = x;
            tmp_before = tmp_nosp = tmp_list3 = NULL;
            s->skip = x;
            sq_verify(s, x, &tmp_before, &tmp_nosp, NO_NODE);
            sq_collide(s, tmp_nosp);
            sq_merge(s, &tmp_list3, tmp_nosp, false);
            t->before = tmp_before;
            t->after = tmp_list3;
        }
        sq_merge(s, pbefore, t->before, true);
        sq_add(s, pno, t->after, NO_NODE, true);
        return;
    }
    s->skip = NO_NODE;
    if (sq_cand(c, x))
        *pno = sq_new(s, *pno, x, writer);
    n = nkids(c, x, k, 64);
    switch (ntag(c, x)) {
    case N_SIZEOF_EXPR: case N_SIZEOF_TYPE: case N_ALIGNOF_EXPR:
    case N_ALIGNOF_TYPE: case N_COMPOUND_LIT: case N_STMT_EXPR: case N_GENERIC:
        return;
    case N_BINARY:
        if (n >= 2 && npunct(c, x) == P_COMMA && !sq_side(c, k[0]) &&
            !(c->ef[k[0]] & EF_SIDE)) {
            /* fold drops a side-effect-free left operand and gives the right
             * one the comma's location */
            uint32_t r = sq_strip(c, k[1]);
            sq_setloc(s, r, cnode_loc(c, x));
            x = r;
            goto restart;
        }
        if (n >= 2 && (npunct(c, x) == P_ANDAND || npunct(c, x) == P_OROR)) {
            /* fold: a constant left operand picks the result; a constant
             * deciding right operand swallows a side-effect-free left one */
            bool orr = npunct(c, x) == P_OROR;
            uint32_t l = sq_strip(c, k[0]), r = sq_strip(c, k[1]);
            if (c->ck[l] == K_ICE) {
                if ((c->cv[l] != 0) == orr)
                    return;
                x = r;
                goto restart;
            }
            if (c->ck[r] == K_ICE && (c->cv[r] != 0) == orr &&
                !sq_side(c, l) && !(c->ef[l] & EF_SIDE))
                return;
        }
        if (n >= 2 && (npunct(c, x) == P_COMMA || npunct(c, x) == P_ANDAND ||
                       npunct(c, x) == P_OROR)) {
            tmp_before = tmp_nosp = tmp_list2 = tmp_list3 = NULL;
            bool tr = npunct(c, x) != P_COMMA;
            if (tr)
                sq_verify_t(s, k[0], &tmp_before, &tmp_nosp);
            else
                sq_verify(s, k[0], &tmp_before, &tmp_nosp, NO_NODE);
            sq_collide(s, tmp_nosp);
            sq_merge(s, pbefore, tmp_before, false);
            sq_merge(s, pbefore, tmp_nosp, false);
            if (tr)
                sq_verify_t(s, k[1], &tmp_list3, &tmp_list2);
            else
                sq_verify(s, k[1], &tmp_list3, &tmp_list2, NO_NODE);
            sq_collide(s, tmp_list2);
            sq_merge(s, pbefore, tmp_list3, false);
            sq_merge(s, pno, tmp_list2, false);
            return;
        }
        if (n == 2 && sq_eq(c, sq_strip(c, k[0]), sq_strip(c, k[1])) &&
            !sq_side(c, k[0]) && !(c->ef[k[0]] & EF_SIDE) &&
            is_int(c, c->ty[k[0]])) {
            switch (npunct(c, x)) {     /* fold: x op x is a constant */
            case P_MINUS: case P_CARET: case P_LT: case P_GT: case P_LE:
            case P_GE: case P_EQEQ: case P_NE: case P_SLASH: case P_PERCENT:
                return;
            default:
                break;
            }
        }
        sq_ops(s, k, n, pno);
        return;
    case N_COND: {
        uint32_t cond = k[0], a1 = n == 3 ? k[1] : k[0], a2 = k[n - 1];
        if (n < 2)
            return;
        if (c->ck[sq_strip(c, cond)] == K_ICE) {    /* folded to one arm */
            uint32_t arm = c->cv[sq_strip(c, cond)] ? a1 : a2;
            x = arm;
            goto restart;
        }
        tmp_before = tmp_list2 = NULL;
        sq_verify_t(s, cond, &tmp_before, &tmp_list2);
        sq_collide(s, tmp_list2);
        sq_merge(s, pbefore, tmp_before, false);
        sq_merge(s, pbefore, tmp_list2, false);

        tmp_list3 = tmp_nosp = NULL;
        sq_verify(s, a1, &tmp_list3, &tmp_nosp, NO_NODE);
        sq_collide(s, tmp_nosp);
        sq_merge(s, pbefore, tmp_list3, false);

        tmp_list3 = tmp_list2 = NULL;
        sq_verify(s, a2, &tmp_list3, &tmp_list2, NO_NODE);
        sq_collide(s, tmp_list2);
        sq_merge(s, pbefore, tmp_list3, false);
        sq_merge(s, &tmp_nosp, tmp_list2, false);
        sq_add(s, pno, tmp_nosp, NO_NODE, false);
        return;
    }
    case N_POSTFIX:
        if (n >= 1)
            sq_verify(s, k[0], pno, pno, x);
        return;
    case N_UNARY:
        switch (npunct(c, x)) {
        case P_INC: case P_DEC:
            if (n >= 1) {
                sq_stab(s, k[0]);
                sq_verify(s, k[0], pno, pno, x);
            }
            return;
        case P_AMP:
            if (n >= 1) {
                uint32_t o = sq_strip(c, k[0]);
                if (o != NO_NODE && ntag(c, o) == N_IDENT)
                    return;
                x = o;
                writer = NO_NODE;
                goto restart;
            }
            return;
        case P_STAR:
            sq_ops(s, k, n, pno);
            return;
        default:
            if (n >= 1) {
                x = k[0];
                writer = NO_NODE;
                goto restart;
            }
            return;
        }
    case N_CAST:
        if (n >= 2) {
            x = k[1];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_VA_ARG:
        if (n >= 1) {
            x = k[0];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_MEMBER_EXPR:
        if (n >= 1) {
            x = k[0];
            writer = NO_NODE;
            goto restart;
        }
        return;
    case N_INDEX:
        sq_ops(s, k, n < 2 ? n : 2, pno);
        return;
    case N_ASSIGN: {
        int op = npunct(c, x);
        if (n < 2)
            return;
        tmp_before = tmp_nosp = tmp_list3 = NULL;
        if (op != P_ASSIGN)
            sq_stab(s, k[0]);
        if (op == P_ASSIGN) {
            sq_verify(s, k[1], &tmp_before, &tmp_nosp, NO_NODE);
        } else {
            /* lhs = lhs op rhs: the right side is a binary expression */
            uint32_t two[2];
            two[0] = k[0];
            two[1] = k[1];
            sq_ops(s, two, 2, &tmp_nosp);
        }
        sq_verify(s, k[0], &tmp_list3, &tmp_list3, x);
        sq_add(s, &tmp_before, tmp_list3, x, true);
        sq_collide(s, tmp_before);
        sq_add(s, pno, tmp_list3, x, false);
        sq_collide1(s, k[0], x, tmp_nosp, true);

        sq_merge(s, pbefore, tmp_before, false);
        if (sq_cand(c, sq_strip(c, k[0])))
            sq_merge(s, &tmp_nosp, sq_new(s, NULL, sq_strip(c, k[0]), x), false);
        sq_add(s, pno, tmp_nosp, NO_NODE, true);
        return;
    }
    case N_CALL: {
        uint32_t j;
        tmp_before = tmp_nosp = NULL;
        if (n < 1)
            return;
        sq_verify(s, k[0], &tmp_before, &tmp_nosp, NO_NODE);
        for (j = 1; j < n; j++) {
            tmp_list2 = tmp_list3 = NULL;
            sq_verify(s, k[j], &tmp_list2, &tmp_list3, NO_NODE);
            sq_merge(s, &tmp_list3, tmp_list2, false);
            sq_add(s, &tmp_before, tmp_list3, NO_NODE, false);
        }
        sq_add(s, &tmp_before, tmp_nosp, NO_NODE, false);
        sq_collide(s, tmp_before);
        sq_add(s, pbefore, tmp_before, NO_NODE, false);
        return;
    }
    default:
        return;
    }
}

/* A full expression: gcc's verify_sequence_points. */
static bool sq_for_cond(Checker *c, uint32_t p, uint32_t i)
{
    uint32_t kids[32], cnt = node_children(c->nodes, p, kids, 32);
    return cnt > 2 && kids[2] == i;
}

static bool sq_truth_wraps(Checker *c, uint32_t e)
{
    e = sq_strip(c, e);
    switch (ntag(c, e)) {
    case N_COND:
        return false;
    case N_UNARY:
        return npunct(c, e) != P_BANG;
    case N_BINARY:
        switch (npunct(c, e)) {
        case P_EQEQ: case P_NE: case P_LT: case P_GT: case P_LE: case P_GE:
        case P_ANDAND: case P_OROR: case P_COMMA:
            return false;
        case P_MINUS: case P_CARET:     /* fold turns `a - b != 0` into `a != b` */
            return !is_int(c, c->ty[e]);
        default:
            return true;
        }
    default:
        return true;
    }
}

static void sq_check(Checker *c, uint32_t e, bool cond)
{
    SeqCtx s;
    TL *before = NULL, *after = NULL;
    if (!diag_enabled(c->diag, "sequence-point") || node_err(c, e) ||
        e - cfirst(c, e) > 400)
        return;
    memset(&s, 0, sizeof s);
    s.c = c;
    s.skip = NO_NODE;
    if (cond && sq_truth_wraps(c, e)) {
        /* gcc checks `e != 0`: one more level of operand merging */
        sq_ops(&s, &e, 1, &after);
    } else {
        sq_verify(&s, e, &before, &after, NO_NODE);
    }
    sq_collide(&s, after);
    for (size_t k = 0; k < s.npool; k++)
        free(s.pool[k]);
    free(s.pool);
    free(s.saved);
    free(s.lnode);
    free(s.lloc);
    while (s.saves) {
        SaveEnt *nx = s.saves->next;
        free(s.saves);
        s.saves = nx;
    }
}

void cexpr_node(Checker *c, uint32_t i)
{
    uint32_t p;
    switch (ntag(c, i)) {
    case N_IDENT: e_ident(c, i); break;
    case N_NUMBER: e_number(c, i); break;
    case N_CHAR: e_char(c, i); break;
    case N_STRING: e_string(c, i); break;
    case N_PAREN: e_paren(c, i); break;
    case N_CALL: e_call(c, i); break;
    case N_INDEX: e_index(c, i); break;
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
        is_int(c, c->ty[i]) &&
        !(ntag(c, strip_paren(c, i)) == N_BINARY &&
          npunct(c, strip_paren(c, i)) == P_COMMA))
        c->ef[i] |= EF_NPC;
    p = c->par[i];
    if (c->fold_pending && (p == NO_NODE || !cexpr_is_expr(ntag(c, p))))
        fold_flush(c, i);
}
