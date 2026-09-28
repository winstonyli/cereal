/* pp.c - preprocessor core: token stream, directives, includes,
 * conditional compilation (C99 6.10.1-6.10.8). */
#include "pp.h"

#include <string.h>
#include <time.h>

#define MAX_INCLUDE_DEPTH 200

static void skip_group(PP *pp, SrcLoc after);

/* ---- small helpers -------------------------------------------------- */

void pp_add_listener(PP *pp, PPListener l)
{
    vec_push(&pp->listeners, l);
}

Token *pp_new_eof(PP *pp, SrcLoc loc)
{
    Token *t = NEW(pp->arena, Token);
    t->kind = TK_EOF;
    t->text = "";
    t->loc = loc;
    return t;
}

Token *pp_copy_token(PP *pp, const Token *t)
{
    Token *c = NEW(pp->arena, Token);
    *c = *t;
    c->next = NULL;
    return c;
}

static bool at_line_start(const Token *t)
{
    return (t->flags & TF_BOL) || t->kind == TK_EOF;
}

/* Cut the rest of the current directive line off the stream. */
Token *pp_read_line(PP *pp)
{
    Token *first = pp->cur, *t = pp->cur, *last = NULL;
    while (!at_line_start(t)) {
        last = t;
        t = t->next;
    }
    pp->cur = t;
    if (!last)
        return pp_new_eof(pp, t->loc);
    last->next = pp_new_eof(pp, last->loc + last->rawlen);
    return first;
}

static SrcLoc line_end_loc(Token *line)
{
    Token *t = line;
    while (t->kind != TK_EOF)
        t = t->next;
    return t->loc;
}

SrcLoc prov_expansion_loc(const Prov *p, SrcLoc fallback)
{
    SrcLoc loc = fallback;
    while (p) {
        Expansion *e = p->exp;
        loc = e->name_loc;
        p = e->name_prov;
    }
    return loc;
}

SrcLoc pp_expansion_loc(const Token *t)
{
    return prov_expansion_loc(t->prov, t->loc);
}

void pp_add_expansion_notes(PP *pp, Diagnostic *d, const Token *t)
{
    const Prov *p = t->prov;
    int n = 0;
    while (p && d && n < 16) {
        Expansion *e = p->exp;
        if (p->kind == PROV_ARG)
            diag_note(pp->diag, d, p->loc,
                      "in argument '%s' of macro '%s'",
                      e->macro->params[p->param]->str, e->macro->name->str);
        diag_note(pp->diag, d, e->name_loc, "in expansion of macro '%s'",
                  e->macro->name->str);
        p = e->name_prov;
        n++;
    }
}

Diagnostic *pp_error_at(PP *pp, const Token *t, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = diag_vreport(pp->diag, DL_ERROR, "", t->loc, fmt, ap);
    va_end(ap);
    diag_set_range(d, t->loc, t->loc + t->rawlen);
    pp_add_expansion_notes(pp, d, t);
    return d;
}

Diagnostic *pp_warn_at(PP *pp, const Token *t, const char *id,
                       const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = diag_vreport(pp->diag, DL_WARNING, id, t->loc, fmt, ap);
    va_end(ap);
    diag_set_range(d, t->loc, t->loc + t->rawlen);
    pp_add_expansion_notes(pp, d, t);
    return d;
}

static void pedantic(PP *pp, SrcLoc loc, const char *fmt, ...)
{
    va_list ap;
    if (!pp->opt->pedantic)
        return;
    va_start(ap, fmt);
    diag_vreport(pp->diag, pp->diag->pedantic_errors ? DL_ERROR : DL_WARNING,
                 "pedantic", loc, fmt, ap);
    va_end(ap);
}

static void check_eol(PP *pp, Token *rest, const char *dir)
{
    if (rest->kind != TK_EOF) {
        Diagnostic *d = diag_report(pp->diag, DL_WARNING, "extra-tokens",
                                    rest->loc,
                                    "extra tokens at end of #%s directive", dir);
        diag_set_range(d, rest->loc, line_end_loc(rest));
    }
}

