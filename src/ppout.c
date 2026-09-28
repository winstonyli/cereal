/* ppout.c - `-E` output: GCC-compatible preprocessed text with linemarkers. */
#include "ppout.h"
#include "thread.h"

#include <string.h>

#define OUTBUF (1u << 20)

/* ---- sinks ---------------------------------------------------------- */

void sink_flush(OutSink *o)
{
    if (o->fp && o->len) {
        fwrite(o->buf, 1, o->len, o->fp);
        o->len = 0;
    }
}

void sink_put(OutSink *o, const char *s, size_t n)
{
    if (o->len + n > o->cap) {
        if (o->fp) {
            sink_flush(o);
            if (!o->buf) {
                o->cap = OUTBUF;
                o->buf = xmalloc(o->cap);
            }
            if (n > o->cap) {
                fwrite(s, 1, n, o->fp);
                return;
            }
        } else {
            size_t nc = o->cap ? o->cap : OUTBUF;
            while (nc < o->len + n)
                nc *= 2;
            o->buf = xrealloc(o->buf, nc);
            o->cap = nc;
        }
    }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
}

static void sink_ch(OutSink *o, char c)
{
    if (o->len < o->cap)
        o->buf[o->len++] = c;
    else
        sink_put(o, &c, 1);
}

void sink_free(OutSink *o)
{
    free(o->buf);
    o->buf = NULL;
    o->len = o->cap = 0;
}

/* ---- the transition ------------------------------------------------- */

static bool is_idchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static bool needs_space(const SrcMgr *sm, const Interner *in, const Tok *a,
                        const Tok *b)
{
    int x, y;
    static const char *const pairs[] = {
        "++", "--", "->", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
        "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "<:", ":>",
        "<%", "%>", "%:", "..", "//", "/*", NULL};
    int i;
    if (a->len == 0 || b->len == 0)
        return false;
    if ((a->kind == TK_IDENT || a->kind == TK_PPNUM) &&
        (b->kind == TK_IDENT || b->kind == TK_PPNUM))
        return true;
    x = (unsigned char)tok_text_raw(sm, in, a)[a->len - 1];
    y = (unsigned char)tok_text_raw(sm, in, b)[0];
    if (a->kind == TK_PPNUM && (y == '.' || y == '+' || y == '-' || is_idchar(y)))
        return true;
    if (a->kind == TK_IDENT && a->len == 1 && x == 'L' &&
        (b->kind == TK_STRING || b->kind == TK_CHAR))
        return true;
    if (x == '.' && b->kind == TK_PPNUM)
        return true;
    if (a->kind == TK_PUNCT && b->kind == TK_PUNCT) {
        for (i = 0; pairs[i]; i++)
            if (pairs[i][0] == x && pairs[i][1] == y)
                return true;
    }
    return false;
}

static void newline(PrintState *st, OutSink *o)
{
    sink_ch(o, '\n');
    st->at_bol = true;
    st->have_prev = false;
}

static void marker(bool linemarkers, PrintState *st, const PrintTok *pt,
                   int flag, OutSink *o)
{
    char num[32];
    const char *name = pt->name;
    size_t i;
    if (!st->at_bol)
        newline(st, o);
    st->file = pt->f;
    st->line = pt->line;
    st->name = pt->name;
    st->delta = pt->delta;
    if (!linemarkers)
        return;
    sprintf(num, "# %u \"", pt->line);
    sink_put(o, num, strlen(num));
    for (i = 0; name[i]; i++) {
        if (name[i] == '"' || name[i] == '\\')
            sink_ch(o, '\\');
        sink_ch(o, name[i]);
    }
    sink_ch(o, '"');
    if (flag) {
        sprintf(num, " %d", flag);
        sink_put(o, num, strlen(num));
    }
    if (pt->system)
        sink_put(o, " 3", 2);
    newline(st, o);
}

