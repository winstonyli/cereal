/* prove_smt.c - the prover's term DAG, its rewrites and its solver queries (see prove_smt.h). */
#include "prove_smt.h"

#include <stdio.h>
#include <string.h>

#include "symx.h"

typedef VEC(PsT) PsVec;

/* ---- the DAG ---- */

void ps_free(Ps *ps) { vec_free(&ps->n); vec_free(&ps->v); }

static PsT mk(Ps *ps, int op, int sort, unsigned w, PsT a, PsT b, PsT c, uint64_t k)
{
    PsNode n;
    n.op = (uint8_t)op; n.sort = (uint8_t)sort; n.w = (uint8_t)w; n.a = a; n.b = b; n.c = c; n.k = k;
    vec_push(&ps->n, n);
    return (PsT)ps->n.len;
}

PsT ps_true(Ps *ps) { return mk(ps, PS_TRUE, PS_BOOL, 0, 0, 0, 0, 0); }
PsT ps_bvc(Ps *ps, unsigned w, uint64_t v) { return mk(ps, PS_BVC, PS_BV, w, 0, 0, 0, v); }
PsT ps_intc(Ps *ps, uint64_t v) { return mk(ps, PS_INTC, PS_INT, 0, 0, 0, 0, v); }

PsT ps_var(Ps *ps, const char *name, int sort, unsigned w)
{
    size_t i;
    for (i = 0; i < ps->v.len; i++)
        if (!strcmp(ps->v.data[i].name, name)) break;
    if (i == ps->v.len) {
        PsVar v;
        memset(&v, 0, sizeof v);
        snprintf(v.name, sizeof v.name, "%s", name);
        v.sort = (uint8_t)sort; v.w = (uint8_t)w;
        vec_push(&ps->v, v);
    }
    return mk(ps, PS_VAR, ps->v.data[i].sort, ps->v.data[i].w, 0, 0, 0, i);
}

PsT ps_op(Ps *ps, int op, PsT a, PsT b)
{
    int sort = PS_BOOL;
    unsigned w = 0;
    switch (op) {
    case PS_BVADD: case PS_BVMUL: case PS_BVNOT: case PS_BVSUB: sort = PS_BV; w = ps_at(ps, a)->w; break;
    case PS_IADD: case PS_IMUL: case PS_ISUB: sort = PS_INT; break;
    case PS_SEL: sort = PS_BV; w = ps_at(ps, a)->w; break;      /* the array's element width */
    default: break;
    }
    return mk(ps, op, sort, w, a, b, 0, 0);
}

PsT ps_resize(Ps *ps, PsT a, unsigned w, bool sign) { return mk(ps, PS_RESIZE, PS_BV, w, a, 0, 0, w | (sign ? PS_SIGNED : 0)); }
PsT ps_store(Ps *ps, PsT arr, PsT idx, PsT val) { return mk(ps, PS_STORE, PS_ARR, ps_at(ps, arr)->w, arr, idx, val, 0); }
PsT ps_forall(Ps *ps, PsT var, PsT body) { return mk(ps, PS_FORALL, PS_BOOL, 0, var, body, 0, 0); }

PsT ps_subst(Ps *ps, PsT t, PsT var, PsT repl)
{
    PsNode n = *ps_at(ps, t);               /* a copy: building nodes may move the arena */
    PsT a, b, c;
    uint64_t vk = ps_at(ps, var)->k;
    if (n.op == PS_VAR) return n.k == vk ? repl : t;
    if (!n.a) return t;                     /* a constant */
    if (n.op == PS_FORALL) {
        if (ps_at(ps, n.a)->k == vk) return t;      /* shadowed; repl must not mention the bound variable */
        b = ps_subst(ps, n.b, var, repl);
        return b == n.b ? t : ps_forall(ps, n.a, b);
    }
    a = ps_subst(ps, n.a, var, repl);
    b = n.b ? ps_subst(ps, n.b, var, repl) : 0;
    c = n.c ? ps_subst(ps, n.c, var, repl) : 0;
    if (a == n.a && b == n.b && c == n.c) return t;
    return mk(ps, n.op, n.sort, n.w, a, b, c, n.k);
}

bool ps_has(const Ps *ps, PsT t, int op)
{
    const PsNode *n = ps_at(ps, t);
    if (n->op == op) return true;
    return (n->a && ps_has(ps, n->a, op)) || (n->b && ps_has(ps, n->b, op)) || (n->c && ps_has(ps, n->c, op));
}

/* ---- value ranges ---- */

bool ps_range(const Ps *ps, PsT t, int64_t *lo, int64_t *hi)
{
    const PsNode *n = ps_at(ps, t);
    int64_t al, ah, bl, bh, c[4];
    unsigned i, wo;
    if (n->sort != PS_BV)
        return false;
    switch (n->op) {
    case PS_BVC:
        if (n->w == 64 && n->k > (uint64_t)INT64_MAX) return false;
        *lo = *hi = (int64_t)n->k;
        return true;
    case PS_VAR:
        if (n->w >= 64) return false;
        *lo = 0; *hi = (int64_t)((UINT64_C(1) << n->w) - 1);
        return true;
    case PS_RESIZE: {                       /* the operand's bits, read signed or unsigned: its own range does not matter */
        const PsNode *o = ps_at(ps, n->a);
        bool sg = (n->k & PS_SIGNED) != 0;
        wo = o->w;
        if (wo >= n->w || wo >= 64) return false;           /* truncation, or no narrower operand */
        if (o->op == PS_BVC) {
            *lo = *hi = sg && (o->k >> (wo - 1) & 1) ? (int64_t)(o->k - (UINT64_C(1) << wo)) : (int64_t)o->k;
            return true;
        }
        if (sg) { *lo = -(int64_t)(UINT64_C(1) << (wo - 1)); *hi = (int64_t)(UINT64_C(1) << (wo - 1)) - 1; }
        else { *lo = 0; *hi = (int64_t)((UINT64_C(1) << wo) - 1); }
        return true;
    }
    case PS_BVADD: case PS_BVSUB: case PS_BVMUL:
        if (n->w != 64 || !ps_range(ps, n->a, &al, &ah) || !ps_range(ps, n->b, &bl, &bh))
            return false;
        if (n->op == PS_BVADD)
            return !__builtin_add_overflow(al, bl, lo) && !__builtin_add_overflow(ah, bh, hi);
        if (n->op == PS_BVSUB)
            return !__builtin_sub_overflow(al, bh, lo) && !__builtin_sub_overflow(ah, bl, hi);
        if (__builtin_mul_overflow(al, bl, &c[0]) || __builtin_mul_overflow(al, bh, &c[1]) ||
            __builtin_mul_overflow(ah, bl, &c[2]) || __builtin_mul_overflow(ah, bh, &c[3]))
            return false;
        *lo = *hi = c[0];
        for (i = 1; i < 4; i++) { if (c[i] < *lo) *lo = c[i]; if (c[i] > *hi) *hi = c[i]; }
        return true;
    default:
        return false;
    }
}

/* ---- bit-vectors to Int ---- */

/* The Int image of a bit-vector term is the number its bits mean, read signed or unsigned (its "view").  Every node has
 * one view (a leaf its first consumer's, an operation its first operand's, a resize its flag's); a consumer that wants
 * the other one gets a fresh Int variable defined from it (a "view change").  An add, subtract or multiply is the Int
 * operation plus an obligation that the result lies in the width's range, so that the image equals the bit-vector result. */

enum { V_NONE, V_S, V_U };

typedef struct PsImg {
    Ps *ps;
    VEC(PsT) facts;                     /* true of the real values whatever the hypotheses: leaf ranges, view changes */
    VEC(PsT) obl;                       /* obligations: an operation's Int result lies in its width's range */
    VEC(uint8_t) lv;                    /* per bit-vector variable: its view (V_NONE until a consumer asks) */
    unsigned fresh;
} PsImg;

static PsT pw2(Ps *ps, unsigned e) { return e == 64 ? mk(ps, PS_P64, PS_INT, 0, 0, 0, 0, 0) : ps_intc(ps, UINT64_C(1) << e); }
static PsT ineg(Ps *ps, PsT m) { return ps_op(ps, PS_ISUB, ps_intc(ps, 0), m); }
static PsT icmp(Ps *ps, int op, PsT a, PsT b) { return ps_op(ps, op, a, b); }

