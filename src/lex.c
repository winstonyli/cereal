/* lex.c - translation phases 1-3: streaming pp-token lexer.
 *
 * Fast path: byte-class tables and tight loops over the raw buffer; valid
 * whenever a token contains no line splice or trigraph, i.e. almost always.
 * Anything that sees '\\' or (with -trigraphs) '?' inside a token restarts
 * that token on the slow path, which reads logical characters and records
 * a cleaned spelling in the scratch area.
 *
 * All reads may run past `lim` into the zero padding that follows every
 * buffer in the location space; a 0 byte at or after `lim` is end of input. */
#include "lex.h"
#include "simd.h"

#include <string.h>

const char *const punct_spelling[P_COUNT] = {
    "",
#define X(name, s) s,
    PUNCT_LIST(X)
#undef X
};

enum {
    C_ID = 1,        /* identifier continue */
    C_IDSTART = 2,
    C_DIGIT = 4,
    C_PPNUM = 8,     /* pp-number continue (besides sign after e/p) */
    C_SPACE = 16,    /* horizontal white space */
    C_SKIPSPECIAL = 32 /* interesting to the skip scanner */
};

static uint8_t cls[256];
static uint8_t cls_dollar[256];
static bool inited;

void lex_global_init(void)
{
    int c;
    if (inited)
        return;
    for (c = 0; c < 256; c++) {
        uint8_t k = 0;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
            c >= 0x80)
            k |= C_ID | C_IDSTART | C_PPNUM;
        if (c >= '0' && c <= '9')
            k |= C_ID | C_DIGIT | C_PPNUM;
        if (c == '.')
            k |= C_PPNUM;
        if (c == ' ' || c == '\t' || c == '\f' || c == '\v')
            k |= C_SPACE;
        if (c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '/' ||
            c == '\\' || c == 0 || c == '?')
            k |= C_SKIPSPECIAL;
        cls[c] = k;
        cls_dollar[c] = k;
    }
    cls_dollar['$'] |= C_ID | C_IDSTART | C_PPNUM;
    inited = true;
}

/* ---- setup ---------------------------------------------------------- */

void lexer_init(Lexer *L, SrcMgr *sm, Interner *in, DiagEngine *d,
                ScratchCursor *sc, LexOptions opt, SrcFile *f)
{
    lex_global_init();
    memset(L, 0, sizeof *L);
    L->region = sm->region;
    L->p = L->line_begin = f->buf;
    L->lim = f->buf + f->size;
    L->sm = sm;
    L->in = in;
    L->diag = d;
    L->scratch = sc;
    L->opt = opt;
    L->bol = true;
}

void lexer_init_range(Lexer *L, SrcMgr *sm, Interner *in, ScratchCursor *sc,
                      LexOptions opt, SrcLoc begin, uint32_t len)
{
    lex_global_init();
    memset(L, 0, sizeof *L);
    L->region = sm->region;
    L->p = L->line_begin = sm->region + begin;
    L->lim = L->p + len;
    L->sm = sm;
    L->in = in;
    L->scratch = sc;
    L->opt = opt;
    L->opt.trigraphs = false;
    L->bol = true;
}

void lexer_free(Lexer *L)
{
    sb_free(&L->clean);
}

void lexer_seek(Lexer *L, SrcLoc loc, bool bol)
{
    L->p = L->region + loc;
    L->bol = bol;
    if (bol)
        L->line_begin = L->p;
    L->space = false;
}

/* ---- slow path: logical characters ---------------------------------- */

#define LEOF (-1)

static int trigraph_char(char c)
{
    switch (c) {
    case '=': return '#';
    case '(': return '[';
    case '/': return '\\';
    case ')': return ']';
    case '\'': return '^';
    case '<': return '{';
    case '!': return '|';
    case '>': return '}';
    case '-': return '~';
    default: return 0;
    }
}

