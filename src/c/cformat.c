/* cformat.c - printf/scanf format checking against the argument types
 * (gcc 13 c-format.cc), strlen constant folding and string reads.
 * Split out of cexpr.c; shares its helpers through cexpr_int.h. */
#include "c/cexpr_int.h"
#include "c/fuzzy.h"

#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>

/* The position of the format argument of the printf-like library functions
 * (gcc's built-in attributes); 0: not one. */
uint32_t builtin_format_pos(const char *name)
{
    static const struct { const char *n; uint32_t pos; } t[] = {
        {"printf", 1}, {"fprintf", 2}, {"sprintf", 2}, {"snprintf", 3},
        {"dprintf", 2}, {"printf_unlocked", 1}, {"fprintf_unlocked", 2},
        {"vprintf", 1}, {"vfprintf", 2}, {"vsprintf", 2}, {"vsnprintf", 3},
        {"vdprintf", 2}};
    size_t m;
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    for (m = 0; m < sizeof t / sizeof *t; m++)
        if (!strcmp(name, t[m].n))
            return t[m].pos;
    return 0;
}

/* ---- -Wformat: the conversions of a printf-style literal against its
 * arguments (gcc's check_format_info for gnu_printf). ---- */

enum { FW_INT, FW_FLT, FW_LDBL, FW_PCHAR, FW_PVOID, FW_PINT, FW_ANY,
       FW_PFLT, FW_PPCHAR, FW_PPVOID };
enum { FL_NONE, FL_HH, FL_H, FL_L, FL_LL, FL_BIGL, FL_Z, FL_T, FL_J };

typedef struct FmtWant {
    int shape;
    int cls;                    /* FW_INT, FW_PINT: fmt_cls of the integer */
    char name[40];              /* as the message spells the wanted type */
    bool write;                 /* the argument is written through */
    bool uns;                   /* FW_PINT: an unsigned integer */
    int sgn;                    /* FW_INT: 1 signed, 2 unsigned conversion */
} FmtWant;

/* An integer type's width class, signedness ignored (0: not an integer). */
static int fmt_cls(TypeKind k)
{
    switch (k) {
    case TY_CHAR: case TY_SCHAR: case TY_UCHAR: return 1;
    case TY_SHORT: case TY_USHORT: return 2;
    case TY_INT: case TY_UINT: return 3;
    case TY_LONG: case TY_ULONG: return 4;
    case TY_LLONG: case TY_ULLONG: return 5;
    case TY_INT128: case TY_UINT128: return 6;
    default: return 0;
    }
}

static bool fmt_unsigned_kind(TypeKind k)
{
    return k == TY_UCHAR || k == TY_USHORT || k == TY_UINT || k == TY_ULONG ||
           k == TY_ULLONG || k == TY_UINT128;
}

/* What conversion `conv` with length `len` takes; false: no argument.  scan:
 * scanf's (pointers to the objects), mflag: its 'm' allocation flag. */
static bool fmt_want(Checker *c, char conv, int len, bool scan, bool mflag,
                     FmtWant *w)
{
    bool uns = conv == 'o' || conv == 'u' || conv == 'x' || conv == 'X';
    bool ptr = conv == 'n' || scan;
    const char *nm;
    w->cls = 3;
    w->write = ptr;
    w->uns = uns && scan;
    w->sgn = 0;
    switch (conv) {
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'n':
        w->shape = ptr ? FW_PINT : FW_INT;
        switch (len) {
        case FL_NONE: nm = uns ? "unsigned int" : "int"; break;
        case FL_HH: case FL_H:
            if (ptr) {
                w->cls = len == FL_HH ? 1 : 2;
                nm = len == FL_HH ? (uns ? "unsigned char" : "signed char")
                                  : (uns ? "short unsigned int" : "short int");
            } else {
                nm = "int";
            }
            break;
        case FL_L: w->cls = 4; nm = uns ? "long unsigned int" : "long int";
            break;
        case FL_LL: case FL_BIGL: w->cls = 5;
            nm = uns ? "long long unsigned int" : "long long int";
            break;
        case FL_Z: w->cls = fmt_cls(c->tgt->size_type);
            nm = uns ? "size_t" : "signed size_t"; break;
        case FL_T: w->cls = fmt_cls(c->tgt->ptrdiff_type);
            nm = uns ? "unsigned ptrdiff_t" : "ptrdiff_t"; break;
        default: w->cls = fmt_cls(c->tgt->intmax_type);
            nm = uns ? "uintmax_t" : "intmax_t"; break;
        }
        /* %hhu / %hu take an int: no complaint about a signed argument */
        w->sgn = ptr || (uns && (len == FL_H || len == FL_HH)) ? 0 : uns ? 2 : 1;
        snprintf(w->name, sizeof w->name, "%s%s", nm, ptr ? " *" : "");
        return true;
    case 'c': case 'C': case '[':
        if (len == FL_L || conv == 'C') {
            w->shape = scan ? FW_PINT : FW_INT;
            w->cls = fmt_cls(scan ? c->tgt->wchar_type : c->tgt->wint_type);
            snprintf(w->name, sizeof w->name, scan ? "wchar_t *" : "wint_t");
        } else if (scan) {
            w->shape = mflag ? FW_PPCHAR : FW_PCHAR;
            snprintf(w->name, sizeof w->name, mflag ? "char **" : "char *");
        } else {
            w->shape = FW_INT;
            w->sgn = 1;
            snprintf(w->name, sizeof w->name, "int");
        }
        return true;
    case 's': case 'S':
        if (len == FL_L || conv == 'S') {
            w->shape = FW_PINT;
            w->cls = fmt_cls(c->tgt->wchar_type);
            snprintf(w->name, sizeof w->name, "wchar_t *");
        } else {
            w->shape = mflag ? FW_PPCHAR : FW_PCHAR;
            snprintf(w->name, sizeof w->name, mflag ? "char **" : "char *");
        }
        return true;
    case 'p':
        w->shape = scan ? FW_PPVOID : FW_PVOID;
        snprintf(w->name, sizeof w->name, scan ? "void **" : "void *");
        return true;
    case 'm':
        return false;
    default:
        if (scan) {
            w->shape = FW_PFLT;
            w->cls = len == FL_BIGL ? 3 : len == FL_L ? 2 : 1;
            snprintf(w->name, sizeof w->name, "%s *",
                     w->cls == 3 ? "long double" : w->cls == 2 ? "double"
                                                                : "float");
            return true;
        }
        w->shape = len == FL_BIGL ? FW_LDBL : FW_FLT;
        snprintf(w->name, sizeof w->name, "%s",
                 len == FL_BIGL ? "long double" : "double");
        return true;
    }
}

