/* skel.c - per-file directive skeleton. */
#include "skel.h"

#include <string.h>

static SkKind classify(Interner *in, const Tok *kw)
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
    for (i = 0; i < ARRAY_LEN(tab); i++)
        if (tok_is_word(in, kw, tab[i].s))
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

Skeleton *skel_get(Arena *a, SrcMgr *sm, Interner *in, LexOptions lo,
                   SrcFile *f)
{
    Skeleton *sk;
    Lexer L;
    Tok t;
    VEC(int) stack = {0};
    VEC(Tok) line = {0};
    bool have = false;
    if (f->skel)
        return f->skel;
    sk = NEW(a, Skeleton);
    sk->file = f;
    sk->guard_ifndef_index = -1;
    lexer_init(&L, sm, in, NULL, &sm->scratch, lo, f); /* phase A only */
    for (;;) {
        if (!have)
            lex_next(&L, &t);
        have = false;
        if (t.kind == TK_EOF)
            break;
        if ((t.flags & TF_BOL) && tok_is_punct(&t, P_HASH)) {
            SkDirective d;
            Tok kw;
            memset(&d, 0, sizeof d);
            d.hash_loc = t.loc;
            d.opener = -1;
            lex_next(&L, &kw);
            if ((kw.flags & TF_BOL) || kw.kind == TK_EOF) {
                t = kw;
                have = true;
                continue; /* null directive */
            }
            d.kind = classify(in, &kw);
            d.kw_loc = kw.loc;
            d.end_loc = kw.loc + kw.len;
            line.len = 0;
            for (;;) {
                lex_next(&L, &t);
                if ((t.flags & TF_BOL) || t.kind == TK_EOF)
                    break;
                vec_push(&line, t);
                d.end_loc = t.loc + t.len;
            }
            have = true;
            d.toks.n = (uint32_t)line.len;
            d.toks.t = NEW_ARRAY(a, Tok, line.len + 1);
            if (line.len)
                memcpy((Tok *)d.toks.t, line.data, sizeof(Tok) * line.len);
            if ((d.kind == SK_IFDEF || d.kind == SK_IFNDEF ||
                 d.kind == SK_DEFINE || d.kind == SK_UNDEF) &&
                d.toks.n && d.toks.t[0].kind == TK_IDENT)
                d.name = tok_ident(in, &d.toks.t[0]);
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
    }
    vec_free(&stack);
    vec_free(&line);
    lexer_free(&L);
    {
        /* move the directives into the arena (skeletons live with the TU) */
        SkDirective *d = NEW_ARRAY(a, SkDirective, sk->dirs.len + 1);
        if (sk->dirs.len)
            memcpy(d, sk->dirs.data, sizeof *d * sk->dirs.len);
        free(sk->dirs.data);
        sk->dirs.data = d;
        sk->dirs.cap = sk->dirs.len;
    }

    /* guard shape */
    if (sk->dirs.len >= 2) {
        SkDirective *d0 = &sk->dirs.data[0], *d1 = &sk->dirs.data[1];
        Ident *g = NULL;
        if (d0->kind == SK_IFNDEF) {
            g = d0->name;
        } else if (d0->kind == SK_IF && d0->toks.n >= 3 &&
                   tok_is_punct(&d0->toks.t[0], P_BANG) &&
                   tok_is_word(in, &d0->toks.t[1], "defined")) {
            const Tok *x = &d0->toks.t[2];
            if (tok_is_punct(x, P_LPAREN) && d0->toks.n >= 4)
                x++;
            if (x->kind == TK_IDENT)
                g = tok_ident(in, x);
        }
        if (g) {
            size_t i;
            sk->guard_ifndef = g;
            sk->guard_ifndef_index = 0;
            if (d1->kind == SK_DEFINE && d1->depth == 1)
                sk->guard_define = d1->name;
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