/* the value of w bits k in a view, as an Int constant */
static PsT img_const(Ps *ps, unsigned w, uint64_t k, int view)
{
    bool neg = view == V_S && (k >> (w - 1) & 1);
    return neg ? ineg(ps, ps_intc(ps, w == 64 ? 0 - k : (UINT64_C(1) << w) - k)) : ps_intc(ps, k);
}

static PsT in_range(Ps *ps, PsT r, unsigned w, int view)
{
    PsT lo = view == V_S ? ineg(ps, pw2(ps, w - 1)) : ps_intc(ps, 0), hi = pw2(ps, view == V_S ? w - 1 : w);
    return ps_op(ps, PS_AND, icmp(ps, PS_ILE, lo, r), icmp(ps, PS_ILT, r, hi));
}

/* an Int variable of the tier's own naming; 0 when ps_var (which matches by name alone) returns a variable of another sort */
static PsT int_var(Ps *ps, const char *nm)
{
    PsT t = ps_var(ps, nm, PS_INT, 0);
    return ps_at(ps, t)->sort == PS_INT ? t : 0;
}

/* v, read in view from at width w, as the value read in view to (a fresh variable, defined, when the views differ; 0 on a name clash) */
static PsT view_change(PsImg *c, PsT v, unsigned w, int from, int to)
{
    Ps *ps = c->ps;
    char nm[24];
    PsT u, zero = ps_intc(ps, 0), keep, wrap;
    if (from == to) return v;
    snprintf(nm, sizeof nm, "_n%u", c->fresh++);
    if (!(u = int_var(ps, nm))) return 0;
    if (from == V_S) {          /* v >= 0: u = v, else u = v + 2^w */
        keep = ps_op(ps, PS_AND, icmp(ps, PS_ILE, zero, v), ps_op(ps, PS_EQ, u, v));
        wrap = ps_op(ps, PS_AND, icmp(ps, PS_ILT, v, zero), ps_op(ps, PS_EQ, u, ps_op(ps, PS_IADD, v, pw2(ps, w))));
    } else {                    /* v < 2^(w-1): u = v, else u = v - 2^w */
        keep = ps_op(ps, PS_AND, icmp(ps, PS_ILT, v, pw2(ps, w - 1)), ps_op(ps, PS_EQ, u, v));
        wrap = ps_op(ps, PS_AND, icmp(ps, PS_ILE, pw2(ps, w - 1), v), ps_op(ps, PS_EQ, u, ps_op(ps, PS_ISUB, v, pw2(ps, w))));
    }
    vec_push(&c->facts, ps_op(ps, PS_OR, keep, wrap));
    return u;
}

/* t's Int image and its view; a constant takes hint's view (0: unsigned).  0 when the tier does not apply. */
static PsT img_bv(PsImg *c, PsT t, int hint, int *view);

/* the images of two operands of width w in one view (a constant follows the other operand) */
static bool img_pair(PsImg *c, PsT x, PsT y, unsigned w, int hint, PsT *ta, PsT *tb, int *view)
{
    int va, vb;
    if (ps_at(c->ps, x)->op == PS_BVC) {
        if (!(*tb = img_bv(c, y, hint, &vb)) || !(*ta = img_bv(c, x, vb, &va))) return false;
    } else {
        if (!(*ta = img_bv(c, x, hint, &va)) || !(*tb = img_bv(c, y, va, &vb))) return false;
    }
    if (!(*tb = view_change(c, *tb, w, vb, va))) return false;
    *view = va;
    return true;
}

static PsT img_bv(PsImg *c, PsT t, int hint, int *view)
{
    Ps *ps = c->ps;
    PsNode n = *ps_at(ps, t);
    PsT a, b, r;
    int v;
    if (!hint) hint = V_U;
    switch (n.op) {
    case PS_BVC:
        *view = hint;
        return img_const(ps, n.w, n.k, hint);
    case PS_VAR: {
        char nm[48];
        if (n.sort != PS_BV || strlen(ps->v.data[n.k].name) + 2 >= sizeof ((PsVar *)0)->name) return 0;
        snprintf(nm, sizeof nm, "%s.i", ps->v.data[n.k].name);
        if (!(a = int_var(ps, nm))) return 0;
        while (c->lv.len <= n.k) vec_push(&c->lv, V_NONE);
        if (c->lv.data[n.k] == V_NONE) {            /* the first consumer picks the view; the range is a fact of the leaf */
            c->lv.data[n.k] = (uint8_t)hint;
            vec_push(&c->facts, in_range(ps, a, n.w, hint));
        }
        *view = c->lv.data[n.k];
        return a;
    }
    case PS_RESIZE: {
        unsigned to = (unsigned)(n.k & 0xff), from = ps_at(ps, n.a)->w;
        int want = n.k & PS_SIGNED ? V_S : V_U;
        if (to < from) return 0;                    /* truncation */
        if (to == from) return img_bv(c, n.a, hint, view);
        if (!(a = img_bv(c, n.a, want, &v))) return 0;
        *view = want;                               /* widening keeps the value */
        return view_change(c, a, from, v, want);
    }
    case PS_BVADD: case PS_BVSUB: case PS_BVMUL: {
        int64_t lo, hi;
        if (!img_pair(c, n.a, n.b, n.w, hint, &a, &b, &v)) return 0;
        r = ps_op(ps, n.op == PS_BVADD ? PS_IADD : n.op == PS_BVSUB ? PS_ISUB : PS_IMUL, a, b);
        /* a signed 64-bit term with an exact range cannot wrap; every other result owes its range */
        if (!(v == V_S && n.w == 64 && ps_range(ps, t, &lo, &hi))) vec_push(&c->obl, in_range(ps, r, n.w, v));
        *view = v;
        return r;
    }
    default:
        return 0;
    }
}

/* a formula's Int image; 0 when the tier does not apply */
static PsT img_fm(PsImg *c, PsT t)
{
    Ps *ps = c->ps;
    PsNode n = *ps_at(ps, t);
    PsT a, b;
    int v;
    switch (n.op) {
    case PS_TRUE: case PS_FALSE: return t;
    case PS_VAR: return n.sort == PS_BOOL ? t : 0;
    case PS_BVULT: case PS_BVULE: case PS_BVSLT: case PS_BVSLE: {
        int want = n.op == PS_BVULT || n.op == PS_BVULE ? V_U : V_S;
        unsigned w = ps_at(ps, n.a)->w;
        if (!img_pair(c, n.a, n.b, w, want, &a, &b, &v)) return 0;
        if (!(a = view_change(c, a, w, v, want)) || !(b = view_change(c, b, w, v, want))) return 0;
        return ps_op(ps, n.op == PS_BVULT || n.op == PS_BVSLT ? PS_ILT : PS_ILE, a, b);
    }
    case PS_EQ:
        if (ps_at(ps, n.a)->sort == PS_BV) {
            if (!img_pair(c, n.a, n.b, ps_at(ps, n.a)->w, V_U, &a, &b, &v)) return 0;
            return ps_op(ps, PS_EQ, a, b);
        }
        if (ps_at(ps, n.a)->sort != PS_BOOL) return 0;
        /* fall through */
    case PS_AND: case PS_OR: case PS_IMP:
        if (!(a = img_fm(c, n.a)) || !(b = img_fm(c, n.b))) return 0;
        return ps_op(ps, n.op, a, b);
    case PS_NOT:
        return (a = img_fm(c, n.a)) ? ps_op(ps, PS_NOT, a, 0) : 0;
    default:
        return 0;
    }
}

/* ---- printing ---- */

static const char *const op_name[] = {
    [PS_BVADD] = "bvadd", [PS_BVMUL] = "bvmul", [PS_BVNOT] = "bvnot", [PS_BVULT] = "bvult", [PS_BVULE] = "bvule",
    [PS_BVSUB] = "bvsub", [PS_BVSLT] = "bvslt", [PS_BVSLE] = "bvsle",
    [PS_IADD] = "+", [PS_IMUL] = "*", [PS_ISUB] = "-", [PS_ILT] = "<", [PS_ILE] = "<=",
    [PS_EQ] = "=", [PS_AND] = "and", [PS_OR] = "or", [PS_NOT] = "not", [PS_IMP] = "=>",
    [PS_SEL] = "select", [PS_STORE] = "store"
};

