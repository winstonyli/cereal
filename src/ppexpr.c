/* ppexpr.c - #if / #elif controlling expressions (C99 6.10.1).
 *
 * All signed arithmetic is done in intmax_t and unsigned in uintmax_t
 * (6.10.1p3).  Subexpressions that are not evaluated (short-circuit, ?:)
 * are parsed but produce no diagnostics for division by zero/overflow. */
#include "pp.h"

#include <string.h>

typedef struct Val {
    uintmax_t v;
    bool uns;
} Val;

typedef struct EP {
    PP *pp;
    Token *t;
    bool ok;
} EP;

static Val expr_comma(EP *p, bool eval);

static Val mkval(uintmax_t v, bool uns)
{
    Val r;
    r.v = v;
    r.uns = uns;
    return r;
}

static intmax_t sv(Val a) { return (intmax_t)a.v; }

static void fail(EP *p, const Token *t, const char *fmt, const char *arg, int n)
{
    if (!p->ok)
        return;
    p->ok = false;
    if (arg)
        pp_error_at(p->pp, t, fmt, n, arg);
    else
        pp_error_at(p->pp, t, fmt);
}

static bool is_punct(EP *p, Punct k)
{
    return tok_is_punct(p->t, k);
}

static void advance(EP *p)
{
    if (p->t->kind != TK_EOF)
        p->t = p->t->next;
}

static void overflow(EP *p, const Token *op, bool eval)
{
    if (eval)
        pp_warn_at(p->pp, op, "integer-overflow-in-if",
                   "integer overflow in preprocessor expression");
}

/* ---- literals ------------------------------------------------------- */

