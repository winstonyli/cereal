/* simd.h - byte-run scanners for the lexer.
 *
 * Each scanner returns the first byte at or after p that ends the run.
 * Buffers in the location space are followed by >= SRC_PAD zero bytes and
 * every run stops at a 0 byte, so 16-byte loads never leave mapped memory.
 * SSE2 (baseline on x86-64) with a portable scalar fallback; define
 * CEREAL_NO_SIMD to force the fallback (self-hosting, testing). */
#ifndef CEREAL_SIMD_H
#define CEREAL_SIMD_H

#include "common.h"

#if (defined(__SSE2__) || defined(_M_X64)) && !defined(CEREAL_NO_SIMD)
#include <emmintrin.h>
#define CEREAL_SSE2 1
#endif

static inline unsigned simd_ctz(unsigned x)
{
#if defined(__GNUC__)
    return (unsigned)__builtin_ctz(x);
#else
    unsigned n = 0;
    while (!(x & 1u)) {
        x >>= 1;
        n++;
    }
    return n;
#endif
}

#ifdef CEREAL_SSE2
static inline __m128i sse_range(__m128i x, char lo, char hi)
{
    /* lo <= x <= hi, unsigned, via the sign-flip trick */
    const __m128i flip = _mm_set1_epi8((char)0x80);
    __m128i y = _mm_xor_si128(x, flip);
    __m128i l = _mm_set1_epi8((char)((unsigned char)lo ^ 0x80));
    __m128i h = _mm_set1_epi8((char)((unsigned char)hi ^ 0x80));
    return _mm_andnot_si128(_mm_or_si128(_mm_cmplt_epi8(y, l), _mm_cmpgt_epi8(y, h)),
                            _mm_set1_epi8((char)0xFF));
}
#endif

/* [A-Za-z0-9_] and bytes >= 0x80 (plus '$' when allowed) */
static inline const char *scan_ident(const char *p, bool dollar)
{
#ifdef CEREAL_SSE2
    const __m128i under = _mm_set1_epi8('_'), dol = _mm_set1_epi8('$');
    const __m128i case_bit = _mm_set1_epi8(0x20);
    for (;;) {
        __m128i x = _mm_loadu_si128((const __m128i *)(const void *)p);
        __m128i m = _mm_or_si128(sse_range(_mm_or_si128(x, case_bit), 'a', 'z'),
                                 sse_range(x, '0', '9'));
        unsigned mask;
        m = _mm_or_si128(m, _mm_cmpeq_epi8(x, under));
        m = _mm_or_si128(m, _mm_cmplt_epi8(x, _mm_setzero_si128())); /* >= 0x80 */
        if (dollar)
            m = _mm_or_si128(m, _mm_cmpeq_epi8(x, dol));
        mask = (unsigned)_mm_movemask_epi8(m);
        if (mask != 0xFFFFu)
            return p + simd_ctz(~mask & 0xFFFFu);
        p += 16;
    }
#else
    for (;;) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c >= 0x80 || (dollar && c == '$'))
            p++;
        else
            return p;
    }
#endif
}

/* spaces and tabs */
static inline const char *scan_blanks(const char *p)
{
#ifdef CEREAL_SSE2
    const __m128i sp = _mm_set1_epi8(' '), tab = _mm_set1_epi8('\t');
    for (;;) {
        __m128i x = _mm_loadu_si128((const __m128i *)(const void *)p);
        unsigned mask = (unsigned)_mm_movemask_epi8(
            _mm_or_si128(_mm_cmpeq_epi8(x, sp), _mm_cmpeq_epi8(x, tab)));
        if (mask != 0xFFFFu)
            return p + simd_ctz(~mask & 0xFFFFu);
        p += 16;
    }
#else
    while (*p == ' ' || *p == '\t')
        p++;
    return p;
#endif
}

/* First of: a, b, c, d, e, 0 (use duplicates to pad unused slots). */
static inline const char *scan_find5(const char *p, char a, char b, char c,
                                     char d, char e)
{
#ifdef CEREAL_SSE2
    const __m128i va = _mm_set1_epi8(a), vb = _mm_set1_epi8(b),
                  vc = _mm_set1_epi8(c), vd = _mm_set1_epi8(d),
                  ve = _mm_set1_epi8(e), z = _mm_setzero_si128();
    for (;;) {
        __m128i x = _mm_loadu_si128((const __m128i *)(const void *)p);
        __m128i m = _mm_or_si128(
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, va), _mm_cmpeq_epi8(x, vb)),
                         _mm_or_si128(_mm_cmpeq_epi8(x, vc), _mm_cmpeq_epi8(x, vd))),
            _mm_or_si128(_mm_cmpeq_epi8(x, ve), _mm_cmpeq_epi8(x, z)));
        unsigned mask = (unsigned)_mm_movemask_epi8(m);
        if (mask)
            return p + simd_ctz(mask);
        p += 16;
    }
#else
    for (;;) {
        char x = *p;
        if (x == a || x == b || x == c || x == d || x == e || x == 0)
            return p;
        p++;
    }
#endif
}

/* First of the skip scanner's specials: \n \r " ' / \\ ? 0 */
static inline const char *scan_skip_special(const char *p)
{
#ifdef CEREAL_SSE2
    const __m128i v1 = _mm_set1_epi8('\n'), v2 = _mm_set1_epi8('\r'),
                  v3 = _mm_set1_epi8('"'), v4 = _mm_set1_epi8('\''),
                  v5 = _mm_set1_epi8('/'), v6 = _mm_set1_epi8('\\'),
                  v7 = _mm_set1_epi8('?'), z = _mm_setzero_si128();
    for (;;) {
        __m128i x = _mm_loadu_si128((const __m128i *)(const void *)p);
        __m128i m = _mm_or_si128(
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, v1), _mm_cmpeq_epi8(x, v2)),
                         _mm_or_si128(_mm_cmpeq_epi8(x, v3), _mm_cmpeq_epi8(x, v4))),
            _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(x, v5), _mm_cmpeq_epi8(x, v6)),
                         _mm_or_si128(_mm_cmpeq_epi8(x, v7), _mm_cmpeq_epi8(x, z))));
        unsigned mask = (unsigned)_mm_movemask_epi8(m);
        if (mask)
            return p + simd_ctz(mask);
        p += 16;
    }
#else
    for (;;) {
        char x = *p;
        if (x == '\n' || x == '\r' || x == '"' || x == '\'' || x == '/' ||
            x == '\\' || x == '?' || x == 0)
            return p;
        p++;
    }
#endif
}

#endif