/* Logical character at p; *n = raw bytes consumed (incl. splices). */
static int getc_at(const Lexer *L, const char *p, uint32_t *n)
{
    const char *start = p;
    for (;;) {
        int c, t;
        uint32_t len = 1;
        if (p >= L->lim) {
            *n = (uint32_t)(p - start);
            return LEOF;
        }
        c = (unsigned char)*p;
        if (c == '?' && L->opt.trigraphs && p + 2 < L->lim && p[1] == '?' &&
            (t = trigraph_char(p[2])) != 0) {
            c = t;
            len = 3;
        }
        if (c == '\\') {
            const char *q = p + len;
            if (q < L->lim && *q == '\n') {
                p = q + 1;
                continue;
            }
            if (q < L->lim && *q == '\r') {
                q++;
                if (q < L->lim && *q == '\n')
                    q++;
                p = q;
                continue;
            }
        }
        *n = (uint32_t)(p + len - start);
        return c;
    }
}

typedef struct Slow {
    Lexer *L;
    const char *p;
} Slow;

static int s_peek(Slow *s)
{
    uint32_t n;
    return getc_at(s->L, s->p, &n);
}

static int s_peek2(Slow *s)
{
    uint32_t n, m;
    if (getc_at(s->L, s->p, &n) == LEOF)
        return LEOF;
    return getc_at(s->L, s->p + n, &m);
}

static int s_take(Slow *s)
{
    uint32_t n;
    int c = getc_at(s->L, s->p, &n);
    if (c != LEOF) {
        sb_putc(&s->L->clean, (char)c);
        s->p += n;
    }
    return c;
}

static bool s_is_idstart(Slow *s, int c)
{
    if (c == LEOF)
        return false;
    return (s->L->opt.dollar_idents ? cls_dollar : cls)[c & 0xFF] & C_IDSTART;
}

static int s_ucn_len(Slow *s)
{
    const char *p = s->p;
    uint32_t n;
    int c = getc_at(s->L, p, &n), want, i;
    if (c != '\\')
        return 0;
    p += n;
    c = getc_at(s->L, p, &n);
    if (c == 'u')
        want = 4;
    else if (c == 'U')
        want = 8;
    else
        return 0;
    p += n;
    for (i = 0; i < want; i++) {
        c = getc_at(s->L, p, &n);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
        p += n;
    }
    return want + 2;
}

static void s_take_n(Slow *s, int n)
{
    while (n-- > 0)
        s_take(s);
}

static bool s_quoted(Slow *s, int quote)
{
    s_take(s);
    for (;;) {
        int c = s_peek(s);
        if (c == LEOF || c == '\n' || c == '\r')
            return false;
        s_take(s);
        if (c == quote)
            return true;
        if (c == '\\') {
            c = s_peek(s);
            if (c != LEOF && c != '\n' && c != '\r')
                s_take(s);
        }
    }
}

static void s_rest_of_line(Slow *s)
{
    for (;;) {
        int c = s_peek(s);
        if (c == LEOF || c == '\n' || c == '\r')
            return;
        s_take(s);
    }
}

typedef struct PunctEnt {
    const char *s;
    Punct p;
    bool digraph;
} PunctEnt;

