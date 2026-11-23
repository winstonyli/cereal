/* prove_vc.c - the obligation generator of --prove (docs/PROVE_OBLIGATIONS_DESIGN.md): one function's Cfg
 * and its #pragma cereal specs become solver goals (prove_smt.h) and a per-obligation report.
 *
 * Execution is symbolic and forward over the placed blocks.  A cut point is the target of a retreating edge
 * (design decision e3); the code after it is a region analysed from a havoced state, under the requires and the
 * invariants of that cut point.  Reaching a cut point ends a path and yields that point's invariant goals. */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "c/cexpr_int.h"
#include "c/prove_smt.h"
#include "c/tcfg_int.h"

#define MAX_PATHS 256
#define RLIMIT 100000000UL

enum { V_BV, V_BOOL, V_PTR };

typedef struct Val {
    uint8_t k, w;
    bool sgn;
    int al;                 /* V_PTR: the allocation */
    PsT t;                  /* V_PTR: the offset in elements, 64 bits */
} Val;

typedef struct Slot { uint32_t sym; uint8_t w; bool sgn, param; } Slot;
typedef struct Alloc { uint32_t sym; uint8_t w; bool sgn, stored; PsT len, mem0; } Alloc;

typedef struct St {
    PsT *loc, *mem;         /* per slot, per allocation */
    VEC(PsT) hy;            /* the path condition and the accesses assumed in bounds */
} St;

typedef struct Goal { SrcLoc loc; const char *label; uint32_t region; PsT conj; } Goal;
typedef struct Region { uint32_t blk; PsT *base; size_t nbase; } Region;
typedef struct SpecAt { uint32_t cut, node; } SpecAt;

typedef struct Ctx {
    Cfg *L;
    Checker *c;
    Ps ps;
    const uint32_t *ord;
    jmp_buf jb;
    char why[120];
    VEC(Slot) sl;
    VEC(Alloc) al;
    uint32_t nblk;
    uint8_t *cut;           /* per block id */
    uint8_t *body;          /* nblk x nblk: body[c * nblk + b], b in c's natural loop */
    uint8_t *queued;
    VEC(SpecAt) inv;
    VEC(uint32_t) ens, req;
    VEC(PsT) reqt;
    VEC(Goal) goals;
    VEC(Region) regs;
    VEC(void *) blobs;
    VEC(struct St *) sts;
    VEC(uint32_t) es, et;
    VEC(PsT) base;
    unsigned cur, npaths, nfresh;
    bool inspec;
    int cdepth;
    Val ret;
    unsigned proved, bad;       /* the report's counts (in Ctx: prove_func's locals would be clobbered by longjmp) */
} Ctx;

static const char *const opn[TO_COUNT] = {
#define X(n, s) s,
    TAST_OPS(X)
#undef X
};

static Val unsup(Ctx *x, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(x->why, sizeof x->why, fmt, ap);
    va_end(ap);
    longjmp(x->jb, 1);
}

static void *xa(Ctx *x, size_t n)
{
    void *p = xcalloc(1, n ? n : 1);
    vec_push(&x->blobs, p);
    return p;
}

static const char *fmt_a(Ctx *x, const char *fmt, const char *a)
{
    size_t n = strlen(fmt) + strlen(a) + 1;
    char *s = xa(x, n);
    snprintf(s, n, fmt, a);
    return s;
}

static PsT op2(Ctx *x, int op, PsT a, PsT b) { return ps_op(&x->ps, op, a, b); }
static PsT notq(Ctx *x, PsT a) { return ps_op(&x->ps, PS_NOT, a, 0); }
static PsT andq(Ctx *x, PsT a, PsT b) { return a ? op2(x, PS_AND, a, b) : b; }

static PsT and_all(Ctx *x, const PsT *t, size_t n)
{
    PsT r = 0;
    size_t i;
    for (i = 0; i < n; i++)
        r = andq(x, r, t[i]);
    return r;
}

static Val mkv(int k, PsT t, unsigned w, bool sgn)
{
    Val v;
    memset(&v, 0, sizeof v);
    v.k = (uint8_t)k;
    v.t = t;
    v.w = (uint8_t)w;
    v.sgn = sgn;
    return v;
}

/* ---- types ---- */

static bool int_ty(Ctx *x, TypeId t, unsigned *w, bool *sgn)
{
    Checker *c = x->c;
    unsigned b;
    if (!type_is_integer(&c->tt, t))
        return false;
    b = int_bits(c, t);
    if (b < 8)
        b = 8;                              /* _Bool */
    if (b != 8 && b != 16 && b != 32 && b != 64)
        return false;
    *w = b;
    *sgn = type_is_signed(&c->tt, t);
    return true;
}

