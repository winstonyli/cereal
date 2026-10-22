/* eval_libc.c - the host library of the evaluator (docs/EVAL_DESIGN.md, E3).
 *
 * A call of a function the unit only declares reaches the table at the end:
 * output (printf and friends), the heap (malloc and friends), strings and memory,
 * abs, exit.  Each function works on evaluator objects only through the checked
 * access of eval.c (ev_access and its callers below), so undefined behaviour stops
 * evaluation with a reason - a read out of bounds or of an uninitialized byte, a
 * write to a string literal, a double free, a format that does not match its
 * arguments - and a future shadow of the objects (symbolic values) is carried by
 * the same copies.  Any other external function is reported as unsupported. */
#include <limits.h>
#include <math.h>
#include <stdio.h>

#include "c/eval_int.h"

#define NEED(n) do { if (na < (n)) { UNSUP(v, "call with too few arguments"); return 0; } } while (0)

/* ---- checked memory ---------------------------------------------------------------- */

/* [p, p + n) for reading: in bounds and initialized.  NULL after a trap. */
static const uint8_t *rd(Ev *v, uint64_t p, uint64_t n, const char *who)
{
    Obj *o = ev_access(v, p, n, who);
    uint64_t i;
    if (!o)
        return NULL;
    for (i = 0; i < n; i++)
        if (!o->init[poff(p) + i]) {
            TRAP(v, "%s reads an uninitialized value", who);
            return NULL;
        }
    return o->b + poff(p);
}

/* [p, p + n) for writing, marked initialized: the caller writes all n bytes. */
static uint8_t *wr(Ev *v, uint64_t p, uint64_t n, const char *who)
{
    Obj *o = ev_access(v, p, n, who);
    if (!o)
        return NULL;
    if (o->ro) {
        TRAP(v, "write to a string literal");
        return NULL;
    }
    memset(o->init + poff(p), 1, n);
    return o->b + poff(p);
}

/* The length of the string at p, reading at most `limit` bytes; false after a trap. */
static bool slen(Ev *v, uint64_t p, uint64_t limit, uint64_t *len, const char *who)
{
    Obj *o = ev_access(v, p, 0, who);
    uint64_t i, from = (uint64_t)poff(p);
    if (!o)
        return false;
    for (i = from; i < o->size && i - from < limit; i++) {
        if (!o->init[i]) {
            TRAP(v, "%s reads an uninitialized value", who);
            return false;
        }
        if (!o->b[i])
            break;
    }
    if (i == o->size && i - from < limit) {
        TRAP(v, "%s of a string that is not terminated", who);
        return false;
    }
    *len = i - from;
    return true;
}

static bool overlap(uint64_t a, uint64_t an, uint64_t b, uint64_t bn)
{
    return pid(a) == pid(b) && (uint64_t)poff(a) < (uint64_t)poff(b) + bn &&
           (uint64_t)poff(b) < (uint64_t)poff(a) + an;
}

/* ---- output ------------------------------------------------------------------------ */

typedef struct Buf {
    char *p;
    size_t n, cap;
} Buf;

static void bput(Buf *b, const char *s, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = xrealloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void bprintf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int m;
    char *t;
    va_start(ap, fmt);
    m = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    t = xmalloc((size_t)m + 1);
    va_start(ap, fmt);
    vsnprintf(t, (size_t)m + 1, fmt, ap);
    va_end(ap);
    bput(b, t, (size_t)m);
    free(t);
}

/* The stream behind a FILE * (stdout or stderr): false after a stop. */
static bool stream(Ev *v, uint64_t fp, FILE **f)
{
    if (pid(fp) && poff(fp) == 0 && pid(fp) == v->token[1])
        *f = v->o.out;
    else if (pid(fp) && poff(fp) == 0 && pid(fp) == v->token[2])
        *f = v->o.err;
    else {
        UNSUP(v, "stream other than stdout and stderr");
        return false;
    }
    return true;
}

static void emit(FILE *f, const char *s, size_t n)
{
    if (f)
        fwrite(s, 1, n, f);
}

