/* pp.c - preprocessor core: token stream, contexts, directives, includes,
 * conditional compilation (C99 6.10.1-6.10.8). */
#include "pp.h"

#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_INCLUDE_DEPTH 200

/* ---- token buffers -------------------------------------------------- */

static int size_class(uint32_t cap)
{
    int k = 0;
    while ((16u << k) < cap)
        k++;
    return k;
}

void tokbuf_init(PP *pp, TokBuf *b, uint32_t mincap)
{
    int k = size_class(mincap ? mincap : 16);
    if (k >= POOL_CLASSES)
        fatal("token buffer too large");
    b->len = 0;
    b->cap = 16u << k;
    b->t = pp->pool.free[k].len ? vec_pop(&pp->pool.free[k])
                                : xmalloc(sizeof(Tok) * b->cap);
}

void tokbuf_release(PP *pp, TokBuf *b)
{
    if (b->t) {
        int k = size_class(b->cap);
        vec_push(&pp->pool.free[k], b->t);
    }
    b->t = NULL;
    b->len = b->cap = 0;
}

void tokbuf_grow(PP *pp, TokBuf *b)
{
    TokBuf nb;
    if (!b->t) {
        tokbuf_init(pp, b, 16);
        return;
    }
    tokbuf_init(pp, &nb, b->cap * 2);
    memcpy(nb.t, b->t, sizeof(Tok) * b->len);
    nb.len = b->len;
    tokbuf_release(pp, b);
    *b = nb;
}

static void tokpool_free(TokPool *p)
{
    int k;
    for (k = 0; k < POOL_CLASSES; k++) {
        size_t i;
        for (i = 0; i < p->free[k].len; i++)
            free(p->free[k].data[i]);
        vec_free(&p->free[k]);
    }
}

/* ---- contexts and the raw token stream ------------------------------ */

void pp_push_context(PP *pp, Context c)
{
    vec_push(&pp->ctx, c);
}

static void pop_context(PP *pp)
{
    Context *c = &vec_last(&pp->ctx);
    tokbuf_release(pp, &c->owned);
    pp->ctx.len--;
}

static void phase_a_read(PP *pp, Tok *t);
static void guard_note_activity(PP *pp);
static bool plan_read(PP *pp, Tok *t);

/* Poisoned identifiers are reported where they are lexed (as GCC does):
 * source text and the lines of active directives, not the replay of a
 * macro body defined before the pragma. */
static void check_poison_tok(PP *pp, const Tok *t)
{
    Ident *id = ident_by_id(pp->in, t->aux);
    if (pp_poisoned(pp, id))
        pp_error_at(pp, t, "attempt to use poisoned \"%s\"", id->str);
}

/* libcpp's -Wc++-compat: C++ operator names used as identifiers. */
static void check_cxx_opname(PP *pp, const Tok *t)
{
    static const char *const ops[] = {"and", "and_eq", "bitand", "bitor",
        "compl", "not", "not_eq", "or", "or_eq", "xor", "xor_eq"};
    Ident *id = ident_by_id(pp->in, t->aux);
    size_t k;
    if (id->len > 6 || id->len < 2)
        return;
    for (k = 0; k < sizeof ops / sizeof *ops; k++)
        if (!strcmp(id->str, ops[k]) && diag_enabled(pp->diag, "c++-compat")) {
            pp_warn_at(pp, t, "c++-compat", "identifier \"%s\" is a special "
                       "operator name in C++", id->str);
            return;
        }
}

static void check_poison(PP *pp, TokSpan s)
{
    uint32_t i;
    if (!pp->mt->npoison)
        return;
    for (i = 0; i < s.n; i++)
        if (s.t[i].kind == TK_IDENT)
            check_poison_tok(pp, &s.t[i]);
}

TokSrc pp_read_raw(PP *pp, Tok *t)
{
    bool reread;
    for (;;) {
        if (pp->cancel && atomic_load_u32(pp->cancel))
            pp->halted = true;
        if (pp->halted) {
            memset(t, 0, sizeof *t);
            t->kind = TK_EOF;
            t->flags = TF_BOL;
            return SRC_LEXER;
        }
        if (pp->ctx.len) {
            Context *c = &vec_last(&pp->ctx);
            if (c->pos < c->end) {
                *t = c->toks[c->pos++];
                pp->tok_exp_loc = c->self_loc && !c->root_obj &&
                                          !(t->flags & (TF_ORIGIN_BODY |
                                                        TF_PASTED | TF_SYNTH))
                                      ? t->loc : c->exp_loc;
                pp->tok_root_obj = c->root_obj;
                pp->tok_exp_id = c->exp_id;
                pp->tok_root = c->root_id;
                return SRC_CONTEXT;
            }
            if (c->barrier) {
                memset(t, 0, sizeof *t);
                t->kind = TK_EOF;
                t->loc = c->exp_loc;
                return SRC_BARRIER;
            }
            pop_context(pp);
            continue;
        }
        reread = pp->has_pending && pp->pending_unread;
        pp->pending_unread = false;
        if (pp->mode == PPM_PLAN) {
            if (!plan_read(pp, t))
                continue; /* it pushed a context */
        } else if (pp->mode == PPM_PHASE_A) {
            phase_a_read(pp, t);
        } else {
            if (pp->has_pending) {
                *t = pp->pending;
                pp->has_pending = false;
            } else {
                lex_next(&pp->lex, t);
            }
            /* any text outside a directive, even inside an argument list,
             * voids an include guard */
            if (t->kind != TK_EOF && !pp->in_directive &&
                !((t->flags & TF_BOL) && tok_is_punct(t, P_HASH)))
                guard_note_activity(pp);
        }
        if (t->kind == TK_IDENT) {
            if (pp->reads) /* poisoning is read even without a lookup */
                cell_reads_note(pp->reads, t->aux, pp->version_item);
            if (!reread && pp->mt->npoison)
                check_poison_tok(pp, t);
            if (!reread)
                check_cxx_opname(pp, t);
        }
        pp->tok_exp_loc = t->loc;
        pp->tok_root_obj = false;
        pp->tok_exp_id = NO_EXP;
        pp->tok_root = NO_EXP;
        return SRC_LEXER;
    }
}

void pp_unread(PP *pp, const Tok *t, TokSrc src)
{
    if (src == SRC_CONTEXT) {
        vec_last(&pp->ctx).pos--;
    } else if (src == SRC_LEXER) {
        pp->pending = *t;
        pp->has_pending = true;
        pp->pending_unread = true;
    }
}

/* ---- small helpers -------------------------------------------------- */

void pp_version_mismatch(const PP *pp, const Ident *id)
{
    fatal("macro version mismatch for '%s' at seq %u", id->str, pp->seq);
}

void pp_add_listener(PP *pp, PPListener l)
{
    if (l.expand)
        pp->track = TRACK_EXPANSIONS;
    vec_push(&pp->listeners, l);
}

Tok pp_make_token(PP *pp, TokKind k, const char *text, size_t n, SrcLoc loc,
                  uint16_t flags)
{
    Tok t;
    memset(&t, 0, sizeof t);
    t.kind = (uint8_t)k;
    t.loc = loc;
    t.len = (uint32_t)n;
    t.flags = flags;
    if (k == TK_IDENT) {
        t.aux = intern(pp->in, text, n)->id;
    } else {
        t.aux = srcmgr_scratch(pp->sm, &pp->scratch, text, n);
        t.flags |= TF_SPELL;
    }
    return t;
}

void pp_add_expansion_notes(PP *pp, Diagnostic *d)
{
    size_t i = pp->ctx.len;
    int n = 0;
    if (pp->diag->track0)
        return;
    while (d && i-- > 0 && n < 16) {
        Context *c = &pp->ctx.data[i];
        if (c->macro && c->name_loc) {
            diag_note(pp->diag, d, c->name_loc, "in expansion of macro '%s'",
                      c->macro->name->str);
            n++;
        }
    }
}

Diagnostic *pp_error_at(PP *pp, const Tok *t, const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = diag_vreport(pp->diag, DL_ERROR, "", t->loc, fmt, ap);
    va_end(ap);
    diag_set_range(d, t->loc, t->loc + t->len);
    pp_add_expansion_notes(pp, d);
    return d;
}

