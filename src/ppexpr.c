/* ppexpr.c - #if / #elif controlling expressions (C99 6.10.1).
 *
 * All signed arithmetic is done in intmax_t and unsigned in uintmax_t
 * (6.10.1p3).  Subexpressions that are not evaluated (short-circuit, ?:)
 * are parsed but produce no diagnostics for division by zero/overflow. */
#include "pp.h"
#include "c/lit.h"

#include <string.h>

typedef struct Val {
    uintmax_t v;
    bool uns;
    SrcLoc loc;             /* where libcpp places the operand: its token or its operator */
} Val;

typedef struct EP {
    PP *pp;
    const Tok *t;           /* the token array ends with a TK_EOF sentinel */
    const Tok *start;       /* its first token */
    bool ok;
} EP;

#define TXT(p, t) pp_text((p)->pp, (t))

static Val expr_comma(EP *p, bool eval);

static Val mkval(uintmax_t v, bool uns)
{
    Val r;
    r.v = v;
    r.uns = uns;
    r.loc = 0;
    return r;
}

static intmax_t sv(Val a) { return (intmax_t)a.v; }

static void fail(EP *p, const Tok *t, const char *fmt, const char *arg, int n)
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
        p->t++;
}

/* gcc's operator table: a token that wants operands. */
static bool is_operator(const Tok *t)
{
    static const Punct ops[] = {P_STAR, P_SLASH, P_PERCENT, P_PLUS, P_MINUS,
        P_SHL, P_SHR, P_LT, P_GT, P_LE, P_GE, P_EQEQ, P_NE, P_AMP, P_CARET,
        P_PIPE, P_ANDAND, P_OROR, P_QUESTION, P_COLON, P_COMMA, P_RPAREN};
    size_t i;
    if (t->kind != TK_PUNCT)
        return false;
    for (i = 0; i < sizeof ops / sizeof *ops; i++)
        if (t->punct == ops[i])
            return true;
    return false;
}

/* A value was wanted and operator token t came instead (or the end): the
 * wording follows the operator before it (gcc's _cpp_parse_expr). */
static bool want_value_error(EP *p, const Tok *t)
{
    const Tok *prev = p->t > p->start ? p->t - 1 : NULL;
    bool lparen = prev && tok_is_punct(prev, P_LPAREN);
    if (t->kind == TK_EOF) {
        if (!prev) {
            fail(p, t, "#if with no expression", NULL, 0);
            return true;
        }
    } else if (!is_operator(t)) {
        return false;
    }
    if (!prev && tok_is_punct(t, P_RPAREN)) {
        fail(p, t, "missing '(' in expression", NULL, 0);
        return true;
    }
    if (lparen && t->kind == TK_EOF) {
        fail(p, prev, "missing ')' in expression", NULL, 0);
        return true;
    }
    if (prev && tok_is_punct(t, P_RPAREN) && lparen) {
        fail(p, t, "missing expression between '(' and ')'", NULL, 0);
        return true;
    }
    if (prev && !lparen && prev->kind == TK_PUNCT) {
        fail(p, t, "operator '%.*s' has no right operand", TXT(p, prev),
             (int)prev->len);
        return true;
    }
    if (t->kind != TK_EOF) {
        fail(p, t, "operator '%.*s' has no left operand", TXT(p, t),
             (int)t->len);
        return true;
    }
    return false;
}

static void overflow(EP *p, const Tok *op, bool eval)
{
    (void)op;
    /* libcpp: at the token that triggered the reduction */
    if (eval)
        pp_warn_at(p->pp, p->t, "integer-overflow-in-if",
                   "integer overflow in preprocessor expression");
}

/* ---- literals ------------------------------------------------------- */