static void put_sort(StrBuf *sb, int sort, unsigned w)
{
    switch (sort) {
    case PS_BOOL: sb_puts(sb, "Bool"); break;
    case PS_INT: sb_puts(sb, "Int"); break;
    case PS_BV: sb_printf(sb, "(_ BitVec %u)", w); break;
    default: sb_printf(sb, "(Array (_ BitVec 64) (_ BitVec %u))", w); break;
    }
}

void ps_print(const Ps *ps, PsT t, StrBuf *sb)
{
    const PsNode *n = ps_at(ps, t);
    switch (n->op) {
    case PS_TRUE: sb_puts(sb, "true"); return;
    case PS_FALSE: sb_puts(sb, "false"); return;
    case PS_BVC: sb_printf(sb, "(_ bv%llu %u)", (unsigned long long)n->k, (unsigned)n->w); return;
    case PS_INTC: sb_printf(sb, "%llu", (unsigned long long)n->k); return;
    case PS_P64: sb_puts(sb, "18446744073709551616"); return;
    case PS_VAR: sb_puts(sb, ps->v.data[n->k].name); return;
    case PS_FORALL:
        sb_puts(sb, "(forall ((");
        sb_puts(sb, ps->v.data[ps_at(ps, n->a)->k].name);
        sb_putc(sb, ' ');
        put_sort(sb, ps_at(ps, n->a)->sort, ps_at(ps, n->a)->w);
        sb_puts(sb, ")) ");
        ps_print(ps, n->b, sb);
        sb_putc(sb, ')');
        return;
    case PS_RESIZE: {
        unsigned from = ps_at(ps, n->a)->w, to = (unsigned)(n->k & 0xff);
        if (to == from) { ps_print(ps, n->a, sb); return; }
        if (to < from) sb_printf(sb, "((_ extract %u 0) ", to - 1);
        else sb_printf(sb, "((_ %s %u) ", n->k & PS_SIGNED ? "sign_extend" : "zero_extend", to - from);
        ps_print(ps, n->a, sb);
        sb_putc(sb, ')');
        return;
    }
    default: break;
    }
    sb_putc(sb, '(');
    sb_puts(sb, op_name[n->op]);
    sb_putc(sb, ' ');
    ps_print(ps, n->a, sb);
    if (n->b) { sb_putc(sb, ' '); ps_print(ps, n->b, sb); }
    if (n->c) { sb_putc(sb, ' '); ps_print(ps, n->c, sb); }
    sb_putc(sb, ')');
}

/* ---- the solver ---- */

struct PsSolver {
    SxSolver *sx;
    const Ps *ps;               /* the DAG whose variables are declared on the pipe */
    VEC(uint8_t) declared;      /* per variable of ps */
};

PsSolver *ps_open(const char *cmd, char *err, size_t errn)
{
    PsSolver *s;
    SxSolver *x = sx_open(cmd, err, errn);
    if (!x) return NULL;
    s = xcalloc(1, sizeof *s);
    s->sx = x;
    return s;
}

void ps_close(PsSolver *s)
{
    if (!s) return;
    sx_close(s->sx);
    vec_free(&s->declared);
    free(s);
}

/* Declares, in text, the free variables of t that the pipe has not seen.  A variable a quantifier binds is not free. */
static void declare(PsSolver *s, const Ps *ps, PsT t, StrBuf *sb)
{
    const PsNode *n = ps_at(ps, t);
    if (n->op == PS_VAR) {
        while (s->declared.len < ps->v.len) vec_push(&s->declared, 0);
        if (!s->declared.data[n->k]) {
            s->declared.data[n->k] = 1;
            sb_printf(sb, "(declare-const %s ", ps->v.data[n->k].name);
            put_sort(sb, n->sort, n->w);
            sb_puts(sb, ")\n");
        }
        return;
    }
    if (n->op == PS_FORALL) { declare(s, ps, n->b, sb); return; }
    if (n->a) declare(s, ps, n->a, sb);
    if (n->b) declare(s, ps, n->b, sb);
    if (n->c) declare(s, ps, n->c, sb);
}

/* A bound variable is declared by the pass above too (it is a PS_VAR in the body); undeclare it by never emitting a
 * declaration that a quantifier shadows: the declaration is harmless, so the caller marks quantified names up front. */
static void mark_bound(PsSolver *s, const Ps *ps, PsT t)
{
    const PsNode *n = ps_at(ps, t);
    if (n->op == PS_FORALL) {
        while (s->declared.len < ps->v.len) vec_push(&s->declared, 0);
        s->declared.data[ps_at(ps, n->a)->k] = 1;       /* never declared as a constant */
        mark_bound(s, ps, n->b);
        return;
    }
    if (n->a) mark_bound(s, ps, n->a);
    if (n->b) mark_bound(s, ps, n->b);
    if (n->c) mark_bound(s, ps, n->c);
}

/* ---- the instantiation tier ----
 * A quantifier the goal proves is replaced by its body at a fresh constant (valid iff the body is, for any value);
 * a quantifier assumed is replaced by its body at each of those constants (a consequence, so a weaker hypothesis).
 * What the result proves, the original proves; the solver then needs no quantifier instantiation of its own, which
 * it does slowly over wide array elements (the int copy loop's "preserved" goal: 5.5 s against 0.1 s). */
typedef struct PsSk { VEC(PsT) v; unsigned next; bool bad; } PsSk;

static bool ps_conn(int op) { return op == PS_AND || op == PS_OR || op == PS_NOT || op == PS_IMP; }

/* pass 1: positive quantifiers become fresh constants (collected in sk) */
static PsT skolem(Ps *ps, PsSk *sk, PsT t, bool pos)
{
    PsNode n = *ps_at(ps, t);
    PsT a, b;
    if (n.op == PS_FORALL) {
        const PsVar *bv = &ps->v.data[ps_at(ps, n.a)->k];
        char nm[48];
        PsT c;
        if (!pos) return t;
        snprintf(nm, sizeof nm, "sk%u_%u_%u", sk->next++, (unsigned)bv->sort, (unsigned)bv->w);
        c = ps_var(ps, nm, bv->sort, bv->w);
        vec_push(&sk->v, c);
        return skolem(ps, sk, ps_subst(ps, n.b, n.a, c), pos);
    }
    if (!ps_conn(n.op)) return t;
    a = skolem(ps, sk, n.a, n.op == PS_NOT || n.op == PS_IMP ? !pos : pos);
    b = n.b ? skolem(ps, sk, n.b, pos) : 0;
    return a == n.a && b == n.b ? t : mk(ps, n.op, n.sort, n.w, a, b, 0, 0);
}

/* pass 2: the quantifiers that remain are assumed, whatever their place */
static PsT weaken(Ps *ps, PsSk *sk, PsT t, bool pos)
{
    PsNode n = *ps_at(ps, t);
    PsT a, b, r = 0;
    size_t i;
    if (n.op == PS_FORALL) {
        if (pos) { sk->bad = true; return t; }     /* a quantifier to prove inside an assumed one: no tier */
        for (i = 0; i < sk->v.len; i++) {
            PsT inst = weaken(ps, sk, ps_subst(ps, n.b, n.a, sk->v.data[i]), pos);
            r = r ? ps_op(ps, PS_AND, r, inst) : inst;
        }
        return r ? r : ps_true(ps);
    }
    if (!ps_conn(n.op)) return t;
    a = weaken(ps, sk, n.a, n.op == PS_NOT || n.op == PS_IMP ? !pos : pos);
    b = n.b ? weaken(ps, sk, n.b, pos) : 0;
    return a == n.a && b == n.b ? t : mk(ps, n.op, n.sort, n.w, a, b, 0, 0);
}

static int valid_q(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit);

static int valid_inst(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit)
{
    PsSk sk = {{0}, 0, false};
    PsVec h = {0};
    size_t i;
    int r;
    goal = weaken(ps, &sk, skolem(ps, &sk, goal, true), true);
    for (i = 0; i < n; i++) vec_push(&h, weaken(ps, &sk, hyps[i], false));
    r = sk.bad ? PS_UNKNOWN : valid_q(s, ps, h.data, h.len, goal, rlimit);
    vec_free(&h); vec_free(&sk.v);
    return r;
}

