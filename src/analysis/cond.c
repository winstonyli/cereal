/* cond.c - conditional-compilation analysis.
 *
 * Configuration space: every #if/#elif/#ifdef/#ifndef/#else in a user file
 * (active or not) is turned into a presence condition, a BDD over
 *   - defined(X)
 *   - threshold atoms "the value of X is >= c" for every constant c that X
 *     is compared with, with the C99 rule that an undefined X is 0
 *   - opaque atoms for calls such as __GNUC_PREREQ(4, 1) and for arithmetic
 *     or two-macro comparisons, keyed by canonical text
 * Each branch is checked by satisfiability: dead if it can never be
 * selected, redundant if it is always selected once reached.
 *
 * Dynamic checks use preprocessor events: tested-but-never-defined macros
 * with a similar defined name (typos). */
#include "analysis.h"

#include "bdd.h"

#include <string.h>

/* ---- expression trees ----------------------------------------------- */

typedef enum { N_NUM, N_VAR, N_DEFINED, N_CALL, N_UN, N_BIN, N_TERN } NKind;

typedef struct Node {
    NKind kind;
    int op;                 /* Punct for N_UN/N_BIN */
    intmax_t num;
    Ident *id;              /* N_VAR/N_DEFINED */
    const char *text;       /* N_CALL: canonical text */
    struct Node *a, *b, *c;
} Node;

typedef struct PX {
    Arena *arena;
    Analysis *an;
    const Tok *t;           /* array ends with a TK_EOF sentinel */
    bool ok;
} PX;

#define PTEXT(p, t) pp_text((p)->an->pp, (t))

static Node *px_cond(PX *p);

static Node *mk(PX *p, NKind k)
{
    Node *n = NEW(p->arena, Node);
    n->kind = k;
    return n;
}

static bool num_value(PX *p, const Tok *t, intmax_t *out)
{
    const char *s = PTEXT(p, t);
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

static Node *px_primary(PX *p)
{
    const Tok *t = p->t;
    Node *n;
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
            return mk(p, N_NUM);
        }
        n = mk(p, N_DEFINED);
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
        n = mk(p, N_CALL);
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
        n = mk(p, N_VAR);
        n->id = tok_ident(p->an->in, t);
        p->t = (t + 1);
        return n;
    }
    if (t->kind == TK_PPNUM) {
        n = mk(p, N_NUM);
        if (!num_value(p, t, &n->num))
            p->ok = false;
        p->t = (t + 1);
        return n;
    }
    if (t->kind == TK_CHAR) {
        n = mk(p, N_NUM);
        n->num = (unsigned char)PTEXT(p, t)[PTEXT(p, t)[0] == 'L' ? 2 : 1];
        p->t = (t + 1);
        return n;
    }
    p->ok = false;
    p->t = t->kind == TK_EOF ? t : (t + 1);
    return mk(p, N_NUM);
}