static int slot_of(Ctx *x, uint32_t sym)
{
    size_t i;
    for (i = 0; i < x->sl.len; i++)
        if (x->sl.data[i].sym == sym)
            return (int)i;
    return -1;
}

static int alloc_of_sym(Ctx *x, uint32_t sym)
{
    size_t i;
    for (i = 0; i < x->al.len; i++)
        if (x->al.data[i].sym == sym)
            return (int)i;
    return -1;
}

static const char *sym_name(Ctx *x, uint32_t sym) { return cident(x->c, csym(x->c, sym)->name); }

/* The allocation a pointer expression walks from, read off its tree. */
static int ptr_alloc(Ctx *x, uint32_t n)
{
    const TNode *t = nd(x->L, n);
    int a;
    switch (t->op) {
    case TO_PTRADD: case TO_CAST: case TO_DEREF: return ptr_alloc(x, kid(x->L, n, 0));
    case TO_LOAD:
        t = nd(x->L, kid(x->L, n, 0));
        if (t->op == TO_SYM && (a = alloc_of_sym(x, t->sym)) >= 0)
            return a;
        break;
    default: break;
    }
    unsup(x, "a pointer that is not a parameter plus an index");
    return -1;
}

/* Finds the parameters and locals, and which allocations are stored to. */
static void scan_syms(Ctx *x)
{
    El *e = x->L->e;
    Checker *c = x->c;
    size_t i, j;
    U32Vec decl = {0};
    for (i = 0; i < e->nodes.len; i++)
        if (e->nodes.data[i].op == TO_LOCAL)
            vec_push(&decl, e->nodes.data[i].sym);
    for (i = 0; i < e->nodes.len; i++) {
        const TNode *t = &e->nodes.data[i];
        const CSym *s;
        bool isdecl = false;
        unsigned w;
        bool sg;
        if (t->op != TO_SYM || slot_of(x, t->sym) >= 0 || alloc_of_sym(x, t->sym) >= 0)
            continue;
        s = csym(c, t->sym);
        if (!(t->sym & SYM_LOCAL) || s->kind != CS_OBJ || s->sc == SC_STATIC || s->sc == SC_EXTERN) {
            vec_free(&decl);
            unsup(x, "the global, static or function %s", cident(c, s->name));
        }
        for (j = 0; j < decl.len; j++)
            isdecl |= decl.data[j] == t->sym;
        if (is_ptr(c, t->ty)) {
            Alloc a;
            memset(&a, 0, sizeof a);
            if (isdecl) {
                vec_free(&decl);
                unsup(x, "the pointer local %s", cident(c, s->name));
            }
            if (!int_ty(x, pointee(c, t->ty), &w, &sg)) {
                vec_free(&decl);
                unsup(x, "the pointer %s to a non-integer", cident(c, s->name));
            }
            a.sym = t->sym; a.w = (uint8_t)w; a.sgn = sg;
            vec_push(&x->al, a);
        } else if (int_ty(x, t->ty, &w, &sg)) {
            Slot sl;
            sl.sym = t->sym; sl.w = (uint8_t)w; sl.sgn = sg; sl.param = !isdecl;
            vec_push(&x->sl, sl);
        } else {
            vec_free(&decl);
            unsup(x, "the object %s of a type outside the subset", cident(c, s->name));
        }
    }
    vec_free(&decl);
    for (i = 0; i < e->nodes.len; i++) {
        const TNode *t = &e->nodes.data[i];
        if (t->op == TO_STORE && e->nodes.data[e->kids.data[t->kid0]].op == TO_DEREF)
            x->al.data[ptr_alloc(x, e->kids.data[t->kid0])].stored = true;
    }
}

/* ---- the state ---- */

static void push_hy(St *s, PsT t) { vec_push(&s->hy, t); }

static St *st_init(Ctx *x, unsigned rg)
{
    size_t i;
    char nm[48];
    St *s = xa(x, sizeof *s);
    vec_push(&x->sts, s);
    s->loc = xa(x, x->sl.len * sizeof(PsT));
    s->mem = xa(x, x->al.len * sizeof(PsT));
    for (i = 0; i < x->sl.len; i++) {
        const Slot *sl = &x->sl.data[i];
        if (sl->param)
            snprintf(nm, sizeof nm, "p%zu_%.14s", i, sym_name(x, sl->sym));
        else
            snprintf(nm, sizeof nm, "v%zu_%.14s_r%u", i, sym_name(x, sl->sym), rg);
        s->loc[i] = ps_var(&x->ps, nm, PS_BV, sl->w);
    }
    for (i = 0; i < x->al.len; i++) {
        const Alloc *a = &x->al.data[i];
        if (rg == 0 || !a->stored)
            s->mem[i] = a->mem0;
        else {
            snprintf(nm, sizeof nm, "M%zu_r%u", i, rg);
            s->mem[i] = ps_var(&x->ps, nm, PS_ARR, a->w);
        }
    }
    return s;
}

