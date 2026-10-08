/* ppexpand.c - macro replacement (C99 6.10.3), _Pragma (6.10.9), builtin
 * macros (6.10.8) and #pragma handling (6.10.6).
 *
 * The replacement of an invocation is built in a pooled buffer and pushed
 * as a Context; rescanning happens as the main loop reads from it.  The
 * macro stays disabled until its context is exhausted. */
#include "pp.h"

#include <string.h>
#include <time.h>
#include <sys/stat.h>

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
    TokBuf pragmas;          /* #pragma lines met inside the arguments */
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
        Macro *m = pp_macro(pp, ident_by_id(pp->in, t->aux));
        if (m && pp_macro_disabled(pp, m))
            t->flags |= TF_NOEXPAND;
    }
}

/* ---- argument collection ------------------------------------------- */

/* The '(' has been read.  On failure the consumed tokens are dropped and
 * the caller prints the name alone, as GCC does.  Either way, pragma
 * directives inside the arguments are left in a->pragmas for the caller:
 * GCC emits them before the invocation's result. */
static bool collect_args(PP *pp, Macro *m, const Tok *name, const Tok *lparen,
                         Args *a)
{
    int depth = 0;
    uint32_t arg_begin;
    memset(a, 0, sizeof *a);
    tokbuf_init(pp, &a->all, 32);
    tokbuf_push(pp, &a->all, *lparen);
    arg_begin = a->all.len;
    for (;;) {
        Tok t;
        TokSrc src = pp_read_raw(pp, &t);
        if (t.kind == TK_EOF) {
            Tok at = *name;     /* gcc: after the last token read */
            if (src == SRC_LEXER) {
                const Tok *last = &a->all.t[a->all.len - 1];
                at.loc = last->loc + last->len;
                at.len = 0;
            } else if (pp->in_directive) {
                /* the end of the directive line is gcc's EOF token */
                const Tok *last = &a->all.t[a->all.len - 1];
                at.loc = eol_after(pp, last->loc + last->len);
                at.len = 0;
            } else if (pp->last_lex_end) {
                /* the EOF came from an argument: gcc's cur_token[-1] is the
                 * last token the lexer produced */
                at.loc = pp->last_lex_loc;
                at.len = 0;
            }
            Diagnostic *ud = pp_error_at(pp, &at,
                        "unterminated argument list invoking macro \"%s\"",
                        m->name->str);
            if (ud)
                diag_set_range(ud, name->loc, name->loc + name->len);
            if (src == SRC_LEXER)
                pp_unread(pp, &t, src); /* let the caller see the EOF */
            args_free(pp, a, 0);
            return false;
        }
        if (src == SRC_LEXER && !pp->in_directive &&
            (t.kind == TK_DIRMARK ||
             ((t.flags & TF_BOL) && tok_is_punct(&t, P_HASH)))) {
            /* C99 6.10.3p11: undefined; GCC and Clang process it.  In phase
             * B the directive already ran in phase A: advance the version. */
            if (pp->diag->pedantic)
                diag_report(pp->diag, pp->diag->pedantic_errors ? DL_ERROR : DL_WARNING,
                            "directive-in-macro-args", t.loc,
                            "embedding a directive within macro arguments is not portable");
            if (t.kind == TK_DIRMARK)
                pp_plan_apply_dir(pp, t.aux);
            else
                pp_directive(pp, &t);
            continue;
        }
        if (t.kind == TK_PRAGMA) {
            tokbuf_push(pp, &a->pragmas, t);
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
        /* gcc: at the ')', and only where __VA_OPT__ is not available (the
         * GNU modes), with no option tag */
        if (pp->opt->pedantic && !pp->opt->gnu_mode)
            pp_pedwarn(pp, a->rparen_loc, "ISO C99 requires at least one "
                       "argument for the \"...\" in a variadic macro");
        av_push(&a->start, a->all.len);
        av_push(&a->count, 0);
        av_push(&a->present, 0);
    }
    /* libcpp: F() of a macro whose only parameter is variadic counts as
     * omitting it, except in the ISO (non-GNU) modes: , ## drops the comma */
    if (m->variadic && m->nparams == 1 && a->start.len == 1 &&
        a->count.data[0] == 0 && pp->opt->gnu_mode)
        a->present.data[0] = 0;
    if ((int)a->start.len != m->nparams) {
        Tok rp;
        memset(&rp, 0, sizeof rp);
        rp.loc = a->rparen_loc;
        rp.len = a->rparen_len;
        if ((int)a->start.len > m->nparams)
            pp_error_at(pp, &rp,
                        "macro \"%s\" passed %d arguments, but takes just %d",
                        m->name->str, (int)a->start.len, m->nparams);
        else
            pp_error_at(pp, &rp,
                        "macro \"%s\" requires %d arguments, but only %d given",
                        m->name->str, m->nparams, (int)a->start.len);
        args_free(pp, a, 0);
        return false;
    }
    if (m->nparams > 0 && diag_enabled(pp->diag, "c90-c99-compat")) {
        int k;
        for (k = 0; k < m->nparams; k++)
            if (a->count.data[k] == 0)
                diag_report(pp->diag, DL_WARNING, "c90-c99-compat",
                            a->rparen_loc,
                            "invoking macro %s argument %d: empty macro "
                            "arguments are undefined in ISO C90",
                            m->name->str, k + 1);
    }
    a->expanded = m->nparams <= ARGS_INLINE
                      ? a->exp_inl
                      : xcalloc((size_t)m->nparams + 1, sizeof(TokBuf));
    return true;
}

/* ---- sub-stream expansion ------------------------------------------ */

static void expand_into(PP *pp, TokSpan in, TokBuf *out, SrcLoc exp_loc,
                        uint32_t exp_id, uint32_t root, bool self_loc)
{
    Context c;
    size_t base = pp->ctx.len;
    Tok lastlex = {0};
    bool carry = pp->carry_space;
    memset(&c, 0, sizeof c);
    c.toks = in.t;
    c.end = in.n;
    c.barrier = true;
    c.self_loc = self_loc;
    c.root_obj = self_loc && pp->subst_root_obj;
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
        if (src == SRC_CONTEXT && pp->ctx.len - 1 == base)
            lastlex = t;        /* the last token of the source line read */
        if (pp->in_if_expr && tok_is_punct(&t, P_HASH)) {
            /* #pred(answer), a GCC assertion: kept unexpanded */
            Tok u;
            TokSrc us;
            tokbuf_push(pp, out, t);
            us = pp_read_raw(pp, &u);
            if (us == SRC_BARRIER || u.kind != TK_IDENT) {
                pp_unread(pp, &u, us);
                continue;
            }
            tokbuf_push(pp, out, u);
            us = pp_read_raw(pp, &u);
            if (us == SRC_BARRIER || !tok_is_punct(&u, P_LPAREN)) {
                pp_unread(pp, &u, us);
                continue;
            }
            do {
                tokbuf_push(pp, out, u);
                us = pp_read_raw(pp, &u);
            } while (us != SRC_BARRIER && !tok_is_punct(&u, P_RPAREN));
            if (us == SRC_BARRIER)
                pp_unread(pp, &u, us);
            else
                tokbuf_push(pp, out, u);
            continue;
        }
        if (t.kind == TK_IDENT) {
            Ident *id = ident_by_id(pp->in, t.aux);
            if (pp->in_if_expr && id == pp->id_defined) {
                /* keep `defined X` / `defined ( X )` unexpanded */
                Tok u;
                TokSrc us;
                bool viamacro = src == SRC_CONTEXT && pp->ctx.len - 1 != base;
                bool paren = false;
                tokbuf_push(pp, out, t);
                us = pp_read_raw(pp, &u);
                if (us == SRC_CONTEXT && pp->ctx.len - 1 == base)
                    lastlex = u;
                if (us != SRC_BARRIER && tok_is_punct(&u, P_LPAREN)) {
                    paren = true;
                    tokbuf_push(pp, out, u);
                    us = pp_read_raw(pp, &u);
                    if (us == SRC_CONTEXT && pp->ctx.len - 1 == base)
                        lastlex = u;
                }
                if (us != SRC_BARRIER && u.kind == TK_IDENT) {
                    tokbuf_push(pp, out, u);
                    if (paren) {        /* libcpp reads the ')' too */
                        TokSrc vs = pp_read_raw(pp, &u);
                        if (vs != SRC_BARRIER && tok_is_punct(&u, P_RPAREN)) {
                            tokbuf_push(pp, out, u);
                            if (vs == SRC_CONTEXT && pp->ctx.len - 1 == base)
                                lastlex = u;
                        } else {
                            pp_unread(pp, &u, vs);
                        }
                    }
                    if (viamacro) {
                        Diagnostic *wd = diag_report(
                            pp->diag, DL_WARNING, "expansion-to-defined",
                            lastlex.loc, "this use of \"defined\" may not be "
                            "portable");
                        diag_set_range(wd, lastlex.loc,
                                       lastlex.loc + lastlex.len);
                    }
                } else {
                    pp_unread(pp, &u, us);
                }
                continue;
            }
            Macro *im = pp_macro(pp, id);
            if (im && !(t.flags & TF_NOEXPAND)) {
                if (pp_macro_disabled(pp, im)) {
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
        if (pp->in_if_expr && pp->diag->track0 && src == SRC_CONTEXT &&
            t.kind == TK_PPNUM) {
            vec_push(&pp->if_exp, t.loc);
            vec_push(&pp->if_exp, pp->tok_exp_loc);
        }
        tokbuf_push(pp, out, t);
    }
    /* everything above the barrier is exhausted and gone */
    while (pp->ctx.len > base + 1) {
        Context *x = &vec_last(&pp->ctx);
        tokbuf_release(pp, &x->owned);
        pp->ctx.len--;
    }
    pp->ctx.len = base;
    pp->carry_space = carry;
}

void pp_expand_into(PP *pp, TokSpan in, TokBuf *out)
{
    expand_into(pp, in, out, in.n ? in.t[0].loc : 0, NO_EXP, NO_EXP, false);
}

static bool needs_expansion(PP *pp, TokSpan s)
{
    uint32_t i;
    for (i = 0; i < s.n; i++)
        if (s.t[i].kind == TK_IDENT && !(s.t[i].flags & TF_NOEXPAND) &&
            pp_macro(pp, ident_by_id(pp->in, s.t[i].aux)))
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
        expand_into(pp, raw, &a->expanded[i], exp_loc, exp_id, root, true);
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
    /* libcpp: an odd run of trailing stray backslashes is dropped */
    for (i = arg.n; i > 0 && arg.t[i - 1].kind == TK_OTHER &&
                    pp_text(pp, &arg.t[i - 1])[0] == 0x5c; i--)
        ;
    if ((arg.n - i) & 1) {
        diag_report(pp->diag, DL_WARNING, "", pp->paste_loc,
                    "invalid string literal, ignoring final '\\'");
        sb->len--;
    }
    sb_putc(sb, '"');
    sl = srcmgr_scratch(pp->sm, &pp->scratch, sb->data, sb->len);
    memset(&r, 0, sizeof r);
    r.kind = TK_STRING;
    r.loc = pp->last_lex_end ? pp->last_lex_loc : hash->loc; /* libcpp: cur_token[-1] */
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
    sl = srcmgr_scratch(pp->sm, &pp->scratch, sb->data ? sb->data : "", sb->len);
    lexer_init_range(&L, pp->sm, pp->in, &pp->scratch, pp->opt->lex, sl, (uint32_t)sb->len);
    lex_next(&L, &r);
    valid = r.kind != TK_EOF && !(r.flags & TF_UNTERMINATED) && L.p == L.lim;
    if (valid && pp->opt->lex.norm < 3 && pp->diag &&
        (r.kind == TK_IDENT || r.kind == TK_PPNUM) && sb->len) {
        size_t k;
        for (k = 0; k < sb->len; k++)
            if ((unsigned char)sb->data[k] >= 0x80 || sb->data[k] == '\\') {
                /* libcpp warns at column 1 of the line being read */
                SrcFile *f = srcmgr_file_of(pp->sm, pp->paste_loc);
                uint32_t line, col;
                SrcLoc at = pp->paste_loc;
                if (f) {
                    srcmgr_linecol(f, pp->paste_loc, &line, &col);
                    at = pp->paste_loc - (col - 1);
                }
                lex_norm_check(&L, pp->diag, at, sb->data, sb->data + sb->len,
                               r.kind == TK_PPNUM);
                break;
            }
    }
    lexer_free(&L);
    if (!valid) {
        /* libcpp: at the lhs token (in the definition) with an expansion
         * note, or at the expansion point under -ftrack-macro-expansion=0 */
        bool t0 = pp->diag->track0;
        Diagnostic *d = diag_report(pp->diag, DL_ERROR, "", t0 ? site : lhs->loc,
            "pasting \"%.*s\" and \"%.*s\" does not give a valid preprocessing "
            "token", (int)lhs->len, pp_text(pp, lhs), (int)rhs->len,
            pp_text(pp, rhs));
        if (!t0) {
            diag_note(pp->diag, d, pp->paste_name_loc, "in expansion of macro '%s'",
                      m->name->str);
            pp_add_expansion_notes(pp, d);
        }
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
                } else {
                    push_arg(pp, out, arg_span(a, rp),
                             (uint16_t)(rt->flags & TF_SPACE));
                }
                continue;
            }
            if (m->funclike && tok_is_punct(rt, P_HASH) &&
                i + 1 < m->body_len && (m->body[i + 1].flags & TF_PARAM)) {
                /* x ## #y: '#' binds first; paste its string */
                single = stringize(pp, arg_span(a, m->body[i + 1].punct), rt,
                                   site, m);
                rhs.t = &single;
                rhs.n = 1;
                i++;
            } else if (rp >= 0) {
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

static bool builtin_query(PP *pp, Macro *m, const Tok *name, bool *result);

/* __has_attribute(id) and __has_builtin(id), as gcc's c_common_has_attribute
 * reads them: a lone identifier, each failure reported at the last token
 * read (an end of line is put back, so the token before it).  A nested
 * __has_*() is a number once expanded, so it is not an identifier. */
static void query_ident(PP *pp, Macro *m, const Tok *lp, bool *result)
{
    Tok t, nt;
    TokSrc src = pp_read_raw(pp, &t);
    Ident *id;
    const char *nm = m->name->str;
    if (t.kind == TK_EOF) {
        pp_unread(pp, &t, src);
        pp_error_at(pp, lp, "macro \"%s\" requires an identifier", nm);
        return;
    }
    if (t.kind != TK_IDENT) {
        pp_error_at(pp, &t, "macro \"%s\" requires an identifier", nm);
        return;
    }
    id = ident_by_id(pp->in, t.aux);
    {
        Macro *im = pp_macro(pp, id);
        if (im && im->builtin >= BUILTIN_HAS_INCLUDE &&
            !(t.flags & TF_NOEXPAND)) {
            bool dummy;
            builtin_query(pp, im, &t, &dummy);
            pp_error_at(pp, &t, "macro \"%s\" requires an identifier", nm);
            return;
        }
    }
    src = pp_read_raw(pp, &nt);
    if (!tok_is_punct(&nt, P_RPAREN)) {
        if (nt.kind == TK_EOF)
            pp_unread(pp, &nt, src);
        if (m->builtin == BUILTIN_HAS_BUILTIN) {
            pp_error_at(pp, nt.kind == TK_EOF ? &t : &nt,
                        "expected ')' after \"%s\"", id->str);
            while (nt.kind != TK_EOF && !tok_is_punct(&nt, P_RPAREN)) {
                src = pp_read_raw(pp, &nt);     /* gcc skips to the ')' */
                if (nt.kind == TK_EOF)
                    pp_unread(pp, &nt, src);
            }
        } else
            pp_error_at(pp, nt.kind == TK_EOF ? &t : &nt,
                        "missing ')' after \"%s\"", nm);
        return;
    }
    *result = name_in_table(m->builtin == BUILTIN_HAS_BUILTIN ? pp->host_builtins
                                                              : pp->host_attrs,
                            id->str, id->len);
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
        if (t.kind == TK_EOF) {
            pp_unread(pp, &t, src);
            pp_error_at(pp, name, "missing '(' after \"%s\"", m->name->str);
        } else {
            pp_error_at(pp, &t, "missing '(' after \"%s\"", m->name->str);
        }
        return false;
    }
    if (m->builtin == BUILTIN_HAS_ATTRIBUTE || m->builtin == BUILTIN_HAS_BUILTIN) {
        query_ident(pp, m, &t, result);
        return true;
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
            Ident *id = ident_by_id(pp->in, op.t[0].aux);
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

/* Only listeners read the counts (the index, -Wunused-macros); without
 * them, nothing share-writes a hot cache line per expansion. */
static void count_expansion(PP *pp, Macro *m)
{
    /* parallel workers: counted at the join, for kept slices only */
    if (pp->track != TRACK_NONE && pp->mode != PPM_PLAN)
        atomic_add_u32(&m->expansions, 1);
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
    e->seq = pp->versioned ? pp->version : pp->seq;
    e->seq_item = pp->version_item;
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

/* Push the pragmas hoisted out of an argument list (they are read next). */
static void push_pragmas(PP *pp, TokBuf *b)
{
    Context c;
    if (b->len == 0) {
        tokbuf_release(pp, b);
        return;
    }
    memset(&c, 0, sizeof c);
    c.owned = *b;
    c.toks = c.owned.t;
    c.end = c.owned.len;
    c.exp_loc = c.toks[0].loc;
    c.exp_id = c.root_id = NO_EXP;
    pp_push_context(pp, c);
    memset(b, 0, sizeof *b);
}

static Tok builtin_token(PP *pp, Macro *m, const Tok *name, SrcLoc exp_loc)
{
    char buf[64];
    const char *s = buf;
    TokKind k = TK_PPNUM;
    uint16_t fl = (uint16_t)((name->flags & (TF_SPACE | TF_BOL)) | TF_SYNTH);
    if ((m->builtin == BUILTIN_DATE || m->builtin == BUILTIN_TIME ||
         m->builtin == BUILTIN_TIMESTAMP) && diag_enabled(pp->diag, "date-time"))
        pp_warn_at(pp, name, "date-time", "macro \"%s\" might prevent "
                   "reproducible builds",
                   m->builtin == BUILTIN_DATE ? "__DATE__" :
                   m->builtin == BUILTIN_TIME ? "__TIME__" : "__TIMESTAMP__");
    switch (m->builtin) {
    case BUILTIN_LINE:
        if (pp->reads) /* the result depends on absolute line numbers */
            cell_reads_line(pp->reads, pp->diag->key);
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
    case BUILTIN_TIMESTAMP: {   /* the current file's modification time */
        struct stat st;
        time_t t;
        const char *a = "??? ??? ?? ??:??:?? ????";
        char tb[32];
        if (!stat(pp->inc->file->path, &st)) {
            t = st.st_mtime;
            if (ctime_r(&t, tb)) {
                tb[24] = 0;
                a = tb;
            }
        }
        snprintf(buf, sizeof buf, "\"%s\"", a);
        k = TK_STRING;
        break;
    }
    case BUILTIN_COUNTER:
        if (pp->mode == PPM_PLAN)
            pp->diverged = true; /* value depends on other segments */
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
        diag_report(pp->diag, DL_WARNING, "unknown-pragmas",
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

/* Pragmas whose effect is on preprocessor state (not just output). */
/* #pragma GCC dependency "file" [text]: warn if file is newer than the
 * current file. */
/* gcc lexes a _Pragma operand from its own buffer, and a diagnostic that
 * names a token there lands on the _Pragma's line at that token's column in
 * the destringized text. */
static SrcLoc scratch_col(PP *pp, SrcLoc at, SrcLoc base, SrcLoc where)
{
    SrcFile *f = srcmgr_file_of(pp->sm, at);
    uint32_t line, col;
    srcmgr_linecol(f, at, &line, &col);
    return srcmgr_loc_of(f, line, where - base + 1);
}

static void pragma_dependency(PP *pp, TokSpan toks, SrcLoc at)
{
    SrcFile *tf = srcmgr_file_of(pp->sm, toks.t[0].loc);
    bool sc = !tf || tf->kind == SF_SCRATCH; /* from _Pragma */
    const Tok *nt = toks.n > 2 ? &toks.t[2] : NULL;
    char *name;
    SrcFile *f;
    int di;
    struct stat a, b;
    if (!nt || nt->kind != TK_STRING || pp_text(pp, nt)[0] != '"') {
        const Tok *last = &toks.t[toks.n - 1];
        diag_report(pp->diag, DL_ERROR, "",
                    sc ? scratch_col(pp, at, toks.t[0].loc,
                                     nt ? nt->loc : last->loc + last->len)
                       : nt ? nt->loc : toks.t[1].loc,
                    "#pragma dependency expects \"FILENAME\" or <FILENAME>");
        return;
    }
    name = arena_strndup(pp->arena, pp_text(pp, nt) + 1, nt->len - 2);
    f = pp_find_include(pp, name, false, false, &di);
    if (!f) {
        diag_report(pp->diag, DL_ERROR, "", sc ? at : nt->loc, "%s: No such file or directory",
                    name);
        return;
    }
    if (stat(f->path, &a) == 0 && stat(pp->inc->file->path, &b) == 0 &&
        a.st_mtime > b.st_mtime)
        diag_report(pp->diag, DL_WARNING, "", sc ? at : nt->loc,
                    "current file is older than %s", name);
}

static bool state_pragma(PP *pp, TokSpan toks)
{
    return word(pp, toks, 0, "once") || word(pp, toks, 0, "push_macro") ||
           word(pp, toks, 0, "pop_macro") ||
           (word(pp, toks, 0, "GCC") &&
            (word(pp, toks, 1, "system_header") || word(pp, toks, 1, "poison")));
}

/* A _Pragma operand is lexed from scratch space: report (and record) such
 * a pragma at the _Pragma itself, like GCC; per-thread scratch offsets
 * would also make locations differ between runs. */
#define PLOC(t) (from_scratch ? loc : (t)->loc)

void pp_do_pragma(PP *pp, TokSpan toks, SrcLoc loc)
{
    SrcFile *tf = toks.n ? srcmgr_file_of(pp->sm, toks.t[0].loc) : NULL;
    bool from_scratch = !tf || tf->kind == SF_SCRATCH;
    bool emit = true;
    uint32_t i;
    if (pp->mode == PPM_PLAN && state_pragma(pp, toks)) {
        /* only the sequential engine may change state from text */
        pp->diverged = true;
        return;
    }
    PP_EMIT(pp, pragma, loc, toks);
    if (word(pp, toks, 0, "once")) {
        if (pp->inc->prev == NULL)
            diag_report(pp->diag, DL_WARNING, "", PLOC(&toks.t[0]),
                        "#pragma once in main file");
        pp->inc->file->pragma_once = true;
        emit = false;
    } else if (word(pp, toks, 0, "STDC") && word(pp, toks, 1, "FLOAT_CONST_DECIMAL64")) {
        /* c-pragma.c handle_pragma_float_const_decimal64 */
        if (pp->opt->pedantic)
            diag_report(pp->diag,
                        pp->diag->pedantic_errors ? DL_ERROR : DL_WARNING,
                        "pedantic", PLOC(&toks.t[0]),
                        "ISO C does not support '#pragma STDC FLOAT_CONST_DECIMAL64'");
        if (toks.n != 3 || !(word(pp, toks, 2, "ON") || word(pp, toks, 2, "OFF") ||
                             word(pp, toks, 2, "DEFAULT")))
            diag_report(pp->diag, DL_WARNING, "pragmas", PLOC(&toks.t[0]),
                        "malformed '#pragma STDC FLOAT_CONST_DECIMAL64', ignored");
    } else if (word(pp, toks, 0, "STDC")) {
        if (!(word(pp, toks, 1, "FP_CONTRACT") || word(pp, toks, 1, "FENV_ACCESS") ||
              word(pp, toks, 1, "CX_LIMITED_RANGE")) ||
            !(word(pp, toks, 2, "ON") || word(pp, toks, 2, "OFF") ||
              word(pp, toks, 2, "DEFAULT")) ||
            toks.n != 3)
            diag_report(pp->diag, DL_WARNING, "stdc-pragma", PLOC(&toks.t[0]),
                        "malformed or unknown STDC pragma (C99 6.10.6p2)");
    } else if (word(pp, toks, 0, "GCC")) {
        if (word(pp, toks, 1, "system_header")) {
            if (pp->inc->prev) {
                pp->inc->file->system_header = true;
                pp->inc->system = true;
            } else {
                diag_report(pp->diag, DL_WARNING, "",
                            from_scratch ? scratch_col(pp, loc, toks.t[0].loc,
                                                       toks.t[1].loc)
                                         : toks.t[1].loc,
                            "#pragma system_header ignored outside include "
                            "file");
            }
            emit = false;
        } else if (word(pp, toks, 1, "dependency")) {
            emit = false;
            pragma_dependency(pp, toks, loc);
        } else if (word(pp, toks, 1, "poison")) {
            emit = false;
            for (i = 2; i < toks.n; i++) {
                if (toks.t[i].kind != TK_IDENT) {
                    diag_report(pp->diag, DL_ERROR, "", PLOC(&toks.t[i]),
                                "invalid #pragma GCC poison directive");
                    break;
                }
                {
                    Ident *id = ident_by_id(pp->in, toks.t[i].aux);
                    MacroSlot *sl = mt_slot_w(pp->mt, id->id);
                    Macro *m = sl->cur;
                    PP_EMIT(pp, macro_ref, id, m, &toks.t[i], REF_PRAGMA);
                    if (sl->poison_seq)
                        continue;
                    /* like GCC: the definition is dropped; versioned, so
                     * text before the pragma is unaffected */
                    if (m) {
                        diag_report(pp->diag, DL_WARNING, "", PLOC(&toks.t[i]),
                                    "poisoning existing macro \"%s\"",
                                    id->str);
                        m->undef_loc = PLOC(&toks.t[0]);
                        m->undef_seq = pp->seq;
                        sl->cur = NULL;
                        PP_EMIT(pp, undef, id, m, loc, PLOC(&toks.t[i]));
                    }
                    sl->poison_seq = ++pp->seq;
                    pp->mt->npoison++;
                }
            }
            PP_EMIT(pp, checkpoint, PLOC(&toks.t[0]), pp->seq);
        } else if (word(pp, toks, 1, "warning") || word(pp, toks, 1, "error")) {
            bool err = word(pp, toks, 1, "error");
            uint32_t k = 2;
            const Tok *msg;
            emit = false;
            if (k < toks.n && tok_is_punct(&toks.t[k], P_LPAREN))
                k++;
            if (k < toks.n) {
                msg = &toks.t[k];
                diag_report(pp->diag, err ? DL_ERROR : DL_WARNING,
                            err ? "" : "pp-warning-directive", PLOC(msg),
                            "%.*s",
                            msg->kind == TK_STRING ? (int)msg->len - 2 : (int)msg->len,
                            pp_text(pp, msg) + (msg->kind == TK_STRING));
            }
        }
    } else if (word(pp, toks, 0, "push_macro")) {
        TokSpan rest;
        Ident *id;
        emit = false; /* consumed, like GCC */
        rest.t = toks.t + 1;
        rest.n = toks.n - 1;
        id = pragma_macro_name(pp, rest);
        if (id) {
            MacroStackEnt *e = NEW(pp->arena, MacroStackEnt);
            e->name = id;
            e->macro = mt_cur(pp->mt, id);
            e->next = pp->pushed;
            pp->pushed = e;
        }
    } else if (word(pp, toks, 0, "pop_macro")) {
        TokSpan rest;
        Ident *id;
        emit = false;
        rest.t = toks.t + 1;
        rest.n = toks.n - 1;
        id = pragma_macro_name(pp, rest);
        if (id) {
            MacroStackEnt **pe = &pp->pushed;
            while (*pe && (*pe)->name != id)
                pe = &(*pe)->next;
            if (*pe) {
                Macro *restored = (*pe)->macro, *cur = mt_cur(pp->mt, id);
                uint32_t ev = pp->seq++;
                if (cur) {
                    cur->undef_loc = PLOC(&toks.t[0]);
                    cur->undef_seq = ev;
                }
                if (restored) {
                    /* re-instate as a new version: versions never revive */
                    Macro *v = NEW(pp->arena, Macro);
                    *v = *restored;
                    v->alias_of = restored->alias_of ? restored->alias_of
                                                     : restored;
                    v->id = (uint32_t)pp->macros.len;
                    v->def_seq = ev;
                    v->undef_seq = UINT32_MAX;
                    v->undef_loc = 0;
                    v->expansions = v->cond_refs = 0;
                    v->user = NULL;
                    pp_install_macro(pp, v);
                    vec_push(&pp->macros, v);
                    restored = v;
                }
                mt_slot_w(pp->mt, id->id)->cur = restored;
                *pe = (*pe)->next;
                PP_EMIT(pp, checkpoint, PLOC(&toks.t[0]), pp->seq);
            }
        }
    } else if (word(pp, toks, 0, "message")) {
        /* handle_pragma_message: [(] string... [)] then junk; macros in the
         * operands are expanded */
        TokBuf ex;
        TokSpan rest;
        const Tok *et;
        uint32_t k = 0, en, nstr = 0;
        bool paren;
        StrBuf msg = {0};
        rest.t = toks.t + 1;
        rest.n = toks.n - 1;
        for (k = 0; k < rest.n; k++) {
            const Tok *t = &rest.t[k];
            if (t->kind == TK_OTHER && (t->flags & TF_UNTERMINATED)) {
                if (from_scratch)   /* directive lines warned in read_line */
                    pp_warn_at(pp, t, "",
                               "missing terminating \" character");
                pp_error_at(pp, t, "missing terminating \" character");
            }
        }
        k = 0;
        tokbuf_init(pp, &ex, rest.n + 8);
        pp_expand_into(pp, rest, &ex);
        et = ex.t;
        en = ex.len;
        paren = en && tok_is_punct(&et[0], P_LPAREN);
        if (paren)
            k++;
        while (k < en && et[k].kind == TK_STRING) {
            const Tok *t = &et[k++];
            nstr++;
            if (t->len >= 2)
                sb_putn(&msg, pp_text(pp, t) + 1, t->len - 2);
        }
        if (!nstr) {
            diag_report(pp->diag, DL_WARNING, "pragmas", PLOC(&toks.t[0]),
                        "expected a string after '#pragma message'");
        } else if (paren && !(k < en && tok_is_punct(&et[k], P_RPAREN))) {
            diag_report(pp->diag, DL_WARNING, "pragmas", PLOC(&toks.t[0]),
                        "malformed '#pragma message', ignored");
        } else {
            if (paren)
                k++;
            if (k < en)
                diag_report(pp->diag, DL_WARNING, "pragmas", PLOC(&et[k]),
                            "junk at end of '#pragma message'");
            if (msg.len)
                diag_report(pp->diag, DL_NOTE, "", PLOC(&toks.t[0]),
                            "#pragma message: %.*s", (int)msg.len, msg.data);
        }
        sb_free(&msg);
        tokbuf_release(pp, &ex);
    } else {
        /* cb_def_pragma: the first two tokens, at a line-only location */
        const char *s1 = toks.n ? pp_text(pp, &toks.t[0]) : "", *s2 = "";
        int n1 = toks.n ? (int)toks.t[0].len : 0, n2 = 0;
        if (toks.n > 1) {
            s2 = pp_text(pp, &toks.t[1]);
            n2 = (int)toks.t[1].len;
        }
        pp->diag->nocol_next = true;
        diag_report(pp->diag, DL_WARNING, "unknown-pragmas",
                    toks.n ? PLOC(&toks.t[0]) : loc,
                    "ignoring '#pragma %.*s %.*s'", n1, s1, n2, s2);
    }
    if (emit) {
        StrBuf *sb = &pp->sb;
        sb->len = 0;
        sb_puts(sb, "pragma");
        for (i = 0; i < toks.n; i++) {
            if (i == 0 || (toks.t[i].flags & (TF_SPACE | TF_BOL)))
                sb_putc(sb, ' ');
            sb_putn(sb, pp_text(pp, &toks.t[i]), toks.t[i].len);
        }
        pp_emit_line(pp, sb->data, sb->len, loc);
    }
#undef PLOC
}

/* A directive line for the output (#pragma, #ident): text after the '#'. */
void pp_emit_line(PP *pp, const char *text, size_t len, SrcLoc loc)
{
    Tok t;
    memset(&t, 0, sizeof t);
    t.kind = TK_PRAGMA;
    t.loc = loc;
    t.len = (uint32_t)len;
    t.aux = srcmgr_scratch(pp->sm, &pp->scratch, text, len);
    t.flags = TF_SPELL | TF_BOL;
    if (pp->mode == PPM_PHASE_A) {
        PlanItem it;
        memset(&it, 0, sizeof it);
        it.kind = PI_PRAGMA;
        it.version = pp->seq;
        it.counter = pp->counter;
        it.frame = pp->pframe;
        it.begin = t.loc;
        it.end = (SrcLoc)pp->plan->pragmas.len;
        vec_push(&pp->plan->pragmas, t);
        vec_push(&pp->plan->items, it);
    } else {
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
    sl = srcmgr_scratch(pp->sm, &pp->scratch, sb->data ? sb->data : "", sb->len);
    lexer_init_range(&L, pp->sm, pp->in, &pp->scratch, pp->opt->lex, sl, (uint32_t)sb->len);
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
/* Read the next token of a _Pragma operand as GCC does: macro-expanded,
 * directives processed, continuing past the end of an included file. */
static TokSrc read_operand(PP *pp, Tok *t, TokBuf *hoist)
{
    for (;;) {
        TokSrc src = pp_read_raw(pp, t);
        if (src == SRC_LEXER && t->kind == TK_EOF && !pp->in_directive) {
            if (pp_cross_file_end(pp))
                continue;
            return src;
        }
        if (t->kind == TK_IDENT && !(t->flags & TF_NOEXPAND)) {
            Macro *m = pp_macro(pp, ident_by_id(pp->in, t->aux));
            if (m && pp_macro_disabled(pp, m))
                t->flags |= TF_NOEXPAND;
            else if (m && pp_try_expand(pp, t, src))
                continue;
        }
        if (src == SRC_LEXER && !pp->in_directive) {
            if (t->kind == TK_DIRMARK) {
                pp_plan_apply_dir(pp, t->aux);
                continue;
            }
            if ((t->flags & TF_BOL) && tok_is_punct(t, P_HASH)) {
                pp_directive(pp, t);
                continue;
            }
        }
        if (t->kind == TK_PRAGMA) { /* a #pragma line: printed first */
            tokbuf_push(pp, hoist, *t);
            continue;
        }
        return src;
    }
}

static bool pragma_op_error(PP *pp, Tok *name, const Tok *prev, Tok *bad,
                            TokSrc src, TokBuf *hoist)
{
    if (bad->kind == TK_EOF)
        pp_unread(pp, bad, src);
    pp_error_at(pp, bad->kind == TK_EOF ? prev : bad,
                "_Pragma takes a parenthesized string literal");
    if (hoist->len == 0) {
        tokbuf_release(pp, hoist);
        return false;
    }
    name->flags |= TF_NOEXPAND; /* printed after the hoisted lines */
    push_single(pp, *name, NULL, pp->tok_exp_loc, NO_EXP, NO_EXP, name->loc);
    push_pragmas(pp, hoist);
    return true;
}

static bool do_pragma_op(PP *pp, Tok *name)
{
    TokBuf hoist = {0};
    Tok lp, s, rp;
    TokSrc a, b, c;
    TokBuf toks;
    TokSpan span;
    if (pp->in_directive)
        return false; /* GCC: a plain identifier in any directive */
    if (pp->collecting_args)
        return false; /* deferred until the result is rescanned */
    /* GCC: whatever was read is consumed (EOF excepted), the name is
     * printed as a plain identifier */
    a = read_operand(pp, &lp, &hoist);
    if (!tok_is_punct(&lp, P_LPAREN))
        return pragma_op_error(pp, name, name, &lp, a, &hoist);
    b = read_operand(pp, &s, &hoist);
    if (s.kind != TK_STRING)
        return pragma_op_error(pp, name, &lp, &s, b, &hoist);
    c = read_operand(pp, &rp, &hoist);
    if (!tok_is_punct(&rp, P_RPAREN))
        return pragma_op_error(pp, name, &s, &rp, c, &hoist);
    toks = destringize(pp, &s);
    span.t = toks.t;
    span.n = toks.len;
    pp_do_pragma(pp, span, name->loc);
    tokbuf_release(pp, &toks);
    push_pragmas(pp, &hoist);
    return true;
}

/* ---- entry point ---------------------------------------------------- */

bool pp_try_expand(PP *pp, Tok *name, TokSrc src)
{
    Ident *id = ident_by_id(pp->in, name->aux);
    Macro *m = pp_macro(pp, id);
    uint32_t parent = src == SRC_CONTEXT ? pp->tok_exp_id : NO_EXP;
    uint32_t parent_root = pp->tok_root;
    SrcLoc exp_loc = pp->tok_exp_loc;
    SrcLoc site = exp_loc;
    uint16_t lead = (uint16_t)(name->flags & (TF_SPACE | TF_BOL));
    Expansion *e;
    uint32_t eid, root;
    Context c;
    TokBuf pragmas = {0};
    uint32_t vseq, vitem;
    bool root_obj = src == SRC_LEXER ? !m->funclike : pp->tok_root_obj;
    bool saved_root_obj = pp->subst_root_obj;

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
            count_expansion(pp, m);
            PP_EMIT(pp, expand, e, NULL, 0);
            push_single(pp, pp_make_token(pp, TK_PPNUM, r ? "1" : "0", 1,
                                          name->loc, (uint16_t)(lead | TF_SYNTH)),
                        NULL, exp_loc, e ? e->id : NO_EXP,
                        e ? e->root : NO_EXP, name->loc);
            return true;
        }
        e = new_expansion(pp, m, name, parent, parent_root);
        count_expansion(pp, m);
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
        count_expansion(pp, m);
        PP_EMIT(pp, expand, e, NULL, 0);
        if (m->body_len == 0) {
            if (lead & TF_SPACE)
                pp->carry_space = true;
            return true;
        }
        pp->subst_root_obj = root_obj;
        pp->paste_loc = name->loc + name->len;
        pp->paste_name_loc = name->loc;
        subst(pp, m, NULL, lead, site, exp_loc, eid, root, &c.owned);
        pp->subst_root_obj = saved_root_obj;
    } else {
        Tok lp;
        TokSrc ls = pp_read_raw(pp, &lp);
        Args a;
        TokSpan *spans;
        int i;
        if (!tok_is_punct(&lp, P_LPAREN)) {
            pp_unread(pp, &lp, ls);
            if (!pp->collecting_args && !(m->file && m->file->system_header) &&
                diag_enabled(pp->diag, "traditional"))
                diag_report(pp->diag, DL_WARNING, "traditional", name->loc,
                            "function-like macro \"%s\" must be used with "
                            "arguments in traditional C", id->str);
            return false;
        }
        vseq = pp->versioned ? pp->version : pp->seq; /* at the name */
        vitem = pp->version_item;
        if (!collect_args(pp, m, name, &lp, &a)) {
            if (a.pragmas.len == 0) {
                tokbuf_release(pp, &a.pragmas);
                return false;
            }
            /* the name follows the hoisted pragmas */
            name->flags |= TF_NOEXPAND;
            push_single(pp, *name, NULL, exp_loc, parent, parent_root,
                        name->loc);
            push_pragmas(pp, &a.pragmas);
            return true;
        }
        pragmas = a.pragmas;
        e = new_expansion(pp, m, name, parent, parent_root);
        eid = e ? e->id : NO_EXP;
        root = e ? e->root : NO_EXP;
        if (e) {
            e->end_loc = a.rparen_loc + a.rparen_len;
            e->seq = vseq;
            e->seq_item = vitem;
        }
        count_expansion(pp, m);
        if (pp->track != TRACK_NONE) {
            spans = xmalloc(sizeof(TokSpan) * ((size_t)m->nparams + 1));
            for (i = 0; i < m->nparams; i++)
                spans[i] = arg_span(&a, i);
            PP_EMIT(pp, expand, e, spans, m->nparams);
            free(spans);
        }
        pp->subst_root_obj = root_obj;
        pp->paste_loc = a.rparen_loc;
        pp->paste_name_loc = name->loc;
        subst(pp, m, &a, lead, site, exp_loc, eid, root, &c.owned);
        pp->subst_root_obj = saved_root_obj;
        args_free(pp, &a, m->nparams);
    }
    if (c.owned.len == 0) {
        tokbuf_release(pp, &c.owned);
        if (lead & TF_SPACE)
            pp->carry_space = true;
        push_pragmas(pp, &pragmas);
        return true;
    }
    c.toks = c.owned.t;
    c.end = c.owned.len;
    c.macro = m;
    c.exp_id = eid;
    c.root_id = root;
    c.exp_loc = exp_loc;
    c.name_loc = name->loc;
    c.root_obj = root_obj;
    pp_push_context(pp, c);
    push_pragmas(pp, &pragmas);
    return true;
}