static const PunctEnt puncts[] = {
    {"%:%:", P_HASHHASH, true}, {"...", P_ELLIPSIS, false},
    {"<<=", P_SHL_ASSIGN, false}, {">>=", P_SHR_ASSIGN, false},
    {"->", P_ARROW, false}, {"++", P_INC, false}, {"--", P_DEC, false},
    {"<<", P_SHL, false}, {">>", P_SHR, false}, {"<=", P_LE, false},
    {">=", P_GE, false}, {"==", P_EQEQ, false}, {"!=", P_NE, false},
    {"&&", P_ANDAND, false}, {"||", P_OROR, false}, {"*=", P_MUL_ASSIGN, false},
    {"/=", P_DIV_ASSIGN, false}, {"%=", P_MOD_ASSIGN, false},
    {"+=", P_ADD_ASSIGN, false}, {"-=", P_SUB_ASSIGN, false},
    {"&=", P_AND_ASSIGN, false}, {"^=", P_XOR_ASSIGN, false},
    {"|=", P_OR_ASSIGN, false}, {"##", P_HASHHASH, false},
    {"<:", P_LBRACKET, true}, {":>", P_RBRACKET, true}, {"<%", P_LBRACE, true},
    {"%>", P_RBRACE, true}, {"%:", P_HASH, true},
    {"[", P_LBRACKET, false}, {"]", P_RBRACKET, false}, {"(", P_LPAREN, false},
    {")", P_RPAREN, false}, {"{", P_LBRACE, false}, {"}", P_RBRACE, false},
    {".", P_DOT, false}, {"&", P_AMP, false}, {"*", P_STAR, false},
    {"+", P_PLUS, false}, {"-", P_MINUS, false}, {"~", P_TILDE, false},
    {"!", P_BANG, false}, {"/", P_SLASH, false}, {"%", P_PERCENT, false},
    {"<", P_LT, false}, {">", P_GT, false}, {"^", P_CARET, false},
    {"|", P_PIPE, false}, {"?", P_QUESTION, false}, {":", P_COLON, false},
    {";", P_SEMI, false}, {"=", P_ASSIGN, false}, {",", P_COMMA, false},
    {"#", P_HASH, false},
};

static const PunctEnt *s_match_punct(Slow *s)
{
    int ch[4];
    const char *p = s->p;
    uint32_t n;
    size_t i, k;
    for (k = 0; k < 4; k++) {
        ch[k] = getc_at(s->L, p, &n);
        p += n;
    }
    for (i = 0; i < ARRAY_LEN(puncts); i++) {
        const char *q = puncts[i].s;
        for (k = 0; q[k]; k++)
            if (ch[k] != (unsigned char)q[k])
                break;
        if (!q[k])
            return &puncts[i];
    }
    return NULL;
}

/* Lex one token starting at `start` the careful way. */
static void lex_slow(Lexer *L, const char *start, Tok *t, uint16_t flags)
{
    Slow s;
    int c;
    const PunctEnt *pe;
    s.L = L;
    s.p = start;
    L->clean.len = 0;
    c = s_peek(&s);
    t->punct = 0;
    if (c == 'L' && (s_peek2(&s) == '\'' || s_peek2(&s) == '"')) {
        int q;
        s_take(&s);
        q = s_peek(&s);
        if (s_quoted(&s, q)) {
            t->kind = q == '"' ? TK_STRING : TK_CHAR;
        } else {
            s_rest_of_line(&s);
            t->kind = TK_OTHER;
            flags |= TF_UNTERMINATED;
        }
    } else if (s_is_idstart(&s, c) || s_ucn_len(&s)) {
        t->kind = TK_IDENT;
        for (;;) {
            int d = s_peek(&s), u;
            if (s_is_idstart(&s, d) || (d >= '0' && d <= '9')) {
                s_take(&s);
            } else if ((u = s_ucn_len(&s)) != 0) {
                flags |= TF_UCN;
                s_take_n(&s, u);
            } else {
                break;
            }
        }
    } else if ((c >= '0' && c <= '9') ||
               (c == '.' && s_peek2(&s) >= '0' && s_peek2(&s) <= '9')) {
        t->kind = TK_PPNUM;
        s_take(&s);
        for (;;) {
            int d = s_peek(&s), u;
            if ((d == 'e' || d == 'E' || d == 'p' || d == 'P') &&
                (s_peek2(&s) == '+' || s_peek2(&s) == '-')) {
                s_take(&s);
                s_take(&s);
            } else if (s_is_idstart(&s, d) || (d >= '0' && d <= '9') ||
                       d == '.') {
                s_take(&s);
            } else if ((u = s_ucn_len(&s)) != 0) {
                s_take_n(&s, u);
            } else {
                break;
            }
        }
    } else if (c == '\'' || c == '"') {
        if (s_quoted(&s, c)) {
            t->kind = c == '"' ? TK_STRING : TK_CHAR;
        } else {
            s_rest_of_line(&s);
            t->kind = TK_OTHER;
            flags |= TF_UNTERMINATED;
        }
    } else if ((pe = s_match_punct(&s)) != NULL) {
        t->kind = TK_PUNCT;
        t->punct = (uint8_t)pe->p;
        if (pe->digraph)
            flags |= TF_DIGRAPH;
        s_take_n(&s, (int)strlen(pe->s));
    } else {
        t->kind = TK_OTHER;
        s_take(&s);
    }
    t->loc = (SrcLoc)(start - L->region);
    t->len = (uint32_t)L->clean.len;
    t->aux = 0;
    if ((uint32_t)(s.p - start) != t->len ||
        memcmp(start, L->clean.data, t->len) != 0)
        flags |= TF_SPLICED;
    if (t->kind == TK_IDENT) {
        t->aux = intern(L->in, L->clean.data, L->clean.len)->id;
    } else if ((flags & TF_SPLICED) && L->scratch) {
        t->aux = srcmgr_scratch(L->sm, L->scratch, L->clean.data, L->clean.len);
        flags |= TF_SPELL;
    }
    t->flags = flags;
    L->p = s.p;
}