/* The type an argument has when passed: arrays decayed, the default
 * promotions applied (the typedef spelling kept when they change nothing). */
static TypeId fmt_argtype(Checker *c, uint32_t a)
{
    TypeId t = rvt(c, a), p;
    if (type_ckind(TT, t) == TY_FLOAT)
        return TYPE_B(DOUBLE);
    if (is_int(c, t)) {
        p = promoted(c, a);
        if (p != TYPE_UNQUAL(type_canon(TT, t)))
            return p;
    }
    return unqual(c, t);
}

/* Does an argument of type t (from fmt_argtype) satisfy w? */
static bool fmt_arg_ok(Checker *c, const FmtWant *w, TypeId t)
{
    TypeKind k = type_ckind(TT, t), pk, qk;
    TypeId ct, pt;
    switch (w->shape) {
    case FW_INT: return fmt_cls(k) == w->cls;
    case FW_FLT: return k == TY_DOUBLE;
    case FW_LDBL: return k == TY_LDOUBLE;
    case FW_ANY: return true;
    default: break;
    }
    if (k != TY_PTR)
        return false;
    ct = type_canon(TT, t);
    pt = type_canon(TT, type_base(TT, ct));
    pk = type_kind(TT, TYPE_UNQUAL(pt));
    switch (w->shape) {
    case FW_PCHAR: return fmt_cls(pk) == 1;
    case FW_PVOID: return !c->opt.pedantic || pk == TY_VOID || fmt_cls(pk) == 1;
    case FW_PFLT:
        return pk == (w->cls == 3 ? TY_LDOUBLE : w->cls == 2 ? TY_DOUBLE
                                                             : TY_FLOAT);
    case FW_PPCHAR: case FW_PPVOID:
        if (pk != TY_PTR)
            return false;
        qk = type_kind(TT, TYPE_UNQUAL(type_canon(TT, type_base(TT, pt))));
        return w->shape == FW_PPVOID ? qk == TY_VOID : fmt_cls(qk) == 1;
    default:
        if (fmt_cls(pk) != w->cls)
            return false;
        if (!c->opt.pedantic)
            return true;
        if (w->cls == 1)
            return w->uns ? pk == TY_UCHAR : pk == TY_SCHAR;
        return w->uns == fmt_unsigned_kind(pk);
    }
}

/* where conversion warnings go in a format without exact columns, when
 * that differs from the format's location ("fmt + N" reports at the '+') */
static SrcLoc fmt_mloc;

typedef struct FmtCtx {
    Checker *c;
    const uint32_t *kv;
    uint32_t nk, ai;            /* the next argument to take (an index in kv) */
    SrcLoc whole, base, call;   /* the literal; its first byte (exact); the call */
    const uint32_t *off;        /* byte offsets in the spelling, if exact */
    bool exact;
    bool va;                    /* a va_list: arguments not checked */
} FmtCtx;

static SrcLoc fmt_loc(const FmtCtx *x, size_t i)
{
    return x->exact ? x->base + x->off[i] : fmt_mloc ? fmt_mloc : x->whole;
}

/* Takes the next argument for `what` (a description such as "format '%d'"),
 * checking it against w. */
static void fmt_take(FmtCtx *x, const FmtWant *w, SrcLoc loc, const char *what)
{
    Checker *c = x->c;
    uint32_t a;
    TypeId t;
    TypeKind k;
    if (x->va)
        return;
    if (x->ai >= x->nk) {
        cwarn(c, loc, "format=", "%s expects a matching '%s' argument", what,
              w->name);
        return;
    }
    a = x->kv[x->ai];
    if (node_err(c, a) || c->ty[a] == ERRT) {
        x->ai++;
        return;
    }
    t = fmt_argtype(c, a);
    k = type_ckind(TT, t);
    if (w->write && k == TY_PTR &&
        (tquals(c, type_base(TT, type_canon(TT, t))) & TQ_CONST))
        cwarn(c, x->call, "format=", "writing into constant object "
              "(argument %u)", x->ai);
    if (w->shape == FW_INT && w->sgn && fmt_cls(k) && fmt_arg_ok(c, w, t) &&
        diag_enabled(c->diag, "format-signedness")) {
        /* -Wformat-signedness: an unsigned type narrower than int promotes
         * to int but is still fine for either conversion */
        TypeKind ok = type_ckind(TT, rvt(c, a));
        bool small_u = ok == TY_UCHAR || ok == TY_USHORT;
        bool uk = fmt_unsigned_kind(k);
        if (!small_u && k != TY_CHAR && (w->sgn == 2) != uk)
            cwarn(c, loc, "format=", "%s expects argument of type '%s', but "
                  "argument %u has type %s", what, w->name, x->ai,
                  type_q(TT, t));
    }
    if ((fmt_cls(k) || k == TY_DOUBLE || k == TY_LDOUBLE || k == TY_PTR ||
         is_record(c, t)) && !fmt_arg_ok(c, w, t))
        cwarn(c, loc, "format=", "%s expects argument of type '%s', but "
              "argument %u has type %s", what, w->name, x->ai, type_q(TT, t));
    x->ai++;
}

/* The bytes of a format literal (its pieces concatenated and unescaped),
 * with each byte's offset in its piece's spelling when the literal is a
 * single piece written in the file. */
