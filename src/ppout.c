/* ppout.c - `-E` output: GCC-compatible preprocessed text with linemarkers. */
#include "ppout.h"

#include <string.h>

typedef struct Printer {
    PP *pp;
    FILE *out;
    SrcFile *file;         /* file of the last printed line */
    uint32_t line;         /* presumed line of the output cursor */
    bool at_bol;
    int pending_flag;      /* 1 = entered, 2 = returned */
    bool linemarkers;
    Token *prev;
} Printer;

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

static bool needs_space(const Token *a, const Token *b)
{
    int x, y;
    static const char *const pairs[] = {
        "++", "--", "->", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
        "*=", "/=", "%=", "+=", "-=", "&=", "^=", "|=", "##", "<:", ":>",
        "<%", "%>", "%:", "..", "//", "/*", NULL};
    int i;
    if (!a || a->len == 0 || b->len == 0)
        return false;
    x = (unsigned char)a->text[a->len - 1];
    y = (unsigned char)b->text[0];
    if ((a->kind == TK_IDENT || a->kind == TK_PPNUM) &&
        (b->kind == TK_IDENT || b->kind == TK_PPNUM))
        return true;
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
    fputc('\n', p->out);
    p->at_bol = true;
    p->prev = NULL;
}

static void marker(Printer *p, SrcFile *f, uint32_t line, int flag)
{
    const char *name = f->name;
    size_t i;
    if (!p->at_bol)
        newline(p);
    if (!p->linemarkers) {
        p->file = f;
        p->line = line;
        return;
    }
    {
        IncludeFrame *fr;
        for (fr = p->pp->inc; fr; fr = fr->prev)
            if (fr->file == f) {
                name = fr->presumed_name;
                break;
            }
    }
    fprintf(p->out, "# %u \"", line);
    for (i = 0; name[i]; i++) {
        if (name[i] == '"' || name[i] == '\\')
            fputc('\\', p->out);
        fputc(name[i], p->out);
    }
    fputc('"', p->out);
    if (flag)
        fprintf(p->out, " %d", flag);
    if (f->system_header)
        fputs(" 3", p->out);
    newline(p);
    p->file = f;
    p->line = line;
}

void pp_write_output(PP *pp, FILE *out, bool linemarkers)
{
    Printer pr;
    PPListener l;
    memset(&pr, 0, sizeof pr);
    memset(&l, 0, sizeof l);
    pr.pp = pp;
    pr.out = out;
    pr.at_bol = true;
    pr.linemarkers = linemarkers;
    l.ctx = &pr;
    l.file_enter = on_enter;
    l.file_exit = on_exit;
    pp_add_listener(pp, l);

    for (;;) {
        Token *t = pp_next(pp);
        SrcLoc loc;
        SrcFile *f;
        uint32_t line;
        if (t->kind == TK_EOF)
            break;
        loc = t->kind == TK_PRAGMA ? t->loc : pp_expansion_loc(t);
        f = srcmgr_file_of(pp->sm, loc);
        line = f ? pp_presumed_line(pp, loc) : pr.line;
        if (f && f->kind == SF_VIRTUAL && f != pp->builtin_file) {
            /* _Pragma or tokens from the command line */
        }
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
            } else if (line > pr.line || t->kind == TK_PRAGMA || !pr.at_bol) {
                if (line < pr.line && !pr.at_bol && t->kind != TK_PRAGMA) {
                    /* backwards within an invocation spanning lines: stay */
                    goto same_line;
                }
                marker(&pr, f, line, 0);
            }
        }
    same_line:
        if (t->kind == TK_PRAGMA) {
            if (!pr.at_bol)
                newline(&pr);
            fprintf(out, "#%.*s", (int)t->len, t->text);
            newline(&pr);
            pr.line++;
            continue;
        }
        if (pr.at_bol) {
            if (t->flags & TF_SPACE)
                fputc(' ', out);
        } else if ((t->flags & (TF_SPACE | TF_BOL)) || needs_space(pr.prev, t)) {
            fputc(' ', out);
        }
        fwrite(t->text, 1, t->len, out);
        pr.at_bol = false;
        pr.prev = t;
    }
    if (!pr.at_bol)
        newline(&pr);
}
