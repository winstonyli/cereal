/* pc.c - the symbolic fold: presence conditions of every group of a unit. */
#include "pc.h"
#include "pcq.h"

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
 * Single-threaded runs only. */
static VEC(uint64_t) taken;
static VEC(uint64_t) chains;        /* the chain at each open file */

static uint64_t mixh(uint64_t h, uint32_t x)
{
    h = (h ^ x) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

static void trace_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    uint64_t c = chains.len ? vec_last(&chains) : 0;
    (void)ctx;
    (void)f;
    vec_push(&chains, via && via->hash_loc ? mixh(c, via->hash_loc) : c);
}

static void trace_exit(void *ctx, SrcFile *f)
{
    (void)ctx;
    (void)f;
    if (chains.len)
        chains.len--;
}

static void trace_cond(void *ctx, const CondEvent *ev)
{
    (void)ctx;
    if (ev->taken && ev->kind != COND_ENDIF && chains.len)
        vec_push(&taken, mixh(vec_last(&chains), ev->kw_loc));
}

void pc_trace_attach(Analysis *a)
{
    PPListener l;
    memset(&l, 0, sizeof l);
    l.file_enter = trace_enter;
    l.file_exit = trace_exit;
    l.cond = trace_cond;
    pp_add_listener(a->pp, l);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static bool was_taken(uint64_t inst)
{
    return bsearch(&inst, taken.data, taken.len, sizeof *taken.data,
                   cmp_u64) != NULL;
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
           m->body[0].kind == TK_PPNUM && pc_ppnum(a, &m->body[0], n);
}

bool pc_host_atom(PcUnit *u, const PcAtom *at)
{
    const Macro *m;
    intmax_t n;
    if (!at || at->kind == PA_TEXT)
        return false;
    m = host_macro(u, at->id);
    if (at->kind == PA_DEF)
        return m != NULL;
    return macro_number(u->an, m, &n) && n >= at->c;
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
    e.cond = s->def;
    e.known = false;
    e.n = 0;
    e.opq = NULL;
    e.expr = NULL;
    e.fn = NULL;
    vec_push(&s->vals, e);
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
 * variadics, # or ## (the call stays opaque).  t is the #define's tokens. */
static const PcFn *fn_macro(PcUnit *u, const Tok *t, uint32_t n)
{
    Analysis *an = u->an;
    Ident **ps = NULL;
    PcFn *f;
    uint32_t np = 0, i = 2;
    if (n < 3)
        return NULL;
    ps = NEW_ARRAY(an->arena, Ident *, n);
    if (!tok_is_punct(&t[i], P_RPAREN)) {
        for (;; i++) {
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
        if (tok_is_punct(&t[k], P_HASH) || tok_is_punct(&t[k], P_HASHHASH) ||
            tok_is_punct(&t[k], P_ELLIPSIS))
            return NULL;
    f = NEW(an->arena, PcFn);
    f->params = ps;
    f->np = np;
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
        return was_taken(mixh(u->chain, d->kw_loc)) ? BDD_TRUE : BDD_FALSE;
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
        u->taint |= PC_T_COMPUTED;
        vec_push(&u->skipped, "computed");
        return;
    }
    g = pp_resolve_include(u->an->pp, f, from_dir, name, angled, d->next, &di);
    if (!g) {
        u->unres++;
        u->taint |= angled ? PC_T_ANGLED : PC_T_QUOTED;
        vec_push(&u->skipped, arena_printf(u->an->arena, angled ? "<%s>" : "\"%s\"", name));
        return;
    }
    was = u->conc;
    chain = u->chain;
    if (g->kind == SF_SYSTEM || g->system_header) {
        u->conc = true;
        u->sys++;
    }
    u->chain = mixh(chain, d->hash_loc);
    fold_file(u, g, di, cur, depth + 1);
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
                                : arena_printf(u->an->arena, "%s@%u",
                                               d->name->str, d->kw_loc),
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
    u->an = a;
    pc_init(&u->cx, a, PC_BDD_CAP);
    u->cx.env.ctx = u;
    u->cx.env.defined = env_defined;
    u->cx.env.values = env_values;
    qsort(taken.data, taken.len, sizeof *taken.data, cmp_u64);
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
    vec_free(&taken);
    vec_free(&chains);
    free(u->htab);
    pc_free(&u->cx);
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
    case PA_TEXT:
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
    *known = at->kind != PA_TEXT;
    return *known && pc_host_atom(ctx, at);
}

void pc_dump(PcUnit *u, FILE *out, int flags)
{
    PcValuation hv = {u, host_get};
    size_t i, j, maxsize = 0;
    size_t tot[2][2][2] = {{{0}}};      /* [exact][tainted][oracle agrees] */
    for (i = 0; i < u->groups.len; i++) {
        const PcGroup *g = &u->groups.data[i];
        const SkDirective *d =
            &((Skeleton *)g->file->skel)->dirs.data[g->dir];
        BddVars vs = {0};
        StrBuf sb = {0};
        uint32_t line = 0, col;
        const char *base = strrchr(g->file->name, '/');
        bool exact = true;
        int sat = pc_sat(&u->cx, g->pc);
        srcmgr_linecol(g->file, d->kw_loc, &line, &col);
        bdd_vars(&u->cx.m, g->pc, &vs);
        for (j = 0; j < vs.len; j++) {
            const PcAtom *at = pc_atom_at(&u->cx, vs.data[j]);
            if (at && at->kind == PA_TEXT)
                exact = false;
        }
        vec_free(&vs);
        put_formula(&u->cx, &sb, g->pc);
        int host = g->pc < 2 ? (int)g->pc
                             : bdd_eval_fn(&u->cx.m, g->pc, host_var, u);
        fprintf(out, "pc: %s:%u %s sat=%c host=%d exact=%d taint=%u",
                base ? base + 1 : g->file->name, line, kind_name(d->kind),
                sat < 0 ? '?' : '0' + sat, host, exact, g->taint);
        if (flags & PCD_SIZE) {
            size_t sz = bdd_count(&u->cx.m, g->pc);
            fprintf(out, " size=%zu", sz);
            maxsize = MAX(maxsize, sz);
        }
        if (flags & PCD_ORACLE) {
            bool active = was_taken(g->inst);
            fprintf(out, " oracle=%s", active == (host != 0) ? "ok" : "BAD");
            tot[exact][g->taint != 0][active == (host != 0)]++;
        }
        fprintf(out, " :: %s\n", sb_cstr(&sb));
        sb_free(&sb);
        if ((flags & PCD_WHY) && host == 0 && sat == 1) {
            PcCube cubes[3];
            size_t nc = pc_preconditions(&u->cx, g->pc, &hv, 3, cubes), k;
            for (k = 0; k < nc; k++) {
                char *r = pc_cube_render(&cubes[k], true);
                fprintf(out, "pc:   why[%zu] cost=%u :: %s\n", k + 1,
                        cubes[k].cost, r);
                free(r);
                pc_cube_free(&cubes[k]);
            }
        }
    }
    fprintf(out, "pc: total groups=%zu files=%zu sys=%zu unresolved=%zu "
            "computed=%zu over=%d", u->groups.len, u->files, u->sys,
            u->unres, u->computed, u->over);
    if (flags & PCD_SIZE)
        fprintf(out, " table=%zu maxsize=%zu", bdd_size(&u->cx.m), maxsize);
    fputc('\n', out);
    if (flags & PCD_SKIPPED)
        for (i = 0; i < u->skipped.len; i++)
            fprintf(out, "pc: skipped %s\n", u->skipped.data[i]);
    if (flags & PCD_ORACLE)
        fprintf(out, "pc: oracle exact-untainted ok=%zu bad=%zu, "
                "exact-tainted ok=%zu bad=%zu, opaque-untainted ok=%zu bad=%zu, "
                "opaque-tainted ok=%zu bad=%zu\n", tot[1][0][1], tot[1][0][0],
                tot[1][1][1], tot[1][1][0], tot[0][0][1], tot[0][0][0],
                tot[0][1][1], tot[0][1][0]);
}