static bool fmt_decode(Checker *c, uint32_t s, char **buf, uint32_t **off,
                       size_t *n, bool *exact)
{
    uint32_t np = c->nodes[s].aux ? c->nodes[s].aux : 1, t;
    size_t cap = 1, m = 0, len, j;
    const char *tx;
    for (t = 0; t < np; t++) {
        tx = ttext(c, c->nodes[s].tok + t, &len);
        cap += len;
    }
    *buf = malloc(cap);
    *off = malloc(cap * sizeof **off);
    /* a token a macro expansion made has no substring location in gcc */
    *exact = np == 1 && (!c->u->toks[c->nodes[s].tok].exp ||
                         c->u->toks[c->nodes[s].tok].exp ==
                             c->u->toks[c->nodes[s].tok].t.loc);
    for (t = 0; t < np; t++) {
        tx = ttext(c, c->nodes[s].tok + t, &len);
        if (lit_str_prefix(tx, len) || len < 2)
            goto bad;
        for (j = 1; j + 1 < len; j++) {
            unsigned char ch = (unsigned char)tx[j];
            size_t at = j;
            if (ch >= 0x80 || ch == '\t')
                *exact = false;
            if (ch == '\\') {
                ch = (unsigned char)tx[++j];
                switch (ch) {
                case 'n': ch = '\n'; break;
                case 't': ch = '\t'; break;
                case 'r': ch = '\r'; break;
                case 'a': ch = '\a'; break;
                case 'b': ch = '\b'; break;
                case 'f': ch = '\f'; break;
                case 'v': ch = '\v'; break;
                case 'e': ch = 27; break;
                case '\\': case '\'': case '"': case '?': break;
                case 'x': {
                    unsigned v = 0;
                    while (j + 2 < len && isxdigit((unsigned char)tx[j + 1])) {
                        char d = tx[++j];
                        v = v * 16 + (unsigned)(d <= '9' ? d - '0'
                                                : (d | 32) - 'a' + 10);
                    }
                    ch = (unsigned char)v;
                    break;
                }
                case '0': case '1': case '2': case '3': case '4': case '5':
                case '6': case '7': {
                    unsigned v = (unsigned)(ch - '0'), d = 1;
                    while (d < 3 && j + 2 < len && tx[j + 1] >= '0' &&
                           tx[j + 1] <= '7') {
                        v = v * 8 + (unsigned)(tx[++j] - '0');
                        d++;
                    }
                    ch = (unsigned char)v;
                    break;
                }
                default: goto bad;
                }
            }
            (*buf)[m] = (char)ch;
            (*off)[m++] = (uint32_t)at;
        }
    }
    *n = m;
    return true;
bad:
    free(*buf);
    free(*off);
    return false;
}

/* The conversions of a literal format s of a printf-like (or, scan, scanf-like)
 * function against the arguments kv[first..nk) (first 0: a va_list, the
 * arguments are not checked). */
