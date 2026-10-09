/* check.c - the checker's core: diagnostics, scopes and symbols, the walk
 * over each unit, the end of the translation unit (check.h,
 * docs/TYPES.md).  Declarations are in cdecl.c (with cattr.c, cspec.c),
 * expressions in cexpr.c (with cwarn_expr.c, cprint.c, ...). */
#include "c/check_int.h"

#include <ctype.h>
#include <string.h>

/* ---- diagnostics ------------------------------------------------------ */

bool cin_system(Checker *c, SrcLoc loc);
bool cin_system(Checker *c, SrcLoc loc)
{
    return srcmgr_is_system(srcmgr_file_of(c->sm, loc), loc);
}

static bool in_system(Checker *c, SrcLoc loc)
{
    return cin_system(c, loc);
}

/* gcc follows a diagnostic located in a macro expansion with a note per
 * macro it was expanded through.  Diagnostics carry only a location: the
 * token is the one of this unit spelled there, nearest the node being
 * checked (the same spelled location recurs in every use of a macro). */
static void macro_notes(Checker *c, Diagnostic *d, SrcLoc loc, bool is_note)
{
    const PTok *tk = c->u ? c->u->toks : NULL;
    uint32_t n = c->u ? c->u->ntoks : 0, i, best = 0, from = 0, bd = 0;
    bool found = false;
    int pass;
    if (!tk || c->diag->track0 || !loc)
        return;
    if (c->cur_node != NO_NODE && c->cur_node < c->nn)
        from = c->nodes[c->cur_node].tok;
    /* the common case, a token written in the source itself or an argument
     * (no note), is near the node: settle it without scanning the unit */
    for (i = from > 256 ? from - 256 : 0; i < n && i < from + 256; i++)
        if (tk[i].t.loc == loc && (!tk[i].exp || tk[i].exp == loc ||
                                     (tk[i].t.flags & TF_ORIGIN_ARG)))
            return;
    /* the token nearest the node is wanted: look near it first, and scan
     * the unit only if nothing is there */
    for (pass = is_note; pass < 2 && !found; pass++)
    for (i = pass ? 0 : (from > 256 ? from - 256 : 0);
         i < (pass ? n : (n < from + 256 ? n : from + 256)); i++) {
        uint32_t dist;
        if (tk[i].t.loc != loc || !tk[i].exp || tk[i].exp == loc ||
            (tk[i].t.flags & TF_ORIGIN_ARG)) /* gcc: no note for an argument */
            continue;
        if (is_note && loc == c->mn_loc) {
            /* the same replacement token: an earlier use of the macro */
            if (tk[i].exp == c->mn_exp || i >= c->mn_idx)
                continue;
            dist = c->mn_idx - i;
        } else if (is_note) {
            /* another token of the diagnostics invocation */
            if (tk[i].exp != c->mn_exp)
                continue;
            dist = i > c->mn_idx ? i - c->mn_idx : c->mn_idx - i;
        } else
            dist = i > from ? i - from : from - i;
        if (!found || dist < bd) {
            found = true;
            best = i;
            bd = dist;
        }
    }
    if (!found)
        return;
    if (!is_note) {
        c->mn_exp = tk[best].exp, c->mn_idx = best, c->mn_loc = loc;
        d->oloc = tk[best].exp;     /* it follows what precedes the use */
    }
    diag_expansion_notes(c->diag, d, c->sm, loc, tk[best].exp,
                         (tk[best].t.flags & TF_ORIGIN_BODY) &&
                             !(tk[best].t.flags & TF_ORIGIN_ARG),
                         c->opt.macro_chain, c->opt.macro_ctx);
}

