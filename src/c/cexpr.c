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
        /* same precision: prefer _FloatN, then long double, double */
        if (ka >= TY_FLOAT32 && ka <= TY_FLOAT64X)
            return a;
        if (kb >= TY_FLOAT32 && kb <= TY_FLOAT64X)
            return b;
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

#define SC_MAXLEN 256
#define SC_NONE 0xFFFFFFFFu

/* Damerau-Levenshtein distance with gcc's costs: 2 per edit, 1 for a
 * change of case only. */
static unsigned sc_dist(const char *s, size_t n, const char *t, size_t m)
{
    unsigned rows[3][SC_MAXLEN + 1];
    unsigned *pp = rows[0], *pr = rows[1], *cur = rows[2];
    size_t i, j;
    if (n > SC_MAXLEN || m > SC_MAXLEN)
        return SC_NONE;
    for (j = 0; j <= m; j++)
        pr[j] = (unsigned)j * 2;
    for (i = 1; i <= n; i++) {
        unsigned *tmp;
        cur[0] = (unsigned)i * 2;
        for (j = 1; j <= m; j++) {
            unsigned sub, del = pr[j] + 2, ins = cur[j - 1] + 2, best;
            char a = s[i - 1], b = t[j - 1];
            if (a == b)
                sub = pr[j - 1];
            else if (a >= 0 && b >= 0 && (a | 0x20) == (b | 0x20) &&
                     ((a >= 'a' && a <= 'z') || (a >= 'A' && a <= 'Z')))
                sub = pr[j - 1] + 1;
            else
                sub = pr[j - 1] + 2;
            best = sub < del ? sub : del;
            if (ins < best)
                best = ins;
            if (i > 1 && j > 1 && s[i - 1] == t[j - 2] && s[i - 2] == t[j - 1] &&
                pp[j - 2] + 2 < best)
                best = pp[j - 2] + 2;
            cur[j] = best;
        }
        tmp = pp;
        pp = pr;
        pr = cur;
        cur = tmp;
    }
    return pr[m];
}

static unsigned sc_cutoff(size_t goal, size_t cand)
{
    size_t mx = goal > cand ? goal : cand, mn = goal > cand ? cand : goal;
    if (mx <= 1)
        return 0;
    if (mx - mn <= 1)
        return 2 * (unsigned)(mx / 3 > 1 ? mx / 3 : 1);
    return 2 * (unsigned)((mx + 2) / 3);
}

typedef struct Best {
    const char *goal;
    size_t goal_len;
    const char *str;
    size_t len;
    unsigned dist;
} Best;

static void best_init(Best *b, const char *goal)
{
    b->goal = goal;
    b->goal_len = strlen(goal);
    b->str = NULL;
    b->len = 0;
    b->dist = SC_NONE;
}

static void best_consider(Best *b, const char *s)
{
    size_t n = strlen(s);
    unsigned mind = (unsigned)(n > b->goal_len ? n - b->goal_len
                                               : b->goal_len - n) * 2, d;
    if (mind >= b->dist || mind > sc_cutoff(b->goal_len, n))
        return;
    d = sc_dist(b->goal, b->goal_len, s, n);
    if (d < b->dist) {
        b->dist = d;
        b->str = s;
        b->len = n;
    }
}

static const char *best_get(Best *b)
{
    if (!b->str || b->dist == 0 || b->dist > sc_cutoff(b->goal_len, b->len))
        return NULL;
    return b->str;
}

/* Names reserved to the implementation: not suggested unless the goal is
 * one too. */
static bool reserved_name(const char *s)
{
    return s[0] == '_' && (s[1] == '_' || (s[1] >= 'A' && s[1] <= 'Z'));
}

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