/* Is the argument a pointer / an integer (not a float)? */
static bool ptr_ty(Ev *v, TypeId t)
{
    return type_ckind(&v->c->tt, t) == TY_PTR;
}

/* printf-style formatting of the arguments a[0..na) by the format at fmt into b.  The
 * argument's type must be the conversion's: a mismatch is undefined behaviour. */
static bool format(Ev *v, Buf *b, uint64_t fmt, const HArg *a, unsigned na, const char *who)
{
    uint64_t len, i = 0;
    unsigned ai = 0;
    const uint8_t *src;
    char *s;
    bool ok = true;
    if (!slen(v, fmt, UINT64_MAX, &len, who) || !(src = rd(v, fmt, len, who)))
        return false;
    s = xmalloc(len + 1);
    memcpy(s, src, len);
    s[len] = 0;
    while (ok && i < len) {
        char spec[32], conv;
        size_t sl = 0;
        int lmod = 0;                   /* 0, 'H' (hh), 'h', 'l', 'L' (ll), 'z' (z, j, t) */
        bool has_prec = false;
        long prec = 0;
        const HArg *arg;
        Ti ti;
        if (s[i] != '%') {
            bput(b, s + i++, 1);
            continue;
        }
        i++;
        if (s[i] == '%') {
            bput(b, "%", 1);
            i++;
            continue;
        }
        spec[sl++] = '%';
        while (s[i] && strchr("-+ #0", s[i]) && sl < 8)
            spec[sl++] = s[i++];
        if (s[i] == '*') {
            if (ai >= na || !ev_tinfo(v, a[ai].ty, &ti) || ti.flt || ptr_ty(v, a[ai].ty)) {
                TRAP(v, "%s: width is not an int argument", who);
                break;
            }
            sl += (size_t)snprintf(spec + sl, sizeof spec - sl, "%d", (int)a[ai++].x);
            i++;
        } else {
            while (s[i] >= '0' && s[i] <= '9' && sl < 16)
                spec[sl++] = s[i++];
        }
        if (s[i] == '.') {
            has_prec = true;
            i++;
            if (s[i] == '*') {
                if (ai >= na || !ev_tinfo(v, a[ai].ty, &ti) || ti.flt || ptr_ty(v, a[ai].ty)) {
                    TRAP(v, "%s: precision is not an int argument", who);
                    break;
                }
                prec = (int)a[ai++].x;
                i++;
            } else {
                while (s[i] >= '0' && s[i] <= '9')
                    prec = prec * 10 + (s[i++] - '0');
            }
            if (prec < 0)
                has_prec = false;
            else
                sl += (size_t)snprintf(spec + sl, sizeof spec - sl, ".%ld", prec);
        }
        if (s[i] == 'h')
            lmod = s[i + 1] == 'h' ? (i += 2, 'H') : (i++, 'h');
        else if (s[i] == 'l')
            lmod = s[i + 1] == 'l' ? (i += 2, 'L') : (i++, 'l');
        else if (s[i] == 'z' || s[i] == 'j' || s[i] == 't') {
            lmod = 'z';
            i++;
        } else if (s[i] == 'L') {
            UNSUP(v, "%s: long double conversion", who);
            break;
        }
        conv = s[i];
        if (!conv) {
            TRAP(v, "%s: the format ends inside a conversion", who);
            break;
        }
        i++;
        if (sl >= sizeof spec - 4) {
            UNSUP(v, "%s: conversion specification too long", who);
            break;
        }
        if (conv == 'n' || !strchr("diuxXocspfFeEgGaA", conv)) {
            UNSUP(v, "%s: conversion %%%c", who, conv);
            break;
        }
        if (ai >= na) {
            TRAP(v, "%s: too few arguments for the format", who);
            break;
        }
        arg = &a[ai++];
        if (!ev_tinfo(v, arg->ty, &ti))
            break;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            bool wide = lmod == 'l' || lmod == 'L' || lmod == 'z';
            if (ti.flt || ptr_ty(v, arg->ty) || (wide ? ti.bytes != 8 : ti.bytes > 4)) {
                TRAP(v, "%s: %%%c given an argument of another type", who, conv);
                ok = false;
                break;
            }
            if (conv == 'c' && lmod) {
                UNSUP(v, "%s: wide character conversion", who);
                ok = false;
                break;
            }
            spec[sl] = 0;
            if (wide) {
                strcat(spec, "ll");
                spec[sl + 2] = conv;
                spec[sl + 3] = 0;
                if (conv == 'd' || conv == 'i')
                    bprintf(b, spec, (long long)arg->x);
                else
                    bprintf(b, spec, (unsigned long long)arg->x);
            } else {
                if (lmod)
                    strcat(spec, lmod == 'H' ? "hh" : "h");
                sl = strlen(spec);
                spec[sl] = conv;
                spec[sl + 1] = 0;
                if (conv == 'd' || conv == 'i' || conv == 'c')
                    bprintf(b, spec, (int)arg->x);
                else
                    bprintf(b, spec, (unsigned)arg->x);
            }
            break;
        }
        case 's': {
            uint64_t n;
            const uint8_t *t;
            char *z;
            if (!ptr_ty(v, arg->ty) || lmod) {
                TRAP(v, "%s: %%s given an argument that is not a pointer", who);
                ok = false;
                break;
            }
            if (!slen(v, arg->x, has_prec ? (uint64_t)prec : UINT64_MAX, &n, who) ||
                !(t = rd(v, arg->x, n, who))) {
                ok = false;
                break;
            }
            z = xmalloc(n + 1);
            memcpy(z, t, n);
            z[n] = 0;
            spec[sl] = 's';
            spec[sl + 1] = 0;
            bprintf(b, spec, z);
            free(z);
            break;
        }
        case 'p': {
            char t[32];
            if (!ptr_ty(v, arg->ty) || lmod) {
                TRAP(v, "%s: %%p given an argument that is not a pointer", who);
                ok = false;
                break;
            }
            if (pid(arg->x) == 0)
                strcpy(t, "(nil)");
            else
                snprintf(t, sizeof t, "0x%llx", (unsigned long long)((uint64_t)pid(arg->x) << 16) +
                                                    (unsigned long long)(uint32_t)poff(arg->x));
            spec[sl] = 's';
            spec[sl + 1] = 0;
            bprintf(b, spec, t);
            break;
        }
        default: {                      /* f F e E g G a A */
            if (!ti.flt || lmod) {
                TRAP(v, "%s: %%%c given an argument that is not a double", who, conv);
                ok = false;
                break;
            }
            spec[sl] = conv;
            spec[sl + 1] = 0;
            bprintf(b, spec, ev_fget(&ti, arg->x));
            break;
        }
        }
    }
    free(s);
    return ok && !v->res.status;            /* arguments beyond the conversions are ignored */
}