char *tokens_str(Arena *a, const Token *t, const Token *end)
{
    StrBuf sb = {0};
    char *r;
    bool first = true;
    for (; t && t != end && t->kind != TK_EOF; t = t->next) {
        if (t->kind == TK_PLACEMARKER)
            continue;
        if (!first && (t->flags & (TF_SPACE | TF_BOL)))
            sb_putc(&sb, ' ');
        sb_putn(&sb, t->text, t->len);
        first = false;
    }
    r = arena_strndup(a, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    return r;
}

const char *macro_kind_str(const Macro *m)
{
    return m->funclike ? "function-like" : "object-like";
}

char *macro_signature(Arena *a, const Macro *m)
{
    StrBuf sb = {0};
    char *r;
    int i;
    sb_puts(&sb, m->name->str);
    if (m->funclike) {
        sb_putc(&sb, '(');
        for (i = 0; i < m->nparams; i++) {
            if (i)
                sb_puts(&sb, ", ");
            if (m->variadic && i == m->nparams - 1) {
                if (m->gnu_named_variadic)
                    sb_printf(&sb, "%s...", m->params[i]->str);
                else
                    sb_puts(&sb, "...");
            } else {
                sb_puts(&sb, m->params[i]->str);
            }
        }
        sb_putc(&sb, ')');
    }
    r = arena_strndup(a, sb_cstr(&sb), sb.len);
    sb_free(&sb);
    return r;
}

char *macro_body_str(Arena *a, const Macro *m)
{
    return tokens_str(a, m->body, NULL);
}

bool macro_is_guard_like(const Macro *m)
{
    return m->body_len == 0 && !m->funclike;
}

/* ---- hide sets ------------------------------------------------------ */

bool hs_contains(Hideset *hs, Macro *m)
{
    for (; hs; hs = hs->next)
        if (hs->macro == m)
            return true;
    return false;
}

Hideset *hs_add(PP *pp, Hideset *hs, Macro *m)
{
    Hideset *n;
    if (hs_contains(hs, m))
        return hs;
    n = NEW(pp->arena, Hideset);
    n->macro = m;
    n->next = hs;
    return n;
}

Hideset *hs_union(PP *pp, Hideset *a, Hideset *b)
{
    Hideset *r = b;
    if (!a)
        return b;
    if (!b)
        return a;
    for (; a; a = a->next)
        r = hs_add(pp, r, a->macro);
    return r;
}

Hideset *hs_intersect(PP *pp, Hideset *a, Hideset *b)
{
    Hideset *r = NULL;
    for (; a; a = a->next)
        if (hs_contains(b, a->macro))
            r = hs_add(pp, r, a->macro);
    return r;
}

/* ---- include stack -------------------------------------------------- */

static void include_chain_cb(void *ctx, SrcLoc **locs, int *n)
{
    PP *pp = ctx;
    IncludeFrame *f;
    pp->chain_buf.len = 0;
    for (f = pp->inc; f; f = f->prev)
        if (f->include_loc)
            vec_push(&pp->chain_buf, f->include_loc);
    *locs = pp->chain_buf.data;
    *n = (int)pp->chain_buf.len;
}

static void push_file(PP *pp, SrcFile *f, SrcLoc include_loc, int dir_index,
                      const IncludeEvent *via)
{
    IncludeFrame *fr = NEW(pp->arena, IncludeFrame);
    fr->prev = pp->inc;
    fr->file = f;
    fr->rest = pp->cur;
    fr->include_loc = include_loc;
    fr->dir_index = dir_index;
    fr->cond_base = pp->cond;
    fr->presumed_name = f->name;
    pp->inc = fr;
    pp->include_depth++;
    pp->cur = lex_file(pp->arena, pp->in, pp->diag, pp->opt->lex, f);
    PP_EMIT(pp, file_enter, f, via);
}

/* Returns false at the end of the translation unit. */
static bool pop_file(PP *pp)
{
    IncludeFrame *fr = pp->inc;
    while (pp->cond != fr->cond_base) {
        diag_report(pp->diag, DL_ERROR, "", pp->cond->if_loc,
                    "unterminated conditional directive");
        pp->cond = pp->cond->prev;
    }
    if (fr->guard_state == G_AFTER && !fr->file->guard_checked)
        fr->file->guard = fr->guard_candidate;
    fr->file->guard_checked = true;
    PP_EMIT(pp, file_exit, fr->file);
    if (!fr->prev)
        return false;
    pp->cur = fr->rest;
    pp->inc = fr->prev;
    pp->include_depth--;
    return true;
}

/* Something other than the guard's own directives happened at file level. */
static void guard_note_activity(PP *pp)
{
    IncludeFrame *fr = pp->inc;
    if (fr && (fr->guard_state == G_START || fr->guard_state == G_AFTER))
        fr->guard_state = G_INVALID;
}

/* ---- init ----------------------------------------------------------- */

static const char *const month_names[] = {"Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec"};

static Macro *new_builtin(PP *pp, const char *name, BuiltinKind k,
                          bool funclike)
{
    Macro *m = NEW(pp->arena, Macro);
    m->name = intern_cstr(pp->in, name);
    m->id = (uint32_t)pp->macros.len;
    m->builtin = k;
    m->funclike = funclike;
    m->predefined = true;
    m->body = pp_new_eof(pp, 0);
    m->file = pp->builtin_file;
    m->def_seq = pp->seq++;
    m->prev = m->name->history;
    m->name->history = m;
    m->name->macro = m;
    vec_push(&pp->macros, m);
    return m;
}

void pp_init(PP *pp, Arena *a, Interner *in, SrcMgr *sm, DiagEngine *d,
             PPOptions *opt)
{
    size_t i;
    memset(pp, 0, sizeof *pp);
    pp->arena = a;
    pp->in = in;
    pp->sm = sm;
    pp->diag = d;
    pp->opt = opt;
    d->include_chain = include_chain_cb;
    d->include_chain_ctx = pp;

    pp->id_defined = intern_cstr(in, "defined");
    pp->id_va_args = intern_cstr(in, "__VA_ARGS__");
    pp->id_has_include = intern_cstr(in, "__has_include");
    pp->id_has_include_next = intern_cstr(in, "__has_include_next");
    pp->id_pragma = intern_cstr(in, "_Pragma");
    pp->id_once = intern_cstr(in, "once");

    /* search path: quote dirs, then -I, then system */
    for (i = 0; i < opt->quote_dirs.len; i++)
        vec_push(&pp->search, opt->quote_dirs.data[i]);
    pp->first_angle = pp->search.len;
    for (i = 0; i < opt->angle_dirs.len; i++)
        vec_push(&pp->search, opt->angle_dirs.data[i]);
    pp->first_system = pp->search.len;
    for (i = 0; i < opt->system_dirs.len; i++)
        vec_push(&pp->search, opt->system_dirs.data[i]);

    pp->builtin_file = srcmgr_add_virtual(sm, "<built-in>", "", 0);

    if (!opt->date_str) {
        time_t now = time(NULL);
        const char *sde = getenv("SOURCE_DATE_EPOCH");
        struct tm *tm;
        if (sde && *sde)
            now = (time_t)strtoll(sde, NULL, 10);
        tm = sde && *sde ? gmtime(&now) : localtime(&now);
        opt->date_str = arena_printf(a, "\"%s %2d %d\"", month_names[tm->tm_mon],
                                     tm->tm_mday, tm->tm_year + 1900);
        opt->time_str = arena_printf(a, "\"%02d:%02d:%02d\"", tm->tm_hour,
                                     tm->tm_min, tm->tm_sec);
    }

    new_builtin(pp, "__FILE__", BUILTIN_FILE, false);
    new_builtin(pp, "__LINE__", BUILTIN_LINE, false);
    new_builtin(pp, "__DATE__", BUILTIN_DATE, false);
    new_builtin(pp, "__TIME__", BUILTIN_TIME, false);
    new_builtin(pp, "_Pragma", BUILTIN_PRAGMA_OP, true);
    if (opt->gnu_extensions) {
        new_builtin(pp, "__COUNTER__", BUILTIN_COUNTER, false);
        new_builtin(pp, "__INCLUDE_LEVEL__", BUILTIN_INCLUDE_LEVEL, false);
        new_builtin(pp, "__BASE_FILE__", BUILTIN_BASE_FILE, false);
        new_builtin(pp, "__has_include", BUILTIN_HAS_INCLUDE, true);
        new_builtin(pp, "__has_include_next", BUILTIN_HAS_INCLUDE_NEXT, true);
        new_builtin(pp, "__has_attribute", BUILTIN_HAS_ATTRIBUTE, true);
        new_builtin(pp, "__has_builtin", BUILTIN_HAS_BUILTIN, true);
        new_builtin(pp, "__has_c_attribute", BUILTIN_HAS_C_ATTRIBUTE, true);
        new_builtin(pp, "__has_cpp_attribute", BUILTIN_HAS_CPP_ATTRIBUTE, true);
    }
}

void pp_free(PP *pp)
{
    vec_free(&pp->macros);
    vec_free(&pp->expansions);
    vec_free(&pp->listeners);
    vec_free(&pp->search);
    vec_free(&pp->chain_buf);
}

/* Predefines and command-line macros are processed as a virtual file that is
 * pushed before the main file (so they get real locations). */
static StrBuf predef_buf;

void pp_define_builtin_text(PP *pp, const char *name, const char *text)
{
    (void)pp;
    sb_printf(&predef_buf, "#define %s %s\n", name, text);
}

void pp_cmdline_define(PP *pp, const char *def)
{
    const char *eq = strchr(def, '=');
    (void)pp;
    if (eq)
        sb_printf(&predef_buf, "#define %.*s %s\n", (int)(eq - def), def, eq + 1);
    else
        sb_printf(&predef_buf, "#define %s 1\n", def);
}

void pp_cmdline_undef(PP *pp, const char *name)
{
    (void)pp;
    sb_printf(&predef_buf, "#undef %s\n", name);
}

void pp_cmdline_include(PP *pp, const char *path)
{
    (void)pp;
    sb_printf(&predef_buf, "#include \"%s\"\n", path);
}

bool pp_enter_main(PP *pp, const char *path)
{
    SrcFile *f = srcmgr_load(pp->sm, path, SF_USER);
    SrcFile *pre;
    if (!f) {
        diag_report(pp->diag, DL_FATAL, "", 0, "cannot open '%s'", path);
        sb_free(&predef_buf);
        return false;
    }
    pp->main_file = f;
    pp->cur = pp_new_eof(pp, 0);
    push_file(pp, f, 0, -1, NULL);
    /* predefines run first, as if included at the top of the main file */
    pre = srcmgr_add_virtual(pp->sm, "<command line>",
                             predef_buf.data ? predef_buf.data : "",
                             predef_buf.len);
    sb_free(&predef_buf);
    pre->system_header = true;
    push_file(pp, pre, 0, -1, NULL);
    return true;
}

/* ---- main loop ------------------------------------------------------ */

static void warn_unterminated(PP *pp, Token *t)
{
    pp_warn_at(pp, t, "invalid-pp-token", "missing terminating %c character",
               t->text[0] == 'L' ? t->text[1] : t->text[0]);
}

Token *pp_next_raw(PP *pp)
{
    Token *t = pp->cur;
    if (t->kind != TK_EOF)
        pp->cur = t->next;
    return t;
}

Token *pp_next(PP *pp)
{
    for (;;) {
        Token *t = pp->cur;
        if (t->kind == TK_EOF) {
            if (!pop_file(pp))
                return t;
            continue;
        }
        pp->cur = t->next;
        if (!t->prov && (t->flags & TF_BOL) && tok_is_punct(t, P_HASH)) {
            pp_directive(pp, t);
            continue;
        }
        if (!t->prov)
            guard_note_activity(pp);
        if (t->kind == TK_IDENT) {
            if (t->ident->flags & IDF_POISONED)
                pp_error_at(pp, t, "attempt to use poisoned \"%s\"",
                            t->ident->str);
            if (pp_try_expand(pp, t))
                continue;
        } else if (t->flags & TF_UNTERMINATED) {
            warn_unterminated(pp, t);
        }
        return t;
    }
}

/* ---- conditionals --------------------------------------------------- */

static CondFrame *push_cond(PP *pp, CondKind k, SrcLoc loc, bool active)
{
    CondFrame *c = NEW(pp->arena, CondFrame);
    c->prev = pp->cond;
    c->kind = k;
    c->if_loc = loc;
    c->parent_active = true;
    c->active = active;
    c->taken_any = active;
    c->include_depth = pp->include_depth;
    pp->cond = c;
    return c;
}

static bool cur_active(PP *pp)
{
    return !pp->cond || pp->cond->active;
}

static void emit_cond(PP *pp, CondKind k, Token *hash, Token *kw, Token *expr,
                      bool evaluated, bool value, bool taken)
{
    CondEvent ev;
    ev.kind = k;
    ev.hash_loc = hash->loc;
    ev.kw_loc = kw->loc;
    ev.end_loc = expr ? line_end_loc(expr) : kw->loc + kw->rawlen;
    ev.expr = expr;
    ev.evaluated = evaluated;
    ev.value = value;
    ev.taken = taken;
    PP_EMIT(pp, cond, &ev);
}

/* Skip tokens of an inactive group up to (not including) the '#' of the
 * #elif/#else/#endif that ends it. */
static void skip_group(PP *pp, SrcLoc after)
{
    int depth = 0;
    Token *t = pp->cur;
    SrcLoc begin = after;
    while (t->kind != TK_EOF) {
        if ((t->flags & TF_BOL) && tok_is_punct(t, P_HASH) &&
            t->next->kind == TK_IDENT && !(t->next->flags & TF_BOL)) {
            Token *d = t->next;
            if (tok_is_ident(d, "if") || tok_is_ident(d, "ifdef") ||
                tok_is_ident(d, "ifndef")) {
                depth++;
            } else if (tok_is_ident(d, "endif")) {
                if (depth == 0)
                    break;
                depth--;
            } else if ((tok_is_ident(d, "elif") || tok_is_ident(d, "else")) &&
                       depth == 0) {
                break;
            }
        }
        t = t->next;
    }
    pp->cur = t;
    {
        SrcFile *f = pp->inc->file;
        uint32_t line, col;
        SrcLoc end = t->loc;
        if (t->kind != TK_EOF) {
            srcmgr_linecol(f, t->loc, &line, &col);
            end = srcmgr_loc_of(f, line, 1);
        }
        if (begin < end)
            PP_EMIT(pp, skipped, begin, end);
    }
}

/* Location of the start of the line after `line_end`. */
static SrcLoc next_line_loc(PP *pp, SrcLoc line_end)
{
    SrcFile *f = srcmgr_file_of(pp->sm, line_end);
    uint32_t line, col;
    SrcLoc l;
    if (!f)
        return line_end;
    srcmgr_linecol(f, line_end, &line, &col);
    l = srcmgr_loc_of(f, line + 1, 1);
    return l ? l : f->base + f->size;
}

void pp_macro_ref(PP *pp, Token *name, RefKind kind)
{
    Macro *m = name->ident->macro;
    name->ident->flags |= IDF_EVER_REFD;
    if (m && kind != REF_EXPANSION && kind != REF_UNDEF)
        m->cond_refs++;
    PP_EMIT(pp, macro_ref, name->ident, m, name, kind);
}

static void do_if(PP *pp, Token *hash, Token *kw, CondKind k)
{
    Token *line = pp_read_line(pp);
    bool active = cur_active(pp), val = false, ok = true;
    CondFrame *c;
    IncludeFrame *fr = pp->inc;

    /* guard detection: first thing in the file is #ifndef X / #if !defined X */
    if (active && pp->cond == fr->cond_base && fr->guard_state == G_START) {
        Token *g = NULL;
        if (k == COND_IFNDEF && line->kind == TK_IDENT)
            g = line;
        else if (k == COND_IF && tok_is_punct(line, P_BANG) &&
                 tok_is_ident(line->next, "defined")) {
            Token *x = line->next->next;
            if (tok_is_punct(x, P_LPAREN) && x->next->kind == TK_IDENT &&
                tok_is_punct(x->next->next, P_RPAREN) &&
                x->next->next->next->kind == TK_EOF)
                g = x->next;
            else if (x->kind == TK_IDENT && x->next->kind == TK_EOF)
                g = x;
        }
        if (g)
            fr->guard_candidate = g->ident;
        else
            fr->guard_state = G_INVALID;
    } else {
        guard_note_activity(pp);
    }

    if (!active) {
        c = push_cond(pp, k, hash->loc, false);
        c->parent_active = false;
        c->taken_any = true; /* nothing in this chain can be taken */
        emit_cond(pp, k, hash, kw, line, false, false, false);
        skip_group(pp, next_line_loc(pp, line_end_loc(line)));
        return;
    }

    if (k == COND_IF) {
        if (line->kind == TK_EOF) {
            diag_report(pp->diag, DL_ERROR, "", kw->loc, "#if with no expression");
            ok = false;
        } else {
            Token *copy = NULL, **tail = &copy, *t;
            /* evaluate a copy: listeners get the raw expression */
            for (t = line;; t = t->next) {
                *tail = pp_copy_token(pp, t);
                if (t->kind == TK_EOF)
                    break;
                tail = &(*tail)->next;
            }
            val = pp_eval_if(pp, copy, kw->loc, &ok);
        }
    } else {
        if (line->kind != TK_IDENT) {
            diag_report(pp->diag, DL_ERROR, "", line->kind == TK_EOF ? kw->loc : line->loc,
                        "macro name missing in #%s directive",
                        k == COND_IFDEF ? "ifdef" : "ifndef");
            ok = false;
        } else {
            pp_macro_ref(pp, line, REF_IFDEF);
            val = (line->ident->macro != NULL) == (k == COND_IFDEF);
            check_eol(pp, line->next, k == COND_IFDEF ? "ifdef" : "ifndef");
        }
    }
    if (!ok)
        val = false;
    c = push_cond(pp, k, hash->loc, val);
    if (fr->guard_state == G_START && fr->guard_candidate) {
        fr->guard_state = G_IN_GUARD;
        fr->guard_frame = c;
    }
    emit_cond(pp, k, hash, kw, line, true, val, val);
    if (!val)
        skip_group(pp, next_line_loc(pp, line_end_loc(line)));
}

static void do_elif_else(PP *pp, Token *hash, Token *kw, CondKind k)
{
    Token *line = pp_read_line(pp);
    CondFrame *c = pp->cond;
    const char *name = k == COND_ELIF ? "elif" : "else";
    IncludeFrame *fr = pp->inc;
    if (!c || c->include_depth != pp->include_depth) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc, "#%s without #if", name);
        return;
    }
    if (c->seen_else) {
        Diagnostic *d = diag_report(pp->diag, DL_ERROR, "", kw->loc,
                                    "#%s after #else", name);
        diag_note(pp->diag, d, c->if_loc, "the conditional began here");
    }
    if (fr->guard_state == G_IN_GUARD && fr->guard_frame == c)
        fr->guard_state = G_INVALID; /* #else/#elif on the guard */
    c->kind = k;
    if (k == COND_ELSE) {
        c->seen_else = true;
        check_eol(pp, line, "else");
        c->active = c->parent_active && !c->taken_any;
        c->taken_any |= c->active;
        emit_cond(pp, k, hash, kw, line, c->parent_active, c->active, c->active);
    } else {
        bool val = false, ok = true, evaluated = false;
        if (c->parent_active && !c->taken_any) {
            Token *copy = NULL, **tail = &copy, *t;
            for (t = line;; t = t->next) {
                *tail = pp_copy_token(pp, t);
                if (t->kind == TK_EOF)
                    break;
                tail = &(*tail)->next;
            }
            if (line->kind == TK_EOF) {
                diag_report(pp->diag, DL_ERROR, "", kw->loc,
                            "#elif with no expression");
                ok = false;
            } else {
                val = pp_eval_if(pp, copy, kw->loc, &ok) && ok;
            }
            evaluated = true;
        }
        c->active = val;
        c->taken_any |= val;
        emit_cond(pp, k, hash, kw, line, evaluated, val, val);
    }
    if (!c->active)
        skip_group(pp, next_line_loc(pp, line_end_loc(line)));
}