/* Does t need more than quantifier-free arrays of bit-vectors: a quantifier, or an Int term? */
static bool beyond_abv(const Ps *ps, PsT t)
{
    const PsNode *n = ps_at(ps, t);
    if (n->op == PS_FORALL || n->sort == PS_INT) return true;
    return (n->a && beyond_abv(ps, n->a)) || (n->b && beyond_abv(ps, n->b)) || (n->c && beyond_abv(ps, n->c));
}

static int valid_q(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit)
{
    StrBuf q = {0};
    bool full;
    const char *a;
    size_t i;
    int r;
    /* Every query starts from a reset: z3's incremental mode (after a push) answers `unknown` on quantified array
     * queries that a fresh state proves (checked on the int copy loop's "preserved" goal), and the variable numbers
     * of another DAG mean other things. */
    /* The logic picks z3's solver: QF_ABV answered the instantiated copy-loop goal in 0.09 s where ALL took 16 s. */
    full = beyond_abv(ps, goal);
    for (i = 0; i < n && !full; i++) full = beyond_abv(ps, hyps[i]);
    a = sx_raw(s->sx, full ? "(reset)\n(set-option :produce-models true)\n(set-logic ALL)\n"
                           : "(reset)\n(set-option :produce-models true)\n(set-logic QF_ABV)\n");
    if (!a || strstr(a, "(error")) return PS_ERROR;
    s->ps = ps;
    s->declared.len = 0;
    for (i = 0; i < n; i++) mark_bound(s, ps, hyps[i]);
    mark_bound(s, ps, goal);
    for (i = 0; i < n; i++) declare(s, ps, hyps[i], &q);
    declare(s, ps, goal, &q);
    sb_printf(&q, "(set-option :rlimit %lu)\n", rlimit);
    for (i = 0; i < n; i++) {
        sb_puts(&q, "(assert ");
        ps_print(ps, hyps[i], &q);
        sb_puts(&q, ")\n");
    }
    sb_puts(&q, "(assert (not ");
    ps_print(ps, goal, &q);
    sb_puts(&q, "))\n(check-sat)\n");
    a = sx_raw(s->sx, sb_cstr(&q));
    sb_free(&q);
    if (!a || strstr(a, "(error")) return PS_ERROR;
    while (*a == ' ' || *a == '\n' || *a == '\r') a++;
    r = !strncmp(a, "unsat", 5) ? PS_VALID : !strncmp(a, "sat", 3) ? PS_INVALID : !strncmp(a, "unknown", 7) ? PS_UNKNOWN : PS_ERROR;
    return r;
}

int ps_valid(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit)
{
    bool q = ps_has(ps, goal, PS_FORALL);
    size_t i;
    for (i = 0; i < n && !q; i++) q = ps_has(ps, hyps[i], PS_FORALL);
    /* The instantiation tier proves or says nothing, and gets a tenth of the budget: the copy loops' goals need
     * between 1M and 3.3M of the 100M (measured), while the nodstlen golden's "preserved" goal ran 25 s to
     * `unknown` at the full budget and the full query proved it in 0.7 s. */
    if (q && valid_inst(s, ps, hyps, n, goal, rlimit / 10) == PS_VALID) return PS_VALID;
    return valid_q(s, ps, hyps, n, goal, rlimit);
}

/* One goal over some hypotheses.  A hypothesis's image is faithful only when its own obligations hold, and proving them
 * from an unfaithful image is circular (x + y <= z with x = y = INT_MAX, z = 0 holds in bit-vectors and is false over
 * Int), so the images join a set S one at a time, each once its obligations follow from the facts and the set so far
 * (a least fixpoint).  A hypothesis that never joins, or cannot be translated, is left out: a weaker assumption, still
 * sound, though an `invalid` then means only "not from S".
 *
 * One PsQ serves a whole goal.  Its hypotheses are a stack (the base ones, then an implication's antecedent while its
 * consequent is proved); the facts, leaf views and obligations only grow, and an obligation proved from a set prefix
 * stays proved while that prefix does, so the base hypotheses' obligations are proved once, not once per conjunct. */
typedef struct PsQ {
    PsSolver *s;
    PsImg c;
    VEC(PsT) set, args;                 /* the joined images; facts + set, as one query's hypotheses */
    VEC(size_t) setk;                   /* per joined image: its hypothesis */
    VEC(PsT) himg;                      /* per hypothesis: its image (0: not translatable) */
    VEC(size_t) o0, o1;                 /* per hypothesis: its obligations are c.obl[o0, o1) */
    VEC(uint8_t) in;                    /* per hypothesis: joined */
    VEC(size_t) proved;                 /* per obligation: 1 + the set size it was proved from (0: not proved) */
    VEC(size_t) failed;                 /* per obligation: 1 + the set size at its last failed attempt (0: none) */
    unsigned long rlimit;
} PsQ;

static void pq_sync(PsQ *q)            /* proved and failed track the obligations, new ones empty */
{
    q->proved.len = q->failed.len = q->c.obl.len < q->proved.len ? q->c.obl.len : q->proved.len;
    while (q->proved.len < q->c.obl.len) { vec_push(&q->proved, 0); vec_push(&q->failed, 0); }
}

static bool pq_hold(PsQ *q, size_t k)
{
    size_t i;
    if (q->proved.data[k]) return true;
    if (q->failed.data[k] == q->set.len + 1) return false;
    q->args.len = 0;
    for (i = 0; i < q->c.facts.len; i++) vec_push(&q->args, q->c.facts.data[i]);
    for (i = 0; i < q->set.len; i++) vec_push(&q->args, q->set.data[i]);
    if (ps_valid(q->s, q->c.ps, q->args.data, q->args.len, q->c.obl.data[k], q->rlimit) == PS_VALID) q->proved.data[k] = q->set.len + 1;
    else q->failed.data[k] = q->set.len + 1;
    return q->proved.data[k] != 0;
}

static void pq_push(PsQ *q, PsT hyp)
{
    size_t m = q->c.obl.len;
    PsT h = img_fm(&q->c, hyp);
    if (!h) q->c.obl.len = m;
    pq_sync(q);
    vec_push(&q->himg, h); vec_push(&q->o0, m); vec_push(&q->o1, q->c.obl.len); vec_push(&q->in, 0);
}

static void pq_close(PsQ *q)           /* the least fixpoint over the current hypotheses */
{
    bool changed, ok;
    size_t i, k;
    do {
        changed = false;
        for (i = 0; i < q->himg.len; i++) {
            if (!q->himg.data[i] || q->in.data[i]) continue;
            for (ok = true, k = q->o0.data[i]; k < q->o1.data[i] && ok; k++) ok = pq_hold(q, k);
            if (ok) { q->in.data[i] = 1; vec_push(&q->set, q->himg.data[i]); vec_push(&q->setk, i); changed = true; }
        }
    } while (changed);
}

/* back to nh hypotheses and a set of ns: what joined because of the removed ones leaves, and so do the proofs from it */
static void pq_pop(PsQ *q, size_t nh, size_t ns)
{
    size_t k;
    for (k = ns; k < q->set.len; k++) if (q->setk.data[k] < nh) q->in.data[q->setk.data[k]] = 0;
    q->himg.len = q->o0.len = q->o1.len = q->in.len = nh;
    q->set.len = q->setk.len = ns;
    for (k = 0; k < q->proved.len; k++) {
        if (q->proved.data[k] > ns + 1) q->proved.data[k] = 0;
        if (q->failed.data[k] > ns + 1) q->failed.data[k] = 0;
    }
}

static void flatten_and(const Ps *ps, PsT t, PsVec *out)
{
    const PsNode *n = ps_at(ps, t);
    if (n->op == PS_AND) { PsT a = n->a, b = n->b; flatten_and(ps, a, out); flatten_and(ps, b, out); }
    else vec_push(out, t);
}

