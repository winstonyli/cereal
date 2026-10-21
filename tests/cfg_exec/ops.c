/* Integer operators at every width and signedness, free of undefined behaviour except signed
 * overflow (the sweep runs with -fwrapv): shift counts are masked, divisors are made nonzero
 * and not -1. */
#define OPS(N, T, BITS)                                                       \
T N##_add(T a, T b) { return a + b; }                                         \
T N##_sub(T a, T b) { return a - b; }                                         \
T N##_mul(T a, T b) { return a * b; }                                         \
T N##_div(T a, T b) { return a / (b == 0 || b == (T)-1 ? (T)3 : b); }         \
T N##_rem(T a, T b) { return a % (b == 0 || b == (T)-1 ? (T)3 : b); }         \
T N##_shl(T a, T b) { return (T)((unsigned long long)a << ((unsigned long long)b % BITS)); } \
T N##_shr(T a, T b) { return a >> ((unsigned long long)b % BITS); }           \
T N##_and(T a, T b) { return a & b; }                                         \
T N##_or(T a, T b) { return a | b; }                                          \
T N##_xor(T a, T b) { return a ^ b; }                                         \
T N##_neg(T a) { return -a; }                                                 \
T N##_not(T a) { return ~a; }                                                 \
int N##_lt(T a, T b) { return a < b; }                                        \
int N##_le(T a, T b) { return a <= b; }                                       \
int N##_eq(T a, T b) { return a == b; }                                       \
int N##_ne(T a, T b) { return a != b; }

OPS(sc, signed char, 8)
OPS(uc, unsigned char, 8)
OPS(ss, short, 16)
OPS(us, unsigned short, 16)
OPS(si, int, 32)
OPS(ui, unsigned, 32)
OPS(sl, long long, 64)
OPS(ul, unsigned long long, 64)

/* conversions between widths and signedness */
long long cv_s2l(int a) { return a; }
long long cv_u2l(unsigned a) { return a; }
unsigned long long cv_s2ul(int a) { return a; }
int cv_l2i(long long a) { return (int)a; }
unsigned char cv_i2uc(int a) { return (unsigned char)a; }
signed char cv_i2sc(int a) { return (signed char)a; }
int cv_uc2i(unsigned char a) { return a; }
int cv_cmp_mixed(int a, unsigned b) { return a < b; }
long long cv_mixed_add(int a, unsigned b) { return a + b; }
