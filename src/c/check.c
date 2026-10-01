/* check.c - the checker's core: diagnostics, scopes and symbols, the walk
 * over each unit, the end of the translation unit (check.h,
 * docs/TYPES.md).  Declarations are in cdecl.c, expressions in cexpr.c. */
#include "c/check_int.h"

#include <string.h>

/* ---- diagnostics ------------------------------------------------------ */

static bool in_system(Checker *c, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    return f && f->system_header;
}

static Diagnostic *vrep(Checker *c, DiagLevel lvl, const char *id, SrcLoc loc,
                        const char *fmt, va_list ap)
{
    if (c->quiet)
        return NULL;
    return diag_vreport(c->diag, lvl, id ? id : "", loc, fmt, ap);
}

void cerror(Checker *c, SrcLoc loc, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vrep(c, DL_ERROR, "", loc, fmt, ap);
    va_end(ap);
}

Diagnostic *cerror_d(Checker *c, SrcLoc loc, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = vrep(c, DL_ERROR, "", loc, fmt, ap);
    va_end(ap);
    return d;
}

void cwarn(Checker *c, SrcLoc loc, const char *id, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vrep(c, DL_WARNING, id, loc, fmt, ap);
    va_end(ap);
}

Diagnostic *cwarn_d(Checker *c, DiagLevel lvl, SrcLoc loc, const char *id,
                    const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = vrep(c, lvl, id, loc, fmt, ap);
    va_end(ap);
    return d;
}

static Diagnostic *vped(Checker *c, SrcLoc loc, const char *id,
                        const char *fmt, va_list ap)
{
    if (id && *id && !diag_enabled(c->diag, id))
        return NULL;
    /* gcc clears `pedantic` while parsing under __extension__ */
    if (id && !strcmp(id, "pedantic") && c->cur_node != NO_NODE &&
        cexpr_in_extension(c, c->cur_node))
        return NULL;
    if (in_system(c, loc) && !c->diag->show_system)
        return NULL;
    return vrep(c, c->opt.pedantic_errors ? DL_ERROR : DL_WARNING, id, loc,
                fmt, ap);
}

Diagnostic *cpedwarn(Checker *c, SrcLoc loc, const char *id,
                     const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = vped(c, loc, id, fmt, ap);
    va_end(ap);
    return d;
}

Diagnostic *cpedantic(Checker *c, SrcLoc loc, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    if (!c->opt.pedantic)
        return NULL;
    va_start(ap, fmt);
    d = vped(c, loc, "pedantic", fmt, ap);
    va_end(ap);
    return d;
}

void cnote(Checker *c, Diagnostic *d, SrcLoc loc, const char *fmt, ...)
{
    DiagNote n;
    va_list ap;
    if (!d)
        return;
    c->sb.len = 0;
    va_start(ap, fmt);
    sb_vprintf(&c->sb, fmt, ap);
    va_end(ap);
    n.loc = loc;
    n.msg = arena_strndup(c->diag->arena, c->sb.data, c->sb.len);
    vec_push(&d->notes, n);
}

/* gcc's input_location while the parser looks at token tok: the first
 * token of that token's line (libcpp's line_change callback). */
uint32_t cbol_tok(Checker *c, uint32_t tok)
{
    uint32_t k, bol = 0;        /* bol: BOL token + 1, 0 when none */
    for (k = tok + 1; k-- > 0;) {
        if (c->il_first == c->u->first_tok && c->il_tok == k + 1) {    /* the memo: callers walk forward */
            bol = c->il_bol;
            break;
        }
        if (c->u->toks[k].t.flags & TF_BOL) {
            bol = k + 1;
            break;
        }
    }
    c->il_first = c->u->first_tok;
    c->il_tok = tok + 1;
    c->il_bol = bol;
    return bol;
}

SrcLoc cinput_loc(Checker *c, uint32_t tok)
{
    uint32_t bol;
    if (!c->u->ntoks)
        return c->last_bol;
    if (tok >= c->u->ntoks)
        tok = c->u->ntoks - 1;
    bol = cbol_tok(c, tok);
    if (!bol)
        return c->last_bol ? c->last_bol : ctok_loc(c, 0);
    return c->u->toks[bol - 1].exp ? c->u->toks[bol - 1].exp
                                   : ctok_loc(c, bol - 1);
}

/* The index of the first ';' at nesting depth 0 from token tok, or the
 * unit's last token. */