Diagnostic *pp_warn_at(PP *pp, const Tok *t, const char *id,
                       const char *fmt, ...)
{
    Diagnostic *d;
    va_list ap;
    va_start(ap, fmt);
    d = diag_vreport(pp->diag, DL_WARNING, id, t->loc, fmt, ap);
    va_end(ap);
    diag_set_range(d, t->loc, t->loc + t->len);
    pp_add_expansion_notes(pp, d);
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

static SrcLoc span_end(PP *pp, TokSpan s, SrcLoc fallback)
{
    (void)pp;
    if (!s.n)
        return fallback;
    return s.t[s.n - 1].loc + s.t[s.n - 1].len;
}

static void check_eol(PP *pp, TokSpan rest, const char *dir)
{
    if (rest.n) {
        Diagnostic *d = diag_report(pp->diag, DL_WARNING, "extra-tokens",
                                    rest.t[0].loc,
                                    "extra tokens at end of #%s directive", dir);
        diag_set_range(d, rest.t[0].loc, span_end(pp, rest, rest.t[0].loc));
    }
}

static TokSpan span_from(TokSpan s, uint32_t i)
{
    TokSpan r;
    r.t = s.t + (i < s.n ? i : s.n);
    r.n = i < s.n ? s.n - i : 0;
    return r;
}

char *tokens_str(PP *pp, TokSpan s)
{
    StrBuf sb = {0};
    char *r;
    uint32_t i;
    for (i = 0; i < s.n; i++) {
        const Tok *t = &s.t[i];
        if (i && (t->flags & (TF_SPACE | TF_BOL)))
            sb_putc(&sb, ' ');
        sb_putn(&sb, pp_text(pp, t), t->len);
    }
    r = arena_strndup(pp->arena, sb_cstr(&sb), sb.len);
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

char *macro_body_str(PP *pp, const Macro *m)
{
    TokSpan s;
    s.t = m->body;
    s.n = m->body_len;
    return tokens_str(pp, s);
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

static PlanItem *plan_add(PP *pp, PlanKind k);
static void phase_a_fix_dir_frame(PP *pp);

static void push_file(PP *pp, SrcFile *f, SrcLoc include_loc, int dir_index,
                      const IncludeEvent *via)
{
    IncludeFrame *fr = NEW(pp->arena, IncludeFrame);
    if (pp->mode == PPM_PHASE_A) {
        PlanFrame *pf = NEW(pp->arena, PlanFrame);
        phase_a_fix_dir_frame(pp);
        pf->parent = pp->pframe;
        pf->file = f;
        pf->presumed_name = f->name;
        pf->include_loc = include_loc;
        pf->dir_index = dir_index;
        pf->depth = pp->include_depth + 1;
        pf->system = f->system_header;
        pp->pframe = pf;
        plan_add(pp, PI_ENTER);
    }
    fr->prev = pp->inc;
    fr->file = f;
    fr->saved_lex = pp->lex;
    fr->saved_pending = pp->pending;
    fr->saved_has_pending = pp->has_pending;
    fr->include_loc = include_loc;
    fr->dir_index = dir_index;
    fr->cond_base = pp->cond;
    fr->presumed_name = f->name;
    fr->system = f->system_header;
    pp->inc = fr;
    pp->include_depth++;
    pp->has_pending = false;
    lexer_init(&pp->lex, pp->sm, pp->in, pp->diag, &pp->scratch, pp->opt->lex, f);
    PP_EMIT(pp, file_enter, f, via);
}

/* Returns false at the end of the translation unit. */
static bool pop_file(PP *pp)
{
    IncludeFrame *fr = pp->inc;
    if (pp->mode == PPM_PHASE_A) {
        pp->pframe = pp->pframe->parent;
        plan_add(pp, PI_EXIT);
        pp->diag->key = (uint32_t)pp->plan->items.len - 1;
    }
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
    lexer_free(&pp->lex);
    pp->lex = fr->saved_lex;
    pp->pending = fr->saved_pending;
    pp->has_pending = fr->saved_has_pending;
    pp->inc = fr->prev;
    pp->include_depth--;
    return true;
}

static void guard_note_activity(PP *pp)
{
    IncludeFrame *fr = pp->inc;
    if (fr && (fr->guard_state == G_START || fr->guard_state == G_AFTER))
        fr->guard_state = G_INVALID;
}

const char *pp_presumed_name(PP *pp, SrcFile *f)
{
    IncludeFrame *fr;
    for (fr = pp->inc; fr; fr = fr->prev)
        if (fr->file == f)
            return fr->presumed_name;
    return f->name;
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
            return (uint32_t)((int32_t)line + line_adj_delta(fr->adj, line));
        }
    return line;
}

/* ---- init ----------------------------------------------------------- */

static const char *const month_names[] = {"Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec"};

void pp_install_macro(PP *pp, Macro *m)
{
    MacroSlot *s = mt_slot_w(pp->mt, m->name->id);
    m->prev = s->hist;
    s->hist = m;
    s->cur = m;
}

static Macro *new_builtin(PP *pp, const char *name, BuiltinKind k,
                          bool funclike)
{
    Macro *m = NEW(pp->arena, Macro);
    m->name = intern_cstr(pp->in, name);
    m->id = (uint32_t)pp->macros.len;
    m->builtin = k;
    m->funclike = funclike;
    m->predefined = true;
    m->undef_seq = UINT32_MAX;
    m->file = pp->builtin_file;
    m->def_seq = pp->seq++;
    pp_install_macro(pp, m);
    vec_push(&pp->macros, m);
    return m;
}

void pp_options_finish(PPOptions *opt)
{
    time_t now = time(NULL);
    const char *sde = getenv("SOURCE_DATE_EPOCH");
    struct tm *tm;
    if (opt->date_str)
        return;
    if (sde && *sde)
        now = (time_t)strtoll(sde, NULL, 10);
    tm = sde && *sde ? gmtime(&now) : localtime(&now);
    sprintf(opt->date_buf, "\"%s %2d %d\"", month_names[tm->tm_mon],
            tm->tm_mday, tm->tm_year + 1900);
    sprintf(opt->time_buf, "\"%02d:%02d:%02d\"", tm->tm_hour, tm->tm_min,
            tm->tm_sec);
    opt->date_str = opt->date_buf;
    opt->time_str = opt->time_buf;
}

void pp_init(PP *pp, Arena *a, Interner *in, SrcMgr *sm, DiagEngine *d,
             PPOptions *opt)
{
    size_t i;
    memset(pp, 0, sizeof *pp);
    pp->arena = a;
    pp->in = in;
    pp->mt = mt_new();
    pp->mt_owned = true;
    pp->sm = sm;
    pp->diag = d;
    pp->opt = opt;
    d->include_chain = include_chain_cb;
    d->include_chain_ctx = pp;
    lex_global_init();

    pp->id_defined = intern_cstr(in, "defined");
    pp->id_va_args = intern_cstr(in, "__VA_ARGS__");
    pp->id_pragma = intern_cstr(in, "_Pragma");
    pp->dir_item = SIZE_MAX;
    {
        static const char *const dirs[] = {
            "", "if", "ifdef", "ifndef", "elif", "else", "endif", "define",
            "undef", "include", "include_next", "line", "error", "warning",
            "pragma", "ident", "sccs", "assert", "unassert"};
        int k;
        for (k = 1; k < (int)ARRAY_LEN(dirs); k++)
            intern_cstr(in, dirs[k])->kw = (uint16_t)k;
    }

    for (i = 0; i < opt->quote_dirs.len; i++)
        vec_push(&pp->search, opt->quote_dirs.data[i]);
    pp->first_angle = pp->search.len;
    for (i = 0; i < opt->angle_dirs.len; i++)
        vec_push(&pp->search, opt->angle_dirs.data[i]);
    pp->first_system = pp->search.len;
    for (i = 0; i < opt->system_dirs.len; i++)
        vec_push(&pp->search, opt->system_dirs.data[i]);

    pp->builtin_file = srcmgr_add_virtual(sm, "<built-in>", "", 0);

    if (!opt->date_str)
        pp_options_finish(opt);

    new_builtin(pp, "__FILE__", BUILTIN_FILE, false);
    new_builtin(pp, "__LINE__", BUILTIN_LINE, false);
    new_builtin(pp, "__DATE__", BUILTIN_DATE, false);
    new_builtin(pp, "__TIME__", BUILTIN_TIME, false);
    new_builtin(pp, "__TIMESTAMP__", BUILTIN_TIMESTAMP, false);
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

void pp_init_worker(PP *w, const PP *main, Arena *a, DiagEngine *d)
{
    size_t i;
    memset(w, 0, sizeof *w);
    w->arena = a;
    w->in = main->in;
    w->mt = main->mt;
    w->sm = main->sm;
    w->diag = d;
    w->opt = main->opt;
    d->include_chain = include_chain_cb;
    d->include_chain_ctx = w;
    w->id_defined = main->id_defined;
    w->id_va_args = main->id_va_args;
    w->id_pragma = main->id_pragma;
    for (i = 0; i < main->search.len; i++)
        vec_push(&w->search, main->search.data[i]);
    w->first_angle = main->first_angle;
    w->first_system = main->first_system;
    w->builtin_file = main->builtin_file;
    w->main_file = main->main_file;
    w->host_attrs = main->host_attrs;
    w->host_builtins = main->host_builtins;
    w->dir_item = SIZE_MAX;
    w->track = main->track;
    w->cancel = main->cancel;
}

void pp_free(PP *pp)
{
    while (pp->ctx.len)
        pop_context(pp);
    while (pp->inc && pp->inc->prev) {
        lexer_free(&pp->lex);
        pp->lex = pp->inc->saved_lex;
        pp->inc = pp->inc->prev;
    }
    lexer_free(&pp->lex);
    tokbuf_release(pp, &pp->line);
    tokpool_free(&pp->pool);
    vec_free(&pp->ctx);
    vec_free(&pp->macros);
    vec_free(&pp->expansions);
    vec_free(&pp->listeners);
    vec_free(&pp->search);
    vec_free(&pp->chain_buf);
    sb_free(&pp->sb);
    sb_free(&pp->predef);
    if (pp->mt_owned)
        mt_free(pp->mt);
}

/* Predefines and command-line macros form a virtual file entered first. */

void pp_define_builtin_text(PP *pp, const char *name, const char *text)
{
    sb_printf(&pp->predef, "#define %s %s\n", name, text);
}

void pp_cmdline_define(PP *pp, const char *def)
{
    const char *eq = strchr(def, '=');
    if (eq)
        sb_printf(&pp->predef, "#define %.*s %s\n", (int)(eq - def), def, eq + 1);
    else
        sb_printf(&pp->predef, "#define %s 1\n", def);
}

void pp_cmdline_undef(PP *pp, const char *name)
{
    sb_printf(&pp->predef, "#undef %s\n", name);
}

void pp_cmdline_include(PP *pp, const char *path)
{
    sb_printf(&pp->predef, "#include \"%s\"\n", path);
}

bool pp_enter_main(PP *pp, const char *path)
{
    SrcFile *f = srcmgr_load(pp->sm, path, SF_USER);
    SrcFile *pre;
    if (!f) {
        diag_report(pp->diag, DL_FATAL, "", 0, "cannot open '%s'", path);
        sb_free(&pp->predef);
        return false;
    }
    pp->main_file = f;
    /* an empty lexer below the main file */
    lexer_init(&pp->lex, pp->sm, pp->in, pp->diag, &pp->scratch, pp->opt->lex,
               pp->builtin_file);
    push_file(pp, f, 0, -1, NULL);
    pre = srcmgr_add_virtual(pp->sm, "<command line>",
                             pp->predef.data ? pp->predef.data : "",
                             pp->predef.len);
    sb_free(&pp->predef);
    pre->system_header = true;
    push_file(pp, pre, 0, -1, NULL);
    return true;
}

/* ---- main loop ------------------------------------------------------ */

static bool plan_exit(PP *pp);

bool pp_next(PP *pp, Tok *out)
{
    for (;;) {
        Tok t;
        TokSrc src;
        pp->reading_top = true;
        src = pp_read_raw(pp, &t);
        pp->reading_top = false;
        if (src == SRC_LEXER) {
            if (t.kind == TK_EOF) {
                if (pp->halted ||
                    (pp->mode == PPM_PLAN ? !plan_exit(pp) : !pop_file(pp))) {
                    *out = t;
                    return false;
                }
                continue;
            }
            if (t.kind == TK_DIRMARK) {
                pp_plan_apply_dir(pp, t.aux);
                continue;
            }
            if ((t.flags & TF_BOL) && t.kind == TK_PUNCT && t.punct == P_HASH) {
                pp_directive(pp, &t);
                continue;
            }
            guard_note_activity(pp);
        }
        if (t.kind == TK_IDENT) {
            Ident *id = ident_by_id(pp->in, t.aux);
            Macro *m = pp_macro(pp, id);
            if (m && !(t.flags & TF_NOEXPAND)) {
                if (pp_macro_disabled(pp, m)) {
                    t.flags |= TF_NOEXPAND;
                } else {
                    SrcLoc el = pp->tok_exp_loc;
                    uint32_t root = pp->tok_root;
                    if (pp_try_expand(pp, &t, src))
                        continue;
                    pp->tok_exp_loc = el;
                    pp->tok_root = root;
                }
            }
        } else if (t.flags & TF_UNTERMINATED) {
            pp_warn_at(pp, &t, "invalid-pp-token",
                       "missing terminating %c character",
                       pp_text(pp, &t)[pp_text(pp, &t)[0] == 'L' ? 1 : 0]);
        }
        if (pp->carry_space) {
            t.flags |= TF_SPACE;
            pp->carry_space = false;
        }
        pp->out_exp_loc = pp->tok_exp_loc;
        pp->out_root = pp->tok_root;
        *out = t;
        return true;
    }
}

/* Continue past the end of an included file (GCC does for _Pragma
 * operands; argument lists stop there).  False at the end of the TU, where
 * the EOF must stay readable. */
bool pp_cross_file_end(PP *pp)
{
    if (pp->halted || !pp->inc || !pp->inc->prev)
        return false;
    return pp->mode == PPM_PLAN ? plan_exit(pp) : pop_file(pp);
}

/* ---- directive lines ------------------------------------------------ */

/* Read the rest of the directive line from the lexer into pp->line. */
static TokSpan read_line(PP *pp)
{
    TokSpan s;
    pp->line.len = 0;
    for (;;) {
        Tok t;
        if (pp->has_pending) {
            t = pp->pending;
            pp->has_pending = false;
        } else {
            lex_next(&pp->lex, &t);
        }
        if ((t.flags & TF_BOL) || t.kind == TK_EOF) {
            pp->pending = t;
            pp->has_pending = true;
            pp->pending_unread = false;
            break;
        }
        tokbuf_push(pp, &pp->line, t);
    }
    s.t = pp->line.t;
    s.n = pp->line.len;
    if (pp->dir_poison)
        check_poison(pp, s);
    if (diag_enabled(pp->diag, "c++-compat")) {
        uint32_t k;
        for (k = 0; k < s.n; k++)
            if (s.t[k].kind == TK_IDENT)
                check_cxx_opname(pp, &s.t[k]);
    }
    return s;
}

/* The start of the line holding loc, a token the lexer just produced.
 * The lexer knows the last line start it crossed; scanning back from loc
 * could land inside a multi-line comment. */
static SrcLoc line_start_of(PP *pp, SrcLoc loc)
{
    SrcFile *f = pp->inc->file;
    const char *p = pp->sm->region + loc;
    const char *lb = pp->lex.line_begin;
    if (lb && lb >= f->buf && lb <= p)
        return (SrcLoc)(lb - pp->sm->region);
    while (p > f->buf && p[-1] != '\n' && p[-1] != '\r')
        p--;
    return (SrcLoc)(p - pp->sm->region);
}

/* Skip an inactive group; leaves the lexer at the '#' of the #elif/#else/
 * #endif that ends it (or at end of file). */
static void skip_group(PP *pp)
{
    Lexer *L = &pp->lex;
    int depth = 0;
    SrcLoc begin, end;
    if (pp->has_pending) {
        if (pp->pending.kind == TK_EOF)
            return;
        lexer_seek(L, line_start_of(pp, pp->pending.loc), true);
        pp->has_pending = false;
    }
    begin = lexer_loc(L);
    for (;;) {
        if (lex_line_is_directive(L)) {
            SrcLoc hash = lexer_loc(L);
            Tok h, kw;
            lex_next(L, &h);
            lex_next(L, &kw);
            if (kw.kind == TK_EOF) {
                end = lexer_loc(L);
                break;
            }
            if (kw.flags & TF_BOL) {
                lexer_seek(L, line_start_of(pp, kw.loc), true);
                continue;
            }
            if (kw.kind == TK_IDENT) {
                int k = ident_by_id(pp->in, kw.aux)->kw;
                if (k == KW_IF || k == KW_IFDEF || k == KW_IFNDEF) {
                    depth++;
                } else if (k == KW_ENDIF) {
                    if (depth == 0) {
                        end = line_start_of(pp, hash);
                        lexer_seek(L, hash, true);
                        break;
                    }
                    depth--;
                } else if ((k == KW_ELIF || k == KW_ELSE) && depth == 0) {
                    end = line_start_of(pp, hash);
                    lexer_seek(L, hash, true);
                    break;
                }
            }
            if (!lex_next_line(L)) {
                end = lexer_loc(L);
                break;
            }
        } else if (!lex_next_line(L)) {
            end = lexer_loc(L);
            break;
        }
    }
    if (begin < end)
        PP_EMIT(pp, skipped, begin, end);
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

static void emit_cond(PP *pp, CondKind k, const Tok *hash, const Tok *kw,
                      TokSpan expr, bool evaluated, bool value, bool taken)
{
    CondEvent ev;
    ev.kind = k;
    ev.hash_loc = hash->loc;
    ev.kw_loc = kw->loc;
    ev.end_loc = span_end(pp, expr, kw->loc + kw->len);
    ev.expr = expr;
    ev.evaluated = evaluated;
    ev.value = value;
    ev.taken = taken;
    PP_EMIT(pp, cond, &ev);
}

void pp_macro_ref(PP *pp, const Tok *name, RefKind kind)
{
    Ident *id = ident_by_id(pp->in, name->aux);
    Macro *m = pp_macro(pp, id);
    if (m && kind != REF_EXPANSION && kind != REF_UNDEF)
        atomic_add_u32(&m->cond_refs, 1);
    PP_EMIT(pp, macro_ref, id, m, name, kind);
}

static bool is_word(PP *pp, const Tok *t, const char *s)
{
    return tok_is_word(pp->in, t, s);
}

static void do_if(PP *pp, const Tok *hash, const Tok *kw, CondKind k)
{
    TokSpan line = read_line(pp);
    bool active = cur_active(pp), val = false, ok = true;
    CondFrame *c;
    IncludeFrame *fr = pp->inc;

    if (active && pp->cond == fr->cond_base && fr->guard_state == G_START) {
        const Tok *g = NULL;
        if (k == COND_IFNDEF && line.n >= 1 && line.t[0].kind == TK_IDENT)
            g = &line.t[0];
        else if (k == COND_IF && line.n >= 3 && tok_is_punct(&line.t[0], P_BANG) &&
                 is_word(pp, &line.t[1], "defined")) {
            if (line.n == 5 && tok_is_punct(&line.t[2], P_LPAREN) &&
                line.t[3].kind == TK_IDENT && tok_is_punct(&line.t[4], P_RPAREN))
                g = &line.t[3];
            else if (line.n == 3 && line.t[2].kind == TK_IDENT)
                g = &line.t[2];
        }
        if (g)
            fr->guard_candidate = ident_by_id(pp->in, g->aux);
        else
            fr->guard_state = G_INVALID;
    } else {
        guard_note_activity(pp);
    }

    if (!active) {
        c = push_cond(pp, k, hash->loc, false);
        c->parent_active = false;
        c->taken_any = true;
        emit_cond(pp, k, hash, kw, line, false, false, false);
        skip_group(pp);
        return;
    }

    if (k == COND_IF) {
        if (line.n == 0) {
            diag_report(pp->diag, DL_ERROR, "", kw->loc, "#if with no expression");
            ok = false;
        } else {
            val = pp_eval_if(pp, line, &ok);
        }
    } else {
        if (line.n == 0 || line.t[0].kind != TK_IDENT) {
            diag_report(pp->diag, DL_ERROR, "", line.n ? line.t[0].loc : kw->loc,
                        "macro name missing in #%s directive",
                        k == COND_IFDEF ? "ifdef" : "ifndef");
            ok = false;
        } else {
            pp_macro_ref(pp, &line.t[0], REF_IFDEF);
            val = (pp_macro(pp, ident_by_id(pp->in, line.t[0].aux)) != NULL) ==
                  (k == COND_IFDEF);
            check_eol(pp, span_from(line, 1), k == COND_IFDEF ? "ifdef" : "ifndef");
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
        skip_group(pp);
}

static void do_elif_else(PP *pp, const Tok *hash, const Tok *kw, CondKind k)
{
    TokSpan line = read_line(pp);
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
        fr->guard_state = G_INVALID;
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
            if (line.n == 0) {
                diag_report(pp->diag, DL_ERROR, "", kw->loc,
                            "#elif with no expression");
                ok = false;
            } else {
                val = pp_eval_if(pp, line, &ok) && ok;
            }
            evaluated = true;
        }
        c->active = val;
        c->taken_any |= val;
        emit_cond(pp, k, hash, kw, line, evaluated, val, val);
    }
    if (!c->active)
        skip_group(pp);
}

static void do_endif(PP *pp, const Tok *hash, const Tok *kw)
{
    TokSpan line = read_line(pp);
    CondFrame *c = pp->cond;
    IncludeFrame *fr = pp->inc;
    if (!c || c->include_depth != pp->include_depth) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc, "#endif without #if");
        return;
    }
    check_eol(pp, line, "endif");
    emit_cond(pp, COND_ENDIF, hash, kw, line, c->parent_active, false, false);
    if (fr->guard_state == G_IN_GUARD && fr->guard_frame == c)
        fr->guard_state = G_AFTER;
    pp->cond = c->prev;
}

/* ---- #define / #undef ---------------------------------------------- */

static bool is_builtin_name(const Ident *id)
{
    static const char *const names[] = {
        "__STDC__", "__STDC_VERSION__", "__STDC_HOSTED__", "__FILE__",
        "__LINE__", "__DATE__", "__TIME__", "__TIMESTAMP__",
        "__STDC_IEC_559__",
        "__STDC_IEC_559_COMPLEX__", "__STDC_ISO_10646__", NULL};
    int i;
    for (i = 0; names[i]; i++)
        if (strcmp(id->str, names[i]) == 0)
            return true;
    return false;
}

static bool macros_identical(PP *pp, const Macro *a, const Macro *b)
{
    uint32_t i;
    int k;
    if (a->funclike != b->funclike || a->nparams != b->nparams ||
        a->variadic != b->variadic || a->builtin || b->builtin ||
        a->body_len != b->body_len)
        return false;
    for (k = 0; k < a->nparams; k++)
        if (a->params[k] != b->params[k])
            return false;
    for (i = 0; i < a->body_len; i++) {
        const Tok *x = &a->body[i], *y = &b->body[i];
        if (x->kind != y->kind || x->len != y->len ||
            memcmp(pp_text(pp, x), pp_text(pp, y), x->len) != 0)
            return false;
        if (i && ((x->flags & TF_SPACE) != 0) != ((y->flags & TF_SPACE) != 0))
            return false;
    }
    return true;
}

static void do_define(PP *pp, const Tok *hash)
{
    TokSpan line = read_line(pp);
    const Tok *name;
    Macro *m, *old;
    VEC(Ident *) params = {0};
    VEC(SrcLoc) plocs = {0};
    bool ok = true;
    uint32_t i, b;
    Ident *nid;

    if (line.n == 0 || line.t[0].kind != TK_IDENT) {
        diag_report(pp->diag, DL_ERROR, "", line.n ? line.t[0].loc : hash->loc,
                    line.n ? "macro names must be identifiers"
                           : "macro name missing");
        return;
    }
    name = &line.t[0];
    nid = ident_by_id(pp->in, name->aux);
    if (nid == pp->id_defined) {
        diag_report(pp->diag, DL_ERROR, "", name->loc,
                    "'defined' cannot be used as a macro name");
        return;
    }
    m = NEW(pp->arena, Macro);
    m->name = nid;
    m->undef_seq = UINT32_MAX;
    m->hash_loc = hash->loc;
    m->name_loc = name->loc;
    m->file = pp->inc->file;
    m->predefined = pp->inc->file->kind == SF_VIRTUAL;
    i = 1;

    if (i < line.n && tok_is_punct(&line.t[i], P_LPAREN) &&
        !(line.t[i].flags & TF_SPACE)) {
        m->funclike = true;
        i++;
        if (i < line.n && tok_is_punct(&line.t[i], P_RPAREN)) {
            i++;
        } else {
            for (;;) {
                const Tok *t = i < line.n ? &line.t[i] : NULL;
                SrcLoc here = t ? t->loc : span_end(pp, line, hash->loc);
                if (t && tok_is_punct(t, P_ELLIPSIS)) {
                    m->variadic = true;
                    vec_push(&params, pp->id_va_args);
                    vec_push(&plocs, t->loc);
                    i++;
                    if (i >= line.n || !tok_is_punct(&line.t[i], P_RPAREN)) {
                        diag_report(pp->diag, DL_ERROR, "", here,
                                    "expected ')' after '...'");
                        ok = false;
                    }
                    i++;
                    break;
                }
                if (!t || t->kind != TK_IDENT) {
                    diag_report(pp->diag, DL_ERROR, "", here,
                                "expected parameter name, found \"%.*s\"",
                                t ? (int)t->len : 0, t ? pp_text(pp, t) : "");
                    ok = false;
                    break;
                }
                {
                    Ident *pid = ident_by_id(pp->in, t->aux);
                    size_t k;
                    if (pid == pp->id_va_args) {
                        diag_report(pp->diag, DL_ERROR, "", t->loc,
                                    "__VA_ARGS__ can only appear in the expansion "
                                    "of a C99 variadic macro");
                        ok = false;
                        break;
                    }
                    for (k = 0; k < params.len; k++)
                        if (params.data[k] == pid) {
                            diag_report(pp->diag, DL_ERROR, "", t->loc,
                                        "duplicate macro parameter \"%s\"",
                                        pid->str);
                            ok = false;
                        }
                    vec_push(&params, pid);
                    vec_push(&plocs, t->loc);
                }
                i++;
                if (i < line.n && tok_is_punct(&line.t[i], P_ELLIPSIS)) {
                    pedantic(pp, line.t[i].loc,
                             "named variadic macros are a GNU extension");
                    m->variadic = true;
                    m->gnu_named_variadic = true;
                    i++;
                    if (i >= line.n || !tok_is_punct(&line.t[i], P_RPAREN)) {
                        diag_report(pp->diag, DL_ERROR, "",
                                    i < line.n ? line.t[i].loc : here,
                                    "expected ')' after '...'");
                        ok = false;
                    }
                    i++;
                    break;
                }
                if (i < line.n && tok_is_punct(&line.t[i], P_RPAREN)) {
                    i++;
                    break;
                }
                if (i >= line.n || !tok_is_punct(&line.t[i], P_COMMA)) {
                    diag_report(pp->diag, DL_ERROR, "",
                                i < line.n ? line.t[i].loc : here,
                                "expected ',' or ')' in macro parameter list");
                    ok = false;
                    break;
                }
                i++;
            }
        }
    } else if (i < line.n && !(line.t[i].flags & TF_SPACE)) {
        diag_report(pp->diag, DL_WARNING, "", line.t[i].loc,
                    "ISO C99 requires whitespace after the macro name");
    }
    if (!ok) {
        vec_free(&params);
        vec_free(&plocs);
        return;
    }
    m->nparams = (int)params.len;
    m->params = NEW_ARRAY(pp->arena, Ident *, params.len + 1);
    m->param_locs = NEW_ARRAY(pp->arena, SrcLoc, params.len + 1);
    m->param_raw = NEW_ARRAY(pp->arena, uint8_t, params.len + 1);
    m->param_expanded = NEW_ARRAY(pp->arena, uint8_t, params.len + 1);
    if (params.len) {
        memcpy(m->params, params.data, sizeof(Ident *) * params.len);
        memcpy(m->param_locs, plocs.data, sizeof(SrcLoc) * params.len);
    }
    vec_free(&params);
    vec_free(&plocs);

    /* replacement list */
    m->body_len = i < line.n ? line.n - i : 0;
    m->body = NEW_ARRAY(pp->arena, Tok, m->body_len + 1);
    if (m->body_len)
        memcpy(m->body, line.t + i, sizeof(Tok) * m->body_len);
    {
        /* GCC: `##` marks the token before it; a run of them is one */
        uint32_t w = 0;
        for (b = 0; b < m->body_len; b++) {
            if (w && tok_is_punct(&m->body[b], P_HASHHASH) &&
                tok_is_punct(&m->body[w - 1], P_HASHHASH))
                continue;
            m->body[w++] = m->body[b];
        }
        m->body_len = w;
    }
    for (b = 0; b < m->body_len; b++) {
        Tok *t = &m->body[b];
        t->flags &= (uint16_t)~TF_BOL;
        if (t->kind == TK_IDENT) {
            Ident *id = ident_by_id(pp->in, t->aux);
            int k;
            for (k = 0; k < m->nparams; k++)
                if (m->params[k] == id) {
                    t->flags |= TF_PARAM;
                    t->punct = (uint8_t)k;
                }
            if (id == pp->id_va_args && !(m->variadic && !m->gnu_named_variadic)) {
                if (m->gnu_named_variadic)
                    pedantic(pp, t->loc, "__VA_ARGS__ in a named variadic macro");
                else
                    diag_report(pp->diag, pp->opt->pedantic ? DL_ERROR : DL_WARNING,
                                "", t->loc,
                                "__VA_ARGS__ can only appear in the expansion of "
                                "a C99 variadic macro");
            }
        }
    }
    for (b = 0; b < m->body_len; b++) {
        Tok *t = &m->body[b];
        if (m->funclike && tok_is_punct(t, P_HASH)) {
            if (b + 1 >= m->body_len || !(m->body[b + 1].flags & TF_PARAM)) {
                diag_report(pp->diag, DL_ERROR, "", t->loc,
                            "'#' is not followed by a macro parameter");
                return;
            }
            m->has_ops = true;
        }
        if (tok_is_punct(t, P_HASHHASH)) {
            if (b == 0 || b + 1 == m->body_len) {
                diag_report(pp->diag, DL_ERROR, "", t->loc,
                            "'##' cannot appear at either end of a macro "
                            "expansion");
                return;
            }
            m->has_ops = true;
        }
    }
    for (b = 0; b < m->body_len; b++) {
        Tok *t = &m->body[b];
        if (!(t->flags & TF_PARAM))
            continue;
        if ((b > 0 && (tok_is_punct(&m->body[b - 1], P_HASHHASH) ||
                       (m->funclike && tok_is_punct(&m->body[b - 1], P_HASH)))) ||
            (b + 1 < m->body_len && tok_is_punct(&m->body[b + 1], P_HASHHASH)))
            m->param_raw[t->punct] = 1;
        else
            m->param_expanded[t->punct] = 1;
    }
    m->end_loc = span_end(pp, line, name->loc + name->len);

    old = mt_cur(pp->mt, nid);
    if (old) {
        if (old->builtin || is_builtin_name(nid)) {
            if (!m->predefined)
                diag_report(pp->diag, DL_WARNING, "builtin-macro-redefined",
                            name->loc, "redefining builtin macro \"%s\"",
                            nid->str);
        } else if (!macros_identical(pp, old, m)) {
            Diagnostic *d = diag_report(pp->diag, DL_WARNING, "macro-redefined",
                                        name->loc, "\"%s\" redefined", nid->str);
            diag_note(pp->diag, d, old->name_loc,
                      "this is the location of the previous definition");
        }
        old->undef_loc = hash->loc;
        old->undef_seq = pp->seq;
    } else if (is_builtin_name(nid) && !m->predefined) {
        diag_report(pp->diag, DL_WARNING, "builtin-macro-redefined", name->loc,
                    "defining builtin macro \"%s\" is undefined behavior",
                    nid->str);
    }
    m->id = (uint32_t)pp->macros.len;
    m->def_seq = pp->seq++;
    pp_install_macro(pp, m);
    vec_push(&pp->macros, m);
    PP_EMIT(pp, define, m, old);
    PP_EMIT(pp, checkpoint, m->end_loc, pp->seq);
}

static void do_undef(PP *pp, const Tok *hash)
{
    TokSpan line = read_line(pp);
    Macro *m;
    Ident *id;
    if (line.n == 0 || line.t[0].kind != TK_IDENT) {
        diag_report(pp->diag, DL_ERROR, "", line.n ? line.t[0].loc : hash->loc,
                    "macro name missing in #undef");
        return;
    }
    id = ident_by_id(pp->in, line.t[0].aux);
    if (id == pp->id_defined) {
        diag_report(pp->diag, DL_ERROR, "", line.t[0].loc,
                    "'defined' cannot be used as a macro name");
        return;
    }
    check_eol(pp, span_from(line, 1), "undef");
    m = mt_cur(pp->mt, id);
    if (((m && m->builtin) || is_builtin_name(id)) &&
        pp->inc->file->kind != SF_VIRTUAL)
        diag_report(pp->diag, DL_WARNING, "builtin-macro-redefined",
                    line.t[0].loc, "undefining builtin macro \"%s\"", id->str);
    pp_macro_ref(pp, &line.t[0], REF_UNDEF);
    if (m) {
        m->undef_loc = hash->loc;
        m->undef_seq = pp->seq++;
        mt_slot_w(pp->mt, id->id)->cur = NULL;
    }
    PP_EMIT(pp, undef, id, m, hash->loc, line.t[0].loc);
    PP_EMIT(pp, checkpoint, span_end(pp, line, hash->loc), pp->seq);
}

/* ---- #include ------------------------------------------------------- */

/* Resolve and load an included file; the source manager caches both hits
 * and misses, so repeated searches cost no system calls. */
SrcFile *pp_find_include(PP *pp, const char *name, bool angled, bool next,
                         int *dir_index)
{
    size_t i, start;
    SrcFile *f;
    if (name[0] == '/') {
        *dir_index = -1;
        return srcmgr_load(pp->sm, name, SF_USER);
    }
    if (next && pp->inc && pp->inc->dir_index >= 0) {
        start = (size_t)pp->inc->dir_index + 1;
    } else {
        if (!angled && !next && pp->inc) {
            const char *p;
            if (pp->inc->file->kind == SF_VIRTUAL)
                p = name;
            else
                p = path_join(pp->arena, path_dirname(pp->arena, pp->inc->file->path),
                              name);
            if ((f = srcmgr_load(pp->sm, p, pp->inc->file->system_header
                                                 ? SF_SYSTEM : SF_USER)) != NULL) {
                *dir_index = -1;
                return f;
            }
        }
        start = angled ? pp->first_angle : 0;
    }
    for (i = start; i < pp->search.len; i++) {
        bool sys = i >= pp->first_system;
        f = srcmgr_load(pp->sm, path_join(pp->arena, pp->search.data[i], name),
                        sys ? SF_SYSTEM : SF_USER);
        if (f) {
            if (sys)
                f->system_header = true;
            *dir_index = (int)i;
            return f;
        }
    }
    return NULL;
}

static char *parse_header_name(PP *pp, TokSpan line, bool *angled,
                               bool *expanded, uint32_t *rest, SrcLoc *name_end,
                               TokBuf *tmp)
{
    TokSpan s = line;
    *expanded = false;
    if (!(s.t[0].kind == TK_STRING) && !tok_is_punct(&s.t[0], P_LT)) {
        pp_expand_into(pp, line, tmp);
        s.t = tmp->t;
        s.n = tmp->len;
        *expanded = true;
        if (s.n == 0) {
            diag_report(pp->diag, DL_ERROR, "", line.t[0].loc,
                        "#include expects \"FILENAME\" or <FILENAME>");
            return NULL;
        }
    }
    if (s.t[0].kind == TK_STRING && pp_text(pp, &s.t[0])[0] == '"') {
        *angled = false;
        *rest = 1;
        *name_end = s.t[0].loc + s.t[0].len;
        return arena_strndup(pp->arena, pp_text(pp, &s.t[0]) + 1, s.t[0].len - 2);
    }
    if (tok_is_punct(&s.t[0], P_LT)) {
        uint32_t u = 1;
        *angled = true;
        while (u < s.n && !tok_is_punct(&s.t[u], P_GT))
            u++;
        if (u >= s.n) {
            diag_report(pp->diag, DL_ERROR, "", s.t[0].loc,
                        "missing terminating > character");
            return NULL;
        }
        *rest = u + 1;
        *name_end = s.t[u].loc + s.t[u].len;
        if (!*expanded) {
            SrcLoc b = s.t[0].loc + 1, e = s.t[u].loc;
            if (e >= b)
                return arena_strndup(pp->arena, pp->sm->region + b, e - b);
        }
        {
            StrBuf sb = {0};
            uint32_t v;
            char *r;
            for (v = 1; v < u; v++) {
                if (v > 1 && (s.t[v].flags & TF_SPACE))
                    sb_putc(&sb, ' ');
                sb_putn(&sb, pp_text(pp, &s.t[v]), s.t[v].len);
            }
            r = arena_strndup(pp->arena, sb_cstr(&sb), sb.len);
            sb_free(&sb);
            return r;
        }
    }
    diag_report(pp->diag, DL_ERROR, "", s.t[0].loc,
                "#include expects \"FILENAME\" or <FILENAME>");
    return NULL;
}

static void do_include(PP *pp, const Tok *hash, const Tok *kw, bool next)
{
    TokSpan line = read_line(pp);
    bool angled = false, expanded = false;
    uint32_t rest = 0;
    SrcLoc name_end = 0;
    char *name;
    int dir_index = -1;
    IncludeEvent ev;
    SrcFile *f = NULL;
    TokBuf tmp = {0};

    if (next && pp->inc->prev == NULL) {
        diag_report(pp->diag, DL_WARNING, "include-next-in-primary", kw->loc,
                    "#include_next in primary source file");
        next = false;
    }
    if (next)
        pedantic(pp, kw->loc, "#include_next is a GNU extension");
    if (line.n == 0) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc,
                    "#include expects \"FILENAME\" or <FILENAME>");
        return;
    }
    name = parse_header_name(pp, line, &angled, &expanded, &rest, &name_end, &tmp);
    if (!name) {
        tokbuf_release(pp, &tmp);
        return;
    }
    if (!expanded)
        check_eol(pp, span_from(line, rest), next ? "include_next" : "include");
    tokbuf_release(pp, &tmp);
    if (!*name) {
        diag_report(pp->diag, DL_ERROR, "", line.t[0].loc,
                    "empty filename in #include");
        return;
    }
    memset(&ev, 0, sizeof ev);
    ev.hash_loc = hash->loc;
    ev.name_loc = line.t[0].loc;
    ev.name_end = expanded ? span_end(pp, line, line.t[0].loc) : name_end;
    ev.spelled = name;
    ev.angled = angled;
    ev.next = next;
    ev.macro_expanded = expanded;
    ev.from = pp->inc->file;

    f = pp_find_include(pp, name, angled, next, &dir_index);
    ev.file = f;
    if (!f) {
        ev.result = INC_NOT_FOUND;
        diag_report(pp->diag, pp->opt->fatal_missing_include ? DL_FATAL
                                                              : DL_ERROR,
                    "", line.t[0].loc, "'%s' file not found", name);
        if (pp->opt->fatal_missing_include)
            pp->halted = true;
        PP_EMIT(pp, include, &ev);
        return;
    }
    if (f->pragma_once) {
        ev.result = INC_SKIPPED_ONCE;
        PP_EMIT(pp, include, &ev);
        return;
    }
    if (f->guard && mt_cur(pp->mt, f->guard)) {
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
    PP_EMIT(pp, checkpoint, ev.name_end, pp->seq);
    push_file(pp, f, hash->loc, dir_index, &ev);
}

/* ---- #line ---------------------------------------------------------- */

static void do_line(PP *pp, const Tok *hash, const Tok *kw, bool gnu_marker)
{
    TokSpan line;
    TokBuf tmp = {0}, src = {0};
    TokSpan s;
    unsigned long long n = 0;
    uint32_t i, phys, col, k = 0;
    const char *text;
    if (gnu_marker) {
        TokSpan rest = read_line(pp);
        tokbuf_push(pp, &src, *kw);
        for (i = 0; i < rest.n; i++)
            tokbuf_push(pp, &src, rest.t[i]);
        line.t = src.t;
        line.n = src.len;
        /* cpp_pedwarning (CPP_W_NONE, ...): not tagged with -Wpedantic */
        if (pp->opt->pedantic)
            diag_report(pp->diag, pp->diag->pedantic_errors ? DL_ERROR
                        : DL_WARNING, "", kw->loc,
                        "style of line directive is a GCC extension");
    } else {
        line = read_line(pp);
    }
    pp_expand_into(pp, line, &tmp);
    s.t = tmp.t;
    s.n = tmp.len;
    if (s.n == 0 || s.t[0].kind != TK_PPNUM) {
        diag_report(pp->diag, DL_ERROR, "", s.n ? s.t[0].loc : hash->loc,
                    "#line directive requires a positive integer argument");
        goto out;
    }
    text = pp_text(pp, &s.t[0]);
    for (i = 0; i < s.t[0].len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            diag_report(pp->diag, DL_ERROR, "", s.t[0].loc,
                        "\"%.*s\" after #line is not a positive integer",
                        (int)s.t[0].len, text);
            goto out;
        }
        n = n * 10 + (unsigned)(text[i] - '0');
        if (n > 2147483647ull)
            break;
    }
    if (!gnu_marker && (n == 0 || n > 2147483647ull))
        diag_report(pp->diag, pp->opt->pedantic ? DL_WARNING : DL_REMARK,
                    "pedantic", s.t[0].loc,
                    "line number out of range (C99 6.10.4p3)");
    k = 1;
    if (k < s.n && (s.t[k].kind != TK_STRING || pp_text(pp, &s.t[k])[0] != '"')) {
        /* GCC rejects the whole directive */
        diag_report(pp->diag, DL_ERROR, "", s.t[k].loc,
                    "\"%.*s\" is not a valid filename", (int)s.t[k].len,
                    pp_text(pp, &s.t[k]));
        goto out;
    }
    if (k < s.n) {
        const char *st = pp_text(pp, &s.t[k]);
        pp->inc->presumed_name = arena_strndup(pp->arena, st + 1, s.t[k].len - 2);
        k++;
    }
    if (!gnu_marker)
        check_eol(pp, span_from(s, k), "line");
    srcmgr_linecol(pp->inc->file, hash->loc, &phys, &col);
    if (gnu_marker) {           /* flags: 3 = the following text is a system header */
        bool sys = false;
        for (; k < s.n; k++)
            if (s.t[k].kind == TK_PPNUM && s.t[k].len == 1 &&
                pp_text(pp, &s.t[k])[0] == '3')
                sys = true;
        if (sys || pp->inc->file->nsysmarks)
            srcmgr_mark_system(pp->inc->file, phys + 1, sys);
    }
    {
        LineAdj *a = NEW(pp->arena, LineAdj);
        a->prev = pp->inc->adj;
        a->from = phys + 1;
        a->delta = (int32_t)((long long)n - (long long)(phys + 1));
        pp->inc->adj = a;
    }
out:
    tokbuf_release(pp, &tmp);
    tokbuf_release(pp, &src);
}

