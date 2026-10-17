/* pcexpr.c - presence conditions of #if expressions. */
#include "pcexpr.h"
#include "pcfam.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* ---- expression trees ----------------------------------------------- */

typedef enum { PN_NUM, PN_VAR, PN_DEFINED, PN_CALL, PN_UN, PN_BIN, PN_TERN } PNKind;

struct PcNode {
    PNKind kind;
    int op;                 /* Punct for PN_UN/PN_BIN */
    intmax_t num;
    bool u;                 /* PN_NUM: unsigned (a u suffix, or too big) */
    Ident *id;              /* PN_VAR/PN_DEFINED */
    const char *text;       /* PN_CALL: canonical text */
    TokSpan args;           /* PN_CALL: the tokens between the parentheses */
    struct PcNode *a, *b, *c;
};

typedef struct PX {
    Arena *arena;
    Analysis *an;
    const Tok *t;           /* array ends with a TK_EOF sentinel */
    bool ok;
} PX;

#define PTEXT(p, t) pp_text((p)->an->pp, (t))

static PcNode *px_cond(PX *p);

static PcNode *mk(PX *p, PNKind k)
{
    PcNode *n = NEW(p->arena, PcNode);
    n->kind = k;
    return n;
}

bool pc_ppnum(Analysis *an, const Tok *t, intmax_t *out, bool *uns)
{
    const char *s = pp_text(an->pp, t);
    uint32_t i = 0, n = t->len;
    uintmax_t v = 0;
    bool su = false;
    int base = 10;
    if (n > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (s[0] == '0') {
        base = 8;
    }
    for (; i < n; i++) {
        int d;
        char c = s[i];
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else if (c == 'u' || c == 'U' || c == 'l' || c == 'L')
            break;
        else
            return false;
        if (d >= base)
            return false;
        v = v * (uintmax_t)base + (uintmax_t)d;
    }
    for (; i < n; i++)
        if (s[i] == 'u' || s[i] == 'U')
            su = true;
    *out = (intmax_t)v;
    if (uns)
        *uns = su || v > (uintmax_t)INTMAX_MAX;
    return true;
}

static PcNode *px_primary(PX *p)
{
    const Tok *t = p->t;
    PcNode *n;
    if (tok_is_punct(t, P_LPAREN)) {
        p->t = (t + 1);
        n = px_cond(p);
        if (!tok_is_punct(p->t, P_RPAREN)) {
            p->ok = false;
            return n;
        }
        p->t = (p->t + 1);
        return n;
    }
    if (tok_is_word(p->an->in, t, "defined")) {
        bool paren;
        p->t = (t + 1);
        paren = tok_is_punct(p->t, P_LPAREN);
        if (paren)
            p->t = (p->t + 1);
        if (p->t->kind != TK_IDENT) {
            p->ok = false;
            return mk(p, PN_NUM);
        }
        n = mk(p, PN_DEFINED);
        n->id = tok_ident(p->an->in, p->t);
        p->t = (p->t + 1);
        if (paren) {
            if (!tok_is_punct(p->t, P_RPAREN))
                p->ok = false;
            else
                p->t = (p->t + 1);
        }
        return n;
    }
    if (t->kind == TK_IDENT && tok_is_punct((t + 1), P_LPAREN)) {
        /* function-like macro call: opaque */
        StrBuf sb = {0};
        int depth = 0;
        const Tok *u = t;
        n = mk(p, PN_CALL);
        n->id = tok_ident(p->an->in, t);
        sb_putn(&sb, PTEXT(p, u), u->len);
        u = t + 1;              /* the "(" */
        do {
            if (tok_is_punct(u, P_LPAREN))
                depth++;
            else if (tok_is_punct(u, P_RPAREN))
                depth--;
            if (sb.len)
                sb_putc(&sb, ' ');
            sb_putn(&sb, PTEXT(p, u), u->len);
            u = (u + 1);
        } while (u->kind != TK_EOF && depth > 0);
        n->text = arena_strndup(p->arena, sb_cstr(&sb), sb.len);
        sb_free(&sb);
        if (depth == 0 && u - t >= 3) {
            n->args.t = t + 2;
            n->args.n = (uint32_t)(u - t - 3);
        } else {
            n->args.t = NULL;   /* unbalanced */
        }
        p->t = u;
        return n;
    }
    if (t->kind == TK_IDENT) {
        n = mk(p, PN_VAR);
        n->id = tok_ident(p->an->in, t);
        p->t = (t + 1);
        return n;
    }
    if (t->kind == TK_PPNUM) {
        n = mk(p, PN_NUM);
        if (!pc_ppnum(p->an, t, &n->num, &n->u))
            p->ok = false;
        p->t = (t + 1);
        return n;
    }
    if (t->kind == TK_CHAR) {
        n = mk(p, PN_NUM);
        n->num = (unsigned char)PTEXT(p, t)[PTEXT(p, t)[0] == 'L' ? 2 : 1];
        p->t = (t + 1);
        return n;
    }
    p->ok = false;
    p->t = t->kind == TK_EOF ? t : (t + 1);
    return mk(p, PN_NUM);
}

static PcNode *px_unary(PX *p)
{
    const Tok *t = p->t;
    if (tok_is_punct(t, P_BANG) || tok_is_punct(t, P_MINUS) ||
        tok_is_punct(t, P_PLUS) || tok_is_punct(t, P_TILDE)) {
        PcNode *n = mk(p, PN_UN);
        n->op = t->punct;
        p->t = (t + 1);
        n->a = px_unary(p);
        return n;
    }
    return px_primary(p);
}

static int prec(const Tok *t)
{
    if (t->kind != TK_PUNCT)
        return -1;
    switch (t->punct) {
    case P_STAR: case P_SLASH: case P_PERCENT: return 10;
    case P_PLUS: case P_MINUS: return 9;
    case P_SHL: case P_SHR: return 8;
    case P_LT: case P_GT: case P_LE: case P_GE: return 7;
    case P_EQEQ: case P_NE: return 6;
    case P_AMP: return 5;
    case P_CARET: return 4;
    case P_PIPE: return 3;
    case P_ANDAND: return 2;
    case P_OROR: return 1;
    default: return -1;
    }
}

static PcNode *px_binary(PX *p, int min)
{
    PcNode *lhs = px_unary(p);
    for (;;) {
        int pr = prec(p->t);
        PcNode *n;
        if (pr < min || pr < 0)
            return lhs;
        n = mk(p, PN_BIN);
        n->op = p->t->punct;
        p->t = (p->t + 1);
        n->a = lhs;
        n->b = px_binary(p, pr + 1);
        lhs = n;
    }
}

static PcNode *px_cond(PX *p)
{
    PcNode *c = px_binary(p, 1);
    if (tok_is_punct(p->t, P_QUESTION)) {
        PcNode *n = mk(p, PN_TERN);
        p->t = (p->t + 1);
        n->a = c;
        n->b = px_cond(p);
        if (!tok_is_punct(p->t, P_COLON)) {
            p->ok = false;
            return n;
        }
        p->t = (p->t + 1);
        n->c = px_cond(p);
        return n;
    }
    return c;
}

PcNode *pc_parse(PcCtx *cx, TokSpan toks)
{
    Analysis *an = cx->an;
    PX p;
    PcNode *n;
    Tok *arr = NEW_ARRAY(an->arena, Tok, toks.n + 1);
    if (toks.n)
        memcpy(arr, toks.t, sizeof(Tok) * toks.n);
    arr[toks.n].kind = TK_EOF;
    p.arena = an->arena;
    p.an = an;
    p.t = arr;
    p.ok = true;
    n = px_cond(&p);
    if (!p.ok || p.t->kind != TK_EOF)
        return NULL;
    return n;
}

PcNode *pc_defined_node(PcCtx *cx, Ident *id, bool negate)
{
    Arena *a = cx->an->arena;
    PcNode *d = NEW(a, PcNode), *n;
    d->kind = PN_DEFINED;
    d->id = id;
    if (!negate)
        return d;
    n = NEW(a, PcNode);
    n->kind = PN_UN;
    n->op = P_BANG;
    n->a = d;
    return n;
}

/* ---- conditions as BDDs ---------------------------------------------- */

#define PC_K 32

struct PcBlock {
    bool text;
    Ident *id;              /* the macro, or NULL for text */
    const char *key;        /* text */
    uint32_t next;          /* continuation block index + 1, or 0 */
    uint32_t head;          /* the first block of this macro or text */
    uint32_t n;
    PcAtom atoms[PC_K];
};

static uint32_t block_hash(bool text, const Ident *id, const char *key)
{
    return text ? hash_bytes(key, strlen(key))
                : (uint32_t)(((uintptr_t)id >> 4) * 2654435761u);
}

static bool block_is(const PcBlock *b, bool text, const Ident *id,
                     const char *key)
{
    return b->text == text && (text ? !strcmp(b->key, key) : b->id == id);
}

static uint32_t new_block(PcCtx *cx, bool text, Ident *id, const char *key)
{
    PcBlock *b = xcalloc(1, sizeof *b);
    b->text = text;
    b->id = id;
    b->key = key;
    b->head = (uint32_t)cx->blocks.len;
    vec_push(&cx->blocks, b);
    return (uint32_t)cx->blocks.len - 1;
}

static void htab_insert(PcCtx *cx, uint32_t bi)
{
    const PcBlock *b = cx->blocks.data[bi];
    size_t k;
    if (cx->blocks.len * 2 > cx->hcap) {
        size_t i, ncap = cx->hcap ? cx->hcap * 2 : 256;
        uint32_t *old = cx->htab;
        size_t ocap = cx->hcap;
        cx->htab = xcalloc(ncap, sizeof *cx->htab);
        cx->hcap = ncap;
        for (i = 0; i < ocap; i++)
            if (old[i]) {
                const PcBlock *ob = cx->blocks.data[old[i] - 1];
                size_t j = block_hash(ob->text, ob->id, ob->key) & (ncap - 1);
                while (cx->htab[j])
                    j = (j + 1) & (ncap - 1);
                cx->htab[j] = old[i];
            }
        free(old);
    }
    k = block_hash(b->text, b->id, b->key) & (cx->hcap - 1);
    while (cx->htab[k])
        k = (k + 1) & (cx->hcap - 1);
    cx->htab[k] = bi + 1;
}

Bdd pc_atom(PcCtx *cx, PcAKind kind, Ident *id, intmax_t c, const char *text)
{
    bool is_text = kind != PA_DEF && kind != PA_GE;
    uint32_t bi = UINT32_MAX, last;
    PcBlock *b;
    PcAtom *at;
    size_t k, i;
    if (cx->hcap) {
        k = block_hash(is_text, id, text) & (cx->hcap - 1);
        while (cx->htab[k]) {
            if (block_is(cx->blocks.data[cx->htab[k] - 1], is_text, id, text)) {
                bi = cx->htab[k] - 1;
                break;
            }
            k = (k + 1) & (cx->hcap - 1);
        }
    }
    if (bi == UINT32_MAX) {
        bi = new_block(cx, is_text, id, text);
        htab_insert(cx, bi);
    }
    last = bi;
    for (;;) {
        b = cx->blocks.data[last];
        for (i = 0; i < b->n; i++)
            if (b->atoms[i].kind == kind && b->atoms[i].c == c)
                return bdd_var(&cx->m, last * PC_K + (uint32_t)i);
        if (!b->next)
            break;
        last = b->next - 1;
    }
    if (b->n == PC_K) {
        uint32_t nb = new_block(cx, is_text, id, text);
        cx->blocks.data[nb]->head = cx->blocks.data[last]->head;
        cx->blocks.data[last]->next = nb + 1;
        last = nb;
        b = cx->blocks.data[last];
    }
    at = &b->atoms[b->n];
    at->kind = kind;
    at->id = id;
    at->c = c;
    at->text = text;
    at->cls = kind == PA_LEAF ? pc_axis_class(text) : PC_CFG;
    at->derived = id && pc_fam_macro(id->str);
    return bdd_var(&cx->m, last * PC_K + b->n++);
}

const PcAtom *pc_atom_at(const PcCtx *cx, uint32_t var)
{
    const PcBlock *b;
    if (var / PC_K >= cx->blocks.len)
        return NULL;
    b = cx->blocks.data[var / PC_K];
    return var % PC_K < b->n ? &b->atoms[var % PC_K] : NULL;
}

uint32_t pc_var_owner(const PcCtx *cx, uint32_t var)
{
    return var / PC_K < cx->blocks.len ? cx->blocks.data[var / PC_K]->head
                                       : UINT32_MAX;
}

const char *pc_owner_name(const PcCtx *cx, uint32_t owner)
{
    const PcBlock *b = cx->blocks.data[owner];
    return b->text ? b->key : b->id->str;
}

static void node_key(const PcNode *n, StrBuf *sb)
{
    switch (n->kind) {
    case PN_NUM:
        if (n->u)
            sb_printf(sb, "%juU", (uintmax_t)n->num);
        else
            sb_printf(sb, "%jd", n->num);
        break;
    case PN_VAR: sb_printf(sb, "v:%s", n->id->str); break;
    case PN_DEFINED: sb_printf(sb, "d:%s", n->id->str); break;
    case PN_CALL: sb_printf(sb, "c:%s", n->text); break;
    case PN_UN:
        sb_printf(sb, "(u%d ", n->op);
        node_key(n->a, sb);
        sb_putc(sb, ')');
        break;
    case PN_BIN:
        sb_printf(sb, "(b%d ", n->op);
        node_key(n->a, sb);
        sb_putc(sb, ' ');
        node_key(n->b, sb);
        sb_putc(sb, ')');
        break;
    case PN_TERN:
        sb_puts(sb, "(? ");
        node_key(n->a, sb);
        sb_putc(sb, ' ');
        node_key(n->b, sb);
        sb_putc(sb, ' ');
        node_key(n->c, sb);
        sb_putc(sb, ')');
        break;
    }
}

/* "n is nonzero" as an opaque atom keyed by n's canonical text */
static const char *node_key_str(PcCtx *cx, const PcNode *n)
{
    StrBuf sb = {0};
    const char *key;
    node_key(n, &sb);
    key = arena_strndup(cx->an->arena, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    return key;
}

/* ---- values: guarded cases ------------------------------------------- */

/* An integer expression is a list of cases: a value under a condition.  The
 * conditions are disjoint and cover every configuration.  A value is a
 * number, the initial value of a macro scaled and offset (s*X + k: the
 * thresholds of pcexpr.h), a two-digit version s*X + Y + k (Y a macro with a
 * declared range below s, as __GNUC_MINOR__), or opaque: one text, shared by
 * every use of the same computation.  Every operator is a product over the
 * cases of its operands, so macros with several definitions, calls, ?: and
 * comparisons of one macro with another are one mechanism. */
typedef enum { N_CONST, N_INIT, N_VER, N_OPQ } NKind;

typedef struct Num {
    NKind k;
    intmax_t n;             /* the number; for N_INIT, N_VER the k; N_OPQ: 1
                             * when known nonzero */
    Ident *id;              /* N_INIT, N_VER: X, defined here */
    const char *key;        /* N_OPQ */
    bool u;                 /* N_CONST: unsigned */
    intmax_t s;             /* N_INIT, N_VER: the scale of X (>= 1) */
    Ident *lo;              /* N_VER: Y */
} Num;

typedef struct Case {
    Bdd cond;
    Num v;
} Case;

typedef struct Val {
    Case *c;
    uint32_t n;
} Val;

#define MAX_CASES 16

typedef struct VB {          /* a value being built */
    PcCtx *cx;
    VEC(Case) v;
} VB;

static Num n_const(intmax_t n)
{
    Num v = {N_CONST, n, NULL, NULL, false, 1, NULL};
    return v;
}

static Num n_constu(intmax_t n, bool u)
{
    Num v = {N_CONST, n, NULL, NULL, u, 1, NULL};
    return v;
}

static Num n_lin(Ident *id, intmax_t s, intmax_t k)
{
    Num v = {N_INIT, k, id, NULL, false, s, NULL};
    return v;
}

static Num n_init(Ident *id, intmax_t k)
{
    return n_lin(id, 1, k);
}

static Num n_ver(Ident *id, intmax_t s, Ident *lo, intmax_t k)
{
    Num v = {N_VER, k, id, NULL, false, s, lo};
    return v;
}

static Num n_opq(const char *key)
{
    Num v = {N_OPQ, 0, NULL, key, false, 1, NULL};
    return v;
}

/* an opaque value known to be nonzero (the dated answer of a feature test) */
static Num n_opq_nz(const char *key)
{
    Num v = {N_OPQ, 1, NULL, key, false, 1, NULL};
    return v;
}

static bool num_eq(const Num *a, const Num *b)
{
    if (a->k != b->k)
        return false;
    switch (a->k) {
    case N_CONST: return a->n == b->n && a->u == b->u;
    case N_INIT: return a->id == b->id && a->n == b->n && a->s == b->s;
    case N_VER:
        return a->id == b->id && a->n == b->n && a->s == b->s &&
               a->lo == b->lo;
    default: return !strcmp(a->key, b->key);
    }
}

static void vb_add(VB *b, Bdd cond, Num v)
{
    size_t i;
    if (cond == BDD_FALSE)
        return;
    for (i = 0; i < b->v.len; i++)
        if (num_eq(&b->v.data[i].v, &v)) {
            b->v.data[i].cond = bdd_or(&b->cx->m, b->v.data[i].cond, cond);
            return;
        }
    {
        Case c = {cond, v};
        vec_push(&b->v, c);
    }
}

static Val vb_done(VB *b)
{
    Val r;
    r.n = (uint32_t)b->v.len;
    r.c = NEW_ARRAY(b->cx->an->arena, Case, r.n ? r.n : 1);
    if (r.n)
        memcpy(r.c, b->v.data, sizeof(Case) * r.n);
    vec_free(&b->v);
    return r;
}

static void vb_add_val(VB *b, Bdd cond, Val x)
{
    uint32_t i;
    for (i = 0; i < x.n; i++)
        vb_add(b, bdd_and(&b->cx->m, cond, x.c[i].cond), x.c[i].v);
}

static Val v_one(PcCtx *cx, Num v)
{
    VB b = {cx, {0}};
    vb_add(&b, BDD_TRUE, v);
    return vb_done(&b);
}

/* 1 where f holds, else 0 */
static Val v_bool(PcCtx *cx, Bdd f)
{
    VB b = {cx, {0}};
    vb_add(&b, f, n_const(1));
    vb_add(&b, bdd_not(&cx->m, f), n_const(0));
    return vb_done(&b);
}

/* the canonical text of a value, so an opaque result names what it computes */
static const char *num_desc(PcCtx *cx, const Num *v)
{
    Arena *a = cx->an->arena;
    switch (v->k) {
    case N_CONST:
        return v->u ? arena_printf(a, "%juU", (uintmax_t)v->n)
                    : arena_printf(a, "%jd", v->n);
    case N_INIT:
    case N_VER: {
        /* ((s*X) + Y) + k, as key_val reads it back */
        const char *t = v->s == 1
            ? arena_printf(a, "v:%s", v->id->str)
            : arena_printf(a, "(b%d v:%s %jd)", (int)P_STAR, v->id->str, v->s);
        if (v->k == N_VER)
            t = arena_printf(a, "(b%d %s v:%s)", (int)P_PLUS, t, v->lo->str);
        return v->n ? arena_printf(a, "(b%d %s %jd)", (int)P_PLUS, t, v->n)
                    : t;
    }
    default: return v->key;
    }
}

static const char *key_bin(PcCtx *cx, int op, const Num *x, const Num *y)
{
    return arena_printf(cx->an->arena, "(b%d %s %s)", op, num_desc(cx, x),
                        num_desc(cx, y));
}

static Val ev(PcCtx *cx, PcNode *n);

static Bdd ge_bdd(PcCtx *cx, Ident *id, intmax_t c)
{
    return pc_atom(cx, PA_GE, id, c, NULL);
}

/* s * (the initial value of id) >= c, for s >= 1: it is >= ceil(c / s) */
static Bdd ge_lin(PcCtx *cx, Ident *id, intmax_t s, intmax_t c)
{
    return ge_bdd(cx, id, c / s + (c % s > 0));
}

/* s * X + Y >= c for Y in [0, s): X is above c's high digit, or on it with
 * Y at least the low digit */
static Bdd ge_ver(PcCtx *cx, Ident *x, intmax_t s, Ident *y, intmax_t c)
{
    intmax_t q = c / s, r = c % s;
    Bdd lo;
    if (r < 0) {
        q--;
        r += s;
    }
    lo = ge_bdd(cx, x, q);
    if (r)
        lo = bdd_and(&cx->m, lo, ge_bdd(cx, y, r));
    return bdd_or(&cx->m, ge_bdd(cx, x, q + 1), lo);
}

/* the value x (N_INIT or N_VER) >= c */
static Bdd lin_ge(PcCtx *cx, const Num *x, intmax_t c)
{
    return x->k == N_INIT ? ge_lin(cx, x->id, x->s, c)
                          : ge_ver(cx, x->id, x->s, x->lo, c);
}

static Bdd lin_gt(PcCtx *cx, const Num *x, intmax_t c)
{
    return c == INTMAX_MAX ? BDD_FALSE : lin_ge(cx, x, c + 1);
}

/* x op c from the thresholds "value >= c" and "value > c" */
static Bdd lin_cmp(PcCtx *cx, int op, const Num *x, intmax_t c)
{
    BddMgr *m = &cx->m;
    switch (op) {
    case P_GE: return lin_ge(cx, x, c);
    case P_GT: return lin_gt(cx, x, c);
    case P_LT: return bdd_not(m, lin_ge(cx, x, c));
    case P_LE: return bdd_not(m, lin_gt(cx, x, c));
    case P_EQEQ: return bdd_and(m, lin_ge(cx, x, c),
                                bdd_not(m, lin_gt(cx, x, c)));
    default: return bdd_not(m, bdd_and(m, lin_ge(cx, x, c),
                                       bdd_not(m, lin_gt(cx, x, c))));
    }
}

static bool is_cmp(int op)
{
    return op == P_EQEQ || op == P_NE || op == P_LT || op == P_GT ||
           op == P_LE || op == P_GE;
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

/* x op y on constants with C's usual arithmetic conversions: unsigned if
 * either operand is (a shift takes its left operand's type, a comparison
 * gives a signed 0 or 1); false when undefined (division by zero) */
static bool fold(int op, const Num *x, const Num *y, Num *out)
{
    bool uns = x->u || y->u;
    uintmax_t ux = (uintmax_t)x->n, uy = (uintmax_t)y->n;
    intmax_t r;
    bool ru = uns;
    switch (op) {
    case P_STAR: r = (intmax_t)(ux * uy); break;
    case P_SLASH:
        if (!uy) return false;
        r = uns ? (intmax_t)(ux / uy)
                : y->n == -1 ? (intmax_t)(0 - ux) : x->n / y->n;
        break;
    case P_PERCENT:
        if (!uy) return false;
        r = uns ? (intmax_t)(ux % uy) : y->n == -1 ? 0 : x->n % y->n;
        break;
    case P_PLUS: r = (intmax_t)(ux + uy); break;
    case P_MINUS: r = (intmax_t)(ux - uy); break;
    case P_SHL:
        r = (y->n < 0 || y->n > 63) ? 0 : (intmax_t)(ux << y->n);
        ru = x->u;
        break;
    case P_SHR:
        r = (y->n < 0 || y->n > 63) ? 0
            : x->u ? (intmax_t)(ux >> y->n) : x->n >> y->n;
        ru = x->u;
        break;
    case P_LT: r = uns ? ux < uy : x->n < y->n; ru = false; break;
    case P_GT: r = uns ? ux > uy : x->n > y->n; ru = false; break;
    case P_LE: r = uns ? ux <= uy : x->n <= y->n; ru = false; break;
    case P_GE: r = uns ? ux >= uy : x->n >= y->n; ru = false; break;
    case P_EQEQ: r = ux == uy; ru = false; break;
    case P_NE: r = ux != uy; ru = false; break;
    case P_AMP: r = (intmax_t)(ux & uy); break;
    case P_CARET: r = (intmax_t)(ux ^ uy); break;
    case P_PIPE: r = (intmax_t)(ux | uy); break;
    default: return false;
    }
    *out = n_constu(r, ru);
    return true;
}


static Bdd env_defined(PcCtx *cx, Ident *id)
{
    return cx->env.defined ? cx->env.defined(cx->env.ctx, cx, id)
                           : pc_atom(cx, PA_DEF, id, 0, NULL);
}

/* a feature test: __has_include, __has_attribute, ... */
static bool is_feature_test(const PcNode *n)
{
    return !strncmp(n->id->str, "__has_", 6);
}

/* feature tests are 0/1; the dated __has_c_attribute and __has_cpp_attribute
 * answer a year */
static bool is_dated(const PcNode *n)
{
    return strstr(n->id->str, "c_attribute") ||
           strstr(n->id->str, "cpp_attribute");
}

/* the parameter of f that token t names (the variadic one is index f->np),
 * or UINT32_MAX */
static uint32_t fn_param(Analysis *an, const PcFn *f, const Tok *t)
{
    Ident *id = tok_ident(an->in, t);
    uint32_t j;
    if (!id)
        return UINT32_MAX;
    for (j = 0; j < f->np; j++)
        if (id == f->params[j])
            return j;
    return f->va && !strcmp(id->str, "__VA_ARGS__") ? f->np : UINT32_MAX;
}

/* a ## b where a is an identifier and b an identifier or a number: the
 * identifier of both spellings (anything else is not modeled) */
static bool paste(Analysis *an, Tok *a, const Tok *b)
{
    const Ident *ia = tok_ident(an->in, a), *ib = tok_ident(an->in, b), *r;
    const char *sb = ib ? ib->str : pp_text(an->pp, b);
    uint32_t lb = ib ? ib->len : b->len, i;
    char *buf;
    if (!ia || (!ib && b->kind != TK_PPNUM))
        return false;
    for (i = 0; i < lb; i++)
        if (!((sb[i] >= '0' && sb[i] <= '9') || (sb[i] >= 'a' && sb[i] <= 'z') ||
              (sb[i] >= 'A' && sb[i] <= 'Z') || sb[i] == '_'))
            return false;
    buf = arena_alloc(an->arena, ia->len + lb + 1);
    memcpy(buf, ia->str, ia->len);
    memcpy(buf + ia->len, sb, lb);
    r = intern(an->in, buf, ia->len + lb);
    a->kind = TK_IDENT;
    a->flags = TF_PASTED;
    a->len = r->len;
    a->aux = r->id;
    return true;
}

/* The call with the macro's replacement tokens, arguments substituted
 * textually (## pastes the neighbouring tokens), parsed again (C's own order:
 * no parentheses added). */
static PcNode *call_inst(PcCtx *cx, const PcFn *f, const PcNode *call)
{
    Analysis *an = cx->an;
    TokSpan *av;
    uint32_t na = 1, i, j, depth = 0, start = 0, total = 0, k = 0;
    const Tok *at = call->args.t;
    Tok *arr;
    bool glue = false, have = false;
    PX p;
    PcNode *r;
    if (!at)
        return NULL;
    for (i = 0; i < call->args.n; i++) {
        if (tok_is_punct(&at[i], P_LPAREN))
            depth++;
        else if (tok_is_punct(&at[i], P_RPAREN))
            depth--;
        else if (!depth && tok_is_punct(&at[i], P_COMMA))
            na++;
    }
    if (f->va ? na < f->np
              : na != f->np && !(f->np == 0 && na == 1 && !call->args.n))
        return NULL;
    av = NEW_ARRAY(an->arena, TokSpan, na + 1);
    for (i = j = 0; i <= call->args.n; i++) {
        if (i == call->args.n ||
            (!depth && tok_is_punct(&at[i], P_COMMA))) {
            av[j].t = at + start;
            av[j].n = i - start;
            j++;
            start = i + 1;
        } else if (tok_is_punct(&at[i], P_LPAREN)) {
            depth++;
        } else if (tok_is_punct(&at[i], P_RPAREN)) {
            depth--;
        }
    }
    if (f->va) {                /* __VA_ARGS__: the rest, commas included */
        av[f->np].t = f->np < na ? av[f->np].t : at + call->args.n;
        av[f->np].n = f->np < na ? (uint32_t)(at + call->args.n - av[f->np].t)
                                 : 0;
    }
    for (i = 0; i < f->body.n; i++) {
        uint32_t pi = fn_param(an, f, &f->body.t[i]);
        total += pi != UINT32_MAX ? av[pi].n : 1;
    }
    arr = NEW_ARRAY(an->arena, Tok, total + 1);
    for (i = 0; i < f->body.n; i++) {
        const Tok *bt = &f->body.t[i], *src = bt;
        uint32_t pi, m = 1;
        bool merged = false;
        if (tok_is_punct(bt, P_HASHHASH)) {
            glue = true;
            continue;
        }
        pi = fn_param(an, f, bt);
        if (pi != UINT32_MAX) {
            src = av[pi].t;
            m = av[pi].n;
        }
        if (glue && have && m) {
            if (!paste(an, &arr[k - 1], src))
                return NULL;
            src++;
            m--;
            merged = true;
        }
        if (m)
            memcpy(arr + k, src, sizeof(Tok) * m);
        k += m;
        have = m > 0 || merged || (glue && have);
        glue = false;
    }
    arr[k].kind = TK_EOF;
    p.arena = an->arena;
    p.an = an;
    p.t = arr;
    p.ok = true;
    r = px_cond(&p);
    return p.ok && p.t->kind == TK_EOF ? r : NULL;
}


static Bdd num_nz(PcCtx *cx, const Num *v)
{
    switch (v->k) {
    case N_CONST: return v->n ? BDD_TRUE : BDD_FALSE;
    case N_INIT:
    case N_VER:
        return v->n == INTMAX_MIN ? BDD_TRUE : lin_cmp(cx, P_NE, v, -v->n);
    default:
        return v->n ? BDD_TRUE : pc_atom(cx, PA_TEXT, NULL, 0, v->key);
    }
}

/* the condition under which the value is nonzero */
static Bdd truth(PcCtx *cx, Val v)
{
    Bdd r = BDD_FALSE;
    uint32_t i;
    for (i = 0; i < v.n; i++)
        r = bdd_or(&cx->m, r,
                   bdd_and(&cx->m, v.c[i].cond, num_nz(cx, &v.c[i].v)));
    return r;
}

static bool is_lin(const Num *v)
{
    return v->k == N_INIT || v->k == N_VER;
}

/* x with y added to its offset */
static Num lin_add(const Num *x, uintmax_t y)
{
    Num r = *x;
    r.n = (intmax_t)((uintmax_t)x->n + y);
    return r;
}

/* s*X + k times m, for m >= 1 */
static bool lin_mul(const Num *x, intmax_t m, Num *out)
{
    *out = *x;
    return m >= 1 && !__builtin_mul_overflow(x->s, m, &out->s) &&
           !__builtin_mul_overflow(x->n, m, &out->n);
}

/* hi + lo, hi a scaled macro and lo a macro with a declared range that fits
 * in a digit of hi: a version number */
static bool ver_pair(const Num *hi, const Num *lo, Num *out)
{
    intmax_t range;
    if (hi->k != N_INIT || lo->k != N_INIT || lo->s != 1 || hi->id == lo->id)
        return false;
    range = pc_fam_range(lo->id->str);
    if (range < 1 || range > hi->s)
        return false;
    *out = n_ver(hi->id, hi->s, lo->id,
                 (intmax_t)((uintmax_t)hi->n + (uintmax_t)lo->n));
    return true;
}

/* x op y for one pair of values (op a binary operator but && and ||) */
static Val pair_op(PcCtx *cx, int op, const Num *x, const Num *y)
{
    intmax_t c;
    Num r;
    if (x->k == N_CONST && y->k == N_CONST)
        return fold(op, x, y, &r)
                   ? v_one(cx, r)
                   : v_one(cx, n_opq(key_bin(cx, op, x, y)));
    if (is_cmp(op)) {
        /* s*X + k op c  is  s*X op (c - k) */
        if (is_lin(x) && y->k == N_CONST &&
            !__builtin_sub_overflow(y->n, x->n, &c))
            return v_bool(cx, lin_cmp(cx, op, x, c));
        if (x->k == N_CONST && is_lin(y) &&
            !__builtin_sub_overflow(x->n, y->n, &c))
            return v_bool(cx, lin_cmp(cx, swap_cmp(op), y, c));
        if (x->k == N_INIT && y->k == N_INIT && x->id == y->id &&
            x->s == y->s) {
            Num a = n_const(x->n), b = n_const(y->n);
            if (fold(op, &a, &b, &r))
                return v_one(cx, r);
        }
        return v_bool(cx, pc_atom(cx, PA_TEXT, NULL, 0,
                                  key_bin(cx, op, x, y)));
    }
    if (op == P_PLUS && is_lin(x) && y->k == N_CONST)
        return v_one(cx, lin_add(x, (uintmax_t)y->n));
    if (op == P_PLUS && x->k == N_CONST && is_lin(y))
        return v_one(cx, lin_add(y, (uintmax_t)x->n));
    if (op == P_MINUS && is_lin(x) && y->k == N_CONST)
        return v_one(cx, lin_add(x, 0 - (uintmax_t)y->n));
    if (op == P_MINUS && x->k == N_INIT && y->k == N_INIT && x->id == y->id &&
        x->s == y->s)
        return v_one(cx, n_const((intmax_t)((uintmax_t)x->n -
                                            (uintmax_t)y->n)));
    if (op == P_STAR && x->k == N_INIT && y->k == N_CONST && !y->u &&
        lin_mul(x, y->n, &r))
        return v_one(cx, r);
    if (op == P_STAR && x->k == N_CONST && !x->u && y->k == N_INIT &&
        lin_mul(y, x->n, &r))
        return v_one(cx, r);
    if (op == P_SHL && x->k == N_INIT && y->k == N_CONST && !y->u &&
        y->n >= 0 && y->n < 62 && lin_mul(x, (intmax_t)1 << y->n, &r))
        return v_one(cx, r);
    if (op == P_PLUS && (ver_pair(x, y, &r) || ver_pair(y, x, &r)))
        return v_one(cx, r);
    return v_one(cx, n_opq(key_bin(cx, op, x, y)));
}

/* what the macro id is worth now: its definitions under their conditions, 0
 * where it is undefined */
static Val ev_var(PcCtx *cx, Ident *id)
{
    PcEntries es = {0};
    VB b = {cx, {0}};
    size_t i;
    if (cx->env.values) {
        cx->env.values(cx->env.ctx, cx, id, &es);
    } else {
        PcEntry e = {pc_atom(cx, PA_DEF, id, 0, NULL), false, 0, NULL, NULL,
                     NULL};
        vec_push(&es, e);
    }
    for (i = 0; i < es.len; i++) {
        const PcEntry *e = &es.data[i];
        if (e->known) {
            vb_add(&b, e->cond, n_const(e->n));
        } else if (e->expr && cx->depth < 8) {
            Val x;
            cx->depth++;
            x = ev(cx, e->expr);
            cx->depth--;
            vb_add_val(&b, e->cond, x);
        } else if (e->opq) {
            vb_add(&b, e->cond, n_opq(e->opq));
        } else {
            vb_add(&b, e->cond, n_init(id, 0));
        }
    }
    vec_free(&es);
    vb_add(&b, bdd_not(&cx->m, env_defined(cx, id)), n_const(0));
    return vb_done(&b);
}

/* A call NAME(args): each definition of the macro instantiated with the
 * arguments and evaluated where it is used; where the macro is not known to
 * the fold, one opaque value keyed by the call's text (a feature test is
 * 0 or 1). */
static Val ev_call(PcCtx *cx, PcNode *n)
{
    BddMgr *m = &cx->m;
    PcEntries es = {0};
    VB b = {cx, {0}};
    Bdd rest = BDD_TRUE;
    size_t i;
    if (cx->env.values && cx->depth < 8)
        cx->env.values(cx->env.ctx, cx, n->id, &es);
    for (i = 0; i < es.len; i++) {
        const PcEntry *e = &es.data[i];
        PcNode *inst;
        Val x;
        if (!e->fn || !(inst = call_inst(cx, e->fn, n)))
            continue;
        cx->depth++;
        x = ev(cx, inst);
        cx->depth--;
        vb_add_val(&b, e->cond, x);
        rest = bdd_and(m, rest, bdd_not(m, e->cond));
    }
    vec_free(&es);
    if (rest != BDD_FALSE) {
        if (is_feature_test(n)) {
            const char *key = node_key_str(cx, n);
            Bdd a = pc_atom(cx, PA_HAS, NULL, 0, key);
            vb_add(&b, bdd_and(m, rest, a),
                   is_dated(n) ? n_opq_nz(key) : n_const(1));
            vb_add(&b, bdd_and(m, rest, bdd_not(m, a)), n_const(0));
        } else {
            vb_add(&b, rest, n_opq(node_key_str(cx, n)));
        }
    }
    return vb_done(&b);
}

static Val collapse(PcCtx *cx, const PcNode *n)
{
    return v_one(cx, n_opq(node_key_str(cx, n)));
}

static Val ev_unary(PcCtx *cx, PcNode *n)
{
    Val x = ev(cx, n->a);
    VB b = {cx, {0}};
    uint32_t i;
    if (n->op == P_BANG)
        return v_bool(cx, bdd_not(&cx->m, truth(cx, x)));
    if (n->op == P_PLUS)
        return x;
    for (i = 0; i < x.n; i++) {
        const Num *v = &x.c[i].v;
        if (v->k == N_CONST)
            vb_add(&b, x.c[i].cond,
                   n_constu(n->op == P_MINUS ? (intmax_t)(0 - (uintmax_t)v->n)
                                             : ~v->n, v->u));
        else
            vb_add(&b, x.c[i].cond,
                   n_opq(arena_printf(cx->an->arena, "(u%d %s)", n->op,
                                      num_desc(cx, v))));
    }
    return vb_done(&b);
}

static Val ev_binary(PcCtx *cx, PcNode *n)
{
    BddMgr *m = &cx->m;
    Val x = ev(cx, n->a), y;
    VB b = {cx, {0}};
    uint32_t i, j, k;
    if (n->op == P_ANDAND || n->op == P_OROR) {
        bool is_and = n->op == P_ANDAND;
        Bdd tx = truth(cx, x), ty;
        if (tx == (is_and ? BDD_FALSE : BDD_TRUE))
            return v_one(cx, n_const(!is_and)); /* 0 && .. is 0, 1 || .. is 1 */
        ty = truth(cx, ev(cx, n->b));
        return v_bool(cx, is_and ? bdd_and(m, tx, ty) : bdd_or(m, tx, ty));
    }
    y = ev(cx, n->b);
    for (i = 0; i < x.n; i++)
        for (j = 0; j < y.n; j++) {
            Bdd c = bdd_and(m, x.c[i].cond, y.c[j].cond);
            Val r;
            if (c == BDD_FALSE)
                continue;
            r = pair_op(cx, n->op, &x.c[i].v, &y.c[j].v);
            for (k = 0; k < r.n; k++)
                vb_add(&b, bdd_and(m, c, r.c[k].cond), r.c[k].v);
        }
    if (b.v.len > MAX_CASES) {
        vec_free(&b.v);
        return collapse(cx, n);
    }
    return vb_done(&b);
}

static Val ev(PcCtx *cx, PcNode *n)
{
    BddMgr *m = &cx->m;
    switch (n->kind) {
    case PN_NUM:
        return v_one(cx, n_constu(n->num, n->u));
    case PN_DEFINED:
        return v_bool(cx, env_defined(cx, n->id));
    case PN_CALL:
        return ev_call(cx, n);
    case PN_VAR:
        return ev_var(cx, n->id);
    case PN_UN:
        return ev_unary(cx, n);
    case PN_BIN:
        return ev_binary(cx, n);
    case PN_TERN: {
        Bdd t = truth(cx, ev(cx, n->a));
        VB b = {cx, {0}};
        if (t == BDD_TRUE)
            return ev(cx, n->b);
        if (t == BDD_FALSE)
            return ev(cx, n->c);
        vb_add_val(&b, t, ev(cx, n->b));
        vb_add_val(&b, bdd_not(m, t), ev(cx, n->c));
        if (b.v.len > MAX_CASES) {
            vec_free(&b.v);
            return collapse(cx, n);
        }
        return vb_done(&b);
    }
    }
    return collapse(cx, n);
}

PcNode *pc_branch_cond(PcCtx *cx, const SkDirective *d)
{
    switch (d->kind) {
    case SK_IFDEF:
    case SK_ELIFDEF:
        return d->name ? pc_defined_node(cx, d->name, false) : NULL;
    case SK_IFNDEF:
    case SK_ELIFNDEF:
        return d->name ? pc_defined_node(cx, d->name, true) : NULL;
    case SK_IF:
    case SK_ELIF:
        return pc_parse(cx, d->toks);
    default:
        return NULL;
    }
}

bool pc_const_value(PcCtx *cx, TokSpan toks, intmax_t *out)
{
    PcNode *n;
    Val v;
    if (!toks.n || !(n = pc_parse(cx, toks)) || pc_has_vars(n))
        return false;
    v = ev(cx, n);
    if (v.n != 1 || v.c[0].v.k != N_CONST)
        return false;
    *out = v.c[0].v.n;
    return true;
}

bool pc_has_vars(const PcNode *n)
{
    if (!n)
        return false;
    if (n->kind == PN_DEFINED || n->kind == PN_VAR || n->kind == PN_CALL)
        return true;
    return pc_has_vars(n->a) || pc_has_vars(n->b) || pc_has_vars(n->c);
}

Bdd pc_truth(PcCtx *cx, PcNode *n)
{
    return truth(cx, ev(cx, n));
}

/* The clauses GE(hi) implies GE(lo) over the atoms of f's support are
 * conjoined before the test. */
static Bdd pc_monotone(PcCtx *cx, Bdd f)
{
    BddMgr *m = &cx->m;
    BddVars vs = {0};
    size_t i, j;
    Bdd g = f;
    bdd_vars(m, f, &vs);
    for (i = 0; i < vs.len; i++) {
        const PcAtom *a = pc_atom_at(cx, vs.data[i]), *lo = NULL;
        uint32_t lov = 0;
        if (!a || a->kind != PA_GE)
            continue;
        for (j = 0; j < vs.len; j++) {      /* the next lower threshold: the rest follow */
            const PcAtom *b = pc_atom_at(cx, vs.data[j]);
            if (b && b->kind == PA_GE && b->id == a->id && b->c < a->c &&
                (!lo || b->c > lo->c))
                lo = b, lov = vs.data[j];
        }
        if (lo)
            g = bdd_and(m, g, bdd_or(m, bdd_not(m, bdd_var(m, vs.data[i])),
                                     bdd_var(m, lov)));
    }
    vec_free(&vs);
    return g;
}

Bdd pc_constrain(PcCtx *cx, Bdd f)
{
    size_t i = (f * 2654435761u >> 8) % (sizeof cx->cons / sizeof *cx->cons);
    Bdd g;
    if (cx->cons[i].f == f)
        return cx->cons[i].g;
    g = pc_family(cx, pc_monotone(cx, f));
    if (!cx->m.over) {
        cx->cons[i].f = f;
        cx->cons[i].g = g;
    }
    return g;
}

int pc_sat(PcCtx *cx, Bdd f)
{
    Bdd g;
    if (cx->m.over)
        return -1;
    g = pc_constrain(cx, f);
    return cx->m.over ? -1 : g != BDD_FALSE;
}

void pc_init(PcCtx *cx, Analysis *an, size_t node_cap)
{
    memset(cx, 0, sizeof *cx);
    cx->an = an;
    bdd_init(&cx->m, node_cap);
}

void pc_free(PcCtx *cx)
{
    size_t i;
    for (i = 0; i < cx->blocks.len; i++)
        free(cx->blocks.data[i]);
    vec_free(&cx->blocks);
    free(cx->htab);
    bdd_free(&cx->m);
}

/* ---- opaque keys under a host ---------------------------------------- */

typedef struct KeyP {
    const char *s;
    const PcKeyHost *h;
} KeyP;

static bool key_val(KeyP *k, Num *out);

static void key_ws(KeyP *k)
{
    while (*k->s == ' ')
        k->s++;
}

static bool key_close(KeyP *k)
{
    key_ws(k);
    if (*k->s != ')')
        return false;
    k->s++;
    return true;
}

static bool key_val(KeyP *k, Num *out)
{
    key_ws(k);
    if (*k->s == '(') {
        char tag = k->s[1];
        Num a, b, c;
        char *e;
        long op;
        if (tag == '?') {
            k->s += 2;
            if (!key_val(k, &a) || !key_val(k, &b) || !key_val(k, &c) ||
                !key_close(k))
                return false;
            *out = a.n ? b : c;
            return true;
        }
        if (tag != 'b' && tag != 'u')
            return false;
        op = strtol(k->s + 2, &e, 10);
        k->s = e;
        if (!key_val(k, &a))
            return false;
        if (tag == 'u') {
            if (!key_close(k))
                return false;
            if (op == P_BANG)
                *out = n_const(!a.n);
            else if (op == P_MINUS)
                *out = n_constu((intmax_t)(0 - (uintmax_t)a.n), a.u);
            else if (op == P_TILDE)
                *out = n_constu(~a.n, a.u);
            else if (op == P_PLUS)
                *out = a;
            else
                return false;
            return true;
        }
        if (!key_val(k, &b) || !key_close(k))
            return false;
        if (op == P_ANDAND || op == P_OROR) {
            *out = n_const(op == P_ANDAND ? (a.n && b.n) : (a.n || b.n));
            return true;
        }
        return fold((int)op, &a, &b, out);
    }
    if ((k->s[0] == 'v' || k->s[0] == 'd') && k->s[1] == ':') {
        bool is_v = k->s[0] == 'v';
        const char *name = k->s + 2, *e = name;
        intmax_t n;
        while (*e && *e != ' ' && *e != ')')
            e++;
        k->s = e;
        if (is_v) {
            if (!k->h->value(k->h->ctx, name, (size_t)(e - name), &n))
                return false;
        } else {
            n = k->h->defined(k->h->ctx, name, (size_t)(e - name));
        }
        *out = n_const(n);
        return true;
    }
    if ((*k->s >= '0' && *k->s <= '9') || *k->s == '-') {
        char *e;
        bool neg = *k->s == '-';
        uintmax_t v = strtoumax(k->s + neg, &e, 10);
        if (e == k->s + neg)
            return false;
        k->s = e;
        if (*k->s == 'U') {
            k->s++;
            *out = n_constu((intmax_t)v, true);
        } else {
            *out = n_const(neg ? (intmax_t)(0 - v) : (intmax_t)v);
        }
        return true;
    }
    return false;
}

bool pc_key_eval(const char *key, const PcKeyHost *h, intmax_t *out)
{
    KeyP k = {key, h};
    Num r;
    if (!key_val(&k, &r) || *k.s)
        return false;
    *out = r.n;
    return true;
}
