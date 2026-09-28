/* ppexpand.c - macro replacement (C99 6.10.3), _Pragma (6.10.9), builtin
 * macros (6.10.8) and #pragma handling (6.10.6).
 *
 * The replacement of an invocation is built in a pooled buffer and pushed
 * as a Context; rescanning happens as the main loop reads from it.  The
 * macro stays disabled until its context is exhausted. */
#include "pp.h"

#include <string.h>

#define ARGS_INLINE 8

typedef struct ArgVec {
    uint32_t *data;
    size_t len, cap;
    uint32_t inl[ARGS_INLINE];
} ArgVec;

static void av_push(ArgVec *v, uint32_t x)
{
    if (!v->data) {
        v->data = v->inl;
        v->cap = ARGS_INLINE;
    }
    if (v->len == v->cap) {
        uint32_t *n = xmalloc(sizeof(uint32_t) * v->cap * 2);
        memcpy(n, v->data, sizeof(uint32_t) * v->len);
        if (v->data != v->inl)
            free(v->data);
        v->data = n;
        v->cap *= 2;
    }
    v->data[v->len++] = x;
}

static void av_free(ArgVec *v)
{
    if (v->data && v->data != v->inl)
        free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}

typedef struct Args {
    TokBuf all;              /* every token consumed after the name */
    ArgVec start;            /* per argument: index into all */
    ArgVec count;
    ArgVec present;          /* 0 for an omitted variadic argument */
    TokBuf *expanded;        /* lazily computed per parameter */
    TokBuf exp_inl[ARGS_INLINE];
    SrcLoc rparen_loc;
    uint32_t rparen_len;
} Args;

static TokSpan arg_span(const Args *a, int i)
{
    TokSpan s;
    s.t = a->all.t + a->start.data[i];
    s.n = a->count.data[i];
    return s;
}

static void args_free(PP *pp, Args *a, int nparams)
{
    int i;
    if (a->expanded) {
        for (i = 0; i < nparams; i++)
            tokbuf_release(pp, &a->expanded[i]);
        if (a->expanded != a->exp_inl)
            free(a->expanded);
    }
    a->expanded = NULL;
    tokbuf_release(pp, &a->all);
    av_free(&a->start);
    av_free(&a->count);
    av_free(&a->present);
}

static void paint(PP *pp, Tok *t)
{
    if (t->kind == TK_IDENT) {
        Macro *m = pp->in->byid.data[t->aux]->macro;
        if (m && m->disabled)
            t->flags |= TF_NOEXPAND;
    }
}

/* ---- argument collection ------------------------------------------- */

/* The '(' has been read.  On failure the consumed tokens are replayed. */
static bool collect_args(PP *pp, Macro *m, const Tok *name, const Tok *lparen,
                         Args *a)
{
    int depth = 0;
    uint32_t arg_begin;
    bool replayable = true;
    memset(a, 0, sizeof *a);
    tokbuf_init(pp, &a->all, 32);
    tokbuf_push(pp, &a->all, *lparen);
    arg_begin = a->all.len;
    for (;;) {
        Tok t;
        TokSrc src = pp_read_raw(pp, &t);
        if (t.kind == TK_EOF) {
            pp_error_at(pp, name,
                        "unterminated argument list invoking macro \"%s\"",
                        m->name->str);
            if (src == SRC_LEXER)
                pp_unread(pp, &t, src); /* let the caller see the EOF */
            args_free(pp, a, 0);
            return false;
        }
        if (src == SRC_LEXER && (t.flags & TF_BOL) && tok_is_punct(&t, P_HASH) &&
            !pp->in_directive) {
            /* C99 6.10.3p11: undefined; GCC and Clang process it */
            diag_report(pp->diag, DL_WARNING, "directive-in-macro-args", t.loc,
                        "preprocessing directive inside the arguments of "
                        "macro \"%s\" is undefined behavior", m->name->str);
            pp_directive(pp, &t);
            replayable = false;
            continue;
        }
        paint(pp, &t);
        if (tok_is_punct(&t, P_LPAREN)) {
            depth++;
        } else if (tok_is_punct(&t, P_RPAREN)) {
            if (depth == 0) {
                av_push(&a->start, arg_begin);
                av_push(&a->count, a->all.len - arg_begin);
                av_push(&a->present, 1);
                a->rparen_loc = t.loc;
                a->rparen_len = t.len;
                tokbuf_push(pp, &a->all, t);
                break;
            }
            depth--;
        } else if (tok_is_punct(&t, P_COMMA) && depth == 0 &&
                   !(m->variadic && (int)a->start.len == m->nparams - 1)) {
            av_push(&a->start, arg_begin);
            av_push(&a->count, a->all.len - arg_begin);
            av_push(&a->present, 1);
            tokbuf_push(pp, &a->all, t);
            arg_begin = a->all.len;
            continue;
        }
        if (t.flags & TF_BOL)
            t.flags = (uint16_t)((t.flags & ~TF_BOL) | TF_SPACE);
        tokbuf_push(pp, &a->all, t);
    }

    if (m->nparams == 0 && a->start.len == 1 && a->count.data[0] == 0)
        a->start.len = a->count.len = a->present.len = 0;

    if (m->variadic && (int)a->start.len == m->nparams - 1) {
        if (pp->opt->pedantic && !m->gnu_named_variadic)
            diag_report(pp->diag, DL_WARNING, "pedantic", name->loc,
                        "ISO C99 requires at least one argument for the "
                        "\"...\" in a variadic macro");
        av_push(&a->start, a->all.len);
        av_push(&a->count, 0);
        av_push(&a->present, 0);
    }
    if ((int)a->start.len != m->nparams) {
        if ((int)a->start.len > m->nparams)
            pp_error_at(pp, name,
                        "macro \"%s\" passed %d arguments, but takes just %d",
                        m->name->str, (int)a->start.len, m->nparams);
        else
            pp_error_at(pp, name,
                        "macro \"%s\" requires %d arguments, but only %d given",
                        m->name->str, m->nparams, (int)a->start.len);
        if (replayable) {
            /* hand the consumed tokens back, unexpanded name first */
            Context c;
            memset(&c, 0, sizeof c);
            c.owned = a->all;
            a->all.t = NULL;
            c.toks = c.owned.t;
            c.end = c.owned.len;
            c.exp_id = pp->tok_exp_id;
            c.root_id = pp->tok_root;
            c.exp_loc = pp->tok_exp_loc;
            pp_push_context(pp, c);
        }
        args_free(pp, a, 0);
        return false;
    }
    a->expanded = m->nparams <= ARGS_INLINE
                      ? a->exp_inl
                      : xcalloc((size_t)m->nparams + 1, sizeof(TokBuf));
    return true;
}