/* ---- #error / #warning / #pragma ----------------------------------- */

static void do_message(PP *pp, const Tok *kw, bool is_error)
{
    TokSpan line = read_line(pp);
    const char *text = "";
    if (line.n) {
        SrcLoc b = line.t[0].loc, e = span_end(pp, line, b);
        text = arena_strndup(pp->arena, pp->sm->region + b, e - b);
    }
    if (is_error) {
        diag_report(pp->diag, DL_ERROR, "", kw->loc, "#error %s", text);
    } else {
        pedantic(pp, kw->loc, "#warning is a GNU extension");
        diag_report(pp->diag, DL_WARNING, "pp-warning-directive", kw->loc,
                    "#warning %s", text);
    }
}

/* ---- dispatch ------------------------------------------------------- */

/* ---- GCC assertions ---------------------------------------------- */

void pp_assert_str(PP *pp, const char *pred, const char *answer)
{
    Assertion *a = NEW(pp->arena, Assertion);
    a->pred = intern_cstr(pp->in, pred);
    a->answer = arena_strdup(pp->arena, answer);
    a->next = pp->asserts;
    pp->asserts = a;
}

bool pp_parse_assertion(PP *pp, TokSpan s, uint32_t *i, bool need_answer,
                        SrcLoc at, Ident **pred, const char **answer)
{
    StrBuf *sb = &pp->sb;
    uint32_t k = *i;
    *answer = NULL;
    if (k >= s.n) {
        diag_report(pp->diag, DL_ERROR, "", at, "assertion without predicate");
        return false;
    }
    if (s.t[k].kind != TK_IDENT) {
        diag_report(pp->diag, DL_ERROR, "", s.t[k].loc,
                    "predicate must be an identifier");
        *i = k + 1;
        return false;
    }
    *pred = pp_ident(pp, &s.t[k++]);
    if (k >= s.n || !tok_is_punct(&s.t[k], P_LPAREN)) {
        if (need_answer) {
            diag_report(pp->diag, DL_ERROR, "", s.t[k - 1].loc,
                        "missing '(' after predicate");
            *i = k;
            return false;
        }
        *i = k;
        return true;
    }
    k++;
    sb->len = 0;
    for (; k < s.n && !tok_is_punct(&s.t[k], P_RPAREN); k++) {
        if (sb->len)
            sb_putc(sb, ' ');
        sb_putn(sb, pp_text(pp, &s.t[k]), s.t[k].len);
    }
    if (k >= s.n) {
        diag_report(pp->diag, DL_ERROR, "", span_end(pp, s, at),
                    "missing ')' to complete answer");
        *i = k;
        return false;
    }
    if (sb->len == 0) {
        diag_report(pp->diag, DL_ERROR, "", s.t[k].loc,
                    "predicate's answer is empty");
        *i = k + 1;
        return false;
    }
    *answer = arena_strndup(pp->arena, sb->data, sb->len);
    *i = k + 1;
    return true;
}