static uint64_t h_printf(Ev *v, const HArg *a, unsigned na)
{
    Buf b = {0};
    uint64_t r = 0;
    NEED(1);
    if (format(v, &b, a[0].x, a + 1, na - 1, "printf")) {
        emit(v->o.out, b.p ? b.p : "", b.n);
        r = b.n;
    }
    free(b.p);
    return r;
}

static uint64_t h_fprintf(Ev *v, const HArg *a, unsigned na)
{
    Buf b = {0};
    FILE *f;
    uint64_t r = 0;
    NEED(2);
    if (stream(v, a[0].x, &f) && format(v, &b, a[1].x, a + 2, na - 2, "fprintf")) {
        emit(f, b.p ? b.p : "", b.n);
        r = b.n;
    }
    free(b.p);
    return r;
}

/* sprintf (limit: no bound) and snprintf into evaluator memory. */
static uint64_t fmt_to(Ev *v, uint64_t dst, uint64_t limit, bool bounded, uint64_t fmt, const HArg *a,
                       unsigned na, const char *who)
{
    Buf b = {0};
    uint64_t r = 0;
    if (format(v, &b, fmt, a, na, who)) {
        uint64_t k = bounded ? (limit == 0 ? 0 : (b.n < limit - 1 ? b.n : limit - 1)) : b.n;
        if (!bounded || limit) {
            uint8_t *d = wr(v, dst, k + 1, who);
            if (d) {
                if (k)
                    memcpy(d, b.p, k);
                d[k] = 0;
                r = b.n;
            }
        } else {
            r = b.n;
        }
    }
    free(b.p);
    return r;
}

