/* Round trip through libzstd for tests/zstd_e2e.sh: compress compressible
 * generated data at several levels, decompress it, compare, and print a
 * checksum of every compressed frame (the gcc and the cereal builds must agree). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zstd.h"

static uint64_t fnv(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    while (n--)
        h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

static size_t gen(unsigned char *b, size_t n, unsigned seed, int kind)
{
    size_t i;
    uint32_t x = seed * 2654435761u + 1;
    for (i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        if (kind == 0)
            b[i] = (unsigned char)("abcdefgh"[(x >> 24) & 7]);      /* few symbols */
        else if (kind == 1)
            b[i] = i > 64 && (x >> 28) < 12 ? b[i - 1 - (x >> 20) % 64] : (unsigned char)(x >> 24);
        else
            b[i] = (unsigned char)(x >> 24);                          /* incompressible */
    }
    return n;
}

int main(void)
{
    static const int levels[] = {1, 3, 9, 19, -5};
    static const size_t sizes[] = {0, 1, 100, 5000, 70000, 300000};
    size_t si, li, kind;
    for (kind = 0; kind < 3; kind++)
        for (si = 0; si < sizeof sizes / sizeof *sizes; si++)
            for (li = 0; li < sizeof levels / sizeof *levels; li++) {
                size_t n = sizes[si], cap = ZSTD_compressBound(n), c, d;
                unsigned char *src = malloc(n + 1), *cmp = malloc(cap), *out = malloc(n + 1);
                gen(src, n, (unsigned)(si * 7 + li), (int)kind);
                c = ZSTD_compress(cmp, cap, src, n, levels[li]);
                if (ZSTD_isError(c)) {
                    printf("compress error %s\n", ZSTD_getErrorName(c));
                    return 1;
                }
                d = ZSTD_decompress(out, n + 1, cmp, c);
                if (ZSTD_isError(d) || d != n || memcmp(src, out, n)) {
                    printf("MISMATCH kind %zu size %zu level %d\n", kind, n, levels[li]);
                    return 1;
                }
                printf("%zu %zu %d: %zu %016llx\n", kind, n, levels[li], c,
                       (unsigned long long)fnv(cmp, c));
                free(src);
                free(cmp);
                free(out);
            }
    /* the multithreaded compressor (zstdmt_compress.c, with its bit-fields) */
    for (kind = 0; kind < 3; kind++) {
        size_t n = 1000000, cap = ZSTD_compressBound(n), c, d;
        unsigned char *src = malloc(n), *cmp = malloc(cap), *out = malloc(n);
        ZSTD_CCtx *cc = ZSTD_createCCtx();
        gen(src, n, (unsigned)kind, (int)kind);
        ZSTD_CCtx_setParameter(cc, ZSTD_c_nbWorkers, 2);
        ZSTD_CCtx_setParameter(cc, ZSTD_c_jobSize, 256 << 10);
        ZSTD_CCtx_setParameter(cc, ZSTD_c_compressionLevel, 3);
        c = ZSTD_compress2(cc, cmp, cap, src, n);
        d = ZSTD_isError(c) ? c : ZSTD_decompress(out, n, cmp, c);
        if (ZSTD_isError(d) || d != n || memcmp(src, out, n)) {
            printf("MT MISMATCH kind %zu\n", kind);
            return 1;
        }
        printf("mt %zu: %zu %016llx\n", kind, c, (unsigned long long)fnv(cmp, c));
        ZSTD_freeCCtx(cc);
        free(src);
        free(cmp);
        free(out);
    }
    puts("ok");
    return 0;
}