bool pp_assertion_holds(PP *pp, Ident *pred, const char *answer)
{
    const Assertion *a;
    for (a = pp->asserts; a; a = a->next)
        if (a->pred == pred && (!answer || strcmp(a->answer, answer) == 0))
            return true;
    return false;
}

/* #assert pred(answer) / #unassert pred[(answer)] (deprecated GCC). */
static void do_assert(PP *pp, const Tok *kw, bool add)
{
    TokSpan line = read_line(pp);
    uint32_t i = 0;
    Ident *pred;
    const char *answer;
    diag_report(pp->diag, DL_WARNING, "deprecated", kw->loc,
                "#%s is a deprecated GCC extension",
                add ? "assert" : "unassert");
    if (!pp_parse_assertion(pp, line, &i, add, kw->loc + kw->len, &pred,
                            &answer))
        return;
    check_eol(pp, span_from(line, i), add ? "assert" : "unassert");
    if (add) {
        if (!pp_assertion_holds(pp, pred, answer))
            pp_assert_str(pp, pred->str, answer);
    } else {
        Assertion **pa = &pp->asserts;
        while (*pa) {
            if ((*pa)->pred == pred &&
                (!answer || strcmp((*pa)->answer, answer) == 0))
                *pa = (*pa)->next;
            else
                pa = &(*pa)->next;
        }
    }
}