static void do_endif(PP *pp, Token *hash, Token *kw)
{
    Token *line = pp_read_line(pp);
    CondFrame *c = pp->cond;
    IncludeFrame *fr = pp->inc;
    if (!c || c->include_depth != pp->include_depth) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc, "#endif without #if");
        return;
    }
    check_eol(pp, line, "endif");
    emit_cond(pp, COND_ENDIF, hash, kw, line, c->parent_active, false, false);
    if (fr->guard_state == G_IN_GUARD && fr->guard_frame == c)
        fr->guard_state = G_AFTER; /* anything after this invalidates */
    pp->cond = c->prev;
}

/* ---- #define / #undef ---------------------------------------------- */

static bool is_builtin_name(PP *pp, Ident *id)
{
    static const char *const names[] = {
        "__STDC__", "__STDC_VERSION__", "__STDC_HOSTED__", "__FILE__",
        "__LINE__", "__DATE__", "__TIME__", "__STDC_IEC_559__",
        "__STDC_IEC_559_COMPLEX__", "__STDC_ISO_10646__", NULL};
    int i;
    (void)pp;
    for (i = 0; names[i]; i++)
        if (strcmp(id->str, names[i]) == 0)
            return true;
    return false;
}

static bool macros_identical(const Macro *a, const Macro *b)
{
    const Token *x, *y;
    int i;
    if (a->funclike != b->funclike || a->nparams != b->nparams ||
        a->variadic != b->variadic || a->builtin || b->builtin)
        return false;
    for (i = 0; i < a->nparams; i++)
        if (a->params[i] != b->params[i])
            return false;
    for (x = a->body, y = b->body; x->kind != TK_EOF && y->kind != TK_EOF;
         x = x->next, y = y->next) {
        if (x->kind != y->kind || x->len != y->len ||
            memcmp(x->text, y->text, x->len) != 0)
            return false;
        if (x != a->body &&
            ((x->flags & TF_SPACE) != 0) != ((y->flags & TF_SPACE) != 0))
            return false;
    }
    return x->kind == TK_EOF && y->kind == TK_EOF;
}