void print_transition(const SrcMgr *sm, const Interner *in, bool linemarkers,
                      PrintState *st, const PrintTok *pt, OutSink *o)
{
    SrcFile *f = pt->f;
    uint32_t line = pt->line;
    const Tok *t = &pt->t;
    if (f && (f != st->file || st->pending_flag)) {
        /* GCC marks every enter and return, even into the same file */
        marker(linemarkers, st, pt, st->file ? st->pending_flag : 0, o);
    } else if (f && (pt->delta != st->delta ||
                     (pt->name != st->name && strcmp(pt->name, st->name)))) {
        marker(linemarkers, st, pt, 0, o); /* after #line */
    } else if (f && line != st->line) {
        if (line > st->line && line - st->line <= 8) {
            if (!st->at_bol)
                newline(st, o);
            while (st->line + 1 < line) {
                newline(st, o);
                st->line++;
            }
            st->line = line;
        } else if (line > st->line || t->kind == TK_PRAGMA) {
            marker(linemarkers, st, pt, 0, o);
        } else {
            /* backwards (a token of an invocation that began earlier):
             * stay on this output line, but take the token's position so
             * the state after any positioned token depends on it alone */
            st->line = line;
        }
    }
    st->pending_flag = 0; /* a flag belongs to the next token only */
    if (t->kind == TK_PRAGMA) {
        if (!st->at_bol)
            newline(st, o);
        sink_ch(o, '#');
        sink_put(o, tok_text_raw(sm, in, t), t->len);
        newline(st, o);
        st->line++;
        return;
    }
    if (st->at_bol) {
        if (t->flags & TF_SPACE)
            sink_ch(o, ' ');
    } else if ((t->flags & (TF_SPACE | TF_BOL)) ||
               (st->have_prev && needs_space(sm, in, &st->prev, t))) {
        sink_ch(o, ' ');
    }
}

/* ---- the printer ---------------------------------------------------- */

static void on_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Printer *p = ctx;
    (void)f;
    (void)via;
    p->st.pending_flag = 1;
    if (p->open && !p->open->has_first) {
        p->open->events_pre = true;
        p->open->pre_last = 1;
    }
}

static void on_exit(void *ctx, SrcFile *f)
{
    Printer *p = ctx;
    (void)f;
    p->st.pending_flag = 2;
    if (p->open && !p->open->has_first) {
        p->open->events_pre = true;
        p->open->pre_last = 2;
    }
}

void printer_init(Printer *p, PP *pp, OutSink *out, bool linemarkers)
{
    PPListener l;
    memset(p, 0, sizeof *p);
    p->pp = pp;
    p->out = out;
    p->linemarkers = linemarkers;
    p->st.at_bol = true;
    memset(&l, 0, sizeof l);
    l.ctx = p;
    l.file_enter = on_enter;
    l.file_exit = on_exit;
    pp_add_listener(pp, l);
}

static SrcFile *file_of(Printer *p, SrcLoc loc)
{
    SrcFile *f = p->last_f;
    if (f && loc >= f->base && loc < f->base + f->span)
        return f;
    f = srcmgr_file_of(p->pp->sm, loc);
    p->last_f = f;
    return f;
}

static const IncludeFrame *frame_of(PP *pp, SrcFile *f)
{
    const IncludeFrame *fr;
    for (fr = pp->inc; fr; fr = fr->prev)
        if (fr->file == f)
            return fr;
    return NULL;
}

void printer_token(Printer *p, const Tok *t)
{
    PP *pp = p->pp;
    PrintTok pt;
    SrcLoc loc = t->kind == TK_PRAGMA ? t->loc : pp->out_exp_loc;
    SrcFile *f = file_of(p, loc);
    pt.t = *t;
    pt.f = f && f->kind != SF_SCRATCH ? f : NULL;
    pt.line = p->st.line;
    pt.name = "";
    pt.delta = 0;
    pt.system = false;
    if (pt.f) {
        const IncludeFrame *fr = frame_of(pp, pt.f);
        uint32_t line = linecursor_line(&p->lc, pt.f, loc);
        if (fr && fr->line_adj_from && line >= fr->line_adj_from) {
            line = (uint32_t)((int32_t)line + fr->line_delta);
            pt.delta = fr->line_delta;
        }
        pt.line = line;
        pt.name = fr ? fr->presumed_name : pt.f->name;
        pt.system = fr ? fr->system : pt.f->system_header;
    }
    if (p->open && !p->open->has_first) {
        p->open->first = pt;
        print_transition(pp->sm, pp->in, p->linemarkers, &p->st, &pt, p->out);
        p->open->first_body = p->out->len;
        atomic_store_u32(&p->open->has_first, 1); /* publish */
    } else {
        print_transition(pp->sm, pp->in, p->linemarkers, &p->st, &pt, p->out);
    }
    if (t->kind == TK_PRAGMA)
        return;
    sink_put(p->out, pp_text(pp, t), t->len);
    p->st.at_bol = false;
    p->st.prev = *t;
    p->st.have_prev = true;
}

void printer_finish(Printer *p)
{
    if (!p->st.at_bol)
        newline(&p->st, p->out);
}

void pp_write_output(PP *pp, FILE *out, bool linemarkers)
{
    Printer pr;
    OutSink sink;
    Tok t;
    memset(&sink, 0, sizeof sink);
    sink.fp = out;
    printer_init(&pr, pp, &sink, linemarkers);
    while (pp_next(pp, &t))
        printer_token(&pr, &t);
    printer_finish(&pr);
    sink_flush(&sink);
    sink_free(&sink);
}
