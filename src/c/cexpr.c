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
} BTab;

static const BTab *bt_find(const char *name)
{
    size_t lo = 0, hi = sizeof cbuiltin_tab / sizeof *cbuiltin_tab;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = strcmp(name, cbuiltin_tab[mid].name);
        if (!r)
            return (const BTab *)&cbuiltin_tab[mid];
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
        /* libcpp's pedwarns carry no option tag */
        if (c->opt.pedantic && !cexpr_in_extension(c, i))
            cpedwarn(c, loc, "", "%s", l->msg);
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
    if (d)
        cnote(c, d, loc, "include '%s' or provide a declaration of '%s'",
              bt->hdr, bt->name);
}

/* An implicit function declaration (C90; a C99 pedwarn). */
static void implicit_decl(Checker *c, uint32_t i, uint32_t id)
{
    const char *name = cident(c, id);
    SrcLoc loc = cnode_loc(c, i);
    uint32_t ref = id < c->nidents && c->ext[id] ? c->ext[id] - 1 : SYM_NONE;
    const BTab *bt = bt_find(name);
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
        if (d)
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

static bool is_npc(Checker *c, uint32_t n);
static bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out);
static bool rvalue_ok(Checker *c, uint32_t i);

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
        if (ntag(c, q) != N_PARAM)
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
    if (val && !val_ped)
        cpedantic(c, x->loc, "invalid use of pointers to arrays with "
                  "different qualifiers in ISO C before C2X");
    return val;
}

/* The real value of f as gcc prints a REAL_CST ('1.0e+10'). */
static const char *real_str(char *out, long double f)
{
    char t[160], *e, *p;
    int ex;
    snprintf(t, sizeof t, "%.59Le", f);
    e = strchr(t, 'e');
    if (!e) {
        snprintf(out, 160, "%s", t);
        return out;
    }
    ex = atoi(e + 1);
    *e = 0;
    p = t + strlen(t);
    while (p > t && p[-1] == '0')
        *--p = 0;
    if (p > t && p[-1] == '.')
        *p++ = '0', *p = 0;
    snprintf(out, 160, "%se%+d", t, ex);
    return out;
}

/* convert_and_check of arithmetic types: -Woverflow for a constant. */
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
    TypeId pt = x->type, at = x->rhstype, pr = at;
    TypeKind pk = tkind(c, pt), ak = tkind(c, at);
    int pc, ac;
    Diagnostic *d;
    if (is_int(c, at)) {
        pr = type_int_promote(TT, at);
        if (tkind(c, pr) == TY_ENUM)
            pr = TYPE_B(UINT);
        pr = TYPE_UNQUAL(type_canon(TT, pr));
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
                TYPE_UNQUAL(type_canon(TT, pr)) == TYPE_UNQUAL(type_canon(TT, at))
                ? "'%s' argument %d type is %s where %s is expected in a call "
                  "to built-in function declared without prototype"
                : "'%s' argument %d promotes to %s where %s is expected in a "
                  "call to built-in function declared without prototype",
                x->ci->fname, x->ci->parmnum, type_q(TT, pr), type_q(TT, pt));
    if (d)
        cnote(c, d, builtin_loc(c), "built-in '%s' declared here",
              x->ci->fname);
}

static void conv_arith(Conv *x)
{
    Checker *c = x->c;
    uint32_t e = x->expr;
    TypeId lt = x->type, rt = x->rhstype;
    SrcLoc l = x->eloc ? x->eloc : x->loc;
    if (!is_int(c, lt) || tkind(c, lt) == TY_BOOL || inhibited(c, e, false) ||
        (c->ef[e] & EF_OVERFLOW))
        return;
    if (has_ival(c, e) && int_bits(c, rt) <= 64 && int_bits(c, lt) <= 64) {
        uint64_t v = c->cv[e], r = cexpr_trunc(c, lt, v);
        TypeId us, sg, dummy;
        bool warn = false;
        if (cexpr_fits(c, v, rt, lt))
            return;
        sign_map(c, lt, &us, &dummy);
        sign_map(c, lt, &dummy, &sg);
        if (!is_signed(c, lt)) {
            if (!cexpr_fits(c, v, rt, sg)) {
                cwarn(c, l, "overflow", "unsigned conversion from %s to %s "
                      "changes value from '%s' to '%s'", type_q(TT, rt),
                      type_q(TT, lt), vstr(c, rt, v), vstr(c, lt, r));
            }
            return;
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
        return;
    }
    if (c->ck[e] == K_FLOAT && is_flt(c, rt) && int_bits(c, lt) <= 64) {
        long double f = c->fv.data[c->cv[e]];
        unsigned bits = int_bits(c, lt);
        uint64_t r;
        bool ovf;
        char rb[160];
        if (f != f)
            return;
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
            return;
        cwarn(c, l, "overflow", "overflow in conversion from %s to %s changes "
              "value from '%s' to '%s'", type_q(TT, rt), type_q(TT, lt),
              real_str(rb, f), vstr(c, lt, r));
    }
}

/* The declared enum type of an enumerator, else the value's type (gcc's
 * original type of the expression). */
static TypeId orig_type(Checker *c, uint32_t e)
{
    uint32_t s = strip_paren(c, e);
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

    if (diag_enabled(c->diag, "enum-conversion")) {
        TypeId ot = orig_type(c, expr);
        if (tkind(c, ot) == TY_ENUM && kl == TY_ENUM &&
            mainv(c, ot) != mainv(c, lt))
            cwarn(c, x.loc, "enum-conversion", "implicit conversion from %s "
                  "to %s", type_q(TT, ot), type_q(TT, lt));
    }
    if (mainv(c, lt) == mainv(c, rt))
        return true;
    if (kr == TY_VOID) {
        if (ci->warnopt)
            cwarn(c, x.loc, ci->warnopt, "void value not ignored as it ought "
                  "to be");
        else
            cerror(c, x.loc, "void value not ignored as it ought to be");
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
                if (is_array(c, ttl))
                    warn_ped = gq(c, ttr) & qnoat & ~gq(c, ttl);
                if (warn_q || (warn_ped && c->opt.pedantic)) {
                    const char *qs = qual_str(q, gq(c, ttr) & ~gq(c, ttl));
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
    if (!proto && fref != SYM_NONE && (csym(c, fref)->flags & CSF_IMPLICIT)) {
        const BTab *bt = bt_find(cident(c, csym(c, fref)->name));
        if (bt && !bt->mismatch && strcmp(strchr(bt->sig, '|') + 1, "?")) {
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
            cerror(c, expr_loc(c, a), "void value not ignored as it ought to "
                   "be");
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
    if (!call_args(c, i, k[0], t)) {
        set_err(c, i);
        return;
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
static bool binop_operand(Checker *c, uint32_t a)
{
    if (is_void(c, rvt(c, a))) {
        cerror(c, expr_loc(c, a), "void value not ignored as it ought to be");
        return false;
    }
    return rvalue_ok(c, a);
}

static bool truth_ok_at(Checker *c, uint32_t a, SrcLoc loc)
{
    TypeId t = rvt(c, a);
    if (is_record(c, t)) {
        cerror(c, loc, "used %s type value where scalar is "
               "required", tkind(c, t) == TY_UNION ? "union" : "struct");
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
    ok = binop_operand(c, cond) && truth_ok_at(c, cond, cnode_loc(c, i));
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
    {
        ConvInfo ci;
        TypeId lt = unqual(c, c->ty[l]);
        memset(&ci, 0, sizeof ci);
        ci.context = CONV_ASSIGN;
        ci.loc = loc;
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