static void header_note(Checker *c, Diagnostic *d, SrcLoc loc,
                        const char *name, const char *hdr)
{
    cnote(c, d, include_loc(c, loc),
          "'%s' is defined in header '%s'; did you forget to '#include %s'?",
          name, hdr, hdr);
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
    best_init(&b, goal);
    for (k = c->log.len; k-- > 0;) {
        const Bind *bd = &c->log.data[k];
        const char *s;
        if (c->top[bd->ns][bd->ident] == 0)
            continue;
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
        best_consider(&b, s);
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

static void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec);

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

static void pexpr(Checker *c, StrBuf *sb, uint32_t i, int prec)
{
    uint32_t k[3], n;
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
        my = PR_POSTFIX;
        break;
    default: break;
    }
    if (my < prec)
        sb_putc(sb, '(');
    switch (ntag(c, i)) {
    case N_IDENT: case N_NUMBER: case N_CHAR:
        s = ttext(c, c->nodes[i].tok, &len);
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
        pexpr(c, sb, k[0], PR_LOR);
        sb_puts(sb, " ? ");
        if (n == 3)
            pexpr(c, sb, k[1], PR_COMMA);
        sb_puts(sb, " : ");
        pexpr(c, sb, k[n - 1], PR_COND);
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

/* %E of node i, in a buffer valid until the next call (two rotate). */
static const char *estr(Checker *c, uint32_t i)
{
    StrBuf *sb = &c->esb[c->enext++ & 1];
    sb->len = 0;
    pexpr(c, sb, i, PR_COMMA);
    return sb_cstr(sb);
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
        if (l->flags & LIT_TOO_LARGE)
            cpedwarn(c, loc, l->id, "%s", l->msg);
        else
            cwarn(c, loc, l->id, "%s", l->msg);
        break;
    default:
        ped(c, i, loc, "%s", l->msg);
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
        if (p == '8')
            p = 0;
        if (p && prefix && p != prefix) {
            cerror(c, ctok_loc(c, c->nodes[i].tok + k),
                   "unsupported non-standard concatenation of string literals");
            set_err(c, i);
            return;
        }
        if (p)
            prefix = p;
    }
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

/* An implicit function declaration (C90; a C99 pedwarn). */
static void implicit_decl(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    uint32_t ref = id < c->nidents && c->ext[id] ? c->ext[id] - 1 : SYM_NONE;
    CSym s;
    Diagnostic *d;
    if (ref != SYM_NONE && csym(c, ref)->kind == CS_FUNC) {
        CSym *o = csym(c, ref);
        if (!(o->flags & CSF_IMPLICIT)) {
            d = cpedwarn(c, loc, "implicit-function-declaration",
                         "implicit declaration of function '%s'", name);
            old_decl_note(c, d, o);
            o->flags |= CSF_IMPLICIT;
        }
        cbind(c, NS_ORD, id, ref);
        o = csym(c, ref);
        c->ty[i] = o->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        return;
    }
    {
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
    s.ty = type_func(TT, TYPE_B(INT), NULL, 0, TF_NOPROTO);
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
            else
                cwarn(c, cnode_loc(c, i), "",
                      "'%s' is not defined outside of function scope", name);
            c->ty[i] = type_array(TT, type_qual(TYPE_B(CHAR), TQ_CONST), n + 1);
            c->ef[i] = EF_LVALUE | EF_ADDRLV;
            c->cb[i] = CB_NODE | i;
            c->ck[i] = K_ADDR;
            return;
        }
        if (builtin_name(name) || (p != NO_NODE && ntag(c, p) == N_ATTR_ITEM)) {
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
    s->flags |= CSF_USED;
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
        c->ty[i] = s->ty;
        c->ck[i] = K_ADDR;
        c->cb[i] = ref + 1;
        c->ef[i] = EF_ADDRLV;
        return;
    default:
        break;
    }
    c->ty[i] = s->ty;
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
        Diagnostic *d = cerror_d(c, first_loc(c, k[0]),
                                 "called object '%s' is not a function or "
                                 "function pointer", estr(c, k[0]));
        if (ref != SYM_NONE)
            cnote(c, d, csym(c, ref)->loc, "declared here");
        set_err(c, i);
        return;
    }
    /* P2b: arguments against the prototype */
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
        best_init(&b, cident(c, name));
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
static bool rvalue_ok(Checker *c, uint32_t i)
{
    TypeId t = rvt(c, i);
    if (is_void(c, t) || complete(c, t))
        return true;
    incomplete_error(c, first_loc(c, i), i, t);
    return false;
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
    if (tkind(c, t) == TY_BOOL)
        cwarn(c, loc, "bool-operation", inc ? "increment of a boolean "
              "expression" : "decrement of a boolean expression");
    if (is_complex(c, t))
        ped(c, i, loc, "ISO C does not support '++' and '--' on complex types");
    else if (!is_ptr(c, t) && !is_int(c, t) && !is_flt(c, t)) {
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
    /* P2b: a read-only operand */
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
        } else if (c->ck[a] == K_ADDR && is_ptr(c, from)) {
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
        if (len == 8 && !memcmp(s, "_Alignof", 8))
            cpedantic(c, cnode_loc(c, i), "ISO C99 does not support '_Alignof'");
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
                if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ &&
                    csym(c, ref)->align > v)
                    v = csym(c, ref)->align;
            }
        }
    }
    set_ice(c, i, size_type(c), v);
}