/* #ident "string" / #sccs "string": macro-expanded, printed as #ident. */
static void do_ident(PP *pp, const Tok *hash, const Tok *kw)
{
    TokSpan line = read_line(pp);
    TokBuf tmp = {0};
    pp_expand_into(pp, line, &tmp);
    if (tmp.len == 0 || tmp.t[0].kind != TK_STRING) {
        diag_report(pp->diag, DL_ERROR, "", tmp.len ? tmp.t[0].loc : kw->loc,
                    "invalid #%s directive", ident_by_id(pp->in, kw->aux)->str);
    } else {
        StrBuf *sb = &pp->sb;
        TokSpan rest;
        rest.t = tmp.t + 1;
        rest.n = tmp.len - 1;
        check_eol(pp, rest, "ident");
        sb->len = 0;
        sb_puts(sb, "ident ");
        sb_putn(sb, pp_text(pp, &tmp.t[0]), tmp.t[0].len);
        pp_emit_line(pp, sb->data, sb->len, hash->loc);
    }
    tokbuf_release(pp, &tmp);
}

static void phase_a_finish_dir(PP *pp, size_t dir_item);

void pp_directive(PP *pp, const Tok *hash)
{
    Tok kw;
    bool active = cur_active(pp);
    bool saved = pp->in_directive;
    int k;
    size_t dir_item = SIZE_MAX;
    if (pp->mode == PPM_PHASE_A) {
        PlanItem *it = plan_add(pp, PI_DIR);
        it->begin = hash->loc;
        dir_item = pp->plan->items.len - 1;
        pp->dir_item = dir_item;
        pp->diag->key = (uint32_t)dir_item;
    }
    lex_next(&pp->lex, &kw);
    if ((kw.flags & TF_BOL) || kw.kind == TK_EOF) {
        pp->pending = kw; /* null directive */
        pp->has_pending = true;
        pp->pending_unread = false;
        if (dir_item != SIZE_MAX)
            phase_a_finish_dir(pp, dir_item);
        return;
    }
    pp->in_directive = true;
    k = kw.kind == TK_IDENT ? ident_by_id(pp->in, kw.aux)->kw : KW_NONE;
    /* the lines GCC checks for poisoned names (not #elif) */
    pp->dir_poison = active && pp->mode != PPM_PLAN &&
                     (k == KW_IF || k == KW_IFDEF || k == KW_IFNDEF ||
                      k == KW_DEFINE || k == KW_UNDEF || k == KW_INCLUDE ||
                      k == KW_INCLUDE_NEXT || k == KW_LINE);
    switch (k) {
    case KW_IF: do_if(pp, hash, &kw, COND_IF); goto out;
    case KW_IFDEF: do_if(pp, hash, &kw, COND_IFDEF); goto out;
    case KW_IFNDEF: do_if(pp, hash, &kw, COND_IFNDEF); goto out;
    case KW_ELIF: do_elif_else(pp, hash, &kw, COND_ELIF); goto out;
    case KW_ELSE: do_elif_else(pp, hash, &kw, COND_ELSE); goto out;
    case KW_ENDIF: do_endif(pp, hash, &kw); goto out;
    default: break;
    }
    if (!active) {
        read_line(pp);
        goto out;
    }
    guard_note_activity(pp);
    switch (k) {
    case KW_DEFINE: do_define(pp, hash); goto out;
    case KW_UNDEF: do_undef(pp, hash); goto out;
    case KW_INCLUDE: do_include(pp, hash, &kw, false); goto out;
    case KW_LINE: do_line(pp, hash, &kw, false); goto out;
    case KW_ERROR: do_message(pp, &kw, true); goto out;
    case KW_PRAGMA: pp_do_pragma(pp, read_line(pp), hash->loc); goto out;
    case KW_INCLUDE_NEXT:
        if (pp->opt->gnu_extensions) {
            do_include(pp, hash, &kw, true);
            goto out;
        }
        break;
    case KW_WARNING:
        if (pp->opt->gnu_extensions) {
            do_message(pp, &kw, false);
            goto out;
        }
        break;
    case KW_ASSERT:
    case KW_UNASSERT:
        if (pp->opt->gnu_extensions) {
            do_assert(pp, &kw, k == KW_ASSERT);
            goto out;
        }
        break;
    case KW_IDENT:
    case KW_SCCS:
        if (pp->opt->gnu_extensions) {
            pedantic(pp, kw.loc, "#%s is a GCC extension",
                     ident_by_id(pp->in, kw.aux)->str);
            do_ident(pp, hash, &kw);
            goto out;
        }
        break;
    default:
        break;
    }
    if (kw.kind == TK_IDENT) {
        diag_report(pp->diag, DL_ERROR, "", kw.loc,
                    "invalid preprocessing directive #%s",
                    ident_by_id(pp->in, kw.aux)->str);
        read_line(pp);
    } else if (kw.kind == TK_PPNUM && pp->opt->gnu_extensions) {
        do_line(pp, hash, &kw, true);
    } else {
        diag_report(pp->diag, DL_ERROR, "", kw.loc,
                    "invalid preprocessing directive");
        read_line(pp);
    }
out:
    pp->dir_poison = false;
    pp->in_directive = saved;
    if (dir_item != SIZE_MAX)
        phase_a_finish_dir(pp, dir_item);
}