static Val parse_number(EP *p, Token *t)
{
    const char *s = t->text, *end = t->text + t->len;
    uintmax_t v = 0;
    int base = 10;
    bool overflowed = false, uns = false;
    const char *q;
    int nl = 0, nu = 0;

    for (q = s; q < end; q++) {
        if (*q == '.' ||
            ((*q == 'e' || *q == 'E') && !(end - s > 1 && s[0] == '0' &&
                                          (s[1] == 'x' || s[1] == 'X'))) ||
            ((*q == 'p' || *q == 'P') && end - s > 1 && s[0] == '0' &&
             (s[1] == 'x' || s[1] == 'X'))) {
            fail(p, t, "floating constant in preprocessor expression", NULL, 0);
            return mkval(0, false);
        }
    }
    if (end - s > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        if (s == end || !((*s >= '0' && *s <= '9') ||
                          (*s >= 'a' && *s <= 'f') || (*s >= 'A' && *s <= 'F'))) {
            fail(p, t, "invalid hexadecimal constant", NULL, 0);
            return mkval(0, false);
        }
    } else if (s[0] == '0') {
        base = 8;
    }
    for (; s < end; s++) {
        int d;
        char c = *s;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            break;
        if (d >= base) {
            fail(p, t, "invalid digit in octal constant", NULL, 0);
            return mkval(0, false);
        }
        if (v > (UINTMAX_MAX - (uintmax_t)d) / (uintmax_t)base)
            overflowed = true;
        v = v * (uintmax_t)base + (uintmax_t)d;
    }
    /* suffix */
    for (q = s; q < end; q++) {
        if (*q == 'u' || *q == 'U')
            nu++;
        else if (*q == 'l' || *q == 'L') {
            if (q + 1 < end && q[1] == *q)
                q++;
            else if (q + 1 < end && (q[1] == 'l' || q[1] == 'L')) {
                nl = 9; /* lL mixed case */
                break;
            }
            nl++;
        } else {
            nl = 9;
            break;
        }
    }
    if (nu > 1 || nl > 1) {
        StrBuf sb = {0};
        sb_putn(&sb, s, (size_t)(end - s));
        fail(p, t, "invalid suffix \"%.*s\" on integer constant", sb_cstr(&sb),
             (int)sb.len);
        sb_free(&sb);
        return mkval(0, false);
    }
    uns = nu > 0;
    if (overflowed) {
        pp_error_at(p->pp, t, "integer constant is too large for its type");
        uns = true;
    } else if (!uns && v > (uintmax_t)INTMAX_MAX) {
        pp_warn_at(p->pp, t, "", "integer constant is so large that it is "
                                  "unsigned");
        uns = true;
    }
    return mkval(v, uns);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static Val parse_char(EP *p, Token *t)
{
    const char *s = t->text, *end = t->text + t->len - 1;
    bool wide = false;
    intmax_t v = 0;
    int nchars = 0;
    if (*s == 'L') {
        wide = true;
        s++;
    }
    s++; /* opening quote */
    while (s < end) {
        uintmax_t c;
        if (*s == '\\') {
            s++;
            switch (*s) {
            case 'n': c = '\n'; s++; break;
            case 't': c = '\t'; s++; break;
            case 'r': c = '\r'; s++; break;
            case 'a': c = 7; s++; break;
            case 'b': c = 8; s++; break;
            case 'f': c = 12; s++; break;
            case 'v': c = 11; s++; break;
            case 'e': case 'E': c = 27; s++; break;
            case 'x':
                s++;
                c = 0;
                while (s < end && hexval(*s) >= 0)
                    c = c * 16 + (uintmax_t)hexval(*s++);
                break;
            case 'u': case 'U': {
                int n = *s == 'u' ? 4 : 8;
                s++;
                c = 0;
                while (n-- > 0 && s < end && hexval(*s) >= 0)
                    c = c * 16 + (uintmax_t)hexval(*s++);
                break;
            }
            default:
                if (*s >= '0' && *s <= '7') {
                    int n = 0;
                    c = 0;
                    while (n++ < 3 && s < end && *s >= '0' && *s <= '7')
                        c = c * 8 + (uintmax_t)(*s++ - '0');
                } else {
                    c = (unsigned char)*s++;
                }
            }
        } else if (wide && ((unsigned char)*s) >= 0x80) {
            /* decode UTF-8 */
            unsigned char b = (unsigned char)*s++;
            int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
            c = b & (0x3F >> extra);
            while (extra-- > 0 && s < end)
                c = (c << 6) | ((unsigned char)*s++ & 0x3F);
        } else {
            c = (unsigned char)*s++;
        }
        nchars++;
        if (wide)
            v = (intmax_t)(int32_t)(uint32_t)c;
        else
            v = nchars == 1 ? (intmax_t)(signed char)(unsigned char)c
                            : (intmax_t)(int32_t)(((uint32_t)v << 8) |
                                                  (unsigned char)c);
    }
    if (nchars == 0)
        fail(p, t, "empty character constant", NULL, 0);
    else if (nchars > 1)
        diag_report(p->pp->diag, DL_WARNING, "", t->loc,
                    "multi-character character constant");
    return mkval((uintmax_t)v, false);
}

/* ---- grammar -------------------------------------------------------- */

static Val primary(EP *p, bool eval)
{
    Token *t = p->t;
    if (!p->ok)
        return mkval(0, false);
    if (tok_is_punct(t, P_LPAREN)) {
        Val v;
        advance(p);
        v = expr_comma(p, eval);
        if (!is_punct(p, P_RPAREN)) {
            fail(p, p->t->kind == TK_EOF ? t : p->t,
                 "missing ')' in expression", NULL, 0);
            return mkval(0, false);
        }
        advance(p);
        return v;
    }
    switch (t->kind) {
    case TK_PPNUM:
        advance(p);
        if (t->flags & TF_FROM_DEFINED)
            return mkval(t->text[0] == '1', false);
        return parse_number(p, t);
    case TK_CHAR:
        advance(p);
        return parse_char(p, t);
    case TK_IDENT:
        advance(p);
        /* C99 6.10.1p4: remaining identifiers are replaced with 0 */
        {
            Diagnostic *d = diag_report(p->pp->diag, DL_WARNING, "undef",
                                        t->loc,
                                        "\"%s\" is not defined, evaluates to 0",
                                        t->ident->str);
            diag_set_range(d, t->loc, t->loc + t->rawlen);
            pp_add_expansion_notes(p->pp, d, t);
        }
        if (tok_is_punct(p->t, P_LPAREN)) {
            fail(p, t, "function-like macro \"%.*s\" is not defined", t->text,
                 (int)t->len);
        }
        return mkval(0, false);
    case TK_EOF:
        fail(p, t, "#if with no expression", NULL, 0);
        return mkval(0, false);
    case TK_STRING:
        fail(p, t, "token is not valid in preprocessor expressions", NULL, 0);
        return mkval(0, false);
    default:
        if (t->kind == TK_PUNCT || t->kind == TK_OTHER)
            fail(p, t, "token \"%.*s\" is not valid in preprocessor expressions",
                 t->text, (int)t->len);
        else
            fail(p, t, "token is not valid in preprocessor expressions", NULL, 0);
        return mkval(0, false);
    }
}

static Val unary(EP *p, bool eval)
{
    Token *op = p->t;
    Val v;
    if (is_punct(p, P_PLUS)) {
        advance(p);
        return unary(p, eval);
    }
    if (is_punct(p, P_MINUS)) {
        advance(p);
        v = unary(p, eval);
        if (!v.uns && sv(v) == INTMAX_MIN)
            overflow(p, op, eval);
        v.v = (uintmax_t)0 - v.v;
        return v;
    }
    if (is_punct(p, P_TILDE)) {
        advance(p);
        v = unary(p, eval);
        v.v = ~v.v;
        return v;
    }
    if (is_punct(p, P_BANG)) {
        advance(p);
        v = unary(p, eval);
        return mkval(v.v == 0, false);
    }
    return primary(p, eval);
}

static void convert(Val *a, Val *b)
{
    if (a->uns || b->uns)
        a->uns = b->uns = true;
}

static Val mul(EP *p, bool eval)
{
    Val a = unary(p, eval);
    for (;;) {
        Token *op = p->t;
        Val b;
        if (!is_punct(p, P_STAR) && !is_punct(p, P_SLASH) &&
            !is_punct(p, P_PERCENT))
            return a;
        advance(p);
        b = unary(p, eval);
        convert(&a, &b);
        if (op->punct == P_STAR) {
            if (!a.uns) {
                intmax_t x = sv(a), y = sv(b);
                if (x != 0 && y != 0 &&
                    ((x == -1 && y == INTMAX_MIN) || (y == -1 && x == INTMAX_MIN) ||
                     (x != -1 && y != -1 &&
                      ((x > 0 && y > 0 && x > INTMAX_MAX / y) ||
                       (x < 0 && y < 0 && x < INTMAX_MAX / y) ||
                       (x > 0 && y < 0 && y < INTMAX_MIN / x) ||
                       (x < 0 && y > 0 && x < INTMAX_MIN / y)))))
                    overflow(p, op, eval);
            }
            a.v = a.v * b.v;
        } else {
            if (b.v == 0) {
                if (eval)
                    fail(p, op, "division by zero in #if", NULL, 0);
                a.v = 0;
                continue;
            }
            if (a.uns) {
                a.v = op->punct == P_SLASH ? a.v / b.v : a.v % b.v;
            } else {
                intmax_t x = sv(a), y = sv(b);
                if (x == INTMAX_MIN && y == -1) {
                    overflow(p, op, eval);
                    a.v = op->punct == P_SLASH ? (uintmax_t)x : 0;
                } else {
                    a.v = (uintmax_t)(op->punct == P_SLASH ? x / y : x % y);
                }
            }
        }
    }
}

static Val add(EP *p, bool eval)
{
    Val a = mul(p, eval);
    for (;;) {
        Token *op = p->t;
        Val b;
        if (!is_punct(p, P_PLUS) && !is_punct(p, P_MINUS))
            return a;
        advance(p);
        b = mul(p, eval);
        convert(&a, &b);
        if (op->punct == P_PLUS) {
            if (!a.uns && ((sv(b) > 0 && sv(a) > INTMAX_MAX - sv(b)) ||
                           (sv(b) < 0 && sv(a) < INTMAX_MIN - sv(b))))
                overflow(p, op, eval);
            a.v = a.v + b.v;
        } else {
            if (!a.uns && ((sv(b) < 0 && sv(a) > INTMAX_MAX + sv(b)) ||
                           (sv(b) > 0 && sv(a) < INTMAX_MIN + sv(b))))
                overflow(p, op, eval);
            a.v = a.v - b.v;
        }
    }
}

static Val shift(EP *p, bool eval)
{
    Val a = add(p, eval);
    for (;;) {
        Token *op = p->t;
        Val b;
        intmax_t n;
        if (!is_punct(p, P_SHL) && !is_punct(p, P_SHR))
            return a;
        advance(p);
        b = add(p, eval);
        /* result has the type of the (promoted) left operand */
        n = b.uns ? (b.v > 64 ? 64 : (intmax_t)b.v) : sv(b);
        if (op->punct == P_SHR)
            n = -n;
        if (n >= 64 || n <= -64) {
            if (n >= 64 && !a.uns && a.v != 0)
                overflow(p, op, eval);
            a.v = (n < 0 && !a.uns && sv(a) < 0) ? UINTMAX_MAX : 0;
        } else if (n >= 0) {
            if (!a.uns && (sv(a) < 0 || (n > 0 && (a.v >> (63 - n)) != 0)))
                overflow(p, op, eval);
            a.v <<= n;
        } else if (a.uns) {
            a.v >>= -n;
        } else {
            a.v = (uintmax_t)(sv(a) >> -n); /* arithmetic (GCC) */
        }
    }
}

static Val relational(EP *p, bool eval)
{
    Val a = shift(p, eval);
    for (;;) {
        Token *op = p->t;
        Val b;
        bool r;
        if (!is_punct(p, P_LT) && !is_punct(p, P_GT) && !is_punct(p, P_LE) &&
            !is_punct(p, P_GE))
            return a;
        advance(p);
        b = shift(p, eval);
        convert(&a, &b);
        switch (op->punct) {
        case P_LT: r = a.uns ? a.v < b.v : sv(a) < sv(b); break;
        case P_GT: r = a.uns ? a.v > b.v : sv(a) > sv(b); break;
        case P_LE: r = a.uns ? a.v <= b.v : sv(a) <= sv(b); break;
        default: r = a.uns ? a.v >= b.v : sv(a) >= sv(b); break;
        }
        a = mkval(r, false);
    }
}

static Val equality(EP *p, bool eval)
{
    Val a = relational(p, eval);
    for (;;) {
        Token *op = p->t;
        Val b;
        if (!is_punct(p, P_EQEQ) && !is_punct(p, P_NE))
            return a;
        advance(p);
        b = relational(p, eval);
        convert(&a, &b);
        a = mkval((a.v == b.v) == (op->punct == P_EQEQ), false);
    }
}

#define BITOP(name, next, P, OP)                                             \
    static Val name(EP *p, bool eval)                                        \
    {                                                                        \
        Val a = next(p, eval);                                               \
        while (is_punct(p, P)) {                                             \
            Val b;                                                           \
            advance(p);                                                      \
            b = next(p, eval);                                               \
            convert(&a, &b);                                                 \
            a.v = a.v OP b.v;                                                \
        }                                                                    \
        return a;                                                            \
    }

BITOP(band, equality, P_AMP, &)
BITOP(bxor, band, P_CARET, ^)
BITOP(bor, bxor, P_PIPE, |)

static Val land(EP *p, bool eval)
{
    Val a = bor(p, eval);
    while (is_punct(p, P_ANDAND)) {
        Val b;
        bool av = a.v != 0;
        advance(p);
        b = bor(p, eval && av);
        a = mkval(av && b.v != 0, false);
    }
    return a;
}

static Val lor(EP *p, bool eval)
{
    Val a = land(p, eval);
    while (is_punct(p, P_OROR)) {
        Val b;
        bool av = a.v != 0;
        advance(p);
        b = land(p, eval && !av);
        a = mkval(av || b.v != 0, false);
    }
    return a;
}

static Val cond(EP *p, bool eval)
{
    Val c = lor(p, eval), x, y;
    Token *q = p->t;
    if (!is_punct(p, P_QUESTION))
        return c;
    advance(p);
    x = expr_comma(p, eval && c.v != 0);
    if (!is_punct(p, P_COLON)) {
        fail(p, p->t->kind == TK_EOF ? q : p->t, "'?' without following ':'",
             NULL, 0);
        return mkval(0, false);
    }
    advance(p);
    y = cond(p, eval && c.v == 0);
    convert(&x, &y);
    return c.v ? x : y;
}

static Val expr_comma(EP *p, bool eval)
{
    Val v = cond(p, eval);
    while (is_punct(p, P_COMMA)) {
        if (eval && p->pp->opt->pedantic)
            diag_report(p->pp->diag, DL_WARNING, "pedantic", p->t->loc,
                        "comma operator in operand of #if");
        advance(p);
        v = cond(p, eval);
    }
    return v;
}

/* Replace `defined X` / `defined(X)` with 1/0 tokens. */
static Token *resolve_defined(PP *pp, Token *list, bool *ok)
{
    Token head, *tail = &head, *t = list;
    head.next = NULL;
    while (t->kind != TK_EOF) {
        if (t->kind == TK_IDENT && t->ident == pp->id_defined) {
            Token *op = t, *name, *r;
            bool paren = false;
            t = t->next;
            if (tok_is_punct(t, P_LPAREN)) {
                paren = true;
                t = t->next;
            }
            if (t->kind != TK_IDENT) {
                pp_error_at(pp, op, "operator \"defined\" requires an identifier");
                *ok = false;
                break;
            }
            name = t;
            t = t->next;
            if (paren) {
                if (!tok_is_punct(t, P_RPAREN)) {
                    pp_error_at(pp, op, "missing ')' after \"defined\"");
                    *ok = false;
                    break;
                }
                t = t->next;
            }
            pp_macro_ref(pp, name, REF_DEFINED);
            r = pp_copy_token(pp, op);
            r->kind = TK_PPNUM;
            r->text = name->ident->macro ? "1" : "0";
            r->len = 1;
            r->ident = NULL;
            r->flags |= TF_FROM_DEFINED;
            tail = tail->next = r;
            continue;
        }
        tail = tail->next = t;
        t = t->next;
    }
    tail->next = t;
    return head.next;
}

bool pp_eval_if(PP *pp, Token *expr, SrcLoc loc, bool *ok)
{
    EP p;
    Val v;
    Token *list;
    (void)loc;
    pp->in_if_expr = true;
    list = pp_expand_list(pp, expr);
    pp->in_if_expr = false;
    p.pp = pp;
    p.ok = true;
    list = resolve_defined(pp, list, &p.ok);
    if (!p.ok) {
        *ok = false;
        return false;
    }
    p.t = list;
    v = expr_comma(&p, true);
    if (p.ok && p.t->kind != TK_EOF) {
        if (tok_is_punct(p.t, P_ASSIGN) || p.t->punct >= P_ASSIGN)
            fail(&p, p.t, "token \"%.*s\" is not valid in preprocessor "
                          "expressions", p.t->text, (int)p.t->len);
        else
            fail(&p, p.t, "missing binary operator before token \"%.*s\"",
                 p.t->text, (int)p.t->len);
    }
    *ok = p.ok;
    return p.ok && v.v != 0;
}
