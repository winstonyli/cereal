/* cexpr_int.h - helpers shared by the expression checker's translation
 * units (cexpr.c, cformat.c, ...): node and type accessors, small enough to
 * inline. */
#ifndef CEREAL_C_CEXPR_INT_H
#define CEREAL_C_CEXPR_INT_H

#include "c/check_int.h"
#include "c/fuzzy.h"

#define TT (&c->tt)
#define ERRT TYPE_B(ERROR)

enum {
    PR_COMMA, PR_ASSIGN, PR_COND, PR_LOR, PR_LAND, PR_OR, PR_XOR, PR_AND,
    PR_EQ, PR_REL, PR_SHIFT, PR_ADD, PR_MUL, PR_CAST, PR_UNARY, PR_POSTFIX,
    PR_PRIMARY
};

/* A built-in function (c/builtin_tab.c): header, signature, gnu-ness. */
typedef struct BTab {
    const char *name, *hdr;
    unsigned char mismatch;
    const char *sig;
    unsigned char gnu;       /* 1: a built-in only outside -std=c99; 2, 3: only as __builtin_NAME (3: typed) */
} BTab;

/* convert_for_assignment's working state (cexpr.c, cconv.c). */
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

/* ---- node accessors ------------------------------------------------------ */

static inline unsigned ntag(const Checker *c, uint32_t i)
{
    return c->nodes[i].tag;
}

static inline TypeKind tkind(Checker *c, TypeId t)
{
    return type_ckind(TT, t);
}

static inline bool is_err(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_ERROR;
}

/* The punctuator of token tok, P_NONE if it is not one. */
static inline int tpunct(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return P_NONE;
    t = &c->u->toks[tok].t;
    return t->kind == TK_PUNCT ? t->punct : P_NONE;
}

static inline int npunct(const Checker *c, uint32_t i)
{
    return tpunct(c, c->nodes[i].tok);
}

/* The C keyword token tok spells (CK_NONE if none). */
static inline int tckw(const Checker *c, uint32_t tok)
{
    const Tok *t;
    if (tok >= c->u->ntoks)
        return CK_NONE;
    t = &c->u->toks[tok].t;
    if (t->kind != TK_IDENT)
        return CK_NONE;
    return ident_by_id(c->in, t->aux)->ckw & 0xFF;
}

static inline const char *ttext(const Checker *c, uint32_t tok, size_t *len)
{
    const Tok *t = &c->u->toks[tok].t;
    *len = t->kind == TK_IDENT ? ident_by_id(c->in, t->aux)->len : t->len;
    return tok_text_raw(c->sm, c->in, t);
}

/* Children of i: the first and the last. */
static inline uint32_t first_child(const Checker *c, uint32_t i)
{
    uint32_t k, f = cfirst(c, i);
    if (c->nodes[i].size <= 1)
        return NO_NODE;
    k = i - 1;
    while (cfirst(c, k) > f)
        k = cfirst(c, k) - 1;
    return k;
}

static inline uint32_t nkids(const Checker *c, uint32_t i, uint32_t *out,
                      uint32_t max)
{
    return node_children(c->nodes, i, out, max);
}

/* The last token of i's subtree (approximately: the largest token any of
 * its nodes names). */
static inline uint32_t last_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = 0;
    for (k = cfirst(c, i); k <= i; k++) {
        uint32_t t = c->nodes[k].tok;
        if (c->nodes[k].tag == N_STRING)
            t += node_pieces(c, k) - 1u;
        if (t > m)
            m = t;
    }
    return m;
}

/* The first token of i's subtree. */
static inline uint32_t first_tok(const Checker *c, uint32_t i)
{
    uint32_t k, m = c->nodes[i].tok;
    if (c->nodes[i].tag == N_ADDR_LABEL)
        return m - 1;   /* the && before the label name */
    for (k = cfirst(c, i); k < i; k++)
        if (c->nodes[k].tok < m)
            m = c->nodes[k].tok;
    return m;
}

static inline SrcLoc first_loc(const Checker *c, uint32_t i)
{
    return ctok_loc(c, first_tok(c, i));
}

/* gcc's input_location once the parser is past expression i. */
static inline SrcLoc after_loc(Checker *c, uint32_t i)
{
    return cinput_loc(c, last_tok(c, i) + 1);
}

static inline bool in_function(const Checker *c)
{
    return c->func_sym != SYM_NONE;
}