static uint64_t h_sprintf(Ev *v, const HArg *a, unsigned na)
{
    NEED(2);
    return fmt_to(v, a[0].x, 0, false, a[1].x, a + 2, na - 2, "sprintf");
}

static uint64_t h_snprintf(Ev *v, const HArg *a, unsigned na)
{
    NEED(3);
    return fmt_to(v, a[0].x, a[1].x, true, a[2].x, a + 3, na - 3, "snprintf");
}

static bool put_string(Ev *v, FILE *f, uint64_t p, const char *who)
{
    uint64_t n;
    const uint8_t *t;
    if (!slen(v, p, UINT64_MAX, &n, who) || !(t = rd(v, p, n, who)))
        return false;
    emit(f, (const char *)t, n);
    return true;
}

static uint64_t h_puts(Ev *v, const HArg *a, unsigned na)
{
    NEED(1);
    if (!put_string(v, v->o.out, a[0].x, "puts"))
        return 0;
    emit(v->o.out, "\n", 1);
    return 1;
}

static uint64_t h_fputs(Ev *v, const HArg *a, unsigned na)
{
    FILE *f;
    NEED(2);
    if (!stream(v, a[1].x, &f) || !put_string(v, f, a[0].x, "fputs"))
        return 0;
    return 1;
}

static uint64_t h_putchar(Ev *v, const HArg *a, unsigned na)
{
    char c;
    NEED(1);
    c = (char)(unsigned char)a[0].x;
    emit(v->o.out, &c, 1);
    return (unsigned char)c;
}

/* ---- the heap ---------------------------------------------------------------------- */

static uint64_t h_malloc(Ev *v, const HArg *a, unsigned na)
{
    uint32_t id;
    NEED(1);
    id = ev_new_obj(v, a[0].x, false);
    if (!id)
        return 0;
    v->obj.data[id - 1].heap = true;
    return mkptr(id, 0);
}

static uint64_t h_calloc(Ev *v, const HArg *a, unsigned na)
{
    uint64_t n;
    uint32_t id;
    NEED(2);
    if (__builtin_mul_overflow(a[0].x, a[1].x, &n))
        return 0;                       /* malloc's own answer to a size that cannot exist */
    id = ev_new_obj(v, n, false);
    if (!id)
        return 0;
    v->obj.data[id - 1].heap = true;
    ev_zero_n(v, mkptr(id, 0), n);
    return mkptr(id, 0);
}

/* The heap object p points at the start of; NULL after a trap (or for the null pointer). */
static Obj *heap_obj(Ev *v, uint64_t p, const char *who)
{
    Obj *o;
    if (pid(p) == 0)
        return NULL;
    o = &v->obj.data[pid(p) - 1];
    if (!o->heap || poff(p) != 0) {
        TRAP(v, "%s of a pointer that malloc did not return", who);
        return NULL;
    }
    if (!o->live) {
        TRAP(v, "%s of a block already freed", who);
        return NULL;
    }
    return o;
}

static uint64_t h_free(Ev *v, const HArg *a, unsigned na)
{
    Obj *o;
    NEED(1);
    o = heap_obj(v, a[0].x, "free");
    if (o)
        ev_kill_obj(v, pid(a[0].x));
    return 0;
}

static uint64_t h_realloc(Ev *v, const HArg *a, unsigned na)
{
    Obj *o;
    uint32_t id;
    uint64_t keep;
    NEED(2);
    if (pid(a[0].x) == 0)
        return h_malloc(v, a + 1, 1);
    o = heap_obj(v, a[0].x, "realloc");
    if (!o)
        return 0;
    if (a[1].x == 0) {
        UNSUP(v, "realloc to size zero");
        return 0;
    }
    id = ev_new_obj(v, a[1].x, false);
    if (!id)
        return 0;
    o = &v->obj.data[pid(a[0].x) - 1];  /* new_obj may have moved the table */
    v->obj.data[id - 1].heap = true;
    keep = o->size < a[1].x ? o->size : a[1].x;
    memcpy(v->obj.data[id - 1].b, o->b, keep);
    memcpy(v->obj.data[id - 1].init, o->init, keep);
    ev_kill_obj(v, pid(a[0].x));
    return mkptr(id, 0);
}