static int param_index(const Macro *m, const Ident *id)
{
    int i;
    for (i = 0; i < m->nparams; i++)
        if (m->params[i] == id)
            return i;
    return -1;
}

static void do_define(PP *pp, Token *hash)
{
    Token *line = pp_read_line(pp), *name = line, *t, *body;
    Macro *m, *old;
    VEC(Ident *) params = {0};
    VEC(SrcLoc) plocs = {0};
    bool ok = true;
    int n;

    if (name->kind != TK_IDENT) {
        diag_report(pp->diag, DL_ERROR, "",
                    name->kind == TK_EOF ? hash->loc : name->loc,
                    name->kind == TK_EOF ? "macro name missing"
                                         : "macro names must be identifiers");
        return;
    }
    if (name->ident == pp->id_defined) {
        diag_report(pp->diag, DL_ERROR, "", name->loc,
                    "'defined' cannot be used as a macro name");
        return;
    }
    m = NEW(pp->arena, Macro);
    m->name = name->ident;
    m->hash_loc = hash->loc;
    m->name_loc = name->loc;
    m->file = pp->inc->file;
    m->predefined = pp->inc->file->kind == SF_VIRTUAL;
    t = name->next;

    if (tok_is_punct(t, P_LPAREN) && !(t->flags & TF_SPACE)) {
        m->funclike = true;
        t = t->next;
        if (tok_is_punct(t, P_RPAREN)) {
            t = t->next;
        } else {
            for (;;) {
                if (tok_is_punct(t, P_ELLIPSIS)) {
                    m->variadic = true;
                    vec_push(&params, pp->id_va_args);
                    vec_push(&plocs, t->loc);
                    t = t->next;
                    if (!tok_is_punct(t, P_RPAREN)) {
                        diag_report(pp->diag, DL_ERROR, "", t->loc,
                                    "expected ')' after '...'");
                        ok = false;
                        break;
                    }
                    t = t->next;
                    break;
                }
                if (t->kind != TK_IDENT) {
                    diag_report(pp->diag, DL_ERROR, "", t->loc,
                                "expected parameter name, found \"%.*s\"",
                                (int)t->len, t->text);
                    ok = false;
                    break;
                }
                if (t->ident == pp->id_va_args) {
                    diag_report(pp->diag, DL_ERROR, "", t->loc,
                                "__VA_ARGS__ can only appear in the expansion "
                                "of a C99 variadic macro");
                    ok = false;
                    break;
                }
                {
                    size_t i;
                    for (i = 0; i < params.len; i++)
                        if (params.data[i] == t->ident) {
                            diag_report(pp->diag, DL_ERROR, "", t->loc,
                                        "duplicate macro parameter \"%s\"",
                                        t->ident->str);
                            ok = false;
                        }
                }
                vec_push(&params, t->ident);
                vec_push(&plocs, t->loc);
                t = t->next;
                if (tok_is_punct(t, P_ELLIPSIS)) {
                    /* GNU named variadic parameter: args... */
                    pedantic(pp, t->loc, "named variadic macros are a GNU extension");
                    m->variadic = true;
                    m->gnu_named_variadic = true;
                    t = t->next;
                    if (!tok_is_punct(t, P_RPAREN)) {
                        diag_report(pp->diag, DL_ERROR, "", t->loc,
                                    "expected ')' after '...'");
                        ok = false;
                        break;
                    }
                    t = t->next;
                    break;
                }
                if (tok_is_punct(t, P_RPAREN)) {
                    t = t->next;
                    break;
                }
                if (!tok_is_punct(t, P_COMMA)) {
                    diag_report(pp->diag, DL_ERROR, "", t->loc,
                                "expected ',' or ')' in macro parameter list");
                    ok = false;
                    break;
                }
                t = t->next;
            }
        }
    } else if (t->kind != TK_EOF && !(t->flags & TF_SPACE)) {
        diag_report(pp->diag, DL_WARNING, "", t->loc,
                    "ISO C99 requires whitespace after the macro name");
    }
    if (!ok) {
        vec_free(&params);
        vec_free(&plocs);
        return;
    }
    m->nparams = (int)params.len;
    m->params = NEW_ARRAY(pp->arena, Ident *, params.len);
    m->param_locs = NEW_ARRAY(pp->arena, SrcLoc, params.len);
    if (params.len) {
        memcpy(m->params, params.data, sizeof(Ident *) * params.len);
        memcpy(m->param_locs, plocs.data, sizeof(SrcLoc) * params.len);
    }
    vec_free(&params);
    vec_free(&plocs);

    /* replacement list */
    body = t;
    n = 0;
    for (t = body; t->kind != TK_EOF; t = t->next) {
        n++;
        t->flags &= (uint16_t)~TF_BOL;
        if (t->kind == TK_IDENT && t->ident == pp->id_va_args &&
            !(m->variadic && !m->gnu_named_variadic)) {
            if (m->gnu_named_variadic)
                pedantic(pp, t->loc, "__VA_ARGS__ in a named variadic macro");
            else
                diag_report(pp->diag, pp->opt->pedantic ? DL_ERROR : DL_WARNING,
                            "", t->loc,
                            "__VA_ARGS__ can only appear in the expansion of a "
                            "C99 variadic macro");
        }
        if (m->funclike && tok_is_punct(t, P_HASH) &&
            (t->next->kind != TK_IDENT || param_index(m, t->next->ident) < 0)) {
            diag_report(pp->diag, DL_ERROR, "", t->loc,
                        "'#' is not followed by a macro parameter");
            return;
        }
        if (tok_is_punct(t, P_HASHHASH) &&
            (t == body || t->next->kind == TK_EOF)) {
            diag_report(pp->diag, DL_ERROR, "", t->loc,
                        "'##' cannot appear at either end of a macro expansion");
            return;
        }
    }
    m->body = body;
    m->body_len = n;
    m->end_loc = t->loc;

    old = m->name->macro;
    if (old) {
        if (old->builtin || is_builtin_name(pp, m->name)) {
            if (!m->predefined)
                diag_report(pp->diag, DL_WARNING, "builtin-macro-redefined",
                            name->loc, "redefining builtin macro \"%s\"",
                            m->name->str);
        } else if (!macros_identical(old, m)) {
            Diagnostic *d = diag_report(pp->diag, DL_WARNING, "macro-redefined",
                                        name->loc, "\"%s\" redefined",
                                        m->name->str);
            diag_note(pp->diag, d, old->name_loc,
                      "this is the location of the previous definition");
        }
        old->undef_loc = hash->loc;
        old->undef_seq = pp->seq;
    } else if (is_builtin_name(pp, m->name) && !m->predefined) {
        diag_report(pp->diag, DL_WARNING, "builtin-macro-redefined", name->loc,
                    "defining builtin macro \"%s\" is undefined behavior",
                    m->name->str);
    }
    m->id = (uint32_t)pp->macros.len;
    m->def_seq = pp->seq++;
    m->prev = m->name->history;
    m->name->history = m;
    m->name->macro = m;
    vec_push(&pp->macros, m);
    PP_EMIT(pp, define, m, old);
    PP_EMIT(pp, checkpoint, m->end_loc, pp->seq);
}

