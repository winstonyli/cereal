/* lex.c - translation phases 1-3: pp-token lexer.
 *
 * Works directly on the raw buffer.  Trigraphs (phase 1) and line splices
 * (phase 2) are handled by the character reader, so every token's loc is the
 * raw offset of its first character and rawlen spans its raw extent. */
#include "lex.h"

#include <string.h>

const char *const punct_spelling[P_COUNT] = {
    "",
#define X(name, s) s,
    PUNCT_LIST(X)
#undef X
};

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

/* Logical character at raw position p; *n receives the raw length consumed
 * (including any splices skipped before the character). */
static int getc_at(Lexer *L, uint32_t p, uint32_t *n)
{
    uint32_t start = p;
    for (;;) {
        int c, t;
        uint32_t len = 1;
        if (p >= L->size) {
            *n = p - start;
            return LEOF;
        }
        c = (unsigned char)L->buf[p];
        if (c == '?' && L->opt.trigraphs && p + 2 < L->size &&
            L->buf[p + 1] == '?' && (t = trigraph_char(L->buf[p + 2])) != 0) {
            c = t;
            len = 3;
        }
        if (c == '\\') {
            uint32_t q = p + len;
            if (q < L->size && L->buf[q] == '\n') {
                p = q + 1;
                continue;
            }
            if (q < L->size && L->buf[q] == '\r') {
                q++;
                if (q < L->size && L->buf[q] == '\n')
                    q++;
                p = q;
                continue;
            }
        }
        *n = p + len - start;
        return c;
    }
}

static int peek(Lexer *L)
{
    uint32_t n;
    return getc_at(L, L->pos, &n);
}

static int peek2(Lexer *L)
{
    uint32_t n, m;
    if (getc_at(L, L->pos, &n) == LEOF)
        return LEOF;
    return getc_at(L, L->pos + n, &m);
}

/* Consume one logical char, appending it to scratch. */
static int take(Lexer *L)
{
    uint32_t n;
    int c = getc_at(L, L->pos, &n);
    if (c != LEOF) {
        sb_putc(&L->scratch, (char)c);
        L->pos += n;
    }
    return c;
}

static bool is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           c >= 0x80;
}

static bool is_digit(int c) { return c >= '0' && c <= '9'; }
static bool is_xdigit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* If a UCN (\uXXXX or \UXXXXXXXX) starts at pos, return its logical length. */
static int ucn_len(Lexer *L)
{
    uint32_t p = L->pos, n;
    int c = getc_at(L, p, &n), want, i;
    if (c != '\\')
        return 0;
    p += n;
    c = getc_at(L, p, &n);
    if (c == 'u')
        want = 4;
    else if (c == 'U')
        want = 8;
    else
        return 0;
    p += n;
    for (i = 0; i < want; i++) {
        c = getc_at(L, p, &n);
        if (!is_xdigit(c))
            return 0;
        p += n;
    }
    return want + 2;
}

static void take_n(Lexer *L, int n)
{
    while (n-- > 0)
        take(L);
}

static void lex_ident_rest(Lexer *L, uint16_t *flags)
{
    for (;;) {
        int c = peek(L), u;
        if (is_ident_start(c) || is_digit(c)) {
            take(L);
        } else if (c == '$' && L->opt.dollar_idents) {
            *flags |= TF_STRAY_DOLLAR;
            take(L);
        } else if ((u = ucn_len(L)) != 0) {
            *flags |= TF_UCN;
            take_n(L, u);
        } else {
            break;
        }
    }
}

static void lex_ppnum_rest(Lexer *L)
{
    for (;;) {
        int c = peek(L), u;
        if ((c == 'e' || c == 'E' || c == 'p' || c == 'P') &&
            (peek2(L) == '+' || peek2(L) == '-')) {
            take(L);
            take(L);
        } else if (is_ident_start(c) || is_digit(c) || c == '.' ||
                   (c == '$' && L->opt.dollar_idents)) {
            take(L);
        } else if ((u = ucn_len(L)) != 0) {
            take_n(L, u);
        } else {
            break;
        }
    }
}

/* Returns true if terminated. */
static bool lex_quoted(Lexer *L, int quote)
{
    take(L); /* opening quote */
    for (;;) {
        int c = peek(L);
        if (c == LEOF || c == '\n' || c == '\r')
            return false;
        take(L);
        if (c == quote)
            return true;
        if (c == '\\') {
            c = peek(L);
            if (c != LEOF && c != '\n' && c != '\r')
                take(L);
        }
    }
}

static void lex_rest_of_line(Lexer *L)
{
    for (;;) {
        int c = peek(L);
        if (c == LEOF || c == '\n' || c == '\r')
            return;
        take(L);
    }
}

typedef struct PunctEnt {
    const char *s;
    Punct p;
    bool digraph;
} PunctEnt;

/* Longest first. */
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

static const PunctEnt *match_punct(Lexer *L)
{
    int ch[4];
    uint32_t p = L->pos, n;
    size_t i, k;
    for (k = 0; k < 4; k++) {
        ch[k] = getc_at(L, p, &n);
        p += n;
    }
    for (i = 0; i < ARRAY_LEN(puncts); i++) {
        const char *s = puncts[i].s;
        for (k = 0; s[k]; k++)
            if (ch[k] != (unsigned char)s[k])
                break;
        if (!s[k])
            return &puncts[i];
    }
    return NULL;
}

