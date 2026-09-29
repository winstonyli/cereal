/* include.c - include graph and macro-usage analyses. */
#include "analysis.h"
#include "../cell.h"

#include <string.h>

typedef struct FileInfo {
    bool used;              /* a macro defined here was referenced */
    bool entered;
    bool collided;          /* skipped because of a guard collision */
    int ok_includes;        /* times entered through #include */
    SrcLoc first_include;   /* first #include that entered it */
} FileInfo;

typedef struct Edge {
    SrcFile *from, *to;
    SrcLoc hash_loc;
    IncludeResult result;
    int from_instance;      /* which inclusion of `from` */
} Edge;

typedef struct UseLog {
    uint32_t key;
    Macro *m;
} UseLog;

struct IncludeState {
    VEC(UseLog) uses;       /* parallel worker: expansions, for the join */
    VEC(Edge) edges;
    VEC(SrcFile *) stack;   /* current include stack */
    VEC(int) instance;      /* inclusion instance ids, parallel to stack */
    int next_instance;
};

static FileInfo *fi(Analysis *a, SrcFile *f)
{
    if (!f->user)
        f->user = NEW(a->arena, FileInfo);
    (void)a;
    return f->user;
}

static void on_include(void *ctx, const IncludeEvent *ev)
{
    Analysis *a = ctx;
    IncludeState *s = a->inc;
    Edge e;
    size_t i;
    if (!ev->file)
        return;
    e.from = ev->from;
    e.to = ev->file;
    e.hash_loc = ev->hash_loc;
    e.result = ev->result;
    e.from_instance = s->instance.len ? vec_last(&s->instance) : 0;
    /* a redundant second include of a guarded header from the same file */
    if (ev->result != INC_OK && an_user_file(ev->from)) {
        for (i = 0; i < s->edges.len; i++) {
            Edge *p = &s->edges.data[i];
            if (p->from == e.from && p->to == e.to &&
                p->from_instance == e.from_instance) {
                Diagnostic *d = diag_report(a->diag, DL_WARNING,
                    "duplicate-include", ev->hash_loc,
                    "'%s' is already included by this file; this #include "
                    "has no effect", ev->spelled);
                diag_note(a->diag, d, p->hash_loc, "previously included here");
                break;
            }
        }
    }
    vec_push(&s->edges, e);
}

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Analysis *a = ctx;
    IncludeState *s = a->inc;
    FileInfo *info = fi(a, f);
    size_t i;
    if (via && an_user_file(via->from)) {
        for (i = 0; i < s->stack.len; i++)
            if (s->stack.data[i] == f) {
                Diagnostic *d = diag_report(a->diag, DL_REMARK, "include-cycle",
                    via->hash_loc, "include cycle: '%s' includes itself "
                    "(directly or indirectly)", f->name);
                (void)d;
                break;
            }
    }
    if (via) {
        info->ok_includes++;
        if (!info->first_include)
            info->first_include = via->hash_loc;
    }
    /* guard collision: the guard macro is already defined by another file
     * the first time this header is entered */
    if (via && !info->entered && an_user_file(f)) {
        Skeleton *sk = an_skeleton(a, f);
        Macro *m = sk->guard_ifndef ? mt_cur(a->pp->mt, sk->guard_ifndef) : NULL;
        if (m && sk->guard_covers_file && m->file != f && !m->predefined) {
            Diagnostic *d = diag_report(a->diag, DL_WARNING, "guard-collision",
                sk->dirs.data[0].kw_loc,
                "include guard '%s' is already defined by '%s'; the contents "
                "of this header are silently skipped", m->name->str,
                m->file->name);
            diag_note(a->diag, d, m->name_loc, "'%s' defined here",
                      m->name->str);
            info->collided = true;
        }
    }
    info->entered = true;
    vec_push(&s->stack, f);
    vec_push(&s->instance, s->next_instance++);
}

static void on_file_exit(void *ctx, SrcFile *f)
{
    Analysis *a = ctx;
    (void)f;
    if (a->inc->stack.len) {
        a->inc->stack.len--;
        a->inc->instance.len--;
    }
}

