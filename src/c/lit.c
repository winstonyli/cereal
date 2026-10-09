/* lit.c - C literals (lit.h). */
#include "c/lit.h"

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Does v fit type k's value range? */
static bool fits(const Target *tgt, TypeKind k, uint64_t v)
{
    unsigned bits = tgt->size[k] * 8u;
    bool uns = k == TY_UINT || k == TY_ULONG || k == TY_ULLONG ||
               k == TY_UINT128;
    if (!bits)
        return false;
    if (bits >= 64)
        return uns || v <= (uint64_t)INT64_MAX;
    return v <= (uns ? (UINT64_C(1) << bits) - 1
                     : (UINT64_C(1) << (bits - 1)) - 1);
}

static void say(Lit *out, int level, const char *id, const char *fmt,
                const char *arg, int n)
{
    if (out->msg[0] && out->level >= level)
        return;
    out->level = level;
    out->id = id;
    snprintf(out->msg, sizeof out->msg, fmt, n, arg);
}

static void note(Lit *out, int level, const char *id, const char *msg)
{
    say(out, level, id, "%.*s", msg, (int)strlen(msg));
}

static void bad(Lit *out, const char *msg)
{
    out->flags |= LIT_BAD;
    say(out, 2, "", "%.*s", msg, (int)strlen(msg));
}

/* libcpp's fixed-point suffix: [u] [h | l | ll] (r | k), one case throughout. */
static bool fixed_suffix(const char *s, size_t len)
{
    size_t k = 0;
    if (k < len && (s[k] == 'u' || s[k] == 'U'))
        k++;
    if (k < len && (s[k] == 'h' || s[k] == 'H'))
        k++;
    else if (k + 1 < len && (s[k] == 'l' || s[k] == 'L') && s[k + 1] == s[k])
        k += 2;
    else if (k < len && (s[k] == 'l' || s[k] == 'L'))
        k++;
    return k + 1 == len && strchr("rRkK", s[k]);
}

static bool fixed_lit(Lit *out, const char *s, size_t len, unsigned radix)
{
    if (radix == 16 || radix == 2 || !fixed_suffix(s, len))
        return false;
    out->flags |= LIT_FIXED | LIT_BAD;
    note(out, 0, "", "fixed-point constants are a GCC extension");
    return true;
}

/* libcpp's interpret_float_suffix: the type, or TY_ERROR if invalid. */
static TypeKind float_suffix(const char *s, size_t len, bool *imag,
                             bool *nonstd, bool *unsup)
{
    size_t f = 0, d = 0, l = 0, w = 0, q = 0, i = 0, fn = 0, fnx = 0,
           bits = 0, bf16 = 0;
    *imag = *nonstd = false;
    if (len == 2 && (*s == 'd' || *s == 'D')) {
        bool up = *s == 'D';
        switch (s[1]) {
        case 'f': return up ? TY_ERROR : TY_DEC32;
        case 'F': return up ? TY_DEC32 : TY_ERROR;
        case 'd': return up ? TY_ERROR : TY_DEC64;
        case 'D': return up ? TY_DEC64 : TY_ERROR;
        case 'l': return up ? TY_ERROR : TY_DEC128;
        case 'L': return up ? TY_DEC128 : TY_ERROR;
        default: break;
        }
    }
    if (len && (s[len - 1] == 'k' || s[len - 1] == 'K' ||
                s[len - 1] == 'r' || s[len - 1] == 'R'))
        return TY_ERROR; /* fixed-point: not supported by the target */
    while (len--) {
        switch (s[0]) {
        case 'f': case 'F':
            f++;
            if (len > 0 && s[1] >= '1' && s[1] <= '9' && bits == 0) {
                f--;
                while (len > 0 && s[1] >= '0' && s[1] <= '9' && bits < 1000) {
                    bits = bits * 10 + (size_t)(s[1] - '0');
                    len--;
                    s++;
                }
                if (len > 0 && s[1] == 'x') {
                    fnx++;
                    len--;
                    s++;
                } else {
                    fn++;
                }
            }
            break;
        case 'b': case 'B':
            if (len > 2 && s[1] == (s[0] == 'b' ? 'f' : 'F') && s[2] == '1' &&
                s[3] == '6') {
                bf16++;
                len -= 3;
                s += 3;
                break;
            }
            return TY_ERROR;
        case 'd': case 'D': d++; break;
        case 'l': case 'L': l++; break;
        case 'w': case 'W': w++; break;
        case 'q': case 'Q': q++; break;
        case 'i': case 'I': case 'j': case 'J': i++; break;
        default: return TY_ERROR;
        }
        s++;
    }
    if (f + d + l + w + q + fn + fnx + bf16 > 1 || i > 1)
        return TY_ERROR;
    if (fnx && bits != 32 && bits != 64 && bits != 128)
        return TY_ERROR;
    if (fn && ((bits != 16 && bits % 32 != 0) || bits == 96 || bits > 128))
        return TY_ERROR;
    *imag = i != 0;
    *unsup = false;
    *nonstd = w || q || fn || fnx || bf16;
    if (f) return TY_FLOAT;
    if (d) return TY_DOUBLE;
    if (l) return TY_LDOUBLE;
    if (w) return TY_LDOUBLE; /* __float80 is long double on x86 */
    if (q) return TY_FLOAT128;
    if (fn) return bits == 16 ? TY_FLOAT16 : bits == 32 ? TY_FLOAT32
                   : bits == 64 ? TY_FLOAT64 : TY_FLOAT128;
    if (fnx) {
        *unsup = bits == 128;   /* x86 has no _Float128x */
        return bits == 32 ? TY_FLOAT32X : TY_FLOAT64X;
    }
    if (bf16) return TY_BF16;
    return TY_VOID; /* none: double */
}