uint32_t cfind_semi(Checker *c, uint32_t tok)
{
    int depth = 0;
    uint32_t k;
    for (k = tok; k < c->u->ntoks; k++) {
        const Tok *t = &c->u->toks[k].t;
        if (t->kind != TK_PUNCT)
            continue;
        switch (t->punct) {
        case P_LPAREN: case P_LBRACKET: case P_LBRACE: depth++; break;
        case P_RPAREN: case P_RBRACKET: case P_RBRACE: depth--; break;
        case P_SEMI:
            if (depth <= 0)
                return k;
            break;
        default: break;
        }
    }
    return c->u->ntoks ? c->u->ntoks - 1 : 0;
}

/* ---- symbols and scopes ----------------------------------------------- */

static void grow_idents(Checker *c, uint32_t n)
{
    uint32_t cap = c->nidents ? c->nidents : 1024, ns;
    if (n <= c->nidents)
        return;
    while (cap < n)
        cap *= 2;
    for (ns = 0; ns < 2; ns++) {
        c->top[ns] = xrealloc(c->top[ns], cap * sizeof *c->top[ns]);
        memset(c->top[ns] + c->nidents, 0,
               (cap - c->nidents) * sizeof *c->top[ns]);
    }
    c->ext = xrealloc(c->ext, cap * sizeof *c->ext);
    memset(c->ext + c->nidents, 0, (cap - c->nidents) * sizeof *c->ext);
    c->nidents = cap;
}

uint32_t csym_new(Checker *c, bool global, const CSym *s)
{
    if (global) {
        vec_push(&c->gsyms, *s);
        return (uint32_t)c->gsyms.len - 1;
    }
    vec_push(&c->lsyms, *s);
    return ((uint32_t)c->lsyms.len - 1) | SYM_LOCAL;
}

void cbind(Checker *c, int ns, uint32_t ident, uint32_t ref)
{
    Bind b;
    if (!ident)
        return;
    grow_idents(c, ident + 1);
    if (c->cs && c->scopes.len == 1)
        csum_touch(c, ns, ident);   /* a file-scope declaration */
    b.ident = ident;
    b.prev = c->top[ns][ident];
    b.ref = ref;
    b.ty = 0;
    b.ns = (uint8_t)ns;
    vec_push(&c->log, b);
    c->top[ns][ident] = (uint32_t)c->log.len;
}

uint32_t cbind_type(Checker *c, uint32_t ident)
{
    uint32_t b = ident && ident < c->nidents ? c->top[NS_ORD][ident] : 0;
    return b ? c->log.data[b - 1].ty : 0;
}

uint32_t clookup(Checker *c, int ns, uint32_t ident)
{
    uint32_t b = ident && ident < c->nidents ? c->top[ns][ident] : 0;
    if (c->csf && ident &&
        (!b || b - 1 < (c->scopes.len > 1 ? c->scopes.data[1].log
                                          : UINT32_MAX)) &&
        !(ident < c->csf->rn[ns] && c->csf->rs[ns][ident] == c->csf->seq))
        csum_read(c, ns, ident, b);   /* resolved at file scope (or not) */
    if (!b)
        return ns == NS_ORD ? SYM_NONE : 0;
    return c->log.data[b - 1].ref;
}

uint32_t lookup_ord(Checker *c, uint32_t ident)
{
    return clookup(c, NS_ORD, ident);
}

/* The binding of ident in the innermost scope (index + 1), 0 if none. */
uint32_t cbound_here(Checker *c, int ns, uint32_t ident)
{
    uint32_t b;
    if (!ident || ident >= c->nidents)
        return 0;
    b = c->top[ns][ident];
    return b && b - 1 >= vec_last(&c->scopes).log ? b : 0;
}

/* The binding's scope kind. */
ScopeKind cbind_scope(Checker *c, uint32_t b)
{
    size_t k = c->scopes.len;
    while (k-- > 1)
        if (b - 1 >= c->scopes.data[k].log)
            return (ScopeKind)c->scopes.data[k].kind;
    return SCK_FILE;
}

void cscope_push(Checker *c, ScopeKind kind)
{
    ScopeMark m;
    m.log = (uint32_t)c->log.len;
    m.kind = (uint8_t)kind;
    vec_push(&c->scopes, m);
}