/* ---- phase A: directives only --------------------------------------- */

static PlanItem *plan_add(PP *pp, PlanKind k)
{
    PlanItem it;
    memset(&it, 0, sizeof it);
    it.kind = (uint8_t)k;
    it.version = pp->seq;
    it.counter = pp->counter;
    it.frame = pp->pframe;
    vec_push(&pp->plan->items, it);
    return &vec_last(&pp->plan->items);
}

/* An #include pushes the child before the directive finishes: the DIR
 * item must carry the includer's frame. */
static void phase_a_fix_dir_frame(PP *pp)
{
    if (pp->dir_item != SIZE_MAX) {
        PlanItem *it = &pp->plan->items.data[pp->dir_item];
        it->frame = pp->pframe;
        it->version = pp->seq;
        it->counter = pp->counter;
        pp->dir_item = SIZE_MAX;
    }
}

static void phase_a_finish_dir(PP *pp, size_t dir_item)
{
    PlanFrame *pf = pp->pframe;
    IncludeFrame *fr = pp->inc;
    /* #line changed the presumed position: new frame snapshot */
    if (fr && pf && fr->file == pf->file &&
        (fr->presumed_name != pf->presumed_name || fr->adj != pf->adj ||
         fr->system != pf->system)) {
        PlanFrame *n = NEW(pp->arena, PlanFrame);
        *n = *pf;
        n->presumed_name = fr->presumed_name;
        n->adj = fr->adj;
        n->system = fr->system;
        pp->pframe = n;
    }
    if (pp->dir_item == dir_item) {
        PlanItem *it = &pp->plan->items.data[dir_item];
        it->version = pp->seq;
        it->counter = pp->counter;
        it->frame = pp->pframe;
        pp->dir_item = SIZE_MAX;
    }
}

