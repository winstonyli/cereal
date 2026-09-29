/* hash.h - 64-bit content hashes (cell cache keys and fingerprints).
 *
 * An xxHash64-style construction: four independent lanes over 32-byte
 * stripes, then a final avalanche.  Not cryptographic; collisions between
 * honest inputs are negligible at 64 bits. */
#ifndef CEREAL_HASH_H
#define CEREAL_HASH_H

#include <stdint.h>
#include <string.h>

#define H64_P1 0x9E3779B185EBCA87ull
#define H64_P2 0xC2B2AE3D27D4EB4Full
#define H64_P3 0x165667B19E3779F9ull
#define H64_P4 0x85EBCA77C2B2AE63ull
#define H64_P5 0x27D4EB2F165667C5ull

static inline uint64_t h64_rotl(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static inline uint64_t h64_read(const unsigned char *p)
{
    uint64_t w;
    memcpy(&w, p, 8);
    return w;
}

static inline uint64_t h64_round(uint64_t acc, uint64_t w)
{
    acc += w * H64_P2;
    return h64_rotl(acc, 31) * H64_P1;
}

static inline uint64_t h64_merge(uint64_t acc, uint64_t v)
{
    acc ^= h64_round(0, v);
    return acc * H64_P1 + H64_P4;
}

static inline uint64_t h64_avalanche(uint64_t h)
{
    h ^= h >> 33;
    h *= H64_P2;
    h ^= h >> 29;
    h *= H64_P3;
    h ^= h >> 32;
    return h;
}

static inline uint64_t hash64(const void *data, size_t n, uint64_t seed)
{
    const unsigned char *p = data, *end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = seed + H64_P1 + H64_P2, v2 = seed + H64_P2, v3 = seed,
                 v4 = seed - H64_P1;
        const unsigned char *lim = end - 32;
        do {
            v1 = h64_round(v1, h64_read(p));
            v2 = h64_round(v2, h64_read(p + 8));
            v3 = h64_round(v3, h64_read(p + 16));
            v4 = h64_round(v4, h64_read(p + 24));
            p += 32;
        } while (p <= lim);
        h = h64_rotl(v1, 1) + h64_rotl(v2, 7) + h64_rotl(v3, 12) +
            h64_rotl(v4, 18);
        h = h64_merge(h, v1);
        h = h64_merge(h, v2);
        h = h64_merge(h, v3);
        h = h64_merge(h, v4);
    } else {
        h = seed + H64_P5;
    }
    h += (uint64_t)n;
    while (p + 8 <= end) {
        h ^= h64_round(0, h64_read(p));
        h = h64_rotl(h, 27) * H64_P1 + H64_P4;
        p += 8;
    }
    while (p < end) {
        h ^= (uint64_t)*p++ * H64_P5;
        h = h64_rotl(h, 11) * H64_P1;
    }
    return h64_avalanche(h);
}

/* Combine an ordered sequence of hashes. */
static inline uint64_t hash64_mix(uint64_t h, uint64_t v)
{
    return h64_avalanche(h64_round(h ^ H64_P3, v) + H64_P4);
}

static inline uint64_t hash64_str(const char *s, uint64_t seed)
{
    return s ? hash64(s, strlen(s), seed) : seed ^ H64_P5;
}

/* ---- polynomial hashes mod 2^61 - 1 --------------------------------------
 * For sequences that must be hashed in pieces and combined: the hash of a
 * concatenation AB is H(A) * BASE^|B| + H(B).  A prime modulus (not 2^64,
 * where Thue-Morse sequences collide for any base). */
#define M61 ((1ull << 61) - 1)
#define M61_BASE 0x1CE4E5B9A7F3D1Bull   /* < M61 */

static inline uint64_t m61_reduce(uint64_t x)
{
    x = (x & M61) + (x >> 61);
    return x >= M61 ? x - M61 : x;
}

/* a, b < M61 */
static inline uint64_t m61_mul(uint64_t a, uint64_t b)
{
    uint64_t l1 = a & 0xFFFFFFFFu, h1 = a >> 32, l2 = b & 0xFFFFFFFFu,
             h2 = b >> 32;
    uint64_t l = l1 * l2, m = l1 * h2 + l2 * h1, h = h1 * h2;
    uint64_t r = (l & M61) + (l >> 61) + (h << 3) + (m >> 29) +
                 (m << 35 >> 3) + 1;
    r = (r & M61) + (r >> 61);
    r = (r & M61) + (r >> 61);
    return r - 1;
}

static inline uint64_t m61_add(uint64_t a, uint64_t b)
{
    uint64_t r = a + b;
    return r >= M61 ? r - M61 : r;
}

static inline uint64_t m61_sub(uint64_t a, uint64_t b)
{
    return a >= b ? a - b : a + M61 - b;
}

static inline uint64_t m61_pow(uint64_t b, uint64_t e)
{
    uint64_t r = 1;
    while (e) {
        if (e & 1)
            r = m61_mul(r, b);
        b = m61_mul(b, b);
        e >>= 1;
    }
    return r;
}

/* Append one element (any 64-bit value) to a sequence hash. */
static inline uint64_t m61_push(uint64_t h, uint64_t v)
{
    return m61_add(m61_mul(h, M61_BASE), m61_reduce(v));
}

/* H(AB) from H(A), H(B) and |B|. */
static inline uint64_t m61_concat(uint64_t ha, uint64_t hb, uint64_t nb)
{
    return m61_add(m61_mul(ha, m61_pow(M61_BASE, nb)), hb);
}

/* H(B) from prefix hashes H(AB), H(A) and |B|. */
static inline uint64_t m61_range(uint64_t hab, uint64_t ha, uint64_t nb)
{
    return m61_sub(hab, m61_mul(ha, m61_pow(M61_BASE, nb)));
}

#endif