/* ---- fast path ------------------------------------------------------ */

/* Is there a line splice (or ??/ splice) at p? Returns its length. */
static int splice_at(const Lexer *L, const char *p)
{
    int n = 0;
    if (p[0] == '\\')
        n = 1;
    else if (L->opt.trigraphs && p[0] == '?' && p[1] == '?' && p[2] == '/')
        n = 3;
    else
        return 0;
    if (p[n] == '\n')
        return n + 1;
    if (p[n] == '\r')
        return p[n + 1] == '\n' ? n + 2 : n + 1;
    return 0;
}

static void unterminated_comment(Lexer *L, const char *start)
{
    if (L->diag)
        diag_report(L->diag, DL_ERROR, "", (SrcLoc)(start - L->region),
                    "unterminated comment");
}

/* Skip white space, comments and splices; updates bol/space. */
static const char *skip_blank(Lexer *L, const char *p)
{
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (c == ' ' || c == '\t') {
            p = scan_blanks(p + 1);
            L->space = true;
        } else if (c == '\f' || c == '\v') {
            p++;
            L->space = true;
        } else if (c == '\n') {
            p++;
            L->bol = true;
            L->space = false;
            L->line_begin = p;
        } else if (c == '\r') {
            p += p[1] == '\n' ? 2 : 1;
            L->bol = true;
            L->space = false;
            L->line_begin = p;
        } else if (c == '/' && p[1] == '*') {
            const char *start = p;
            p += 2;
            for (;;) {
                const char *s = memchr(p, '*', (size_t)(L->lim - p));
                int sp;
                if (!s) {
                    unterminated_comment(L, start);
                    p = L->lim;
                    break;
                }
                p = s + 1;
                while ((sp = splice_at(L, p)) != 0)
                    p += sp;
                if (*p == '/') {
                    p++;
                    break;
                }
            }
            L->space = true;
        } else if (c == '/' && p[1] == '/') {
            p += 2;
            for (;;) {
                unsigned char d;
                int sp;
                p = scan_find5(p, '\n', '\r', '\\', '?', '\n');
                d = (unsigned char)*p;
                if (d == '\n' || d == '\r' || (d == 0 && p >= L->lim))
                    break;
                if ((d == '\\' || d == '?') && (sp = splice_at(L, p)) != 0) {
                    p += sp;
                    continue;
                }
                p++;
            }
            L->space = true;
        } else if (c == '/' || c == '\\' || c == '?') {
            int sp = splice_at(L, c == '/' ? p + 1 : p);
            if (c == '/' && sp) {
                /* '/' followed by a splice: maybe a comment opener */
                const char *q = p + 1 + sp;
                while ((sp = splice_at(L, q)) != 0)
                    q += sp;
                if (*q == '*' || *q == '/') {
                    /* rare: rewrite the view by skipping via the slow reader */
                    uint32_t n;
                    Slow s;
                    int d;
                    s.L = L;
                    s.p = q + 1;
                    if (*q == '*') {
                        const char *start = p;
                        for (;;) {
                            d = getc_at(L, s.p, &n);
                            if (d == LEOF) {
                                unterminated_comment(L, start);
                                break;
                            }
                            s.p += n;
                            if (d == '*' && getc_at(L, s.p, &n) == '/') {
                                s.p += n;
                                break;
                            }
                        }
                    } else {
                        for (;;) {
                            d = getc_at(L, s.p, &n);
                            if (d == LEOF || d == '\n' || d == '\r')
                                break;
                            s.p += n;
                        }
                    }
                    p = s.p;
                    L->space = true;
                    continue;
                }
                return p;
            }
            if (c != '/' && sp) {
                p += sp;
                continue;
            }
            return p;
        } else if (c == 0 && p < L->lim) {
            if (L->diag && L->nul_line != L->line_begin) { /* once per line */
                diag_report(L->diag, DL_WARNING, "", (SrcLoc)(p - L->region),
                            "null character(s) ignored");
                L->nul_line = L->line_begin;
            }
            p++;
            L->space = true;
        } else {
            return p;
        }
    }
}