void cscope_pop(Checker *c, BindVec *save)
{
    uint32_t mark;
    if (c->scopes.len <= 1)
        return;
    mark = vec_pop(&c->scopes).log;
    if (save)
        for (size_t i = mark; i < c->log.len; i++)
            vec_push(save, c->log.data[i]);
    while (c->log.len > mark) {
        Bind *b = &c->log.data[--c->log.len];
        c->top[b->ns][b->ident] = b->prev;
    }
}

bool cat_file_scope(Checker *c)
{
    return c->scopes.len == 1;
}

ScopeKind cscope_kind(Checker *c)
{
    return (ScopeKind)vec_last(&c->scopes).kind;
}

/* ---- the walk ---------------------------------------------------------- */

static void grow_nodes(Checker *c, uint32_t n)
{
    if (n <= c->cap)
        return;
    c->cap = n < 256 ? 256 : n;
    while (c->cap < n)
        c->cap *= 2;
    c->ty = xrealloc(c->ty, c->cap * sizeof *c->ty);
    c->cv = xrealloc(c->cv, c->cap * sizeof *c->cv);
    c->cb = xrealloc(c->cb, c->cap * sizeof *c->cb);
    c->ck = xrealloc(c->ck, c->cap * sizeof *c->ck);
    c->ef = xrealloc(c->ef, c->cap * sizeof *c->ef);
    c->par = xrealloc(c->par, c->cap * sizeof *c->par);
}

static void compute_parents(Checker *c)
{
    uint32_t i;
    c->stack.len = 0;
    for (i = 0; i < c->nn; i++) {
        uint32_t f = cfirst(c, i);
        while (c->stack.len && vec_last(&c->stack) >= f)
            c->par[vec_pop(&c->stack)] = i;
        vec_push(&c->stack, i);
    }
    while (c->stack.len)
        c->par[vec_pop(&c->stack)] = NO_NODE;
}

/* SCOPE: a block, a prototype, or a function body with its parameters
 * again. */
static void scope_open(Checker *c, uint32_t i)
{
    uint32_t p = c->par[i];
    const Node *n = cnode(c, i);
    if (n->flags & NF_PARAMS) {
        cscope_push(c, SCK_FUNC);
        cdecl_body_scope(c, i);
        return;
    }
    cscope_push(c, p != NO_NODE && cnode(c, p)->tag == N_FUNC ? SCK_PROTO
                                                              : SCK_BLOCK);
}

static void scope_close(Checker *c, uint32_t i)
{
    if (c->scopes.len <= 1)
        return;
    if (cscope_kind(c) == SCK_PROTO) {
        c->cv[i] = c->saved.len;
        cscope_pop(c, &c->saved);
        c->cb[i] = (uint32_t)(c->saved.len - c->cv[i]);
        return;
    }
    if (cscope_kind(c) == SCK_FUNC)
        cdecl_func_end(c, i);
    cscope_pop(c, NULL);
}

static void visit(Checker *c, uint32_t i)
{
    unsigned tag = cnode(c, i)->tag;
    c->cur_node = i;
    c->ty[i] = TYPE_B(ERROR);
    c->cv[i] = 0;
    c->cb[i] = 0;
    c->ck[i] = K_NONE;
    c->ef[i] = 0;
    cstmt_enter(c, i);
    if (c->ci)
        cinit_pre(c, i);
    switch (tag) {
    case N_SCOPE:
        scope_open(c, i);
        cstmt_scope_open(c, i);
        break;
    case N_SCOPE_END:
        cstmt_scope_end(c, i);
        scope_close(c, i);
        cstmt_scope_end_post(c, i);
        break;
    default:
        if (cexpr_is_expr(tag)) {
            cexpr_node(c, i);
            cstmt_expr(c, i);
        } else {
            cdecl_node(c, i);
            cstmt_node(c, i);
        }
        break;
    }
    cinit_post(c, i);
}

