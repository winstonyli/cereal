/* index.c - the LSP-facing model of a translation unit's macros. */
#include "index.h"
#include "json.h"

#include <string.h>

/* ---- recording ------------------------------------------------------ */

static bool loc_is_system(Index *ix, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(ix->sm, loc);
    return !f || f->system_header || f->kind != SF_USER;
}

static void add_ref(Index *ix, Ident *name, Macro *m, SrcLoc loc, uint32_t len,
                    RefKind kind, unsigned flags, Expansion *e)
{
    IdxRef r;
    if (!loc)
        return;
    r.name = name;
    r.macro = m;
    r.loc = loc;
    r.len = len;
    r.kind = kind;
    r.flags = flags | (loc_is_system(ix, loc) ? IREF_SYSTEM : 0);
    r.exp = e;
    vec_push(&ix->refs, r);
    ix->sorted = false;
}

static IdxExp *exp_of(Index *ix, Expansion *e)
{
    while (ix->exps.len <= e->id)
        vec_push(&ix->exps, NULL);
    if (!ix->exps.data[e->id]) {
        IdxExp *x = NEW(ix->arena, IdxExp);
        x->e = e;
        ix->exps.data[e->id] = x;
    }
    return ix->exps.data[e->id];
}

static void on_expand(void *ctx, const Expansion *ce, const TokSpan *args,
                      int nargs)
{
    Index *ix = ctx;
    Expansion *e = (Expansion *)ce;
    IdxExp *x;
    unsigned flags = 0;
    SrcLoc loc;
    int i;
    if (!e)
        return;
    x = exp_of(ix, e);
    loc = e->name_loc;
    x->root = ix->pp->expansions.data[e->root];
    x->depth = e->depth;
    x->nargs = nargs;
    if (nargs) {
        x->args = NEW_ARRAY(ix->arena, char *, nargs);
        for (i = 0; i < nargs; i++)
            x->args[i] = tokens_str(ix->pp, args[i]);
    }
    if (e->name_flags & TF_ORIGIN_BODY)
        flags |= IREF_IN_BODY;
    if (e->name_flags & TF_ORIGIN_ARG)
        flags |= IREF_FROM_ARG;
    if (e->name_flags & TF_PASTED)
        flags |= IREF_PASTED;
    if (flags & (IREF_IN_BODY | IREF_PASTED)) {
        /* dedupe: one ref per (location, macro) */
        size_t k;
        for (k = ix->refs.len; k-- > 0 && k + 64 > ix->refs.len;)
            if (ix->refs.data[k].loc == loc && ix->refs.data[k].macro == e->macro)
                return;
    }
    add_ref(ix, e->macro->name, e->macro, loc,
            (flags & IREF_PASTED) ? 0 : e->macro->name->len, REF_EXPANSION,
            flags, e);
}

static void on_macro_ref(void *ctx, Ident *id, Macro *m, const Tok *tok,
                         RefKind kind)
{
    Index *ix = ctx;
    if (kind == REF_EXPANSION)
        return;
    add_ref(ix, id, m, tok->loc, tok->len, kind,
            (tok->flags & TF_ORIGIN_BODY) ? IREF_IN_BODY : 0, NULL);
}

static void on_define(void *ctx, Macro *m, Macro *replaced)
{
    Index *ix = ctx;
    uint32_t i;
    (void)replaced;
    if (m->predefined)
        return;
    for (i = 0; i < m->body_len; i++) {
        const Tok *t = &m->body[i], *prev = i ? &m->body[i - 1] : NULL;
        if (t->kind != TK_IDENT)
            continue;
        if (t->flags & TF_PARAM) {
            IdxParamRef r;
            r.macro = m;
            r.param = t->punct;
            r.loc = t->loc;
            r.len = t->len;
            vec_push(&ix->params, r);
        } else if (!(prev && (tok_is_punct(prev, P_DOT) ||
                              tok_is_punct(prev, P_ARROW)))) {
            add_ref(ix, tok_ident(ix->pp->in, t), NULL, t->loc, t->len,
                    REF_EXPANSION, IREF_IN_BODY | IREF_STATIC, NULL);
        }
    }
}