/* ---- memory and strings ------------------------------------------------------------ */

static uint64_t h_memcpy(Ev *v, const HArg *a, unsigned na)
{
    NEED(3);
    if (overlap(a[0].x, a[2].x, a[1].x, a[2].x) && a[2].x) {
        TRAP(v, "memcpy of overlapping ranges");
        return 0;
    }
    ev_copy_n(v, a[0].x, a[1].x, a[2].x);
    return a[0].x;
}

static uint64_t h_memmove(Ev *v, const HArg *a, unsigned na)
{
    NEED(3);
    ev_copy_n(v, a[0].x, a[1].x, a[2].x);
    return a[0].x;
}

static uint64_t h_memset(Ev *v, const HArg *a, unsigned na)
{
    uint8_t *d;
    NEED(3);
    d = wr(v, a[0].x, a[2].x, "memset");
    if (d)
        memset(d, (int)(unsigned char)a[1].x, a[2].x);
    return a[0].x;
}

static uint64_t h_memcmp(Ev *v, const HArg *a, unsigned na)
{
    const uint8_t *x, *y;
    uint64_t i;
    NEED(3);
    x = rd(v, a[0].x, a[2].x, "memcmp");
    y = x ? rd(v, a[1].x, a[2].x, "memcmp") : NULL;
    if (!y)
        return 0;
    for (i = 0; i < a[2].x; i++)
        if (x[i] != y[i])
            return (uint64_t)(int64_t)((int)x[i] - (int)y[i]);
    return 0;
}

static uint64_t h_memchr(Ev *v, const HArg *a, unsigned na)
{
    const uint8_t *x;
    uint64_t i;
    NEED(3);
    x = rd(v, a[0].x, a[2].x, "memchr");
    if (!x)
        return 0;
    for (i = 0; i < a[2].x; i++)
        if (x[i] == (uint8_t)a[1].x)
            return mkptr(pid(a[0].x), poff(a[0].x) + (int64_t)i);
    return 0;
}

static uint64_t h_strlen(Ev *v, const HArg *a, unsigned na)
{
    uint64_t n = 0;
    NEED(1);
    slen(v, a[0].x, UINT64_MAX, &n, "strlen");
    return n;
}

static uint64_t cmp_str(Ev *v, uint64_t p, uint64_t q, uint64_t limit, const char *who)
{
    uint64_t i, n, m;
    const uint8_t *x, *y;
    if (!slen(v, p, limit, &n, who) || !slen(v, q, limit, &m, who))
        return 0;
    x = rd(v, p, n < limit ? n + 1 : n, who);
    y = x ? rd(v, q, m < limit ? m + 1 : m, who) : NULL;
    if (!y)
        return 0;
    for (i = 0; i < limit; i++) {
        if (x[i] != y[i])
            return (uint64_t)(int64_t)((int)x[i] - (int)y[i]);
        if (!x[i])
            break;
    }
    return 0;
}

static uint64_t h_strcmp(Ev *v, const HArg *a, unsigned na)
{
    NEED(2);
    return cmp_str(v, a[0].x, a[1].x, UINT64_MAX, "strcmp");
}

static uint64_t h_strncmp(Ev *v, const HArg *a, unsigned na)
{
    NEED(3);
    return cmp_str(v, a[0].x, a[1].x, a[2].x, "strncmp");
}