static void fmt_check(Checker *c, const uint32_t *kv, uint32_t nk,
                      uint32_t first, bool scan, uint32_t s, SrcLoc whole,
                      SrcLoc call, size_t skip, const StrInit *si)
{
    static const struct { char conv; const char *flags; } ft[] = {
        {'d', "-+ 0'I"}, {'i', "-+ 0'I"}, {'o', "-0#"}, {'x', "-0#"},
        {'X', "-0#"}, {'u', "-0'I"}, {'f', "-0 +#'I"}, {'g', "-0 +#'I"},
        {'G', "-0 +#'I"}, {'e', "-0 +#I"}, {'E', "-0 +#I"}, {'a', "-0 +#I"},
        {'A', "-0 +#I"}, {'F', "-0 +#I"}, {'c', "-"}, {'C', "-"}, {'s', "-"},
        {'S', "-"}, {'p', "-"}, {'n', ""}};
    const char *kname = scan ? "gnu_scanf" : "gnu_printf";
    const char *convs = scan ? "diouxXaAeEfFgGcspnCS[" : "diouxXfFeEgGaAcsCSpnm";
    FmtCtx x;
    char *f;
    uint32_t *off;
    size_t n, i = 0, t, st;
    bool exact, dollar = false;
    if (!diag_enabled(c->diag, "format="))
        return;
    if (si) {
        n = si->n;
        f = malloc(n + 1);
        off = calloc(n + 1, sizeof *off);
        memcpy(f, si->b, n);
        exact = false;
    } else if (!fmt_decode(c, s, &f, &off, &n, &exact))
        return;
    if (skip) {                 /* "%d%d" + 2: gcc keeps the old columns */
        if (skip > n)
            skip = n;
        memmove(f, f + skip, n - skip + 1 > 0 ? n - skip : 0);
        n -= skip;
    }
    x.c = c;
    x.kv = kv;
    x.nk = nk;
    x.va = first == 0;
    x.ai = first ? first : nk;
    x.whole = whole;
    x.call = call;
    x.exact = false;
    x.base = 0;
    if (exact) {
        size_t len;
        const char *tx = ttext(c, c->nodes[s].tok, &len);
        SrcLoc b = ctok_loc(c, c->nodes[s].tok);
        /* a parenthesized literal has only its parenthesis's location */
        if (whole == b && !memcmp(srcmgr_ptr(c->sm, b), tx, len)) {
            x.exact = true;
            x.base = b;
        }
    }
    x.off = off;
    for (t = 0; t < n; t++)
        if (!f[t]) {
            cwarn(c, fmt_loc(&x, t), "format-contains-nul",
                  "embedded '\\0' in format");
            n = t;
            break;
        }
    if (!n) {
        cwarn(c, whole, "format-zero-length", "zero-length %s format string",
              kname);
        goto out;
    }
    while (i < n && !dollar) {
        char seen[128] = {0}, conv, flags[16];
        unsigned nf = 0;
        bool width = false, prec = false, supp = false, mflag = false;
        int len = FL_NONE;
        bool badlen = false;
        char lsp[3] = {0, 0, 0};
        FmtWant w;
        char what[48];
        if (f[i] != '%') {
            i++;
            continue;
        }
        st = i++;
        if (i >= n) {
            cwarn(c, fmt_loc(&x, st), "format=",
                  "spurious trailing '%%' in format");
            break;
        }
        while (i < n && strchr(scan ? "*'m" : "-+ #0'I", f[i])) {
            if (seen[(int)f[i]])
                cwarn(c, fmt_loc(&x, i), "format=",
                      "repeated '%c' flag in format", f[i]);
            else if (nf < sizeof flags - 1)
                flags[nf++] = f[i];
            if (c->opt.pedantic && (f[i] == '\'' || f[i] == 'I'))
                cwarn(c, whole, "format=", "ISO C does not support the '%c' "
                      "%s flag", f[i], scan ? "scanf" : "printf");
            seen[(int)f[i]] = 1;
            supp |= scan && f[i] == '*';
            mflag |= scan && f[i] == 'm';
            i++;
        }
        flags[nf] = 0;
        if (!scan && i < n && f[i] == '*') {
            FmtWant iw = {FW_INT, 3, "int", false, false, 0};
            fmt_take(&x, &iw, fmt_loc(&x, i), "field width specifier '*'");
            width = true;
            i++;
        } else {
            while (i < n && isdigit((unsigned char)f[i])) {
                width = true;
                i++;
            }
            if (width && i < n && f[i] == '$') {
                if (c->opt.pedantic)
                    cwarn(c, call, "format=", "ISO C does not support %%n$ "
                          "operand number formats");
                dollar = true;
                break;
            }
        }
        if (!scan && i < n && f[i] == '.') {
            i++;
            prec = true;
            if (i < n && f[i] == '*') {
                FmtWant iw = {FW_INT, 3, "int", false, false, 0};
                fmt_take(&x, &iw, fmt_loc(&x, i),
                         "field precision specifier '.*'");
                i++;
            } else {
                while (i < n && isdigit((unsigned char)f[i]))
                    i++;
            }
        }
        if (i < n) {
            switch (f[i]) {
            case 'h': len = i + 1 < n && f[i + 1] == 'h' ? FL_HH : FL_H; break;
            case 'l': len = i + 1 < n && f[i + 1] == 'l' ? FL_LL : FL_L; break;
            case 'L': len = FL_BIGL; break;
            case 'q': len = FL_LL; break;
            case 'z': case 'Z': len = FL_Z; break;
            case 't': len = FL_T; break;
            case 'j': len = FL_J; break;
            default: break;
            }
            if (len != FL_NONE) {
                lsp[0] = f[i];
                lsp[1] = len == FL_HH || (len == FL_LL && f[i] == 'l') ? f[i] : 0;
                i += lsp[1] ? 2 : 1;
                if ((f[i - 1] == 'q' || f[i - 1] == 'Z') && c->opt.pedantic)
                    cwarn(c, whole, "format=", "ISO C does not support the "
                          "'%c' %s length modifier", f[i - 1], kname);
            }
        }
        if (i >= n) {
            cwarn(c, fmt_loc(&x, i - 1), "format=",
                  "conversion lacks type at end of format");
            break;
        }
        conv = f[i];
        if (conv == '%') {
            if (i - 1 > st) {
                cwarn(c, fmt_loc(&x, i - 1), "format=",
                      "conversion lacks type at end of format");
                continue;       /* the '%' starts the next conversion */
            }
            i++;
            continue;
        }
        if (!conv || !strchr(convs, conv)) {
            if (isprint((unsigned char)conv))
                cwarn(c, fmt_loc(&x, i), "format=",
                      "unknown conversion type character '%c' in format",
                      conv);
            else
                cwarn(c, fmt_loc(&x, i), "format=", "unknown conversion type "
                      "character '\\x%02x' in format", (unsigned char)conv);
            i++;
            continue;
        }
        snprintf(what, sizeof what, "format '%%%s%s%c'", mflag ? "m" : "", lsp,
                 conv);
        if (mflag && c->opt.pedantic)
            cwarn(c, whole, "format=", "ISO C does not support the 'm' scanf "
                  "flag");
        if (conv == '[') {
            /* a scan set: the conversion is located at its last character */
            size_t j = i + 1, e;
            if (j < n && f[j] == '^')
                j++;
            if (j < n && f[j] == ']')
                j++;
            while (j < n && f[j] != ']')
                j++;
            e = j < n ? j - 1 : n - 1;
            if (j >= n)
                cwarn(c, fmt_loc(&x, e), "format=",
                      "no closing ']' for '%%[' format");
            snprintf(what, sizeof what, "format '%%%s%.*s'", lsp,
                     (int)(e - i + 1), f + i);
            i = e;
        }
        if (!scan) {
            for (t = 0; t < sizeof ft / sizeof *ft; t++)
                if (ft[t].conv == conv)
                    break;
            if (t < sizeof ft / sizeof *ft) {
                bool intc = strchr("diouxX", conv) != NULL;
                if (seen[' '] && seen['+'])
                    cwarn(c, whole, "format=", "' ' flag ignored with '+' flag "
                          "in %s format", kname);
                if (seen['0'] && seen['-'])
                    cwarn(c, whole, "format=", "'0' flag ignored with '-' flag "
                          "in %s format", kname);
                if (seen['0'] && prec && intc)
                    cwarn(c, whole, "format=", "'0' flag ignored with "
                          "precision and '%%%c' %s format", conv, kname);
                for (st = 0; flags[st]; st++)
                    if (!strchr(ft[t].flags, flags[st]))
                        cwarn(c, fmt_loc(&x, i), "format=", "'%c' flag used "
                              "with '%%%c' %s format", flags[st], conv, kname);
                if (width && conv == 'n')
                    cwarn(c, fmt_loc(&x, i), "format=", "field width used "
                          "with '%%%c' %s format", conv, kname);
                if (prec && !strchr("diouxXfFeEgGaAsSn", conv))
                    cwarn(c, fmt_loc(&x, i), "format=", "precision used with "
                          "'%%%c' %s format", conv, kname);
            }
        }
        {
            bool intc = strchr("diouxX", conv) != NULL, lenok;
            if (len == FL_NONE || intc || conv == 'n')
                lenok = true;
            else if (conv == 'c' || conv == 's' || conv == '[')
                lenok = len == FL_L;
            else if (strchr("fFeEgGaA", conv))
                lenok = len == FL_BIGL || len == FL_L;
            else
                lenok = conv == 'C' || conv == 'S';
            if (!lenok) {
                cwarn(c, fmt_loc(&x, i), "format=", "use of '%s' length "
                      "modifier with '%c' type character has either no "
                      "effect or undefined behavior", lsp, conv);
                len = FL_NONE;
                lsp[0] = 0;
                badlen = true;
                snprintf(what, sizeof what, "format '%%%c'", conv);
            }
            if (c->opt.pedantic && (strchr(scan ? "CS" : "mCS", conv) ||
                                    (len == FL_BIGL && intc)))
                cwarn(c, fmt_loc(&x, i), "format=", "ISO C does not support "
                      "the '%%%s%c' %s format", lsp, conv, kname);
        }
        if (supp) {
            i++;
            continue;           /* assignment suppressed: no argument */
        }
        if (fmt_want(c, conv, len, scan, mflag, &w)) {
            if (badlen)
                w.shape = FW_ANY;   /* gcc has no type for it */
            fmt_take(&x, &w, fmt_loc(&x, i), what);
        }
        i++;
    }
    if (!dollar && !x.va && x.ai < nk)
        cwarn(c, whole, "format-extra-args", "too many arguments for format");
out:
    free(f);
    free(off);
}