/* ---- sub-stream expansion ------------------------------------------ */

static void expand_into(PP *pp, TokSpan in, TokBuf *out, SrcLoc exp_loc,
                        uint32_t exp_id, uint32_t root)
{
    Context c;
    size_t base = pp->ctx.len;
    bool carry = pp->carry_space;
    memset(&c, 0, sizeof c);
    c.toks = in.t;
    c.end = in.n;
    c.barrier = true;
    c.exp_loc = exp_loc;
    c.exp_id = exp_id;
    c.root_id = root;
    pp->carry_space = false;
    pp_push_context(pp, c);
    for (;;) {
        Tok t;
        TokSrc src = pp_read_raw(pp, &t);
        if (src == SRC_BARRIER)
            break;
        if (t.kind == TK_IDENT) {
            Ident *id = pp->in->byid.data[t.aux];
            if (pp->in_if_expr && id == pp->id_defined) {
                /* keep `defined X` / `defined ( X )` unexpanded */
                Tok u;
                TokSrc us;
                if (src == SRC_CONTEXT && pp->ctx.len - 1 != base)
                    pp_warn_at(pp, &t, "expansion-to-defined",
                               "macro expansion producing 'defined' has "
                               "undefined behavior");
                tokbuf_push(pp, out, t);
                us = pp_read_raw(pp, &u);
                if (us != SRC_BARRIER && tok_is_punct(&u, P_LPAREN)) {
                    tokbuf_push(pp, out, u);
                    us = pp_read_raw(pp, &u);
                }
                if (us != SRC_BARRIER && u.kind == TK_IDENT)
                    tokbuf_push(pp, out, u);
                else
                    pp_unread(pp, &u, us);
                continue;
            }
            if (id->macro && !(t.flags & TF_NOEXPAND)) {
                if (id->macro->disabled) {
                    t.flags |= TF_NOEXPAND;
                } else {
                    SrcLoc el = pp->tok_exp_loc;
                    uint32_t eid = pp->tok_exp_id, rt = pp->tok_root;
                    if (pp_try_expand(pp, &t, src))
                        continue;
                    pp->tok_exp_loc = el;
                    pp->tok_exp_id = eid;
                    pp->tok_root = rt;
                }
            }
        }
        if (pp->carry_space) {
            t.flags |= TF_SPACE;
            pp->carry_space = false;
        }
        tokbuf_push(pp, out, t);
    }
    /* everything above the barrier is exhausted and gone */
    while (pp->ctx.len > base + 1) {
        Context *x = &vec_last(&pp->ctx);
        if (x->macro)
            x->macro->disabled = false;
        tokbuf_release(pp, &x->owned);
        pp->ctx.len--;
    }
    pp->ctx.len = base;
    pp->carry_space = carry;
}

void pp_expand_into(PP *pp, TokSpan in, TokBuf *out)
{
    expand_into(pp, in, out, in.n ? in.t[0].loc : 0, NO_EXP, NO_EXP);
}

static bool needs_expansion(PP *pp, TokSpan s)
{
    uint32_t i;
    for (i = 0; i < s.n; i++)
        if (s.t[i].kind == TK_IDENT && !(s.t[i].flags & TF_NOEXPAND) &&
            pp->in->byid.data[s.t[i].aux]->macro)
            return true;
    return false;
}

