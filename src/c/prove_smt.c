/* prove_smt.c - the prover's term DAG, its rewrites and its solver queries (see prove_smt.h). */
#include "prove_smt.h"

#include <stdio.h>
#include <string.h>

#include "symx.h"

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
    case PS_IADD: case PS_IMUL: sort = PS_INT; break;
    case PS_SEL: sort = PS_BV; w = ps_at(ps, a)->w; break;      /* the array's element width */
    default: break;
    }
    return mk(ps, op, sort, w, a, b, 0, 0);
}

PsT ps_resize(Ps *ps, PsT a, unsigned w, bool sign) { return mk(ps, PS_RESIZE, PS_BV, w, a, 0, 0, w | (sign ? PS_SIGNED : 0)); }
PsT ps_store(Ps *ps, PsT arr, PsT idx, PsT val) { return mk(ps, PS_STORE, PS_ARR, ps_at(ps, arr)->w, arr, idx, val, 0); }
PsT ps_forall(Ps *ps, PsT var, PsT body) { return mk(ps, PS_FORALL, PS_BOOL, 0, var, body, 0, 0); }

static PsT and_all(Ps *ps, const PsT *t, size_t n)
{
    PsT r = 0;
    size_t i;
    for (i = 0; i < n; i++) r = r ? ps_op(ps, PS_AND, r, t[i]) : t[i];
    return r ? r : ps_true(ps);
}

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

/* ---- bit-vectors to Int ---- */

void psint_free(PsInt *o) { vec_free(&o->wrapbv); vec_free(&o->wrapint); vec_free(&o->nonneg); }

PsT ps_to_int(Ps *ps, PsT t, PsInt *obl)
{
    PsNode n = *ps_at(ps, t);
    PsT a, b, r;
    switch (n.op) {
    case PS_TRUE: case PS_FALSE: return t;
    case PS_BVC: return n.w == 64 ? ps_intc(ps, n.k) : 0;
    case PS_VAR:
        if (n.sort == PS_BOOL) return t;
        if (n.sort == PS_BV && n.w == 64) {
            char nm[48];
            PsT x;
            snprintf(nm, sizeof nm, "%s.i", ps->v.data[n.k].name);
            x = ps_var(ps, nm, PS_INT, 0);
            vec_push(&obl->nonneg, ps_op(ps, PS_ILE, ps_intc(ps, 0), x));   /* repeats per use; the solver does not mind */
            return x;
        }
        return 0;
    case PS_BVADD: case PS_BVMUL:
        if (!(a = ps_to_int(ps, n.a, obl)) || !(b = ps_to_int(ps, n.b, obl))) return 0;
        r = ps_op(ps, n.op == PS_BVADD ? PS_IADD : PS_IMUL, a, b);
        if (ps_has(ps, t, PS_BVMUL)) vec_push(&obl->wrapint, ps_op(ps, PS_ILT, r, mk(ps, PS_P64, PS_INT, 0, 0, 0, 0, 0)));
        else vec_push(&obl->wrapbv, ps_op(ps, PS_BVULE, n.a, ps_op(ps, PS_BVNOT, n.b, 0)));
        return r;
    case PS_BVULT: case PS_BVULE:
        if (!(a = ps_to_int(ps, n.a, obl)) || !(b = ps_to_int(ps, n.b, obl))) return 0;
        return ps_op(ps, n.op == PS_BVULT ? PS_ILT : PS_ILE, a, b);
    case PS_EQ:
        if (ps_at(ps, n.a)->sort == PS_ARR) return 0;
        if (!(a = ps_to_int(ps, n.a, obl)) || !(b = ps_to_int(ps, n.b, obl))) return 0;
        return ps_op(ps, PS_EQ, a, b);
    case PS_AND: case PS_OR: case PS_IMP:
        if (!(a = ps_to_int(ps, n.a, obl)) || !(b = ps_to_int(ps, n.b, obl))) return 0;
        return ps_op(ps, n.op, a, b);
    case PS_NOT:
        return (a = ps_to_int(ps, n.a, obl)) ? ps_op(ps, PS_NOT, a, 0) : 0;
    default:
        return 0;
    }
}

/* ---- printing ---- */

static const char *const op_name[] = {
    [PS_BVADD] = "bvadd", [PS_BVMUL] = "bvmul", [PS_BVNOT] = "bvnot", [PS_BVULT] = "bvult", [PS_BVULE] = "bvule",
    [PS_BVSUB] = "bvsub", [PS_BVSLT] = "bvslt", [PS_BVSLE] = "bvsle",
    [PS_IADD] = "+", [PS_IMUL] = "*", [PS_ILT] = "<", [PS_ILE] = "<=",
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
    VEC(PsT) h = {0};
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
    if (q && valid_inst(s, ps, hyps, n, goal, rlimit) == PS_VALID) return PS_VALID;
    return valid_q(s, ps, hyps, n, goal, rlimit);   /* the instantiation tier proves or says nothing */
}