static inline void finish_simple(Lexer *L, Tok *t, TokKind k, const char *p,
                                 const char *q, uint16_t flags)
{
    t->kind = (uint8_t)k;
    t->loc = (SrcLoc)(p - L->region);
    t->len = (uint32_t)(q - p);
    t->flags = flags;
    L->p = q;
}

void lex_next(Lexer *L, Tok *t)
{
    const char *p = skip_blank(L, L->p), *q;
    const uint8_t *cl = L->opt.dollar_idents ? cls_dollar : cls;
    uint16_t flags = (uint16_t)((L->bol ? TF_BOL : 0) | (L->space ? TF_SPACE : 0));
    unsigned char c = (unsigned char)*p;
    bool trig = L->opt.trigraphs;

    L->bol = false;
    L->space = false;
    t->aux = 0;
    t->punct = 0;

    if (p >= L->lim) { /* a range's limit need not be followed by 0 */
        t->kind = TK_EOF;
        t->loc = (SrcLoc)(L->lim - L->region);
        t->len = 0;
        t->flags = TF_BOL;
        L->p = L->lim;
        return;
    }

    if (cl[c] & C_IDSTART) {
        if (c == 'L' && (p[1] == '\'' || p[1] == '"'))
            goto quoted;
        q = scan_ident(p + 1, L->opt.dollar_idents);
        if (*q == '\\' || (*q == '?' && trig))
            goto slow;
        t->kind = TK_IDENT;
        t->loc = (SrcLoc)(p - L->region);
        t->len = (uint32_t)(q - p);
        t->aux = intern(L->in, p, (size_t)(q - p))->id;
        t->flags = flags;
        L->p = q;
        return;
    }

    if ((cls[c] & C_DIGIT) || (c == '.' && (cls[(unsigned char)p[1]] & C_DIGIT))) {
        q = p + 1;
        for (;;) {
            unsigned char d = (unsigned char)*q;
            if (cl[d] & C_PPNUM) {
                if ((d | 0x20) == 'e' || (d | 0x20) == 'p') {
                    if (q[1] == '+' || q[1] == '-') {
                        q += 2;
                        continue;
                    }
                }
                q++;
                continue;
            }
            break;
        }
        if (*q == '\\' || (*q == '?' && trig))
            goto slow;
        finish_simple(L, t, TK_PPNUM, p, q, flags);
        return;
    }

    if (c == '"' || c == '\'') {
    quoted:
        {
            char quote;
            q = p;
            if (*q == 'L')
                q++;
            quote = *q++;
            for (;;) {
                unsigned char d;
                q = scan_find5(q, quote, '\\', '\n', '\r', '?');
                d = (unsigned char)*q;
                if (d == (unsigned char)quote) {
                    q++;
                    break;
                }
                if (d == '\\') {
                    if (q[1] == '\n' || q[1] == '\r')
                        goto slow;
                    q += 2;
                    continue;
                }
                if (d == '\n' || d == '\r' || (d == 0 && q >= L->lim) ||
                    (d == '?' && trig))
                    goto slow;
                q++; /* '?' without trigraphs, or an embedded NUL */
            }
            finish_simple(L, t, quote == '"' ? TK_STRING : TK_CHAR, p, q, flags);
            return;
        }
    }

    /* punctuators: anything that could continue across a splice or a
     * trigraph goes to the slow path */
    if (p[1] == '\\' || (trig && (c == '?' || p[1] == '?')))
        goto slow;
    {
        Punct k;
        int n = 1;
        switch (c) {
        case '[': k = P_LBRACKET; break;
        case ']': k = P_RBRACKET; break;
        case '(': k = P_LPAREN; break;
        case ')': k = P_RPAREN; break;
        case '{': k = P_LBRACE; break;
        case '}': k = P_RBRACE; break;
        case ',': k = P_COMMA; break;
        case ';': k = P_SEMI; break;
        case '~': k = P_TILDE; break;
        case '?': k = P_QUESTION; break;
        case '.':
            if (p[1] == '.' && (p[2] == '\\' || (trig && p[2] == '?'))) {
                goto slow;
            } else if (p[1] == '.' && p[2] == '.') {
                k = P_ELLIPSIS, n = 3;
            } else {
                k = P_DOT;
            }
            break;
        case '-':
            if (p[1] == '>') k = P_ARROW, n = 2;
            else if (p[1] == '-') k = P_DEC, n = 2;
            else if (p[1] == '=') k = P_SUB_ASSIGN, n = 2;
            else k = P_MINUS;
            break;
        case '+':
            if (p[1] == '+') k = P_INC, n = 2;
            else if (p[1] == '=') k = P_ADD_ASSIGN, n = 2;
            else k = P_PLUS;
            break;
        case '&':
            if (p[1] == '&') k = P_ANDAND, n = 2;
            else if (p[1] == '=') k = P_AND_ASSIGN, n = 2;
            else k = P_AMP;
            break;
        case '|':
            if (p[1] == '|') k = P_OROR, n = 2;
            else if (p[1] == '=') k = P_OR_ASSIGN, n = 2;
            else k = P_PIPE;
            break;
        case '*': if (p[1] == '=') k = P_MUL_ASSIGN, n = 2; else k = P_STAR; break;
        case '/': if (p[1] == '=') k = P_DIV_ASSIGN, n = 2; else k = P_SLASH; break;
        case '^': if (p[1] == '=') k = P_XOR_ASSIGN, n = 2; else k = P_CARET; break;
        case '!': if (p[1] == '=') k = P_NE, n = 2; else k = P_BANG; break;
        case '=': if (p[1] == '=') k = P_EQEQ, n = 2; else k = P_ASSIGN; break;
        case '#': if (p[1] == '#') k = P_HASHHASH, n = 2; else k = P_HASH; break;
        case '<':
            if (p[1] == '<') {
                if (p[2] == '\\' || (trig && p[2] == '?'))
                    goto slow;
                if (p[2] == '=') k = P_SHL_ASSIGN, n = 3;
                else k = P_SHL, n = 2;
            } else if (p[1] == '=') k = P_LE, n = 2;
            else if (p[1] == ':') k = P_LBRACKET, n = 2, flags |= TF_DIGRAPH;
            else if (p[1] == '%') k = P_LBRACE, n = 2, flags |= TF_DIGRAPH;
            else k = P_LT;
            break;
        case '>':
            if (p[1] == '>') {
                if (p[2] == '\\' || (trig && p[2] == '?'))
                    goto slow;
                if (p[2] == '=') k = P_SHR_ASSIGN, n = 3;
                else k = P_SHR, n = 2;
            } else if (p[1] == '=') k = P_GE, n = 2;
            else k = P_GT;
            break;
        case ':':
            if (p[1] == '>') k = P_RBRACKET, n = 2, flags |= TF_DIGRAPH;
            else k = P_COLON;
            break;
        case '%':
            if (p[1] == '=') k = P_MOD_ASSIGN, n = 2;
            else if (p[1] == '>') k = P_RBRACE, n = 2, flags |= TF_DIGRAPH;
            else if (p[1] == ':') {
                if (p[2] == '%' || p[2] == '\\' || (trig && p[2] == '?'))
                    goto slow; /* maybe %:%: */
                k = P_HASH, n = 2, flags |= TF_DIGRAPH;
            } else k = P_PERCENT;
            break;
        default:
            if (c == '\\')
                goto slow; /* UCN identifier or stray backslash */
            t->punct = 0;
            finish_simple(L, t, TK_OTHER, p, p + 1, flags);
            return;
        }
        t->punct = (uint8_t)k;
        finish_simple(L, t, TK_PUNCT, p, p + n, flags);
        return;
    }

slow:
    lex_slow(L, p, t, flags);
}