static void on_file_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Index *ix = ctx;
    IdxInclusion *inc = NEW(ix->arena, IdxInclusion);
    IdxCheckpoint cp;
    (void)via;
    inc->file = f;
    cp.loc = f->base;
    cp.seq = ix->pp->seq;
    vec_push(&inc->cps, cp);
    vec_push(&ix->inclusions, inc);
    vec_push(&ix->stack, inc);
}

static void on_file_exit(void *ctx, SrcFile *f)
{
    Index *ix = ctx;
    (void)f;
    if (ix->stack.len)
        ix->stack.len--;
}

static void on_checkpoint(void *ctx, SrcLoc loc, uint32_t seq)
{
    Index *ix = ctx;
    IdxCheckpoint cp;
    if (!ix->stack.len)
        return;
    cp.loc = loc;
    cp.seq = seq;
    vec_push(&vec_last(&ix->stack)->cps, cp);
}

static void on_include(void *ctx, const IncludeEvent *ev)
{
    Index *ix = ctx;
    IdxInclude r;
    r.from = ev->from;
    r.to = ev->file;
    r.hash_loc = ev->hash_loc;
    r.name_loc = ev->name_loc;
    r.name_end = ev->name_end;
    r.spelled = ev->spelled;
    r.result = ev->result;
    vec_push(&ix->includes, r);
}

static void on_skipped(void *ctx, SrcLoc b, SrcLoc e)
{
    Index *ix = ctx;
    SrcRange r;
    r.begin = b;
    r.end = e;
    vec_push(&ix->inactive, r);
}

static void on_cond(void *ctx, const CondEvent *ev)
{
    Index *ix = ctx;
    if (ev->kind == COND_IF || ev->kind == COND_IFDEF || ev->kind == COND_IFNDEF) {
        vec_push(&ix->open_blocks, ev->hash_loc);
    } else if (ev->kind == COND_ENDIF && ix->open_blocks.len) {
        IdxBlock b;
        b.begin = vec_pop(&ix->open_blocks);
        b.end = ev->end_loc;
        vec_push(&ix->blocks, b);
    }
}

void index_init(Index *ix, PP *pp)
{
    PPListener l;
    memset(ix, 0, sizeof *ix);
    ix->pp = pp;
    ix->arena = pp->arena;
    ix->sm = pp->sm;
    memset(&l, 0, sizeof l);
    l.ctx = ix;
    l.expand = on_expand;
    l.macro_ref = on_macro_ref;
    l.define = on_define;
    l.file_enter = on_file_enter;
    l.file_exit = on_file_exit;
    l.checkpoint = on_checkpoint;
    l.include = on_include;
    l.skipped = on_skipped;
    l.cond = on_cond;
    pp_add_listener(pp, l);
}

void index_free(Index *ix)
{
    size_t i;
    for (i = 0; i < ix->inclusions.len; i++)
        vec_free(&ix->inclusions.data[i]->cps);
    for (i = 0; i < ix->exps.len; i++)
        if (ix->exps.data[i])
            sb_free(&ix->exps.data[i]->text);
    vec_free(&ix->refs);
    vec_free(&ix->params);
    vec_free(&ix->exps);
    vec_free(&ix->inclusions);
    vec_free(&ix->stack);
    vec_free(&ix->includes);
    vec_free(&ix->inactive);
    vec_free(&ix->blocks);
    vec_free(&ix->open_blocks);
}

void index_run(Index *ix)
{
    Tok t;
    while (pp_next(ix->pp, &t)) {
        IdxExp *x;
        if (ix->pp->out_root == NO_EXP)
            continue;
        x = exp_of(ix, ix->pp->expansions.data[ix->pp->out_root]);
        if (x->text.len && (t.flags & (TF_SPACE | TF_BOL)))
            sb_putc(&x->text, ' ');
        sb_putn(&x->text, pp_text(ix->pp, &t), t.len);
    }
}

/* ---- queries -------------------------------------------------------- */

static int ref_cmp(const void *a, const void *b)
{
    const IdxRef *x = a, *y = b;
    return x->loc < y->loc ? -1 : x->loc > y->loc;
}