int ps_valid_int(PsSolver *s, Ps *ps, const PsT *hyps, size_t n, PsT goal, unsigned long rlimit)
{
    PsInt obl = {0};
    VEC(PsT) ih = {0}, lin_b = {0}, lin_i = {0};
    PsT g, cb, h;
    size_t i;
    int r = PS_UNKNOWN, rr;
    if (ps_has(ps, goal, PS_FORALL)) return PS_UNKNOWN;
    for (i = 0; i < n; i++) {
        if (ps_has(ps, hyps[i], PS_FORALL)) continue;
        h = ps_to_int(ps, hyps[i], &obl);
        if (!h) goto out;                   /* a hypothesis Int cannot say: the tier does not apply */
        vec_push(&ih, h);
        if (!ps_has(ps, hyps[i], PS_BVMUL)) { vec_push(&lin_b, hyps[i]); vec_push(&lin_i, h); }
    }
    if (!(g = ps_to_int(ps, goal, &obl))) goto out;
    for (i = 0; i < obl.nonneg.len; i++) { vec_push(&ih, obl.nonneg.data[i]); vec_push(&lin_i, obl.nonneg.data[i]); }
    /* 1. the adds with no multiply under them do not wrap, in bit-vectors, from the multiply-free hypotheses */
    if (obl.wrapbv.len) {
        cb = and_all(ps, obl.wrapbv.data, obl.wrapbv.len);
        if (ps_valid(s, ps, lin_b.data, lin_b.len, cb, rlimit) != PS_VALID) goto out;
    }
    /* 2. every multiply, and add over one, stays below 2^64, as Int facts (one query each: a conjunction is slower) */
    for (i = 0; i < obl.wrapint.len; i++)
        if (ps_valid(s, ps, lin_i.data, lin_i.len, obl.wrapint.data[i], rlimit * 25) != PS_VALID) goto out;
    /* 3. the goal over Int; with no wrap possible, an answer there is the answer in bit-vectors */
    rr = ps_valid(s, ps, ih.data, ih.len, g, rlimit * 25);
    r = rr;
out:
    psint_free(&obl); vec_free(&ih); vec_free(&lin_b); vec_free(&lin_i);
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

    /* the copy loop's write bound, and it with the size fact dropped */
    h[0] = ule(&ps, n, szD); h[1] = ult(&ps, i, n);
    expect(&failures, "copy bound", ps_valid(s, &ps, h, 2, ult(&ps, i, szD), 0), PS_VALID);
    expect(&failures, "copy bound without n <= szD", ps_valid(s, &ps, h + 1, 1, ult(&ps, i, szD), 0), PS_INVALID);

    /* the nested loop's write bound: nonlinear as bit-vectors, instant through the Int tier (S7) */
    h[0] = ule(&ps, r, lim); h[1] = ule(&ps, c, lim); h[2] = ult(&ps, i, r); h[3] = ult(&ps, j, c);
    h[4] = ule(&ps, ps_op(&ps, PS_BVMUL, r, c), szD);
    k = ult(&ps, ps_op(&ps, PS_BVADD, ps_op(&ps, PS_BVMUL, i, c), j), szD);
    expect(&failures, "fill2 bound by Int", ps_valid_int(s, &ps, h, 5, k, 400000), PS_VALID);
    expect(&failures, "fill2 bound without r*c <= szD", ps_valid_int(s, &ps, h, 4, k, 400000), PS_INVALID);
    expect(&failures, "fill2 bound without r <= 2^31", ps_valid_int(s, &ps, h + 1, 4, k, 400000), PS_UNKNOWN);   /* the no-wrap proof fails */

    /* wrap: x < x + 1 is false at the top, and the Int tier must not say otherwise */
    k = ult(&ps, x, ps_op(&ps, PS_BVADD, x, one));
    expect(&failures, "x < x + 1 in bit-vectors", ps_valid(s, &ps, NULL, 0, k, 0), PS_INVALID);
    expect(&failures, "x < x + 1 by Int", ps_valid_int(s, &ps, NULL, 0, k, 400000), PS_UNKNOWN);

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
        expect(&failures, "Int tier declines bvsub", ps_valid_int(s, &ps, NULL, 0, ule(&ps, ps_op(&ps, PS_BVSUB, n, i), n), 400000), PS_UNKNOWN);
    }
    ps_close(s);
    ps_free(&ps);
    return failures != 0;
}
