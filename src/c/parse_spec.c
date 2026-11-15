/* parse_spec.c - the parser's spec reader: `#pragma cereal requires|ensures|
 * invariant EXPR` (docs/SPEC_DIRECTIVES_DESIGN.md).  Split out of parse.c; it
 * reaches the parser's node and error helpers through parse_int.h. */
#include "c/parse_int.h"

#include <ctype.h>
#include <string.h>

#include "lex.h"

/* ---- specs: #pragma cereal requires|ensures|invariant EXPR ------------
 * docs/SPEC_DIRECTIVES_DESIGN.md.  The expression is lexed from the
 * pragma's own source text, spliced into the token stream behind the
 * pragma token and parsed in place; the node is the pragma, with the
 * expression as its child and the spec kind in aux. */


const char *const spec_kw[] = {"", "requires", "ensures", "invariant"};

static bool blank(char c)
{
    return c == ' ' || c == '\t';
}

static bool ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || c == '$';
}

/* s is at word w (not a prefix of a longer word): step over it and the
 * blanks after it. */
static bool take_word(const char **s, const char *e, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(e - *s) < n || strncmp(*s, w, n) ||
        ((size_t)(e - *s) > n && ident_char((*s)[n])))
        return false;
    *s += n;
    while (*s < e && blank(**s))
        (*s)++;
    return true;
}

/* Past blanks, comments and line splices in the raw source. */
static const char *trivia(const char *q)
{
    for (;;) {
        if (blank(*q))
            q++;
        else if (q[0] == '\\' && q[1] == '\n')
            q += 2;
        else if (q[0] == '\\' && q[1] == '\r' && q[2] == '\n')
            q += 3;
        else if (q[0] == '/' && q[1] == '*') {
            const char *e = strstr(q + 2, "*/");
            if (!e)
                return q;
            q = e + 2;
        } else
            return q;
    }
}

/* The spec a TK_PRAGMA token is (SPEC_*), 0 for any other pragma; *kw is
 * where its keyword starts in the source line (the expression follows it)
 * or NULL when the source is not a plain '#pragma' line. */
int spec_kind(Parser *p, const Tok *t, const char **kw)
{
    const char *s = tok_text_raw(p->sm, p->in, t), *e = s + t->len;
    const char *r = srcmgr_ptr(p->sm, t->loc);
    int k;
    while (s < e && blank(*s))
        s++;
    (void)take_word(&s, e, "pragma");
    if (!take_word(&s, e, "cereal"))
        return 0;
    for (k = SPEC_REQUIRES; k <= SPEC_INVARIANT; k++)
        if (take_word(&s, e, spec_kw[k]))
            break;
    if (k > SPEC_INVARIANT)
        k = SPEC_UNKNOWN;
    *kw = NULL;
    if (*r == '#') {
        const char *q = trivia(r + 1);
        if (take_word(&q, q + (1 << 20), "pragma") && (q = trivia(q), true) &&
            take_word(&q, q + (1 << 20), "cereal"))
            *kw = trivia(q);
    }
    return k;
}

/* The pragma at toks[pt] is a spec of kind k in a body of kind ctx (SCTX_*). */
void spec_pragma(Parser *p, uint32_t pt, int k, const char *kw, int ctx)
{
    SrcLoc loc = p->toks.data[pt].t.loc;
    const char *r = srcmgr_ptr(p->sm, loc);
    if (k == SPEC_UNKNOWN || !kw) {
        p_err_at(p, pt, kw ? loc + (SrcLoc)(kw - r) : loc,
                k == SPEC_UNKNOWN ? "unknown '#pragma cereal' directive"
                                  : "a spec must be written as '#pragma cereal'");
        p->err.live = false;
    } else if (ctx != (k == SPEC_INVARIANT ? SCTX_LOOP : SCTX_FUNC)) {
        p_err_at(p, pt, loc + (SrcLoc)(kw - r), "'#pragma cereal %s' must start a %s body",
                spec_kw[k], k == SPEC_INVARIANT ? "loop" : "function");
        p->err.live = false;
    } else {
        const char *a = kw + strlen(spec_kw[k]), *q;
        Lexer L;
        LexOptions o;
        Tok t;
        VEC(PTok) v = {0};
        size_t i, n, at = pt + 1, old = p->toks.len;
        uint32_t start;
        PTok eof;
        while (blank(*a))
            a++;
        q = a;
        while (*q && *q != '\n') {
            if (*q == '\\' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n')))
                q += q[1] == '\n' ? 2 : 3;
            else
                q++;
        }
        memset(&o, 0, sizeof o);
        o.dollar_idents = true;     /* $len */
        o.uliterals = true;
        o.c23 = p->year >= 2023;
        lexer_init_range(&L, p->sm, p->in, &p->spec_cur, o,
                         loc + (SrcLoc)(a - r), (uint32_t)(q - a));
        for (lex_next(&L, &t); t.kind != TK_EOF; lex_next(&L, &t)) {
            PTok x;
            memset(&x, 0, sizeof x);
            x.t = t;
            x.t.flags &= (uint8_t)~TF_BOL;
            vec_push(&v, x);
        }
        lexer_free(&L);
        memset(&eof, 0, sizeof eof);
        eof.t.kind = TK_EOF;
        eof.t.loc = loc + (SrcLoc)(q - r);
        vec_push(&v, eof);
        n = v.len;
        for (i = 0; i < n; i++)
            vec_push(&p->toks, p->toks.data[pt]);
        memmove(&p->toks.data[at + n], &p->toks.data[at], (old - at) * sizeof(PTok));
        memcpy(&p->toks.data[at], v.data, n * sizeof(PTok));
        vec_free(&v);
        start = p_nmark(p);
        p->pos = (uint32_t)at;
        p->in_spec = true;
        parse_expr(p);
        p->in_spec = false;
        p->err.live = false;        /* a bad spec does not hush the next one */
        if (!p_at_eof(p)) {
            char buf[160];
            p_err(p, p_ci(p), "junk at end of '#pragma cereal %s'%s", spec_kw[k],
                 p_tok_desc(p, p_ci(p), buf, sizeof buf));
            p->err.live = false;
        }
        p->pos = (uint32_t)(at + n);    /* past the sentinel */
        p_emit(p, N_PRAGMA, pt, start, 0);
        p_set_aux(p, (uint32_t)k);
        return;
    }
    p_leaf(p, N_PRAGMA, pt);
    p->pos = pt + 1;
}


/* adv stepped over tokens [from, to): a spec among them sits in no body's
 * first slot, and is an error, not a silent skip. */
void spec_skipped(Parser *p, uint32_t from, uint32_t to)
{
    uint32_t i;
    for (i = from; i < to; i++) {
        const char *kw, *r;
        int k = spec_kind(p, &p->toks.data[i].t, &kw);
        if (!k || !kw)
            continue;
        r = srcmgr_ptr(p->sm, p->toks.data[i].t.loc);
        if (k == SPEC_UNKNOWN)
            p_err_at(p, i, p->toks.data[i].t.loc + (SrcLoc)(kw - r),
                     "unknown '#pragma cereal' directive");
        else
            p_err_at(p, i, p->toks.data[i].t.loc + (SrcLoc)(kw - r),
                     "'#pragma cereal %s' must start a %s body", spec_kw[k],
                     k == SPEC_INVARIANT ? "loop" : "function");
        p->err.live = false;
    }
}