/* Record the text from the lexer's position (a line start) up to the next
 * directive line as segments, split at certified line starts. */
/* Content-defined cuts: a line fires with probability proportional to its
 * length (about once per `chunk` bytes), decided by a hash of its text,
 * once `chunk / 4` bytes have passed since the last cut.  A cut lands on
 * the next line start, or on the first segment after a directive. */
static void cdc_line(Plan *plan, const char *p, size_t n)
{
    uint64_t h;
    plan->cdc_bytes += n;
    if (plan->cdc_fire || plan->cdc_bytes < plan->chunk / 4)
        return;
    h = hash_bytes(p, n) & 0xFFFFu;
    if (h * plan->chunk < (uint64_t)n * 0x10000u)
        plan->cdc_fire = true;
}

static void phase_a_segment(PP *pp)
{
    Lexer *L = &pp->lex;
    Plan *plan = pp->plan;
    SrcLoc chunk_start = lexer_loc(L), end;
    bool content = false, split = false;
    size_t chunk = plan->chunk;
    size_t max = plan->cdc ? 4 * chunk : chunk;
    bool cut = false;
    DiagEngine *d = L->diag;
    L->diag = NULL; /* the workers lex this text and report */
    if (plan->cdc && plan->cdc_fire) { /* due since before the directive */
        cut = true;
        plan->cdc_fire = false;
        plan->cdc_bytes = 0;
    }
    for (;;) {
        SrcLoc line = lexer_loc(L);
        const char *lp = L->p;
        bool due = chunk && content &&
                   (line - chunk_start >= max || (plan->cdc && plan->cdc_fire));
        if (due) {
            PlanItem *it = plan_add(pp, PI_SEG);
            it->begin = chunk_start;
            it->end = line;
            it->split = split;
            it->cut = cut;
            plan->text_bytes += line - chunk_start;
            chunk_start = line;
            content = false;
            split = true;
            cut = plan->cdc;
            plan->cdc_fire = false;
            plan->cdc_bytes = 0;
        }
        L->unterminated = false;
        if (lex_line_is_directive(L)) {
            end = lexer_loc(L);
            break;
        }
        /* text, or a comment that runs to the end (a worker reports it) */
        if (L->p < L->lim || L->unterminated)
            content = true;
        if (!lex_next_line(L)) {
            end = lexer_loc(L);
            if (plan->cdc)
                cdc_line(plan, lp, (size_t)(L->p - lp));
            break;
        }
        if (plan->cdc)
            cdc_line(plan, lp, (size_t)(L->p - lp));
    }
    if (content) {
        PlanItem *it = plan_add(pp, PI_SEG);
        it->begin = chunk_start;
        it->end = end;
        it->split = split;
        it->cut = cut;
        plan->text_bytes += end - chunk_start;
        guard_note_activity(pp);
    } else {
        if (split)
            guard_note_activity(pp);
        if (cut) /* no text here: the next segment takes the cut */
            plan->cdc_fire = true;
    }
    L->bol = true;
    L->space = false;
    L->diag = d;
}

