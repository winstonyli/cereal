/* The C23 <stdbit.h> built-ins evaluated in place.  gcc 13 has no __builtin_stdc_*, so the gcc
 * side computes each one with the plain bit-scan built-ins below: an oracle written
 * independently of cintrin.h's bi_int. */
#if !__has_builtin(__builtin_stdc_bit_width)
#define W_(v) ((int)(8 * sizeof(v)))
#define LZ_(v) ((v) ? __builtin_clzll((unsigned long long)(v)) - (64 - W_(v)) : W_(v))
#define TZ_(v) ((v) ? __builtin_ctzll((unsigned long long)(v)) : W_(v))
#define C_(x) ((__typeof__(x))~(x))
#define S_(x, e) ({ __typeof__(x) v_ = (x); (e); })
#define __builtin_stdc_leading_zeros(x) S_(x, (unsigned)LZ_(v_))
#define __builtin_stdc_leading_ones(x) S_(C_(x), (unsigned)LZ_(v_))
#define __builtin_stdc_trailing_zeros(x) S_(x, (unsigned)TZ_(v_))
#define __builtin_stdc_trailing_ones(x) S_(C_(x), (unsigned)TZ_(v_))
#define __builtin_stdc_first_leading_zero(x) S_(C_(x), v_ ? (unsigned)LZ_(v_) + 1 : 0u)
#define __builtin_stdc_first_leading_one(x) S_(x, v_ ? (unsigned)LZ_(v_) + 1 : 0u)
#define __builtin_stdc_first_trailing_zero(x) S_(C_(x), v_ ? (unsigned)TZ_(v_) + 1 : 0u)
#define __builtin_stdc_first_trailing_one(x) S_(x, v_ ? (unsigned)TZ_(v_) + 1 : 0u)
#define __builtin_stdc_count_ones(x) S_(x, (unsigned)__builtin_popcountll((unsigned long long)v_))
#define __builtin_stdc_count_zeros(x) S_(C_(x), (unsigned)__builtin_popcountll((unsigned long long)v_))
#define __builtin_stdc_has_single_bit(x) S_(x, (_Bool)(v_ && !(v_ & (v_ - 1))))
#define __builtin_stdc_bit_width(x) S_(x, v_ ? (unsigned)(W_(v_) - LZ_(v_)) : 0u)
#define __builtin_stdc_bit_floor(x) S_(x, (__typeof__(v_))(v_ ? 1ull << (W_(v_) - 1 - LZ_(v_)) : 0))
#define __builtin_stdc_bit_ceil(x) \
    S_(x, (__typeof__(v_))(v_ <= 1 ? 1ull : 1ull << (64 - __builtin_clzll((unsigned long long)(v_ - 1)))))
#endif

static long long acc;

#define SWEEP(N, T, BITS)                                                      \
static void N(void)                                                            \
{                                                                              \
    T hi = (T)1 << (BITS - 1), ones = (T)~(T)0;                                \
    T v[] = {0, 1, 2, 3, 5, 6, 7, 8, hi, (T)(hi + 1), (T)(hi - 1), (T)(hi >> 1), (T)(hi | 1), ones, \
             (T)(ones - 1), (T)(ones >> 1), (T)0x5555555555555555ull, (T)0xaaaaaaaaaaaaaaaaull, \
             (T)0x0f0f0f0f0f0f0f0full, (T)0x00ff00ff00ff00ffull, (T)0x123456789abcdef0ull}; \
    unsigned i;                                                                \
    for (i = 0; i < sizeof v / sizeof *v; i++) {                               \
        T x = v[i];                                                            \
        acc = acc * 31 + __builtin_stdc_leading_zeros(x);                      \
        acc = acc * 31 + __builtin_stdc_leading_ones(x);                       \
        acc = acc * 31 + __builtin_stdc_trailing_zeros(x);                     \
        acc = acc * 31 + __builtin_stdc_trailing_ones(x);                      \
        acc = acc * 31 + __builtin_stdc_first_leading_zero(x);                 \
        acc = acc * 31 + __builtin_stdc_first_leading_one(x);                  \
        acc = acc * 31 + __builtin_stdc_first_trailing_zero(x);                \
        acc = acc * 31 + __builtin_stdc_first_trailing_one(x);                 \
        acc = acc * 31 + __builtin_stdc_count_zeros(x);                        \
        acc = acc * 31 + __builtin_stdc_count_ones(x);                         \
        acc = acc * 31 + __builtin_stdc_has_single_bit(x);                     \
        acc = acc * 31 + __builtin_stdc_bit_width(x);                          \
        acc = acc * 31 + (long long)__builtin_stdc_bit_floor(x);               \
        acc = acc * 31 + (long long)__builtin_stdc_bit_ceil((T)(x >> 1));      \
    }                                                                          \
}
SWEEP(sw8, unsigned char, 8)
SWEEP(sw16, unsigned short, 16)
SWEEP(sw32, unsigned, 32)
SWEEP(sw64, unsigned long long, 64)

int main(void)
{
    sw8();
    sw16();
    sw32();
    sw64();
    return (int)(acc & 0x7fffffff);
}