static TokSpan expanded_arg(PP *pp, Args *a, int i, SrcLoc exp_loc,
                            uint32_t exp_id, uint32_t root)
{
    TokSpan raw = arg_span(a, i), r;
    if (!needs_expansion(pp, raw))
        return raw;
    if (!a->expanded[i].t) {
        bool saved = pp->collecting_args;
        pp->collecting_args = true;
        tokbuf_init(pp, &a->expanded[i], raw.n + 8);
        expand_into(pp, raw, &a->expanded[i], exp_loc, exp_id, root);
        pp->collecting_args = saved;
    }
    r.t = a->expanded[i].t;
    r.n = a->expanded[i].len;
    return r;
}

/* ---- # and ## ------------------------------------------------------- */

static Tok stringize(PP *pp, TokSpan arg, const Tok *hash, SrcLoc site,
                     Macro *m)
{
    StrBuf *sb = &pp->sb;
    uint32_t i, k;
    Tok r;
    Lexer L;
    Tok chk;
    SrcLoc sl;
    sb->len = 0;
    sb_putc(sb, '"');
    for (i = 0; i < arg.n; i++) {
        const Tok *t = &arg.t[i];
        const char *s = pp_text(pp, t);
        if (i && (t->flags & (TF_SPACE | TF_BOL)))
            sb_putc(sb, ' ');
        if (t->kind == TK_STRING || t->kind == TK_CHAR ||
            (t->kind == TK_OTHER && (t->flags & TF_UNTERMINATED))) {
            for (k = 0; k < t->len; k++) {
                if (s[k] == '"' || s[k] == '\\')
                    sb_putc(sb, '\\');
                sb_putc(sb, s[k]);
            }
        } else {
            sb_putn(sb, s, t->len);
        }
    }
    sb_putc(sb, '"');
    sl = srcmgr_scratch(pp->sm, sb->data, sb->len);
    lexer_init_range(&L, pp->sm, pp->in, pp->opt->lex, sl, (uint32_t)sb->len);
    lex_next(&L, &chk);
    if (chk.kind != TK_STRING || L.p != L.lim) {
        Diagnostic *d = diag_report(pp->diag, DL_WARNING, "", site,
            "invalid string literal produced by '#' (C99 6.10.3.2p2 "
            "undefined behavior)");
        diag_note(pp->diag, d, hash->loc, "'#' in the definition of '%s'",
                  m->name->str);
    }
    lexer_free(&L);
    memset(&r, 0, sizeof r);
    r.kind = TK_STRING;
    r.loc = hash->loc;
    r.len = (uint32_t)sb->len;
    r.aux = sl;
    r.flags = TF_SPELL | TF_SYNTH | TF_ORIGIN_BODY;
    return r;
}

static bool paste(PP *pp, const Tok *lhs, const Tok *rhs, const Tok *op,
                  Macro *m, SrcLoc site, Tok *out)
{
    StrBuf *sb = &pp->sb;
    Lexer L;
    Tok r;
    SrcLoc sl;
    bool valid;
    sb->len = 0;
    sb_putn(sb, pp_text(pp, lhs), lhs->len);
    sb_putn(sb, pp_text(pp, rhs), rhs->len);
    sl = srcmgr_scratch(pp->sm, sb->data ? sb->data : "", sb->len);
    lexer_init_range(&L, pp->sm, pp->in, pp->opt->lex, sl, (uint32_t)sb->len);
    lex_next(&L, &r);
    valid = r.kind != TK_EOF && !(r.flags & TF_UNTERMINATED) && L.p == L.lim;
    lexer_free(&L);
    if (!valid) {
        Diagnostic *d = diag_report(pp->diag, DL_ERROR, "", site,
            "pasting \"%.*s\" and \"%.*s\" does not give a valid preprocessing "
            "token (C99 6.10.3.3p3 undefined behavior)", (int)lhs->len,
            pp_text(pp, lhs), (int)rhs->len, pp_text(pp, rhs));
        diag_note(pp->diag, d, op->loc, "'##' in the definition of '%s'",
                  m->name->str);
        return false;
    }
    if (r.kind != TK_IDENT) {
        r.aux = r.loc;
        r.flags |= TF_SPELL;
    }
    /* the pasted spelling exists nowhere in the source: no ORIGIN_* */
    r.flags = (uint16_t)((r.flags & (TF_SPELL | TF_DIGRAPH)) |
                         (lhs->flags & (TF_SPACE | TF_BOL)) | TF_PASTED);
    r.loc = lhs->loc;
    *out = r;
    return true;
}

/* ---- substitution --------------------------------------------------- */

#define TK_PLACEMARKER 0xFF