static St *st_copy(Ctx *x, const St *s)
{
    St *r = xa(x, sizeof *r);
    vec_push(&x->sts, r);
    r->loc = xa(x, x->sl.len * sizeof(PsT));
    r->mem = xa(x, x->al.len * sizeof(PsT));
    memcpy(r->loc, s->loc, x->sl.len * sizeof(PsT));
    memcpy(r->mem, s->mem, x->al.len * sizeof(PsT));
    r->hy.data = NULL;
    r->hy.len = 0;
    r->hy.cap = 0;
    {
        size_t i;
        for (i = 0; i < s->hy.len; i++)
            vec_push(&r->hy, s->hy.data[i]);
    }
    return r;
}

/* ---- goals ---- */

static void add_goal(Ctx *x, St *s, SrcLoc loc, const char *label, PsT g)
{
    PsT pc = and_all(x, s->hy.data, s->hy.len), term = pc ? op2(x, PS_IMP, pc, g) : g;
    size_t i;
    Goal n;
    for (i = 0; i < x->goals.len; i++) {
        Goal *o = &x->goals.data[i];
        if (o->region == x->cur && o->loc == loc && !strcmp(o->label, label)) {
            o->conj = op2(x, PS_AND, o->conj, term);
            return;
        }
    }
    n.loc = loc; n.label = label; n.region = x->cur; n.conj = term;
    vec_push(&x->goals, n);
}

/* ---- expressions ---- */

static Val ev(Ctx *x, St *s, uint32_t n);

static PsT cnd(Ctx *x, Val v)
{
    if (v.k == V_BOOL)
        return v.t;
    if (v.k == V_PTR)
        unsup(x, "a pointer used as a condition");
    return notq(x, op2(x, PS_EQ, v.t, ps_bvc(&x->ps, v.w, 0)));
}

static Val bv(Ctx *x, Val v)
{
    if (v.k != V_BV)
        return unsup(x, "a comparison or pointer used as a value");
    return v;
}

static PsT ext64(Ctx *x, Val v) { return v.w == 64 ? v.t : ps_resize(&x->ps, v.t, 64, v.sgn); }

static PsT kid_at(Cfg *L, uint32_t n, uint32_t j) { return kid(L, n, j); }

static PsT zero64(Ctx *x) { return ps_bvc(&x->ps, 64, 0); }

static void widen2(Ctx *x, Val *a, Val *b)
{
    if (a->w < b->w) {
        a->t = ps_resize(&x->ps, a->t, b->w, a->sgn);
        a->w = b->w;
    } else if (b->w < a->w) {
        b->t = ps_resize(&x->ps, b->t, a->w, b->sgn);
        b->w = a->w;
    }
}

/* A checked access: the goal, and the assumption that it holds afterwards on this path. */
static void access(Ctx *x, St *s, Val p, SrcLoc loc, bool write)
{
    const Alloc *a = &x->al.data[p.al];
    PsT g = op2(x, PS_BVULT, p.t, a->len);
    add_goal(x, s, loc, fmt_a(x, write ? "bounds write %s" : "bounds read %s", sym_name(x, a->sym)), g);
    push_hy(s, g);
}