static void mark_used(Analysis *a, Macro *m)
{
    if (m && m->file)
        fi(a, m->file)->used = true;
}

static void on_expand(void *ctx, const Expansion *e, const TokSpan *args,
                      int nargs)
{
    (void)args;
    (void)nargs;
    mark_used(ctx, e->macro);
}

static void on_macro_ref(void *ctx, Ident *id, Macro *m, const Tok *tok,
                         RefKind kind)
{
    (void)id;
    (void)tok;
    if (kind == REF_IFDEF || kind == REF_DEFINED)
        mark_used(ctx, m);
}

static void log_use(void *ctx, const Expansion *e, const TokSpan *args,
                    int nargs)
{
    Analysis *w = ctx;
    UseLog u;
    (void)args;
    (void)nargs;
    u.key = pp_event_key(w->pp);
    u.m = e->macro;
    vec_push(&w->inc->uses, u);
}

/* Parallel runs: #include and #if events arrive in phase A; a worker only
 * logs which macros its text expanded. */
void *include_fork(Analysis *a, Analysis *w)
{
    PPListener l;
    (void)a;
    w->inc = NEW(w->arena, IncludeState);
    memset(&l, 0, sizeof l);
    l.ctx = w;
    l.expand = log_use;
    pp_add_listener(w->pp, l);
    return w->inc;
}

void include_join(Analysis *a, Analysis *w, uint32_t from, uint32_t to)
{
    size_t i;
    for (i = 0; i < w->inc->uses.len; i++)
        if (w->inc->uses.data[i].key >= from && w->inc->uses.data[i].key < to)
            mark_used(a, w->inc->uses.data[i].m);
}

/* Cells: the definitions a slice expanded (each once). */
typedef struct IncBlob {
    uint32_t n;
    uint32_t *m;
} IncBlob;