static void push_arg(PP *pp, TokBuf *out, TokSpan s, uint16_t lead)
{
    uint32_t i;
    if (s.n == 0) {
        Tok pm;
        memset(&pm, 0, sizeof pm);
        pm.kind = TK_PLACEMARKER;
        pm.flags = lead;
        tokbuf_push(pp, out, pm);
        return;
    }
    for (i = 0; i < s.n; i++) {
        Tok c = s.t[i];
        c.flags |= TF_ORIGIN_ARG;
        if (i == 0)
            c.flags = (uint16_t)((c.flags & ~(TF_SPACE | TF_BOL)) | lead);
        tokbuf_push(pp, out, c);
    }
}

static void subst(PP *pp, Macro *m, Args *a, uint16_t lead, SrcLoc site,
                  SrcLoc exp_loc, uint32_t exp_id, uint32_t root, TokBuf *out)
{
    uint32_t i, w, r;
    bool first = true;
    uint16_t pending_space = 0;
    tokbuf_init(pp, out, m->body_len + 8);

    for (i = 0; i < m->body_len; i++) {
        const Tok *t = &m->body[i];
        uint16_t fl = (uint16_t)(i == 0 ? lead : (t->flags & TF_SPACE));

        if (!m->has_ops && !(t->flags & TF_PARAM)) {
            Tok c = *t;
            c.flags = (uint16_t)((c.flags & ~(TF_SPACE | TF_BOL)) | fl |
                                 TF_ORIGIN_BODY);
            tokbuf_push(pp, out, c);
            continue;
        }

        if (m->funclike && tok_is_punct(t, P_HASH) && i + 1 < m->body_len &&
            (m->body[i + 1].flags & TF_PARAM)) {
            Tok s = stringize(pp, arg_span(a, m->body[i + 1].punct), t, site, m);
            s.flags |= fl;
            tokbuf_push(pp, out, s);
            i++;
            continue;
        }

        if (tok_is_punct(t, P_HASHHASH)) {
            const Tok *op = t, *rt = &m->body[i + 1];
            TokSpan rhs;
            Tok single;
            int rp = (rt->flags & TF_PARAM) ? rt->punct : -1;
            i++;
            if (rp >= 0 && m->variadic && rp == m->nparams - 1 && out->len &&
                tok_is_punct(&out->t[out->len - 1], P_COMMA) &&
                pp->opt->gnu_extensions) {
                /* GNU: , ## __VA_ARGS__ drops the comma when the variable
                 * arguments are omitted entirely (GCC keeps it for F(a,)) */
                if (!a->present.data[rp]) {
                    out->len--;
                    if (pp->opt->pedantic)
                        diag_report(pp->diag, DL_WARNING, "pedantic", op->loc,
                                    "token pasting of ',' and __VA_ARGS__ is "
                                    "a GNU extension");
                } else {
                    push_arg(pp, out, arg_span(a, rp),
                             (uint16_t)(rt->flags & TF_SPACE));
                }
                continue;
            }
            if (rp >= 0) {
                rhs = arg_span(a, rp);
            } else {
                single = *rt;
                single.flags = (uint16_t)((single.flags & ~TF_BOL) | TF_ORIGIN_BODY);
                rhs.t = &single;
                rhs.n = 1;
            }
            if (rhs.n == 0)
                continue; /* x ## <empty>: lhs unchanged */
            {
                Tok first_rhs = rhs.t[0];
                uint32_t k;
                if (rp >= 0)
                    first_rhs.flags |= TF_ORIGIN_ARG;
                if (out->len == 0 || out->t[out->len - 1].kind == TK_PLACEMARKER) {
                    /* <empty> ## y */
                    if (out->len) {
                        first_rhs.flags = (uint16_t)((first_rhs.flags &
                                                      ~(TF_SPACE | TF_BOL)) |
                                                     out->t[out->len - 1].flags);
                        out->t[out->len - 1] = first_rhs;
                    } else {
                        tokbuf_push(pp, out, first_rhs);
                    }
                } else {
                    Tok res;
                    if (paste(pp, &out->t[out->len - 1], &first_rhs, op, m,
                              site, &res))
                        out->t[out->len - 1] = res;
                    else
                        tokbuf_push(pp, out, first_rhs);
                }
                for (k = 1; k < rhs.n; k++) {
                    Tok c = rhs.t[k];
                    c.flags |= TF_ORIGIN_ARG;
                    tokbuf_push(pp, out, c);
                }
            }
            continue;
        }

        if (t->flags & TF_PARAM) {
            int pi = t->punct;
            bool before_paste = i + 1 < m->body_len &&
                                tok_is_punct(&m->body[i + 1], P_HASHHASH);
            TokSpan s = before_paste
                            ? arg_span(a, pi)
                            : expanded_arg(pp, a, pi, exp_loc, exp_id, root);
            push_arg(pp, out, s, fl);
            continue;
        }

        {
            Tok c = *t;
            c.flags = (uint16_t)((c.flags & ~(TF_SPACE | TF_BOL)) | fl |
                                 TF_ORIGIN_BODY);
            tokbuf_push(pp, out, c);
        }
    }

    /* drop placemarkers; the first real token keeps the name's spacing */
    for (r = w = 0; r < out->len; r++) {
        Tok t = out->t[r];
        if (t.kind == TK_PLACEMARKER) {
            if (first)
                pending_space |= t.flags & TF_SPACE;
            continue;
        }
        if (first) {
            t.flags = (uint16_t)((t.flags & ~(TF_SPACE | TF_BOL)) | lead |
                                 pending_space);
            first = false;
        }
        out->t[w++] = t;
    }
    out->len = w;
}