static inline void set_err(Checker *c, uint32_t i)
{
    c->ty[i] = ERRT;
    c->ck[i] = K_ERR;
}

static inline bool node_err(Checker *c, uint32_t i)
{
    return i == NO_NODE || is_err(c, c->ty[i]) || c->ck[i] == K_ERR;
}

static inline void copy_node(Checker *c, uint32_t to, uint32_t from)
{
    c->ty[to] = c->ty[from];
    c->ck[to] = c->ck[from];
    c->cv[to] = c->cv[from];
    c->cb[to] = c->cb[from];
    c->ef[to] = c->ef[from];
}

/* Skips parentheses. */
static inline uint32_t strip_paren(const Checker *c, uint32_t i)
{
    while (i != NO_NODE && c->nodes[i].tag == N_PAREN)
        i = c->nodes[i].size > 1 ? i - 1 : NO_NODE;
    return i;
}

static inline uint32_t fpush(Checker *c, long double v)
{
    vec_push(&c->fv, v);
    return (uint32_t)c->fv.len - 1;
}

/* ---- types --------------------------------------------------------------- */

static inline bool is_int(Checker *c, TypeId t) { return type_is_integer(TT, t); }

static inline bool is_flt(Checker *c, TypeId t) { return type_is_float(TT, t); }

static inline bool is_arith(Checker *c, TypeId t) { return type_is_arith(TT, t); }

static inline bool is_ptr(Checker *c, TypeId t) { return tkind(c, t) == TY_PTR; }

static inline bool is_scalar(Checker *c, TypeId t) { return type_is_scalar(TT, t); }

static inline bool is_void(Checker *c, TypeId t) { return tkind(c, t) == TY_VOID; }

static inline bool is_func(Checker *c, TypeId t) { return tkind(c, t) == TY_FUNC; }

static inline bool is_record(Checker *c, TypeId t) { return type_is_record(TT, t); }

static inline bool is_array(Checker *c, TypeId t)
{
    TypeKind k = tkind(c, t);
    return k == TY_ARRAY || k == TY_VLA;
}

static inline bool is_complex(Checker *c, TypeId t)
{
    return tkind(c, t) == TY_COMPLEX;
}

static inline bool is_signed(Checker *c, TypeId t)
{
    return type_is_signed(TT, t);
}

/* The qualifiers t has, typedefs looked through. */
static inline unsigned tquals(Checker *c, TypeId t)
{
    return TYPE_QUALS(type_canon(TT, t));
}

/* The pointed-to type of a pointer (qualified). */
static inline TypeId pointee(Checker *c, TypeId t)
{
    return type_base(TT, t);
}

/* t without its top-level qualifiers, the typedef spelling kept when the
 * typedef does not itself add qualifiers. */
static inline TypeId unqual(Checker *c, TypeId t)
{
    t = TYPE_UNQUAL(t);
    if (type_ent(TT, t)->kind == TY_TYPEDEF && TYPE_QUALS(type_canon(TT, t)))
        t = TYPE_UNQUAL(type_canon(TT, t));
    return t;
}

/* gcc's TYPE_MAIN_VARIANT: the canonical type without qualifiers. */
static inline TypeId mainv(Checker *c, TypeId t)
{
    return TYPE_UNQUAL(type_canon(TT, t));
}

static inline unsigned int_bits(Checker *c, TypeId t)
{
    if (is_ptr(c, t))
        return c->tgt->ptr_size * 8u;
    return type_int_bits(TT, t);
}

static inline TypeId size_type(Checker *c)
{
    return TYPE_MK(c->tgt->size_type, 0);
}

TypeId cexpr_rvalue_type(Checker *c, uint32_t i);

static inline TypeId rvt(Checker *c, uint32_t i)
{
    return cexpr_rvalue_type(c, i);
}

/* The type after the integer promotions (bit-fields included). */
static inline TypeId promoted(Checker *c, uint32_t i)
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
static inline int float_prec(Checker *c, TypeKind k)
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

uint32_t builtin_format_pos(const char *name);
uint32_t builtin_scanf_pos(const char *name);
void check_format_literal(Checker *c, const uint32_t *kv, uint32_t nk, const
                          CSym *sy, const char *name, SrcLoc loc);
void check_strlen(Checker *c, const uint32_t *kv, uint32_t nk);