/* ---- casts ----------------------------------------------------------------------- */

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
    if (tk == TY_VECTOR || tkind(c, ot) == TY_VECTOR)
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
        cerror(c, after_loc(c, i), "conversion to non-scalar type requested");
        set_err(c, i);
        return;
    }
    if (is_record(c, ot)) {
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
        set_err(c, i);
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
    } else if (is_ptr(c, t) && is_int(c, ot) && tkind(c, ot) != TY_BOOL &&
               tkind(c, ot) != TY_ENUM && int_bits(c, t) != int_bits(c, ot) &&
               c->ck[a] != K_ICE && c->ck[a] != K_FOLD) {
        cwarn(c, loc, "int-to-pointer-cast",
              "cast to pointer from integer of different size");
    }
    conv_const(c, i, a, c->ty[i]);
    if (is_ptr(c, t) && c->ck[i] == K_ADDR && c->cb[i] == 0 &&
        c->cv[i] == 0 && is_intcst(c, a) && !(c->ef[a] & EF_OVERFLOW) &&
        is_void(c, pointee(c, t)) && tquals(c, pointee(c, t)) == 0)
        c->ef[i] |= EF_NPC;
}

/* ---- compound literals, statement expressions --------------------------------------- */

/* The number of elements the initializer list `list` gives an array. */
static uint64_t init_count(Checker *c, uint32_t list)
{
    uint32_t k[256], n = nkids(c, list, k, 256), j;
    uint64_t next = 0, max = 0;
    if (n == 1 && ntag(c, k[0]) != N_DESIGNATED &&
        is_array(c, c->ty[k[0]]) && is_int(c, elem_of(c, c->ty[k[0]]))) {
        bool ok;
        (void)type_size(TT, c->ty[k[0]], &ok);
        if (ok && c->ck[k[0]] == K_ADDR && (c->ef[k[0]] & EF_STRING))
            return type_ent(TT, type_canon(TT, c->ty[k[0]]))->n;
    }
    for (j = 0; j < n; j++) {
        if (ntag(c, k[j]) == N_DESIGNATED) {
            uint32_t d[8], nd = nkids(c, k[j], d, 8);
            if (nd >= 2 && ntag(c, d[0]) == N_DESIG_INDEX) {
                uint32_t e = first_child(c, d[0]);
                if (e != NO_NODE && has_ival(c, e))
                    next = c->cv[e];
            } else if (nd >= 2 && ntag(c, d[0]) == N_DESIG_RANGE) {
                uint32_t r[2];
                if (nkids(c, d[0], r, 2) == 2 && has_ival(c, r[1]))
                    next = c->cv[r[1]];
            }
        }
        next++;
        if (next > max)
            max = next;
    }
    return max;
}

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
        uint64_t n = init_count(c, k[1]);
        t = type_array(TT, elem_of(c, t), n);
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
    ped(c, i, cnode_loc(c, i), "ISO C99 does not support '_Generic'");
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
    ped(c, i, ctok_loc(c, tok > 0 ? tok - 1 : tok), "taking the address of a "
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
    cerror(c, cnode_loc(c, i), "invalid operands to binary %s (have %s and %s)",
           sp, type_q(TT, ta), type_q(TT, tb));
    set_err(c, i);
}