static Diagnostic *vrep(Checker *c, DiagLevel lvl, const char *id, SrcLoc loc,
                        const char *fmt, va_list ap)
{
    Diagnostic *d;
    if (c->quiet)
        return NULL;
    /* gcc's disable_extension_diagnostics also clears warn_traditional */
    if (id && !strcmp(id, "traditional") && c->cur_node != NO_NODE &&
        cexpr_in_extension(c, c->cur_node))
        return NULL;
    d = diag_vreport(c->diag, lvl, id ? id : "", loc, fmt, ap);
    if (d && lvl != DL_NOTE)
        macro_notes(c, d, loc, false);
    return d;
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

static Diagnostic *vped(Checker *c, SrcLoc loc, const char *id, bool err,
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
    return vrep(c, err ? DL_ERROR : DL_WARNING, id, loc, fmt, ap);
}

Diagnostic *cpedwarn(Checker *c, SrcLoc loc, const char *id,
                     const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = vped(c, loc, id, c->opt.pedantic_errors, fmt, ap);
    va_end(ap);
    return d;
}

/* gcc's pedwarn_c11 / pedwarn_c2x: a feature of standard `year`, reported
 * before it as a pedwarn under -pedantic (hidden by -Wno-OPT) and, from
 * `year` on, only as a plain warning when the compat option OPT was given.
 * An explicit option also becomes the diagnostic's tag; __extension__ hides
 * all of it. */
Diagnostic *cpedstd(Checker *c, SrcLoc loc, int year, const char *opt,
                    const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    bool below = c->opt.std_year < year;
    bool expl = diag_option_explicit(c->diag, opt);
    if (!(expl || (below && c->opt.pedantic && diag_enabled(c->diag, opt))))
        return NULL;
    if (c->cur_node != NO_NODE && cexpr_in_extension(c, c->cur_node))
        return NULL;
    va_start(ap, fmt);
    d = vped(c, loc, expl ? opt : "pedantic", below && c->opt.pedantic_errors,
             fmt, ap);
    va_end(ap);
    return d;
}

/* gcc's pedwarn_c90 in C99 mode: the option to report a C90-only restriction
 * under -- its own option when on, else -Wc90-c99-compat unless the own
 * option was turned off; NULL when neither applies. */
const char *cc90_id(Checker *c, const char *own)
{
    if (own && diag_enabled(c->diag, own))
        return own;
    if (diag_enabled(c->diag, "c90-c99-compat") &&
        (!own || diag_option_state(c->diag, own) == -1))
        return "c90-c99-compat";
    return NULL;
}

void cc90(Checker *c, SrcLoc loc, const char *own, const char *fmt, ...)
{
    const char *id = cc90_id(c, own);
    char buf[256];
    va_list ap;
    if (!id)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cwarn(c, loc, id, "%s", buf);
}

void cconst_overflow(Checker *c, SrcLoc loc)
{
    if (diag_enabled(c->diag, "pedantic"))
        cpedwarn(c, loc, "overflow", "overflow in constant expression");
}

/* A token of a predefined macro (__INT_MAX__) has no spelling location of
 * its own: gcc reports it at the use. */
SrcLoc cbuiltin_loc_(const Checker *c, SrcLoc loc, SrcLoc exp)
{
    SrcFile *f = srcmgr_file_of(c->sm, loc);
    if (f && f->kind == SF_VIRTUAL && (!strcmp(f->name, "<built-in>") ||
                       !strcmp(f->name, "<command line>")))
        return exp;
    return loc;
}

Diagnostic *cpedantic(Checker *c, SrcLoc loc, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    if (!c->opt.pedantic)
        return NULL;
    va_start(ap, fmt);
    d = vped(c, loc, "pedantic", c->opt.pedantic_errors, fmt, ap);
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
    macro_notes(c, d, loc, true);   /* a note inside a macro has its chain too */
}

/* gcc's input_location while the parser looks at token tok: the first
 * token of that token's line (libcpp's line_change callback). */
uint32_t cbol_tok(Checker *c, uint32_t tok)
{
    uint32_t k, bol = 0;        /* bol: BOL token + 1, 0 when none */
    for (k = tok + 1; k-- > 0;) {
        /* the memo covers [its BOL token, its token]: no BOL lies in between */
        if (c->il_first == c->u->first_tok && c->il_tok > k &&
            k + 1 >= (c->il_bol ? c->il_bol : 1)) {
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

/* s with every UCN and UTF-8 character as \\U%08x (what gcc prints in the C
 * locale), into out[cap]. */
const char *cident_ucn_to(const char *s, char *out, size_t cap)
{
    char *o = out;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o < out + cap - 12) {
        uint32_t cp = 0;
        unsigned n = 0, k;
        if (p[0] == '\\' && (p[1] == 'u' || p[1] == 'U')) {
            unsigned digits = p[1] == 'u' ? 4 : 8;
            for (k = 0; k < digits && isxdigit(p[2 + k]); k++)
                cp = cp << 4 | (uint32_t)(isdigit(p[2 + k]) ? p[2 + k] - '0'
                                          : (p[2 + k] | 32) - 'a' + 10);
            if (k == digits)
                n = 2 + digits;
        } else if (p[0] >= 0xC0) {
            n = p[0] >= 0xF0 ? 4 : p[0] >= 0xE0 ? 3 : 2;
            cp = p[0] & (0xFFu >> (n + 1));
            for (k = 1; k < n; k++) {
                if ((p[k] & 0xC0) != 0x80) {
                    n = 0;
                    break;
                }
                cp = cp << 6 | (p[k] & 0x3F);
            }
        }
        if (n) {
            o += snprintf(o, 12, "\\U%08x", cp);
            p += n;
        } else {
            *o++ = (char)*p++;
        }
    }
    *o = 0;
    return out;
}

/* s with every UCN as UTF-8 (what gcc prints in a suggestion), into out[cap]. */
const char *cident_utf8_to(const char *s, char *out, size_t cap)
{
    char *o = out;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && o < out + cap - 8) {
        uint32_t cp = 0;
        unsigned k = 0, digits = 0;
        if (p[0] == '\\' && (p[1] == 'u' || p[1] == 'U')) {
            digits = p[1] == 'u' ? 4 : 8;
            for (k = 0; k < digits && isxdigit(p[2 + k]); k++)
                cp = cp << 4 | (uint32_t)(isdigit(p[2 + k]) ? p[2 + k] - '0'
                                          : (p[2 + k] | 32) - 'a' + 10);
        }
        if (digits && k == digits) {
            if (cp < 0x80) {
                *o++ = (char)cp;
            } else if (cp < 0x800) {
                *o++ = (char)(0xC0 | cp >> 6);
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                *o++ = (char)(0xE0 | cp >> 12);
                *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *o++ = (char)(0xF0 | cp >> 18);
                *o++ = (char)(0x80 | (cp >> 12 & 0x3F));
                *o++ = (char)(0x80 | (cp >> 6 & 0x3F));
                *o++ = (char)(0x80 | (cp & 0x3F));
            }
            p += 2 + digits;
        } else {
            *o++ = (char)*p++;
        }
    }
    *o = 0;
    return out;
}

/* The same into one of a few rotating buffers: valid until the fourth call
 * after this one, so a caller that makes more calls copies it first. */
const char *cident_ucn(const char *s)
{
    enum { CAP = 4096 };
    static __thread char ring[4][CAP];
    static __thread unsigned next;
    return cident_ucn_to(s, ring[next++ & 3], CAP);
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

void cgrow_idents(Checker *c, uint32_t n)
{
    uint32_t **a[] = {&c->top[0], &c->top[1], &c->ext, &c->mseen, &c->fopen};
    uint32_t cap = c->nidents ? c->nidents : 1024;
    size_t k;
    if (n <= c->nidents)
        return;
    while (cap < n)
        cap *= 2;
    for (k = 0; k < ARRAY_LEN(a); k++) {
        *a[k] = xrealloc(*a[k], cap * sizeof **a[k]);
        memset(*a[k] + c->nidents, 0, (cap - c->nidents) * sizeof **a[k]);
    }
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
    cgrow_idents(c, ident + 1);
    if (c->cs && c->scopes.len == 1)
        csum_touch(c, ns, ident);   /* a file-scope declaration */
    b.ident = ident;
    b.prev = c->top[ns][ident];
    b.ref = ref;
    b.ty = 0;
    b.nn = 0;
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
    c->dm = xrealloc(c->dm, c->cap * sizeof *c->dm);
}

void choist_at(Checker *c, size_t n0, size_t at)
{
    Diagnostic **d = c->diag->all.data;
    if (c->diag->all.len == n0 + 1 && at < n0) {
        Diagnostic *x = d[n0];
        memmove(d + at + 1, d + at, (n0 - at) * sizeof *d);
        d[at] = x;
    }
}

void choist(Checker *c, uint32_t i, size_t n0)
{
    size_t at = c->dm[cfirst(c, i)];
    Diagnostic **d = c->diag->all.data;
    if (c->diag->all.len == n0 + 1 && at < n0) {
        Diagnostic *x = d[n0];
        memmove(d + at + 1, d + at, (n0 - at) * sizeof *d);
        d[at] = x;
    }
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
    c->first_err_tok = UINT32_MAX;
    if (c->quiet)
        for (i = 0; i < u->nnodes; i++)
            if (u->nodes[i].tag == N_ERROR || (u->nodes[i].flags & NF_ERROR)) {
                /* the error sits at the last token of the flagged subtree */
                uint32_t j, last = u->nodes[i].tok;
                for (j = i + 1 - u->nodes[i].size; j < i; j++)
                    if (u->nodes[j].tok > last)
                        last = u->nodes[j].tok;
                if (last < c->first_err_tok) {
                    c->first_err_tok = last;
                    c->first_err_params = u->nodes[i].tag == N_FUNC;
                }
            }
    c->fold_pending = 0;
    if (c->cs)
        csum_unit_begin(c);
    cstmt_unit_begin(c);
    cinit_reset(c);
    grow_nodes(c, c->nn + 1);
    cgrow_idents(c, interner_count(c->in) + 1);
    compute_parents(c);
    c->lsyms.len = 0;
    if (c->sx)
        csx_unit_begin(c);
    c->saved.len = 0;
    c->specs.len = 0;
    c->recs.len = 0;
    c->fields.len = 0;
    c->ecs.len = 0;
    c->fv.len = 0;
    c->func_sym = SYM_NONE;
    c->cur_func_node = NO_NODE;
    c->func_node = NO_NODE;
    for (i = 0; i < c->nn; i++) {
        c->dm[i] = c->diag->all.len;
        visit(c, i);
    }
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
    if (c->sx)
        csx_unit_end(c);
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
    cgrow_idents(c, interner_count(in) + 1);
    predeclare(c, "__builtin_va_list", c->tt.va_list);
    predeclare(c, "__int128_t",
               type_typedef(&c->tt, intern_cstr(in, "__int128_t")->id,
                            TYPE_B(INT128)));
    predeclare(c, "__uint128_t",
               type_typedef(&c->tt, intern_cstr(in, "__uint128_t")->id,
                            TYPE_B(UINT128)));
    /* gcc 13 declares nullptr_t in every C mode (a distinct null pointer
     * type; void * here, so its conversions are not modelled) */
    predeclare(c, "nullptr_t",
               type_typedef(&c->tt, intern_cstr(in, "nullptr_t")->id,
                            type_ptr(&c->tt, TYPE_B(VOID))));
    c->func_sym = SYM_NONE;
    c->cur_node = NO_NODE;
    if (opt->summaries || opt->dump_summaries || opt->validate_summaries)
        c->cs = csum_new(c);
    if (opt->symidx)
        c->sx = csx_new(opt->symidx_verify);
    return c;
}

struct CIndex *checker_take_index(Checker *c)
{
    return c->sx ? csx_finish(c) : NULL;
}

void checker_finish(Checker *c)
{
    size_t k;
    cdecl_check_inline_statics(c);
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
    vec_free(&c->inl_statics);
    for (uint32_t i = 0; i < c->dep_msgs.len; i++)
        free(c->dep_msgs.data[i]);
    vec_free(&c->dep_msgs);
    for (uint32_t i = 0; i < c->strinits.len; i++)
        free(c->strinits.data[i].b);
    vec_free(&c->strinits);
    vec_free(&c->wina_td);
    vec_free(&c->ucn_seen);
    vec_free(&c->wina_rec);
    cinit_free(c);
    cstmt_free(c);
    cexpr_free_params(c);
    cparm_free(c);
    csum_free(c);
    csx_free(c->sx);
    sb_free(&c->esb[0]);
    sb_free(&c->esb[1]);
    types_free(&c->tt);
    vec_free(&c->gsyms);
    vec_free(&c->lsyms);
    vec_free(&c->log);
    free(c->top[0]);
    free(c->top[1]);
    free(c->ext);
    free(c->mseen);
    free(c->fopen);
    free(c->fkey);
    free(c->fval);
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
    vec_free(&c->tdas);
    vec_free(&c->ahead);
    vec_free(&c->ecs);
    vec_free(&c->saved);
    vec_free(&c->stack);
    vec_free(&c->pack_stack);
    vec_free(&c->tdseen);
    while (c->opt_bad.len)
        free(c->opt_bad.data[--c->opt_bad.len]);
    vec_free(&c->opt_bad);
    vec_free(&c->opt_stack);
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
