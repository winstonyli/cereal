/* pc.c - the symbolic fold: presence conditions of every group of a unit. */
#include "pc.h"
#include "pcq.h"
#include "pcfam.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define PC_BDD_CAP ((size_t)1 << 22)
#define PC_MAX_DEPTH 16

struct PcMacro {
    Ident *id;
    Bdd def;                /* defined now */
    PcEntries vals;         /* the values it can hold now, with conditions */
};

struct PcOnce {
    SrcFile *file;
    bool has;               /* the file says #pragma once */
    Bdd done;               /* it has been entered under this condition */
};

typedef struct PFrame {
    Bdd outer, prior, br;
} PFrame;

/* ---- the real run ------------------------------------------------------ */

/* Inside a system header a condition is whatever the preprocessor decided:
 * its own evaluator expands the function-like macros (__GLIBC_PREREQ) and
 * does the arithmetic the fold cannot.  The run is traced as the set of
 * groups it took, each named by the chain of #include directives entered
 * and the group's directive (PcGroup.inst); the fold looks its instances up.
 * The listeners fire in a parallel run's phase A, on the calling thread, so
 * the trace is the Analysis's own (Analysis.pct), not shared. */
/* The macro events of the system headers entered from outside one, in order:
 * the fold replays them (replay_run) instead of reading those headers again.
 * A header the run did not enter (a guard or #pragma once skipped it, or the
 * include is in a group the host does not take) is still folded from its
 * skeleton. */
enum { EV_ENTER, EV_EXIT, EV_DEF, EV_UNDEF, EV_MISS };
typedef struct PcEv {
    uint8_t kind;
    bool angled;
    union {
        Macro *m;
        Ident *id;
        char *spelled;
    } u;
} PcEv;

typedef struct PcRun {
    uint64_t chain;             /* of the #include that entered the outermost header */
    uint32_t begin, end;        /* its events */
} PcRun;

struct PcTrace {
    VEC(uint64_t) taken;
    VEC(uint64_t) chains;       /* the chain at each open file */
    VEC(PcEv) ev;
    VEC(PcRun) runs;
    uint32_t sys;               /* depth inside system headers */
};

static uint64_t mixh(uint64_t h, uint32_t x)
{
    h = (h ^ x) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

static void trace_ev(PcTrace *t, int kind, void *p)
{
    PcEv e;
    memset(&e, 0, sizeof e);
    e.kind = (uint8_t)kind;
    e.u.m = p;
    vec_push(&t->ev, e);
}

static void trace_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    PcTrace *t = ctx;
    uint64_t c = t->chains.len ? vec_last(&t->chains) : 0;
    uint64_t nc = via && via->hash_loc ? mixh(c, via->hash_loc) : c;
    vec_push(&t->chains, nc);
    if (t->sys || f->kind == SF_SYSTEM || f->system_header) {
        if (!t->sys) {
            PcRun r;
            r.chain = nc;
            r.begin = (uint32_t)t->ev.len;
            r.end = 0;
            vec_push(&t->runs, r);
        }
        t->sys++;
        trace_ev(t, EV_ENTER, NULL);
    }
}

static void trace_exit(void *ctx, SrcFile *f)
{
    PcTrace *t = ctx;
    (void)f;
    if (t->chains.len)
        t->chains.len--;
    if (t->sys) {
        trace_ev(t, EV_EXIT, NULL);
        if (!--t->sys)
            vec_last(&t->runs).end = (uint32_t)t->ev.len;
    }
}

static void trace_define(void *ctx, Macro *m, Macro *replaced)
{
    PcTrace *t = ctx;
    (void)replaced;
    if (t->sys)
        trace_ev(t, EV_DEF, m);
}

static void trace_undef(void *ctx, Ident *id, Macro *m, SrcLoc hash_loc,
                        SrcLoc name_loc)
{
    PcTrace *t = ctx;
    (void)m;
    (void)hash_loc;
    (void)name_loc;
    if (t->sys)
        trace_ev(t, EV_UNDEF, id);
}

static void trace_include(void *ctx, const IncludeEvent *ev)
{
    PcTrace *t = ctx;
    if (t->sys && ev->result == INC_NOT_FOUND && !ev->macros_only) {
        trace_ev(t, EV_MISS, xstrdup(ev->spelled));
        vec_last(&t->ev).angled = ev->angled;
    }
}

static void trace_cond(void *ctx, const CondEvent *ev)
{
    PcTrace *t = ctx;
    if (ev->taken && ev->kind != COND_ENDIF && t->chains.len)
        vec_push(&t->taken, mixh(vec_last(&t->chains), ev->kw_loc));
}

void pc_trace_attach(Analysis *a)
{
    PPListener l;
    if (a->pct)
        return;
    a->pct = xcalloc(1, sizeof *a->pct);
    memset(&l, 0, sizeof l);
    l.ctx = a->pct;
    l.file_enter = trace_enter;
    l.file_exit = trace_exit;
    l.cond = trace_cond;
    l.define = trace_define;
    l.undef = trace_undef;
    l.include = trace_include;
    pp_add_listener(a->pp, l);
}

