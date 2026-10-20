/* The C23 <stdbit.h> built-ins for tests/asm_exec.py: every function on every unsigned width,
 * with operands that hit the zero, all-ones, single-bit and mixed cases.  The back end does
 * not run -std=c23, so the calls are the __builtin_stdc_* ones <stdbit.h> expands to. */
#define SC(N, T, F)                                                            \
long long N##_##F(T v) { return (long long)__builtin_stdc_##F(v); }
#define SC_ALL(N, T)                                                           \
    SC(N, T, leading_zeros) SC(N, T, leading_ones) SC(N, T, trailing_zeros)    \
    SC(N, T, trailing_ones) SC(N, T, first_leading_zero) SC(N, T, first_leading_one) \
    SC(N, T, first_trailing_zero) SC(N, T, first_trailing_one) SC(N, T, count_zeros) \
    SC(N, T, count_ones) SC(N, T, has_single_bit) SC(N, T, bit_width)          \
    SC(N, T, bit_floor)
SC_ALL(uc, unsigned char)
SC_ALL(us, unsigned short)
SC_ALL(ui, unsigned)
SC_ALL(ul, unsigned long)
SC_ALL(ull, unsigned long long)

/* bit_ceil is undefined when the result does not fit, so the operands stay at half the range */
unsigned char uc_bit_ceil(unsigned char v) { return __builtin_stdc_bit_ceil((unsigned char)(v >> 1)); }
unsigned short us_bit_ceil(unsigned short v) { return __builtin_stdc_bit_ceil((unsigned short)(v >> 1)); }
unsigned ui_bit_ceil(unsigned v) { return __builtin_stdc_bit_ceil(v >> 1); }
unsigned long ul_bit_ceil(unsigned long v) { return __builtin_stdc_bit_ceil(v >> 1); }
unsigned long long ull_bit_ceil(unsigned long long v) { return __builtin_stdc_bit_ceil(v >> 1); }

/* the result types */
int t_types(unsigned char c)
{
    return (int)sizeof __builtin_stdc_count_ones(c) * 100 + (int)sizeof __builtin_stdc_bit_floor(c) * 10
         + (int)sizeof __builtin_stdc_has_single_bit(c);
}
int t_cond(unsigned v) { return __builtin_stdc_has_single_bit(v) ? __builtin_stdc_trailing_zeros(v) : -1; }
int t_expr(unsigned a, unsigned long long b) { return (int)(__builtin_stdc_count_ones(a + 1) + __builtin_stdc_bit_width(b * 3)); }

/* The edge operands (zero, all ones, the top and bottom bits, alternating bits) the random
 * arguments seldom reach: a hash of every function over them, per width. */
#define SWEEP(N, T, BITS)                                                      \
long long N##_sweep(void)                                                      \
{                                                                              \
    T hi = (T)1 << (BITS - 1), ones = (T)~(T)0;                                \
    T v[] = {0, 1, 2, 3, 5, 6, 7, 8, hi, hi + 1, hi - 1, hi >> 1, (T)(hi | 1), ones, \
             (T)(ones - 1), (T)(ones >> 1), (T)0x5555555555555555ull, (T)0xaaaaaaaaaaaaaaaaull, \
             (T)0x0f0f0f0f0f0f0f0full, (T)0x00ff00ff00ff00ffull, (T)0x123456789abcdef0ull}; \
    long long acc = 0;                                                         \
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
    return acc;                                                                \
}
SWEEP(uc, unsigned char, 8)
SWEEP(us, unsigned short, 16)
SWEEP(ui, unsigned, 32)
SWEEP(ull, unsigned long long, 64)