/* ---- builtins ------------------------------------------------------- */

static char *quote_string(PP *pp, const char *s)
{
    StrBuf sb = {0};
    char *r;
    sb_putc(&sb, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\')
            sb_putc(&sb, '\\');
        sb_putc(&sb, *s);
    }
    sb_putc(&sb, '"');
    r = arena_strndup(pp->arena, sb.data, sb.len);
    sb_free(&sb);
    return r;
}

static bool name_in_table(const char *const *table, const char *s, size_t n)
{
    int i;
    if (!table)
        return false;
    if (n > 4 && s[0] == '_' && s[1] == '_' && s[n - 1] == '_' && s[n - 2] == '_') {
        s += 2;
        n -= 4;
    }
    for (i = 0; table[i]; i++) {
        const char *x = table[i];
        size_t xn = strlen(x);
        if (xn > 4 && x[0] == '_' && x[1] == '_' && x[xn - 1] == '_' &&
            x[xn - 2] == '_') {
            x += 2;
            xn -= 4;
        }
        if (xn == n && memcmp(x, s, n) == 0)
            return true;
    }
    return false;
}

/* __has_include(...) and friends: the operand is read unexpanded. */
static bool builtin_query(PP *pp, Macro *m, const Tok *name, bool *result)
{
    Tok t;
    TokSrc src = pp_read_raw(pp, &t);
    TokBuf op = {0};
    int depth = 0;
    *result = false;
    if (!tok_is_punct(&t, P_LPAREN)) {
        pp_unread(pp, &t, src);
        pp_error_at(pp, name, "missing '(' after \"%s\"", m->name->str);
        return false;
    }
    for (;;) {
        src = pp_read_raw(pp, &t);
        if (t.kind == TK_EOF) {
            pp_unread(pp, &t, src);
            pp_error_at(pp, name, "missing ')' after \"%s\"", m->name->str);
            tokbuf_release(pp, &op);
            return false;
        }
        if (tok_is_punct(&t, P_LPAREN))
            depth++;
        else if (tok_is_punct(&t, P_RPAREN) && depth-- == 0)
            break;
        tokbuf_push(pp, &op, t);
    }
    switch (m->builtin) {
    case BUILTIN_HAS_INCLUDE:
    case BUILTIN_HAS_INCLUDE_NEXT: {
        char *hname = NULL;
        bool angled = false;
        int di;
        if (!pp->in_if_expr)
            pp_error_at(pp, name, "\"%s\" used outside of preprocessing "
                                  "directive", m->name->str);
        if (op.len && op.t[0].kind == TK_STRING && pp_text(pp, &op.t[0])[0] == '"') {
            hname = arena_strndup(pp->arena, pp_text(pp, &op.t[0]) + 1,
                                  op.t[0].len - 2);
        } else if (op.len && tok_is_punct(&op.t[0], P_LT)) {
            StrBuf sb = {0};
            uint32_t u;
            angled = true;
            for (u = 1; u < op.len && !tok_is_punct(&op.t[u], P_GT); u++) {
                if (u > 1 && (op.t[u].flags & TF_SPACE))
                    sb_putc(&sb, ' ');
                sb_putn(&sb, pp_text(pp, &op.t[u]), op.t[u].len);
            }
            hname = arena_strndup(pp->arena, sb_cstr(&sb), sb.len);
            sb_free(&sb);
        } else {
            pp_error_at(pp, name, "operator \"%s\" requires a header-name",
                        m->name->str);
            tokbuf_release(pp, &op);
            return false;
        }
        *result = pp_find_include(pp, hname, angled,
                                  m->builtin == BUILTIN_HAS_INCLUDE_NEXT &&
                                      pp->inc->prev != NULL,
                                  &di) != NULL;
        break;
    }
    default:
        if (op.len == 1 && op.t[0].kind == TK_IDENT) {
            Ident *id = pp->in->byid.data[op.t[0].aux];
            if (m->builtin == BUILTIN_HAS_ATTRIBUTE ||
                m->builtin == BUILTIN_HAS_C_ATTRIBUTE)
                *result = name_in_table(pp->host_attrs, id->str, id->len);
            else if (m->builtin == BUILTIN_HAS_BUILTIN)
                *result = name_in_table(pp->host_builtins, id->str, id->len);
        }
    }
    tokbuf_release(pp, &op);
    return true;
}