/* libcpp's interpret_int_suffix: false if invalid. */
static bool int_suffix(const char *s, size_t len, bool *u, int *l, bool *imag)
{
    size_t k = len;
    int nu = 0, nl = 0, ni = 0;
    while (k--) {
        switch (s[k]) {
        case 'u': case 'U': nu++; break;
        case 'i': case 'I': case 'j': case 'J': ni++; break;
        case 'l': case 'L':
            nl++;
            if (nl == 2 && s[k] != s[k + 1])
                return false;
            break;
        default:
            return false;
        }
    }
    if (nl > 2 || nu > 1 || ni > 1)
        return false;
    *u = nu != 0;
    *l = nl;
    *imag = ni != 0;
    return true;
}

static bool isdig(int c) { return c >= '0' && c <= '9'; }

void lit_number(const Target *tgt, const char *s, size_t n, Lit *out)
{
    const char *str = s, *limit = s + n, *digits;
    unsigned radix = 10, max_digit = 0;
    enum { NOT_FLOAT, AFTER_POINT, AFTER_EXPON } ff = NOT_FLOAT;
    bool seen_digit = false;
    memset(out, 0, sizeof *out);
    out->ty = TY_INT;
    if (n == 1) {
        out->v = (uint64_t)(*s - '0');
        return;
    }
    if (*str == '0') {
        radix = 8;
        str++;
        if ((*str == 'x' || *str == 'X') && str + 1 < limit &&
            (str[1] == '.' || hexval((unsigned char)str[1]) >= 0)) {
            radix = 16;
            str++;
        } else if ((*str == 'b' || *str == 'B') && str + 1 < limit &&
                   (str[1] == '0' || str[1] == '1')) {
            radix = 2;
            str++;
        }
    }
    digits = str;
    while (str < limit) {
        unsigned c = (unsigned char)*str++;
        if (isdig((int)c) || (radix == 16 && hexval((int)c) >= 0)) {
            seen_digit = true;
            c = (unsigned)hexval((int)c);
            if (c > max_digit)
                max_digit = c;
        } else if (c == '.') {
            if (ff == NOT_FLOAT) {
                ff = AFTER_POINT;
            } else {
                bad(out, "too many decimal points in number");
                return;
            }
        } else if ((radix <= 10 && (c == 'e' || c == 'E')) ||
                   (radix == 16 && (c == 'p' || c == 'P'))) {
            ff = AFTER_EXPON;
            break;
        } else {
            str--;
            break;
        }
    }
    if (ff != NOT_FLOAT && radix == 8)
        radix = 10;
    if (max_digit >= radix) {
        char d = (char)('0' + max_digit);
        say(out, 2, "", radix == 2 ? "invalid digit \"%.*s\" in binary constant"
                                   : "invalid digit \"%.*s\" in octal constant",
            &d, 1);
        out->flags |= LIT_BAD;
        return;
    }
    if (ff != NOT_FLOAT) {
        bool imag, nonstd, unsup;
        TypeKind t;
        char buf[128], *copy = buf;
        size_t m;
        if (radix == 2) {
            bad(out, "invalid prefix \"0b\" for floating constant");
            return;
        }
        if (radix == 16 && !seen_digit) {
            bad(out, "no digits in hexadecimal floating constant");
            return;
        }
        if (ff == AFTER_EXPON) {
            if (str < limit && (*str == '+' || *str == '-'))
                str++;
            if (str >= limit || !isdig(*str)) {
                bad(out, "exponent has no digits");
                return;
            }
            while (str < limit && isdig(*str))
                str++;
        } else if (radix == 16) {
            bad(out, "hexadecimal floating constants require an exponent");
            return;
        }
        if (fixed_lit(out, str, (size_t)(limit - str), radix))
            return;
        t = float_suffix(str, (size_t)(limit - str), &imag, &nonstd, &unsup);
        if (t == TY_ERROR) {
            say(out, 2, "", "invalid suffix \"%.*s\" on floating constant",
                str, (int)(limit - str));
            out->flags |= LIT_BAD;
            return;
        }
        if ((t == TY_DEC32 || t == TY_DEC64 || t == TY_DEC128) &&
            radix != 10) {
            say(out, 2, "",
                "invalid suffix \"%.*s\" with hexadecimal floating constant",
                str, (int)(limit - str));
            out->flags |= LIT_BAD;
            return;
        }
        out->flags |= LIT_FLOAT | (imag ? LIT_IMAGINARY : 0);
        out->ty = t == TY_VOID ? TY_DOUBLE : t;
        if (unsup || !tgt->size[out->ty]) {
            bad(out, "unsupported non-standard suffix on floating constant");
            out->id = "inputloc";   /* gcc reports it at input_location */
            out->ty = TY_DOUBLE;
            return;
        }
        if (t == TY_DEC32 || t == TY_DEC64 || t == TY_DEC128)
            note(out, 0, "c2x", "decimal float constants are a C2X feature");
        else if (t == TY_DOUBLE && !imag && !nonstd &&
                 (memchr(str, 'd', (size_t)(limit - str)) ||
                  memchr(str, 'D', (size_t)(limit - str))))
            note(out, 0, "pedantic", "suffix for double constant is a GCC extension");
        if (nonstd)
            note(out, 0, "inputloc", "non-standard suffix on floating constant");
        else if (imag)
            note(out, 0, "pedantic", "imaginary constants are a GCC extension");
        m = (size_t)(str - s);
        if (m + 1 > sizeof buf)
            copy = xmalloc(m + 1);
        memcpy(copy, s, m);
        copy[m] = 0;
        out->f = strtold(copy, NULL);
        if (copy != buf)
            free(copy);
        return;
    }
    {
        bool u = false, imag = false, over = false;
        int nl = 0;
        uint64_t v = 0;
        const char *q;
        static const TypeKind dec[] = {TY_INT, TY_LONG, TY_LLONG};
        static const TypeKind other[] = {TY_INT, TY_UINT, TY_LONG, TY_ULONG,
                                         TY_LLONG, TY_ULLONG};
        const TypeKind *list;
        size_t nlist, k;
        if (fixed_lit(out, str, (size_t)(limit - str), radix))
            return;
        if (!int_suffix(str, (size_t)(limit - str), &u, &nl, &imag)) {
            say(out, 2, "", "invalid suffix \"%.*s\" on integer constant", str,
                (int)(limit - str));
            out->flags |= LIT_BAD;
            return;
        }
        if (imag) {
            out->flags |= LIT_IMAGINARY;
            note(out, 0, "pedantic", "imaginary constants are a GCC extension");
        }
        if (radix == 2)
            note(out, 0, "c2x", "binary constants are a C2X feature or GCC extension");
        for (q = digits; q < str; q++) {
            unsigned d = (unsigned)hexval((unsigned char)*q);
            if (v > (UINT64_MAX - d) / radix)
                over = true;
            v = v * radix + d;
        }
        out->v = v;
        if (over) {
            note(out, 1, "", "integer constant is too large for its type");
            out->flags |= LIT_WIDE | LIT_TOO_LARGE;
            out->v = 0;
            out->ty = tgt->size[TY_INT128] ? (u ? TY_UINT128 : TY_INT128)
                                           : TY_ULLONG;
            return;
        }
        if (!u && radix == 10 && v > (uint64_t)INT64_MAX) {
            note(out, 1, "", "integer constant is so large that it is unsigned");
            out->flags |= LIT_UNSIGNED_WARN;
        }
        if (u) {
            static const TypeKind ul[] = {TY_UINT, TY_ULONG, TY_ULLONG};
            list = ul + (nl > 2 ? 2 : nl);
            nlist = (size_t)(3 - nl);
        } else if (radix == 10) {
            list = dec + nl;
            nlist = (size_t)(3 - nl);
        } else {
            list = other + 2 * nl;
            nlist = (size_t)(6 - 2 * nl);
        }
        for (k = 0; k < nlist; k++)
            if (fits(tgt, list[k], v)) {
                out->ty = list[k];
                return;
            }
        /* beyond long long: __int128 where the target has it */
        if (tgt->size[TY_INT128]) {
            out->ty = u || radix != 10 ? TY_UINT128 : TY_INT128;
            if (radix != 10 && !u)
                out->ty = TY_INT128; /* MIN(itk_u, itk_s) */
            out->flags |= LIT_WIDE;
        } else {
            out->ty = TY_ULLONG;
        }
    }
}