static void do_undef(PP *pp, Token *hash)
{
    Token *line = pp_read_line(pp);
    Macro *m;
    if (line->kind != TK_IDENT) {
        diag_report(pp->diag, DL_ERROR, "",
                    line->kind == TK_EOF ? hash->loc : line->loc,
                    "macro name missing in #undef");
        return;
    }
    if (line->ident == pp->id_defined) {
        diag_report(pp->diag, DL_ERROR, "", line->loc,
                    "'defined' cannot be used as a macro name");
        return;
    }
    check_eol(pp, line->next, "undef");
    m = line->ident->macro;
    if ((m && m->builtin) || is_builtin_name(pp, line->ident)) {
        if (pp->inc->file->kind != SF_VIRTUAL)
            diag_report(pp->diag, DL_WARNING, "builtin-macro-redefined",
                        line->loc, "undefining builtin macro \"%s\"",
                        line->ident->str);
    }
    pp_macro_ref(pp, line, REF_UNDEF);
    if (m) {
        m->undef_loc = hash->loc;
        m->undef_seq = pp->seq++;
        line->ident->macro = NULL;
    }
    PP_EMIT(pp, undef, line->ident, m, hash->loc, line->loc);
    PP_EMIT(pp, checkpoint, line_end_loc(line), pp->seq);
}