static Expansion *new_expansion(PP *pp, Macro *m, const Tok *name,
                                uint32_t parent, uint32_t parent_root)
{
    Expansion *e;
    if (pp->track == TRACK_NONE)
        return NULL;
    e = NEW(pp->arena, Expansion);
    e->id = (uint32_t)pp->expansions.len;
    e->parent = parent;
    e->root = parent == NO_EXP ? e->id : parent_root;
    e->depth = (uint16_t)(parent == NO_EXP ? 0
                          : pp->expansions.data[parent]->depth + 1);
    e->name_flags = (uint16_t)(name->flags & (TF_ORIGIN_BODY | TF_ORIGIN_ARG |
                                              TF_PASTED));
    e->in_directive = pp->in_directive;
    e->macro = m;
    e->name_loc = name->loc;
    e->end_loc = name->loc + name->len;
    vec_push(&pp->expansions, e);
    return e;
}

static void push_single(PP *pp, Tok t, Macro *m, SrcLoc exp_loc,
                        uint32_t exp_id, uint32_t root, SrcLoc name_loc)
{
    Context c;
    memset(&c, 0, sizeof c);
    tokbuf_init(pp, &c.owned, 1);
    c.owned.t[0] = t;
    c.owned.len = 1;
    c.toks = c.owned.t;
    c.end = 1;
    c.macro = m;
    c.exp_id = exp_id;
    c.root_id = root;
    c.exp_loc = exp_loc;
    c.name_loc = name_loc;
    pp_push_context(pp, c);
}

static Tok builtin_token(PP *pp, Macro *m, const Tok *name, SrcLoc exp_loc)
{
    char buf[64];
    const char *s = buf;
    TokKind k = TK_PPNUM;
    uint16_t fl = (uint16_t)((name->flags & (TF_SPACE | TF_BOL)) | TF_SYNTH);
    switch (m->builtin) {
    case BUILTIN_LINE:
        sprintf(buf, "%u", pp_presumed_line(pp, exp_loc));
        break;
    case BUILTIN_FILE:
        s = quote_string(pp, pp->inc->presumed_name);
        k = TK_STRING;
        break;
    case BUILTIN_BASE_FILE:
        s = quote_string(pp, pp->main_file->name);
        k = TK_STRING;
        break;
    case BUILTIN_DATE:
        s = pp->opt->date_str;
        k = TK_STRING;
        break;
    case BUILTIN_TIME:
        s = pp->opt->time_str;
        k = TK_STRING;
        break;
    case BUILTIN_COUNTER:
        sprintf(buf, "%u", pp->counter++);
        break;
    default: /* BUILTIN_INCLUDE_LEVEL */
        sprintf(buf, "%d", pp->include_depth > 0 ? pp->include_depth - 1 : 0);
        break;
    }
    return pp_make_token(pp, k, s, strlen(s), name->loc, fl);
}

/* ---- _Pragma / #pragma ---------------------------------------------- */

static Ident *pragma_macro_name(PP *pp, TokSpan s)
{
    if (s.n < 3 || !tok_is_punct(&s.t[0], P_LPAREN) || s.t[1].kind != TK_STRING ||
        !tok_is_punct(&s.t[2], P_RPAREN)) {
        diag_report(pp->diag, DL_WARNING, "unknown-pragma",
                    s.n ? s.t[0].loc : 0,
                    "expected (\"name\") in push_macro/pop_macro pragma");
        return NULL;
    }
    return intern(pp->in, pp_text(pp, &s.t[1]) + 1, s.t[1].len - 2);
}

static bool word(PP *pp, TokSpan s, uint32_t i, const char *w)
{
    return i < s.n && tok_is_word(pp->in, &s.t[i], w);
}