static void ensure_sorted(Index *ix)
{
    if (!ix->sorted) {
        if (ix->refs.len)
            qsort(ix->refs.data, ix->refs.len, sizeof(IdxRef), ref_cmp);
        ix->sorted = true;
    }
}

static void add_candidate(IdxTarget *t, Macro *m)
{
    int i;
    for (i = 0; i < t->nmacros; i++)
        if (t->macros[i] == m)
            return;
    if (t->nmacros < (int)ARRAY_LEN(t->macros))
        t->macros[t->nmacros++] = m;
}

static bool in_range(SrcLoc loc, SrcLoc b, uint32_t len)
{
    return loc >= b && loc < b + (len ? len : 1);
}

/* identifier spelled at loc in the raw buffer */
static Ident *ident_at(Index *ix, SrcLoc loc, SrcRange *r)
{
    SrcFile *f = srcmgr_file_of(ix->sm, loc);
    uint32_t off, b, e;
    if (!f)
        return NULL;
    off = loc - f->base;
#define IDC(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || \
                ((c) >= '0' && (c) <= '9') || (c) == '_')
    b = e = off;
    while (b > 0 && IDC(f->buf[b - 1]))
        b--;
    while (e < f->size && IDC(f->buf[e]))
        e++;
#undef IDC
    if (b == e || (f->buf[b] >= '0' && f->buf[b] <= '9'))
        return NULL;
    r->begin = f->base + b;
    r->end = f->base + e;
    return intern(ix->pp->in, f->buf + b, e - b);
}

static uint32_t seq_at(Index *ix, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(ix->sm, loc);
    size_t i;
    for (i = 0; i < ix->inclusions.len; i++) {
        IdxInclusion *inc = ix->inclusions.data[i];
        size_t lo = 0, hi = inc->cps.len;
        if (inc->file != f)
            continue;
        while (hi - lo > 1) {
            size_t mid = (lo + hi) / 2;
            if (inc->cps.data[mid].loc <= loc)
                lo = mid;
            else
                hi = mid;
        }
        return inc->cps.data[lo].seq;
    }
    return ix->pp->seq; /* never entered: end-of-TU state */
}

static bool live_at(const Macro *m, uint32_t seq)
{
    return m->def_seq < seq && !(m->undef_seq && m->undef_seq < seq);
}

size_t index_visible(Index *ix, SrcLoc loc, Macro ***out)
{
    uint32_t seq = seq_at(ix, loc);
    size_t i, n = 0;
    Macro **v = NEW_ARRAY(ix->arena, Macro *, ix->pp->macros.len + 1);
    for (i = 0; i < ix->pp->macros.len; i++)
        if (live_at(ix->pp->macros.data[i], seq))
            v[n++] = ix->pp->macros.data[i];
    *out = v;
    return n;
}