static Val ev_slice(Ctx *x, St *s, uint32_t n)
{
    Val v[6];
    PsT lo, hi, lo2, hi2, a, b, d, k, bound, eq;
    const Alloc *A, *B;
    char nm[24];
    int j;
    for (j = 0; j < 6; j++)
        v[j] = ev(x, s, kid(x->L, n, (uint32_t)j));
    if (v[0].k != V_PTR || v[3].k != V_PTR)
        return unsup(x, "a slice of something other than a pointer parameter");
    for (j = 0; j < 6; j++)
        if (j != 0 && j != 3)
            v[j] = bv(x, v[j]);
    A = &x->al.data[v[0].al];
    B = &x->al.data[v[3].al];
    if (A->w != B->w || A->sgn != B->sgn)
        return unsup(x, "slices of different element types");
    if (ps_at(&x->ps, v[0].t)->op != PS_BVC || ps_at(&x->ps, v[3].t)->op != PS_BVC ||
        ps_at(&x->ps, v[0].t)->k || ps_at(&x->ps, v[3].t)->k)
        return unsup(x, "a slice of an offset pointer");
    lo = ext64(x, v[1]); hi = ext64(x, v[2]); lo2 = ext64(x, v[4]); hi2 = ext64(x, v[5]);
    d = op2(x, PS_BVSUB, hi, lo);
    bound = op2(x, PS_BVULE, lo, hi);
    bound = andq(x, bound, op2(x, PS_BVULE, hi, A->len));
    bound = andq(x, bound, op2(x, PS_BVULE, lo2, hi2));
    bound = andq(x, bound, op2(x, PS_BVULE, hi2, B->len));
    bound = andq(x, bound, op2(x, PS_EQ, d, op2(x, PS_BVSUB, hi2, lo2)));
    snprintf(nm, sizeof nm, "k%u", x->nfresh++);
    k = ps_var(&x->ps, nm, PS_BV, 64);
    a = op2(x, PS_SEL, s->mem[v[0].al], op2(x, PS_BVADD, lo, k));
    b = op2(x, PS_SEL, s->mem[v[3].al], op2(x, PS_BVADD, lo2, k));
    eq = ps_forall(&x->ps, k, op2(x, PS_IMP, op2(x, PS_BVULT, k, d), op2(x, PS_EQ, a, b)));
    return mkv(V_BOOL, andq(x, bound, eq), 0, false);
}

static Val ev_arith(Ctx *x, int pun, Val a, Val b, TypeId ty)
{
    unsigned w;
    bool sg;
    int op = pun == P_PLUS ? PS_BVADD : pun == P_MINUS ? PS_BVSUB : PS_BVMUL;
    a = bv(x, a);
    b = bv(x, b);
    if (x->inspec) {                        /* exact: widen to signed 64 bits, refused unless no value can wrap */
        Val r = mkv(V_BV, op2(x, op, ext64(x, a), ext64(x, b)), 64, true);
        int64_t lo, hi;
        if (!ps_range(&x->ps, r.t, &lo, &hi))
            return unsup(x, "spec arithmetic that may exceed 64 bits");
        return r;
    }
    if (!int_ty(x, ty, &w, &sg) || a.w != w || b.w != w)
        return unsup(x, "arithmetic on mixed or unsupported widths");
    return mkv(V_BV, op2(x, op, a.t, b.t), w, sg);
}

static Val ev_cmp(Ctx *x, int pun, Val a, Val b)
{
    bool sg;
    PsT r;
    a = bv(x, a);
    b = bv(x, b);
    widen2(x, &a, &b);
    sg = a.sgn && b.sgn;
    switch (pun) {
    case P_EQEQ: r = op2(x, PS_EQ, a.t, b.t); break;
    case P_NE: r = notq(x, op2(x, PS_EQ, a.t, b.t)); break;
    case P_LT: r = op2(x, sg ? PS_BVSLT : PS_BVULT, a.t, b.t); break;
    case P_LE: r = op2(x, sg ? PS_BVSLE : PS_BVULE, a.t, b.t); break;
    case P_GT: r = op2(x, sg ? PS_BVSLT : PS_BVULT, b.t, a.t); break;
    default: r = op2(x, sg ? PS_BVSLE : PS_BVULE, b.t, a.t); break;
    }
    return mkv(V_BOOL, r, 0, false);
}

static Val ev_load(Ctx *x, St *s, uint32_t n)
{
    uint32_t lv = kid(x->L, n, 0);
    const TNode *t = nd(x->L, lv);
    if (t->op == TO_SYM) {
        int i = slot_of(x, t->sym), a;
        if (i >= 0)
            return mkv(V_BV, s->loc[i], x->sl.data[i].w, x->sl.data[i].sgn);
        a = alloc_of_sym(x, t->sym);
        if (a >= 0) {
            Val p = mkv(V_PTR, zero64(x), 64, false);
            p.al = a;
            return p;
        }
    } else if (t->op == TO_DEREF) {
        Val p = ev(x, s, kid(x->L, lv, 0));
        const Alloc *a;
        if (x->inspec)
            return unsup(x, "a load through a pointer in a spec");
        if (p.k != V_PTR)
            return unsup(x, "a dereference of a non-pointer");
        a = &x->al.data[p.al];
        if (nd(x->L, n)->ty && !(type_is_integer(&x->c->tt, nd(x->L, n)->ty) && int_bits(x->c, nd(x->L, n)->ty) <= a->w))
            return unsup(x, "an access of another element type");
        access(x, s, p, nd(x->L, lv)->loc, false);
        return mkv(V_BV, op2(x, PS_SEL, s->mem[p.al], p.t), a->w, a->sgn);
    }
    return unsup(x, "a load of a %s", opn[t->op]);
}