void pc_trace_free(Analysis *a)
{
    if (!a->pct)
        return;
    vec_free(&a->pct->taken);
    vec_free(&a->pct->chains);
    {
        size_t i;
        for (i = 0; i < a->pct->ev.len; i++)
            if (a->pct->ev.data[i].kind == EV_MISS)
                free(a->pct->ev.data[i].u.spelled);
    }
    vec_free(&a->pct->ev);
    vec_free(&a->pct->runs);
    free(a->pct);
    a->pct = NULL;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static bool was_taken(const PcUnit *u, uint64_t inst)
{
    return u->taken.len &&
           bsearch(&inst, u->taken.data, u->taken.len, sizeof *u->taken.data, cmp_u64);
}

/* ---- host valuation --------------------------------------------------- */

static const Macro *host_macro(const PcUnit *u, Ident *id)
{
    const PP *pp = u->an->pp;
    return macro_at_version(pp->mt, id, pp->seq_main);
}

/* the value of an object-like macro whose body is one number */
static bool macro_number(Analysis *a, const Macro *m, intmax_t *n)
{
    return m && !m->funclike && m->body_len == 1 &&
           m->body[0].kind == TK_PPNUM && pc_ppnum(a, &m->body[0], n, NULL);
}

static const Macro *host_named(const PcUnit *u, const char *s, size_t n)
{
    Ident *id = intern_find(u->an->in, s, n);
    return id ? host_macro(u, id) : NULL;
}

static bool key_value(void *ctx, const char *s, size_t n, intmax_t *v)
{
    PcUnit *u = ctx;
    const Macro *m = host_named(u, s, n);
    *v = 0;
    return !m || macro_number(u->an, m, v);
}

static bool key_defined(void *ctx, const char *s, size_t n)
{
    return host_named(ctx, s, n) != NULL;
}

/* The host value of an atom; *known false: an opaque key that names a call or
 * a macro that is not a number. */
static bool host_atom(PcUnit *u, const PcAtom *at, bool *known)
{
    const Macro *m;
    intmax_t n;
    *known = true;
    if (!at)
        return false;
    if (at->kind == PA_LEAF) {
        PcKeyHost h = {u, key_value, key_defined};
        return pc_leaf_host(at, &h);
    }
    if (pc_atom_pinned(at)) {
        PcKeyHost h = {u, key_value, key_defined};
        *known = pc_key_eval(at->text, &h, &n);
        return *known && n != 0;
    }
    m = host_macro(u, at->id);
    if (at->kind == PA_DEF)
        return m != NULL;
    return macro_number(u->an, m, &n) && n >= at->c;
}

bool pc_host_atom(PcUnit *u, const PcAtom *at)
{
    bool known;
    return host_atom(u, at, &known);
}

static bool host_var(void *ctx, uint32_t v)
{
    PcUnit *u = ctx;
    return pc_host_atom(u, pc_atom_at(&u->cx, v));
}

/* ---- macro state ------------------------------------------------------ */

static uint32_t id_hash(const Ident *id)
{
    return (uint32_t)(((uintptr_t)id >> 4) * 2654435761u);
}

static void macros_grow(PcUnit *u)
{
    size_t i, ncap = u->hcap ? u->hcap * 2 : 256;
    free(u->htab);
    u->htab = xcalloc(ncap, sizeof *u->htab);
    u->hcap = ncap;
    for (i = 0; i < u->macros.len; i++) {
        size_t k = id_hash(u->macros.data[i]->id) & (ncap - 1);
        while (u->htab[k])
            k = (k + 1) & (ncap - 1);
        u->htab[k] = (uint32_t)i + 1;
    }
}

/* "name@offset": the label of a value the fold does not model (no printf: one
 * per such #define) */
static const char *opq_label(Arena *a, const Ident *name, uint32_t loc)
{
    char digits[12], *s;
    size_t nd = 0, i;
    do
        digits[nd++] = (char)('0' + loc % 10);
    while (loc /= 10);
    s = arena_alloc(a, name->len + nd + 2);
    memcpy(s, name->str, name->len);
    s[name->len] = '@';
    for (i = 0; i < nd; i++)
        s[name->len + 1 + i] = digits[nd - 1 - i];
    return s;
}

static PcMacro *macro_state(PcUnit *u, Ident *id)
{
    size_t k;
    PcMacro *s;
    PcEntry e;
    if (u->hcap) {
        k = id_hash(id) & (u->hcap - 1);
        while (u->htab[k]) {
            if (u->macros.data[u->htab[k] - 1]->id == id)
                return u->macros.data[u->htab[k] - 1];
            k = (k + 1) & (u->hcap - 1);
        }
    }
    s = xcalloc(1, sizeof *s);
    s->id = id;
    s->def = pc_atom(&u->cx, PA_DEF, id, 0, NULL);
    if (!pc_fam_values(&u->cx, id, s->def, &s->vals)) {
        e.cond = s->def;
        e.known = false;
        e.n = 0;
        e.opq = NULL;
        e.expr = NULL;
        e.fn = NULL;
        vec_push(&s->vals, e);
    }
    vec_push(&u->macros, s);
    if (u->macros.len * 2 > u->hcap)
        macros_grow(u);
    else {
        k = id_hash(id) & (u->hcap - 1);
        while (u->htab[k])
            k = (k + 1) & (u->hcap - 1);
        u->htab[k] = (uint32_t)u->macros.len;
    }
    return s;
}

static Bdd env_defined(void *ctx, PcCtx *cx, Ident *id)
{
    PcUnit *u = ctx;
    (void)cx;
    return macro_state(u, id)->def;
}

static void env_values(void *ctx, PcCtx *cx, Ident *id, PcEntries *out)
{
    PcUnit *u = ctx;
    const PcMacro *s = macro_state(u, id);
    size_t i;
    (void)cx;
    for (i = 0; i < s->vals.len; i++)
        vec_push(out, s->vals.data[i]);
}

/* the macro now holds the number n (known) or the unmodeled value opq under
 * cur */
static void do_define(PcUnit *u, Ident *id, bool known, intmax_t n,
                      const char *opq, PcNode *expr, const PcFn *fn, Bdd cur)
{
    BddMgr *m = &u->cx.m;
    PcMacro *s = macro_state(u, id);
    PcEntries keep = {0};
    PcEntry e;
    size_t i;
    s->def = bdd_or(m, cur, s->def);
    for (i = 0; i < s->vals.len; i++) {
        e = s->vals.data[i];
        e.cond = bdd_and(m, e.cond, bdd_not(m, cur));
        if (e.cond != BDD_FALSE)
            vec_push(&keep, e);
    }
    e.cond = cur;
    e.known = known;
    e.n = n;
    e.opq = opq;
    e.expr = expr;
    e.fn = fn;
    if (cur != BDD_FALSE)
        vec_push(&keep, e);
    vec_free(&s->vals);
    s->vals = keep;
}

static void do_undef(PcUnit *u, Ident *id, Bdd cur)
{
    BddMgr *m = &u->cx.m;
    PcMacro *s = macro_state(u, id);
    PcEntries keep = {0};
    size_t i;
    s->def = bdd_and(m, s->def, bdd_not(m, cur));
    for (i = 0; i < s->vals.len; i++) {
        PcEntry e = s->vals.data[i];
        e.cond = bdd_and(m, e.cond, bdd_not(m, cur));
        if (e.cond != BDD_FALSE)
            vec_push(&keep, e);
    }
    vec_free(&s->vals);
    s->vals = keep;
}

/* ---- the fold --------------------------------------------------------- */

/* NAME(a, b) body: the parameters and the body, or NULL when the macro uses
 * # (stringify; the call stays opaque).  t is the #define's tokens. */
static const PcFn *fn_macro(PcUnit *u, const Tok *t, uint32_t n)
{
    Analysis *an = u->an;
    Ident **ps = NULL;
    PcFn *f;
    uint32_t np = 0, i = 2;
    bool va = false;
    if (n < 3)
        return NULL;
    ps = NEW_ARRAY(an->arena, Ident *, n);
    if (!tok_is_punct(&t[i], P_RPAREN)) {
        for (;; i++) {
            if (tok_is_punct(&t[i], P_ELLIPSIS) && i + 1 < n &&
                tok_is_punct(&t[i + 1], P_RPAREN)) {
                va = true;
                i++;
                break;
            }
            if (t[i].kind != TK_IDENT || i + 1 >= n)
                return NULL;
            ps[np++] = tok_ident(an->in, &t[i]);
            i++;
            if (tok_is_punct(&t[i], P_RPAREN))
                break;
            if (!tok_is_punct(&t[i], P_COMMA) || i + 1 >= n)
                return NULL;
        }
    }
    i++;
    for (uint32_t k = i; k < n; k++)
        if (tok_is_punct(&t[k], P_HASH) || tok_is_punct(&t[k], P_ELLIPSIS))
            return NULL;
    if (i < n && (tok_is_punct(&t[i], P_HASHHASH) ||
                  tok_is_punct(&t[n - 1], P_HASHHASH)))
        return NULL;
    f = NEW(an->arena, PcFn);
    f->params = ps;
    f->np = np;
    f->va = va;
    f->body.t = t + i;
    f->body.n = n - i;
    return f;
}


/* the condition of a group: symbolic, or in a system header what the
 * preprocessor decided */
static Bdd group_cond(PcUnit *u, const SkDirective *d)
{
    PcCtx *cx = &u->cx;
    PcNode *n;
    Bdd c;
    if (u->conc)
        return was_taken(u, mixh(u->chain, d->kw_loc)) ? BDD_TRUE : BDD_FALSE;
    n = pc_branch_cond(cx, d);
    if (n) {
        c = pc_truth(cx, n);
    } else {
        StrBuf sb = {0};
        uint32_t i;
        for (i = 0; i < d->toks.n; i++) {
            if (i)
                sb_putc(&sb, ' ');
            sb_putn(&sb, pp_text(u->an->pp, &d->toks.t[i]), d->toks.t[i].len);
        }
        c = pc_atom(cx, PA_TEXT, NULL, 0,
                    arena_strndup(u->an->arena, sb_cstr(&sb), sb.len));
        sb_free(&sb);
    }
    return c;
}

static void record(PcUnit *u, SrcFile *f, size_t dir, const SkDirective *d,
                   Bdd br)
{
    PcGroup g;
    if (u->conc)
        return;
    g.file = f;
    g.dir = (uint32_t)dir;
    g.pc = br;
    g.taint = u->taint;
    g.inst = mixh(u->chain, d->kw_loc);
    vec_push(&u->groups, g);
}

static PcOnce *once_of(PcUnit *u, SrcFile *f, const Skeleton *sk)
{
    PcOnce *o;
    size_t i;
    for (i = 0; i < u->once.len; i++)
        if (u->once.data[i]->file == f)
            return u->once.data[i];
    o = xcalloc(1, sizeof *o);
    o->file = f;
    for (i = 0; i < sk->dirs.len; i++)
        if (sk->dirs.data[i].kind == SK_PRAGMA_ONCE)
            o->has = true;
    vec_push(&u->once, o);
    return o;
}

static void fold_file(PcUnit *u, SrcFile *f, int from_dir, Bdd pc, int depth);

/* the header name of an #include; false when the operand is computed */
static bool include_name(const PcUnit *u, const SkDirective *d, char **name,
                         bool *angled)
{
    const Tok *t = d->toks.t;
    const SrcMgr *sm = u->an->sm;
    if (!d->toks.n)
        return false;
    if (t[0].kind == TK_STRING && pp_text(u->an->pp, &t[0])[0] == '"') {
        *angled = false;
        *name = arena_strndup(u->an->arena, pp_text(u->an->pp, &t[0]) + 1,
                              t[0].len - 2);
        return true;
    }
    if (tok_is_punct(&t[0], P_LT)) {
        const Tok *last = &t[d->toks.n - 1];
        SrcLoc p, e = last->loc + last->len;
        for (p = t[0].loc + 1; p < e; p++)
            if (sm->region[p] == '>') {
                *angled = true;
                *name = arena_strndup(u->an->arena, sm->region + t[0].loc + 1,
                                      p - (t[0].loc + 1));
                return true;
            }
    }
    return false;
}

/* fn_macro for a Macro: the same rules (no # or ..., no ## at an end; no
 * named variadic) */
static const PcFn *fn_of_macro(PcUnit *u, const Macro *m)
{
    Analysis *an = u->an;
    PcFn *f;
    uint32_t i;
    if (m->gnu_named_variadic || m->nparams < (m->variadic ? 1 : 0))
        return NULL;
    for (i = 0; i < m->body_len; i++)
        if (tok_is_punct(&m->body[i], P_HASH) ||
            tok_is_punct(&m->body[i], P_ELLIPSIS))
            return NULL;
    if (m->body_len && (tok_is_punct(&m->body[0], P_HASHHASH) ||
                        tok_is_punct(&m->body[m->body_len - 1], P_HASHHASH)))
        return NULL;
    f = NEW(an->arena, PcFn);
    f->params = m->params;
    f->np = (uint32_t)(m->variadic ? m->nparams - 1 : m->nparams);
    f->va = m->variadic;
    f->body.t = m->body;
    f->body.n = m->body_len;
    return f;
}

static void replay_define(PcUnit *u, const Macro *m, Bdd cur)
{
    intmax_t n = 0;
    bool known = false;
    PcNode *expr = NULL;
    const PcFn *fn = NULL;
    if (m->funclike) {
        fn = fn_of_macro(u, m);
    } else if (m->body_len) {
        TokSpan body = {m->body, m->body_len};
        known = pc_const_value(&u->cx, body, &n);
        if (!known)
            expr = pc_parse(&u->cx, body);
    }
    do_define(u, m->name, known, n,
              known ? NULL : opq_label(u->an->arena, m->name, m->name_loc),
              expr, fn, cur);
}

static int cmp_run(const void *a, const void *b)
{
    const PcRun *x = a, *y = b;
    return x->chain < y->chain ? -1 : x->chain > y->chain;
}

/* The events of the system header the real run entered at this chain;
 * false: it did not (the include was skipped, or is not reached). */
static bool replay_run(PcUnit *u, uint64_t chain, Bdd cur)
{
    PcTrace *t = u->an->pct;
    PcRun key, *r;
    uint32_t i;
    int depth = 0;
    if (!t || !t->runs.len)
        return false;
    if (!u->runs_sorted) {
        qsort(t->runs.data, t->runs.len, sizeof *t->runs.data, cmp_run);
        u->runs_sorted = true;
    }
    key.chain = chain;
    r = bsearch(&key, t->runs.data, t->runs.len, sizeof *t->runs.data, cmp_run);
    if (!r)
        return false;
    for (i = r->begin; i < r->end; i++) {
        const PcEv *e = &t->ev.data[i];
        switch (e->kind) {
        case EV_ENTER:
            u->files++;
            if (depth++)
                u->sys++;
            break;
        case EV_EXIT:
            depth--;
            break;
        case EV_DEF:
            replay_define(u, e->u.m, cur);
            break;
        case EV_UNDEF:
            do_undef(u, e->u.id, cur);
            break;
        case EV_MISS:
            u->unres++;
            u->taint = bdd_or(&u->cx.m, u->taint, cur);
            vec_push(&u->skipped, arena_printf(u->an->arena,
                                               e->angled ? "<%s>" : "\"%s\"",
                                               e->u.spelled));
            break;
        }
    }
    return true;
}

static void do_include(PcUnit *u, SrcFile *f, int from_dir,
                       const SkDirective *d, Bdd cur, int depth)
{
    char *name;
    bool angled = false, was;
    int di = -1;
    uint64_t chain;
    SrcFile *g;
    if (!include_name(u, d, &name, &angled)) {
        u->computed++;
        u->taint = bdd_or(&u->cx.m, u->taint, cur);
        vec_push(&u->skipped, "computed");
        return;
    }
    g = pp_resolve_include(u->an->pp, f, from_dir, name, angled, d->next, &di);
    if (!g) {
        u->unres++;
        u->taint = bdd_or(&u->cx.m, u->taint, cur);
        vec_push(&u->skipped, arena_printf(u->an->arena, angled ? "<%s>" : "\"%s\"", name));
        return;
    }
    was = u->conc;
    chain = u->chain;
    if (g->kind == SF_SYSTEM || g->system_header) {
        u->conc = true;
        u->sys++;
        if (!was) {
            size_t j, k;
            for (j = 0; j <= u->stk.len; j++) {   /* f, and the files above it */
                SrcFile *h = j == u->stk.len ? f : u->stk.data[j];
                for (k = 0; k < u->sysbelow.len && u->sysbelow.data[k] != h; k++)
                    ;
                if (k == u->sysbelow.len)
                    vec_push(&u->sysbelow, h);
            }
        }
    }
    u->chain = mixh(chain, d->hash_loc);
    vec_push(&u->stk, f);
    if (!(!was && u->conc && replay_run(u, u->chain, cur)))
        fold_file(u, g, di, cur, depth + 1);
    u->stk.len--;
    u->chain = chain;
    u->conc = was;
}

static void fold_file(PcUnit *u, SrcFile *f, int from_dir, Bdd pc, int depth)
{
    Skeleton *sk;
    PcOnce *once;
    BddMgr *m = &u->cx.m;
    VEC(PFrame) st = {0};
    size_t i;
    if (depth > PC_MAX_DEPTH || u->over)
        return;
    sk = an_skeleton(u->an, f);
    once = once_of(u, f, sk);
    if (once->has) {
        pc = bdd_and(m, pc, bdd_not(m, once->done));
        if (pc == BDD_FALSE)
            return;
    }
    u->files++;
    for (i = 0; i < sk->dirs.len && !m->over; i++) {
        const SkDirective *d = &sk->dirs.data[i];
        Bdd cur = st.len ? vec_last(&st).br : pc;
        PFrame fr;
        switch (d->kind) {
        case SK_IF: case SK_IFDEF: case SK_IFNDEF: {
            Bdd c = group_cond(u, d);
            fr.outer = cur;
            fr.prior = c;
            fr.br = bdd_and(m, cur, c);
            vec_push(&st, fr);
            record(u, f, i, d, fr.br);
            break;
        }
        case SK_ELIF: case SK_ELIFDEF: case SK_ELIFNDEF: {
            PFrame *t;
            Bdd c;
            if (!st.len)
                break;
            t = &vec_last(&st);
            c = group_cond(u, d);
            t->br = bdd_and(m, t->outer, bdd_and(m, bdd_not(m, t->prior), c));
            t->prior = bdd_or(m, t->prior, c);
            record(u, f, i, d, t->br);
            break;
        }
        case SK_ELSE: {
            PFrame *t;
            if (!st.len)
                break;
            t = &vec_last(&st);
            t->br = bdd_and(m, t->outer, bdd_not(m, t->prior));
            t->prior = BDD_TRUE;
            record(u, f, i, d, t->br);
            break;
        }
        case SK_ENDIF:
            if (st.len)
                st.len--;
            break;
        case SK_DEFINE:
            if (d->name) {
                intmax_t n = 0;
                const Tok *t = d->toks.t;
                bool known = false;
                PcNode *expr = NULL;
                const PcFn *fn = NULL;
                if (d->toks.n > 1 && !(tok_is_punct(&t[1], P_LPAREN) &&
                                       !(t[1].flags & TF_SPACE))) {
                    TokSpan body = {t + 1, d->toks.n - 1};
                    known = pc_const_value(&u->cx, body, &n);
                    if (!known)
                        expr = pc_parse(&u->cx, body);
                } else if (d->toks.n > 1 && tok_is_punct(&t[1], P_LPAREN)) {
                    fn = fn_macro(u, t, d->toks.n);
                }
                do_define(u, d->name, known, n,
                          known ? NULL
                                : opq_label(u->an->arena, d->name, d->kw_loc),
                          expr, fn, cur);
            }
            break;
        case SK_UNDEF:
            if (d->name)
                do_undef(u, d->name, cur);
            break;
        case SK_PRAGMA_ONCE:
            if (!u->conc)
                once->done = bdd_or(m, once->done, cur);
            break;
        case SK_INCLUDE:
            if (cur != BDD_FALSE)
                do_include(u, f, from_dir, d, cur, depth);
            break;
        default:
            break;
        }
    }
    vec_free(&st);
    if (m->over)
        u->over = true;
}

bool pc_fold(PcUnit *u, Analysis *a, SrcFile *main)
{
    memset(u, 0, sizeof *u);
    u->an_own = *a;
    u->an = &u->an_own;
    pc_init(&u->cx, u->an, PC_BDD_CAP);
    u->cx.env.ctx = u;
    u->cx.env.defined = env_defined;
    u->cx.env.values = env_values;
    if (a->pct) {                       /* the trace is spent once folded */
        memcpy(&u->taken, &a->pct->taken, sizeof u->taken);
        memset(&a->pct->taken, 0, sizeof a->pct->taken);
    }
    if (u->taken.len)
        qsort(u->taken.data, u->taken.len, sizeof *u->taken.data, cmp_u64);
    fold_file(u, main, -1, BDD_TRUE, 0);
    return !u->over;
}

void pc_unit_free(PcUnit *u)
{
    size_t i;
    for (i = 0; i < u->macros.len; i++) {
        vec_free(&u->macros.data[i]->vals);
        free(u->macros.data[i]);
    }
    for (i = 0; i < u->once.len; i++)
        free(u->once.data[i]);
    vec_free(&u->macros);
    vec_free(&u->once);
    vec_free(&u->groups);
    vec_free(&u->skipped);
    vec_free(&u->sysbelow);
    vec_free(&u->stk);
    vec_free(&u->taken);
    free(u->htab);
    pc_free(&u->cx);
}

void pc_unit_destroy(PcUnit *u)
{
    if (!u)
        return;
    pc_unit_free(u);
    free(u);
}

/* ---- dump ------------------------------------------------------------- */

static const char *kind_name(SkKind k)
{
    switch (k) {
    case SK_IF: return "if";
    case SK_IFDEF: return "ifdef";
    case SK_IFNDEF: return "ifndef";
    case SK_ELIF: return "elif";
    case SK_ELIFDEF: return "elifdef";
    case SK_ELIFNDEF: return "elifndef";
    default: return "else";
    }
}

static void put_literal(const PcCtx *cx, StrBuf *sb, uint32_t var, bool bit)
{
    const PcAtom *at = pc_atom_at(cx, var);
    if (!at) {
        sb_printf(sb, "%sv%u", bit ? "" : "!", var);
        return;
    }
    switch (at->kind) {
    case PA_DEF:
        sb_printf(sb, "%sdefined(%s)", bit ? "" : "!", at->id->str);
        break;
    case PA_GE:
        sb_printf(sb, "%s%s>=%jd", bit ? "" : "!", at->id->str, at->c);
        break;
    case PA_LEAF:
        sb_printf(sb, "%s%s=%s", bit ? "" : "!", at->text, pc_leaf_name(at));
        break;
    case PA_TEXT:
    case PA_HAS:
        sb_printf(sb, "%s[%s]", bit ? "" : "!", at->text);
        break;
    }
}

typedef struct Dnf {
    const PcCtx *cx;
    StrBuf *sb;
    VEC(uint32_t) path;     /* var * 2 + bit */
    int cubes;
} Dnf;

#define DNF_MAX 6

static void dnf_walk(Dnf *x, Bdd f)
{
    const BddNode *n;
    size_t i;
    if (f == BDD_FALSE || x->cubes > DNF_MAX)
        return;
    if (f == BDD_TRUE) {
        if (x->cubes++ == DNF_MAX)
            return;
        if (x->cubes > 1)
            sb_puts(x->sb, " | ");
        for (i = 0; i < x->path.len; i++) {
            if (i)
                sb_puts(x->sb, " & ");
            put_literal(x->cx, x->sb, x->path.data[i] >> 1,
                        x->path.data[i] & 1);
        }
        if (!x->path.len)
            sb_putc(x->sb, '1');
        return;
    }
    n = &x->cx->m.nodes.data[f];
    vec_push(&x->path, n->var * 2);
    dnf_walk(x, n->lo);
    x->path.len--;
    vec_push(&x->path, n->var * 2 + 1);
    dnf_walk(x, n->hi);
    x->path.len--;
}

static void put_formula(const PcCtx *cx, StrBuf *sb, Bdd f)
{
    Dnf x;
    x.cx = cx;
    x.sb = sb;
    x.path.data = NULL;
    x.path.len = x.path.cap = 0;
    x.cubes = 0;
    dnf_walk(&x, f);
    if (x.cubes == 0)
        sb_putc(sb, '0');
    else if (x.cubes > DNF_MAX)
        sb_puts(sb, " | ...");
    vec_free(&x.path);
}

/* the host valuation as a PcValuation: opaque atoms are not known */
static bool host_get(void *ctx, const PcAtom *at, bool *known)
{
    return host_atom(ctx, at, known);
}

void pc_dump(PcUnit *u, FILE *out, int flags)
{
    PcValuation hv = {u, host_get, NULL};
    size_t i, j, maxsize = 0;
    size_t tot[2][2][2] = {{{0}}};      /* [exact][tainted][oracle agrees] */
    size_t nunk[2] = {0, 0};            /* the host cannot value an atom: [exact] */
    size_t chk[3] = {0, 0, 0};          /* rank 1 against enumeration: ok, bad, skipped */
    for (i = 0; i < u->groups.len; i++) {
        const PcGroup *g = &u->groups.data[i];
        const SkDirective *d =
            &((Skeleton *)g->file->skel)->dirs.data[g->dir];
        BddVars vs = {0};
        StrBuf sb = {0};
        uint32_t line = 0, col;
        const char *base = strrchr(g->file->name, '/');
        bool exact = true, unk = false;
        int sat = pc_sat(&u->cx, g->pc);
        srcmgr_linecol(g->file, d->kw_loc, &line, &col);
        bdd_vars(&u->cx.m, g->pc, &vs);
        for (j = 0; j < vs.len; j++) {
            const PcAtom *at = pc_atom_at(&u->cx, vs.data[j]);
            if (at && pc_atom_pinned(at)) {
                bool known;
                host_atom(u, at, &known);
                exact &= at->kind == PA_HAS;
                unk |= !known;
            }
        }
        vec_free(&vs);
        put_formula(&u->cx, &sb, g->pc);
        int tn = g->taint < 2 ? (int)g->taint
                              : bdd_eval_fn(&u->cx.m, g->taint, host_var, u);
        int host = g->pc < 2 ? (int)g->pc
                             : bdd_eval_fn(&u->cx.m, g->pc, host_var, u);
        fprintf(out, "pc: %s:%u %s sat=%c host=%d exact=%d taint=%d",
                base ? base + 1 : g->file->name, line, kind_name(d->kind),
                sat < 0 ? '?' : '0' + sat, host, exact, tn);
        if (flags & PCD_SIZE) {
            size_t sz = bdd_count(&u->cx.m, g->pc);
            fprintf(out, " size=%zu", sz);
            maxsize = MAX(maxsize, sz);
        }
        if (flags & PCD_ORACLE) {
            bool active = was_taken(u, g->inst);
            if (unk) {
                fprintf(out, " oracle=unknown");
                nunk[exact]++;
            } else {
                fprintf(out, " oracle=%s", active == (host != 0) ? "ok" : "BAD");
                tot[exact][tn][active == (host != 0)]++;
            }
        }
        fprintf(out, " :: %s\n", sb_cstr(&sb));
        sb_free(&sb);
        if ((flags & PCD_CHECK) && sat == 1) {
            int c = pc_check_rank1(&u->cx, g->pc, &hv);
            chk[c < 0 ? 2 : c]++;
            if (c > 0)
                fprintf(out, "pc:   check BAD: rank 1 differs from enumeration\n");
        }
        if ((flags & PCD_ENABLE) && host == 0 && sat == 1) {
            StrBuf w = {0};
            fprintf(out, "pc:   enable %s\n",
                    pc_enable_group(u, g->pc, &w) ? sb_cstr(&w) : "none");
            sb_free(&w);
        }
        if ((flags & PCD_WHY) && host == 0 && sat == 1) {
            PcCube cubes[3];
            size_t nc = pc_preconditions(&u->cx, g->pc, &hv, 3, cubes), k;
            for (k = 0; k < nc; k++) {
                char *r = pc_cube_render(&cubes[k], true);
                fprintf(out, "pc:   why[%zu] target=%u cost=%u :: %s\n", k + 1,
                        cubes[k].target, cubes[k].cost, r);
                free(r);
                pc_cube_free(&cubes[k]);
            }
        }
    }
    if (flags & PCD_COVER) {
        PcCover cv;
        size_t k, m;
        pc_cover(u, 8, &cv);
        for (k = 0; k < cv.n; k++) {
            fprintf(out, "pc: cover[%zu] %s ::", k + 1, cv.items[k].words);
            for (m = 0; m < cv.items[k].n; m++) {
                const PcGroup *g = &u->groups.data[cv.items[k].groups[m]];
                const SkDirective *d =
                    &((Skeleton *)g->file->skel)->dirs.data[g->dir];
                uint32_t line, col;
                const char *base = strrchr(g->file->name, '/');
                srcmgr_linecol(g->file, d->kw_loc, &line, &col);
                fprintf(out, " %s:%u:%s", base ? base + 1 : g->file->name, line,
                        kind_name(d->kind));
            }
            fputc('\n', out);
        }
        fprintf(out, "pc: cover total eligible=%zu covered=%zu overlays=%zu\n",
                cv.eligible, cv.covered, cv.n);
        pc_cover_free(&cv);
    }
    fprintf(out, "pc: total groups=%zu files=%zu sys=%zu unresolved=%zu "
            "computed=%zu over=%d", u->groups.len, u->files, u->sys,
            u->unres, u->computed, u->over);
    if (flags & PCD_SIZE)
        fprintf(out, " table=%zu maxsize=%zu", bdd_size(&u->cx.m), maxsize);
    fputc('\n', out);
    if (flags & PCD_CHECK)
        fprintf(out, "pc: check rank1 ok=%zu bad=%zu skipped=%zu\n", chk[0],
                chk[1], chk[2]);
    if (flags & PCD_SKIPPED)
        for (i = 0; i < u->skipped.len; i++)
            fprintf(out, "pc: skipped %s\n", u->skipped.data[i]);
    if (flags & PCD_ORACLE)
        fprintf(out, "pc: oracle exact-untainted ok=%zu bad=%zu, "
                "exact-tainted ok=%zu bad=%zu, opaque-untainted ok=%zu bad=%zu, "
                "opaque-tainted ok=%zu bad=%zu, undecided=%zu "
                "exact-undecided=%zu\n",
                tot[1][0][1], tot[1][0][0],
                tot[1][1][1], tot[1][1][0], tot[0][0][1], tot[0][0][0],
                tot[0][1][1], tot[0][1][0], nunk[0] + nunk[1], nunk[1]);
}

/* ---- explaining one group -------------------------------------------- */

static bool group_head(SkKind k)
{
    return k <= SK_ELSE && k != SK_ENDIF;
}

typedef struct GroupAt {
    SrcLoc loc, best;
    uint32_t dir;
    bool header, found;
} GroupAt;

/* group g of the file runs up to end: is loc in it, and deeper than the best? */
static void group_close(GroupAt *x, const Skeleton *sk, uint32_t g, SrcLoc end)
{
    const SkDirective *h = &sk->dirs.data[g];
    if (h->hash_loc <= x->loc && x->loc < end && h->hash_loc >= x->best) {
        x->best = h->hash_loc;
        x->dir = g;
        x->header = x->loc < h->end_loc;
        x->found = true;
    }
}

/* The innermost conditional group of the file around loc: a group runs from
 * its #if/#elif/#else to the next directive of its conditional (or the end
 * of the file).  header: loc is on the directive itself rather than in the
 * group's text. */
static bool group_at(const Skeleton *sk, SrcLoc loc, uint32_t *dir, bool *header)
{
    VEC(uint32_t) open = {0};
    GroupAt x = {loc, 0, 0, false, false};
    size_t i;
    for (i = 0; i < sk->dirs.len; i++) {
        const SkDirective *d = &sk->dirs.data[i];
        if ((d->kind == SK_ENDIF || (d->kind >= SK_ELIF && d->kind <= SK_ELSE)) &&
            open.len) {
            group_close(&x, sk, vec_last(&open), d->hash_loc);
            open.len--;
        }
        if (group_head(d->kind))
            vec_push(&open, (uint32_t)i);
    }
    while (open.len) {                  /* unterminated */
        group_close(&x, sk, vec_last(&open), (SrcLoc)-1);
        open.len--;
    }
    vec_free(&open);
    *dir = x.dir;
    *header = x.header;
    return x.found;
}

static bool explain_fixed(void *ctx, const PcAtom *at);

/* The macro guards the header that defines it. */
static bool is_guard(const PcUnit *u, const Ident *id)
{
    const Macro *m = id ? mt_cur(u->an->pp->mt, id) : NULL;
    const Skeleton *sk = m && !m->predefined && m->file ? m->file->skel : NULL;
    return sk && sk->guard_define == id && sk->guard_covers_file;
}

/* The innermost group around loc: its directive, and the condition, taint and
 * activity of every inclusion of the file (or'd). */
static bool group_find(PcUnit *u, SrcFile *f, SrcLoc loc, uint32_t *dir, bool *header,
                       Bdd *pc, Bdd *taint, bool *active)
{
    BddMgr *m = &u->cx.m;
    bool found = false;
    size_t i;
    *pc = *taint = BDD_FALSE;
    *active = false;
    if (!f->skel || u->over || !group_at(f->skel, loc, dir, header))
        return false;
    for (i = 0; i < u->groups.len; i++) {
        const PcGroup *g = &u->groups.data[i];
        if (g->file != f || g->dir != *dir)
            continue;
        *pc = bdd_or(m, *pc, g->pc);    /* each time the file is included */
        *taint = bdd_or(m, *taint, g->taint);
        *active |= was_taken(u, g->inst);
        found = true;
    }
    return found && !m->over;
}

bool pc_explain(PcUnit *u, SrcFile *f, SrcLoc loc, PcExplain *e)
{
    PcValuation hv = {u, host_get, explain_fixed};
    const Skeleton *sk = f->skel;
    BddMgr *m = &u->cx.m;
    Bdd pc, taint;
    StrBuf sb = {0};
    uint32_t col;
    size_t i;
    memset(e, 0, sizeof *e);
    if (!group_find(u, f, loc, &e->dir, &e->header, &pc, &taint, &e->active))
        return false;
    e->file = f;
    e->kind = sk->dirs.data[e->dir].kind;
    srcmgr_linecol(f, sk->dirs.data[e->dir].kw_loc, &e->line, &col);
    e->exact = pc_exact(&u->cx, pc);
    e->tainted = taint != BDD_FALSE &&
                 (taint == BDD_TRUE || bdd_eval_fn(m, taint, host_var, u));
    e->sat = pc_sat(&u->cx, pc);
    put_formula(&u->cx, &sb, pc);
    e->cond = xstrdup(sb_cstr(&sb));
    sb_free(&sb);
    {
        const SkDirective *d = &sk->dirs.data[e->dir];
        const char *s = f->buf + (d->hash_loc - f->base);
        size_t n = 0, lim = d->end_loc > d->hash_loc ? d->end_loc - d->hash_loc : 0;
        while (n < lim && n < 100 && s[n] != '\n' && s[n] != '\r')
            n++;
        StrBuf tx = {0};
        bool cut = n < lim && s[n] != '\n' && s[n] != '\r';
        while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
            n--;
        sb_putn(&tx, s, n);
        if (cut)
            sb_puts(&tx, "...");
        e->text = xstrdup(sb_cstr(&tx));
        sb_free(&tx);
    }
    if (!e->active && e->sat == 1) {
        size_t k, keep = 0;
        e->nwhy = pc_preconditions(&u->cx, pc, &hv, 3, e->why);
        /* "some other OS" is no answer when the table names one */
        for (k = 0; k < e->nwhy; k++)
            keep += !e->why[k].residual;
        if (keep && keep < e->nwhy) {
            for (k = i = 0; k < e->nwhy; k++) {
                if (e->why[k].residual)
                    pc_cube_free(&e->why[k]);
                else
                    e->why[i++] = e->why[k];
            }
            e->nwhy = i;
        }
    }
    return true;
}

void pc_explain_free(PcExplain *e)
{
    size_t i;
    for (i = 0; i < e->nwhy; i++)
        pc_cube_free(&e->why[i]);
    free(e->cond);
    free(e->text);
    memset(e, 0, sizeof *e);
}

/* The guard of a header that includes a system header, directly or below: setting it
 * would make the unit skip that header, but the fold cannot say what else then
 * supplies what its system headers define (they are replayed from the real run, where a
 * later include of the same one was skipped by its own guard and read nothing).
 * s4_falsify.py found 14 of libuv's 54 refusals where `defined(UV_H)` was offered
 * and the group stayed off.  Never offered. */
static bool explain_fixed(void *ctx, const PcAtom *at)
{
    const PcUnit *u = ctx;
    const Macro *m = at->id ? mt_cur(u->an->pp->mt, at->id) : NULL;
    size_t i;
    if (!is_guard(u, at->id))
        return false;
    for (i = 0; i < u->sysbelow.len; i++)
        if (u->sysbelow.data[i] == m->file)
            return true;
    return false;
}

/* Where the unit defines the macro a requirement is about (the requirement
 * is on its value before the unit), the label of the requirement.  A macro
 * that guards the header defining it is said to skip that header: asking for
 * it keeps that header's own definitions out. */

/* ---- cover ------------------------------------------------------------- */

#define MAXOV 16

/* the host valuation with an overlay's words applied */
typedef struct OvVal {
    PcUnit *u;
    struct {
        const char *name;
        bool def;
        intmax_t val;
    } mac[MAXOV];
    int n, std, mode;
} OvVal;

static bool ov_get(void *ctx, const PcAtom *at, bool *known)
{
    OvVal *o = ctx;
    int i;
    if (at->kind == PA_LEAF && (o->std >= 0 || o->mode >= 0)) {
        int ax = pc_leaf_axis(at);
        int want = ax == pc_axis_by_name("std") ? o->std
                 : ax == pc_axis_by_name("mode") ? o->mode : -1;
        if (want >= 0) {
            *known = true;
            return at->c == (unsigned)want;
        }
    }
    if ((at->kind == PA_DEF || at->kind == PA_GE) && at->id)
        for (i = 0; i < o->n; i++)
            if (!strcmp(o->mac[i].name, at->id->str)) {
                *known = true;
                return at->kind == PA_DEF ? o->mac[i].def
                     : o->mac[i].def && o->mac[i].val >= at->c;
            }
    return host_atom(o->u, at, known);
}

/* words: an owned copy, which the overlay's names point into */
static void ov_parse(OvVal *o, char *words)
{
    char *w, *save = NULL;
    o->n = 0;
    o->std = o->mode = -1;
    for (w = strtok_r(words, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        if (!strncmp(w, "-std=", 5)) {
            pc_std_parse(w, &o->std, &o->mode);
        } else if ((w[1] == 'D' || w[1] == 'U') && o->n < MAXOV) {
            char *eq = strchr(w + 2, '=');
            if (eq)
                *eq = 0;
            o->mac[o->n].name = w + 2;
            o->mac[o->n].def = w[1] == 'D';
            o->mac[o->n].val = eq ? strtoimax(eq + 1, NULL, 10) : 1;
            o->n++;
        }
    }
}

/* the macro (or "-std") a word is about */
static size_t word_key(const char *w, char *buf, size_t cap)
{
    size_t n = 0;
    if (!strncmp(w, "-std=", 5)) {
        strcpy(buf, "-std");
        return 4;
    }
    for (w += 2; *w && *w != '=' && *w != ' ' && n + 1 < cap; w++)
        buf[n++] = *w;
    buf[n] = 0;
    return n;
}

/* The words of a and b together, or NULL when they set one name two ways. */
static char *ov_merge(const char *a, const char *b)
{
    StrBuf sb = {0};
    char *copy = xstrdup(b), *w, *save = NULL, *r;
    size_t nwords = 0;
    const char *q;
    sb_puts(&sb, a);
    for (w = strtok_r(copy, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
        char key[96], other[96];
        const char *p;
        bool same = false, clash = false;
        word_key(w, key, sizeof key);
        for (p = a; *p;) {
            size_t len = strcspn(p, " ");
            char one[96];
            if (len && len < sizeof one) {
                memcpy(one, p, len);
                one[len] = 0;
                word_key(one, other, sizeof other);
                if (!strcmp(key, other)) {
                    if (!strcmp(one, w))
                        same = true;
                    else
                        clash = true;
                }
            }
            p += len + (p[len] == ' ');
        }
        if (clash)
            break;
        if (!same) {
            sb_putc(&sb, ' ');
            sb_puts(&sb, w);
        }
    }
    if (w) {                        /* a clash */
        sb_free(&sb);
        free(copy);
        return NULL;
    }
    for (q = sb_cstr(&sb); *q; q++)
        nwords += *q == ' ';
    r = nwords + 1 > MAXOV ? NULL : xstrdup(sb_cstr(&sb));
    sb_free(&sb);
    free(copy);
    return r;
}

/* every group of the list holds under the overlay */
static bool ov_holds_all(PcUnit *u, const char *words, const size_t *g, size_t n)
{
    char *copy = xstrdup(words);
    OvVal o;
    size_t i;
    bool ok = true;
    o.u = u;
    ov_parse(&o, copy);
    for (i = 0; i < n && ok; i++) {
        PcValuation ov = {&o, ov_get, NULL};
        ok = pc_holds(&u->cx, u->groups.data[g[i]].pc, &ov);
    }
    free(copy);
    return ok;
}

static void item_free(PcCoverItem *it)
{
    free(it->words);
    free(it->groups);
}

static int item_cmp(const void *a, const void *b)
{
    const PcCoverItem *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : strcmp(x->words, y->words);
}

/* Fold overlays that agree into one while every group of both still holds. */
static void merge_items(PcUnit *u, PcCover *c)
{
    size_t i, j, k;
    for (i = 0; i < c->n; i++)
        for (j = i + 1; j < c->n;) {
            char *w = ov_merge(c->items[i].words, c->items[j].words);
            size_t *g;
            bool ok = false;
            if (w) {
                g = xmalloc((c->items[i].n + c->items[j].n) * sizeof *g);
                memcpy(g, c->items[i].groups, c->items[i].n * sizeof *g);
                memcpy(g + c->items[i].n, c->items[j].groups,
                       c->items[j].n * sizeof *g);
                ok = ov_holds_all(u, w, g, c->items[i].n + c->items[j].n);
                if (ok) {
                    k = c->items[i].n + c->items[j].n;
                    item_free(&c->items[i]);
                    item_free(&c->items[j]);
                    c->items[i].words = w;
                    c->items[i].groups = g;
                    c->items[i].n = k;
                    memmove(&c->items[j], &c->items[j + 1],
                            (c->n - j - 1) * sizeof *c->items);
                    c->n--;
                    j = i + 1;      /* the merged overlay meets the rest again */
                    continue;
                }
                free(g);
                free(w);
            }
            j++;
        }
}

void pc_cover(PcUnit *u, size_t max, PcCover *out)
{
    VEC(char *) cand = {0};
    VEC(size_t) elig = {0};
    uint64_t **cov;
    size_t i, c, round, words;
    bool *done;
    memset(out, 0, sizeof *out);
    for (i = 0; i < u->groups.len; i++) {
        const PcGroup *g = &u->groups.data[i];
        PcValuation hv = {u, host_get, NULL};
        StrBuf w = {0};
        if (pc_holds(&u->cx, g->pc, &hv) || !pc_exact(&u->cx, g->pc)) {
            sb_free(&w);
            continue;
        }
        if (pc_enable_group(u, g->pc, &w)) {
            for (c = 0; c < cand.len && strcmp(cand.data[c], sb_cstr(&w)); c++)
                ;
            if (c == cand.len)
                vec_push(&cand, xstrdup(sb_cstr(&w)));
            vec_push(&elig, i);
        }
        sb_free(&w);
    }
    out->eligible = elig.len;
    words = (elig.len + 63) / 64;
    cov = xcalloc(cand.len ? cand.len : 1, sizeof *cov);
    for (c = 0; c < cand.len; c++) {        /* what each overlay turns on */
        char *copy = xstrdup(cand.data[c]);
        OvVal o;
        o.u = u;
        ov_parse(&o, copy);
        cov[c] = xcalloc(words ? words : 1, sizeof **cov);
        for (i = 0; i < elig.len; i++) {
            PcValuation ov = {&o, ov_get, NULL};
            if (pc_holds(&u->cx, u->groups.data[elig.data[i]].pc, &ov))
                cov[c][i / 64] |= (uint64_t)1 << (i % 64);
        }
        free(copy);
    }
    done = xcalloc(elig.len ? elig.len : 1, sizeof *done);
    out->items = xcalloc(cand.len ? cand.len : 1, sizeof *out->items);
    for (round = 0; round < cand.len; round++) {
        size_t best = 0, bestc = 0, gain;
        for (c = 0; c < cand.len; c++) {
            gain = 0;
            for (i = 0; i < elig.len; i++)
                gain += !done[i] && (cov[c][i / 64] >> (i % 64) & 1);
            if (gain > best) {
                best = gain;
                bestc = c;
            }
        }
        if (!best)
            break;
        {
            PcCoverItem *it = &out->items[out->n++];
            it->words = xstrdup(cand.data[bestc]);
            it->groups = xmalloc(best * sizeof *it->groups);
            for (i = 0; i < elig.len; i++)
                if (!done[i] && (cov[bestc][i / 64] >> (i % 64) & 1)) {
                    done[i] = true;
                    it->groups[it->n++] = elig.data[i];
                }
        }
    }
    merge_items(u, out);
    qsort(out->items, out->n, sizeof *out->items, item_cmp);
    for (round = max; round < out->n; round++)
        item_free(&out->items[round]);
    if (out->n > max)
        out->n = max;
    for (round = 0; round < out->n; round++)
        out->covered += out->items[round].n;
    for (c = 0; c < cand.len; c++) {
        free(cov[c]);
        free(cand.data[c]);
    }
    free(cov);
    free(done);
    vec_free(&cand);
    vec_free(&elig);
}

void pc_cover_free(PcCover *c)
{
    size_t i;
    for (i = 0; i < c->n; i++) {
        free(c->items[i].words);
        free(c->items[i].groups);
    }
    free(c->items);
    memset(c, 0, sizeof *c);
}

/* The unit defines the macro itself: a test before that definition is a guard
 * or a default, and an overlay would change what the unit reads or defines. */
static bool unit_defines(const PcUnit *u, const PcReq *q)
{
    const Macro *m = q->id ? mt_cur(u->an->pp->mt, q->id) : NULL;
    return m && !m->predefined && !m->cfg && m->file;
}

/* no overlay for a macro the unit defines (a guard changes which files are read) */
static bool unit_enable(PcUnit *u, const PcCube *c, const PcValuation *v,
                        StrBuf *sb)
{
    uint32_t i;
    for (i = 0; i < c->n; i++)
        if (c->reqs[i].status != PQ_MET && unit_defines(u, &c->reqs[i]))
            return false;
    return pc_enable(&u->cx, c, v, sb);
}

/* What an overlay cannot change: the target, and a macro the unit defines itself. */
static bool enable_fixed(void *ctx, const PcAtom *at)
{
    const PcUnit *u = ctx;
    const Macro *m = at->id ? mt_cur(u->an->pp->mt, at->id) : NULL;
    return at->cls == PC_TARGET || (m && !m->predefined && !m->cfg && m->file);
}

bool pc_enable_group(PcUnit *u, Bdd pc, StrBuf *sb)
{
    PcValuation hv = {u, host_get, enable_fixed};
    PcCube cubes[3];
    size_t n = pc_preconditions(&u->cx, pc, &hv, 3, cubes), k;
    bool ok = false;
    for (k = 0; k < n; k++) {
        /* the first cube that works, but a plain `#if X` also has a "X below 0" cube:
         * a nonnegative value is what anyone would write */
        if (!ok || strstr(sb_cstr(sb), "=-")) {
            StrBuf w = {0};
            if (unit_enable(u, &cubes[k], &hv, &w) && (!ok || !strstr(sb_cstr(&w), "=-"))) {
                sb->len = 0;
                sb_puts(sb, sb_cstr(&w));
                ok = true;
            }
            sb_free(&w);
        }
        pc_cube_free(&cubes[k]);
    }
    return ok;
}

size_t pc_enable_edits(PcUnit *u, SrcFile *f, SrcLoc loc, PcEdit *out, size_t max)
{
    Bdd pc, taint;
    uint32_t dir;
    bool header, active;
    StrBuf w = {0};
    char *words, *tok, *save = NULL;
    size_t n = 0;
    if (!group_find(u, f, loc, &dir, &header, &pc, &taint, &active) || active ||
        !pc_exact(&u->cx, pc) || pc_sat(&u->cx, pc) != 1 || !pc_enable_group(u, pc, &w)) {
        sb_free(&w);
        return 0;
    }
    words = xstrdup(sb_cstr(&w));
    sb_free(&w);
    for (tok = strtok_r(words, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
        char *eq = strchr(tok, '=');
        const char *val = eq ? eq + 1 : "1";
        const Macro *m;
        Ident *id;
        if (tok[0] != '-' || tok[1] != 'D' || n == max)
            goto none;                  /* -U, -std: not a line of the header */
        if (eq)
            *eq = 0;
        id = intern_find(u->an->in, tok + 2, strlen(tok + 2));
        m = id ? mt_cur(u->an->pp->mt, id) : NULL;
        if (!m || !m->cfg || m->funclike || !m->file || m->end_loc < m->name_loc + id->len)
            goto none;                  /* only a macro the header defines */
        out[n].file = m->file;
        out[n].name = id->str;
        out[n].begin = m->name_loc + id->len - m->file->base;
        out[n].end = m->end_loc - m->file->base;
        out[n].text = xmalloc(strlen(val) + 2);
        sprintf(out[n].text, " %s", val);
        n++;
    }
    free(words);
    return n;
none:
    while (n)
        free(out[--n].text);
    free(words);
    return 0;
}

bool pc_req_via(const PcUnit *u, const PcReq *q, StrBuf *sb)
{
    const Macro *m = q->id ? mt_cur(u->an->pp->mt, q->id) : NULL;
    uint32_t line, col;
    if (!m || m->predefined || m->cfg || !m->file)
        return false;
    if (is_guard(u, q->id)) {
        sb_printf(sb, "the guard of %s: it makes the unit skip that header",
                  m->file->path);
        return true;
    }
    srcmgr_linecol(m->file, m->name_loc, &line, &col);
    sb_printf(sb, "the unit defines `%s` at %s:%u", q->id->str, m->file->path,
              line);
    return true;
}

/* The unmet requirements of one way to turn a group on, "target `X`; `Y` (unknown)". */
static void put_cube(const PcUnit *u, const PcCube *c, StrBuf *sb)
{
    static const char *const label[] = {"", "target ", "environment ", "unknown ",
                                        "language "};
    uint32_t j;
    bool first = true;
    StrBuf via = {0};
    for (j = 0; j < c->n; j++) {
        const PcReq *q = &c->reqs[j];
        if (q->status == PQ_MET)
            continue;
        sb_printf(sb, "%s%s`%s`%s", first ? "" : "; ", label[q->kind],
                  q->clause, q->status == PQ_UNKNOWN ? " (unknown)" : "");
        via.len = 0;
        if (pc_req_via(u, q, &via))
            sb_printf(sb, " (%s)", sb_cstr(&via));
        first = false;
    }
    sb_free(&via);
}

void pc_explain_needs(const PcUnit *u, const PcExplain *e, StrBuf *sb)
{
    size_t i;
    sb_printf(sb, "`%s`: ", e->text);       /* "os = other" alone does not say which */
    if (e->sat == 0) {
        sb_puts(sb, "its condition cannot hold");
        return;
    }
    if (!e->nwhy) {
        sb_puts(sb, e->exact ? "no way to turn it on was found"
                             : "it depends on a test the analysis cannot evaluate");
        return;
    }
    sb_puts(sb, "to compile it: ");
    for (i = 0; i < e->nwhy; i++) {
        if (i)
            sb_puts(sb, " or ");
        put_cube(u, &e->why[i], sb);
    }
}

void pc_explain_render(const PcUnit *u, const PcExplain *e, StrBuf *sb)
{
    size_t i;
    sb_printf(sb, "%s: `%s` (%s:%u)", e->active ? "**Compiled**"
              : e->sat == 0 ? "**Never compiled**" : "**Not compiled**", e->text,
              e->file->path, e->line);
    sb_puts(sb, e->active ? " is taken in this build"
            : e->sat == 0 ? ": its condition cannot hold"
                          : " is skipped in this build");
    if (!e->exact)
        sb_puts(sb, "\n\n(it depends on a test the analysis cannot evaluate)");
    if (e->tainted)
        sb_puts(sb, "\n\n(an earlier `#include` was not followed: this may differ)");
    if (e->cond && strlen(e->cond) <= 100)
        sb_printf(sb, "\n\nCondition: `%s`", e->cond);
    if (e->nwhy)
        sb_puts(sb, "\n\nTo compile it:");
    for (i = 0; i < e->nwhy; i++) {
        sb_printf(sb, "\n%zu. ", i + 1);
        put_cube(u, &e->why[i], sb);
    }
}
