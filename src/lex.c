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
#include "utf8.h"

#include <ctype.h>
#include <pthread.h>
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
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;

static void init_tables(void)
{
    int c;
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
}

void lex_global_init(void)
{
    pthread_once(&tables_once, init_tables);
}

/* ---- setup ---------------------------------------------------------- */

/* Does the file have a byte >= 0x80? */
static bool has_high(const char *p, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        if (w & 0x8080808080808080ull)
            return true;
    }
    for (; i < n; i++)
        if ((unsigned char)p[i] & 0x80)
            return true;
    return false;
}

static inline bool has_high_in(const char *p, const char *q)
{
    for (; p < q; p++)
        if ((unsigned char)*p & 0x80)
            return true;
    return false;
}

/* Attach the diagnostics and decide, from [p, lim), whether the text can
 * need the slow path's UTF-8 handling (hi8) or -Wbidi-chars (bidi_live). */
void lexer_set_diag(Lexer *L, DiagEngine *d)
{
    size_t n = (size_t)(L->lim - L->p);
    L->diag = d;
    L->hi8 = d && has_high(L->p, n);
    L->bidi_live = d && (L->opt.bidi & (BIDI_UNPAIRED | BIDI_ANY)) &&
                   ((L->hi8 && memchr(L->p, 0xE2, n)) ||
                    (L->opt.bidi & BIDI_UCN));
    L->bidi_hi = L->p;
}

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
    lexer_set_diag(L, d);
}