static Val ev_store(Ctx *x, St *s, uint32_t n)
{
    uint32_t lv = kid(x->L, n, 0);
    const TNode *t = nd(x->L, lv);
    Val v = bv(x, ev(x, s, kid(x->L, n, 1)));
    if (x->cdepth)
        return unsup(x, "a store under && or ||");
    if (t->op == TO_SYM) {
        int i = slot_of(x, t->sym);
        if (i < 0 || x->sl.data[i].param)
            return unsup(x, "an assignment to a parameter or a pointer");
        if (v.w != x->sl.data[i].w)
            return unsup(x, "a store of another width");
        s->loc[i] = v.t;
        return v;
    }
    if (t->op == TO_DEREF) {
        Val p = ev(x, s, kid(x->L, lv, 0));
        if (p.k != V_PTR)
            return unsup(x, "a store through a non-pointer");
        if (v.w != x->al.data[p.al].w)
            return unsup(x, "a store of another element width");
        access(x, s, p, nd(x->L, lv)->loc, true);
        s->mem[p.al] = ps_store(&x->ps, s->mem[p.al], p.t, v.t);
        return v;
    }
    return unsup(x, "a store to a %s", opn[t->op]);
}

static Val ev(Ctx *x, St *s, uint32_t n)
{
    Cfg *L = x->L;
    const TNode *t = nd(L, n);
    unsigned w;
    bool sg;
    switch (t->op) {
    case TO_CONST:
        if (!int_ty(x, t->ty, &w, &sg))
            return unsup(x, "a constant of a non-integer type");
        return mkv(V_BV, ps_bvc(&x->ps, w, w == 64 ? (uint64_t)t->val : (uint64_t)t->val & ((1ull << w) - 1)), w, sg);
    case TO_LOAD: return ev_load(x, s, n);
    case TO_STORE: return ev_store(x, s, n);
    case TO_POST: {
        const TNode *st = nd(L, kid(L, n, 0)), *lv;
        int i;
        if (st->op != TO_STORE || (lv = nd(L, kid_at(L, kid(L, n, 0), 0)))->op != TO_SYM || (i = slot_of(x, lv->sym)) < 0)
            return unsup(x, "a post-increment of something but a local");
        {
            Val old = mkv(V_BV, s->loc[i], x->sl.data[i].w, x->sl.data[i].sgn);
            ev(x, s, kid(L, n, 0));
            return old;
        }
    }
    case TO_CAST: {
        Val a = ev(x, s, kid(L, n, 0));
        if (tkind(x->c, t->ty) == TY_BOOL)
            return mkv(V_BOOL, cnd(x, a), 0, false);
        if (is_ptr(x->c, t->ty)) {
            if (a.k != V_PTR)
                return unsup(x, "a cast to a pointer");
            return a;
        }
        if (!int_ty(x, t->ty, &w, &sg))
            return unsup(x, "a cast to a type outside the subset");
        a = bv(x, a);
        if (w != a.w)
            a.t = ps_resize(&x->ps, a.t, w, a.sgn);
        a.w = (uint8_t)w;
        a.sgn = sg;
        return a;
    }
    case TO_UNOP: {
        Val a = ev(x, s, kid(L, n, 0));
        if (t->pun == P_BANG)
            return mkv(V_BOOL, notq(x, cnd(x, a)), 0, false);
        a = bv(x, a);
        if (t->pun == P_PLUS)
            return a;
        if (t->pun == P_MINUS) {
            if (x->inspec) {                /* exact, as ev_arith: 0 - a over signed 64 bits, refused unless it cannot wrap */
                Val r = mkv(V_BV, op2(x, PS_BVSUB, ps_bvc(&x->ps, 64, 0), ext64(x, a)), 64, true);
                int64_t lo, hi;
                if (!ps_range(&x->ps, r.t, &lo, &hi))
                    return unsup(x, "spec arithmetic that may exceed 64 bits");
                return r;
            }
            a.t = op2(x, PS_BVSUB, ps_bvc(&x->ps, a.w, 0), a.t);
            return a;
        }
        return unsup(x, "the unary operator %s", punct_spelling[t->pun]);
    }
    case TO_BINOP: {
        Val a, b;
        if (t->pun != P_PLUS && t->pun != P_MINUS && t->pun != P_STAR)
            return unsup(x, "the operator %s", punct_spelling[t->pun]);
        a = ev(x, s, kid(L, n, 0));
        b = ev(x, s, kid(L, n, 1));
        return ev_arith(x, t->pun, a, b, t->ty);
    }
    case TO_CMP: {
        Val a = ev(x, s, kid(L, n, 0)), b = ev(x, s, kid(L, n, 1));
        if (a.k == V_PTR || b.k == V_PTR)
            return unsup(x, "a pointer comparison");
        return ev_cmp(x, t->pun, a, b);
    }
    case TO_LOGAND: case TO_LOGOR: {
        PsT a = cnd(x, ev(x, s, kid(L, n, 0))), b;
        size_t mark = s->hy.len;
        push_hy(s, t->op == TO_LOGAND ? a : notq(x, a));
        x->cdepth++;
        b = cnd(x, ev(x, s, kid(L, n, 1)));
        x->cdepth--;
        s->hy.len = mark;
        return mkv(V_BOOL, op2(x, t->op == TO_LOGAND ? PS_AND : PS_OR, a, b), 0, false);
    }
    case TO_PTRADD: {
        Val p = ev(x, s, kid(L, n, 0)), i = bv(x, ev(x, s, kid(L, n, 1)));
        if (p.k != V_PTR)
            return unsup(x, "pointer arithmetic on a non-parameter");
        if ((uint64_t)t->val != x->al.data[p.al].w / 8u)
            return unsup(x, "pointer arithmetic with another element size");
        p.t = op2(x, PS_BVADD, p.t, ext64(x, i));
        return p;
    }
    case TO_RESULT:
        if (x->ret.k == V_PTR)
            return unsup(x, "return of a pointer in an ensures");
        return x->ret;
    case TO_LEN: {
        Val p = ev(x, s, kid(L, n, 0));
        if (p.k != V_PTR)
            return unsup(x, "$len of something but a pointer parameter");
        return mkv(V_BV, x->al.data[p.al].len, 64, false);
    }
    case TO_SLICEEQ: return ev_slice(x, s, n);
    default: return unsup(x, "the %s operation", opn[t->op]);
    }
}