/* The position of the format argument of the scanf-like library functions. */
uint32_t builtin_scanf_pos(const char *name)
{
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    if (!strcmp(name, "scanf"))
        return 1;
    if (!strcmp(name, "vscanf"))
        return 1;
    return !strcmp(name, "fscanf") || !strcmp(name, "sscanf") ||
           !strcmp(name, "vfscanf") || !strcmp(name, "vsscanf") ? 2 : 0;
}

/* A library function taking a va_list: gcc's built-in format attribute has
 * first_arg_num 0. */
static bool builtin_is_va(const char *name)
{
    if (!strncmp(name, "__builtin_", 10))
        name += 10;
    return name[0] == 'v';
}

/* check_function_format's -Wsuggest-attribute=format: a call with a va_list
 * and a format that is not a literal, in a function that has a char *
 * parameter but no format attribute of its own for that kind. */
static void suggest_format(Checker *c, bool scan, SrcLoc where)
{
    const CSym *f;
    TypeId ft;
    uint32_t k, n;
    if (c->func_sym == SYM_NONE || !diag_enabled(c->diag, "suggest-attribute=format"))
        return;
    f = csym(c, c->func_sym);
    if (f->fmt && (f->fmt >> 24) == (scan ? 2u : 1u))
        return;
    ft = type_canon(TT, f->ty);
    if (type_kind(TT, ft) != TY_FUNC)
        return;
    n = type_ent(TT, ft)->n;
    for (k = 0; k < n; k++) {
        TypeId t = type_canon(TT, type_params(TT, ft)[k]);
        if (type_kind(TT, t) == TY_PTR &&
            type_kind(TT, TYPE_UNQUAL(type_canon(TT, type_base(TT, t)))) ==
                TY_CHAR) {
            cwarn(c, where, "suggest-attribute=format", "function '%s' might "
                  "be a candidate for '%s' format attribute",
                  cident(c, f->name), scan ? "gnu_scanf" : "gnu_printf");
            return;
        }
    }
}

/* check_format_arg: the string literals a format expression can be: the arms
 * of ?:, and the result of a call to a function with a format_arg attribute
 * (its own argument).  False when some arm is something else. */
static bool fmt_leaves(Checker *c, uint32_t s, uint32_t *out, uint32_t *n,
                       unsigned depth)
{
    uint32_t k[33], nk;
    s = strip_paren(c, s);
    if (s == NO_NODE || node_err(c, s) || depth > 8 || *n >= 16)
        return false;
    switch (ntag(c, s)) {
    case N_STRING:
        out[(*n)++] = s;
        return true;
    case N_COND:
        nk = nkids(c, s, k, 3);
    {
        uint32_t n0 = *n;
        /* a constant condition folds to one arm */
        if (nk == 3 && (c->ck[k[0]] == K_ICE || c->ck[k[0]] == K_FOLD) &&
            is_int(c, c->ty[k[0]]))
            return fmt_leaves(c, cexpr_sval(c, k[0]) ? k[1] : k[2], out, n,
                              depth + 1);
        if (nk < 2 || !fmt_leaves(c, nk == 3 ? k[1] : k[0], out, n, depth + 1) ||
            !fmt_leaves(c, k[nk - 1], out, n, depth + 1))
            return false;
        /* gcc folds `c ? "x" : "x"` to "x" */
        if (*n == n0 + 2 &&
            ntag(c, strip_paren(c, nk == 3 ? k[1] : k[0])) == N_STRING &&
            ntag(c, strip_paren(c, k[nk - 1])) == N_STRING) {
            char *f1, *f2;
            uint32_t *o1, *o2;
            size_t n1, n2;
            bool x1, x2, same = false;
            if (fmt_decode(c, out[n0], &f1, &o1, &n1, &x1)) {
                if (fmt_decode(c, out[n0 + 1], &f2, &o2, &n2, &x2)) {
                    same = n1 == n2 && !memcmp(f1, f2, n1);
                    free(f2);
                    free(o2);
                }
                free(f1);
                free(o1);
            }
            if (same)
                (*n)--;
        }
        return true;
    }
    case N_CALL: {
        uint32_t f, fn;
        nk = nkids(c, s, k, 33);
        if (nk < 2)
            return false;
        f = strip_paren(c, k[0]);
        if (f == NO_NODE || ntag(c, f) != N_IDENT || c->ck[f] != K_ADDR ||
            !c->cb[f] || (c->cb[f] & CB_NODE) ||
            csym(c, c->cb[f] - 1)->kind != CS_FUNC)
            return false;
        fn = csym(c, c->cb[f] - 1)->fmtarg;
        if (!fn || fn >= nk)
            return false;
        return fmt_leaves(c, k[fn], out, n, depth + 1);
    }
    default:
        return false;
    }
}