/* ---- #include ------------------------------------------------------- */

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        /* reject directories: reading one fails */
        int c = fgetc(f);
        bool ok = !(c == EOF && ferror(f));
        fclose(f);
        return ok;
    }
    return false;
}

char *pp_search_include(PP *pp, const char *name, bool angled, bool next,
                        int *dir_index)
{
    size_t i, start;
    if (name[0] == '/') {
        *dir_index = -1;
        return file_exists(name) ? arena_strdup(pp->arena, name) : NULL;
    }
    if (next && pp->inc && pp->inc->dir_index >= 0) {
        start = (size_t)pp->inc->dir_index + 1;
    } else {
        if (!angled && !next && pp->inc) {
            char *dir = path_dirname(pp->arena, pp->inc->file->path);
            char *p = path_join(pp->arena, dir, name);
            if (pp->inc->file->kind == SF_VIRTUAL)
                p = arena_strdup(pp->arena, name);
            if (file_exists(p)) {
                *dir_index = -1;
                return p;
            }
        }
        start = angled ? pp->first_angle : 0;
    }
    for (i = start; i < pp->search.len; i++) {
        char *p = path_join(pp->arena, pp->search.data[i], name);
        if (file_exists(p)) {
            *dir_index = (int)i;
            return p;
        }
    }
    return NULL;
}