void pp_do_pragma(PP *pp, TokSpan toks, SrcLoc loc)
{
    bool emit = true;
    uint32_t i;
    PP_EMIT(pp, pragma, loc, toks);
    if (word(pp, toks, 0, "once")) {
        if (pp->inc->prev == NULL)
            diag_report(pp->diag, DL_WARNING, "", toks.t[0].loc,
                        "#pragma once in main file");
        pp->inc->file->pragma_once = true;
        emit = false;
    } else if (word(pp, toks, 0, "STDC")) {
        if (!(word(pp, toks, 1, "FP_CONTRACT") || word(pp, toks, 1, "FENV_ACCESS") ||
              word(pp, toks, 1, "CX_LIMITED_RANGE")) ||
            !(word(pp, toks, 2, "ON") || word(pp, toks, 2, "OFF") ||
              word(pp, toks, 2, "DEFAULT")) ||
            toks.n != 3)
            diag_report(pp->diag, DL_WARNING, "stdc-pragma", toks.t[0].loc,
                        "malformed or unknown STDC pragma (C99 6.10.6p2)");
    } else if (word(pp, toks, 0, "GCC")) {
        if (word(pp, toks, 1, "system_header")) {
            if (pp->inc->prev)
                pp->inc->file->system_header = true;
            emit = false;
        } else if (word(pp, toks, 1, "poison")) {
            for (i = 2; i < toks.n; i++)
                if (toks.t[i].kind == TK_IDENT) {
                    Ident *id = pp->in->byid.data[toks.t[i].aux];
                    id->flags |= IDF_POISONED;
                    PP_EMIT(pp, macro_ref, id, id->macro, &toks.t[i], REF_PRAGMA);
                }
        } else if (word(pp, toks, 1, "warning") || word(pp, toks, 1, "error")) {
            bool err = word(pp, toks, 1, "error");
            uint32_t k = 2;
            const Tok *msg;
            if (k < toks.n && tok_is_punct(&toks.t[k], P_LPAREN))
                k++;
            if (k < toks.n) {
                msg = &toks.t[k];
                diag_report(pp->diag, err ? DL_ERROR : DL_WARNING,
                            err ? "" : "pp-warning-directive", toks.t[1].loc,
                            "%.*s",
                            msg->kind == TK_STRING ? (int)msg->len - 2 : (int)msg->len,
                            pp_text(pp, msg) + (msg->kind == TK_STRING));
            }
        }
    } else if (word(pp, toks, 0, "push_macro")) {
        TokSpan rest;
        Ident *id;
        rest.t = toks.t + 1;
        rest.n = toks.n - 1;
        id = pragma_macro_name(pp, rest);
        if (id) {
            MacroStackEnt *e = NEW(pp->arena, MacroStackEnt);
            e->name = id;
            e->macro = id->macro;
            e->next = pp->pushed;
            pp->pushed = e;
        }
    } else if (word(pp, toks, 0, "pop_macro")) {
        TokSpan rest;
        Ident *id;
        rest.t = toks.t + 1;
        rest.n = toks.n - 1;
        id = pragma_macro_name(pp, rest);
        if (id) {
            MacroStackEnt **pe = &pp->pushed;
            while (*pe && (*pe)->name != id)
                pe = &(*pe)->next;
            if (!*pe) {
                diag_report(pp->diag, DL_WARNING, "unbalanced-push-pop-macro",
                            toks.t[0].loc, "pop_macro(\"%s\") without push_macro",
                            id->str);
            } else {
                Macro *restored = (*pe)->macro;
                if (id->macro && id->macro != restored) {
                    id->macro->undef_loc = toks.t[0].loc;
                    id->macro->undef_seq = pp->seq;
                }
                id->macro = restored;
                if (restored)
                    restored->undef_loc = 0, restored->undef_seq = 0;
                pp->seq++;
                *pe = (*pe)->next;
                PP_EMIT(pp, checkpoint, toks.t[0].loc, pp->seq);
            }
        }
    } else if (toks.n) {
        diag_report(pp->diag, DL_WARNING, "unknown-pragma", toks.t[0].loc,
                    "unknown pragma ignored");
    }
    if (emit) {
        StrBuf *sb = &pp->sb;
        Tok t;
        sb->len = 0;
        sb_puts(sb, "pragma");
        for (i = 0; i < toks.n; i++) {
            sb_putc(sb, ' ');
            sb_putn(sb, pp_text(pp, &toks.t[i]), toks.t[i].len);
        }
        memset(&t, 0, sizeof t);
        t.kind = TK_PRAGMA;
        t.loc = loc;
        t.len = (uint32_t)sb->len;
        t.aux = srcmgr_scratch(pp->sm, sb->data, sb->len);
        t.flags = TF_SPELL | TF_BOL;
        push_single(pp, t, NULL, loc, NO_EXP, NO_EXP, 0);
    }
}

static TokBuf destringize(PP *pp, const Tok *str)
{
    /* C99 6.10.9: delete L prefix and quotes; \" -> ", \\ -> \ */
    StrBuf *sb = &pp->sb;
    const char *s = pp_text(pp, str);
    uint32_t n = str->len, i;
    Lexer L;
    TokBuf out = {0};
    SrcLoc sl;
    if (s[0] == 'L') {
        s++;
        n--;
    }
    sb->len = 0;
    for (i = 1; i + 1 < n; i++) {
        if (s[i] == '\\' && (s[i + 1] == '"' || s[i + 1] == '\\') && i + 2 < n)
            i++;
        sb_putc(sb, s[i]);
    }
    sl = srcmgr_scratch(pp->sm, sb->data ? sb->data : "", sb->len);
    lexer_init_range(&L, pp->sm, pp->in, pp->opt->lex, sl, (uint32_t)sb->len);
    for (;;) {
        Tok t;
        lex_next(&L, &t);
        if (t.kind == TK_EOF)
            break;
        t.flags &= (uint16_t)~TF_BOL;
        tokbuf_push(pp, &out, t);
    }
    lexer_free(&L);
    return out;
}