static Val parse_number(EP *p, const Tok *t)
{
    const char *s = TXT(p, t), *end = s + t->len;
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
    } else if (end - s > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B') &&
               (s[2] == '0' || s[2] == '1')) {
        base = 2;
        s += 2;
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
        if (d >= base && base == 2) {
            StrBuf sb = {0};
            sb_putc(&sb, c);
            fail(p, t, "invalid digit \"%.*s\" in binary constant",
                 sb_cstr(&sb), 1);
            sb_free(&sb);
            return mkval(0, false);
        }
        if (d >= base) {        /* libcpp names the largest digit */
            char mx[2] = {c, 0};
            const char *qd;
            for (qd = s; qd < end && *qd >= '0' && *qd <= '9'; qd++)
                if (*qd > mx[0])
                    mx[0] = *qd;
            fail(p, t, "invalid digit \"%.*s\" in octal constant", mx, 1);
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
    if (nu > 0 && p->ok && diag_enabled(p->pp->diag, "traditional") &&
        !diag_hidden_in_system_header(p->pp->diag, t->loc)) {
        Tok u = *t;
        size_t k;
        for (k = 0; k + 1 < p->pp->if_exp.len; k += 2)   /* track0: where used */
            if (p->pp->if_exp.data[k] == t->loc)
                u.loc = p->pp->if_exp.data[k + 1];
        pp_warn_at(p->pp, &u, "traditional", "traditional C rejects the "
                   "\"%.*s\" suffix", (int)(end - s), s);
    }
    if (base == 2 && p->pp->opt->pedantic) {
        Tok u = *t;
        size_t k;
        for (k = 0; k + 1 < p->pp->if_exp.len; k += 2)   /* track0: where used */
            if (p->pp->if_exp.data[k] == t->loc)
                u.loc = p->pp->if_exp.data[k + 1];
        if (p->pp->diag->pedantic_errors)
            pp_error_at(p->pp, &u, "binary constants are a C2X feature or GCC "
                        "extension");
        else
            pp_warn_at(p->pp, &u, "", "binary constants are a C2X feature or "
                       "GCC extension");
    }
    if (overflowed) {
        pp_error_at(p->pp, t, "integer constant is too large for its type");
        uns = true;
    } else if (!uns && v > (uintmax_t)INTMAX_MAX) {
        /* libcpp CPP_DL_PEDWARN: an error under -pedantic-errors */
        if (p->pp->diag->pedantic_errors)
            pp_error_at(p->pp, t, "integer constant is so large that it is "
                                  "unsigned");
        else
            pp_warn_at(p->pp, t, "", "integer constant is so large that it "
                                     "is unsigned");
        uns = true;
    }
    return mkval(v, uns);
}


typedef struct PEsc {
    PP *pp;
    SrcLoc loc;
} PEsc;

static Val parse_char_(EP *p, const Tok *t);

static void esc_emit(void *ctx, int level, const char *msg)
{
    PEsc *e = ctx;
    if (level == 2)
        diag_report(e->pp->diag, DL_ERROR, "", e->loc, "%s", msg);
    else
        pp_pedwarn(e->pp, e->loc, "%s", msg);
}

static Val parse_char(EP *p, const Tok *t)
{
    PEsc esc;
    esc.pp = p->pp;
    esc.loc = t->loc;
    lit_escape_diags(TXT(p, t), t->len, p->pp->opt->pedantic, esc_emit, &esc);
    return parse_char_(p, t);
}

static Val parse_char_(EP *p, const Tok *t)
{
    const char *s = TXT(p, t), *end = s + t->len - 1;
    bool wide = false, uns = false;
    intmax_t v = 0;
    int nchars = 0;
    int ubits = 32;
    if (*s == 'L' || *s == 'u' || *s == 'U') {  /* u, U: gnu99/C11 */
        wide = true;
        uns = *s != 'L';
        ubits = *s == 'u' ? 16 : 32;
        s++;
    }
    s++; /* opening quote */
    while (s < end) {
        bool ucn;
        uint32_t c = lit_char_one(&s, end, wide, &ucn);
        nchars++;
        if (wide && uns)
            v = (intmax_t)(ubits == 16 ? (uint16_t)c : (uint32_t)c);
        else if (wide)
            v = (intmax_t)(int32_t)(uint32_t)c;
        else
            v = nchars == 1 ? (intmax_t)(signed char)(unsigned char)c
                            : (intmax_t)(int32_t)(((uint32_t)v << 8) |
                                                  (unsigned char)c);
    }
    if (nchars == 0)
        fail(p, t, "empty character constant", NULL, 0);
    else if (nchars > (wide ? 1 : 4))   /* libcpp: more than fits its type */
        diag_report(p->pp->diag, DL_WARNING, "", t->loc,
                    "character constant too long for its type");
    else if (nchars > 1)
        diag_report(p->pp->diag, DL_WARNING, "multichar", t->loc,
                    "multi-character character constant");
    return mkval((uintmax_t)v, uns);
}

/* ---- grammar -------------------------------------------------------- */

static Val primary(EP *p, bool eval)
{
    const Tok *t = p->t;
    if (!p->ok)
        return mkval(0, false);
    if (tok_is_punct(t, P_LPAREN)) {
        Val v;
        advance(p);
        v = expr_comma(p, eval);
        if (is_punct(p, P_COLON)) {
            fail(p, p->t, " ':' without preceding '?'", NULL, 0);
            return mkval(0, false);
        }
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
            return mkval(TXT(p, t)[0] == '1', false);
        return parse_number(p, t);
    case TK_CHAR:
        advance(p);
        return parse_char(p, t);
    case TK_IDENT:
        advance(p);
        /* C99 6.10.1p4: remaining identifiers are replaced with 0 */
        PP_EMIT(p->pp, macro_ref, pp_ident(p->pp, t), NULL, t, REF_IF_VALUE);
        {
            Diagnostic *d = diag_report(p->pp->diag, DL_WARNING, "undef",
                                        t->loc,
                                        "\"%s\" is not defined, evaluates to 0",
                                        pp_ident(p->pp, t)->str);
            diag_set_range(d, t->loc, t->loc + t->len);
        }
        if (tok_is_punct(p->t, P_LPAREN)) {
            fail(p, t, "function-like macro \"%.*s\" is not defined", TXT(p, t),
                 (int)t->len);
        }
        return mkval(0, false);
    case TK_EOF:
        if (!want_value_error(p, t))
            fail(p, t, "#if with no expression", NULL, 0);
        return mkval(0, false);
    case TK_STRING:
        fail(p, t, "token \"%.*s\" is not valid in preprocessor expressions",
             TXT(p, t), (int)t->len);
        return mkval(0, false);
    default:
        if (want_value_error(p, t))
            return mkval(0, false);
        if (t->kind == TK_PUNCT || t->kind == TK_OTHER)
            fail(p, t, "token \"%.*s\" is not valid in preprocessor expressions",
                 TXT(p, t), (int)t->len);
        else
            fail(p, t, "token is not valid in preprocessor expressions", NULL, 0);
        return mkval(0, false);
    }
}

static Val unary(EP *p, bool eval)
{
    const Tok *op = p->t;
    Val v;
    if (is_punct(p, P_PLUS)) {
        advance(p);
        v = unary(p, eval);
        v.loc = op->loc;
        if (eval && p->ok && diag_enabled(p->pp->diag, "traditional"))   /* at the lookahead */
            diag_report(p->pp->diag, DL_WARNING, "traditional", p->t->loc,
                        "traditional C rejects the unary plus operator");
        return v;
    }
    if (is_punct(p, P_MINUS)) {
        advance(p);
        v = unary(p, eval);
        if (!v.uns && sv(v) == INTMAX_MIN)
            overflow(p, op, eval);
        v.v = (uintmax_t)0 - v.v;
        v.loc = op->loc;
        return v;
    }
    if (is_punct(p, P_TILDE)) {
        advance(p);
        v = unary(p, eval);
        v.v = ~v.v;
        v.loc = op->loc;
        return v;
    }
    if (is_punct(p, P_BANG)) {
        advance(p);
        v = unary(p, eval);
        v = mkval(v.v == 0, false);
        v.loc = op->loc;
        return v;
    }
    v = primary(p, eval);
    if (!tok_is_punct(op, P_LPAREN))
        v.loc = op->loc;
    return v;
}

/* libcpp's check_promotion, for the operators it flags (* / % + - < > <= >=
 * and the arms of ?:; not == != or the bit operations): -Wall warns when the
 * signed operand is negative.  Skipped subexpressions warn too. */
static void convert(EP *p, const Tok *op, Val *a, Val *b)
{
    if (a->uns != b->uns && p->ok &&
        diag_enabled(p->pp->diag, "sign-promo-in-if")) {
        bool left = !a->uns;
        Val *neg = left ? a : b;
        if (sv(*neg) < 0)
            diag_report(p->pp->diag, DL_WARNING, "sign-promo-in-if", neg->loc,
                        "the %s operand of \"%.*s\" changes sign when promoted",
                        left ? "left" : "right", (int)op->len, TXT(p, op));
    }
    if (a->uns || b->uns)
        a->uns = b->uns = true;
}

static void convert_quiet(Val *a, Val *b)
{
    if (a->uns || b->uns)
        a->uns = b->uns = true;
}

static Val mul(EP *p, bool eval)
{
    Val a = unary(p, eval);
    for (;;) {
        const Tok *op = p->t;
        Val b;
        if (!is_punct(p, P_STAR) && !is_punct(p, P_SLASH) &&
            !is_punct(p, P_PERCENT))
            return a;
        advance(p);
        b = unary(p, eval);
        convert(p, op, &a, &b);
        a.loc = op->loc;
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
        const Tok *op = p->t;
        Val b;
        if (!is_punct(p, P_PLUS) && !is_punct(p, P_MINUS))
            return a;
        advance(p);
        b = mul(p, eval);
        convert(p, op, &a, &b);
        a.loc = op->loc;
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
        const Tok *op = p->t;
        Val b;
        intmax_t n;
        if (!is_punct(p, P_SHL) && !is_punct(p, P_SHR))
            return a;
        advance(p);
        b = add(p, eval);
        a.loc = op->loc;
        /* result has the type of the (promoted) left operand */
        n = b.uns ? (b.v > 64 ? 64 : (intmax_t)b.v) : sv(b);
        if (op->punct == P_SHR)
            n = -n;
        if (n >= 64 || n <= -64) {
            if (n >= 64 && !a.uns && a.v != 0)
                overflow(p, op, eval);
            a.v = (n < 0 && !a.uns && sv(a) < 0) ? UINTMAX_MAX : 0;
        } else if (n >= 0) {
            /* libcpp num_lshift: overflow if shifting back loses the value */
            if (!a.uns && (intmax_t)(a.v << n) >> n != sv(a))
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
        const Tok *op = p->t;
        Val b;
        bool r;
        if (!is_punct(p, P_LT) && !is_punct(p, P_GT) && !is_punct(p, P_LE) &&
            !is_punct(p, P_GE))
            return a;
        advance(p);
        b = shift(p, eval);
        convert(p, op, &a, &b);
        switch (op->punct) {
        case P_LT: r = a.uns ? a.v < b.v : sv(a) < sv(b); break;
        case P_GT: r = a.uns ? a.v > b.v : sv(a) > sv(b); break;
        case P_LE: r = a.uns ? a.v <= b.v : sv(a) <= sv(b); break;
        default: r = a.uns ? a.v >= b.v : sv(a) >= sv(b); break;
        }
        a = mkval(r, false);
        a.loc = op->loc;
    }
}

static Val equality(EP *p, bool eval)
{
    Val a = relational(p, eval);
    for (;;) {
        const Tok *op = p->t;
        Val b;
        if (!is_punct(p, P_EQEQ) && !is_punct(p, P_NE))
            return a;
        advance(p);
        b = relational(p, eval);
        convert_quiet(&a, &b);
        a = mkval((a.v == b.v) == (op->punct == P_EQEQ), false);
        a.loc = op->loc;
    }
}

#define BITOP(name, next, P, OP)                                             \
    static Val name(EP *p, bool eval)                                        \
    {                                                                        \
        Val a = next(p, eval);                                               \
        while (is_punct(p, P)) {                                             \
            const Tok *op = p->t;                                            \
            Val b;                                                           \
            advance(p);                                                      \
            b = next(p, eval);                                               \
            convert_quiet(&a, &b);                                           \
            a.v = a.v OP b.v;                                                \
            a.loc = op->loc;                                                 \
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
        const Tok *op = p->t;
        bool av = a.v != 0;
        advance(p);
        b = bor(p, eval && av);
        a = mkval(av && b.v != 0, false);
        a.loc = op->loc;
    }
    return a;
}

static Val lor(EP *p, bool eval)
{
    Val a = land(p, eval);
    while (is_punct(p, P_OROR)) {
        Val b;
        const Tok *op = p->t;
        bool av = a.v != 0;
        advance(p);
        b = land(p, eval && !av);
        a = mkval(av || b.v != 0, false);
        a.loc = op->loc;
    }
    return a;
}

static Val cond(EP *p, bool eval)
{
    Val c = lor(p, eval), x, y, r;
    const Tok *q = p->t, *colon;
    if (!is_punct(p, P_QUESTION))
        return c;
    advance(p);
    x = expr_comma(p, eval && c.v != 0);
    if (!is_punct(p, P_COLON)) {
        (void)q;
        fail(p, p->t, "'?' without following ':'", NULL, 0);
        return mkval(0, false);
    }
    colon = p->t;
    advance(p);
    y = cond(p, eval && c.v == 0);
    convert(p, colon, &x, &y);
    r = c.v ? x : y;
    r.loc = q->loc;
    return r;
}

static Val expr_comma(EP *p, bool eval)
{
    Val v = cond(p, eval);
    while (is_punct(p, P_COMMA)) {
        advance(p);
        v = cond(p, eval);
        if (eval && p->ok && p->pp->opt->pedantic)   /* at the token that ends the operand */
            diag_report(p->pp->diag,
                        p->pp->diag->pedantic_errors ? DL_ERROR : DL_WARNING,
                        "pedantic", p->t->loc,
                        "comma operator in operand of #if");
    }
    return v;
}

/* Replace `defined X` / `defined(X)` with 1/0 tokens; appends an EOF. */
static bool resolve_defined(PP *pp, TokSpan in, TokBuf *out)
{
    uint32_t i = 0;
    Tok eof;
    while (i < in.n) {
        const Tok *t = &in.t[i];
        if (t->kind == TK_IDENT && pp_ident(pp, t) == pp->id_defined) {
            const Tok *op = t, *name;
            Tok r;
            bool paren = false;
            i++;
            if (i < in.n && tok_is_punct(&in.t[i], P_LPAREN)) {
                paren = true;
                i++;
            }
            if (i >= in.n || in.t[i].kind != TK_IDENT) {
                pp_error_at(pp, op, "operator \"defined\" requires an identifier");
                return false;
            }
            name = &in.t[i++];
            if (paren) {
                if (i >= in.n || !tok_is_punct(&in.t[i], P_RPAREN)) {
                    pp_error_at(pp, op, "missing ')' after \"defined\"");
                    return false;
                }
                i++;
            }
            pp_macro_ref(pp, name, REF_DEFINED);
            r = pp_make_token(pp, TK_PPNUM,
                              pp_macro(pp, pp_ident(pp, name)) ? "1" : "0",
                              1, op->loc, (uint16_t)(op->flags | TF_FROM_DEFINED));
            tokbuf_push(pp, out, r);
            continue;
        }
        if (tok_is_punct(t, P_HASH)) { /* GCC assertion */
            Ident *pred;
            const char *answer;
            Tok r;
            if (pp->opt->pedantic)
                pp_pedwarn(pp, t->loc, "assertions are a GCC extension");
            else
                diag_report(pp->diag, DL_WARNING, "deprecated", t->loc,
                            "assertions are a deprecated extension");
            i++;
            /* GCC: a malformed assertion is reported and tests false */
            bool ok = pp_parse_assertion(pp, in, &i, false, t->loc + t->len,
                                         &pred, &answer);
            r = pp_make_token(pp, TK_PPNUM,
                              ok && pp_assertion_holds(pp, pred, answer)
                                  ? "1" : "0",
                              1, t->loc, t->flags);
            tokbuf_push(pp, out, r);
            continue;
        }
        tokbuf_push(pp, out, *t);
        i++;
    }
    memset(&eof, 0, sizeof eof);
    eof.kind = TK_EOF;
    eof.loc = in.n ? in.t[in.n - 1].loc + in.t[in.n - 1].len : 0;
    tokbuf_push(pp, out, eof);
    return true;
}

bool pp_eval_if(PP *pp, TokSpan expr, bool *ok)
{
    EP p;
    Val v;
    TokBuf exp = {0}, res = {0};
    TokSpan es;
    pp->in_if_expr = true;
    pp->if_exp.len = 0;
    tokbuf_init(pp, &exp, expr.n + 8);
    pp_expand_into(pp, expr, &exp);
    pp->in_if_expr = false;
    p.pp = pp;
    p.ok = true;
    es.t = exp.t;
    es.n = exp.len;
    tokbuf_init(pp, &res, exp.len + 1);
    if (!resolve_defined(pp, es, &res)) {
        tokbuf_release(pp, &exp);
        tokbuf_release(pp, &res);
        *ok = false;
        return false;
    }
    if (expr.n && res.len)      /* the end of the directive line, comments included */
        res.t[res.len - 1].loc =
            eol_after(pp, expr.t[expr.n - 1].loc + expr.t[expr.n - 1].len);
    p.t = p.start = res.t;
    v = expr_comma(&p, true);
    if (p.ok && p.t->kind != TK_EOF) {
        if (p.t->kind == TK_STRING ||
            (p.t->kind == TK_PUNCT && p.t->punct >= P_ASSIGN))
            fail(&p, p.t, "token \"%.*s\" is not valid in preprocessor "
                          "expressions", TXT(&p, p.t), (int)p.t->len);
        else if (tok_is_punct(p.t, P_RPAREN))
            fail(&p, p.t, "missing '(' in expression", NULL, 0);
        else if (tok_is_punct(p.t, P_COLON))
            fail(&p, p.t, " ':' without preceding '?'", NULL, 0);
        else
            fail(&p, p.t, "missing binary operator before token \"%.*s\"",
                 TXT(&p, p.t), (int)p.t->len);
    }
    tokbuf_release(pp, &exp);
    tokbuf_release(pp, &res);
    *ok = p.ok;
    return p.ok && v.v != 0;
}
