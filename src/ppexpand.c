/* ppexpand.c - macro replacement (C99 6.10.3), _Pragma (6.10.9), builtin
 * macros (6.10.8) and #pragma handling (6.10.6).
 *
 * Expansion is Prosser's hide-set algorithm on linked token lists: the
 * replacement is spliced in front of the remaining input and rescanned
 * together with it. */
#include "pp.h"

#include <string.h>

typedef struct Arg {
    Token *raw;       /* unexpanded, EOF-terminated */
    Token *expanded;  /* lazily computed full expansion */
    bool present;     /* false for an omitted variadic argument */
} Arg;

static Expansion *new_expansion(PP *pp, Macro *m, Token *name)
{
    Expansion *e = NEW(pp->arena, Expansion);
    e->id = pp->next_exp_id++;
    e->macro = m;
    e->name_loc = name->loc;
    e->name_prov = name->prov;
    e->end_loc = name->loc + name->rawlen;
    e->end_prov = name->prov;
    e->in_directive = pp->in_directive;
    vec_push(&pp->expansions, e);
    return e;
}

static Prov *new_prov(PP *pp, ProvKind k, Expansion *e, SrcLoc loc,
                      Prov *inner, int param)
{
    Prov *p = NEW(pp->arena, Prov);
    p->kind = k;
    p->exp = e;
    p->loc = loc;
    p->inner = inner;
    p->param = param;
    return p;
}

static int param_of(const Macro *m, const Token *t)
{
    int i;
    if (t->kind != TK_IDENT || !m->funclike)
        return -1;
    for (i = 0; i < m->nparams; i++)
        if (m->params[i] == t->ident)
            return i;
    return -1;
}

/* ---- argument collection ------------------------------------------- */

/* Reads the arguments of a function-like macro; pp->cur is at '('.
 * Returns false (with nothing consumed) on error. */
static bool collect_args(PP *pp, Macro *m, Token *name, Arg **args_out,
                         int *nargs_out, Token **rparen_out)
{
    VEC(Arg) args = {0};
    Token head, *tail = &head;
    Token *saved = pp->cur;
    int depth = 0;
    bool in_sub = pp->in_directive || pp->collecting_args;
    bool prev_collecting = pp->collecting_args;
    Arg a;

    pp->cur = pp->cur->next; /* '(' */
    head.next = NULL;
    for (;;) {
        Token *t = pp->cur;
        if (t->kind == TK_EOF) {
            Diagnostic *d = pp_error_at(pp, name,
                "unterminated argument list invoking macro \"%s\"",
                m->name->str);
            (void)d;
            vec_free(&args);
            if (saved)
                pp->cur = saved;
            return false;
        }
        if (!t->prov && (t->flags & TF_BOL) && tok_is_punct(t, P_HASH) &&
            !in_sub) {
            /* C99 6.10.3p11: undefined; GCC and Clang process it */
            Token *kw = t->next;
            if (!(kw->flags & TF_BOL) && kw->kind != TK_EOF) {
                diag_report(pp->diag, DL_WARNING, "directive-in-macro-args",
                            kw->loc,
                            "preprocessing directive inside the arguments of "
                            "macro \"%s\" is undefined behavior",
                            m->name->str);
            }
            pp->cur = t->next;
            pp->collecting_args = true;
            pp_directive(pp, t);
            pp->collecting_args = prev_collecting;
            saved = NULL; /* cannot restore past a directive */
            continue;
        }
        pp->cur = t->next;
        if (tok_is_punct(t, P_LPAREN)) {
            depth++;
        } else if (tok_is_punct(t, P_RPAREN)) {
            if (depth == 0) {
                tail->next = pp_new_eof(pp, t->loc);
                a.raw = head.next;
                a.expanded = NULL;
                a.present = true;
                vec_push(&args, a);
                *rparen_out = t;
                break;
            }
            depth--;
        } else if (tok_is_punct(t, P_COMMA) && depth == 0 &&
                   !(m->variadic && (int)args.len == m->nparams - 1)) {
            tail->next = pp_new_eof(pp, t->loc);
            a.raw = head.next;
            a.expanded = NULL;
            a.present = true;
            vec_push(&args, a);
            head.next = NULL;
            tail = &head;
            continue;
        }
        tail = tail->next = pp_copy_token(pp, t);
        tail->flags &= (uint16_t)~TF_BOL;
        if (t->flags & TF_BOL)
            tail->flags |= TF_SPACE;
    }

    /* F() for a zero-parameter macro has no arguments */
    if (m->nparams == 0 && args.len == 1 && args.data[0].raw->kind == TK_EOF)
        args.len = 0;

    if (m->variadic && (int)args.len == m->nparams - 1) {
        if (pp->opt->pedantic && !m->gnu_named_variadic)
            diag_report(pp->diag, DL_WARNING, "pedantic", name->loc,
                        "ISO C99 requires at least one argument for the "
                        "\"...\" in a variadic macro");
        a.raw = pp_new_eof(pp, (*rparen_out)->loc);
        a.expanded = NULL;
        a.present = false;
        vec_push(&args, a);
    }
    if ((int)args.len != m->nparams) {
        if ((int)args.len > m->nparams)
            pp_error_at(pp, name,
                        "macro \"%s\" passed %d arguments, but takes just %d",
                        m->name->str, (int)args.len, m->nparams);
        else
            pp_error_at(pp, name,
                        "macro \"%s\" requires %d arguments, but only %d given",
                        m->name->str, m->nparams, (int)args.len);
        vec_free(&args);
        if (saved)
            pp->cur = saved;
        return false;
    }
    *args_out = NEW_ARRAY(pp->arena, Arg, args.len ? args.len : 1);
    if (args.len)
        memcpy(*args_out, args.data, sizeof(Arg) * args.len);
    *nargs_out = (int)args.len;
    vec_free(&args);
    return true;
}

