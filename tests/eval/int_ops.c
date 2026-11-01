/* + - * / % & | ^ << >> on 8-, 16-, 32- and 64-bit integers, signed (defined cases only) and
 * unsigned, run and folded; eval.sh compares the evaluator with gcc */
#include <stdio.h>
#include <stdint.h>

#define H(h, v) ((h) * 1000003ULL + (unsigned long long)(v))

#define UOPS(N, T, W)                                                              \
    static unsigned long long u##N(T a, T b)                                       \
    {                                                                              \
        unsigned long long h = 7;                                                  \
        unsigned k = (unsigned)(b % W);                                            \
        h = H(h, (T)(a + b)); h = H(h, (T)(a - b)); h = H(h, (T)((uint64_t)a * b)); \
        if (b) { h = H(h, (T)(a / b)); h = H(h, (T)(a % b)); }                     \
        h = H(h, (T)(a & b)); h = H(h, (T)(a | b)); h = H(h, (T)(a ^ b));          \
        h = H(h, (T)(a >> k)); h = H(h, (T)((uint64_t)a << k));                    \
        return h;                                                                  \
    }

/* signed: every operation is done wide first and kept only when it fits T */
#define SOPS(N, T, W, LO)                                                            \
    static unsigned long long s##N(T a, T b)                                       \
    {                                                                              \
        unsigned long long h = 11;                                                 \
        long long x = a, y = b, lo = (LO), hi = -(lo + 1);                          \
        unsigned k = (unsigned)((y < 0 ? -y : y) % W);                             \
        if (x + y >= lo && x + y <= hi) h = H(h, (T)(a + b));                      \
        if (x - y >= lo && x - y <= hi) h = H(h, (T)(a - b));                      \
        if (W < 64 && x * y >= lo && x * y <= hi) h = H(h, (T)(a * b));            \
        if (b != 0 && !(b == -1 && x == lo)) { h = H(h, (T)(a / b)); h = H(h, (T)(a % b)); } \
        h = H(h, (T)(a & b)); h = H(h, (T)(a | b)); h = H(h, (T)(a ^ b));          \
        h = H(h, (T)(a >> k));                                                     \
        if (a >= 0 && W < 64 && (x << k) <= hi) h = H(h, (T)(a << k));             \
        return h;                                                                  \
    }

UOPS(8, uint8_t, 8)
UOPS(16, uint16_t, 16)
UOPS(32, uint32_t, 32)
UOPS(64, uint64_t, 64)
SOPS(8, int8_t, 8, -128)
SOPS(16, int16_t, 16, -32768)
SOPS(32, int32_t, 32, -2147483647LL - 1)
SOPS(64, int64_t, 64, -9223372036854775807LL - 1)

int main(void)
{
    static const long long v[] = {0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 100, 127, 128, 255,
                                  256, 1000, 32767, 32768, 65535, 65536, 2147483647LL,
                                  2147483648LL, 4294967295LL, 4294967296LL, -1, -2, -3, -7,
                                  -8, -100, -127, -128, -129, -255, -32768, -32769, -65536,
                                  -2147483648LL, -2147483649LL};
    unsigned long long h = 0;
    unsigned i, j;
    for (i = 0; i < sizeof v / sizeof v[0]; i++)
        for (j = 0; j < sizeof v / sizeof v[0]; j++) {
            long long x = v[i], y = v[j];
            h = H(h, u8((uint8_t)x, (uint8_t)y)); h = H(h, u16((uint16_t)x, (uint16_t)y));
            h = H(h, u32((uint32_t)x, (uint32_t)y)); h = H(h, u64((uint64_t)x, (uint64_t)y));
            h = H(h, s8((int8_t)x, (int8_t)y)); h = H(h, s16((int16_t)x, (int16_t)y));
            h = H(h, s32((int32_t)x, (int32_t)y)); h = H(h, s64(x, y));
        }
    printf("%llu\n", h);
    printf("%d %u %d %d\n", 7 / 2 - 8 % 3, 1u << 31, -17 >> 2, (int)(0x7fffffff + 0u));
    return (int)(h & 127);
}