/* the goal's conjuncts one by one; the antecedent of an implication (the path condition) is more hypotheses */
static int valid_int_goal(PsQ *q, PsT goal)
{
    PsNode n = *ps_at(q->c.ps, goal);
    size_t i, m;
    int r;
    PsT g;
    if (n.op == PS_AND) {
        r = valid_int_goal(q, n.a);
        return r != PS_VALID ? r : valid_int_goal(q, n.b);
    }
    if (n.op == PS_IMP) {
        PsVec ante = {0};
        size_t nh = q->himg.len, ns = q->set.len;
        flatten_and(q->c.ps, n.a, &ante);
        for (i = 0; i < ante.len; i++) pq_push(q, ante.data[i]);
        vec_free(&ante);
        pq_close(q);
        r = valid_int_goal(q, n.b);
        pq_pop(q, nh, ns);
        return r;
    }
    m = q->c.obl.len;
    g = img_fm(&q->c, goal);
    if (!g) q->c.obl.len = m;
    pq_sync(q);
    if (!g) return PS_UNKNOWN;
    for (i = m; i < q->c.obl.len; i++)
        if (!pq_hold(q, i)) return PS_UNKNOWN;
    q->args.len = 0;
    for (i = 0; i < q->c.facts.len; i++) vec_push(&q->args, q->c.facts.data[i]);
    for (i = 0; i < q->set.len; i++) vec_push(&q->args, q->set.data[i]);
    return ps_valid(q->s, q->c.ps, q->args.data, q->args.len, g, q->rlimit);
}

int ps_valid_int(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit)
{
    PsQ q = {0};
    PsVec h = {0};
    size_t i;
    int r;
    q.s = s; q.c.ps = ps; q.rlimit = rlimit;
    for (i = 0; i < n; i++) flatten_and(ps, hyps[i], &h);
    for (i = 0; i < h.len; i++) pq_push(&q, h.data[i]);
    pq_close(&q);
    r = valid_int_goal(&q, goal);
    vec_free(&h);
    vec_free(&q.c.facts); vec_free(&q.c.obl); vec_free(&q.c.lv); vec_free(&q.set); vec_free(&q.args); vec_free(&q.setk);
    vec_free(&q.himg); vec_free(&q.o0); vec_free(&q.o1); vec_free(&q.in); vec_free(&q.proved); vec_free(&q.failed);
    return r;
}

/* Does t multiply two things neither of which is a constant?  Those are the goals bit-vectors cannot answer (S8). */
static bool var_mul(const Ps *ps, PsT t)
{
    const PsNode *n = ps_at(ps, t);
    if (n->op == PS_BVMUL && ps_at(ps, n->a)->op != PS_BVC && ps_at(ps, n->b)->op != PS_BVC) return true;
    return (n->a && var_mul(ps, n->a)) || (n->b && var_mul(ps, n->b)) || (n->c && var_mul(ps, n->c));
}

int ps_prove(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit)
{
    bool mul = var_mul(ps, goal);
    size_t i;
    int r;
    for (i = 0; i < n && !mul; i++) mul = var_mul(ps, hyps[i]);
    /* Over a multiply the bit-vector query spends its whole budget to no answer, so the Int tier goes first. */
    if (mul && ps_valid_int(s, ps, hyps, n, goal, rlimit) == PS_VALID) return PS_VALID;
    r = ps_valid(s, ps, hyps, n, goal, rlimit);
    /* Only a proof is taken from the Int tier: with a hypothesis left out, its `sat` proves nothing. */
    if (!mul && r == PS_UNKNOWN && ps_valid_int(s, ps, hyps, n, goal, rlimit) == PS_VALID) return PS_VALID;
    return r;
}

/* ---- self-test ---- */

static void expect(int *failures, const char *what, int got, int want)
{
    if (got != want) { fprintf(stderr, "prove selftest: %s: got %d, want %d\n", what, got, want); (*failures)++; }
}

static PsT bv(Ps *ps, const char *nm) { return ps_var(ps, nm, PS_BV, 64); }
static PsT ult(Ps *ps, PsT a, PsT b) { return ps_op(ps, PS_BVULT, a, b); }
static PsT ule(Ps *ps, PsT a, PsT b) { return ps_op(ps, PS_BVULE, a, b); }