/* ---- # and ## ------------------------------------------------------- */

static Token *stringize(PP *pp, Arg *arg, const Token *hash, Expansion *e)
{
    StrBuf sb = {0};
    Token *t, *r;
    Token *list;
    sb_putc(&sb, '"');
    for (t = arg->raw; t->kind != TK_EOF; t = t->next) {
        uint32_t i;
        if (t != arg->raw && (t->flags & (TF_SPACE | TF_BOL)))
            sb_putc(&sb, ' ');
        if (t->kind == TK_STRING || t->kind == TK_CHAR ||
            (t->kind == TK_OTHER && (t->flags & TF_UNTERMINATED))) {
            for (i = 0; i < t->len; i++) {
                if (t->text[i] == '"' || t->text[i] == '\\')
                    sb_putc(&sb, '\\');
                sb_putc(&sb, t->text[i]);
            }
        } else {
            sb_putn(&sb, t->text, t->len);
        }
    }
    sb_putc(&sb, '"');
    list = lex_buffer(pp->arena, pp->in, pp->opt->lex, sb.data, sb.len,
                      hash->loc);
    if (list->kind != TK_STRING || list->next->kind != TK_EOF) {
        Diagnostic *d = diag_report(pp->diag, DL_WARNING, "",
            prov_expansion_loc(e->name_prov, e->name_loc),
            "invalid string literal produced by '#' (C99 6.10.3.2p2 "
            "undefined behavior)");
        diag_note(pp->diag, d, hash->loc, "'#' in the definition of '%s'",
                  e->macro->name->str);
        r = NEW(pp->arena, Token);
        r->kind = TK_STRING;
        r->text = arena_strndup(pp->arena, sb.data, sb.len);
        r->len = (uint32_t)sb.len;
        r->loc = hash->loc;
    } else {
        r = list;
        r->next = NULL;
    }
    sb_free(&sb);
    r->prov = new_prov(pp, PROV_STRINGIZE, e, hash->loc, NULL, -1);
    r->rawlen = hash->rawlen;
    return r;
}

/* Paste lhs ## rhs.  Returns the new token, or NULL (with both tokens kept
 * separately by the caller) if the result is not a single valid token. */