static uint64_t h_strcpy(Ev *v, const HArg *a, unsigned na)
{
    uint64_t n;
    const uint8_t *s;
    uint8_t *d;
    NEED(2);
    if (!slen(v, a[1].x, UINT64_MAX, &n, "strcpy") || !(s = rd(v, a[1].x, n + 1, "strcpy")))
        return 0;
    if (overlap(a[0].x, n + 1, a[1].x, n + 1)) {
        TRAP(v, "strcpy of overlapping ranges");
        return 0;
    }
    d = wr(v, a[0].x, n + 1, "strcpy");
    if (d)
        memcpy(d, s, n + 1);
    return a[0].x;
}

static uint64_t h_strncpy(Ev *v, const HArg *a, unsigned na)
{
    uint64_t n, k;
    const uint8_t *s;
    uint8_t *d;
    NEED(3);
    if (!slen(v, a[1].x, a[2].x, &n, "strncpy"))
        return 0;
    k = n < a[2].x ? n + 1 : n;         /* the terminator is copied when it is within n */
    if (!(s = rd(v, a[1].x, k, "strncpy")))
        return 0;
    if (overlap(a[0].x, a[2].x, a[1].x, k)) {
        TRAP(v, "strncpy of overlapping ranges");
        return 0;
    }
    d = wr(v, a[0].x, a[2].x, "strncpy");
    if (d) {
        memcpy(d, s, k);
        memset(d + k, 0, a[2].x - k);
    }
    return a[0].x;
}

static uint64_t h_strcat(Ev *v, const HArg *a, unsigned na)
{
    uint64_t dn, n;
    const uint8_t *s;
    uint8_t *d;
    NEED(2);
    if (!slen(v, a[0].x, UINT64_MAX, &dn, "strcat") || !slen(v, a[1].x, UINT64_MAX, &n, "strcat") ||
        !(s = rd(v, a[1].x, n + 1, "strcat")))
        return 0;
    if (overlap(mkptr(pid(a[0].x), poff(a[0].x) + (int64_t)dn), n + 1, a[1].x, n + 1)) {
        TRAP(v, "strcat of overlapping ranges");
        return 0;
    }
    d = wr(v, mkptr(pid(a[0].x), poff(a[0].x) + (int64_t)dn), n + 1, "strcat");
    if (d)
        memcpy(d, s, n + 1);
    return a[0].x;
}

static uint64_t find_char(Ev *v, const HArg *a, unsigned na, bool last, const char *who)
{
    uint64_t n, i, hit = 0;
    const uint8_t *s;
    NEED(2);
    if (!slen(v, a[0].x, UINT64_MAX, &n, who) || !(s = rd(v, a[0].x, n + 1, who)))
        return 0;
    for (i = 0; i <= n; i++)
        if (s[i] == (uint8_t)a[1].x) {
            hit = mkptr(pid(a[0].x), poff(a[0].x) + (int64_t)i);
            if (!last)
                break;
        }
    return hit;
}

static uint64_t h_strchr(Ev *v, const HArg *a, unsigned na) { return find_char(v, a, na, false, "strchr"); }
static uint64_t h_strrchr(Ev *v, const HArg *a, unsigned na) { return find_char(v, a, na, true, "strrchr"); }

/* ---- numbers and the process ------------------------------------------------------- */

static uint64_t h_abs(Ev *v, const HArg *a, unsigned na)
{
    int x;
    NEED(1);
    x = (int)a[0].x;
    if (x == INT_MIN && !v->o.wrap)
        TRAP(v, "abs of the most negative int");
    return (uint64_t)(x < 0 ? -(int64_t)x : (int64_t)x);
}

static uint64_t h_labs(Ev *v, const HArg *a, unsigned na)
{
    int64_t x;
    NEED(1);
    x = (int64_t)a[0].x;
    if (x == INT64_MIN && !v->o.wrap)
        TRAP(v, "abs of the most negative long");
    return x < 0 ? 0 - a[0].x : a[0].x;
}