/* ---- the graph ---- */

static unsigned succs(Ctx *x, uint32_t b, uint32_t out[2])
{
    const CBlk *B = &x->L->b.data[b];
    switch (B->term) {
    case TK_JMP: out[0] = B->a; return 1;
    case TK_BR: out[0] = B->a; out[1] = B->b; return 2;
    case TK_RET: return 0;
    default: unsup(x, "a switch, computed goto or open block"); return 0;
    }
}

/* Cut points (targets of retreating edges) and each one's natural loop. */
static void find_loops(Ctx *x)
{
    Cfg *L = x->L;
    size_t pos, i, j;
    VEC(uint32_t) es = {0}, et = {0};
    for (pos = 0; pos < L->npl; pos++) {         /* before any vector is live: succs may give up */
        uint32_t t[2];
        succs(x, x->ord[pos], t);
    }
    for (pos = 0; pos < L->npl; pos++) {
        uint32_t b = x->ord[pos], t[2];
        unsigned k = succs(x, b, t), q;
        for (q = 0; q < k; q++) {
            vec_push(&es, b);
            vec_push(&et, t[q]);
        }
    }
    x->nblk = (uint32_t)L->b.len;
    x->cut = xa(x, x->nblk);
    x->queued = xa(x, x->nblk);
    x->body = xa(x, (size_t)x->nblk * x->nblk);
    for (i = 0; i < es.len; i++)
        if (L->b.data[et.data[i]].pos <= L->b.data[es.data[i]].pos)
            x->cut[et.data[i]] = 1;
    for (j = 0; j < x->nblk; j++) {
        uint8_t *in = x->body + j * x->nblk;
        VEC(uint32_t) stack = {0};
        if (!x->cut[j])
            continue;
        in[j] = 1;
        for (i = 0; i < es.len; i++)
            if (et.data[i] == j && L->b.data[es.data[i]].pos >= L->b.data[j].pos && !in[es.data[i]]) {
                in[es.data[i]] = 1;
                vec_push(&stack, es.data[i]);
            }
        while (stack.len) {
            uint32_t m = stack.data[--stack.len];
            for (i = 0; i < es.len; i++)
                if (et.data[i] == m && !in[es.data[i]]) {
                    in[es.data[i]] = 1;
                    vec_push(&stack, es.data[i]);
                }
        }
        vec_free(&stack);
        if (in[x->ord[0]] && x->ord[0] != j) {
            vec_free(&es);
            vec_free(&et);
            unsup(x, "a loop with more than one entry");
        }
    }
    vec_free(&es);
    vec_free(&et);
}

static size_t body_size(Ctx *x, uint32_t c)
{
    size_t n = 0, i;
    for (i = 0; i < x->nblk; i++)
        n += x->body[(size_t)c * x->nblk + i];
    return n;
}