/* The code point of a Unicode character name; the bidi controls are the
 * names known, any other is taken to be some character past U+00FF. */
uint32_t lit_named_ucn(const char *s, const char *e)
{
    static const struct { const char *name; uint32_t cp; } t[] = {
        {"LEFT-TO-RIGHT EMBEDDING", 0x202A}, {"RIGHT-TO-LEFT EMBEDDING", 0x202B},
        {"POP DIRECTIONAL FORMATTING", 0x202C},
        {"LEFT-TO-RIGHT OVERRIDE", 0x202D}, {"RIGHT-TO-LEFT OVERRIDE", 0x202E},
        {"LEFT-TO-RIGHT ISOLATE", 0x2066}, {"RIGHT-TO-LEFT ISOLATE", 0x2067},
        {"FIRST STRONG ISOLATE", 0x2068}, {"POP DIRECTIONAL ISOLATE", 0x2069},
    };
    size_t k, n = (size_t)(e - s);
    for (k = 0; k < sizeof t / sizeof *t; k++)
        if (strlen(t[k].name) == n && !strncasecmp(t[k].name, s, n))
            return t[k].cp;
    return 0x100;
}

/* One character (or escape) of a char or string literal body; returns
 * its value and advances *p. */
uint32_t lit_char_one(const char **p, const char *end, bool wide,
                        bool *ucn)
{
    const char *s = *p;
    uint32_t c;
    *ucn = false;
    if (*s != '\\') {
        unsigned char b = (unsigned char)*s++;
        if (wide && b >= 0x80) { /* UTF-8 source */
            int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
            c = b & (0x3Fu >> extra);
            while (extra-- > 0 && s < end)
                c = (c << 6) | ((unsigned char)*s++ & 0x3F);
        } else {
            c = b;
        }
        *p = s;
        return c;
    }
    s++;
    if (s >= end) {
        *p = s;
        return '\\';
    }
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
        while (s < end && hexval((unsigned char)*s) >= 0)
            c = c * 16 + (uint32_t)hexval((unsigned char)*s++);
        break;
    case 'u': case 'U': {
        int k = *s == 'u' ? 4 : 8;
        s++;
        c = 0;
        if (s < end && *s == '{') {        /* \u{...}: any number of digits */
            s++;
            while (s < end && hexval((unsigned char)*s) >= 0)
                c = c << 4 | (uint32_t)hexval((unsigned char)*s++);
            if (s < end && *s == '}')
                s++;
        } else {
            while (k-- > 0 && s < end && hexval((unsigned char)*s) >= 0)
                c = c * 16 + (uint32_t)hexval((unsigned char)*s++);
        }
        *ucn = true;
        break;
    }
    case 'N':
        if (s + 1 < end && s[1] == '{') {  /* \N{NAME}: only the bidi names */
            const char *e = memchr(s, '}', (size_t)(end - s));
            c = lit_named_ucn(s + 2, e ? e : end);
            s = e ? e + 1 : end;
            *ucn = true;
        } else {
            c = (unsigned char)*s++;
        }
        break;
    default:
        if (*s >= '0' && *s <= '7') {
            int k = 0;
            c = 0;
            while (k++ < 3 && s < end && *s >= '0' && *s <= '7')
                c = c * 8 + (uint32_t)(*s++ - '0');
        } else {
            c = (unsigned char)*s++;
        }
    }
    *p = s;
    return c;
}