IdxTarget index_resolve(Index *ix, SrcLoc loc)
{
    IdxTarget t;
    size_t i, lo, hi;
    memset(&t, 0, sizeof t);
    t.param = -1;
    ensure_sorted(ix);

    /* #include operand */
    for (i = 0; i < ix->includes.len; i++) {
        IdxInclude *inc = &ix->includes.data[i];
        if (loc >= inc->name_loc && loc < inc->name_end && inc->to) {
            t.kind = TGT_INCLUDE;
            t.file = inc->to;
            t.range.begin = inc->name_loc;
            t.range.end = inc->name_end;
            return t;
        }
    }

    /* definitions and parameters */
    for (i = 0; i < ix->pp->macros.len; i++) {
        Macro *m = ix->pp->macros.data[i];
        int k;
        if (m->predefined)
            continue;
        if (in_range(loc, m->name_loc, m->name->len)) {
            t.kind = TGT_MACRO;
            t.name = m->name;
            add_candidate(&t, m);
            t.range.begin = m->name_loc;
            t.range.end = m->name_loc + m->name->len;
            return t;
        }
        for (k = 0; k < m->nparams; k++)
            if (in_range(loc, m->param_locs[k], m->params[k]->len)) {
                t.kind = TGT_PARAM;
                t.name = m->params[k];
                t.macros[0] = m;
                t.nmacros = 1;
                t.param = k;
                t.range.begin = m->param_locs[k];
                t.range.end = m->param_locs[k] + m->params[k]->len;
                return t;
            }
    }
    for (i = 0; i < ix->params.len; i++) {
        IdxParamRef *p = &ix->params.data[i];
        if (in_range(loc, p->loc, p->len)) {
            t.kind = TGT_PARAM;
            t.name = p->macro->params[p->param];
            t.macros[0] = p->macro;
            t.nmacros = 1;
            t.param = p->param;
            t.range.begin = p->loc;
            t.range.end = p->loc + p->len;
            return t;
        }
    }

    /* recorded references (binary search on the sorted refs) */
    lo = 0;
    hi = ix->refs.len;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ix->refs.data[mid].loc + MAX(ix->refs.data[mid].len, 1u) <= loc)
            lo = mid + 1;
        else
            hi = mid;
    }
    /* back up over refs that start earlier but still cover loc */
    while (lo > 0 && ix->refs.data[lo - 1].loc + 256 > loc)
        lo--;
    for (i = lo; i < ix->refs.len && ix->refs.data[i].loc <= loc; i++) {
        IdxRef *r = &ix->refs.data[i];
        if (!in_range(loc, r->loc, r->len))
            continue;
        t.kind = TGT_MACRO;
        t.name = r->name;
        t.range.begin = r->loc;
        t.range.end = r->loc + r->len;
        if (r->macro)
            add_candidate(&t, r->macro);
        if (r->exp && r->exp->parent == NO_EXP && !(r->flags & IREF_IN_BODY) &&
            r->exp->id < ix->exps.len)
            t.top = ix->exps.data[r->exp->id];
    }
    if (t.kind == TGT_MACRO && t.nmacros == 0) {
        /* static body ref or undefined name: every definition by name */
        Macro *m;
        for (m = t.name->history; m; m = m->prev)
            if (!m->builtin)
                add_candidate(&t, m);
        if (!t.nmacros && !t.name->history && !t.top) {
            /* not a macro at all (e.g. a function named in a body), unless
             * it is tested with #ifdef: keep those as unresolved refs */
            bool tested = false;
            for (i = 0; i < ix->refs.len; i++)
                if (ix->refs.data[i].name == t.name &&
                    !(ix->refs.data[i].flags & IREF_STATIC))
                    tested = true;
            if (!tested)
                memset(&t, 0, sizeof t);
        }
    }
    if (t.kind != TGT_NONE)
        return t;

    /* plain identifier (inactive code, comments excluded) */
    {
        SrcRange r;
        Ident *id = ident_at(ix, loc, &r);
        Macro **vis;
        size_t n, k;
        if (!id || !id->history)
            return t;
        t.kind = TGT_MACRO;
        t.name = id;
        t.range = r;
        n = index_visible(ix, loc, &vis);
        for (k = 0; k < n; k++)
            if (vis[k]->name == id)
                add_candidate(&t, vis[k]);
        if (!t.nmacros) {
            Macro *m;
            for (m = id->history; m; m = m->prev)
                if (!m->builtin)
                    add_candidate(&t, m);
        }
    }
    return t;
}