/* check_format_info's complaint about a format that is not a string
 * literal: -Wformat-security (or -Wformat-nonliteral) with no arguments to
 * check, -Wformat-nonliteral with some. */
/* Record the bytes of a const char array's string initializer: a format
 * read through the array is checked like the literal (c-family
 * check_format_arg via decl_constant_value). */
/* const char t[][N] = { "a", "b", ... }: the rows' bytes, zero padded to the
 * array's size. */
static void note_strinit_rows(Checker *c, CSym *s, uint32_t list)
{
    TypeId row = type_base(TT, type_canon(TT, s->ty)), ce;
    uint32_t kk[256], nk;
    bool ok = false, good = true;
    uint64_t rs, total;
    StrInit si;
    uint32_t j;
    if (type_ckind(TT, s->ty) != TY_ARRAY || type_ckind(TT, row) != TY_ARRAY)
        return;
    ce = type_base(TT, type_canon(TT, row));
    if (((TYPE_QUALS(ce) | TYPE_QUALS(row) | TYPE_QUALS(s->ty)) &
         (TQ_CONST | TQ_VOLATILE)) != TQ_CONST ||
        mainv(c, ce) != TYPE_B(CHAR))
        return;
    rs = type_size(TT, row, &ok);
    nk = nkids(c, list, kk, 256);
    if (!ok || !rs || !nk || nk > 256)
        return;
    total = type_size(TT, s->ty, &ok);
    if (!ok || !total)           /* a [] bound counts the initializers */
        total = nk * rs;
    si.b = xcalloc(1, total);
    for (j = 0; good && j < nk; j++) {
        char *f;
        uint32_t *off;
        size_t n;
        bool exact;
        if (ntag(c, kk[j]) != N_STRING || (j + 1) * rs > total ||
            !fmt_decode(c, kk[j], &f, &off, &n, &exact)) {
            good = false;
            break;
        }
        free(off);
        if (n > rs)
            n = rs;
        memcpy(si.b + j * rs, f, n);
        free(f);
    }
    if (!good) {
        free(si.b);
        return;
    }
    si.n = total;
    si.uns = false;
    vec_push(&c->strinits, si);
    s->strinit = (uint32_t)c->strinits.len;
}

void cexpr_note_strinit(Checker *c, CSym *s, uint32_t init)
{
    uint32_t lit = strip_paren(c, init);
    TypeId et;
    StrInit si;
    char *f;
    uint32_t *off;
    size_t n;
    bool exact;
    if (lit != NO_NODE && ntag(c, lit) == N_INIT_LIST) {
        note_strinit_rows(c, s, lit);
        return;
    }
    if (lit == NO_NODE || ntag(c, lit) != N_STRING || type_ckind(TT, s->ty) != TY_ARRAY)
        return;
    et = type_base(TT, type_canon(TT, s->ty));
    if ((TYPE_QUALS(et) & (TQ_CONST | TQ_VOLATILE)) != TQ_CONST ||
        (mainv(c, et) != TYPE_B(CHAR) && mainv(c, et) != TYPE_B(UCHAR) &&
         mainv(c, et) != TYPE_B(SCHAR)))
        return;
    if (!fmt_decode(c, lit, &f, &off, &n, &exact))
        return;
    free(off);
    si.b = f;
    si.n = n;
    si.uns = mainv(c, et) != TYPE_B(CHAR);
    vec_push(&c->strinits, si);
    s->strinit = (uint32_t)c->strinits.len;
}

/* gcc's c_strlen on the argument of strlen: a constant string (a literal or
 * a const char array with a string initializer) plus a constant offset is
 * checked against its bounds (-Warray-bounds=), and an array whose bytes
 * from the offset on hold no nul is -Wstringop-overread. */
typedef struct SlRes {
    uint32_t ref;               /* the array's symbol, SYM_NONE for a literal */
    size_t n;                   /* literal bytes (without the nul) */
    int64_t size, off;
    bool known;
    int64_t base;               /* the row's offset in a multidimensional array */
} SlRes;