void lexer_init_range(Lexer *L, SrcMgr *sm, Interner *in, ScratchCursor *sc,
                      LexOptions opt, SrcLoc begin, uint32_t len)
{
    lex_global_init();
    memset(L, 0, sizeof *L);
    L->region = sm->region;
    L->p = L->line_begin = sm->region + begin;
    L->lim = L->p + len;
    L->hi8 = has_high(L->p, len);   /* diagnostics come later */
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

/* ---- -Wbidi-chars (libcpp's bidi namespace) ------------------------ */

enum { BK_NONE, BK_LRE, BK_RLE, BK_LRO, BK_RLO, BK_LRI, BK_RLI, BK_FSI,
       BK_PDF, BK_PDI, BK_LTR, BK_RTL };

static const char *const bk_name[] = {
    "", "U+202A (LEFT-TO-RIGHT EMBEDDING)", "U+202B (RIGHT-TO-LEFT EMBEDDING)",
    "U+202D (LEFT-TO-RIGHT OVERRIDE)", "U+202E (RIGHT-TO-LEFT OVERRIDE)",
    "U+2066 (LEFT-TO-RIGHT ISOLATE)", "U+2067 (RIGHT-TO-LEFT ISOLATE)",
    "U+2068 (FIRST STRONG ISOLATE)", "U+202C (POP DIRECTIONAL FORMATTING)",
    "U+2069 (POP DIRECTIONAL ISOLATE)", "U+200E (LEFT-TO-RIGHT MARK)",
    "U+200F (RIGHT-TO-LEFT MARK)",
};

/* The control character whose UTF-8 form starts at p (p[0] == 0xE2). */
static int bidi_utf8_kind(const unsigned char *p)
{
    if (p[1] == 0x80) {
        switch (p[2]) {
        case 0xaa: return BK_LRE;
        case 0xab: return BK_RLE;
        case 0xac: return BK_PDF;
        case 0xad: return BK_LRO;
        case 0xae: return BK_RLO;
        case 0x8e: return BK_LTR;
        case 0x8f: return BK_RTL;
        }
    } else if (p[1] == 0x81) {
        switch (p[2]) {
        case 0xa6: return BK_LRI;
        case 0xa7: return BK_RLI;
        case 0xa8: return BK_FSI;
        case 0xa9: return BK_PDI;
        }
    }
    return BK_NONE;
}

/* The same for a UCN whose \u or \U ends just before p. */
static int bidi_ucn_kind(const unsigned char *p, bool big)
{
    if (big) {
        if (p[0] != '0' || p[1] != '0' || p[2] != '0' || p[3] != '0')
            return BK_NONE;
        p += 4;
    } else if (p[0] == '{') {
        p++;
        while (*p == '0')
            p++;
        if (p[0] != '2' || p[1] != '0' || !isxdigit(p[2]) || !isxdigit(p[3]) ||
            p[4] != '}')
            return BK_NONE;
    }
    if (p[0] != '2' || p[1] != '0')
        return BK_NONE;
    if (p[2] == '2') {
        switch (p[3] | 0x20) {
        case 'a': return BK_LRE;
        case 'b': return BK_RLE;
        case 'c': return BK_PDF;
        case 'd': return BK_LRO;
        case 'e': return BK_RLO;
        }
    } else if (p[2] == '6') {
        switch (p[3]) {
        case '6': return BK_LRI;
        case '7': return BK_RLI;
        case '8': return BK_FSI;
        case '9': return BK_PDI;
        }
    } else if (p[2] == '0') {
        switch (p[3] | 0x20) {
        case 'e': return BK_LTR;
        case 'f': return BK_RTL;
        }
    }
    return BK_NONE;
}

/* A control written \N{NAME} (the text after the brace, up to the end). */
static int bidi_named_kind(const char *p, const char *e)
{
    static const struct { const char *name; int kind; } t[] = {
        {"LEFT-TO-RIGHT EMBEDDING", BK_LRE}, {"RIGHT-TO-LEFT EMBEDDING", BK_RLE},
        {"POP DIRECTIONAL FORMATTING", BK_PDF},
        {"LEFT-TO-RIGHT OVERRIDE", BK_LRO}, {"RIGHT-TO-LEFT OVERRIDE", BK_RLO},
        {"LEFT-TO-RIGHT ISOLATE", BK_LRI}, {"RIGHT-TO-LEFT ISOLATE", BK_RLI},
        {"FIRST STRONG ISOLATE", BK_FSI}, {"POP DIRECTIONAL ISOLATE", BK_PDI},
        {"LEFT-TO-RIGHT MARK", BK_LTR}, {"RIGHT-TO-LEFT MARK", BK_RTL},
    };
    size_t k, n = 0;
    while (p + n < e && p[n] != '}')
        n++;
    if (p + n >= e)
        return BK_NONE;
    for (k = 0; k < sizeof t / sizeof *t; k++)
        if (strlen(t[k].name) == n && !strncmp(t[k].name, p, n))
            return t[k].kind;
    return BK_NONE;
}

/* The pop kind of the innermost open context (PDF, PDI or none). */
static int bidi_cur(const Lexer *L)
{
    return L->bd_n ? (L->bd[L->bd_n - 1] & 1 ? BK_PDF : BK_PDI) : BK_NONE;
}

/* A bidirectional control character at p, written as a UCN or not. */
static void bidi_char(Lexer *L, const char *p, int kind, bool ucn)
{
    unsigned f = L->opt.bidi;
    SrcLoc loc = (SrcLoc)(p - L->region);
    if (kind == bidi_cur(L)) {
        if (f == (BIDI_UNPAIRED | BIDI_UCN) &&
            ((L->bd[L->bd_n - 1] >> 1) & 1u) != (unsigned)ucn)
            diag_report(L->diag, DL_WARNING, "bidi-chars=", loc,
                        "UTF-8 vs UCN mismatch when closing a context by "
                        "\"%s\"", bk_name[kind]);
    } else if ((f & BIDI_ANY) && (!ucn || (f & BIDI_UCN))) {
        if (kind == BK_PDF || kind == BK_PDI)
            diag_report(L->diag, DL_WARNING, "bidi-chars=", loc,
                        "\"%s\" is closing an unopened context", bk_name[kind]);
        else
            diag_report(L->diag, DL_WARNING, "bidi-chars=", loc,
                        "found problematic Unicode character \"%s\"",
                        bk_name[kind]);
    }
    switch (kind) {
    case BK_LRE: case BK_RLE: case BK_LRO: case BK_RLO:
    case BK_LRI: case BK_RLI: case BK_FSI:
        if (L->bd_n < sizeof L->bd)
            L->bd[L->bd_n++] = (uint8_t)((kind <= BK_RLO ? 1 : 0) | ucn << 1);
        break;
    case BK_PDF:
        if (bidi_cur(L) == BK_PDF)
            L->bd_n--;
        break;
    case BK_PDI: {
        uint32_t i;
        for (i = L->bd_n; i-- > 0;)
            if (!(L->bd[i] & 1)) {
                L->bd_n = i;
                break;
            }
        break;
    }
    }
}

/* A comment, literal or identifier ends at p: warn if a context it opened is
 * still open at the character before p. */
static void bidi_close(Lexer *L, const char *p)
{
    unsigned f = L->opt.bidi;
    if (L->bd_n && (f & BIDI_UNPAIRED) &&
        (!(L->bd[L->bd_n - 1] & 2) || (f & BIDI_UCN)))
        diag_report(L->diag, DL_WARNING, "bidi-chars=",
                    (SrcLoc)(p - 1 - L->region),
                    L->bd_n > 1 ? "unpaired UTF-8 bidirectional control "
                                  "characters detected"
                                : "unpaired UTF-8 bidirectional control "
                                  "character detected");
    L->bd_n = 0;
}

/* The controls in [s, e), UCNs only where a literal may have them. */
static void bidi_scan(Lexer *L, const char *s, const char *e, bool ucn_ok)
{
    const char *p = s;
    while (p < e) {
        unsigned char c = (unsigned char)*p;
        int k;
        if (c == 0xE2) {
            if ((k = bidi_utf8_kind((const unsigned char *)p)) != BK_NONE)
                bidi_char(L, p, k, false);
            p++;
        } else if (c == '\\' && ucn_ok) {
            if ((p[1] == 'u' || p[1] == 'U') &&
                (k = bidi_ucn_kind((const unsigned char *)p + 2, p[1] == 'U')) !=
                    BK_NONE)
                bidi_char(L, p, k, true);
            else if (p[1] == 'N' && p + 2 < e && p[2] == '{' &&
                     (k = bidi_named_kind(p + 3, e)) != BK_NONE)
                bidi_char(L, p, k, true);
            p += 2;
        } else {
            p++;
        }
    }
}

/* forms_identifier_p met a backslash at p: a UCN is a candidate. */
static void bidi_try_ucn(Lexer *L, const char *p)
{
    int k;
    if ((p[1] == 'u' || p[1] == 'U') &&
        (k = bidi_ucn_kind((const unsigned char *)p + 2, p[1] == 'U')) != BK_NONE)
        bidi_char(L, p, k, true);
    else if (p[1] == 'N' && p[2] == '{' &&
             (k = bidi_named_kind(p + 3, L->lim)) != BK_NONE)
        bidi_char(L, p, k, true);
}

/* One context [s, e) closing at p (the pointer libcpp gives
 * maybe_warn_bidi_on_close). */
static void bidi_ctx(Lexer *L, const char *s, const char *e, const char *p,
                     bool ucn_ok)
{
    size_t n, n0;
    if (s < L->bidi_hi)         /* the line is being lexed again */
        return;
    n = (size_t)(e - s);
    n0 = L->diag->all.len;
    if (memchr(s, 0xE2, n) ||
        (ucn_ok && ((L->opt.bidi & BIDI_UCN) || L->bd_n) &&
         memchr(s, '\\', n)))
        bidi_scan(L, s, e, ucn_ok);
    if (L->bd_n)
        bidi_close(L, p);
    /* gcc warns as it lexes the token: before any checker diagnostic at
     * the token's own start */
    if (ucn_ok) {                   /* back over the opening quote and prefix */
        int k;
        for (k = 0; k < 3 && s > L->region + 1 && s[-1] &&
                    strchr("\"'LuU8", s[-1]); k++)
            s--;
        /* gcc has lexed this token as the lookahead of the one before it,
         * so the diagnostics of that token's parse follow */
        while (s > L->region + 1 && (s[-1] == ' ' || s[-1] == '\t'))
            s--;
        if (s > L->region + 1) {
            if (isalnum((unsigned char)s[-1]) || s[-1] == '_')
                while (s > L->region + 1 &&
                       (isalnum((unsigned char)s[-1]) || s[-1] == '_'))
                    s--;
            else
                s--;
        }
    }
    for (; n0 < L->diag->all.len; n0++)
        if (s > L->region + 1)
            L->diag->all.data[n0]->oloc = (SrcLoc)(s - L->region) - 1;
    L->bidi_hi = e;
}

/* A string or character constant whose text is [s, e), e the closing quote. */
static inline void bidi_lit(Lexer *L, const char *s, const char *e)
{
    if (__builtin_expect(L->bidi_live, 0))
        bidi_ctx(L, s, e, e, true);
}

/* A comment's text [s, e): a block comment is a context a line, closed at
 * each newline and after the final slash; a line comment is one only if it
 * has a 0xE2 byte (libcpp skips it by bytes otherwise). */
static void bidi_comment(Lexer *L, const char *s, const char *e, bool block)
{
    const char *nl;
    if (!block && !memchr(s, 0xE2, (size_t)(e - s)))
        return;
    while (block && (nl = memchr(s, '\n', (size_t)(e - s))) != NULL) {
        bidi_ctx(L, s, nl, nl > s && nl[-1] == '\r' ? nl : nl + 1, false);
        s = nl + 1;
    }
    bidi_ctx(L, s, e, e, false);
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
    if (c == LEOF || c >= 0x80)
        return false;
    return (s->L->opt.dollar_idents ? cls_dollar : cls)[c & 0xFF] & C_IDSTART;
}

/* The identifier text with every UCN spelled \uXXXX / \UXXXXXXXX (upper-case
 * hex, the shorter form when it fits); 0 if it does not fit in cap. */
static size_t ucn_canon(const char *p, size_t n, char *out, size_t cap)
{
    size_t i = 0, o = 0;
    while (i < n) {
        if (p[i] == '\\' && i + 1 < n && (p[i + 1] == 'u' || p[i + 1] == 'U')) {
            size_t want = p[i + 1] == 'u' ? 4 : 8, k;
            unsigned long v = 0;
            char buf[16];
            int w;
            if (i + 2 + want > n)
                return 0;
            for (k = 0; k < want; k++) {
                char h = p[i + 2 + k];
                v = v * 16 + (unsigned long)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
            }
            w = v <= 0xFFFF ? snprintf(buf, sizeof buf, "\\u%04lX", v)
                            : snprintf(buf, sizeof buf, "\\U%08lX", v);
            if (o + (size_t)w >= cap)
                return 0;
            memcpy(out + o, buf, (size_t)w);
            o += (size_t)w;
            i += 2 + want;
        } else {
            if (o + 1 >= cap)
                return 0;
            out[o++] = p[i++];
        }
    }
    return o;
}

#include "ucn99.h"

#include "ucnx.h"

/* ucn_valid_in_identifier under -pedantic in C99: 1 ok, 0 not an identifier
 * character, 2 ok but not as the first one. */
static int ucn99_cp(unsigned long v)
{
    size_t lo = 0, hi = sizeof ucn99 / sizeof *ucn99;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (v < ucn99[mid].lo)
            hi = mid;
        else if (v > ucn99[mid].hi)
            lo = mid + 1;
        else
            return ucn99[mid].nostart ? 2 : 1;
    }
    return 0;
}

