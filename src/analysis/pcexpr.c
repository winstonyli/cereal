/* pcexpr.c - presence conditions of #if expressions. */
#include "pcexpr.h"

#include <string.h>

/* ---- expression trees ----------------------------------------------- */

typedef enum { PN_NUM, PN_VAR, PN_DEFINED, PN_CALL, PN_UN, PN_BIN, PN_TERN } PNKind;

struct PcNode {
    PNKind kind;
    int op;                 /* Punct for PN_UN/PN_BIN */
    intmax_t num;
    Ident *id;              /* PN_VAR/PN_DEFINED */
    const char *text;       /* PN_CALL: canonical text */
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

bool pc_ppnum(Analysis *an, const Tok *t, intmax_t *out)
{
    const char *s = pp_text(an->pp, t);
    uint32_t i = 0, n = t->len;
    uintmax_t v = 0;
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
    *out = (intmax_t)v;
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
        if (!pc_ppnum(p->an, t, &n->num))
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
    bool is_text = kind == PA_TEXT;
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
        cx->blocks.data[last]->next = nb + 1;
        last = nb;
        b = cx->blocks.data[last];
    }
    at = &b->atoms[b->n];
    at->kind = kind;
    at->id = id;
    at->c = c;
    at->text = text;
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

static void node_key(const PcNode *n, StrBuf *sb)
{
    switch (n->kind) {
    case PN_NUM: sb_printf(sb, "%jd", (intmax_t)n->num); break;
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
static Bdd text_atom(PcCtx *cx, const PcNode *n)
{
    StrBuf sb = {0};
    const char *key;
    Bdd b;
    node_key(n, &sb);
    key = arena_strndup(cx->an->arena, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    b = pc_atom(cx, PA_TEXT, NULL, 0, key);
    return b;
}

/* What a subexpression is worth:
 *   V_CONST  the integer n
 *   V_BOOL   0 or 1 according to b
 *   V_VAL    the effective value of macro id
 *   V_OPQ    an integer nothing is known about except the text of node */
typedef enum { V_CONST, V_BOOL, V_VAL, V_OPQ } VKind;

typedef struct Val {
    VKind k;
    intmax_t n;
    Bdd b;
    Ident *id;
    PcNode *node;
} Val;

static Val v_const(intmax_t n)
{
    Val v = {V_CONST, n, 0, NULL, NULL};
    return v;
}

static Val v_bool(Bdd b)
{
    Val v = {V_BOOL, 0, b, NULL, NULL};
    if (b == BDD_TRUE || b == BDD_FALSE)
        return v_const(b == BDD_TRUE);
    return v;
}

static Val v_opq(PcNode *n)
{
    Val v = {V_OPQ, 0, 0, NULL, n};
    return v;
}

static Val ev(PcCtx *cx, PcNode *n);
static Bdd num_cmp(PcCtx *cx, int op, Ident *id, intmax_t c);
static bool fold(int op, intmax_t x, intmax_t y, intmax_t *out);

static Bdd ge_bdd(PcCtx *cx, Ident *id, intmax_t c)
{
    return pc_atom(cx, PA_GE, id, c, NULL);
}

/* the initial value of id is >= c + 1 */
static Bdd gt_bdd(PcCtx *cx, Ident *id, intmax_t c)
{
    return c == INTMAX_MAX ? BDD_FALSE : ge_bdd(cx, id, c + 1);
}

static Bdd truth(PcCtx *cx, Val v)
{
    BddMgr *m = &cx->m;
    switch (v.k) {
    case V_CONST:
        return v.n ? BDD_TRUE : BDD_FALSE;
    case V_BOOL:
        return v.b;
    case V_VAL:
        return bdd_not(m, num_cmp(cx, P_EQEQ, v.id, 0));
    case V_OPQ:
        if (v.node->kind == PN_TERN) {
            Bdd c = truth(cx, ev(cx, v.node->a));
            Bdd t = truth(cx, ev(cx, v.node->b));
            return bdd_ite(m, c, t, truth(cx, ev(cx, v.node->c)));
        }
        return text_atom(cx, v.node);
    }
    return BDD_FALSE;
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

/* x op y on constants; false when undefined (division by zero) */
static bool fold(int op, intmax_t x, intmax_t y, intmax_t *out)
{
    uintmax_t ux = (uintmax_t)x, uy = (uintmax_t)y;
    switch (op) {
    case P_STAR: *out = (intmax_t)(ux * uy); break;
    case P_SLASH:
        if (!y) return false;
        *out = y == -1 ? (intmax_t)(0 - ux) : x / y;
        break;
    case P_PERCENT:
        if (!y) return false;
        *out = y == -1 ? 0 : x % y;
        break;
    case P_PLUS: *out = (intmax_t)(ux + uy); break;
    case P_MINUS: *out = (intmax_t)(ux - uy); break;
    case P_SHL: *out = (y < 0 || y > 62) ? 0 : (intmax_t)(ux << y); break;
    case P_SHR: *out = (y < 0 || y > 62) ? 0 : x >> y; break;
    case P_LT: *out = x < y; break;
    case P_GT: *out = x > y; break;
    case P_LE: *out = x <= y; break;
    case P_GE: *out = x >= y; break;
    case P_EQEQ: *out = x == y; break;
    case P_NE: *out = x != y; break;
    case P_AMP: *out = x & y; break;
    case P_CARET: *out = x ^ y; break;
    case P_PIPE: *out = x | y; break;
    default: return false;
    }
    return true;
}

static Bdd env_defined(PcCtx *cx, Ident *id)
{
    return cx->env.defined ? cx->env.defined(cx->env.ctx, cx, id)
                           : pc_atom(cx, PA_DEF, id, 0, NULL);
}

/* the initial value of id, when it is defined, op c, as thresholds */
static Bdd init_cmp(PcCtx *cx, int op, Ident *id, intmax_t c)
{
    BddMgr *m = &cx->m;
    switch (op) {
    case P_GE: return ge_bdd(cx, id, c);
    case P_GT: return gt_bdd(cx, id, c);
    case P_LT: return bdd_not(m, ge_bdd(cx, id, c));
    case P_LE: return bdd_not(m, gt_bdd(cx, id, c));
    case P_EQEQ: return bdd_and(m, ge_bdd(cx, id, c),
                                bdd_not(m, gt_bdd(cx, id, c)));
    default: return bdd_not(m, bdd_and(m, ge_bdd(cx, id, c),
                                       bdd_not(m, gt_bdd(cx, id, c))));
    }
}

/* (value of id now) op c: every value it can hold under its condition, and
 * 0 when it is undefined */
static Bdd num_cmp(PcCtx *cx, int op, Ident *id, intmax_t c)
{
    BddMgr *m = &cx->m;
    PcEntries es = {0};
    Bdd r = BDD_FALSE;
    intmax_t z = 0;
    size_t i;
    if (cx->env.values) {
        cx->env.values(cx->env.ctx, cx, id, &es);
    } else {
        PcEntry e = {pc_atom(cx, PA_DEF, id, 0, NULL), false, 0};
        vec_push(&es, e);
    }
    for (i = 0; i < es.len; i++) {
        const PcEntry *e = &es.data[i];
        intmax_t t = 0;
        Bdd tb;
        if (e->known) {
            fold(op, e->n, c, &t);
            tb = t ? BDD_TRUE : BDD_FALSE;
        } else {
            tb = init_cmp(cx, op, id, c);
        }
        r = bdd_or(m, r, bdd_and(m, e->cond, tb));
    }
    vec_free(&es);
    fold(op, 0, c, &z);
    return bdd_or(m, r, bdd_and(m, bdd_not(m, env_defined(cx, id)),
                                z ? BDD_TRUE : BDD_FALSE));
}

/* b (0 or 1) op c, or c op b when flipped */
static Bdd cmp_bool(PcCtx *cx, int op, Bdd b, intmax_t c, bool flipped)
{
    intmax_t r0 = 0, r1 = 0;
    fold(op, flipped ? c : 0, flipped ? 0 : c, &r0);
    fold(op, flipped ? c : 1, flipped ? 1 : c, &r1);
    return bdd_ite(&cx->m, b, r1 ? BDD_TRUE : BDD_FALSE,
                   r0 ? BDD_TRUE : BDD_FALSE);
}

static Val binop(PcCtx *cx, PcNode *n, Val x, Val y)
{
    int op = n->op;
    intmax_t r;
    if (x.k == V_CONST && y.k == V_CONST)
        return fold(op, x.n, y.n, &r) ? v_const(r) : v_opq(n);
    if (is_cmp(op)) {
        if (x.k == V_VAL && y.k == V_CONST)
            return v_bool(num_cmp(cx, op, x.id, y.n));
        if (x.k == V_CONST && y.k == V_VAL)
            return v_bool(num_cmp(cx, swap_cmp(op), y.id, x.n));
        if (x.k == V_BOOL && y.k == V_CONST)
            return v_bool(cmp_bool(cx, op, x.b, y.n, false));
        if (x.k == V_CONST && y.k == V_BOOL)
            return v_bool(cmp_bool(cx, op, y.b, x.n, true));
        if (x.k == V_BOOL && y.k == V_BOOL &&
            (op == P_EQEQ || op == P_NE)) {
            Bdd same = bdd_ite(&cx->m, x.b, y.b, bdd_not(&cx->m, y.b));
            return v_bool(op == P_EQEQ ? same : bdd_not(&cx->m, same));
        }
    }
    return v_opq(n);
}

static Val ev(PcCtx *cx, PcNode *n)
{
    Val x, y;
    switch (n->kind) {
    case PN_NUM:
        return v_const(n->num);
    case PN_DEFINED:
        return v_bool(env_defined(cx, n->id));
    case PN_CALL:
        return v_bool(text_atom(cx, n));
    case PN_VAR:
        x.k = V_VAL;
        x.n = 0;
        x.b = 0;
        x.id = n->id;
        x.node = NULL;
        return x;
    case PN_UN:
        x = ev(cx, n->a);
        switch (n->op) {
        case P_BANG:
            return v_bool(bdd_not(&cx->m, truth(cx, x)));
        case P_PLUS:
            return x;
        case P_MINUS:
            if (x.k == V_CONST)
                return v_const((intmax_t)(0 - (uintmax_t)x.n));
            break;
        case P_TILDE:
            if (x.k == V_CONST)
                return v_const(~x.n);
            break;
        }
        return v_opq(n);
    case PN_TERN:
        x = ev(cx, n->a);
        if (x.k == V_CONST)
            return ev(cx, x.n ? n->b : n->c);
        return v_opq(n);        /* its truth is the ite, see truth() */
    case PN_BIN:
        x = ev(cx, n->a);
        if (n->op == P_ANDAND || n->op == P_OROR) {
            bool is_and = n->op == P_ANDAND;
            Bdd tx, ty;
            if (x.k == V_CONST && (x.n != 0) != is_and)
                return v_const(!is_and);    /* 0 && .. is 0, 1 || .. is 1 */
            y = ev(cx, n->b);
            ty = truth(cx, y);
            if (x.k == V_CONST)
                return v_bool(ty);
            tx = truth(cx, x);
            return v_bool(is_and ? bdd_and(&cx->m, tx, ty)
                                 : bdd_or(&cx->m, tx, ty));
        }
        y = ev(cx, n->b);
        return binop(cx, n, x, y);
    }
    return v_opq(n);
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
int pc_sat(PcCtx *cx, Bdd f)
{
    BddMgr *m = &cx->m;
    BddVars vs = {0};
    size_t i, j;
    Bdd g = f;
    if (m->over)
        return -1;
    bdd_vars(m, f, &vs);
    for (i = 0; i < vs.len; i++) {
        const PcAtom *a = pc_atom_at(cx, vs.data[i]);
        if (!a || a->kind != PA_GE)
            continue;
        for (j = 0; j < vs.len; j++) {
            const PcAtom *b = pc_atom_at(cx, vs.data[j]);
            if (b && b->kind == PA_GE && b->id == a->id && b->c < a->c)
                g = bdd_and(m, g, bdd_or(m, bdd_not(m, bdd_var(m, vs.data[i])),
                                         bdd_var(m, vs.data[j])));
        }
    }
    vec_free(&vs);
    return m->over ? -1 : g != BDD_FALSE;
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