size_t index_references(Index *ix, const IdxTarget *t, IdxRef **out)
{
    VEC(IdxRef) v = {0};
    size_t i;
    int k;
    IdxRef r;
    ensure_sorted(ix);
    memset(&r, 0, sizeof r);
    if (t->kind == TGT_PARAM) {
        Macro *m = t->macros[0];
        r.name = t->name;
        r.macro = m;
        r.loc = m->param_locs[t->param];
        r.len = t->name->len;
        r.kind = REF_EXPANSION;
        vec_push(&v, r);
        for (i = 0; i < ix->params.len; i++) {
            IdxParamRef *p = &ix->params.data[i];
            if (p->macro == m && p->param == t->param) {
                r.loc = p->loc;
                r.len = p->len;
                r.flags = IREF_IN_BODY;
                vec_push(&v, r);
            }
        }
    } else if (t->kind == TGT_MACRO) {
        for (k = 0; k < t->nmacros; k++) {
            Macro *m = t->macros[k];
            r.name = m->name;
            r.macro = m;
            r.loc = m->name_loc;
            r.len = m->name->len;
            r.kind = REF_EXPANSION;
            r.flags = 0;
            vec_push(&v, r);
        }
        for (i = 0; i < ix->refs.len; i++) {
            IdxRef *x = &ix->refs.data[i];
            bool match = false;
            if (x->macro) {
                for (k = 0; k < t->nmacros; k++)
                    if (x->macro == t->macros[k])
                        match = true;
            } else if (x->name == t->name) {
                match = true; /* undefined-at-the-time or static textual */
            }
            if (!match)
                continue;
            /* a static body ref that was also recorded dynamically */
            if (x->flags & IREF_STATIC) {
                size_t j;
                bool dup = false;
                for (j = 0; j < v.len; j++)
                    if (v.data[j].loc == x->loc)
                        dup = true;
                if (dup)
                    continue;
            }
            vec_push(&v, *x);
        }
    }
    /* one entry per location; a resolved (dynamic) ref beats the static
     * textual one recorded at #define time */
    {
        size_t w = 0, j;
        for (i = 0; i < v.len; i++) {
            bool dup = false;
            for (j = 0; j < w; j++)
                if (v.data[j].loc == v.data[i].loc && v.data[i].len &&
                    v.data[j].len) {
                    dup = true;
                    if ((v.data[j].flags & IREF_STATIC) &&
                        !(v.data[i].flags & IREF_STATIC))
                        v.data[j] = v.data[i];
                }
            if (!dup)
                v.data[w++] = v.data[i];
        }
        v.len = w;
    }
    *out = NEW_ARRAY(ix->arena, IdxRef, v.len + 1);
    if (v.len)
        memcpy(*out, v.data, sizeof(IdxRef) * v.len);
    i = v.len;
    vec_free(&v);
    return i;
}

SrcFile *index_find_file(Index *ix, const char *path)
{
    char *norm = path_normalize(ix->arena, path);
    size_t i, n = strlen(norm);
    for (i = 0; i < ix->sm->files.len; i++) {
        SrcFile *f = ix->sm->files.data[i];
        size_t m = strlen(f->path);
        if (f->kind == SF_VIRTUAL)
            continue;
        if (!strcmp(f->path, norm))
            return f;
        if (m > n && f->path[m - n - 1] == '/' && !strcmp(f->path + m - n, norm))
            return f;
        if (n > m && norm[n - m - 1] == '/' && !strcmp(norm + n - m, f->path))
            return f;
    }
    return NULL;
}

/* ---- JSON dump ------------------------------------------------------ */

static void jloc(JsonWriter *w, SrcMgr *sm, const char *prefix, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(sm, loc);
    uint32_t line = 0, col = 0;
    char key[32];
    if (f)
        srcmgr_linecol(f, loc, &line, &col);
    if (!*prefix) {
        json_key(w, "file");
        if (f)
            json_str(w, f->name);
        else
            json_null(w);
    }
    sprintf(key, "%sline", prefix);
    json_key(w, key);
    json_int(w, line);
    sprintf(key, "%scol", prefix);
    json_key(w, key);
    json_int(w, col);
}

static const char *ref_kind_name(RefKind k)
{
    switch (k) {
    case REF_EXPANSION: return "expansion";
    case REF_IFDEF: return "ifdef";
    case REF_DEFINED: return "defined";
    case REF_UNDEF: return "undef";
    case REF_PRAGMA: return "pragma";
    case REF_IF_VALUE: return "if-value";
    }
    return "?";
}