static Token *paste(PP *pp, Token *lhs, Token *rhs, Token *op, Expansion *e)
{
    StrBuf sb = {0};
    Token *list, *r;
    bool valid;
    sb_putn(&sb, lhs->text, lhs->len);
    sb_putn(&sb, rhs->text, rhs->len);
    list = lex_buffer(pp->arena, pp->in, pp->opt->lex, sb.data ? sb.data : "",
                      sb.len, lhs->loc);
    valid = list->kind != TK_EOF && list->next->kind == TK_EOF &&
            !(list->flags & TF_UNTERMINATED);
    if (!valid) {
        /* report at the call site; the definition is a note */
        Diagnostic *d = diag_report(pp->diag, DL_ERROR, "",
            prov_expansion_loc(e->name_prov, e->name_loc),
            "pasting \"%.*s\" and \"%.*s\" does not give a valid preprocessing "
            "token (C99 6.10.3.3p3 undefined behavior)", (int)lhs->len,
            lhs->text, (int)rhs->len, rhs->text);
        diag_note(pp->diag, d, op->loc, "'##' in the definition of '%s'",
                  e->macro->name->str);
        PP_EMIT(pp, paste, e, lhs, rhs, NULL, false);
        sb_free(&sb);
        return NULL;
    }
    sb_free(&sb);
    r = list;
    r->next = NULL;
    r->flags = (uint16_t)(lhs->flags & (TF_SPACE | TF_BOL));
    r->loc = lhs->loc;
    r->rawlen = lhs->rawlen;
    r->hs = lhs->hs;
    r->prov = new_prov(pp, PROV_PASTE, e, op->loc, lhs->prov, -1);
    PP_EMIT(pp, paste, e, lhs, rhs, r, true);
    return r;
}

/* ---- substitution --------------------------------------------------- */

static Token *expand_arg(PP *pp, Arg *a)
{
    if (!a->expanded) {
        bool saved = pp->collecting_args;
        pp->collecting_args = true;
        a->expanded = pp_expand_list(pp, a->raw);
        pp->collecting_args = saved;
    }
    return a->expanded;
}

typedef struct OutList {
    Token head;
    Token *tail;
} OutList;

static void out_init(OutList *o)
{
    memset(&o->head, 0, sizeof o->head);
    o->tail = &o->head;
}

static void out_push(OutList *o, Token *t)
{
    t->next = NULL;
    o->tail->next = t;
    o->tail = t;
}

static Token *placemarker(PP *pp, SrcLoc loc)
{
    Token *t = NEW(pp->arena, Token);
    t->kind = TK_PLACEMARKER;
    t->text = "";
    t->loc = loc;
    return t;
}

/* Copies argument tokens with ARG provenance. */
static void push_arg_tokens(PP *pp, OutList *o, Token *list, Expansion *e,
                            int param, SrcLoc ploc, uint16_t first_flags,
                            bool mark_first)
{
    Token *t;
    bool first = true;
    if (list->kind == TK_EOF) {
        Token *pm = placemarker(pp, ploc);
        pm->flags = first_flags;
        out_push(o, pm);
        return;
    }
    for (t = list; t->kind != TK_EOF; t = t->next) {
        Token *c = pp_copy_token(pp, t);
        c->prov = new_prov(pp, PROV_ARG, e, ploc, t->prov, param);
        if (first && mark_first)
            c->flags = (uint16_t)((c->flags & ~(TF_SPACE | TF_BOL)) |
                                  first_flags);
        first = false;
        out_push(o, c);
    }
}

