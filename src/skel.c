/* skel.c - per-file directive skeleton. */
#include "skel.h"

#include <string.h>

static SkKind classify(const Token *kw)
{
    static const struct {
        const char *s;
        SkKind k;
    } tab[] = {{"if", SK_IF},         {"ifdef", SK_IFDEF},
               {"ifndef", SK_IFNDEF}, {"elif", SK_ELIF},
               {"else", SK_ELSE},     {"endif", SK_ENDIF},
               {"define", SK_DEFINE}, {"undef", SK_UNDEF},
               {"include", SK_INCLUDE}, {"include_next", SK_INCLUDE}};
    size_t i;
    if (kw->kind != TK_IDENT)
        return SK_OTHER;
    for (i = 0; i < ARRAY_LEN(tab); i++)
        if (tok_is_ident(kw, tab[i].s))
            return tab[i].k;
    return SK_OTHER;
}

/* Text of a comment following the last token of a directive line. */
static const char *trailing_comment(Arena *a, SrcFile *f, SrcLoc after)
{
    uint32_t off = after - f->base, e = off;
    const char *s, *p;
    while (e < f->size && f->buf[e] != '\n')
        e++;
    s = f->buf + off;
    for (p = s; p + 1 < f->buf + e; p++) {
        if (p[0] == '/' && (p[1] == '*' || p[1] == '/')) {
            const char *b = p + 2, *q = f->buf + e;
            if (p[1] == '*') {
                const char *end = strstr(b, "*/");
                if (end && end < q)
                    q = end;
            }
            return arena_strndup(a, b, (size_t)(q - b));
        }
    }
    return NULL;
}

Skeleton *skel_get(Arena *a, Interner *in, LexOptions lo, SrcFile *f)
{
    Skeleton *sk;
    Token *t;
    VEC(int) stack = {0};
    if (f->skel)
        return f->skel;
    sk = NEW(a, Skeleton);
    sk->file = f;
    sk->guard_ifndef_index = -1;
    t = lex_file(a, in, NULL, lo, f);
    while (t->kind != TK_EOF) {
        if ((t->flags & TF_BOL) && tok_is_punct(t, P_HASH)) {
            Token *hash = t, *kw = t->next, *first, *last;
            SkDirective d;
            memset(&d, 0, sizeof d);
            d.hash_loc = hash->loc;
            d.opener = -1;
            if (kw->flags & TF_BOL || kw->kind == TK_EOF) {
                t = kw;
                continue; /* null directive */
            }
            d.kind = classify(kw);
            d.kw_loc = kw->loc;
            /* cut the line */
            first = kw->next;
            last = kw;
            for (t = first; !(t->flags & TF_BOL) && t->kind != TK_EOF; t = t->next)
                last = t;
            d.end_loc = last->loc + last->rawlen;
            if (first == t) {
                d.toks = NEW(a, Token);
                d.toks->kind = TK_EOF;
                d.toks->loc = d.end_loc;
                d.toks->text = "";
            } else {
                Token *eof = NEW(a, Token);
                eof->kind = TK_EOF;
                eof->loc = d.end_loc;
                eof->text = "";
                last->next = eof;
                d.toks = first;
            }
            if ((d.kind == SK_IFDEF || d.kind == SK_IFNDEF ||
                 d.kind == SK_DEFINE || d.kind == SK_UNDEF) &&
                d.toks->kind == TK_IDENT)
                d.name = d.toks->ident;
            if (d.kind == SK_ELSE || d.kind == SK_ENDIF)
                d.comment = trailing_comment(a, f, d.end_loc);
            switch (d.kind) {
            case SK_IF: case SK_IFDEF: case SK_IFNDEF:
                d.depth = (int)stack.len;
                vec_push(&stack, (int)sk->dirs.len);
                break;
            case SK_ELIF: case SK_ELSE:
                d.depth = stack.len ? (int)stack.len - 1 : 0;
                d.opener = stack.len ? vec_last(&stack) : -1;
                break;
            case SK_ENDIF:
                d.depth = stack.len ? (int)stack.len - 1 : 0;
                d.opener = stack.len ? vec_pop(&stack) : -1;
                break;
            default:
                d.depth = (int)stack.len;
            }
            vec_push(&sk->dirs, d);
            continue;
        }
        sk->ntokens++;
        if (stack.len == 0)
            sk->ntokens_toplevel++;
        t = t->next;
    }
    vec_free(&stack);

    /* guard shape */
    if (sk->dirs.len >= 2) {
        SkDirective *d0 = &sk->dirs.data[0], *d1 = &sk->dirs.data[1];
        Ident *g = NULL;
        if (d0->kind == SK_IFNDEF)
            g = d0->name;
        else if (d0->kind == SK_IF && tok_is_punct(d0->toks, P_BANG) &&
                 tok_is_ident(d0->toks->next, "defined")) {
            Token *x = d0->toks->next->next;
            if (tok_is_punct(x, P_LPAREN))
                x = x->next;
            if (x->kind == TK_IDENT)
                g = x->ident;
        }
        if (g) {
            size_t i;
            sk->guard_ifndef = g;
            sk->guard_ifndef_index = 0;
            if (d1->kind == SK_DEFINE && d1->depth == 1)
                sk->guard_define = d1->name;
            /* covers the file: the matching #endif is the last directive
             * and no tokens are at top level */
            for (i = 1; i < sk->dirs.len; i++)
                if (sk->dirs.data[i].kind == SK_ENDIF &&
                    sk->dirs.data[i].opener == 0)
                    break;
            sk->guard_covers_file = i == sk->dirs.len - 1 &&
                                    sk->ntokens_toplevel == 0;
        }
    }
    f->skel = sk;
    return sk;
}