/* An operand of a binary operator used for its value: false (after an
 * error) for void and incomplete ones. */
static bool binop_operand(Checker *c, uint32_t a)
{
    if (is_void(c, rvt(c, a))) {
        cerror(c, expr_loc(c, a), "void value not ignored as it ought to be");
        return false;
    }
    return rvalue_ok(c, a);
}

static bool truth_ok(Checker *c, uint32_t a)
{
    TypeId t = rvt(c, a);
    if (is_record(c, t)) {
        cerror(c, first_loc(c, a), "used %s type value where scalar is "
               "required", tkind(c, t) == TY_UNION ? "union" : "struct");
        return false;
    }
    return true;
}

static void e_comma(Checker *c, uint32_t i, uint32_t a, uint32_t b)
{
    TypeId t;
    if (node_err(c, a) || node_err(c, b)) {
        set_err(c, i);
        return;
    }
    if (!(c->ef[a] & EF_SIDE)) {
        uint32_t s = strip_paren(c, a);
        bool voidcast = ntag(c, s) == N_CAST && is_void(c, c->ty[s]);
        if (!voidcast && !inhibited(c, i, false))
            cwarn(c, cnode_loc(c, i), "unused-value", "left-hand operand of "
                  "comma expression has no effect");
    }
    t = rvt(c, b);
    c->ty[i] = t;
    c->ef[i] = (c->ef[a] | c->ef[b]) & EF_SIDE;
    if (c->ef[a] & EF_SIDE)
        return;
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
    ok = truth_ok(c, a);
    ok = truth_ok(c, b) && ok;
    if (!ok) {
        set_err(c, i);
        return;
    }
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
    if (is_intcst(c, a) && is_intcst(c, b)) {
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

static void e_compare(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool eq = op == P_EQEQ || op == P_NE;
    bool pa = is_ptr(c, ta), pb = is_ptr(c, tb);
    uint32_t sa = strip_paren(c, a), sb = strip_paren(c, b);
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        c->ty[i] = TYPE_B(INT);
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
    if (eq && ((pa && char_zero(c, b)) || (pb && char_zero(c, a)))) {
        uint32_t p = pa ? a : b;
        Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "pointer-compare",
                                "comparison between pointer and zero "
                                "character constant");
        cnote(c, d, first_loc(c, p), "did you mean to dereference the pointer?");
    }
    if (pa && pb) {
        TypeId tta = pointee(c, ta), ttb = pointee(c, tb);
        bool compat = type_compatible(TT, mainv(c, tta), mainv(c, ttb));
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
    if (((c->ef[sa] | c->ef[sb]) & EF_STRING) && !inhibited(c, i, false))
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

static void e_shift(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b);
    bool left = op == P_SHL, int_const = true, cnt_ok = false;
    unsigned prec;
    const char *dir = left ? "left" : "right";
    if (!is_int(c, ta) || !is_int(c, tb)) {
        if (tkind(c, ta) == TY_VECTOR) {
            c->ty[i] = ta;
            return;
        }
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
    if (!type_compatible(TT, mainv(c, pa), mainv(c, pb))) {
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

static void e_arith(Checker *c, uint32_t i, uint32_t a, uint32_t b, int op)
{
    SrcLoc loc = cnode_loc(c, i);
    TypeId ta = promoted(c, a), tb = promoted(c, b), rt;
    bool need_int = op == P_PERCENT || op == P_AMP || op == P_PIPE ||
                    op == P_CARET;
    bool zero_div = false;
    long double fa, fb;
    if (tkind(c, ta) == TY_VECTOR || tkind(c, tb) == TY_VECTOR) {
        c->ty[i] = tkind(c, ta) == TY_VECTOR ? ta : tb;
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
    ok = binop_operand(c, k[0]);
    ok = binop_operand(c, k[1]) && ok;
    if (!ok) {
        set_err(c, i);
        return;
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
    ok = binop_operand(c, cond) && truth_ok(c, cond);
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
    c->ef[i] = (c->ef[cond] | c->ef[els] | (mid != NO_NODE ? c->ef[mid] : 0)) &
               EF_SIDE;
    if (mainv(c, t1) == mainv(c, t2)) {
        rt = unqual(c, t1);
    } else if (is_arith(c, t1) && is_arith(c, t2)) {
        rt = common_type(c, promoted(c, ch), promoted(c, els));
    } else if (is_void(c, t1) || is_void(c, t2)) {
        ped(c, i, cl, "ISO C forbids conditional expr with only one void "
                      "side");
        rt = TYPE_B(VOID);
    } else if (is_ptr(c, t1) && is_ptr(c, t2)) {
        TypeId p1 = pointee(c, t1), p2 = pointee(c, t2);
        unsigned q = tquals(c, p1) | tquals(c, p2);
        if (type_compatible(TT, mainv(c, p1), mainv(c, p2))) {
            rt = type_ptr(TT, type_qual(type_composite(TT, mainv(c, p1),
                                                       mainv(c, p2)), q));
        } else if (is_npc(c, ch)) {
            rt = t2;
        } else if (is_npc(c, els)) {
            rt = t1;
        } else if (is_void(c, p1) || is_void(c, p2)) {
            if (is_func(c, p1) || is_func(c, p2))
                ped(c, i, cl, "ISO C forbids conditional expr between 'void *' "
                              "and function pointer");
            rt = type_ptr(TT, type_qual(TYPE_B(VOID), q));
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

static void e_assign(Checker *c, uint32_t i)
{
    uint32_t k[2], l, r;
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
    if (is_func(c, c->ty[l])) {
        cerror(c, loc, "assignment of function '%s'", estr(c, l));
        set_err(c, i);
        return;
    }
    if (!(c->ef[l] & EF_LVALUE)) {
        cerror(c, loc, "lvalue required as left operand of assignment");
        set_err(c, i);
        return;
    }
    if (is_array(c, c->ty[l])) {
        cerror(c, loc, "assignment to expression with array type");
        set_err(c, i);
        return;
    }
    if (is_void(c, rvt(c, r))) {
        cerror(c, expr_loc(c, r), "void value not ignored as it ought to be");
        set_err(c, i);
        return;
    }
    /* P2b: compatibility of the operand types, read-only lvalues, the
     * checks of the compound assignments */
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
    case N_GENERIC_ASSOC: case N_ADDR_LABEL:
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
    case N_CONVERTVECTOR: e_convertvector(c, i); break;
    case N_GENERIC: e_generic(c, i); break;
    case N_GENERIC_ASSOC: e_generic_assoc(c, i); break;
    case N_ADDR_LABEL: e_addr_label(c, i); break;
    default: set_err(c, i); break;
    }
    /* a null pointer constant: an integer constant expression with value 0 */
    if (is_intcst(c, i) && !(c->ef[i] & EF_OVERFLOW) && c->cv[i] == 0 &&
        is_int(c, c->ty[i]))
        c->ef[i] |= EF_NPC;
    p = c->par[i];
    if (c->fold_pending && (p == NO_NODE || !cexpr_is_expr(ntag(c, p))))
        fold_flush(c, i);
}