/* Spelling of `<...>` from the raw source between two tokens. */
static char *raw_between(PP *pp, Token *lt, Token *gt)
{
    SrcFile *f = srcmgr_file_of(pp->sm, lt->loc);
    if (!f || gt->loc <= lt->loc)
        return NULL;
    return arena_strndup(pp->arena, f->buf + (lt->loc + lt->rawlen - f->base),
                         gt->loc - (lt->loc + lt->rawlen));
}

/* Parse a header-name from a directive line.  Returns the name (without
 * delimiters) and sets *angled; *rest receives the token after it. */
static char *parse_header_name(PP *pp, Token *line, bool *angled,
                               bool *expanded, Token **rest, SrcLoc *name_end)
{
    Token *t = line;
    *expanded = false;
    if (t->kind != TK_STRING && !tok_is_punct(t, P_LT)) {
        t = pp_expand_list(pp, line);
        *expanded = true;
    }
    if (t->kind == TK_STRING && t->text[0] == '"') {
        *angled = false;
        *rest = t->next;
        *name_end = t->loc + t->rawlen;
        return arena_strndup(pp->arena, t->text + 1, t->len - 2);
    }
    if (tok_is_punct(t, P_LT)) {
        Token *u = t->next;
        *angled = true;
        while (u->kind != TK_EOF && !tok_is_punct(u, P_GT))
            u = u->next;
        if (u->kind == TK_EOF) {
            diag_report(pp->diag, DL_ERROR, "", t->loc,
                        "missing terminating > character");
            return NULL;
        }
        *rest = u->next;
        *name_end = u->loc + u->rawlen;
        if (!*expanded && !t->prov) {
            char *s = raw_between(pp, t, u);
            if (s)
                return s;
        }
        /* computed include: concatenate spellings (implementation-defined) */
        {
            StrBuf sb = {0};
            Token *v;
            char *r;
            for (v = t->next; v != u; v = v->next) {
                if (v != t->next && (v->flags & TF_SPACE))
                    sb_putc(&sb, ' ');
                sb_putn(&sb, v->text, v->len);
            }
            r = arena_strndup(pp->arena, sb_cstr(&sb), sb.len);
            sb_free(&sb);
            return r;
        }
    }
    diag_report(pp->diag, DL_ERROR, "", t->kind == TK_EOF ? line->loc : t->loc,
                "#include expects \"FILENAME\" or <FILENAME>");
    return NULL;
}

static void do_include(PP *pp, Token *hash, Token *kw, bool next)
{
    Token *line = pp_read_line(pp), *rest = NULL;
    bool angled = false, expanded = false;
    SrcLoc name_end = 0;
    char *name, *path;
    int dir_index = -1;
    IncludeEvent ev;
    SrcFile *f = NULL;

    guard_note_activity(pp);
    if (next && pp->inc->prev == NULL) {
        diag_report(pp->diag, DL_WARNING, "include-next-in-primary", kw->loc,
                    "#include_next in primary source file");
        next = false;
    }
    if (next)
        pedantic(pp, kw->loc, "#include_next is a GNU extension");
    if (line->kind == TK_EOF) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc,
                    "#include expects \"FILENAME\" or <FILENAME>");
        return;
    }
    name = parse_header_name(pp, line, &angled, &expanded, &rest, &name_end);
    if (!name)
        return;
    if (rest)
        check_eol(pp, rest, next ? "include_next" : "include");
    if (!*name) {
        diag_report(pp->diag, DL_ERROR, "", line->loc, "empty filename in #include");
        return;
    }

    memset(&ev, 0, sizeof ev);
    ev.hash_loc = hash->loc;
    ev.name_loc = line->loc;
    ev.name_end = name_end;
    ev.spelled = name;
    ev.angled = angled;
    ev.next = next;
    ev.macro_expanded = expanded;
    ev.from = pp->inc->file;

    path = pp_search_include(pp, name, angled, next, &dir_index);
    if (path) {
        bool sys = dir_index >= 0 && (size_t)dir_index >= pp->first_system;
        f = srcmgr_load(pp->sm, path, sys ? SF_SYSTEM : SF_USER);
        if (f && sys)
            f->system_header = true;
    }
    ev.file = f;
    if (!f) {
        ev.result = INC_NOT_FOUND;
        diag_report(pp->diag, DL_ERROR, "", line->loc, "'%s' file not found",
                    name);
        PP_EMIT(pp, include, &ev);
        return;
    }
    if (f->pragma_once) {
        ev.result = INC_SKIPPED_ONCE;
        PP_EMIT(pp, include, &ev);
        return;
    }
    if (f->guard && f->guard->macro) {
        ev.result = INC_SKIPPED_GUARD;
        PP_EMIT(pp, include, &ev);
        return;
    }
    if (pp->include_depth >= MAX_INCLUDE_DEPTH) {
        diag_report(pp->diag, DL_ERROR, "", hash->loc,
                    "#include nested depth %d exceeds maximum of %d",
                    pp->include_depth, MAX_INCLUDE_DEPTH);
        return;
    }
    ev.result = INC_OK;
    PP_EMIT(pp, include, &ev);
    PP_EMIT(pp, checkpoint, name_end, pp->seq);
    push_file(pp, f, hash->loc, dir_index, &ev);
}

/* ---- #line ---------------------------------------------------------- */