static Token *subst(PP *pp, Macro *m, Arg *args, Expansion *e,
                    Hideset *hs, uint16_t lead_flags)
{
    OutList o;
    Token *t, *prev_body = NULL;
    out_init(&o);

    for (t = m->body; t->kind != TK_EOF; prev_body = t, t = t->next) {
        int pi;
        uint16_t fl = (uint16_t)(t == m->body ? lead_flags
                                              : (t->flags & TF_SPACE));

        /* # param */
        if (m->funclike && tok_is_punct(t, P_HASH) &&
            (pi = param_of(m, t->next)) >= 0) {
            Token *s = stringize(pp, &args[pi], t, e);
            s->flags = fl;
            out_push(&o, s);
            t = t->next;
            prev_body = t;
            continue;
        }

        /* lhs ## rhs */
        if (tok_is_punct(t, P_HASHHASH)) {
            Token *op = t, *rhs_list, *lhs = o.tail;
            Token *rest = NULL;
            int rpi;
            t = t->next;
            prev_body = t;
            rpi = param_of(m, t);
            /* GNU: , ## __VA_ARGS__ drops the comma when the variable
             * arguments are omitted entirely (GCC keeps it for F(a,)) */
            if (rpi >= 0 && m->variadic && rpi == m->nparams - 1 &&
                lhs != &o.head && tok_is_punct(lhs, P_COMMA) &&
                pp->opt->gnu_extensions) {
                if (!args[rpi].present) {
                    /* remove the comma */
                    Token *p = &o.head;
                    while (p->next != lhs)
                        p = p->next;
                    p->next = NULL;
                    o.tail = p;
                    if (pp->opt->pedantic)
                        diag_report(pp->diag, DL_WARNING, "pedantic", op->loc,
                                    "token pasting of ',' and __VA_ARGS__ is "
                                    "a GNU extension");
                } else {
                    push_arg_tokens(pp, &o, args[rpi].raw, e, rpi, t->loc,
                                    t->flags & TF_SPACE, true);
                }
                continue;
            }
            if (rpi >= 0) {
                rhs_list = args[rpi].raw;
            } else {
                Token *c = pp_copy_token(pp, t);
                c->prov = new_prov(pp, PROV_BODY, e, t->loc, NULL, -1);
                c->next = pp_new_eof(pp, t->loc);
                rhs_list = c;
            }
            if (rhs_list->kind == TK_EOF) {
                /* x ## <empty>: lhs unchanged */
                continue;
            }
            rest = rhs_list->next;
            if (lhs == &o.head || lhs->kind == TK_PLACEMARKER) {
                /* <empty> ## y */
                Token *c = pp_copy_token(pp, rhs_list);
                if (rpi >= 0)
                    c->prov = new_prov(pp, PROV_ARG, e, t->loc,
                                       rhs_list->prov, rpi);
                if (lhs != &o.head) {
                    c->flags = lhs->flags;
                    lhs->kind = TK_PLACEMARKER; /* keep, removed later */
                }
                out_push(&o, c);
            } else {
                Token *r = paste(pp, lhs, rhs_list, op, e);
                if (r) {
                    /* replace lhs with r */
                    Token *p = &o.head;
                    while (p->next != lhs)
                        p = p->next;
                    p->next = r;
                    o.tail = r;
                } else {
                    Token *c = pp_copy_token(pp, rhs_list);
                    if (rpi >= 0)
                        c->prov = new_prov(pp, PROV_ARG, e, t->loc,
                                           rhs_list->prov, rpi);
                    out_push(&o, c);
                }
            }
            if (rpi >= 0 && rest->kind != TK_EOF) {
                /* remaining argument tokens follow unchanged */
                Token *u;
                for (u = rest; u->kind != TK_EOF; u = u->next) {
                    Token *c = pp_copy_token(pp, u);
                    c->prov = new_prov(pp, PROV_ARG, e, t->loc, u->prov, rpi);
                    out_push(&o, c);
                }
            }
            continue;
        }

        pi = param_of(m, t);
        if (pi >= 0) {
            bool before_paste = tok_is_punct(t->next, P_HASHHASH);
            Token *list = before_paste ? args[pi].raw : expand_arg(pp, &args[pi]);
            push_arg_tokens(pp, &o, list, e, pi, t->loc, fl, true);
            continue;
        }

        {
            Token *c = pp_copy_token(pp, t);
            c->flags = (uint16_t)((c->flags & ~(TF_SPACE | TF_BOL)) | fl);
            c->prov = new_prov(pp, PROV_BODY, e, t->loc, NULL, -1);
            out_push(&o, c);
        }
    }
    (void)prev_body;

    /* drop placemarkers, apply hide set, keep leading space of the name */
    {
        Token head, *tail = &head, *u;
        bool first = true;
        uint16_t pending_space = 0;
        head.next = NULL;
        for (u = o.head.next; u; u = u->next) {
            if (u->kind == TK_PLACEMARKER) {
                if (first)
                    pending_space |= u->flags & TF_SPACE;
                continue;
            }
            u->hs = hs_union(pp, u->hs, hs);
            if (first) {
                u->flags = (uint16_t)((u->flags & ~(TF_SPACE | TF_BOL)) |
                                      lead_flags | pending_space);
                first = false;
            }
            tail->next = u;
            tail = u;
        }
        tail->next = NULL;
        return head.next;
    }
}

/* Splice `list` in front of pp->cur. */
static void push_front(PP *pp, Token *list)
{
    Token *t;
    if (!list)
        return;
    for (t = list; t->next; t = t->next)
        ;
    t->next = pp->cur;
    pp->cur = list;
}