static unsigned utf8_len(uint32_t c)
{
    return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
}

void lit_char(const Target *tgt, const char *s, size_t n, Lit *out)
{
    const char *end = s + n, *q;
    int prefix = 0;
    unsigned nchars = 0, bits;
    uint64_t v = 0;
    memset(out, 0, sizeof *out);
    out->ty = TY_INT;
    if (*s == 'L' || *s == 'u' || *s == 'U') {
        prefix = *s++;
        if (prefix == 'u' && *s == '8')
            prefix = '8', s++;
    }
    if (prefix == 'L')
        out->ty = tgt->wchar_type;
    else if (prefix == 'u')
        out->ty = tgt->char16_type;
    else if (prefix == 'U')
        out->ty = tgt->char32_type;
    else if (prefix == '8')
        out->ty = TY_UCHAR;
    if (end > s && end[-1] == '\'')
        end--;
    q = s + 1;
    bits = tgt->size[out->ty] * 8u;
    while (q < end) {
        bool ucn;
        bool wide = prefix && prefix != '8';
        uint32_t c = lit_char_one(&q, end, wide, &ucn);
        if (!wide && ucn) {
            /* a UCN in a narrow constant: its UTF-8 bytes */
            unsigned k, len = utf8_len(c);
            for (k = 0; k < len; k++) {
                unsigned sh = 6 * (len - 1 - k);
                unsigned b = len == 1 ? c
                             : k == 0 ? ((0xF00u >> len) & 0xFF) | (c >> sh)
                                      : 0x80 | ((c >> sh) & 0x3F);
                v = (v << 8) | (b & 0xFF);
            }
            nchars += len;
        } else {
            if (prefix == 'u' && c > 0xFFFF) {  /* a surrogate pair */
                nchars++;
                c = 0xDC00 | ((c - 0x10000) & 0x3FF);
            }
            nchars++;
            v = wide ? c : (v << 8) | (c & 0xFF);
        }
    }
    if (nchars == 0) {
        bad(out, "empty character constant");
        return;
    }
    if (prefix && prefix != '8') {
        if (nchars > 1)
            note(out, 1, "", "character constant too long for its type");
        if (bits < 64)
            v &= (UINT64_C(1) << bits) - 1;
        if (bits < 64 && (out->ty == TY_INT || out->ty == TY_LONG ||
                          out->ty == TY_SHORT) &&
            (v >> (bits - 1)) & 1)
            v |= ~((UINT64_C(1) << bits) - 1);
    } else if (nchars <= 1) {
        v &= 0xFF;
        if (!prefix && tgt->char_signed && (v & 0x80))
            v |= ~UINT64_C(0xFF);
    } else {
        unsigned maxc = tgt->size[TY_INT];
        if (prefix == '8')
            maxc = 1;
        if (nchars > maxc)
            note(out, prefix == '8' ? 2 : 1, "",
                 "character constant too long for its type");
        else
            note(out, 1, "multichar", "multi-character character constant");
        out->flags |= LIT_MULTICHAR;
        if (prefix == '8') {
            v &= 0xFF;
        } else { /* int width, sign-extended */
            v &= 0xFFFFFFFFu;
            if (v & 0x80000000u)
                v |= ~UINT64_C(0xFFFFFFFF);
        }
    }
    out->v = v;
}