/* Sorts the specs: requires and ensures lead the entry block, an invariant belongs to its innermost loop. */
static void collect_specs(Ctx *x)
{
    Cfg *L = x->L;
    size_t pos, i;
    for (pos = 0; pos < L->npl; pos++) {
        uint32_t b = x->ord[pos];
        const CBlk *B = &L->b.data[b];
        for (i = 0; i < B->st.len; i++) {
            uint32_t n = B->st.data[i];
            int kind;
            if (nd(L, n)->op != TO_SPEC)
                continue;
            kind = (int)nd(L, n)->val;
            if (kind == SPEC_INVARIANT) {
                SpecAt sa;
                uint32_t c, best = TN;
                for (c = 0; c < x->nblk; c++)
                    if (x->cut[c] && x->body[(size_t)c * x->nblk + b] && (best == TN || body_size(x, c) < body_size(x, best)))
                        best = c;
                if (best == TN)
                    unsup(x, "an invariant outside a loop");
                sa.cut = best; sa.node = n;
                vec_push(&x->inv, sa);
            } else if ((kind == SPEC_REQUIRES || kind == SPEC_ENSURES) && pos == 0) {
                vec_push(kind == SPEC_REQUIRES ? &x->req : &x->ens, n);
            } else {
                unsup(x, "a contract that is not at the start of the function");
            }
        }
    }
}

static PsT spec_term(Ctx *x, St *s, uint32_t spec)
{
    PsT r;
    x->inspec = true;
    r = cnd(x, ev(x, s, kid(x->L, spec, 0)));
    x->inspec = false;
    return r;
}

static void arrive(Ctx *x, St *s, uint32_t t, uint32_t from);

static void run_block(Ctx *x, St *s, uint32_t b)
{
    Cfg *L = x->L;
    const CBlk *B = &L->b.data[b];
    size_t i;
    for (i = 0; i < B->st.len; i++) {
        uint32_t n = B->st.data[i];
        switch (nd(L, n)->op) {
        case TO_EXPRSTMT: ev(x, s, kid(L, n, 0)); break;
        case TO_LOCAL: {            /* `T x = e;` stores e; a declaration alone leaves x free */
            int sl = slot_of(x, nd(L, n)->sym);
            if (nd(L, n)->nkids == 1 && sl >= 0) {
                Val v = bv(x, ev(x, s, kid(L, n, 0)));
                if (v.w != x->sl.data[sl].w)
                    unsup(x, "an initializer of another width");
                s->loc[sl] = v.t;
            }
            break;
        }
        case TO_SPEC: case TO_NOP: break;
        default: unsup(x, "the statement %s", opn[nd(L, n)->op]);
        }
    }
    switch (B->term) {
    case TK_JMP: arrive(x, s, B->a, b); break;
    case TK_BR: {
        PsT c = cnd(x, ev(x, s, B->val));
        St *s2;
        if (++x->npaths > MAX_PATHS)
            unsup(x, "more than %d paths in a region", MAX_PATHS);
        s2 = st_copy(x, s);
        push_hy(s, c);
        push_hy(s2, notq(x, c));
        arrive(x, s, B->a, b);
        arrive(x, s2, B->b, b);
        break;
    }
    case TK_RET: {
        size_t k;
        x->ret = mkv(V_BV, 0, 0, false);
        if (B->val != TN)
            x->ret = ev(x, s, B->val);
        for (k = 0; k < x->ens.len; k++)
            add_goal(x, s, nd(L, x->ens.data[k])->loc, "ensures", spec_term(x, s, x->ens.data[k]));
        break;
    }
    default: unsup(x, "a switch, computed goto or open block");
    }
}

static void arrive(Ctx *x, St *s, uint32_t t, uint32_t from)
{
    if (!x->cut[t]) {
        run_block(x, s, t);
        return;
    }
    {
        bool back = from != TN && x->body[(size_t)t * x->nblk + from];
        size_t i;
        for (i = 0; i < x->inv.len; i++)
            if (x->inv.data[i].cut == t)
                add_goal(x, s, nd(x->L, x->inv.data[i].node)->loc,
                         back ? "invariant preserved" : "invariant holds on entry",
                         spec_term(x, s, x->inv.data[i].node));
        if (!x->queued[t]) {
            Region r;
            x->queued[t] = 1;
            r.blk = t; r.base = NULL; r.nbase = 0;
            vec_push(&x->regs, r);
        }
    }
}

/* ---- the run ---- */

static void put_loc(Checker *c, FILE *out, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    uint32_t line = 0, col = 0;
    if (f)
        srcmgr_linecol(f, loc, &line, &col);
    fprintf(out, "%s:%u:%u", f ? f->name : "?", line, col);
}