/* ---- skip scanner --------------------------------------------------- */

bool lex_line_is_directive(Lexer *L)
{
    const char *p;
    L->bol = true;
    L->space = false;
    p = skip_blank(L, L->p);
    L->p = p;
    return *p == '#' || (p[0] == '%' && p[1] == ':') ||
           (L->opt.trigraphs && p[0] == '?' && p[1] == '?' && p[2] == '=');
}

bool lex_next_line(Lexer *L)
{
    const char *p = L->p;
    bool trig = L->opt.trigraphs;
    for (;;) {
        unsigned char c;
        int sp;
        p = scan_skip_special(p);
        c = (unsigned char)*p;
        switch (c) {
        case '\n':
            L->p = L->line_begin = p + 1;
            return true;
        case '\r':
            L->p = L->line_begin = p + (p[1] == '\n' ? 2 : 1);
            return true;
        case 0:
            if (p >= L->lim) {
                L->p = L->lim;
                return false;
            }
            p++;
            break;
        case '"':
        case '\'': {
            char quote = (char)c;
            p++;
            for (;;) {
                unsigned char d;
                p = scan_find5(p, quote, '\\', '\n', '\r', '?');
                d = (unsigned char)*p;
                if (d == (unsigned char)quote) {
                    p++;
                    break;
                }
                if (d == '\\' || (trig && d == '?')) {
                    if ((sp = splice_at(L, p)) != 0) {
                        p += sp;
                        continue;
                    }
                    if (d == '\\') {
                        p += (p[1] == '\n' || p[1] == '\r' || p[1] == 0) ? 1 : 2;
                        continue;
                    }
                }
                if (d == '\n' || d == '\r' || (d == 0 && p >= L->lim))
                    break; /* unterminated: ends at end of line */
                p++;
            }
            break;
        }
        case '/':
            if (p[1] == '*' || p[1] == '/' || p[1] == '\\') {
                /* reuse the blank skipper for comments (keeps splices right) */
                const char *q;
                L->bol = false;
                q = skip_blank(L, p);
                if (q == p) {
                    p++;
                } else {
                    /* skip_blank may have consumed the newline too */
                    if (L->bol && q > p) {
                        L->p = L->line_begin; /* the line start, not q */
                        L->bol = false;
                        return true;
                    }
                    p = q;
                }
            } else {
                p++;
            }
            break;
        case '\\':
        case '?':
            if ((sp = splice_at(L, p)) != 0)
                p += sp;
            else
                p++;
            break;
        default:
            p++;
        }
    }
}

uint32_t tok_raw_len(const SrcMgr *sm, const Interner *in, const Tok *t)
{
    Lexer L;
    Tok u;
    SrcFile *f;
    if (!(t->flags & TF_SPLICED))
        return t->len;
    f = srcmgr_file_of(sm, t->loc);
    if (!f)
        return t->len;
    memset(&L, 0, sizeof L);
    lex_global_init();
    L.region = sm->region;
    L.p = sm->region + t->loc;
    L.lim = f->buf + f->size;
    L.sm = (SrcMgr *)sm;
    L.in = (Interner *)in;
    L.opt.trigraphs = true;
    L.opt.dollar_idents = true;
    lex_slow(&L, L.p, &u, 0);
    lexer_free(&L);
    return (uint32_t)(L.p - (sm->region + t->loc));
}