void index_dump_json(Index *ix, FILE *out, bool all)
{
    JsonWriter w;
    size_t i;
    PP *pp = ix->pp;
    ensure_sorted(ix);
    json_init(&w, out);
    w.pretty = true;
    json_begin_object(&w);

    json_key(&w, "macros");
    json_begin_array(&w);
    for (i = 0; i < pp->macros.len; i++) {
        Macro *m = pp->macros.data[i];
        int k;
        if (!all && (m->predefined || !m->file || m->file->kind != SF_USER ||
                     m->file->system_header))
            continue;
        json_begin_object(&w);
        json_key(&w, "id");
        json_int(&w, m->id);
        json_key(&w, "name");
        json_str(&w, m->name->str);
        json_key(&w, "kind");
        json_str(&w, macro_kind_str(m));
        json_key(&w, "signature");
        json_str(&w, macro_signature(ix->arena, m));
        json_key(&w, "body");
        json_str(&w, macro_body_str(ix->pp, m));
        jloc(&w, ix->sm, "", m->name_loc);
        json_key(&w, "params");
        json_begin_array(&w);
        for (k = 0; k < m->nparams; k++) {
            json_begin_object(&w);
            json_key(&w, "name");
            json_str(&w, m->params[k]->str);
            jloc(&w, ix->sm, "", m->param_locs[k]);
            json_end_object(&w);
        }
        json_end_array(&w);
        json_key(&w, "undef");
        if (m->undef_loc) {
            json_begin_object(&w);
            jloc(&w, ix->sm, "", m->undef_loc);
            json_end_object(&w);
        } else {
            json_null(&w);
        }
        json_key(&w, "expansions");
        json_int(&w, m->expansions);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "references");
    json_begin_array(&w);
    for (i = 0; i < ix->refs.len; i++) {
        IdxRef *r = &ix->refs.data[i];
        if (!all && (r->flags & IREF_SYSTEM))
            continue;
        json_begin_object(&w);
        json_key(&w, "name");
        json_str(&w, r->name->str);
        json_key(&w, "macro");
        if (r->macro)
            json_int(&w, r->macro->id);
        else
            json_null(&w);
        json_key(&w, "kind");
        json_str(&w, (r->flags & IREF_STATIC) ? "body-text" : ref_kind_name(r->kind));
        jloc(&w, ix->sm, "", r->loc);
        json_key(&w, "len");
        json_int(&w, r->len);
        json_key(&w, "inBody");
        json_bool(&w, (r->flags & IREF_IN_BODY) != 0);
        json_key(&w, "renamable");
        json_bool(&w, !(r->flags & (IREF_PASTED | IREF_SYSTEM)));
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "expansions");
    json_begin_array(&w);
    for (i = 0; i < ix->exps.len; i++) {
        IdxExp *x = ix->exps.data[i];
        if (!x || x->root != x->e || x->e->in_directive ||
            (!all && loc_is_system(ix, x->e->name_loc)))
            continue;
        json_begin_object(&w);
        json_key(&w, "macro");
        json_int(&w, x->e->macro->id);
        json_key(&w, "name");
        json_str(&w, x->e->macro->name->str);
        jloc(&w, ix->sm, "", x->e->name_loc);
        jloc(&w, ix->sm, "end", x->e->end_loc);
        json_key(&w, "text");
        json_str(&w, sb_cstr(&x->text));
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "inactive");
    json_begin_array(&w);
    for (i = 0; i < ix->inactive.len; i++) {
        SrcRange r = ix->inactive.data[i];
        if (!all && loc_is_system(ix, r.begin))
            continue;
        json_begin_object(&w);
        jloc(&w, ix->sm, "", r.begin);
        jloc(&w, ix->sm, "end", r.end);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "folding");
    json_begin_array(&w);
    for (i = 0; i < ix->blocks.len; i++) {
        IdxBlock b = ix->blocks.data[i];
        if (!all && loc_is_system(ix, b.begin))
            continue;
        json_begin_object(&w);
        jloc(&w, ix->sm, "", b.begin);
        jloc(&w, ix->sm, "end", b.end);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_key(&w, "includes");
    json_begin_array(&w);
    for (i = 0; i < ix->includes.len; i++) {
        IdxInclude *inc = &ix->includes.data[i];
        static const char *const res[] = {"included", "skipped-guard",
                                          "skipped-once", "not-found"};
        if (!all && loc_is_system(ix, inc->hash_loc))
            continue;
        json_begin_object(&w);
        jloc(&w, ix->sm, "", inc->hash_loc);
        json_key(&w, "spelled");
        json_str(&w, inc->spelled);
        json_key(&w, "target");
        if (inc->to)
            json_str(&w, inc->to->name);
        else
            json_null(&w);
        json_key(&w, "result");
        json_str(&w, res[inc->result]);
        json_end_object(&w);
    }
    json_end_array(&w);

    json_end_object(&w);
    fputc('\n', out);
}