static void do_line(PP *pp, Token *hash, Token *kw, bool gnu_marker)
{
    Token *line = gnu_marker ? kw : pp_read_line(pp);
    Token *t;
    unsigned long long n = 0;
    uint32_t i, phys, col;
    if (gnu_marker) {
        /* kw is the number; read the rest of the line */
        Token *rest = pp_read_line(pp);
        kw->next = rest;
        line = kw;
        pedantic(pp, kw->loc, "style of line directive is a GCC extension");
    }
    t = pp_expand_list(pp, line);
    if (t->kind != TK_PPNUM) {
        diag_report(pp->diag, DL_ERROR, "", t->kind == TK_EOF ? hash->loc : t->loc,
                    "#line directive requires a positive integer argument");
        return;
    }
    for (i = 0; i < t->len; i++) {
        if (t->text[i] < '0' || t->text[i] > '9') {
            diag_report(pp->diag, DL_ERROR, "", t->loc,
                        "\"%.*s\" after #line is not a positive integer",
                        (int)t->len, t->text);
            return;
        }
        n = n * 10 + (unsigned)(t->text[i] - '0');
        if (n > 2147483647ull)
            break;
    }
    if (!gnu_marker && (n == 0 || n > 2147483647ull))
        diag_report(pp->diag, pp->opt->pedantic ? DL_WARNING : DL_REMARK,
                    "pedantic", t->loc,
                    "line number out of range (C99 6.10.4p3)");
    t = t->next;
    if (t->kind == TK_STRING) {
        if (t->text[0] != '"') {
            diag_report(pp->diag, DL_ERROR, "", t->loc,
                        "invalid filename for #line directive");
        } else {
            pp->inc->presumed_name =
                arena_strndup(pp->arena, t->text + 1, t->len - 2);
        }
        t = t->next;
    }
    if (!gnu_marker)
        check_eol(pp, t, "line");
    srcmgr_linecol(pp->inc->file, hash->loc, &phys, &col);
    /* the line after the directive gets number n */
    pp->inc->line_adj_from = phys + 1;
    pp->inc->line_delta = (int32_t)((long long)n - (long long)(phys + 1));
}

uint32_t pp_presumed_line(PP *pp, SrcLoc loc)
{
    SrcFile *f = srcmgr_file_of(pp->sm, loc);
    uint32_t line = 0, col;
    IncludeFrame *fr;
    if (!f)
        return 0;
    srcmgr_linecol(f, loc, &line, &col);
    for (fr = pp->inc; fr; fr = fr->prev)
        if (fr->file == f) {
            if (fr->line_adj_from && line >= fr->line_adj_from)
                return (uint32_t)((int32_t)line + fr->line_delta);
            break;
        }
    return line;
}

/* ---- #error / #warning / #pragma ----------------------------------- */

static void do_message(PP *pp, Token *hash, Token *kw, bool is_error)
{
    Token *line = pp_read_line(pp);
    SrcFile *f = pp->inc->file;
    uint32_t l, c, len;
    const char *text = "";
    (void)hash;
    if (line->kind != TK_EOF) {
        srcmgr_linecol(f, line->loc, &l, &c);
        text = srcmgr_line_text(f, l, &len);
        text = arena_strndup(pp->arena, text + c - 1, len - (c - 1));
    }
    if (is_error) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc, "#error %s", text);
    } else {
        pedantic(pp, kw->loc, "#warning is a GNU extension");
        diag_report(pp->diag, DL_WARNING, "pp-warning-directive", kw->loc,
                    "#warning %s", text);
    }
}

static void do_pragma_directive(PP *pp, Token *hash, Token *kw)
{
    Token *line = pp_read_line(pp);
    (void)kw;
    guard_note_activity(pp);
    pp_do_pragma(pp, line, hash->loc);
}

/* ---- directive dispatch --------------------------------------------- */

void pp_directive(PP *pp, Token *hash)
{
    Token *kw = pp->cur;
    bool active = cur_active(pp);
    if (at_line_start(kw)) {
        /* null directive */
        return;
    }
    pp->cur = kw->next;
    pp->in_directive = true;
    if (kw->kind == TK_IDENT) {
        const char *s = kw->ident->str;
        if (!strcmp(s, "if")) {
            do_if(pp, hash, kw, COND_IF);
            goto out;
        } else if (!strcmp(s, "ifdef")) {
            do_if(pp, hash, kw, COND_IFDEF);
            goto out;
        } else if (!strcmp(s, "ifndef")) {
            do_if(pp, hash, kw, COND_IFNDEF);
            goto out;
        } else if (!strcmp(s, "elif")) {
            do_elif_else(pp, hash, kw, COND_ELIF);
            goto out;
        } else if (!strcmp(s, "else")) {
            do_elif_else(pp, hash, kw, COND_ELSE);
            goto out;
        } else if (!strcmp(s, "endif")) {
            do_endif(pp, hash, kw);
            goto out;
        }
    }
    if (!active) {
        /* cannot happen: skip_group stops only at conditionals */
        pp_read_line(pp);
        goto out;
    }
    guard_note_activity(pp);
    if (kw->kind == TK_IDENT) {
        const char *s = kw->ident->str;
        if (!strcmp(s, "define"))
            do_define(pp, hash);
        else if (!strcmp(s, "undef"))
            do_undef(pp, hash);
        else if (!strcmp(s, "include"))
            do_include(pp, hash, kw, false);
        else if (!strcmp(s, "include_next") && pp->opt->gnu_extensions)
            do_include(pp, hash, kw, true);
        else if (!strcmp(s, "line"))
            do_line(pp, hash, kw, false);
        else if (!strcmp(s, "error"))
            do_message(pp, hash, kw, true);
        else if (!strcmp(s, "warning") && pp->opt->gnu_extensions)
            do_message(pp, hash, kw, false);
        else if (!strcmp(s, "pragma"))
            do_pragma_directive(pp, hash, kw);
        else if ((!strcmp(s, "ident") || !strcmp(s, "sccs")) &&
                 pp->opt->gnu_extensions) {
            pedantic(pp, kw->loc, "#%s is a GCC extension", s);
            pp_read_line(pp);
        } else {
            diag_report(pp->diag, DL_ERROR, "", kw->loc,
                        "invalid preprocessing directive #%s", s);
            pp_read_line(pp);
        }
    } else if (kw->kind == TK_PPNUM && pp->opt->gnu_extensions) {
        do_line(pp, hash, kw, true);
    } else {
        diag_report(pp->diag, DL_ERROR, "", kw->loc,
                    "invalid preprocessing directive");
        pp_read_line(pp);
    }
out:
    pp->in_directive = false;
}