static Node *px_unary(PX *p)
{
    const Tok *t = p->t;
    if (tok_is_punct(t, P_BANG) || tok_is_punct(t, P_MINUS) ||
        tok_is_punct(t, P_PLUS) || tok_is_punct(t, P_TILDE)) {
        Node *n = mk(p, N_UN);
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

static Node *px_binary(PX *p, int min)
{
    Node *lhs = px_unary(p);
    for (;;) {
        int pr = prec(p->t);
        Node *n;
        if (pr < min || pr < 0)
            return lhs;
        n = mk(p, N_BIN);
        n->op = p->t->punct;
        p->t = (p->t + 1);
        n->a = lhs;
        n->b = px_binary(p, pr + 1);
        lhs = n;
    }
}

static Node *px_cond(PX *p)
{
    Node *c = px_binary(p, 1);
    if (tok_is_punct(p->t, P_QUESTION)) {
        Node *n = mk(p, N_TERN);
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

static Node *parse_cond(Analysis *an, TokSpan toks)
{
    PX p;
    Node *n;
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

static Node *make_defined(Arena *a, Ident *id, bool negate)
{
    Node *d = NEW(a, Node), *n;
    d->kind = N_DEFINED;
    d->id = id;
    if (!negate)
        return d;
    n = NEW(a, Node);
    n->kind = N_UN;
    n->op = P_BANG;
    n->a = d;
    return n;
}

/* ---- conditions as BDDs ---------------------------------------------- */

/* The variables of one file's BDD manager, created in order of first use:
 *   A_DEF   defined(X)
 *   A_GE    the effective value of X is >= c  (X undefined counts as 0)
 *   A_TEXT  an opaque condition (a function-like call, arithmetic on a
 *           macro, a comparison of two macros), keyed by canonical text
 * The links between atoms (GE(hi) implies GE(lo); an undefined X is 0) are
 * clauses added per query over the atoms in its support. */
typedef enum { A_DEF, A_GE, A_TEXT } AKind;

typedef struct Atom {
    AKind kind;
    Ident *id;              /* A_DEF, A_GE */
    intmax_t c;             /* A_GE */
    const char *text;       /* A_TEXT */
} Atom;

typedef struct Ctx {
    Analysis *an;
    BddMgr m;
    VEC(Atom) atoms;
} Ctx;

#define COND_BDD_CAP ((size_t)1 << 20)

static Bdd atom_bdd(Ctx *cx, AKind kind, Ident *id, intmax_t c,
                    const char *text)
{
    size_t i;
    Atom at;
    for (i = 0; i < cx->atoms.len; i++) {
        const Atom *x = &cx->atoms.data[i];
        if (x->kind == kind && x->id == id && x->c == c &&
            (kind != A_TEXT || !strcmp(x->text, text)))
            return bdd_var(&cx->m, (uint32_t)i);
    }
    at.kind = kind;
    at.id = id;
    at.c = c;
    at.text = text;
    vec_push(&cx->atoms, at);
    return bdd_var(&cx->m, (uint32_t)(cx->atoms.len - 1));
}

static void node_key(const Node *n, StrBuf *sb)
{
    switch (n->kind) {
    case N_NUM: sb_printf(sb, "%jd", (intmax_t)n->num); break;
    case N_VAR: sb_printf(sb, "v:%s", n->id->str); break;
    case N_DEFINED: sb_printf(sb, "d:%s", n->id->str); break;
    case N_CALL: sb_printf(sb, "c:%s", n->text); break;
    case N_UN:
        sb_printf(sb, "(u%d ", n->op);
        node_key(n->a, sb);
        sb_putc(sb, ')');
        break;
    case N_BIN:
        sb_printf(sb, "(b%d ", n->op);
        node_key(n->a, sb);
        sb_putc(sb, ' ');
        node_key(n->b, sb);
        sb_putc(sb, ')');
        break;
    case N_TERN:
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
static Bdd text_atom(Ctx *cx, const Node *n)
{
    StrBuf sb = {0};
    const char *key;
    Bdd b;
    node_key(n, &sb);
    key = arena_strndup(cx->an->arena, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    b = atom_bdd(cx, A_TEXT, NULL, 0, key);
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
    Node *node;
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

static Val v_opq(Node *n)
{
    Val v = {V_OPQ, 0, 0, NULL, n};
    return v;
}

static Val ev(Ctx *cx, Node *n);

static Bdd ge_bdd(Ctx *cx, Ident *id, intmax_t c)
{
    return atom_bdd(cx, A_GE, id, c, NULL);
}

/* the effective value of id is >= c + 1 */
static Bdd gt_bdd(Ctx *cx, Ident *id, intmax_t c)
{
    return c == INTMAX_MAX ? BDD_FALSE : ge_bdd(cx, id, c + 1);
}

static Bdd truth(Ctx *cx, Val v)
{
    BddMgr *m = &cx->m;
    switch (v.k) {
    case V_CONST:
        return v.n ? BDD_TRUE : BDD_FALSE;
    case V_BOOL:
        return v.b;
    case V_VAL:
        /* nonzero: not (>= 0 and not >= 1) */
        return bdd_not(m, bdd_and(m, ge_bdd(cx, v.id, 0),
                                  bdd_not(m, ge_bdd(cx, v.id, 1))));
    case V_OPQ:
        if (v.node->kind == N_TERN) {
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

/* (effective value of id) op c, as thresholds */
static Bdd cmp_val(Ctx *cx, int op, Ident *id, intmax_t c)
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

/* b (0 or 1) op c, or c op b when flipped */
static Bdd cmp_bool(Ctx *cx, int op, Bdd b, intmax_t c, bool flipped)
{
    intmax_t r0 = 0, r1 = 0;
    fold(op, flipped ? c : 0, flipped ? 0 : c, &r0);
    fold(op, flipped ? c : 1, flipped ? 1 : c, &r1);
    return bdd_ite(&cx->m, b, r1 ? BDD_TRUE : BDD_FALSE,
                   r0 ? BDD_TRUE : BDD_FALSE);
}

static Val binop(Ctx *cx, Node *n, Val x, Val y)
{
    int op = n->op;
    intmax_t r;
    if (x.k == V_CONST && y.k == V_CONST)
        return fold(op, x.n, y.n, &r) ? v_const(r) : v_opq(n);
    if (is_cmp(op)) {
        if (x.k == V_VAL && y.k == V_CONST)
            return v_bool(cmp_val(cx, op, x.id, y.n));
        if (x.k == V_CONST && y.k == V_VAL)
            return v_bool(cmp_val(cx, swap_cmp(op), y.id, x.n));
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

static Val ev(Ctx *cx, Node *n)
{
    Val x, y;
    switch (n->kind) {
    case N_NUM:
        return v_const(n->num);
    case N_DEFINED:
        return v_bool(atom_bdd(cx, A_DEF, n->id, 0, NULL));
    case N_CALL:
        return v_bool(text_atom(cx, n));
    case N_VAR:
        x.k = V_VAL;
        x.n = 0;
        x.b = 0;
        x.id = n->id;
        x.node = NULL;
        return x;
    case N_UN:
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
    case N_TERN:
        x = ev(cx, n->a);
        if (x.k == V_CONST)
            return ev(cx, x.n ? n->b : n->c);
        return v_opq(n);        /* its truth is the ite, see truth() */
    case N_BIN:
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

static bool has_vars(Node *n)
{
    if (!n)
        return false;
    if (n->kind == N_DEFINED || n->kind == N_VAR || n->kind == N_CALL)
        return true;
    return has_vars(n->a) || has_vars(n->b) || has_vars(n->c);
}

/* Is f satisfiable?  1 yes, 0 no, -1 unknown (the BDD outgrew its cap).
 * The links between the atoms of f's support are conjoined first. */
static int sat(Ctx *cx, Bdd f)
{
    BddMgr *m = &cx->m;
    size_t i, j, n = cx->atoms.len;
    bool *seen;
    Bdd g = f;
    if (m->over)
        return -1;
    seen = xcalloc(n ? n : 1, 1);
    bdd_support(m, f, seen, (uint32_t)n);
    for (i = 0; i < n; i++) {
        const Atom *a = &cx->atoms.data[i];
        if (!seen[i] || a->kind != A_GE)
            continue;
        for (j = 0; j < n; j++) {
            const Atom *b = &cx->atoms.data[j];
            Bdd vi, vj;
            if (!seen[j] || b->id != a->id)
                continue;
            vi = bdd_var(m, (uint32_t)i);
            vj = bdd_var(m, (uint32_t)j);
            if (b->kind == A_GE && b->c < a->c)         /* >= hi means >= lo */
                g = bdd_and(m, g, bdd_or(m, bdd_not(m, vi), vj));
            else if (b->kind == A_DEF)                  /* undefined is 0 */
                g = bdd_and(m, g, bdd_or(m, vj, a->c > 0 ? bdd_not(m, vi) : vi));
        }
    }
    free(seen);
    return m->over ? -1 : g != BDD_FALSE;
}

/* ---- per-file walk -------------------------------------------------- */

typedef struct Frame {
    int opener;             /* skeleton index of the #if */
    Bdd outer;              /* the enclosing branches are taken */
    Bdd negprior;           /* no earlier branch of this chain was taken */
    Bdd reach;              /* outer and negprior: this position is reached */
    Bdd cur;                /* the current branch's condition holds */
    bool has_cur;           /* false for #else and for an unmodeled condition */
    bool cur_novars;        /* the condition mentions no macro (#if 0) */
    bool dead;              /* current branch already known dead */
    bool unknown;           /* a condition could not be modeled */
} Frame;

struct CondState {
    VEC(SrcFile *) done;
    /* dynamic */
    VEC(Tok) undefined_refs;
};

void cond_discard(Analysis *a)
{
    vec_free(&a->cond->done);
    vec_free(&a->cond->undefined_refs);
}

static Node *branch_cond(Analysis *an, SkDirective *d)
{
    switch (d->kind) {
    case SK_IFDEF:
        return d->name ? make_defined(an->arena, d->name, false) : NULL;
    case SK_IFNDEF:
        return d->name ? make_defined(an->arena, d->name, true) : NULL;
    case SK_IF:
    case SK_ELIF:
        return parse_cond(an, d->toks);
    default:
        return NULL;
    }
}

/* Evaluate the branch condition c into fr (c is NULL when it did not parse). */
static void set_cond(Ctx *cx, Frame *fr, Node *c)
{
    fr->has_cur = c != NULL;
    fr->unknown |= c == NULL;
    fr->cur_novars = c && !has_vars(c);
    if (c)
        fr->cur = truth(cx, ev(cx, c));
}

static void set_reach(Ctx *cx, Frame *fr)
{
    fr->reach = bdd_and(&cx->m, fr->outer, fr->negprior);
}

static bool path_dead(Frame *stack, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        if (stack[i].dead)
            return true;
    return false;
}

static bool path_unknown(Frame *stack, int depth)
{
    int i;
    for (i = 0; i < depth; i++)
        if (stack[i].unknown)
            return true;
    return false;
}

static const char *dir_name(SkKind k)
{
    switch (k) {
    case SK_IF: return "#if";
    case SK_IFDEF: return "#ifdef";
    case SK_IFNDEF: return "#ifndef";
    case SK_ELIF: return "#elif";
    case SK_ELSE: return "#else";
    default: return "#endif";
    }
}

static void check_branch(Analysis *a, Skeleton *sk, Ctx *cx, Frame *stack,
                         int depth, SkDirective *d)
{
    BddMgr *m = &cx->m;
    Frame *fr = &stack[depth - 1];
    int reach, sat_c, sat_notc;

    if (m->over || path_unknown(stack, depth) || path_dead(stack, depth - 1))
        return;
    if (fr->has_cur && fr->cur_novars) {
        /* constant condition: #if 0 / #if 1 */
        if (fr->cur != BDD_TRUE && fr->cur != BDD_FALSE)
            return;             /* 1 / 0 by zero, say: nothing to claim */
        if (d->kind != SK_IFNDEF && d->kind != SK_IFDEF)
            diag_report(a->diag, DL_REMARK, "cond-constant", d->kw_loc,
                        "%s condition is constant (%s)", dir_name(d->kind),
                        fr->cur == BDD_TRUE ? "always true" : "always false");
        fr->dead = fr->cur == BDD_FALSE;
        return;
    }
    /* reachability of this branch position */
    reach = sat(cx, fr->reach);
    if (reach == 0) {
        Diagnostic *dg = diag_report(a->diag, DL_WARNING, "cond-dead-branch",
            d->kw_loc, "%s branch can never be taken: the enclosing and "
            "earlier conditions already cover every configuration",
            dir_name(d->kind));
        if (d->opener >= 0)
            diag_note(a->diag, dg, sk->dirs.data[d->opener].kw_loc,
                      "conditional starts here");
        fr->dead = true;
        return;
    }
    if (!fr->has_cur || reach < 0)
        return;
    sat_c = sat(cx, bdd_and(m, fr->reach, fr->cur));
    if (sat_c == 0) {
        int self = sat(cx, fr->cur);
        diag_report(a->diag, DL_WARNING, "cond-dead-branch", d->kw_loc,
                    self == 0 ? "%s condition can never be true"
                              : "%s condition contradicts the enclosing or "
                                "earlier conditions; this branch is dead in "
                                "every configuration",
                    dir_name(d->kind));
        fr->dead = true;
        return;
    }
    sat_notc = sat(cx, bdd_and(m, fr->reach, bdd_not(m, fr->cur)));
    if (sat_notc == 0 && d->kind != SK_IFNDEF) {
        diag_report(a->diag, DL_WARNING, "cond-redundant", d->kw_loc,
                    "%s condition is always true here (implied by the "
                    "enclosing and earlier conditions)%s", dir_name(d->kind),
                    d->kind == SK_ELIF ? "; use #else" : "");
    }
}

static bool macro_like_word(const char *s, size_t n)
{
    static const char *const skip[] = {"if", "ifdef", "ifndef", "endif", "else",
                                       "elif", "defined", "not", "end", "of",
                                       "the", "and", "or", "NOT", "TODO",
                                       "XXX", "FIXME", NULL};
    size_t i;
    bool upper = false;
    int k;
    if (n < 2)
        return false;
    for (k = 0; skip[k]; k++)
        if (strlen(skip[k]) == n && !memcmp(skip[k], s, n))
            return false;
    for (i = 0; i < n; i++)
        if ((s[i] >= 'A' && s[i] <= 'Z') || s[i] == '_')
            upper = true;
    return upper;
}

static bool chain_mentions(const Interner *in, Skeleton *sk, int opener,
                           int upto, const char *w,
                           size_t n)
{
    int i;
    for (i = opener; i <= upto && i < (int)sk->dirs.len; i++) {
        SkDirective *d = &sk->dirs.data[i];
        uint32_t k;
        if (i != opener && d->opener != opener)
            continue;
        if (d->kind == SK_ENDIF)
            continue;
        for (k = 0; k < d->toks.n; k++) {
            const Tok *t = &d->toks.t[k];
            if (t->kind == TK_IDENT && t->len == n &&
                !memcmp(tok_ident(in, t)->str, w, n))
                return true;
        }
    }
    return false;
}

static void check_label(Analysis *a, Skeleton *sk, int idx)
{
    SkDirective *d = &sk->dirs.data[idx];
    const char *s = d->comment, *first_word = NULL;
    size_t first_len = 0;
    bool any = false;
    if (!s || d->opener < 0)
        return;
    while (*s) {
        const char *b;
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '_')) {
            s++;
            continue;
        }
        b = s;
        while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
               (*s >= '0' && *s <= '9') || *s == '_')
            s++;
        if (!macro_like_word(b, (size_t)(s - b)))
            continue;
        if (chain_mentions(a->in, sk, d->opener, idx, b, (size_t)(s - b)))
            return; /* matches */
        if (!any) {
            first_word = b;
            first_len = (size_t)(s - b);
        }
        any = true;
    }
    if (any) {
        Diagnostic *dg = diag_report(a->diag, DL_WARNING, "endif-label",
            d->kw_loc, "comment on %s mentions '%.*s', which the conditional "
            "does not test", dir_name(d->kind), (int)first_len, first_word);
        diag_note(a->diag, dg, sk->dirs.data[d->opener].kw_loc,
                  "the conditional being closed starts here");
    }
}

static void analyze_file(Analysis *a, SrcFile *f)
{
    Skeleton *sk = an_skeleton(a, f);
    Ctx cx = {0};
    VEC(Frame) st = {0};
    size_t i;
    if (!sk->dirs.len)
        return;
    cx.an = a;
    bdd_init(&cx.m, COND_BDD_CAP);
    /* past the BDD cap (cx.m.over) the rest of the file is left unchecked */
    for (i = 0; i < sk->dirs.len && !cx.m.over; i++) {
        SkDirective *d = &sk->dirs.data[i];
        switch (d->kind) {
        case SK_IF: case SK_IFDEF: case SK_IFNDEF: {
            Frame fr;
            memset(&fr, 0, sizeof fr);
            fr.opener = (int)i;
            fr.outer = BDD_TRUE;
            if (st.len) {
                const Frame *up = &vec_last(&st);
                fr.outer = up->has_cur ? bdd_and(&cx.m, up->reach, up->cur)
                                       : up->reach;
            }
            fr.negprior = BDD_TRUE;
            set_reach(&cx, &fr);
            set_cond(&cx, &fr, branch_cond(a, d));
            vec_push(&st, fr);
            check_branch(a, sk, &cx, st.data, (int)st.len, d);
            break;
        }
        case SK_ELIF: case SK_ELSE: {
            Frame *fr;
            if (!st.len)
                break;
            fr = &vec_last(&st);
            if (fr->has_cur)
                fr->negprior = bdd_and(&cx.m, fr->negprior,
                                       bdd_not(&cx.m, fr->cur));
            else
                fr->unknown = true;
            fr->dead = false;
            set_reach(&cx, fr);
            if (d->kind == SK_ELSE) {
                fr->has_cur = false;
                fr->cur_novars = false;
                check_label(a, sk, (int)i);
            } else {
                set_cond(&cx, fr, branch_cond(a, d));
            }
            check_branch(a, sk, &cx, st.data, (int)st.len, d);
            break;
        }
        case SK_ENDIF:
            check_label(a, sk, (int)i);
            if (st.len)
                st.len--;
            break;
        default:
            break;
        }
    }
    vec_free(&st);
    vec_free(&cx.atoms);
    bdd_free(&cx.m);
}

/* FOO_A vs FOO_B, V1 vs V2: distinct names by design, not typos. */
static bool looks_like_sibling(const Ident *x, const Ident *y)
{
    uint32_t i, diffs = 0, digit_only = 1;
    const char *seg_x = x->str, *seg_y = y->str;
    if (x->len != y->len)
        return false;
    for (i = 0; i < x->len; i++) {
        if (x->str[i] != y->str[i]) {
            diffs++;
            if (!(x->str[i] >= '0' && x->str[i] <= '9') ||
                !(y->str[i] >= '0' && y->str[i] <= '9'))
                digit_only = 0;
        }
    }
    if (diffs == 0)
        return false;
    if (digit_only)
        return true;
    if (diffs == 1) {
        /* the differing char is in a short final segment */
        const char *px = strrchr(seg_x, '_'), *py = strrchr(seg_y, '_');
        size_t lx = px ? strlen(px + 1) : x->len;
        size_t ly = py ? strlen(py + 1) : y->len;
        for (i = 0; x->str[i] == y->str[i]; i++)
            ;
        if (px && py && (size_t)(px - seg_x) < i && lx <= 2 && ly <= 2)
            return true;
    }
    return false;
}

/* ---- listeners ------------------------------------------------------ */

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Analysis *a = ctx;
    size_t i;
    (void)via;
    if (!an_user_file(f))
        return;
    for (i = 0; i < a->cond->done.len; i++)
        if (a->cond->done.data[i] == f)
            return;
    vec_push(&a->cond->done, f);
    analyze_file(a, f);
}

static void on_macro_ref(void *ctx, Ident *id, Macro *m, const Tok *tok,
                         RefKind kind)
{
    Analysis *a = ctx;
    (void)id;
    if (m || (kind != REF_IFDEF && kind != REF_DEFINED && kind != REF_IF_VALUE))
        return;
    if (!an_user_loc(a, tok->loc))
        return;
    vec_push(&a->cond->undefined_refs, *tok);
}

void cond_attach(Analysis *a)
{
    PPListener l;
    memset(&l, 0, sizeof l);
    a->cond = NEW(a->arena, CondState);
    l.ctx = a;
    l.file_enter = on_file_enter;
    l.macro_ref = on_macro_ref;
    pp_add_listener(a->pp, l);
}

void cond_finish(Analysis *a)
{
    size_t i, j;
    VEC(Ident *) seen = {0};
    for (i = 0; i < a->cond->undefined_refs.len; i++) {
        Tok *t = &a->cond->undefined_refs.data[i];
        Ident *id = tok_ident(a->in, t), *best = NULL;
        unsigned best_d = 3, limit;
        bool dup = false;
        if (mt_hist(a->pp->mt, id))
            continue; /* defined at some point in the TU */
        {
            /* a misspelled include guard is reported by -Wheader-guard */
            SrcFile *f = srcmgr_file_of(a->sm, t->loc);
            Skeleton *sk = f ? an_skeleton(a, f) : NULL;
            if (sk && sk->guard_ifndef == id && sk->guard_define &&
                sk->guard_define != id)
                continue;
        }
        for (j = 0; j < seen.len; j++)
            if (seen.data[j] == id)
                dup = true;
        if (dup)
            continue;
        vec_push(&seen, id);
        limit = id->len >= 8 ? 2 : 1;
        if (id->len >= 4) {
            /* candidates: every name defined in the TU; ties go to the
             * smallest spelling, so the pick does not depend on interning
             * order (the interner can outlive a build) */
            size_t k;
            for (k = 0; k < a->pp->macros.len; k++) {
                Ident *cand = a->pp->macros.data[k]->name;
                unsigned dist;
                if (cand == id || cand == best || looks_like_sibling(id, cand))
                    continue;
                dist = edit_distance(id->str, id->len, cand->str, cand->len,
                                     limit);
                if (dist <= limit && (dist < best_d ||
                                      (dist == best_d &&
                                       strcmp(cand->str, best->str) < 0))) {
                    best_d = dist;
                    best = cand;
                }
            }
        }
        if (best) {
            Diagnostic *d = diag_report(a->diag, DL_WARNING, "cond-typo", t->loc,
                "'%s' is never defined in this translation unit; did you mean "
                "'%s'?", id->str, best->str);
            diag_set_range(d, t->loc, t->loc + t->len);
            if (d && mt_hist(a->pp->mt, best)->file && mt_hist(a->pp->mt, best)->file->kind != SF_VIRTUAL)
                diag_note(a->diag, d, mt_hist(a->pp->mt, best)->name_loc,
                          "'%s' is defined here", best->str);
            if (d)
                fix_tok_replace(a->diag, d, 0, a->sm, a->in, t, best->str,
                                FIX_INFO);
        } else {
            diag_report(a->diag, DL_REMARK, "cond-never-defined", t->loc,
                        "'%s' is never defined in this translation unit",
                        id->str);
        }
    }
    vec_free(&seen);
    vec_free(&a->cond->done);
    vec_free(&a->cond->undefined_refs);
}