/* ctype in the "C" locale; the argument is an int that is EOF or an unsigned char value. */
static uint64_t h_tolower(Ev *v, const HArg *a, unsigned na)
{
    int c;
    NEED(1);
    c = (int)a[0].x;
    return (uint64_t)(int64_t)(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

static uint64_t h_toupper(Ev *v, const HArg *a, unsigned na)
{
    int c;
    NEED(1);
    c = (int)a[0].x;
    return (uint64_t)(int64_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
}

/* floor, ceil, frexp of a double (the prototype converts the argument). */
static bool dbl(Ev *v, const HArg *a, double *d)
{
    Ti ti;
    if (!ev_tinfo(v, a->ty, &ti))
        return false;
    *d = ev_fget(&ti, a->x);
    return true;
}

static uint64_t dret(double d)
{
    Ti ti = {8, false, false, true};
    return ev_fput(&ti, d);
}

static uint64_t h_floor(Ev *v, const HArg *a, unsigned na)
{
    double d;
    NEED(1);
    return dbl(v, a, &d) ? dret(floor(d)) : 0;
}

static uint64_t h_ceil(Ev *v, const HArg *a, unsigned na)
{
    double d;
    NEED(1);
    return dbl(v, a, &d) ? dret(ceil(d)) : 0;
}

static uint64_t h_frexp(Ev *v, const HArg *a, unsigned na)
{
    double d, m;
    int e;
    uint8_t *p;
    NEED(2);
    if (!dbl(v, a, &d))
        return 0;
    m = frexp(d, &e);
    p = wr(v, a[1].x, 4, "frexp");
    if (!p)
        return 0;
    memcpy(p, &e, 4);
    return dret(m);
}

static uint64_t h_exit(Ev *v, const HArg *a, unsigned na)
{
    NEED(1);
    ev_fail(v, EV_EXIT, "exit");
    if (v->res.status == EV_EXIT)
        v->res.code = (int)a[0].x;
    return 0;
}

static uint64_t h_abort(Ev *v, const HArg *a, unsigned na)
{
    (void)a;
    (void)na;
    TRAP(v, "abort() called");
    return 0;
}

/* ---- the table --------------------------------------------------------------------- */

static const struct {
    const char *name;
    HostFn fn;
} host[] = {
    {"printf", h_printf},   {"fprintf", h_fprintf}, {"sprintf", h_sprintf}, {"snprintf", h_snprintf},
    {"puts", h_puts},       {"fputs", h_fputs},     {"putchar", h_putchar},
    {"malloc", h_malloc},   {"calloc", h_calloc},   {"realloc", h_realloc}, {"free", h_free},
    {"memcpy", h_memcpy},   {"memmove", h_memmove}, {"memset", h_memset},   {"memcmp", h_memcmp},
    {"memchr", h_memchr},   {"strlen", h_strlen},   {"strcmp", h_strcmp},   {"strncmp", h_strncmp},
    {"strcpy", h_strcpy},   {"strncpy", h_strncpy}, {"strcat", h_strcat},   {"strchr", h_strchr},
    {"strrchr", h_strrchr}, {"abs", h_abs},         {"labs", h_labs},       {"llabs", h_labs},
    {"tolower", h_tolower}, {"toupper", h_toupper}, {"floor", h_floor},     {"ceil", h_ceil},
    {"frexp", h_frexp},     {"exit", h_exit},       {"abort", h_abort},
};

HostFn ev_host_fn(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof host / sizeof *host; i++)
        if (!strcmp(host[i].name, name))
            return host[i].fn;
    return NULL;
}

/* stdout and stderr: a pointer object holding a pointer to a one-byte token. */
uint32_t ev_host_object(Ev *v, const char *name)
{
    int k = !strcmp(name, "stdout") ? 1 : !strcmp(name, "stderr") ? 2 : 0;
    if (!k)
        return 0;
    if (!v->stream[k]) {
        uint32_t tok = ev_new_obj(v, 1, true), ptr = ev_new_obj(v, 8, true);
        uint64_t p;
        if (!tok || !ptr)
            return 0;
        v->obj.data[tok - 1].init[0] = 1;
        p = mkptr(tok, 0);
        memcpy(v->obj.data[ptr - 1].b, &p, 8);
        memset(v->obj.data[ptr - 1].init, 1, 8);
        v->token[k] = tok;
        v->stream[k] = ptr;
    }
    return v->stream[k];
}