static int sl_resolve(Checker *c, uint32_t e, int64_t off, bool known,
                      SlRes *out, int max, int depth)
{
    uint32_t k[3];
    int m = 0;
    if (depth > 8)
        return 0;
    e = strip_paren(c, e);
    if (e == NO_NODE || node_err(c, e))
        return 0;
    switch (ntag(c, e)) {
    case N_CAST:
        if (nkids(c, e, k, 2) == 2)
            return sl_resolve(c, k[1], off, known, out, max, depth + 1);
        return 0;
    case N_COND:
        if (nkids(c, e, k, 3) == 3) {
            m = sl_resolve(c, k[1], off, known, out, max, depth + 1);
            if (m < max)
                m += sl_resolve(c, k[2], off, known, out + m, max - m,
                                depth + 1);
        }
        return m;
    case N_BINARY: {
        int p = npunct(c, e);
        uint32_t sd, nd;
        if ((p != P_PLUS && p != P_MINUS) || nkids(c, e, k, 2) != 2)
            return 0;
        sd = k[0];
        nd = k[1];
        if (p == P_PLUS && !is_ptr(c, rvt(c, sd)) && !is_array(c, c->ty[sd])) {
            sd = k[1];
            nd = k[0];
        }
        if (!is_int(c, rvt(c, nd)) ||
            (!is_ptr(c, rvt(c, sd)) && !is_array(c, c->ty[sd])))
            return 0;
        if ((c->ck[nd] == K_ICE || c->ck[nd] == K_FOLD) && known)
            off += p == P_PLUS ? cexpr_sval(c, nd) : -cexpr_sval(c, nd);
        else
            known = false;
        return sl_resolve(c, sd, off, known, out, max, depth + 1);
    }
    case N_UNARY: {
        uint32_t ix;
        if (npunct(c, e) != P_AMP || nkids(c, e, k, 1) != 1)
            return 0;
        ix = strip_paren(c, k[0]);
        if (ix != NO_NODE && !node_err(c, ix) && ntag(c, ix) == N_UNARY &&
            npunct(c, ix) == P_STAR && nkids(c, ix, k, 1) == 1)
            return sl_resolve(c, k[0], off, known, out, max, depth + 1);
        if (ix == NO_NODE || node_err(c, ix) || ntag(c, ix) != N_INDEX ||
            nkids(c, ix, k, 2) != 2 || ntag(c, strip_paren(c, k[0])) == N_MEMBER_EXPR)
            return 0;      /* (gcc offsets &obj.m[K] from the whole object) */
        if ((c->ck[k[1]] == K_ICE || c->ck[k[1]] == K_FOLD) && known)
            off += cexpr_sval(c, k[1]);
        else
            known = false;
        return sl_resolve(c, k[0], off, known, out, max, depth + 1);
    }
    case N_INDEX: {
        /* a row of a multidimensional array with a constant index */
        bool ok = false;
        uint64_t rs;
        int r;
        if (nkids(c, e, k, 2) != 2 || type_ckind(TT, c->ty[e]) != TY_ARRAY ||
            type_ckind(TT, c->ty[k[0]]) != TY_ARRAY ||
            !(c->ck[k[1]] == K_ICE || c->ck[k[1]] == K_FOLD))
            return 0;
        rs = type_size(TT, c->ty[e], &ok);
        if (!ok || !rs)
            return 0;
        r = sl_resolve(c, k[0], off, known, out, max, depth + 1);
        if (r == 1 && out[0].ref != SYM_NONE) {
            out[0].base += cexpr_sval(c, k[1]) * (int64_t)rs;
            out[0].size = (int64_t)rs;
            return 1;
        }
        return 0;
    }
    case N_STRING: {
        char *f;
        uint32_t *fo;
        size_t n;
        bool exact;
        if (!fmt_decode(c, e, &f, &fo, &n, &exact))
            return 0;
        free(f);
        free(fo);
        out[0] = (SlRes){SYM_NONE, n, (int64_t)n + 1, off, known};
        return 1;
    }
    case N_MEMBER_EXPR: {
        /* a member array of a const object with an initializer (no bytes) */
        uint32_t b = first_child(c, e), ref;
        bool ok = false;
        uint64_t sz;
        if (b == NO_NODE || (c->nodes[e].flags & NF_ARROW))
            return 0;
        b = strip_paren(c, b);
        if (b == NO_NODE || node_err(c, b) || ntag(c, b) != N_IDENT ||
            type_ckind(TT, c->ty[e]) != TY_ARRAY ||
            !(TYPE_QUALS(c->ty[b]) & TQ_CONST))
            return 0;
        ref = lookup_ord(c, cnode_ident(c, b));
        if (ref == SYM_NONE || (ref & SYM_LOCAL) || csym(c, ref)->kind != CS_OBJ ||
            !(csym(c, ref)->flags & CSF_DEFINED))
            return 0;
        sz = type_size(TT, c->ty[e], &ok);
        out[0] = (SlRes){ref, 0, ok ? (int64_t)sz : 0, off, known};
        return 1;
    }
    case N_IDENT: {
        uint32_t ref;
        bool ok = false;
        uint64_t sz;
        if (type_ckind(TT, c->ty[e]) != TY_ARRAY)
            return 0;
        ref = lookup_ord(c, cnode_ident(c, e));
        if (ref == SYM_NONE || csym(c, ref)->kind != CS_OBJ ||
            !csym(c, ref)->strinit)
            return 0;
        sz = type_size(TT, c->ty[e], &ok);
        if (!ok)
            return 0;
        out[0] = (SlRes){ref, 0, (int64_t)sz, off, known};
        return 1;
    }
    default:
        return 0;
    }
}

void check_strlen(Checker *c, const uint32_t *kv, uint32_t nk)
{
    SlRes r[4];
    int n, j;
    uint32_t a, k3[3];
    SrcLoc loc;
    if (nk != 2 || (!diag_enabled(c->diag, "array-bounds=") &&
                    !diag_enabled(c->diag, "stringop-overread")))
        return;
    a = kv[1];
    n = sl_resolve(c, a, 0, true, r, 4, 0);
    loc = ntag(c, a) == N_PAREN ? ctok_loc(c, c->nodes[a].tok) : expr_loc(c, a);
    if (ntag(c, a) == N_COND && nkids(c, a, k3, 3) == 3)     /* gcc: the ':' */
        loc = cexpr_colon_loc(c, a, k3[1], k3[2]);
    for (j = 0; j < n; j++) {
        const CSym *sy = r[j].ref != SYM_NONE ? csym(c, r[j].ref) : NULL;
        if (r[j].known && (r[j].off < 0 || r[j].off >= r[j].size)) {
            Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "array-bounds=",
                                    "offset '%d' outside bounds of constant "
                                    "string", (int)r[j].off);
            if (d && sy)
                cnote(c, d, sy->loc, "'%s' declared here",
                      cident(c, sy->name));
        } else if (sy && sy->strinit) {
            const StrInit *si = &c->strinits.data[sy->strinit - 1];
            size_t from = (size_t)r[j].base + (r[j].known ? (size_t)r[j].off : 0);
            size_t end = (size_t)(r[j].base + r[j].size);
            if ((int64_t)si->n >= r[j].base + r[j].size &&
                (from >= end || !memchr(si->b + from, 0, end - from))) {
                Diagnostic *d = cwarn_d(c, DL_WARNING, loc, "stringop-overread",
                                        "'strlen' argument missing "
                                        "terminating nul");
                if (d)
                    cnote(c, d, sy->loc, "referenced argument declared here");
                break;      /* the call is then marked no-warning */
            }
        }
    }
}