void checker_unit(Checker *c, const ParseUnit *u, bool had_errors)
{
    uint32_t i;
    c->u = u;
    c->il_tok = 0;              /* drop the cinput_loc memo */
    c->nodes = u->nodes;
    c->nn = u->nnodes;
    c->quiet = had_errors && !(u->nnodes && u->nodes[u->nnodes - 1].tag == N_FUNC_DEF);
    c->fold_pending = 0;
    if (c->cs)
        csum_unit_begin(c);
    cstmt_unit_begin(c);
    cinit_reset(c);
    grow_nodes(c, c->nn + 1);
    grow_idents(c, interner_count(c->in) + 1);
    compute_parents(c);
    c->lsyms.len = 0;
    c->saved.len = 0;
    c->specs.len = 0;
    c->recs.len = 0;
    c->fields.len = 0;
    c->ecs.len = 0;
    c->fv.len = 0;
    c->func_sym = SYM_NONE;
    c->cur_func_node = NO_NODE;
    c->func_node = NO_NODE;
    for (i = 0; i < c->nn; i++)
        visit(c, i);
    c->cur_node = NO_NODE;
    while (c->scopes.len > 1)
        cscope_pop(c, NULL);
    if (c->cs)
        csum_unit_end(c);
    /* the last line start, for input_location in the next unit */
    for (i = u->ntoks; i-- > 0;)
        if (u->toks[i].t.flags & TF_BOL) {
            c->last_bol = ctok_loc(c, i);
            break;
        }
    c->quiet = false;
}

/* ---- setup and the end ------------------------------------------------- */

static void predeclare(Checker *c, const char *name, TypeId to)
{
    CSym s;
    uint32_t id = intern_cstr(c->in, name)->id;
    memset(&s, 0, sizeof s);
    s.name = id;
    s.kind = CS_TYPEDEF;
    s.sc = SC_TYPEDEF;
    s.ty = to;
    cbind(c, NS_ORD, id, csym_new(c, true, &s));
}

Checker *checker_new(SrcMgr *sm, Interner *in, DiagEngine *diag,
                     const CheckOptions *opt)
{
    Checker *c = xcalloc(1, sizeof *c);
    c->sm = sm;
    c->in = in;
    c->diag = diag;
    c->opt = *opt;
    c->tgt = opt->target ? opt->target : &target_x86_64;
    types_init(&c->tt, c->tgt, in);
    cscope_push(c, SCK_FILE);
    grow_idents(c, interner_count(in) + 1);
    predeclare(c, "__builtin_va_list", c->tt.va_list);
    predeclare(c, "__int128_t",
               type_typedef(&c->tt, intern_cstr(in, "__int128_t")->id,
                            TYPE_B(INT128)));
    predeclare(c, "__uint128_t",
               type_typedef(&c->tt, intern_cstr(in, "__uint128_t")->id,
                            TYPE_B(UINT128)));
    c->func_sym = SYM_NONE;
    c->cur_node = NO_NODE;
    if (opt->summaries || opt->dump_summaries || opt->validate_summaries)
        c->cs = csum_new(c);
    return c;
}

void checker_finish(Checker *c)
{
    size_t k;
    for (k = 0; k < c->tentative.len; k++)
        cdecl_finish_object(c, c->tentative.data[k]);
    if (c->opt.dump)
        fflush(c->opt.dump);
}

void checker_free(Checker *c)
{
    if (!c)
        return;
    vec_free(&c->undecl);
    for (uint32_t i = 0; i < c->dep_msgs.len; i++)
        free(c->dep_msgs.data[i]);
    vec_free(&c->dep_msgs);
    cinit_free(c);
    cstmt_free(c);
    cexpr_free_params(c);
    cparm_free(c);
    csum_free(c);
    sb_free(&c->esb[0]);
    sb_free(&c->esb[1]);
    types_free(&c->tt);
    vec_free(&c->gsyms);
    vec_free(&c->lsyms);
    vec_free(&c->log);
    free(c->top[0]);
    free(c->top[1]);
    free(c->ext);
    vec_free(&c->scopes);
    free(c->ty);
    free(c->cv);
    free(c->cb);
    free(c->ck);
    free(c->ef);
    free(c->par);
    vec_free(&c->fv);
    vec_free(&c->specs);
    vec_free(&c->recs);
    vec_free(&c->fields);
    vec_free(&c->anames);
    vec_free(&c->ecs);
    vec_free(&c->saved);
    vec_free(&c->stack);
    vec_free(&c->pack_stack);
    if (c->diag_cur || c->diag_stack.len) {
        size_t k;
        for (k = 0; k < c->diag_stack.len; k++)
            diag_config_free(c->diag_stack.data[k].cfg);
        diag_config_free(c->diag_cur);
        c->diag->cfg = c->diag_cfg0;
    }
    vec_free(&c->diag_stack);
    vec_free(&c->nested_undef);
    vec_free(&c->tentative);
    sb_free(&c->sb);
    free(c);
}