static unsigned long ucn_value(const char *p, int digits)
{
    unsigned long v = 0;
    int k;
    for (k = 0; k < digits; k++)
        v = v * 16 + (unsigned long)(p[k] <= '9' ? p[k] - '0' : (p[k] | 32) - 'a' + 10);
    return v;
}

/* The same for a code point written in UTF-8: without -pedantic gcc accepts
 * the union of the C99, C++ and C11 sets. */
static int utf8_id_class(const Lexer *L, uint32_t cp)
{
    size_t lo = 0, hi = sizeof ucnx / sizeof *ucnx;
    if (L->opt.ucn_c99)
        return cp > 0xFFFF ? 0 : ucn99_cp(cp);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < ucnx[mid].lo)
            hi = mid;
        else if (cp > ucnx[mid].hi)
            lo = mid + 1;
        else
            return ucnx[mid].cls;
    }
    return 0;
}

/* The UTF-8 character at s->p as an identifier character: its class (see
 * ucn99_class) and byte length; 0 if it is not valid UTF-8 or no identifier
 * character, in which case it is a token of its own. */
static int s_utf8_id(Slow *s, int *len)
{
    uint32_t cp;
    int n = utf8_dec((const unsigned char *)s->p, (size_t)(s->L->lim - s->p), &cp);
    if (!n)
        return 0;
    *len = n;
    return utf8_id_class(s->L, cp);
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
/* Length of a string/char literal prefix at p (0: none). */
static int qprefix(const char *p, bool uliterals)
{
    if (p[0] == 'L' && (p[1] == '\'' || p[1] == '"'))
        return 1;
    if (!uliterals)
        return 0;
    if ((p[0] == 'u' || p[0] == 'U') && (p[1] == '\'' || p[1] == '"'))
        return 1;
    if (p[0] == 'u' && p[1] == '8' && p[2] == '"')
        return 2;
    return 0;
}

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
    if ((c == 'L' || (L->opt.uliterals && (c == 'u' || c == 'U'))) &&
        (s_peek2(&s) == '\'' || s_peek2(&s) == '"')) {
        int q;
        s_take(&s);
        q = s_peek(&s);
        if (s_quoted(&s, q)) {
            t->kind = q == '"' ? TK_STRING : TK_CHAR;
            bidi_lit(L, start + 2, s.p - 1);
        } else {
            s_rest_of_line(&s);
            t->kind = TK_OTHER;
            flags |= TF_UNTERMINATED;
        }
    } else if (s_is_idstart(&s, c) || s_ucn_len(&s) ||
               (c >= 0x80 && s_utf8_id(&s, &(int){0}))) {
        bool ext = false;           /* has a UTF-8 character or a UCN */
        t->kind = TK_IDENT;
        for (;;) {
            int d = s_peek(&s), u, k;
            if (d == '\\' && L->bidi_live)
                bidi_try_ucn(L, s.p);
            if (s_is_idstart(&s, d) || (d >= '0' && d <= '9')) {
                s_take(&s);
            } else if (d >= 0x80) {
                int bk;
                if (d == 0xE2 && L->bidi_live &&
                    (bk = bidi_utf8_kind((const unsigned char *)s.p)) != BK_NONE)
                    bidi_char(L, s.p, bk, false);
                if (!(k = s_utf8_id(&s, &u)))
                    break;
                if (k == 2 && s.p == start && L->diag)
                    diag_report(L->diag, DL_ERROR, "", (SrcLoc)(start - L->region),
                                "extended character %.*s is not valid at the "
                                "start of an identifier", u, s.p);
                ext = true;
                s_take_n(&s, u);
            } else if ((u = s_ucn_len(&s)) != 0) {
                ext = true;
                flags |= TF_UCN;
                if (L->diag) {          /* _cpp_valid_ucn in an identifier */
                    unsigned long v = ucn_value(s.p + 2, u - 2);
                    int cl;
                    if ((v < 0xA0 && v != 0x24 && v != 0x40 && v != 0x60) ||
                        v >= 0x80000000ul || (v >= 0xD800 && v <= 0xDFFF))
                        diag_report(L->diag, DL_ERROR, "",
                                    (SrcLoc)(start - L->region),
                                    "%.*s is not a valid universal character",
                                    u, s.p);
                    else if (v != 0x24 || !L->opt.dollar_idents) {
                        cl = utf8_id_class(L, (uint32_t)v);
                        if (cl == 0 || (cl == 2 && s.p == start))
                            diag_report(L->diag, DL_ERROR, "",
                                        (SrcLoc)(start - L->region),
                                        "universal character %.*s is not "
                                        "valid %san identifier", u, s.p,
                                        cl ? "at the start of " : "in ");
                    }
                }
                s_take_n(&s, u);
            } else {
                break;
            }
        }
        if (ext && L->bidi_live)
            bidi_close(L, s.p);
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
            } else if (d >= 0x80 && s_utf8_id(&s, &u)) {
                s_take_n(&s, u);
            } else {
                break;
            }
        }
    } else if (c == '\'' || c == '"') {
        if (s_quoted(&s, c)) {
            t->kind = c == '"' ? TK_STRING : TK_CHAR;
            bidi_lit(L, start + 1, s.p - 1);
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
        int u = 1;
        t->kind = TK_OTHER;
        if (c == '\\' && L->bidi_live)    /* forms_identifier_p looked here */
            bidi_try_ucn(L, s.p);
        if (c >= 0x80) {        /* a whole valid UTF-8 character, else a byte */
            uint32_t cp;
            int bk;
            if (L->bidi_live && c == 0xE2 &&
                (bk = bidi_utf8_kind((const unsigned char *)s.p)) != BK_NONE)
                bidi_char(L, s.p, bk, false);
            u = utf8_dec((const unsigned char *)s.p, (size_t)(L->lim - s.p), &cp);
            if (!u)
                u = 1;
        }
        s_take_n(&s, u);
    }
    t->loc = (SrcLoc)(start - L->region);
    t->len = (uint32_t)L->clean.len;
    t->aux = 0;
    if ((uint32_t)(s.p - start) != t->len ||
        memcmp(start, L->clean.data, t->len) != 0)
        flags |= TF_SPLICED;
    if (t->kind == TK_IDENT) {
        char canon[512];
        size_t cn = (flags & TF_UCN) ? ucn_canon(L->clean.data, L->clean.len,
                                                 canon, sizeof canon) : 0;
        if (cn)         /* \u00c1, \u00C1 and \U000000C1 name one identifier */
            t->aux = intern(L->in, canon, cn)->id;
        else
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
    L->unterminated = true;
    if (L->diag)
        diag_report(L->diag, DL_ERROR, "", (SrcLoc)(start - L->region),
                    "unterminated comment");
}

/* Skip white space, comments and splices; updates bol/space.  Stops at
 * the lexer's limit: a range (a phase-B segment) is not NUL-terminated,
 * and what follows it can be anything, an unterminated comment even. */
static const char *skip_blank(Lexer *L, const char *p)
{
    for (;;) {
        unsigned char c;
        if (p >= L->lim)
            return p;
        c = (unsigned char)*p;
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
            if (L->bidi_live)
                bidi_comment(L, start + 2, p, true);
            L->space = true;
        } else if (c == '/' && p[1] == '/') {
            const char *cs = p + 2;
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
            if (L->bidi_live)
                bidi_comment(L, cs, p, false);
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
        if ((c == 'L' || c == 'u' || c == 'U') &&
            qprefix(p, L->opt.uliterals))
            goto quoted;
        q = scan_ident(p + 1, L->opt.dollar_idents);
        if (*q == '\\' || (*q == '?' && trig) || (L->hi8 && has_high_in(p, q)))
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
        if (*q == '\\' || (*q == '?' && trig) || (L->hi8 && has_high_in(p, q)))
            goto slow;
        finish_simple(L, t, TK_PPNUM, p, q, flags);
        return;
    }

    if (c == '"' || c == '\'') {
    quoted:
        {
            char quote;
            const char *qs;
            q = p + qprefix(p, L->opt.uliterals);
            quote = *q++;
            qs = q;
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
            bidi_lit(L, qs, q - 1);
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
        case '#':
            if (p[1] == '#') k = P_HASHHASH, n = 2;
            else if (p[1] == '\\' || (trig && p[1] == '?'))
                goto slow; /* maybe a spliced or trigraph ## */
            else k = P_HASH;
            break;
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
    bool hash;
    L->bol = true;
    L->space = false;
    p = skip_blank(L, L->p);
    L->p = p;
    if (!(*p == '#' || (p[0] == '%' && p[1] == ':') ||
          (L->opt.trigraphs && p[0] == '?' && p[1] == '?' && p[2] == '=')))
        return false;
    if (p[0] == '#' && p[1] != '#' && p[1] != '\\' &&
        p[1] != '?')
        return true;
    /* a line starting with ## (%:%:, spliced, trigraphs) is text: lex the
     * first token to tell, then put the cursor back */
    {
        DiagEngine *d = L->diag;
        ScratchCursor *sc = L->scratch;
        const char *lb = L->line_begin, *nl = L->nul_line;
        bool un = L->unterminated;
        Tok t;
        L->diag = NULL;
        L->scratch = NULL;
        lex_next(L, &t);
        hash = t.kind == TK_PUNCT && t.punct == P_HASH;
        L->p = p;
        L->bol = true;
        L->space = false;
        L->diag = d;
        L->scratch = sc;
        L->line_begin = lb;
        L->nul_line = nl;
        L->unterminated = un;
    }
    return hash;
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