/* _Pragma ( string-literal ) */
static bool do_pragma_op(PP *pp, const Tok *name)
{
    Tok lp, s, rp;
    TokSrc a, b, c;
    TokBuf toks;
    TokSpan span;
    if (pp->in_if_expr) {
        pp_error_at(pp, name, "_Pragma is not allowed in #if");
        return false;
    }
    if (pp->collecting_args)
        return false; /* deferred until the result is rescanned */
    a = pp_read_raw(pp, &lp);
    if (!tok_is_punct(&lp, P_LPAREN)) {
        pp_unread(pp, &lp, a);
        return false; /* plain identifier */
    }
    b = pp_read_raw(pp, &s);
    c = pp_read_raw(pp, &rp);
    if (s.kind != TK_STRING || !tok_is_punct(&rp, P_RPAREN)) {
        (void)b;
        (void)c;
        pp_error_at(pp, name, "_Pragma takes a parenthesized string literal");
        return true;
    }
    toks = destringize(pp, &s);
    span.t = toks.t;
    span.n = toks.len;
    pp_do_pragma(pp, span, name->loc);
    tokbuf_release(pp, &toks);
    return true;
}

/* ---- entry point ---------------------------------------------------- */

bool pp_try_expand(PP *pp, Tok *name, TokSrc src)
{
    Ident *id = pp->in->byid.data[name->aux];
    Macro *m = id->macro;
    uint32_t parent = src == SRC_CONTEXT ? pp->tok_exp_id : NO_EXP;
    uint32_t parent_root = pp->tok_root;
    SrcLoc exp_loc = pp->tok_exp_loc;
    SrcLoc site = exp_loc;
    uint16_t lead = (uint16_t)(name->flags & (TF_SPACE | TF_BOL));
    Expansion *e;
    uint32_t eid, root;
    Context c;

    if (m->builtin) {
        if (m->builtin == BUILTIN_PRAGMA_OP)
            return do_pragma_op(pp, name);
        if (m->builtin >= BUILTIN_HAS_INCLUDE) {
            Tok nt;
            TokSrc ns;
            bool r = false;
            ns = pp_read_raw(pp, &nt);
            pp_unread(pp, &nt, ns);
            if (!tok_is_punct(&nt, P_LPAREN) && !pp->in_if_expr)
                return false; /* plain identifier outside #if */
            e = new_expansion(pp, m, name, parent, parent_root);
            if (!builtin_query(pp, m, name, &r))
                r = false;
            m->expansions++;
            PP_EMIT(pp, expand, e, NULL, 0);
            push_single(pp, pp_make_token(pp, TK_PPNUM, r ? "1" : "0", 1,
                                          name->loc, (uint16_t)(lead | TF_SYNTH)),
                        NULL, exp_loc, e ? e->id : NO_EXP,
                        e ? e->root : NO_EXP, name->loc);
            return true;
        }
        e = new_expansion(pp, m, name, parent, parent_root);
        m->expansions++;
        PP_EMIT(pp, expand, e, NULL, 0);
        push_single(pp, builtin_token(pp, m, name, exp_loc), NULL, exp_loc,
                    e ? e->id : NO_EXP, e ? e->root : NO_EXP, name->loc);
        return true;
    }

    memset(&c, 0, sizeof c);
    if (!m->funclike) {
        e = new_expansion(pp, m, name, parent, parent_root);
        eid = e ? e->id : NO_EXP;
        root = e ? e->root : NO_EXP;
        m->expansions++;
        PP_EMIT(pp, expand, e, NULL, 0);
        if (m->body_len == 0) {
            if (lead & TF_SPACE)
                pp->carry_space = true;
            return true;
        }
        subst(pp, m, NULL, lead, site, exp_loc, eid, root, &c.owned);
    } else {
        Tok lp;
        TokSrc ls = pp_read_raw(pp, &lp);
        Args a;
        TokSpan *spans;
        int i;
        if (!tok_is_punct(&lp, P_LPAREN)) {
            pp_unread(pp, &lp, ls);
            return false;
        }
        if (!collect_args(pp, m, name, &lp, &a))
            return false;
        e = new_expansion(pp, m, name, parent, parent_root);
        eid = e ? e->id : NO_EXP;
        root = e ? e->root : NO_EXP;
        if (e)
            e->end_loc = a.rparen_loc + a.rparen_len;
        m->expansions++;
        if (pp->track != TRACK_NONE) {
            spans = xmalloc(sizeof(TokSpan) * ((size_t)m->nparams + 1));
            for (i = 0; i < m->nparams; i++)
                spans[i] = arg_span(&a, i);
            PP_EMIT(pp, expand, e, spans, m->nparams);
            free(spans);
        }
        subst(pp, m, &a, lead, site, exp_loc, eid, root, &c.owned);
        args_free(pp, &a, m->nparams);
    }
    if (c.owned.len == 0) {
        tokbuf_release(pp, &c.owned);
        if (lead & TF_SPACE)
            pp->carry_space = true;
        return true;
    }
    c.toks = c.owned.t;
    c.end = c.owned.len;
    c.macro = m;
    c.exp_id = eid;
    c.root_id = root;
    c.exp_loc = exp_loc;
    c.name_loc = name->loc;
    pp_push_context(pp, c);
    return true;
}
