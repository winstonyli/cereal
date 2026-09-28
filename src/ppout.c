/* ppout.c - `-E` output: GCC-compatible preprocessed text with linemarkers. */
#include "ppout.h"

#include <string.h>

#define OUTBUF (1u << 20)

typedef struct Printer {
    PP *pp;
    FILE *out;
    char *buf;
    size_t len;
    SrcFile *file;           /* file of the output cursor */
    uint32_t line;           /* presumed line of the output cursor */
    LineCursor lc;
    SrcFile *last_f;         /* cache for file lookup */
    bool at_bol;
    int pending_flag;        /* 1 = entered, 2 = returned */
    bool linemarkers;
    Tok prev;
    bool have_prev;
} Printer;

static void flush(Printer *p)
{
    if (p->len)
        fwrite(p->buf, 1, p->len, p->out);
    p->len = 0;
}

static void put(Printer *p, const char *s, size_t n)
{
    if (p->len + n > OUTBUF) {
        flush(p);
        if (n > OUTBUF) {
            fwrite(s, 1, n, p->out);
            return;
        }
    }
    memcpy(p->buf + p->len, s, n);
    p->len += n;
}

static void putch(Printer *p, char c)
{
    if (p->len == OUTBUF)
        flush(p);
    p->buf[p->len++] = c;
}

static void on_enter(void *ctx, SrcFile *f, const IncludeEvent *via)
{
    Printer *p = ctx;
    (void)f;
    (void)via;
    p->pending_flag = 1;
}

static void on_exit(void *ctx, SrcFile *f)
{
    Printer *p = ctx;
    (void)f;
    p->pending_flag = 2;
}

static bool is_idchar(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '$' || c >= 0x80;
}

static bool needs_space(Printer *p, const Tok *a, const Tok *b)
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
    x = (unsigned char)pp_text(p->pp, a)[a->len - 1];
    y = (unsigned char)pp_text(p->pp, b)[0];
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

static void newline(Printer *p)
{
    putch(p, '\n');
    p->at_bol = true;
    p->have_prev = false;
}

static void marker(Printer *p, SrcFile *f, uint32_t line, int flag)
{
    const char *name;
    char num[32];
    size_t i;
    if (!p->at_bol)
        newline(p);
    p->file = f;
    p->line = line;
    if (!p->linemarkers)
        return;
    name = pp_presumed_name(p->pp, f);
    sprintf(num, "# %u \"", line);
    put(p, num, strlen(num));
    for (i = 0; name[i]; i++) {
        if (name[i] == '"' || name[i] == '\\')
            putch(p, '\\');
        putch(p, name[i]);
    }
    putch(p, '"');
    if (flag) {
        sprintf(num, " %d", flag);
        put(p, num, strlen(num));
    }
    if (f->system_header)
        put(p, " 3", 2);
    newline(p);
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

static uint32_t presumed_line(Printer *p, SrcFile *f, SrcLoc loc)
{
    IncludeFrame *fr = p->pp->inc;
    uint32_t line = linecursor_line(&p->lc, f, loc);
    if (fr && fr->file == f && fr->line_adj_from && line >= fr->line_adj_from)
        return (uint32_t)((int32_t)line + fr->line_delta);
    if (fr && fr->file != f)
        return pp_presumed_line(p->pp, loc);
    return line;
}

void pp_write_output(PP *pp, FILE *out, bool linemarkers)
{
    Printer pr;
    PPListener l;
    Tok t;
    memset(&pr, 0, sizeof pr);
    memset(&l, 0, sizeof l);
    pr.pp = pp;
    pr.out = out;
    pr.buf = xmalloc(OUTBUF);
    pr.at_bol = true;
    pr.linemarkers = linemarkers;
    l.ctx = &pr;
    l.file_enter = on_enter;
    l.file_exit = on_exit;
    pp_add_listener(pp, l);

    while (pp_next(pp, &t)) {
        SrcLoc loc = t.kind == TK_PRAGMA ? t.loc : pp->out_exp_loc;
        SrcFile *f = file_of(&pr, loc);
        uint32_t line;
        if (f && f->kind == SF_SCRATCH)
            f = NULL;
        line = f ? presumed_line(&pr, f, loc) : pr.line;
        if (f && f != pr.file) {
            marker(&pr, f, line, pr.file ? pr.pending_flag : 0);
            pr.pending_flag = 0;
        } else if (f && line != pr.line) {
            if (line > pr.line && line - pr.line <= 8) {
                if (!pr.at_bol)
                    newline(&pr);
                while (pr.line + 1 < line) {
                    newline(&pr);
                    pr.line++;
                }
                pr.line = line;
            } else if (line > pr.line || t.kind == TK_PRAGMA || !pr.at_bol) {
                if (!(line < pr.line && !pr.at_bol && t.kind != TK_PRAGMA))
                    marker(&pr, f, line, 0);
            }
        }
        if (t.kind == TK_PRAGMA) {
            if (!pr.at_bol)
                newline(&pr);
            putch(&pr, '#');
            put(&pr, pp_text(pp, &t), t.len);
            newline(&pr);
            pr.line++;
            continue;
        }
        if (pr.at_bol) {
            if (t.flags & TF_SPACE)
                putch(&pr, ' ');
        } else if ((t.flags & (TF_SPACE | TF_BOL)) ||
                   (pr.have_prev && needs_space(&pr, &pr.prev, &t))) {
            putch(&pr, ' ');
        }
        put(&pr, pp_text(pp, &t), t.len);
        pr.at_bol = false;
        pr.prev = t;
        pr.have_prev = true;
    }
    if (!pr.at_bol)
        newline(&pr);
    flush(&pr);
    free(pr.buf);
}