void addr_rvalue(Checker *c, uint32_t i);
void alias_deref(Checker *c, uint32_t p, bool use_loc, SrcLoc loc);
size_t asm_string(Checker *c, uint32_t n, char *out, size_t cap);
const BTab *bt_find(Checker *c, const char *name, bool any);
TypeId bt_func_type(Checker *c, const BTab *b);
SrcLoc call_loc(Checker *c, uint32_t f);
SrcLoc callee_err_loc(Checker *c, uint32_t f);
void check_restrict(Checker *c, const uint32_t *kv, uint32_t nk, uint32_t
                    parms, uint32_t nparm, SrcLoc loc, bool builtin, bool is_bt);
bool complete(Checker *c, TypeId t);
void cplx_set(Checker *c, uint32_t i, long double re, long double im);
void double_promo(Checker *c, uint32_t at, SrcLoc loc, TypeId from, TypeId to,
                  const char *what);
TypeId elem_of(Checker *c, TypeId t);
uint64_t elem_size(Checker *c, TypeId pt);
const char *estr(Checker *c, uint32_t i);
bool extra_on(Checker *c);
const Field *find_field(Checker *c, TypeId rec, uint32_t name, uint64_t *off,
                        unsigned *quals);
void fuzzy_fields(Checker *c, Best *b, TypeId rec);
bool gcc_integer(Checker *c, TypeId t);
bool has_ival(Checker *c, uint32_t i);
SrcLoc header_note_loc(Checker *c, SrcLoc loc, const char *hdr);
void incomplete_error(Checker *c, SrcLoc loc, uint32_t value, TypeId t);
bool inhibited(Checker *c, uint32_t i, bool fold);
bool is_const(Checker *c, uint32_t i);
bool is_decimal_flt(Checker *c, TypeId t);
bool is_intcst(Checker *c, uint32_t i);
bool ival_neg(Checker *c, TypeId t, uint64_t v);
TypeId mvt(Checker *c, TypeId t);
SrcLoc param_loc(Checker *c, uint32_t ref, uint32_t idx, uint32_t at, SrcLoc
                 dflt);
Diagnostic *ped(Checker *c, uint32_t i, SrcLoc loc, const char *fmt, ...);
bool ptr_arith_ok(Checker *c, uint32_t i, SrcLoc loc, TypeId pt);
bool rvalue_ok(Checker *c, uint32_t i);
bool rvalue_ok_at(Checker *c, uint32_t i, SrcLoc loc);
void memset_args(Checker *c, const uint32_t *kv, uint32_t nk, const char *name);
void sizeof_memaccess(Checker *c, const uint32_t *kv, uint32_t nk, const char
                      *name);
TypeId vec_elem(Checker *c, TypeId vt);
unsigned vec_esize(Checker *c, TypeId el);
bool zero_size_ok(const char *name);
void e_call(Checker *c, uint32_t i);
void e_index(Checker *c, uint32_t i);
void e_member(Checker *c, uint32_t i);
bool same_addr_const(Checker *c, uint32_t a, uint32_t b);

unsigned bf_width(Checker *c, uint32_t n);
TypeId cplx_comp(Checker *c, TypeId t);
bool cplx_get(Checker *c, uint32_t n, long double *re, long double *im, TypeId
              *ct);
uint64_t cplx_u(long double v);
bool float_to_int(Checker *c, long double f, TypeId t, uint64_t *out);
bool float_to_int_bits(Checker *c, long double f, TypeId t, unsigned bits,
                       uint64_t *out);
bool is_cmp_op(int op);
bool opeq(Checker *c, uint32_t x, uint32_t y);
bool part_conv(Checker *c, TypeId from, TypeId to, long double v, long double
               *out);
void sign_map(Checker *c, TypeId t, TypeId *uns, TypeId *sgn);
const char *vstr(Checker *c, TypeId t, uint64_t v);
bool const_fits(Checker *c, uint32_t n, TypeId as, TypeId t);
void conv_arith(Conv *x);
void conv_operand(Checker *c, SrcLoc l, TypeId lt, uint32_t a, bool prom);
bool dec_mix(Checker *c, uint32_t i, TypeId ta, TypeId tb);
bool float_ovf(Checker *c, long double f, TypeId lt, uint64_t *r);
bool gcc_real(Checker *c, TypeId t);
TypeId orig_type(Checker *c, uint32_t e);
unsigned uc_prec(Checker *c, TypeId t);
bool vector_truth_node(Checker *c, uint32_t e);

#endif