int lit_str_prefix(const char *s, size_t n)
{
    if (n > 2 && s[0] == 'u' && s[1] == '8')
        return '8';
    if (n > 1 && (s[0] == 'L' || s[0] == 'u' || s[0] == 'U'))
        return s[0];
    return 0;
}

void lit_escape_diags(const char *s, size_t n, bool pedantic, LitEscFn fn,
                      void *ctx)
{
    int prefix = lit_str_prefix(s, n);
    const char *q = s + (prefix == '8' ? 2 : prefix ? 1 : 0), *end = s + n;
    unsigned bits = prefix == 'u' ? 16 : (prefix == 'L' || prefix == 'U') ? 32 : 8;
    uint32_t mask = bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
    char quote = n ? *q : 0;
    if (quote != '"' && quote != '\'')
        return;
    q++;
    if (end > q && end[-1] == quote)
        end--;
    while (q < end) {
        char msg[96];
        int c;
        if (*q++ != '\\' || q >= end)
            continue;
        c = (unsigned char)*q++;
        if (c == 'x') {
            uint32_t v = 0;
            bool ov = false, delim = false;
            int nd = 0;
            const char *base = q - 2;
            if (q < end && *q == '{') {
                delim = true;
                q++;
            }
            for (; q < end && isxdigit((unsigned char)*q); q++, nd++) {
                ov |= (v ^ (v << 4 >> 4)) != 0;
                v = (v << 4) + (uint32_t)(isdigit((unsigned char)*q)
                                          ? *q - '0' : (*q | 32) - 'a' + 10);
            }
            if (delim && q < end && *q == '}') {
                q++;
                if (!nd) {
                    fn(ctx, 2, "empty delimited escape sequence");
                    continue;
                }
                if (pedantic)
                    fn(ctx, 1, "delimited escape sequences are only valid "
                       "in C++23");
                delim = false;
            }
            if (!nd) {
                fn(ctx, 2, "\\x used with no following hex digits");
                continue;
            }
            if (delim) {
                snprintf(msg, sizeof msg, "'\\x{' not terminated with '}' "
                         "after %.*s", (int)(q - base), base);
                fn(ctx, 2, msg);
                continue;
            }
            if (ov || v != (v & mask))
                fn(ctx, 1, "hex escape sequence out of range");
        } else if ((c >= '0' && c <= '7') || c == 'o') {
            uint32_t v = 0;
            bool ov = false, delim = false;
            int count = 0;
            const char *base = q - 2;
            q--;
            if (*q == 'o') {
                q++;
                if (q >= end || *q != '{')
                    fn(ctx, 2, "'\\o' not followed by '{'");
                else {
                    q++;
                    delim = true;
                }
            }
            while (q < end && count++ < 3 && *q >= '0' && *q <= '7') {
                if (delim) {
                    count = 2;
                    ov |= (v ^ (v << 3 >> 3)) != 0;
                }
                v = (v << 3) + (uint32_t)(*q++ - '0');
            }
            if (delim) {
                if (q < end && *q == '}') {
                    q++;
                    if (count == 1) {
                        fn(ctx, 2, "empty delimited escape sequence");
                        continue;
                    }
                    if (pedantic)
                        fn(ctx, 1, "delimited escape sequences are only "
                           "valid in C++23");
                } else {
                    snprintf(msg, sizeof msg, "'\\o{' not terminated with '}' "
                             "after %.*s", (int)(q - base), base);
                    fn(ctx, 2, msg);
                    continue;
                }
            }
            if (ov || v != (v & mask))
                fn(ctx, 1, "octal escape sequence out of range");
        } else if (strchr("\\'\"?abfnrtvuUN", c)) {
            continue;
        } else if (c == 'e' || c == 'E') {
            if (pedantic) {
                snprintf(msg, sizeof msg, "non-ISO-standard escape sequence, '\\%c'", c);
                fn(ctx, 1, msg);
            }
        } else if (strchr("({[%", c) && !pedantic) {
            continue;
        } else {
            if (isgraph(c))
                snprintf(msg, sizeof msg, "unknown escape sequence: '\\%c'", c);
            else
                snprintf(msg, sizeof msg, "unknown escape sequence: '\\%03o'", c);
            fn(ctx, 1, msg);
        }
    }
}

void lit_str_units(const char *s, size_t n, unsigned width, uint64_t *units)
{
    const char *end = s + n, *q;
    int prefix = lit_str_prefix(s, n);
    q = s + (prefix == '8' ? 3 : prefix ? 2 : 1);
    if (end > q && end[-1] == '"')
        end--;
    while (q < end) {
        bool ucn;
        const char *at = q;
        uint32_t c;
        if (width == 1) {
            if (*q != '\\') { /* bytes as they are */
                q++;
                (*units)++;
                continue;
            }
            c = lit_char_one(&q, end, false, &ucn);
            *units += ucn ? utf8_len(c) : 1;
            continue;
        }
        c = lit_char_one(&q, end, true, &ucn);
        if (width == 2 && c >= 0x10000 && (ucn || *at != '\\'))
            *units += 2; /* a surrogate pair */
        else
            (*units)++;
    }
}