static void analyse(Ctx *x, PsSolver *sv, FILE *out)
{
    Cfg *L = x->L;
    size_t i, r;
    char nm[48];
    scan_syms(x);
    for (i = 0; i < x->al.len; i++) {
        Alloc *a = &x->al.data[i];
        snprintf(nm, sizeof nm, "len%zu", i);
        a->len = ps_var(&x->ps, nm, PS_BV, 64);
        snprintf(nm, sizeof nm, "M%zu", i);
        a->mem0 = ps_var(&x->ps, nm, PS_ARR, a->w);
    }
    find_loops(x);
    collect_specs(x);
    {
        Region r0;
        r0.blk = TN; r0.base = NULL; r0.nbase = 0;
        vec_push(&x->regs, r0);
    }
    for (r = 0; r < x->regs.len; r++) {
        St *s;
        size_t g0 = x->goals.len, k;
        x->cur = (unsigned)r;
        x->base.len = 0;
        x->npaths = 1;
        s = st_init(x, (unsigned)r);
        if (r == 0) {
            for (k = 0; k < x->req.len; k++)
                vec_push(&x->reqt, spec_term(x, s, x->req.data[k]));
        }
        for (k = 0; k < x->reqt.len; k++)
            vec_push(&x->base, x->reqt.data[k]);
        if (r > 0)
            for (k = 0; k < x->inv.len; k++)
                if (x->inv.data[k].cut == x->regs.data[r].blk)
                    vec_push(&x->base, spec_term(x, s, x->inv.data[k].node));
        if (r == 0)
            arrive(x, s, x->ord[0], TN);
        else
            run_block(x, s, x->regs.data[r].blk);
        for (k = g0; k < x->goals.len; k++) {
            const Goal *g = &x->goals.data[k];
            int res = ps_prove(sv, &x->ps, x->base.data, x->base.len, g->conj, RLIMIT);
            const char *st = res == PS_VALID ? "proved" : res == PS_INVALID ? "not proved (sat)"
                           : res == PS_UNKNOWN ? "not proved (unknown)" : "not proved (solver error)";
            fputs("  ", out);
            put_loc(x->c, out, g->loc);
            fprintf(out, "  %-22s %s\n", st, g->label);
            if (res == PS_VALID)
                ++x->proved;
            else
                ++x->bad;
        }
    }
    (void)L;
}

void prove_func(Cfg *L, const uint32_t *ord, FILE *out)
{
    Ctx x;
    PsSolver *sv;
    char err[200];
    volatile unsigned unsupported = 0;
    size_t i;
    const char *fn = L->e->fsym ? cident(L->e->c, csym(L->e->c, L->e->fsym)->name) : "?";
    memset(&x, 0, sizeof x);
    x.L = L;
    x.c = L->e->c;
    x.ord = ord;
    sv = ps_open("z3 -in -smt2", err, sizeof err);
    if (!sv) {
        fprintf(out, "%s: unsupported: no solver: %s\n", fn, err);
        unsupported = 1;
    } else {
        FILE *buf = tmpfile();
        long len;
        if (!buf)
            buf = out;
        if (setjmp(x.jb)) {
            if (buf != out)
                fclose(buf);
            fprintf(out, "%s: unsupported: %s\n", fn, x.why);
            unsupported = 1;
        } else {
            analyse(&x, sv, buf);
            if (buf != out) {
                char chunk[512];
                size_t n;
                len = ftell(buf);
                (void)len;
                rewind(buf);
                fprintf(out, "%s: assuming pointer parameters are distinct allocations; proved provided no signed arithmetic overflows\n", fn);
                while ((n = fread(chunk, 1, sizeof chunk, buf)) > 0)
                    fwrite(chunk, 1, n, out);
                fclose(buf);
            }
            fprintf(out, "%s: %u proved, %u not proved, 0 unsupported\n", fn, x.proved, x.bad);
        }
        ps_close(sv);
    }
    if ((x.bad || unsupported) && x.c->opt.tast_bad)
        ++*x.c->opt.tast_bad;
    for (i = 0; i < x.sts.len; i++)
        vec_free(&x.sts.data[i]->hy);
    vec_free(&x.sts);
    vec_free(&x.base);
    for (i = 0; i < x.blobs.len; i++)
        free(x.blobs.data[i]);
    vec_free(&x.blobs);
    ps_free(&x.ps);
    vec_free(&x.sl); vec_free(&x.al); vec_free(&x.inv); vec_free(&x.ens); vec_free(&x.req);
    vec_free(&x.reqt); vec_free(&x.goals); vec_free(&x.regs);
}