static bool skip_space_and_comments(Lexer *L, bool *bol, bool *space)
{
    for (;;) {
        uint32_t n;
        int c = getc_at(L, L->pos, &n);
        if (c == '\n') {
            L->pos += n;
            *bol = true;
            *space = false;
        } else if (c == '\r') {
            L->pos += n;
            if (L->pos < L->size && L->buf[L->pos] == '\n')
                L->pos++;
            *bol = true;
            *space = false;
        } else if (c == ' ' || c == '\t' || c == '\f' || c == '\v') {
            L->pos += n;
            *space = true;
        } else if (c == '/' && peek2(L) == '*') {
            uint32_t start = L->pos;
            L->pos += n;
            getc_at(L, L->pos, &n);
            L->pos += n;
            for (;;) {
                c = getc_at(L, L->pos, &n);
                if (c == LEOF) {
                    if (L->diag)
                        diag_report(L->diag, DL_ERROR, "", L->base + start,
                                    "unterminated comment");
                    return false;
                }
                L->pos += n;
                if (c == '*' && peek(L) == '/') {
                    getc_at(L, L->pos, &n);
                    L->pos += n;
                    break;
                }
            }
            *space = true;
        } else if (c == '/' && peek2(L) == '/') {
            for (;;) {
                c = getc_at(L, L->pos, &n);
                if (c == LEOF || c == '\n' || c == '\r')
                    break;
                L->pos += n;
            }
            *space = true;
        } else {
            return c != LEOF;
        }
    }
}

static Token *lex_one(Lexer *L, bool bol, bool space)
{
    Token *t = NEW(L->arena, Token);
    uint32_t start = L->pos;
    int c = peek(L);
    uint16_t flags = (uint16_t)((bol ? TF_BOL : 0) | (space ? TF_SPACE : 0));
    const PunctEnt *pe;

    L->scratch.len = 0;
    if ((c == 'L') && (peek2(L) == '\'' || peek2(L) == '"')) {
        int q;
        take(L);
        q = peek(L);
        if (lex_quoted(L, q)) {
            t->kind = q == '"' ? TK_STRING : TK_CHAR;
        } else {
            lex_rest_of_line(L);
            t->kind = TK_OTHER;
            flags |= TF_UNTERMINATED;
        }
    } else if (is_ident_start(c) || (c == '$' && L->opt.dollar_idents) ||
               ucn_len(L) != 0) {
        t->kind = TK_IDENT;
        lex_ident_rest(L, &flags);
    } else if (is_digit(c) || (c == '.' && is_digit(peek2(L)))) {
        t->kind = TK_PPNUM;
        take(L);
        lex_ppnum_rest(L);
    } else if (c == '\'' || c == '"') {
        if (lex_quoted(L, c)) {
            t->kind = c == '"' ? TK_STRING : TK_CHAR;
        } else {
            lex_rest_of_line(L);
            t->kind = TK_OTHER;
            flags |= TF_UNTERMINATED;
        }
    } else if ((pe = match_punct(L)) != NULL) {
        t->kind = TK_PUNCT;
        t->punct = (uint8_t)pe->p;
        if (pe->digraph)
            flags |= TF_DIGRAPH;
        take_n(L, (int)strlen(pe->s));
    } else {
        t->kind = TK_OTHER;
        take(L);
    }

    t->rawlen = L->pos - start;
    t->len = (uint32_t)L->scratch.len;
    if (t->len == t->rawlen) {
        t->text = L->buf + start;
    } else {
        t->text = arena_strndup(L->arena, L->scratch.data, L->scratch.len);
        flags |= TF_SPLICED;
    }
    t->flags = flags;
    t->loc = L->base ? L->base + start : 0;
    if (t->kind == TK_IDENT)
        t->ident = intern(L->in, t->text, t->len);
    return t;
}

static Token *lex_all(Lexer *L)
{
    Token head, *tail = &head;
    bool bol = true, space = false;
    head.next = NULL;
    while (skip_space_and_comments(L, &bol, &space)) {
        Token *t = lex_one(L, bol, space);
        tail = tail->next = t;
        bol = space = false;
    }
    {
        Token *eof = NEW(L->arena, Token);
        eof->kind = TK_EOF;
        eof->flags = TF_BOL;
        eof->text = "";
        eof->loc = L->base ? L->base + L->size : 0;
        tail->next = eof;
    }
    sb_free(&L->scratch);
    return head.next;
}

Token *lex_file(Arena *a, Interner *in, DiagEngine *d, LexOptions opt,
                SrcFile *f)
{
    Lexer L;
    memset(&L, 0, sizeof L);
    L.arena = a;
    L.in = in;
    L.diag = d;
    L.opt = opt;
    L.buf = f->buf;
    L.size = f->size;
    L.base = f->base;
    return lex_all(&L);
}

Token *lex_buffer(Arena *a, Interner *in, LexOptions opt, const char *buf,
                  size_t len, SrcLoc loc)
{
    Lexer L;
    Token *t, *list;
    memset(&L, 0, sizeof L);
    L.arena = a;
    L.in = in;
    L.opt = opt;
    L.opt.trigraphs = false;
    L.buf = arena_strndup(a, buf, len);
    L.size = (uint32_t)len;
    L.base = 0;
    list = lex_all(&L);
    for (t = list; t; t = t->next) {
        t->loc = loc;
        t->rawlen = 0;
    }
    return list;
}

char *tok_str(Arena *a, const Token *t)
{
    return arena_strndup(a, t->text, t->len);
}
