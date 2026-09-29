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

#endif