static void phase_a_read(PP *pp, Tok *t)
{
    if (pp->has_pending) {
        Tok p = pp->pending;
        pp->has_pending = false;
        if (p.kind == TK_EOF || ((p.flags & TF_BOL) && tok_is_punct(&p, P_HASH))) {
            *t = p;
            return;
        }
        lexer_seek(&pp->lex, line_start_of(pp, p.loc), true);
    }
    phase_a_segment(pp);
    lex_next(&pp->lex, t);
}

bool pp_run_phase_a(PP *pp, Plan *plan)
{
    Tok t;
    pp->mode = PPM_PHASE_A;
    pp->plan = plan;
    pp->dir_item = SIZE_MAX;
    while (pp_next(pp, &t))
        ; /* phase A never yields tokens */
    return true;
}

/* ---- phase B: reading a plan ---------------------------------------- */

static IncludeFrame *frame_from_plan(PP *pp, const PlanFrame *pf)
{
    IncludeFrame *fr;
    if (!pf)
        return NULL;
    fr = NEW(pp->arena, IncludeFrame);
    fr->prev = frame_from_plan(pp, pf->parent);
    fr->file = pf->file;
    fr->presumed_name = pf->presumed_name;
    fr->adj = pf->adj;
    fr->include_loc = pf->include_loc;
    fr->dir_index = pf->dir_index;
    fr->system = pf->system;
    return fr;
}

static void apply_frame(PP *pp, const PlanFrame *pf)
{
    IncludeFrame *fr = pp->inc;
    if (!fr || !pf)
        return;
    fr->presumed_name = pf->presumed_name;
    fr->adj = pf->adj;
    fr->system = pf->system;
}

void pp_plan_start(PP *pp, Plan *plan, size_t item)
{
    const PlanItem *it = &plan->items.data[item];
    pp->mode = PPM_PLAN;
    pp->plan = plan;
    pp->plan_pos = item;
    pp->seg_active = false;
    pp->versioned = true;
    pp->version = it->version;
    pp->version_item = (uint32_t)item;
    pp->counter = it->counter;
    /* the frame in effect before this item */
    pp->inc = frame_from_plan(pp, it->kind == PI_ENTER ? it->frame->parent
                                                        : it->frame);
    pp->include_depth = it->kind == PI_ENTER ? it->frame->depth - 1
                                             : (it->frame ? it->frame->depth : 0);
    pp->main_file = plan->items.data[0].frame->file;
}

void pp_plan_apply_dir(PP *pp, uint32_t item)
{
    const PlanItem *it = &pp->plan->items.data[item];
    pp->version = it->version;
    pp->version_item = item;
    pp->counter = it->counter;
    apply_frame(pp, it->frame);
}

static bool plan_read(PP *pp, Tok *t)
{
    Plan *plan = pp->plan;
    for (;;) {
        PlanItem *it;
        if (pp->has_pending) {
            *t = pp->pending;
            pp->has_pending = false;
            return true;
        }
        if (pp->seg_active) {
            lex_next(&pp->lex, t);
            if (t->kind != TK_EOF)
                return true;
            pp->seg_active = false;
        }
        memset(t, 0, sizeof *t);
        t->kind = TK_EOF;
        t->flags = TF_BOL;
        if (pp->plan_pos >= plan->items.len)
            return true;
        if (pp->plan_stop && pp->plan_pos >= pp->plan_stop) {
            pp->plan_stop_clean = pp->plan_pos == pp->plan_stop &&
                                  pp->reading_top && pp->ctx.len == 0 &&
                                  !pp->carry_space;
            pp->plan_pos = plan->items.len;
            return true;
        }
        it = &plan->items.data[pp->plan_pos];
        pp->diag->key = (uint32_t)pp->plan_pos;
        switch (it->kind) {
        case PI_SEG:
            if (pp->on_boundary &&
                !pp->on_boundary(pp->boundary_ctx, pp->plan_pos,
                                 pp->reading_top && pp->ctx.len == 0 &&
                                     !pp->carry_space)) {
                pp->plan_pos = plan->items.len; /* stop */
                return true;
            }
            pp->version = it->version;
            pp->version_item = (uint32_t)pp->plan_pos;
            pp->counter = it->counter;
            apply_frame(pp, it->frame);
            lexer_free(&pp->lex);
            lexer_init_range(&pp->lex, pp->sm, pp->in, &pp->scratch,
                             pp->opt->lex, it->begin, it->end - it->begin);
            pp->lex.diag = pp->diag;
            pp->lex.opt = pp->opt->lex;
            pp->seg_active = true;
            pp->plan_pos++;
            continue;
        case PI_DIR:
            t->kind = TK_DIRMARK;
            t->loc = it->begin;
            t->aux = (uint32_t)pp->plan_pos;
            pp->plan_pos++;
            return true;
        case PI_ENTER: {
            IncludeFrame *fr = frame_from_plan(pp, it->frame);
            fr->prev = pp->inc;
            pp->inc = fr;
            pp->include_depth = it->frame->depth;
            pp->plan_pos++;
            PP_EMIT(pp, file_enter, it->frame->file, NULL);
            continue;
        }
        case PI_EXIT:
            t->loc = it->begin;
            return true; /* pp_next's EOF handling calls plan_exit */
        case PI_PRAGMA: {
            Context c;
            memset(&c, 0, sizeof c);
            tokbuf_init(pp, &c.owned, 1);
            c.owned.t[0] = plan->pragmas.data[it->end];
            c.owned.len = 1;
            c.toks = c.owned.t;
            c.end = 1;
            c.exp_loc = it->begin;
            c.exp_id = c.root_id = NO_EXP;
            pp->plan_pos++;
            pp_push_context(pp, c);
            return false;
        }
        }
    }
}

static bool plan_exit(PP *pp)
{
    Plan *plan = pp->plan;
    if (pp->plan_pos >= plan->items.len ||
        plan->items.data[pp->plan_pos].kind != PI_EXIT)
        return false;
    if (pp->inc) {
        PP_EMIT(pp, file_exit, pp->inc->file);
        pp->inc = pp->inc->prev;
    }
    pp->include_depth--;
    pp->plan_pos++;
    return pp->plan_pos < plan->items.len;
}