void check_format_literal(Checker *c, const uint32_t *kv, uint32_t nk,
                                 const CSym *sy, const char *name, SrcLoc loc)
{
    uint32_t pos = 0, first = 0, a, s;
    SrcLoc where = loc, input;
    bool nonlit = false, scan = false;
    if (sy && sy->fmt) {
        pos = (sy->fmt >> 12) & 0xfff;
        first = sy->fmt & 0xfff;
        scan = (sy->fmt >> 24) == 2;
    } else if ((pos = builtin_format_pos(name))) {
        first = builtin_is_va(name) ? 0 : pos + 1;
    } else if ((pos = builtin_scanf_pos(name))) {
        first = builtin_is_va(name) ? 0 : pos + 1;
        scan = true;
    }
    if (!pos || nk - 1 < pos)
        return;
    a = kv[pos];
    /* gcc's input_location: the line of the token after the call's ')' */
    where = input = cinput_loc(c, last_tok(c, kv[nk - 1]) + 1);
    s = strip_paren(c, a);
    if (s == NO_NODE || node_err(c, a))
        return;
    if (ntag(c, s) == N_STRING) {
        fmt_check(c, kv, nk, first, scan, s,
                  a != s && ntag(c, a) == N_PAREN ? ctok_loc(c, c->nodes[a].tok)
                                                  : expr_loc(c, a), loc, 0, NULL);
        return;
    }
    {
        /* casts, "str" + N and &"str"[N] of a literal */
        uint32_t u = s, k[3];
        int64_t off = 0;
        bool moved = false, amp = false, plus = false;
        uint32_t pn = NO_NODE;
        for (;;) {
            uint32_t u0 = u;
            u = strip_paren(c, u);
            pn = u0 != u && ntag(c, u0) == N_PAREN ? u0 : NO_NODE;
            if (u == NO_NODE || node_err(c, u))
                break;
            if (ntag(c, u) == N_CAST && nkids(c, u, k, 2) == 2) {
                u = k[1];
                moved = true;
                continue;
            }
            if (ntag(c, u) == N_BINARY && npunct(c, u) == P_PLUS &&
                nkids(c, u, k, 2) == 2) {
                uint32_t sd = strip_paren(c, k[0]), nd = k[1];
                if (!(c->ck[nd] == K_ICE || c->ck[nd] == K_FOLD)) {
                    sd = strip_paren(c, k[1]);
                    nd = k[0];
                }
                if ((c->ck[nd] == K_ICE || c->ck[nd] == K_FOLD) &&
                    is_int(c, c->ty[nd])) {
                    off += cexpr_sval(c, nd);
                    u = sd;
                    moved = plus = true;
                    continue;
                }
            }
            if (ntag(c, u) == N_UNARY && npunct(c, u) == P_AMP &&
                (nkids(c, u, k, 1), first_child(c, u) != NO_NODE)) {
                uint32_t ix = strip_paren(c, first_child(c, u));
                if (ix != NO_NODE && ntag(c, ix) == N_INDEX &&
                    nkids(c, ix, k, 2) == 2 && (c->ck[k[1]] == K_ICE ||
                                                c->ck[k[1]] == K_FOLD)) {
                    off += cexpr_sval(c, k[1]);
                    u = k[0];
                    moved = amp = true;
                    continue;
                }
            }
            break;
        }
        if (u != NO_NODE && !node_err(c, u) && ntag(c, u) == N_IDENT &&
            type_ckind(TT, c->ty[u]) == TY_ARRAY && !amp) {
            /* a const array is read through its initializer */
            uint32_t ref = lookup_ord(c, cnode_ident(c, u));
            const StrInit *si = NULL;
            if (ref != SYM_NONE && csym(c, ref)->kind == CS_OBJ &&
                csym(c, ref)->strinit)
                si = &c->strinits.data[csym(c, ref)->strinit - 1];
            if (si) {
                SrcLoc w = pn != NO_NODE ? ctok_loc(c, c->nodes[pn].tok)
                                         : expr_loc(c, u);
                if (si->uns)
                    cwarn(c, expr_loc(c, u), "format=", "format string is not "
                          "an array of type 'char'");
                else if (off >= 0) {
                    fmt_mloc = plus ? expr_loc(c, strip_paren(c, a)) : 0;
                    fmt_check(c, kv, nk, first, scan, NO_NODE, w, loc,
                              (size_t)off, si);
                    fmt_mloc = 0;
                }
                return;
            }
        }
        if (moved && u != NO_NODE && !node_err(c, u) && ntag(c, u) == N_STRING) {
            size_t len;
            const char *tx = ttext(c, c->nodes[u].tok, &len);
            if (lit_str_prefix(tx, len)) {
                cwarn(c, ctok_loc(c, c->nodes[u].tok), "format=",
                      "format is a wide character string");
                return;
            }
            if (off >= 0)
                fmt_check(c, kv, nk, first, scan, u,
                          amp ? expr_loc(c, a) : ctok_loc(c, c->nodes[u].tok),
                          loc, (size_t)off, NULL);
            return;
        }
    }
    {
        uint32_t lv[16], nl = 0, m;
        if (fmt_leaves(c, s, lv, &nl, 0)) {
            for (m = 0; m < nl; m++)
                fmt_check(c, kv, nk, first, scan, lv[m], expr_loc(c, lv[m]), loc, 0, NULL);
            return;
        }
    }
    if (type_ckind(TT, c->ty[s]) == TY_ARRAY && c->ck[s] != K_ERR) {
        /* a writable array: its address has a location of its own; a
         * const one is read through its initializer */
        if (!(TYPE_QUALS(type_base(TT, type_canon(TT, c->ty[s]))) & TQ_CONST) &&
            !(c->ck[s] == K_ADDR && (c->cb[s] & CB_NODE))) {
            nonlit = true;
            where = expr_loc(c, a);
        }
    } else if (type_ckind(TT, c->ty[s]) == TY_VLA && c->ck[s] != K_ERR) {
        nonlit = true;          /* never read through an initializer */
        where = expr_loc(c, a);
    } else if (c->ck[s] == K_NONE) {
        nonlit = true;
    }
    if (!nonlit)
        return;
    if (!first)
        suggest_format(c, scan, input);
    if (first && nk <= first) {
        const char *opt = !scan && diag_enabled(c->diag, "format-security")
                          ? "format-security" : "format-nonliteral";
        cwarn(c, where, opt, "format not a string literal and no format "
              "arguments");
    } else {
        cwarn(c, where, "format-nonliteral", "format not a string literal, "
              "argument types not checked");
    }
}