int prove_selftest(void)
{
    int failures = 0;
    Ps ps = {0};
    char err[200];
    StrBuf sb = {0};
    PsSolver *s = ps_open("z3 -in -smt2", err, sizeof err);
    PsT n, szD, i, j, r, c, k, d, src, zero, h[6], x, one, lim;
    if (!s) { fprintf(stderr, "prove selftest: %s\n", err); return 1; }
    n = bv(&ps, "n"); szD = bv(&ps, "szD"); i = bv(&ps, "i"); j = bv(&ps, "j");
    r = bv(&ps, "r"); c = bv(&ps, "c"); x = bv(&ps, "x");
    d = ps_var(&ps, "D", PS_ARR, 8); src = ps_var(&ps, "S", PS_ARR, 8);
    zero = ps_bvc(&ps, 64, 0); one = ps_bvc(&ps, 64, 1); lim = ps_bvc(&ps, 64, 2147483648u);

    /* substitution */
    ps_print(&ps, ps_subst(&ps, ps_op(&ps, PS_BVADD, x, i), x, ps_bvc(&ps, 64, 3)), &sb);
    if (strcmp(sb_cstr(&sb), "(bvadd (_ bv3 64) i)")) { fprintf(stderr, "prove selftest: subst: %s\n", sb_cstr(&sb)); failures++; }
    sb_free(&sb);

    /* value ranges: a narrow variable's width, signed or unsigned; a product of two int-ranged values fits, a third factor
     * or a 64-bit leaf does not */
    {
        PsT r32 = ps_var(&ps, "r32", PS_BV, 32), c32 = ps_var(&ps, "c32", PS_BV, 32), p;
        int64_t lo, hi;
        p = ps_op(&ps, PS_BVMUL, ps_resize(&ps, r32, 64, true), ps_resize(&ps, c32, 64, true));
        if (!ps_range(&ps, p, &lo, &hi) || lo != -2147483647LL * 2147483648LL || hi != 2147483648LL * 2147483648LL)
            { fprintf(stderr, "prove selftest: range of r*c: %d [%lld, %lld]\n", 0, (long long)lo, (long long)hi); failures++; }
        if (!ps_range(&ps, ps_op(&ps, PS_BVADD, p, one), &lo, &hi) || hi != 2147483648LL * 2147483648LL + 1)
            { fprintf(stderr, "prove selftest: range of r*c+1\n"); failures++; }
        if (ps_range(&ps, ps_op(&ps, PS_BVMUL, p, ps_resize(&ps, c32, 64, true)), &lo, &hi))
            { fprintf(stderr, "prove selftest: r*c*c fits\n"); failures++; }
        if (ps_range(&ps, n, &lo, &hi))
            { fprintf(stderr, "prove selftest: a 64-bit leaf has a range\n"); failures++; }
        if (!ps_range(&ps, ps_resize(&ps, r32, 64, false), &lo, &hi) || lo != 0 || hi != 4294967295LL)
            { fprintf(stderr, "prove selftest: range of zext\n"); failures++; }
    }

    /* the copy loop's write bound, and it with the size fact dropped */
    h[0] = ule(&ps, n, szD); h[1] = ult(&ps, i, n);
    expect(&failures, "copy bound", ps_valid(s, &ps, h, 2, ult(&ps, i, szD), 0), PS_VALID);
    expect(&failures, "copy bound without n <= szD", ps_valid(s, &ps, h + 1, 1, ult(&ps, i, szD), 0), PS_INVALID);

    /* the nested loop's write bound: nonlinear as bit-vectors, instant through the Int tier (S7) */
    h[0] = ule(&ps, r, lim); h[1] = ule(&ps, c, lim); h[2] = ult(&ps, i, r); h[3] = ult(&ps, j, c);
    h[4] = ule(&ps, ps_op(&ps, PS_BVMUL, r, c), szD);
    k = ult(&ps, ps_op(&ps, PS_BVADD, ps_op(&ps, PS_BVMUL, i, c), j), szD);
    expect(&failures, "fill2 bound by Int", ps_valid_int(s, &ps, h, 5, k, 10000000), PS_VALID);
    expect(&failures, "fill2 bound without r*c <= szD", ps_valid_int(s, &ps, h, 4, k, 10000000), PS_INVALID);
    expect(&failures, "fill2 bound without r <= 2^31", ps_valid_int(s, &ps, h + 1, 4, k, 10000000), PS_UNKNOWN);   /* the no-wrap proof fails */

    /* narrow images (NARROW_INT_PLAN T2): int-wide indices, spec products, views and the faithful set */
    {
        PsT i3 = ps_var(&ps, "i3", PS_BV, 32), j3 = ps_var(&ps, "j3", PS_BV, 32), r3 = ps_var(&ps, "r3", PS_BV, 32),
            c3 = ps_var(&ps, "c3", PS_BV, 32), a8 = ps_var(&ps, "a8", PS_BV, 8), u3 = ps_var(&ps, "u3", PS_BV, 32),
            v3 = ps_var(&ps, "v3", PS_BV, 32), x3 = ps_var(&ps, "x3", PS_BV, 32), y3 = ps_var(&ps, "y3", PS_BV, 32),
            z3 = ps_var(&ps, "z3", PS_BV, 32), z32 = ps_bvc(&ps, 32, 0), p, idx, g, hh[8];
#define SX(t) ps_resize(&ps, (t), 64, true)
        /* the nested loop over int: 0 <= i < r, 0 <= j < c, r * c <= szD (spec product, 64-bit) */
        hh[0] = ps_op(&ps, PS_BVSLE, z32, i3); hh[1] = ps_op(&ps, PS_BVSLT, i3, r3);
        hh[2] = ps_op(&ps, PS_BVSLE, z32, j3); hh[3] = ps_op(&ps, PS_BVSLT, j3, c3);
        hh[4] = ule(&ps, ps_op(&ps, PS_BVMUL, SX(r3), SX(c3)), szD);
        p = ps_op(&ps, PS_BVADD, ps_op(&ps, PS_BVMUL, SX(i3), SX(c3)), SX(j3));
        expect(&failures, "int fill2, spec arithmetic", ps_valid_int(s, &ps, hh, 5, ult(&ps, p, szD), 10000000), PS_VALID);
        expect(&failures, "int fill2 without r*c <= szD", ps_valid_int(s, &ps, hh, 4, ult(&ps, p, szD), 10000000), PS_INVALID);
        /* the same with int arithmetic in the code: the 32-bit product and sum must not wrap, which needs szD <= INT_MAX */
        hh[5] = ule(&ps, szD, ps_bvc(&ps, 64, 2147483647u));
        idx = ps_op(&ps, PS_BVADD, ps_op(&ps, PS_BVMUL, i3, c3), j3);
        g = ult(&ps, ps_op(&ps, PS_BVADD, zero, SX(idx)), szD);
        expect(&failures, "int fill2, 32-bit index", ps_valid_int(s, &ps, hh, 6, g, 10000000), PS_VALID);
        hh[4] = hh[5];
        expect(&failures, "int fill2 index without r*c <= szD", ps_valid_int(s, &ps, hh, 5, g, 10000000), PS_UNKNOWN);
        hh[4] = ule(&ps, ps_op(&ps, PS_BVMUL, SX(r3), SX(c3)), szD);
        expect(&failures, "int fill2 index without szD <= INT_MAX", ps_valid_int(s, &ps, hh, 5, g, 10000000), PS_UNKNOWN);
        /* views: a signed byte is below 128 signed, not unsigned */
        expect(&failures, "sext a8 <s 128", ps_valid_int(s, &ps, NULL, 0,
               ps_op(&ps, PS_BVSLT, ps_resize(&ps, a8, 32, true), ps_bvc(&ps, 32, 128)), 10000000), PS_VALID);
        expect(&failures, "sext a8 <u 128", ps_valid_int(s, &ps, NULL, 0,
               ult(&ps, ps_resize(&ps, a8, 32, true), ps_bvc(&ps, 32, 128)), 10000000), PS_INVALID);
        /* a sum in the unsigned view read signed */
        hh[0] = ult(&ps, u3, ps_bvc(&ps, 32, 1u << 30)); hh[1] = ult(&ps, v3, ps_bvc(&ps, 32, 1u << 30));
        expect(&failures, "sext of an unsigned sum", ps_valid_int(s, &ps, hh, 2,
               ps_op(&ps, PS_BVSLT, SX(ps_op(&ps, PS_BVADD, u3, v3)), ps_bvc(&ps, 64, 1u << 31)), 10000000), PS_VALID);
        /* the index shape the access goals have: 0 + sext idx below a length */
        hh[0] = ps_op(&ps, PS_BVSLE, z32, i3); hh[1] = ps_op(&ps, PS_BVSLT, i3, r3); hh[2] = ule(&ps, SX(r3), szD);
        expect(&failures, "0 + sext i <u len", ps_valid_int(s, &ps, hh, 3, ult(&ps, ps_op(&ps, PS_BVADD, zero, SX(i3)), szD), 10000000), PS_VALID);
        /* an unsigned difference: n - i <= n holds when i <= n */
        expect(&failures, "n - i <=u n with i <=u n", ps_valid_int(s, &ps, (hh[0] = ule(&ps, i, n), hh), 1,
               ule(&ps, ps_op(&ps, PS_BVSUB, n, i), n), 10000000), PS_VALID);
        /* a hypothesis whose own no-wrap proof would need it: x + y <= z with x = y = INT_MAX and z = 0 holds in
         * bit-vectors (the sum wraps negative) and is false over Int; assuming it would prove anything */
        hh[0] = ps_op(&ps, PS_BVSLE, ps_bvc(&ps, 32, 0x7fffffffu), x3); hh[1] = ps_op(&ps, PS_BVSLE, ps_bvc(&ps, 32, 0x7fffffffu), y3);
        hh[2] = ps_op(&ps, PS_BVSLE, z3, z32); hh[3] = ps_op(&ps, PS_BVSLE, ps_op(&ps, PS_BVADD, x3, y3), z3);
        expect(&failures, "circular no-wrap", ps_valid_int(s, &ps, hh, 4, ult(&ps, x3, x3), 10000000), PS_INVALID);
        /* the policy: a proof from the Int tier is taken, its other answers are not.  Here the Int tier drops the wrapping
         * hypothesis and cannot prove the goal; bit-vectors prove it from that hypothesis. */
        g = ps_op(&ps, PS_BVSLE, ps_op(&ps, PS_BVADD, x3, y3), z32);
        expect(&failures, "bit-vector proof kept", ps_prove(s, &ps, hh, 4, g, 10000000), PS_VALID);
        expect(&failures, "Int tier alone has no proof there", ps_valid_int(s, &ps, hh, 4, g, 10000000), PS_UNKNOWN);
        /* and the Int tier's `sat` is ignored: the goal x3 <u x3 is false in bit-vectors too, so INVALID stands */
        expect(&failures, "bit-vector answer kept", ps_prove(s, &ps, hh, 4, ult(&ps, x3, x3), 10000000), PS_INVALID);
        /* over a product the Int tier goes first: this goal has no bit-vector answer in the budget */
        hh[0] = ps_op(&ps, PS_BVSLE, z32, i3); hh[1] = ps_op(&ps, PS_BVSLT, i3, r3);
        hh[2] = ps_op(&ps, PS_BVSLE, z32, j3); hh[3] = ps_op(&ps, PS_BVSLT, j3, c3);
        hh[4] = ule(&ps, ps_op(&ps, PS_BVMUL, SX(r3), SX(c3)), szD);
        expect(&failures, "product goal through ps_prove", ps_prove(s, &ps, hh, 5,
               ult(&ps, ps_op(&ps, PS_BVADD, ps_op(&ps, PS_BVMUL, SX(i3), SX(c3)), SX(j3)), szD), 10000000), PS_VALID);
        /* review gaps (T2-T3 review, 2026-11-22): views, constants, the skip rule, goal shapes, the fixpoint order */
        {
            PsT m3 = ps_var(&ps, "m3", PS_BV, 32), q3 = ps_var(&ps, "q3", PS_BV, 32), ci, c64 = ps_bvc(&ps, 64, UINT64_C(1) << 63),
                bnd[5];
            /* a name the tier makes (q3.i) already taken by a bit-vector: the tier does not apply */
            (void)ps_var(&ps, "q3.i", PS_BV, 32);
            expect(&failures, "name clash with an image", ps_valid_int(s, &ps, NULL, 0, ule(&ps, q3, q3), 10000000), PS_UNKNOWN);
            /* a negative constant in the signed view: -2 < m3 < 0 means m3 = -1 */
            hh[0] = ps_op(&ps, PS_BVSLT, ps_bvc(&ps, 32, 0xfffffffeu), m3); hh[1] = ps_op(&ps, PS_BVSLT, m3, z32);
            expect(&failures, "m3 == -1", ps_valid_int(s, &ps, hh, 2, ps_op(&ps, PS_EQ, m3, ps_bvc(&ps, 32, 0xffffffffu)), 10000000), PS_VALID);
            expect(&failures, "m3 == 1 is not", ps_valid_int(s, &ps, hh, 2, ps_op(&ps, PS_EQ, m3, ps_bvc(&ps, 32, 1)), 10000000), PS_INVALID);
            expect(&failures, "m3 + 1 == 0", ps_valid_int(s, &ps, hh, 2, ps_op(&ps, PS_EQ, ps_op(&ps, PS_BVADD, m3, ps_bvc(&ps, 32, 1)), z32), 10000000), PS_VALID);
            /* a 64-bit constant above INT64_MAX: signed it is the most negative, and 2^64 - 1 + 1 wraps */
            expect(&failures, "2^63 <s 0", ps_valid_int(s, &ps, NULL, 0, ps_op(&ps, PS_BVSLT, c64, zero), 10000000), PS_VALID);
            expect(&failures, "max + 1 wraps", ps_valid_int(s, &ps, NULL, 0,
                   ps_op(&ps, PS_EQ, ps_op(&ps, PS_BVADD, ps_bvc(&ps, 64, UINT64_MAX), one), zero), 10000000), PS_UNKNOWN);
            /* the skip rule: a signed 64-bit product of three sign-extended ints can wrap, so its range is owed */
            hh[0] = ps_op(&ps, PS_BVSLE, z32, i3); hh[1] = ps_op(&ps, PS_BVSLE, z32, j3); hh[2] = ps_op(&ps, PS_BVSLE, z32, r3);
            ci = ps_op(&ps, PS_BVMUL, ps_op(&ps, PS_BVMUL, SX(i3), SX(j3)), SX(r3));
            expect(&failures, "triple product is not exact", ps_valid_int(s, &ps, hh, 3, ps_op(&ps, PS_BVSLE, zero, ci), 10000000), PS_UNKNOWN);
            /* subtraction: signed, with both operands non-negative, stays in range; unsigned, it can go below zero */
            hh[0] = ps_op(&ps, PS_BVSLE, z32, i3); hh[1] = ps_op(&ps, PS_BVSLE, i3, r3);
            expect(&failures, "0 <=s r - i", ps_valid_int(s, &ps, hh, 2, ps_op(&ps, PS_BVSLE, z32, ps_op(&ps, PS_BVSUB, r3, i3)), 10000000), PS_VALID);
            expect(&failures, "u - v <=u u unconditionally", ps_valid_int(s, &ps, NULL, 0, ule(&ps, ps_op(&ps, PS_BVSUB, u3, v3), u3), 10000000), PS_UNKNOWN);
            /* zero-extension of a leaf first read signed: x3 <s 0 means zext x3 >= 2^31 */
            hh[0] = ps_op(&ps, PS_BVSLT, x3, z32);
            expect(&failures, "zext of a negative", ps_valid_int(s, &ps, hh, 1,
                   ule(&ps, ps_bvc(&ps, 64, 0x80000000u), ps_resize(&ps, x3, 64, false)), 10000000), PS_VALID);
            /* nested implications and a conjunct that holds only under its own antecedent */
            hh[0] = ps_op(&ps, PS_BVSLT, i3, r3); hh[1] = ps_op(&ps, PS_BVSLE, z32, i3);
            g = ult(&ps, SX(i3), SX(r3));
            expect(&failures, "nested implications", ps_valid_int(s, &ps, NULL, 0,
                   ps_op(&ps, PS_IMP, hh[0], ps_op(&ps, PS_IMP, hh[1], g)), 10000000), PS_VALID);
            expect(&failures, "an antecedent does not leak to its sibling", ps_valid_int(s, &ps, NULL, 0,
                   ps_op(&ps, PS_AND, ps_op(&ps, PS_IMP, hh[1], ps_op(&ps, PS_BVSLE, zero, SX(i3))),
                         ps_op(&ps, PS_BVSLE, zero, SX(i3))), 10000000), PS_INVALID);
            /* the fixpoint in reverse order: the sum's hypothesis is listed before the bounds that make it faithful */
            bnd[0] = ps_op(&ps, PS_BVSLE, ps_op(&ps, PS_BVADD, x3, y3), z32);
            bnd[1] = ps_op(&ps, PS_BVSLE, x3, ps_bvc(&ps, 32, 1000)); bnd[2] = ps_op(&ps, PS_BVSLE, y3, ps_bvc(&ps, 32, 1000));
            bnd[3] = ps_op(&ps, PS_BVSLE, ps_bvc(&ps, 32, 0xfffffc18u), x3); bnd[4] = ps_op(&ps, PS_BVSLE, ps_bvc(&ps, 32, 0xfffffc18u), y3);
            expect(&failures, "fixpoint, hypothesis first", ps_valid_int(s, &ps, bnd, 5,
                   ps_op(&ps, PS_BVSLT, ps_op(&ps, PS_BVADD, x3, y3), ps_bvc(&ps, 32, 1)), 10000000), PS_VALID);
            /* what joins the set because of an antecedent leaves with it: the sum's hypothesis is faithful only under the
             * bounds, so the sibling conjunct, proved without them, must not see it */
            {
                PsT bb = ps_op(&ps, PS_AND, ps_op(&ps, PS_AND, bnd[1], bnd[2]), ps_op(&ps, PS_AND, bnd[3], bnd[4]));
                PsT sum1 = ps_op(&ps, PS_BVSLT, ps_op(&ps, PS_BVADD, x3, y3), ps_bvc(&ps, 32, 1));
                expect(&failures, "a joined hypothesis leaves with its antecedent", ps_valid_int(s, &ps, bnd, 1,
                       ps_op(&ps, PS_AND, ps_op(&ps, PS_IMP, bb, sum1), sum1), 10000000), PS_UNKNOWN);
                expect(&failures, "and joins again under the next antecedent", ps_valid_int(s, &ps, bnd, 1,
                       ps_op(&ps, PS_AND, ps_op(&ps, PS_IMP, bb, sum1), ps_op(&ps, PS_IMP, bb, sum1)), 10000000), PS_VALID);
                /* a proof of the sum's no-wrap made under the bounds is not a proof without them: x + y <= 0 with y >= 0
                 * gives x <= 0 over Int only */
                PsT hs[2], xz = ps_op(&ps, PS_BVSLE, x3, z32);
                hs[0] = bnd[0]; hs[1] = ps_op(&ps, PS_BVSLE, z32, y3);
                expect(&failures, "a proof from the antecedent's set leaves with it", ps_valid_int(s, &ps, hs, 2,
                       ps_op(&ps, PS_AND, ps_op(&ps, PS_IMP, bb, xz), ps_op(&ps, PS_IMP, ps_op(&ps, PS_BVSLE, z3, z3), xz)), 10000000), PS_INVALID);
                expect(&failures, "x <= 0 under the bounds", ps_valid_int(s, &ps, hs, 2, ps_op(&ps, PS_IMP, bb, xz), 10000000), PS_VALID);
                expect(&failures, "and joins under it", ps_valid_int(s, &ps, bnd, 1, ps_op(&ps, PS_IMP, bb, sum1), 10000000), PS_VALID);
            }
            /* the retry: no multiply, so bit-vectors go first, and at a budget (z3's deterministic rlimit, measured 2026-11-22:
             * `unknown` for bit-vectors from 100 to 10000, proved from 30000; the Int tier proves from 3000) they give no
             * answer while the Int tier proves it, and ps_prove takes the proof */
            hh[0] = ps_op(&ps, PS_BVSLE, z32, i3); hh[1] = ps_op(&ps, PS_BVSLT, i3, r3); hh[2] = ule(&ps, SX(r3), szD);
            g = ult(&ps, ps_op(&ps, PS_BVADD, zero, SX(i3)), szD);
            expect(&failures, "retry: bit-vectors alone say unknown", ps_valid(s, &ps, hh, 3, g, 6000), PS_UNKNOWN);
            expect(&failures, "retry: unknown becomes valid", ps_prove(s, &ps, hh, 3, g, 6000), PS_VALID);
            /* truncation: the tier does not apply */
            expect(&failures, "truncation", ps_valid_int(s, &ps, NULL, 0, ult(&ps, ps_resize(&ps, x, 32, false), ps_bvc(&ps, 32, 5)), 10000000), PS_UNKNOWN);
        }
#undef SX
    }

    /* wrap: x < x + 1 is false at the top, and the Int tier must not say otherwise */
    k = ult(&ps, x, ps_op(&ps, PS_BVADD, x, one));
    expect(&failures, "x < x + 1 in bit-vectors", ps_valid(s, &ps, NULL, 0, k, 0), PS_INVALID);
    expect(&failures, "x < x + 1 by Int", ps_valid_int(s, &ps, NULL, 0, k, 10000000), PS_UNKNOWN);

    /* arrays and a quantifier: copied below i, so equal at 0 if 0 < i */
    {
        PsT kv = ps_var(&ps, "kq", PS_BV, 64);
        PsT eq = ps_op(&ps, PS_EQ, ps_op(&ps, PS_SEL, d, kv), ps_op(&ps, PS_SEL, src, kv));
        PsT all = ps_forall(&ps, kv, ps_op(&ps, PS_IMP, ult(&ps, kv, i), eq));
        PsT g0 = ps_op(&ps, PS_EQ, ps_op(&ps, PS_SEL, d, zero), ps_op(&ps, PS_SEL, src, zero));
        h[0] = all; h[1] = ult(&ps, zero, i);
        expect(&failures, "forall instance", ps_valid(s, &ps, h, 2, g0, 0), PS_VALID);
        expect(&failures, "forall without 0 < i", ps_valid(s, &ps, h, 1, g0, 0), PS_INVALID);
        expect(&failures, "store then select", ps_valid(s, &ps, NULL, 0,
               ps_op(&ps, PS_EQ, ps_op(&ps, PS_SEL, ps_store(&ps, d, zero, ps_bvc(&ps, 8, 7)), zero), ps_bvc(&ps, 8, 7)), 0), PS_VALID);
        /* the instantiation tier: a quantifier to prove is proved at a fresh constant (a smaller bound follows, a
         * larger one does not, and one more copied element does), by the fallback when the tier says nothing */
        {
            PsT jb = ps_var(&ps, "jq", PS_BV, 64);
            PsT gj = ps_forall(&ps, kv, ps_op(&ps, PS_IMP, ult(&ps, kv, jb), eq));
            PsT step = ps_op(&ps, PS_EQ, ps_op(&ps, PS_SEL, ps_store(&ps, d, i, ps_op(&ps, PS_SEL, src, i)), kv), ps_op(&ps, PS_SEL, src, kv));
            PsT gi1 = ps_forall(&ps, kv, ps_op(&ps, PS_IMP, ult(&ps, kv, ps_op(&ps, PS_BVADD, i, one)), step));
            h[0] = all; h[1] = ps_op(&ps, PS_BVULE, jb, i);
            expect(&failures, "forall goal, smaller bound", ps_valid(s, &ps, h, 2, gj, 0), PS_VALID);
            expect(&failures, "forall goal, larger bound", ps_valid(s, &ps, h, 1, gj, 0), PS_INVALID);
            expect(&failures, "forall goal, one more copied", ps_valid(s, &ps, h, 1, gi1, 0), PS_VALID);
            expect(&failures, "forall goal, two more", ps_valid(s, &ps, h, 1,
                   ps_forall(&ps, kv, ps_op(&ps, PS_IMP, ult(&ps, kv, ps_op(&ps, PS_BVADD, i, ps_bvc(&ps, 64, 2))), step)), 0), PS_INVALID);
        }
    }
    /* narrow widths, subtraction, signed order and resize (docs/PROVE_OBLIGATIONS_DESIGN.md section 3) */
    {
        PsT u = ps_var(&ps, "u32", PS_BV, 32), v = ps_var(&ps, "v32", PS_BV, 32), b8 = ps_var(&ps, "b8", PS_BV, 8);
        PsT z32 = ps_bvc(&ps, 32, 0), max32 = ps_bvc(&ps, 32, 0xffffffffu), one32 = ps_bvc(&ps, 32, 1), c80 = ps_bvc(&ps, 8, 0x80);
        PsT sx = ps_resize(&ps, c80, 32, true), zx = ps_resize(&ps, c80, 32, false);
        PsT a16 = ps_var(&ps, "A16", PS_ARR, 16), a32 = ps_var(&ps, "A32", PS_ARR, 32);
        PsT st;
        /* a 32-bit add wraps; with the top value excluded it does not */
        k = ult(&ps, u, ps_op(&ps, PS_BVADD, u, one32));
        expect(&failures, "32-bit u < u + 1", ps_valid(s, &ps, NULL, 0, k, 0), PS_INVALID);
        h[0] = ps_op(&ps, PS_NOT, ps_op(&ps, PS_EQ, u, max32), 0);
        expect(&failures, "32-bit u < u + 1 below the top", ps_valid(s, &ps, h, 1, k, 0), PS_VALID);
        expect(&failures, "(u - v) + v == u", ps_valid(s, &ps, NULL, 0,
               ps_op(&ps, PS_EQ, ps_op(&ps, PS_BVADD, ps_op(&ps, PS_BVSUB, u, v), v), u), 0), PS_VALID);
        /* signed order is not unsigned order (a signed compare read as unsigned is the mutant) */
        expect(&failures, "u <s 0 is satisfiable", ps_valid(s, &ps, NULL, 0, ps_op(&ps, PS_NOT, ps_op(&ps, PS_BVSLT, u, z32), 0), 0), PS_INVALID);
        expect(&failures, "u <u 0 is never", ps_valid(s, &ps, NULL, 0, ps_op(&ps, PS_NOT, ult(&ps, u, z32), 0), 0), PS_VALID);
        h[0] = ps_op(&ps, PS_BVSLT, u, z32);
        expect(&failures, "u <s 0 means u >u 2^31 - 1", ps_valid(s, &ps, h, 1, ult(&ps, ps_bvc(&ps, 32, 0x7fffffff), u), 0), PS_VALID);
        expect(&failures, "u <=s u", ps_valid(s, &ps, NULL, 0, ps_op(&ps, PS_BVSLE, u, u), 0), PS_VALID);
        /* sign-extension: 0x80 as 8 bits is -128, and zero-extension must differ from it */
        expect(&failures, "sext 0x80 == 0xffffff80", ps_valid(s, &ps, NULL, 0, ps_op(&ps, PS_EQ, sx, ps_bvc(&ps, 32, 0xffffff80u)), 0), PS_VALID);
        expect(&failures, "zext 0x80 == 0x80", ps_valid(s, &ps, NULL, 0, ps_op(&ps, PS_EQ, zx, ps_bvc(&ps, 32, 0x80)), 0), PS_VALID);
        expect(&failures, "zext 0x80 == sext 0x80", ps_valid(s, &ps, NULL, 0, ps_op(&ps, PS_EQ, zx, sx), 0), PS_INVALID);
        expect(&failures, "trunc 0x1ff to 8 == 0xff", ps_valid(s, &ps, NULL, 0,
               ps_op(&ps, PS_EQ, ps_resize(&ps, ps_bvc(&ps, 32, 0x1ff), 8, false), ps_bvc(&ps, 8, 0xff)), 0), PS_VALID);
        expect(&failures, "sext keeps the sign",  ps_valid(s, &ps, NULL, 0,
               ps_op(&ps, PS_EQ, ps_op(&ps, PS_BVSLT, ps_resize(&ps, b8, 32, true), z32), ps_op(&ps, PS_BVSLT, b8, ps_bvc(&ps, 8, 0))), 0), PS_VALID);
        /* arrays with 16- and 32-bit elements */
        st = ps_store(&ps, a32, zero, ps_bvc(&ps, 32, 7));
        expect(&failures, "32-bit store then select", ps_valid(s, &ps, NULL, 0,
               ps_op(&ps, PS_EQ, ps_op(&ps, PS_SEL, st, zero), ps_bvc(&ps, 32, 7)), 0), PS_VALID);
        expect(&failures, "16-bit select is not 8-bit", ps_valid(s, &ps, NULL, 0,
               ps_op(&ps, PS_EQ, ps_op(&ps, PS_SEL, a16, zero), ps_bvc(&ps, 16, 7)), 0), PS_INVALID);
        /* the Int tier does not apply to the new operations */
        expect(&failures, "Int tier declines bvsub", ps_valid_int(s, &ps, NULL, 0, ule(&ps, ps_op(&ps, PS_BVSUB, n, i), n), 10000000), PS_UNKNOWN);
    }
    ps_close(s);
    ps_free(&ps);
    return failures != 0;
}