/* ---- builtins ------------------------------------------------------- */

static Token *make_token(PP *pp, TokKind k, const char *text, Token *at,
                         Expansion *e)
{
    Token *t = NEW(pp->arena, Token);
    t->kind = (uint8_t)k;
    t->text = arena_strdup(pp->arena, text);
    t->len = (uint32_t)strlen(text);
    t->loc = at->loc;
    t->rawlen = at->rawlen;
    t->flags = (uint16_t)(at->flags & (TF_SPACE | TF_BOL));
    t->prov = new_prov(pp, PROV_BUILTIN, e, at->loc, NULL, -1);
    if (k == TK_IDENT)
        t->ident = intern(pp->in, t->text, t->len);
    return t;
}

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

static bool name_in_table(const char *const *table, const Token *t)
{
    const char *s = t->text;
    size_t n = t->len;
    int i;
    if (!table)
        return false;
    /* __attr__ and attr are equivalent */
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

/* __has_include(...) etc.  pp->cur is just after the name. */
static bool builtin_query(PP *pp, Macro *m, Token *name, bool *result)
{
    Token *lp = pp->cur, *t, *arg_first, *rp;
    int depth = 0;
    if (!tok_is_punct(lp, P_LPAREN)) {
        pp_error_at(pp, name, "missing '(' after \"%s\"", m->name->str);
        return false;
    }
    arg_first = lp->next;
    for (t = arg_first; t->kind != TK_EOF; t = t->next) {
        if (tok_is_punct(t, P_LPAREN))
            depth++;
        else if (tok_is_punct(t, P_RPAREN) && depth-- == 0)
            break;
    }
    if (t->kind == TK_EOF) {
        pp_error_at(pp, name, "missing ')' after \"%s\"", m->name->str);
        pp->cur = t;
        return false;
    }
    rp = t;
    pp->cur = rp->next;
    *result = false;
    switch (m->builtin) {
    case BUILTIN_HAS_INCLUDE:
    case BUILTIN_HAS_INCLUDE_NEXT: {
        Token *first = arg_first;
        char *hname = NULL;
        bool angled = false;
        int di;
        if (!pp->in_if_expr)
            pp_error_at(pp, name, "\"%s\" used outside of preprocessing "
                                  "directive", m->name->str);
        if (first->kind == TK_STRING && first->text[0] == '"') {
            hname = arena_strndup(pp->arena, first->text + 1, first->len - 2);
        } else if (tok_is_punct(first, P_LT)) {
            StrBuf sb = {0};
            Token *u;
            angled = true;
            for (u = first->next; u != rp && !tok_is_punct(u, P_GT); u = u->next) {
                if (u != first->next && (u->flags & TF_SPACE))
                    sb_putc(&sb, ' ');
                sb_putn(&sb, u->text, u->len);
            }
            hname = arena_strndup(pp->arena, sb_cstr(&sb), sb.len);
            sb_free(&sb);
        } else {
            pp_error_at(pp, first, "operator \"%s\" requires a header-name",
                        m->name->str);
            return false;
        }
        *result = pp_search_include(pp, hname, angled,
                                    m->builtin == BUILTIN_HAS_INCLUDE_NEXT &&
                                        pp->inc->prev != NULL,
                                    &di) != NULL;
        return true;
    }
    default:
        if (arg_first->kind != TK_IDENT || arg_first->next != rp) {
            /* scoped attribute gnu::x or similar: not supported -> 0 */
            *result = false;
            return true;
        }
        if (m->builtin == BUILTIN_HAS_ATTRIBUTE ||
            m->builtin == BUILTIN_HAS_C_ATTRIBUTE)
            *result = name_in_table(pp->host_attrs, arg_first);
        else if (m->builtin == BUILTIN_HAS_BUILTIN)
            *result = name_in_table(pp->host_builtins, arg_first);
        return true;
    }
}

Token *pp_builtin_expand(PP *pp, Macro *m, Token *tok, Expansion *e)
{
    char buf[64];
    switch (m->builtin) {
    case BUILTIN_LINE:
        sprintf(buf, "%u", pp_presumed_line(pp, pp_expansion_loc(tok)));
        return make_token(pp, TK_PPNUM, buf, tok, e);
    case BUILTIN_FILE:
        return make_token(pp, TK_STRING, quote_string(pp, pp->inc->presumed_name),
                          tok, e);
    case BUILTIN_BASE_FILE:
        return make_token(pp, TK_STRING, quote_string(pp, pp->main_file->name),
                          tok, e);
    case BUILTIN_DATE:
        return make_token(pp, TK_STRING, pp->opt->date_str, tok, e);
    case BUILTIN_TIME:
        return make_token(pp, TK_STRING, pp->opt->time_str, tok, e);
    case BUILTIN_COUNTER:
        sprintf(buf, "%u", pp->counter++);
        return make_token(pp, TK_PPNUM, buf, tok, e);
    case BUILTIN_INCLUDE_LEVEL:
        sprintf(buf, "%d", pp->include_depth > 0 ? pp->include_depth - 1 : 0);
        return make_token(pp, TK_PPNUM, buf, tok, e);
    default:
        return NULL;
    }
}

/* ---- _Pragma / #pragma ---------------------------------------------- */

Token *pp_destringize_pragma(PP *pp, Token *str)
{
    /* C99 6.10.9: delete L prefix, quotes; \" -> ", \\ -> \ */
    StrBuf sb = {0};
    const char *s = str->text;
    uint32_t n = str->len, i;
    Token *list, *t;
    if (s[0] == 'L') {
        s++;
        n--;
    }
    for (i = 1; i + 1 < n; i++) {
        if (s[i] == '\\' && (s[i + 1] == '"' || s[i + 1] == '\\') && i + 2 < n)
            i++;
        sb_putc(&sb, s[i]);
    }
    list = lex_buffer(pp->arena, pp->in, pp->opt->lex, sb.data ? sb.data : "",
                      sb.len, str->loc);
    sb_free(&sb);
    for (t = list; t; t = t->next) {
        t->flags &= (uint16_t)~TF_BOL;
        t->prov = str->prov;
    }
    return list;
}

static char *pragma_text(PP *pp, Token *toks)
{
    StrBuf sb = {0};
    char *r;
    Token *t;
    sb_puts(&sb, "pragma");
    for (t = toks; t->kind != TK_EOF; t = t->next) {
        sb_putc(&sb, ' ');
        sb_putn(&sb, t->text, t->len);
    }
    r = arena_strndup(pp->arena, sb.data, sb.len);
    sb_free(&sb);
    return r;
}

static void push_pragma_token(PP *pp, Token *toks, SrcLoc loc)
{
    Token *t = NEW(pp->arena, Token);
    t->kind = TK_PRAGMA;
    t->text = pragma_text(pp, toks);
    t->len = (uint32_t)strlen(t->text);
    t->loc = loc;
    t->flags = TF_BOL;
    /* give it a provenance so it is never mistaken for a directive */
    t->prov = NULL;
    t->next = pp->cur;
    pp->cur = t;
}

static Ident *pragma_macro_name(PP *pp, Token *t)
{
    /* ( "NAME" ) */
    if (!tok_is_punct(t, P_LPAREN) || t->next->kind != TK_STRING ||
        !tok_is_punct(t->next->next, P_RPAREN)) {
        diag_report(pp->diag, DL_WARNING, "unknown-pragma", t->loc,
                    "expected (\"name\") in push_macro/pop_macro pragma");
        return NULL;
    }
    return intern(pp->in, t->next->text + 1, t->next->len - 2);
}

void pp_do_pragma(PP *pp, Token *toks, SrcLoc loc)
{
    Token *t = toks;
    bool emit = true;
    PP_EMIT(pp, pragma, loc, toks);
    if (tok_is_ident(t, "once")) {
        if (pp->inc->prev == NULL)
            diag_report(pp->diag, DL_WARNING, "", t->loc,
                        "#pragma once in main file");
        pp->inc->file->pragma_once = true;
        emit = false;
    } else if (tok_is_ident(t, "STDC")) {
        Token *w = t->next, *v = w->next;
        if (!(tok_is_ident(w, "FP_CONTRACT") || tok_is_ident(w, "FENV_ACCESS") ||
              tok_is_ident(w, "CX_LIMITED_RANGE")) ||
            !(tok_is_ident(v, "ON") || tok_is_ident(v, "OFF") ||
              tok_is_ident(v, "DEFAULT")) ||
            v->next->kind != TK_EOF)
            diag_report(pp->diag, DL_WARNING, "stdc-pragma", t->loc,
                        "malformed or unknown STDC pragma (C99 6.10.6p2)");
    } else if (tok_is_ident(t, "GCC")) {
        Token *w = t->next;
        if (tok_is_ident(w, "system_header")) {
            if (pp->inc->prev)
                pp->inc->file->system_header = true;
            emit = false;
        } else if (tok_is_ident(w, "poison")) {
            Token *u;
            for (u = w->next; u->kind != TK_EOF; u = u->next)
                if (u->kind == TK_IDENT) {
                    u->ident->flags |= IDF_POISONED;
                    PP_EMIT(pp, macro_ref, u->ident, u->ident->macro, u,
                            REF_PRAGMA);
                }
        } else if (tok_is_ident(w, "warning") || tok_is_ident(w, "error")) {
            Token *msg = w->next;
            bool err = tok_is_ident(w, "error");
            if (tok_is_punct(msg, P_LPAREN))
                msg = msg->next;
            diag_report(pp->diag, err ? DL_ERROR : DL_WARNING,
                        err ? "" : "pp-warning-directive", w->loc, "%.*s",
                        msg->kind == TK_STRING ? (int)msg->len - 2 : (int)msg->len,
                        msg->kind == TK_STRING ? msg->text + 1 : msg->text);
        }
    } else if (tok_is_ident(t, "push_macro")) {
        Ident *id = pragma_macro_name(pp, t->next);
        if (id) {
            MacroStackEnt *e = NEW(pp->arena, MacroStackEnt);
            e->name = id;
            e->macro = id->macro;
            e->next = pp->pushed;
            pp->pushed = e;
        }
    } else if (tok_is_ident(t, "pop_macro")) {
        Ident *id = pragma_macro_name(pp, t->next);
        if (id) {
            MacroStackEnt **pe = &pp->pushed;
            while (*pe && (*pe)->name != id)
                pe = &(*pe)->next;
            if (!*pe) {
                diag_report(pp->diag, DL_WARNING, "unbalanced-push-pop-macro",
                            t->loc, "pop_macro(\"%s\") without push_macro",
                            id->str);
            } else {
                Macro *restored = (*pe)->macro;
                if (id->macro && id->macro != restored) {
                    id->macro->undef_loc = t->loc;
                    id->macro->undef_seq = pp->seq;
                }
                id->macro = restored;
                if (restored)
                    restored->undef_loc = 0, restored->undef_seq = 0;
                pp->seq++;
                *pe = (*pe)->next;
                PP_EMIT(pp, checkpoint, t->loc, pp->seq);
            }
        }
    } else {
        diag_report(pp->diag, DL_WARNING, "unknown-pragma", t->loc,
                    "unknown pragma ignored");
    }
    if (emit)
        push_pragma_token(pp, toks, loc);
}

/* _Pragma ( string-literal ); pp->cur is after the name. */
static bool do_pragma_op(PP *pp, Token *name)
{
    Token *lp = pp->cur, *s, *rp, *toks;
    if (pp->in_if_expr) {
        pp_error_at(pp, name, "_Pragma is not allowed in #if");
        return false;
    }
    if (pp->collecting_args) {
        /* deferred: executed when the substituted result is rescanned */
        return false;
    }
    s = lp->next;
    rp = s->next;
    if (!tok_is_punct(lp, P_LPAREN) || s->kind != TK_STRING ||
        !tok_is_punct(rp, P_RPAREN)) {
        pp_error_at(pp, name, "_Pragma takes a parenthesized string literal");
        return false;
    }
    pp->cur = rp->next;
    toks = pp_destringize_pragma(pp, s);
    pp_do_pragma(pp, toks, name->loc);
    return true;
}

/* ---- entry points --------------------------------------------------- */

bool pp_try_expand(PP *pp, Token *tok)
{
    Ident *id = tok->ident;
    Macro *m = id->macro;
    Expansion *e;
    Hideset *hs;
    Token *result;
    uint16_t lead = (uint16_t)(tok->flags & (TF_SPACE | TF_BOL));

    if (!m || (tok->flags & TF_NOEXPAND))
        return false;
    if (hs_contains(tok->hs, m)) {
        tok->flags |= TF_NOEXPAND;
        return false;
    }

    if (m->builtin) {
        if (m->builtin == BUILTIN_PRAGMA_OP) {
            if (!tok_is_punct(pp->cur, P_LPAREN))
                return false;
            return do_pragma_op(pp, tok);
        }
        if (m->builtin >= BUILTIN_HAS_INCLUDE) {
            bool r = false;
            Token *res;
            if (!tok_is_punct(pp->cur, P_LPAREN) && !pp->in_if_expr)
                return false; /* plain identifier outside #if */
            e = new_expansion(pp, m, tok);
            if (!builtin_query(pp, m, tok, &r))
                r = false;
            res = make_token(pp, TK_PPNUM, r ? "1" : "0", tok, e);
            m->expansions++;
            PP_EMIT(pp, expand, e, NULL, 0);
            res->next = pp->cur;
            pp->cur = res;
            return true;
        }
        e = new_expansion(pp, m, tok);
        result = pp_builtin_expand(pp, m, tok, e);
        m->expansions++;
        PP_EMIT(pp, expand, e, NULL, 0);
        result->next = pp->cur;
        pp->cur = result;
        return true;
    }

    if (!m->funclike) {
        e = new_expansion(pp, m, tok);
        m->expansions++;
        PP_EMIT(pp, expand, e, NULL, 0);
        hs = hs_add(pp, tok->hs, m);
        result = subst(pp, m, NULL, e, hs, lead);
        if (!result && (lead & TF_SPACE) && pp->cur->kind != TK_EOF)
            pp->cur->flags |= TF_SPACE;
        push_front(pp, result);
        return true;
    }

    /* function-like: needs '(' as the next token (newlines are white space) */
    if (!tok_is_punct(pp->cur, P_LPAREN))
        return false;
    {
        Arg *args;
        int nargs;
        Token *rparen = NULL;
        Token **raw;
        int i;
        if (!collect_args(pp, m, tok, &args, &nargs, &rparen)) {
            return false;
        }
        e = new_expansion(pp, m, tok);
        e->end_loc = rparen->loc + rparen->rawlen;
        e->end_prov = rparen->prov;
        m->expansions++;
        raw = NEW_ARRAY(pp->arena, Token *, nargs ? nargs : 1);
        for (i = 0; i < nargs; i++)
            raw[i] = args[i].raw;
        PP_EMIT(pp, expand, e, raw, nargs);
        hs = hs_add(pp, hs_intersect(pp, tok->hs, rparen->hs), m);
        result = subst(pp, m, args, e, hs, lead);
        if (!result && (lead & TF_SPACE) && pp->cur->kind != TK_EOF)
            pp->cur->flags |= TF_SPACE;
        push_front(pp, result);
        return true;
    }
}

/* Fully macro-expand an EOF-terminated list in isolation (arguments,
 * #include/#line operands, #if expressions). */
Token *pp_expand_list(PP *pp, Token *list)
{
    Token *saved = pp->cur;
    Token head, *tail = &head;
    head.next = NULL;
    pp->cur = list;
    for (;;) {
        Token *t = pp->cur;
        if (t->kind == TK_EOF) {
            tail->next = t;
            break;
        }
        pp->cur = t->next;
        if (t->kind == TK_IDENT && pp->in_if_expr &&
            t->ident == pp->id_defined) {
            /* keep `defined X` / `defined ( X )` unexpanded */
            Token *c = pp_copy_token(pp, t);
            tail = tail->next = c;
            if (t->prov)
                pp_warn_at(pp, t, "expansion-to-defined",
                           "macro expansion producing 'defined' has "
                           "undefined behavior");
            if (tok_is_punct(pp->cur, P_LPAREN)) {
                tail = tail->next = pp_copy_token(pp, pp->cur);
                pp->cur = pp->cur->next;
            }
            if (pp->cur->kind == TK_IDENT) {
                tail = tail->next = pp_copy_token(pp, pp->cur);
                pp->cur = pp->cur->next;
            }
            continue;
        }
        if (t->kind == TK_IDENT && pp_try_expand(pp, t))
            continue;
        tail = tail->next = pp_copy_token(pp, t);
    }
    pp->cur = saved;
    return head.next;
}