void *include_encode(Analysis *w, uint32_t from, uint32_t to, CellEnc *e)
{
    IncBlob *b = NEW(cenc_arena(e), IncBlob);
    VEC(uint32_t) v = {0};
    size_t i, k, lo = 0, hi = w->inc->uses.len;
    while (lo < hi) { /* the log is in plan order */
        size_t mid = lo + (hi - lo) / 2;
        if (w->inc->uses.data[mid].key < from)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (i = lo; i < w->inc->uses.len; i++) {
        const UseLog *u = &w->inc->uses.data[i];
        uint32_t m;
        bool dup = false;
        if (u->key >= to)
            break;
        m = cenc_macro(e, u->m);
        for (k = v.len; k-- > 0 && !dup;) /* repeats are usually recent */
            dup = v.data[k] == m;
        if (!dup)
            vec_push(&v, m);
    }
    b->n = (uint32_t)v.len;
    b->m = NEW_ARRAY(cenc_arena(e), uint32_t, v.len + 1);
    if (v.len)
        memcpy(b->m, v.data, sizeof(uint32_t) * v.len);
    vec_free(&v);
    return b;
}

void include_decode(Analysis *w, const void *blob, const CellDec *d)
{
    const IncBlob *b = blob;
    uint32_t i;
    w->inc = NEW(w->arena, IncludeState);
    for (i = 0; i < b->n; i++) {
        UseLog u;
        u.key = cdec_item(d, 0);
        u.m = cdec_macro(d, b->m[i]);
        vec_push(&w->inc->uses, u);
    }
}

void include_release(Analysis *w)
{
    vec_free(&w->inc->uses);
}

void include_attach(Analysis *a)
{
    PPListener l;
    memset(&l, 0, sizeof l);
    a->inc = NEW(a->arena, IncludeState);
    l.ctx = a;
    l.include = on_include;
    l.file_enter = on_file_enter;
    l.file_exit = on_file_exit;
    l.expand = on_expand;
    l.macro_ref = on_macro_ref;
    pp_add_listener(a->pp, l);
}

static bool name_has_suffix(const char *s, const char *suf)
{
    size_t n = strlen(s), m = strlen(suf);
    return n >= m && !strcmp(s + n - m, suf);
}

static void check_header(Analysis *a, SrcFile *f)
{
    Skeleton *sk = an_skeleton(a, f);
    FileInfo *info = f->user;

    /* #ifndef FOO_H / #define FOO_HH */
    if (sk->guard_ifndef && sk->guard_define &&
        sk->guard_ifndef != sk->guard_define) {
        Ident *g = sk->guard_ifndef, *d = sk->guard_define;
        unsigned lim = MAX(2u, g->len / 4);
        if (edit_distance(g->str, g->len, d->str, d->len, lim) <= lim) {
            Diagnostic *dg = diag_report(a->diag, DL_WARNING, "header-guard",
                sk->dirs.data[1].kw_loc,
                "'%s' is used as a header guard here, followed by #define of "
                "a different macro '%s'", g->str, d->str);
            diag_note(a->diag, dg, sk->dirs.data[0].kw_loc,
                      "'%s' is tested here; did you mean to define it?", g->str);
            if (dg)
                dg->fixit = g->str;
        }
    }

    /* missing guard: skip headers that are clearly meant for multiple
     * inclusion (X-macro tables) */
    if (!f->pragma_once && !(sk->guard_ifndef && sk->guard_covers_file) &&
        info && info->ok_includes == 1 && sk->ntokens + sk->dirs.len > 0 &&
        !name_has_suffix(f->name, ".def") && !name_has_suffix(f->name, ".inc") &&
        !name_has_suffix(f->name, ".x")) {
        diag_report(a->diag, DL_WARNING, "missing-header-guard", f->base,
                    "header '%s' has no include guard or #pragma once",
                    f->name);
    }
}

void include_finish(Analysis *a)
{
    IncludeState *s = a->inc;
    PP *pp = a->pp;
    size_t i, j;

    for (i = 0; i < srcmgr_nfiles(a->sm); i++) {
        SrcFile *f = srcmgr_file(a->sm, (uint32_t)i);
        if (f != pp->main_file && an_user_file(f) && f->user &&
            ((FileInfo *)f->user)->entered)
            check_header(a, f);
    }

    /* includes of macro-only headers whose macros are never used */
    for (i = 0; i < s->edges.len; i++) {
        Edge *e = &s->edges.data[i];
        Skeleton *sk;
        bool nested = false, dup = false;
        if (e->result != INC_OK || !an_user_file(e->from) ||
            !an_user_file(e->to))
            continue;
        for (j = 0; j < i; j++)
            if (s->edges.data[j].to == e->to && s->edges.data[j].result == INC_OK)
                dup = true;
        if (dup)
            continue;
        sk = an_skeleton(a, e->to);
        for (j = 0; j < sk->dirs.len; j++)
            if (sk->dirs.data[j].kind == SK_INCLUDE)
                nested = true;
        if (nested || sk->ntokens > 0 || !e->to->user ||
            ((FileInfo *)e->to->user)->used ||
            ((FileInfo *)e->to->user)->collided)
            continue;
        {
            bool defines = false;
            for (j = 0; j < sk->dirs.len; j++)
                if (sk->dirs.data[j].kind == SK_DEFINE &&
                    sk->dirs.data[j].name != sk->guard_ifndef)
                    defines = true;
            if (!defines)
                continue;
        }
        diag_report(a->diag, DL_WARNING, "unused-include", e->hash_loc,
                    "no macro from '%s' is used; this #include has no effect",
                    e->to->name);
    }

    /* unused macros in the main file (like -Wunused-macros) */
    for (i = 0; i < pp->macros.len; i++) {
        Macro *m = pp->macros.data[i];
        Skeleton *sk;
        if (m->file != pp->main_file || m->predefined || m->builtin ||
            m->expansions || m->cond_refs)
            continue;
        sk = an_skeleton(a, m->file);
        if (sk->guard_define == m->name)
            continue;
        diag_report(a->diag, DL_WARNING, "unused-macros", m->name_loc,
                    "macro '%s' is defined but never used", m->name->str);
    }

    vec_free(&s->edges);
    vec_free(&s->stack);
    vec_free(&s->instance);
}

void include_discard(Analysis *a)
{
    vec_free(&a->inc->uses);
    vec_free(&a->inc->edges);
    vec_free(&a->inc->stack);
    vec_free(&a->inc->instance);
}
