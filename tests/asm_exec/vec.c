/* Vector types (GNU vector_size) and the SSE2 intrinsics built on them, for tests/asm_exec.py: a
 * vector is a memory aggregate and an operation on vectors is the scalar operation on every lane. */
#include <emmintrin.h>
typedef int v4si __attribute__((vector_size(16)));
typedef unsigned int v4su __attribute__((vector_size(16)));
typedef short v8hi __attribute__((vector_size(16)));
typedef unsigned char v16qu __attribute__((vector_size(16)));
typedef signed char v16qi __attribute__((vector_size(16)));
typedef long long v2di __attribute__((vector_size(16)));
typedef float v4sf __attribute__((vector_size(16)));
typedef double v2df __attribute__((vector_size(16)));
typedef int v2si __attribute__((vector_size(8)));

static v4si addv(v4si a, v4si b)
{
    return a + b;
}

static int fold4(v4si x)
{
    return x[0] * 1 + x[1] * 3 + x[2] * 5 + x[3] * 7;
}

int arith(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    v4si z = x + y * x - 3;
    return fold4(z) ^ fold4(x - y) ^ fold4(x & y | 5) ^ fold4(x ^ y);
}

int divmod(int a, int b)
{
    v4si x = {a, b, a + 1, b + 1}, y = {3, 5, 7, 9};
    return fold4(x / y) + fold4(x % y);
}

int unary(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d};
    return fold4(-x) + fold4(~x) + fold4(+x);
}

int shifts(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d};
    v4su u = {a, b, c, d};
    v4si n = {1, 2, 3, 4};
    return fold4(x << 2) ^ fold4(x >> n) ^ fold4((v4si)(u >> 3)) ^ fold4(x << n);
}

int compares(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    return fold4(x < y) + 3 * fold4(x == y) + 5 * fold4(x >= y) + 7 * fold4(x != y) + fold4(x > 3);
}

int scalars(int a, int b)
{
    v4si x = {a, b, a - b, a + b};
    return fold4(x + 1) + fold4(2 * x) + fold4(x - b) + fold4(100 / (x | 1));
}

int bytes(int a, int b)
{
    v4si x = {a, b, a ^ b, a - b};
    v16qu u = (v16qu)x;
    v16qi s = (v16qi)x;
    v16qu w = u + u * 3;
    return u[0] + u[5] * 3 + u[15] * 7 + s[1] + s[6] * 5 + w[2] + w[9] * 11 + (u > w)[4];
}

int shorts(int a, int b)
{
    v8hi x = {a, b, a + b, a - b, a * b, a ^ b, 1, -1}, y = {b, a, 7, 8, 9, 10, 11, 12};
    v8hi z = x * y + x;
    return z[0] + z[1] * 3 + z[2] * 5 + z[3] * 7 + z[4] * 9 + z[5] * 11 + z[6] * 13 + z[7] * 15;
}

long long wide(long long a, long long b)
{
    v2di x = {a, b}, y = {b, a};
    v2di z = x * y - x + (x < y);
    return z[0] ^ z[1];
}

int floats(float a, float b)
{
    v4sf x = {a, b, a + b, a - b}, y = {b, a, 2.0f, 0.5f};
    v4sf z = x * y + x / (y + 1.5f) - 1.0f;
    v4si m = x < y;
    return (int)(z[0] * 16) + (int)(z[1] * 8) + (int)(z[2] * 4) + (int)(z[3] * 2) + m[0] + m[1] * 3 + m[2] * 5 + m[3] * 7;
}

double doubles(double a, double b)
{
    v2df x = {a, b}, y = {b, a};
    v2df z = (x - y) * (x + y) / (y * y + 1.0);
    return z[0] - 2 * z[1];
}

int compound(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    x += y;
    x *= 3;
    x -= 1;
    x ^= y;
    x <<= 1;
    x |= 1;
    y &= x;
    return fold4(x) * 3 + fold4(y);
}

int lanes(int a, int b, int c, int d)
{
    v4si x = {0, 0, 0, 0};
    x[0] = a;
    x[3] = d;
    x[1] = x[0] + b;
    x[2] += c;
    x[2] *= 2;
    return fold4(x);
}

int calls(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {1, 2, 3, 4};
    return fold4(addv(x, y)) + fold4(addv(addv(x, x), addv(y, y)));
}

int pointers(int a, int b)
{
    v4si buf[3], *p = buf;
    int i;
    for (i = 0; i < 3; i++) {
        v4si t = {a + i, b - i, i, i * a};
        p[i] = t + p[i > 0 ? i - 1 : 0] * 0 + t;
    }
    return fold4(buf[0]) + 3 * fold4(buf[1]) + 5 * fold4(buf[2]);
}

int small(int a, int b)
{
    v2si x = {a, b}, y = {b, a};
    v2si z = x * y + x;
    return z[0] * 3 + z[1];
}

int cond(int a, int b, int c, int d)
{
    v4si x = {a, b, c, d}, y = {d, c, b, a};
    v4si m = x < y;
    return fold4((x & m) | (y & ~m));
}

/* gcc's intrinsics: inline functions of a system header, taking and returning vectors */
static int mask16(__m128i v)
{
    return _mm_movemask_epi8(v);
}

int mm_eq(long long a, long long b, long long c, long long d)
{
    __m128i x = _mm_set_epi64x(a, b), y = _mm_set_epi64x(c, d);
    return mask16(_mm_cmpeq_epi8(x, y)) + 65536 * mask16(_mm_cmpeq_epi8(x, x));
}

int mm_bytes(int a, int b)
{
    __m128i x = _mm_set1_epi8((char)a), y = _mm_set_epi8(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, b, 13, 14, 15, a);
    return mask16(_mm_cmpgt_epi8(x, y)) ^ (mask16(_mm_cmplt_epi8(x, y)) << 3) ^ mask16(_mm_add_epi8(x, y)) ^
           mask16(_mm_sub_epi8(y, x));
}

int mm_logic(int a, int b, int c, int d)
{
    __m128i x = _mm_set_epi32(a, b, c, d), y = _mm_set_epi32(d, c, b, a);
    __m128i z = _mm_xor_si128(_mm_and_si128(x, y), _mm_or_si128(x, _mm_andnot_si128(y, x)));
    return _mm_cvtsi128_si32(z) + mask16(z) + mask16(_mm_add_epi32(z, x)) + mask16(_mm_mullo_epi16(x, y));
}

int mm_mem(int a, int b)
{
    char buf[48];
    int i, s = 0;
    for (i = 0; i < 48; i++)
        buf[i] = (char)(a + i * b);
    _mm_storeu_si128((__m128i *)(buf + 1), _mm_loadu_si128((const __m128i *)(buf + 20)));
    _mm_storeu_si128((__m128i *)(buf + 32), _mm_set1_epi8((char)b));
    for (i = 0; i < 48; i++)
        s = s * 31 + buf[i];
    return s + mask16(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)buf), _mm_loadu_si128((const __m128i *)(buf + 16))));
}

/* the rest of SSE2 integer: every lane of the result hashed */
static int hash(__m128i v)
{
    unsigned char b[16];
    int i, s = 17;
    _mm_storeu_si128((__m128i *)b, v);
    for (i = 0; i < 16; i++)
        s = s * 31 + b[i];
    return s;
}

static __m128i mk(int a, int b, int c, int d)
{
    return _mm_set_epi32(a, b, c, d);
}

int mm_minmax(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d), y = mk(d, a, b, c);
    return hash(_mm_max_epu8(x, y)) ^ hash(_mm_min_epu8(x, y)) ^ hash(_mm_max_epi16(x, y)) ^ hash(_mm_min_epi16(x, y));
}

int mm_sat(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d), y = mk(d, a, b, c);
    return hash(_mm_adds_epi8(x, y)) + hash(_mm_adds_epu8(x, y)) + hash(_mm_adds_epi16(x, y)) + hash(_mm_adds_epu16(x, y)) +
           hash(_mm_subs_epi8(x, y)) + hash(_mm_subs_epu8(x, y)) + hash(_mm_subs_epi16(x, y)) + hash(_mm_subs_epu16(x, y));
}

int mm_arith(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d), y = mk(d, a, b, c);
    return hash(_mm_avg_epu8(x, y)) ^ hash(_mm_avg_epu16(x, y)) ^ hash(_mm_mulhi_epi16(x, y)) ^ hash(_mm_mulhi_epu16(x, y)) ^
           hash(_mm_madd_epi16(x, y)) ^ hash(_mm_mul_epu32(x, y)) ^ hash(_mm_sad_epu8(x, y));
}

int mm_shift(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d);
    int n = d & 63;
    return hash(_mm_slli_epi16(x, n)) ^ hash(_mm_slli_epi32(x, n)) ^ hash(_mm_slli_epi64(x, n)) ^ hash(_mm_srli_epi16(x, n)) ^
           hash(_mm_srli_epi32(x, n)) ^ hash(_mm_srli_epi64(x, n)) ^ hash(_mm_srai_epi16(x, n)) ^ hash(_mm_srai_epi32(x, n)) ^
           hash(_mm_sll_epi32(x, _mm_cvtsi32_si128(n))) ^ hash(_mm_srl_epi64(x, _mm_cvtsi32_si128(n))) ^
           hash(_mm_sra_epi16(x, _mm_cvtsi32_si128(n))) ^
           hash(_mm_slli_epi16(x, 16)) ^ hash(_mm_srai_epi16(x, 16)) ^ hash(_mm_srli_epi32(x, 32)) ^ hash(_mm_srai_epi32(x, 32)) ^
           hash(_mm_slli_epi64(x, 64)) ^ hash(_mm_srai_epi32(x, -1)) ^ hash(_mm_srli_epi16(x, 15));
}

int mm_shuffle(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d), y = mk(d, a, b, c);
    return hash(_mm_unpacklo_epi8(x, y)) ^ hash(_mm_unpackhi_epi8(x, y)) ^ hash(_mm_unpacklo_epi16(x, y)) ^
           hash(_mm_unpackhi_epi16(x, y)) ^ hash(_mm_unpacklo_epi32(x, y)) ^ hash(_mm_unpackhi_epi32(x, y)) ^
           hash(_mm_unpacklo_epi64(x, y)) ^ hash(_mm_unpackhi_epi64(x, y)) ^ hash(_mm_shuffle_epi32(x, 0x1b)) ^
           hash(_mm_shufflelo_epi16(x, 0x4e)) ^ hash(_mm_shufflehi_epi16(x, 0xb1)) ^ hash(_mm_slli_si128(x, 3)) ^
           hash(_mm_srli_si128(y, 5)) ^ hash(_mm_move_epi64(x));
}

int mm_pack(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d), y = mk(d * 300, a * 7, b, c * 40000);
    return hash(_mm_packs_epi16(x, y)) ^ hash(_mm_packus_epi16(x, y)) ^ hash(_mm_packs_epi32(x, y));
}

int mm_elem(int a, int b, int c, int d)
{
    __m128i x = mk(a, b, c, d);
    x = _mm_insert_epi16(x, b, 3);
    return _mm_cvtsi128_si32(x) + _mm_extract_epi16(x, 5) + _mm_extract_epi16(_mm_srli_si128(x, 4), 1) + hash(x);
}

/* single and double precision; inputs include NaN and infinities */
static __m128 fk(int a, int b, int c, int d)
{
    union { unsigned u; float f; } nn = {0x7fc00000u}, ii = {0x7f800000u};
    float nan = nn.f, inf = ii.f;
    float p[8] = {(float)a / 8, (float)b * 1.5f, nan, (float)c, inf, -inf, (float)d / 3, 0.0f};
    return _mm_set_ps(p[(d >> 3) & 7], p[(c >> 2) & 7], p[(b >> 1) & 7], p[a & 7]);
}

static __m128d dk(int a, int b, int c, int d)
{
    union { unsigned long long u; double f; } nn = {0x7ff8000000000000ull}, ii = {0x7ff0000000000000ull};
    double nan = nn.f, inf = ii.f;
    double p[8] = {(double)a / 8, (double)b * 1.5, nan, (double)c, inf, -inf, (double)d / 3, 0.0};
    return _mm_set_pd(p[(d >> 3) & 7], p[(c >> 2) & 7]);
}

static int fh(__m128 v) { return hash(_mm_castps_si128(v)); }
static int dh(__m128d v) { return hash(_mm_castpd_si128(v)); }

int mm_fops(int a, int b, int c, int d)
{
    __m128 x = fk(a, b, c, d), y = fk(d, a, b, c);
    return fh(_mm_add_ps(x, y)) ^ fh(_mm_sub_ps(x, y)) ^ fh(_mm_mul_ps(x, y)) ^ fh(_mm_div_ps(x, y)) ^ fh(_mm_min_ps(x, y)) ^
           fh(_mm_max_ps(x, y)) ^ fh(_mm_add_ss(x, y)) ^ fh(_mm_sub_ss(x, y)) ^ fh(_mm_mul_ss(x, y)) ^ fh(_mm_div_ss(x, y)) ^
           fh(_mm_min_ss(x, y)) ^ fh(_mm_max_ss(x, y)) ^ fh(_mm_and_ps(x, y)) ^ fh(_mm_or_ps(x, y)) ^ fh(_mm_xor_ps(x, y)) ^
           fh(_mm_andnot_ps(x, y));
}

int mm_dops(int a, int b, int c, int d)
{
    __m128d x = dk(a, b, c, d), y = dk(d, a, b, c);
    return dh(_mm_add_pd(x, y)) ^ dh(_mm_sub_pd(x, y)) ^ dh(_mm_mul_pd(x, y)) ^ dh(_mm_div_pd(x, y)) ^ dh(_mm_min_pd(x, y)) ^
           dh(_mm_max_pd(x, y)) ^ dh(_mm_add_sd(x, y)) ^ dh(_mm_sub_sd(x, y)) ^ dh(_mm_mul_sd(x, y)) ^ dh(_mm_div_sd(x, y)) ^
           dh(_mm_min_sd(x, y)) ^ dh(_mm_max_sd(x, y)) ^ dh(_mm_and_pd(x, y)) ^ dh(_mm_or_pd(x, y)) ^ dh(_mm_xor_pd(x, y)) ^
           dh(_mm_andnot_pd(x, y));
}

int mm_fcmp(int a, int b, int c, int d)
{
    __m128 x = fk(a, b, c, d), y = fk(d, a, b, c);
    __m128d u = dk(a, b, c, d), v = dk(d, a, b, c);
    return fh(_mm_cmpeq_ps(x, y)) ^ fh(_mm_cmplt_ps(x, y)) ^ fh(_mm_cmple_ps(x, y)) ^ fh(_mm_cmpgt_ps(x, y)) ^
           fh(_mm_cmpge_ps(x, y)) ^ fh(_mm_cmpneq_ps(x, y)) ^ fh(_mm_cmpnlt_ps(x, y)) ^ fh(_mm_cmpnle_ps(x, y)) ^
           fh(_mm_cmpngt_ps(x, y)) ^ fh(_mm_cmpnge_ps(x, y)) ^ fh(_mm_cmpord_ps(x, y)) ^ fh(_mm_cmpunord_ps(x, y)) ^
           fh(_mm_cmpeq_ss(x, y)) ^ fh(_mm_cmplt_ss(x, y)) ^ fh(_mm_cmpunord_ss(x, y)) ^ fh(_mm_cmpnle_ss(x, y)) ^
           dh(_mm_cmpeq_pd(u, v)) ^ dh(_mm_cmplt_pd(u, v)) ^ dh(_mm_cmple_pd(u, v)) ^ dh(_mm_cmpgt_pd(u, v)) ^
           dh(_mm_cmpge_pd(u, v)) ^ dh(_mm_cmpneq_pd(u, v)) ^ dh(_mm_cmpnlt_pd(u, v)) ^ dh(_mm_cmpord_pd(u, v)) ^
           dh(_mm_cmpunord_pd(u, v)) ^ dh(_mm_cmpeq_sd(u, v)) ^ dh(_mm_cmpge_sd(u, v)) ^ dh(_mm_cmpngt_sd(u, v)) ^
           _mm_movemask_ps(x) * 3 + _mm_movemask_pd(u) * 5 +
           (_mm_comieq_ss(x, y) << 1) + (_mm_comilt_ss(x, y) << 2) + (_mm_comige_ss(x, y) << 3) + (_mm_ucomineq_ss(x, y) << 4) +
           (_mm_comieq_sd(u, v) << 5) + (_mm_comile_sd(u, v) << 6) + (_mm_ucomigt_sd(u, v) << 7) + (_mm_comineq_sd(u, v) << 8);
}

int mm_fshuf(int a, int b, int c, int d)
{
    __m128 x = fk(a, b, c, d), y = fk(d, a, b, c);
    __m128d u = dk(a, b, c, d), v = dk(d, a, b, c);
    return fh(_mm_shuffle_ps(x, y, 0x1b)) ^ fh(_mm_shuffle_ps(x, y, 0xe4)) ^ fh(_mm_unpacklo_ps(x, y)) ^ fh(_mm_unpackhi_ps(x, y)) ^
           fh(__builtin_ia32_movss(x, y)) ^ fh(_mm_movehl_ps(x, y)) ^ fh(_mm_movelh_ps(x, y)) ^ dh(_mm_shuffle_pd(u, v, 1)) ^
           dh(_mm_shuffle_pd(u, v, 2)) ^ dh(_mm_unpacklo_pd(u, v)) ^ dh(_mm_unpackhi_pd(u, v)) ^ dh(__builtin_ia32_movsd(u, v));
}

int mm_fcvt(int a, int b, int c, int d)
{
    __m128 x = fk(a, b, c, d), y = fk(d, a, b, c);
    __m128d u = dk(a, b, c, d);
    __m128i i = mk(a, b, c, d);
    x = _mm_set_ps(a / 4.0f, b * 0.25f, c * 1.5f, d * 100.0f);
    u = _mm_set_pd(a * 2.5, d / 3.0);
    return fh(_mm_cvtepi32_ps(i)) ^ hash(_mm_cvttps_epi32(x)) ^ dh(_mm_cvtepi32_pd(i)) ^ dh(_mm_cvtps_pd(x)) ^
           fh(_mm_cvtpd_ps(u)) ^ hash(_mm_cvttpd_epi32(u));
}

int mm_fcvt2(int a, int b, int c, int d)
{
    __m128 x = _mm_set_ps(a / 4.0f, b * 0.25f, c * 1.5f, d * 100.0f), y = fk(d, a, b, c);
    __m128d u = _mm_set_pd(a * 2.5, d / 3.0);
    return dh(_mm_cvtsi32_sd(u, a)) ^ dh(_mm_cvtsi64_sd(u, (long long)a * b)) ^ fh(_mm_cvtsi32_ss(x, b)) ^
           fh(_mm_cvtsi64_ss(x, (long long)c * d)) ^ fh(_mm_cvtsd_ss(y, u)) ^ dh(_mm_cvtss_sd(u, x));
}

int mm_fcvt3(int a, int b, int c, int d)
{
    __m128 x = _mm_set_ps(a / 4.0f, b * 0.25f, c * 1.5f, d * 100.0f);
    __m128d u = _mm_set_pd(a * 2.5, d / 3.0);
    return _mm_cvttsd_si32(u) ^ (int)_mm_cvttsd_si64(u) ^ _mm_cvttss_si32(x) ^ (int)_mm_cvttss_si64(x);
}

/* __builtin_shuffle (constant and variable masks, one and two inputs), __builtin_shufflevector, and
 * the header functions that use them */
typedef int vi4 __attribute__((vector_size(16)));
typedef short vs8 __attribute__((vector_size(16)));

int shuf_var(int a, int b, int c, int d)
{
    vi4 x = {a, b, c, d}, y = {d, c, b, a}, m = {a, b, c, d}, m1 = {d, 0, 5, 9};
    vi4 r = __builtin_shuffle(x, m), s = __builtin_shuffle(x, y, m1), t = __builtin_shuffle(x, y, m);
    return hash((__m128i)r) ^ hash((__m128i)s) * 3 ^ hash((__m128i)t) * 5;
}

int shuf_const(int a, int b, int c, int d)
{
    vi4 x = {a, b, c, d}, y = {d, c, b, a};
    vs8 u = {a, b, c, d, d, c, b, a}, v = {c, d, a, b, b, a, d, c};
    vi4 r = __builtin_shuffle(x, (vi4){3, 2, 1, 0}), s = __builtin_shuffle(x, y, (vi4){4, 1, 6, 3});
    vs8 w = __builtin_shuffle(u, v, (vs8){8, 0, 9, 1, 10, 2, 11, 3});
    vi4 o1 = __builtin_shuffle(x, (vi4){5, -1, 9, 2}), o2 = __builtin_shuffle(x, y, (vi4){13, -1, 9, 2});   /* taken modulo the lanes in play */
    vi4 p = __builtin_shufflevector(x, y, 0, 5, 2, 7), q = __builtin_shufflevector(x, x, 3, 3, 1, 0);
    vi4 g = __builtin_shufflevector(x, y, 7, 6, 0, 1);
    vs8 z = __builtin_shufflevector(u, v, 15, 1, 8, 2, 9, 3, 10, 4);
    return hash((__m128i)r) ^ hash((__m128i)s) * 3 ^ hash((__m128i)w) * 5 ^ hash((__m128i)p) * 7 ^ hash((__m128i)q) * 9 ^
           hash((__m128i)g) * 11 ^ hash((__m128i)z) * 13 ^ hash((__m128i)o1) * 15 ^ hash((__m128i)o2) * 17;
}

typedef float vf4 __attribute__((vector_size(16)));
typedef double vd2 __attribute__((vector_size(16)));
typedef long long vl2 __attribute__((vector_size(16)));
typedef unsigned char vb8 __attribute__((vector_size(8)));
typedef unsigned short vu8 __attribute__((vector_size(16)));

int convvec(int a, int b, int c, int d)
{
    vi4 x = {a, b, c, d};
    vi4 s = {a & 255, b & 255, c & 255, d & 255};
    vf4 f = __builtin_convertvector(s, vf4);
    vi4 back = __builtin_convertvector(f * 1.5f, vi4);
    vd2 dd = {a & 1023, b & 1023};
    vl2 ll = __builtin_convertvector(dd * 2.5, vl2);
    vd2 d2 = __builtin_convertvector(ll, vd2);
    vs8 h = {a, b, c, d, d, c, b, a};
    vu8 u = __builtin_convertvector(h, vu8);
    vi4 w = __builtin_convertvector((vi4){a, b, c, d}, vi4);
    vb8 by = __builtin_convertvector((vs8){a, b, c, d, d, c, b, a}, vb8);
    return hash((__m128i)f) ^ hash((__m128i)back) * 3 ^ hash((__m128i)ll) * 5 ^ hash((__m128i)d2) * 7 ^ hash((__m128i)u) * 11 ^
           hash((__m128i)w) * 13 ^ (by[0] + by[1] * 3 + by[2] * 5 + by[3] * 7 + by[4] * 11 + by[5] * 13 + by[6] * 17 + by[7] * 19) * 17 ^ x[0];
}

int mm_undef(int a, int b, int c, int d)    /* a pragma inside the header function */
{
    __m128i u = _mm_undefined_si128();
    (void)u;
    (void)_mm_undefined_ps();
    return a + b * c - d;
}

int mm_move(int a, int b, int c, int d)
{
    __m128 x = fk(a, b, c, d), y = fk(d, a, b, c);
    __m128d u = dk(a, b, c, d), v = dk(d, a, b, c);
    return fh(_mm_move_ss(x, y)) ^ dh(_mm_move_sd(u, v));
}

/* square roots (negative, NaN and infinite lanes included), converts that round to nearest even
 * (ties, out of range), fences and non-temporal stores */
int mm_sqrt(int a, int b, int c, int d)
{
    __m128 x = fk(a, b, c, d), y = fk(d, a, b, c);
    __m128d u = dk(a, b, c, d), v = dk(d, a, b, c);
    return fh(_mm_sqrt_ps(x)) ^ fh(_mm_sqrt_ss(x)) * 3 ^ fh(_mm_sqrt_ps(y)) * 5 ^ dh(_mm_sqrt_pd(u)) ^ dh(_mm_sqrt_sd(u, v)) * 3 ^
           dh(_mm_sqrt_pd(v)) * 5;
}

int mm_round(int a, int b, int c, int d)
{
    __m128 x = _mm_set_ps(a / 2.0f, b * 0.5f - 0.25f, c * 1.5f, d * 1e9f);
    __m128 t = _mm_set_ps(0.5f, 1.5f, 2.5f, -0.5f);
    __m128d u = _mm_set_pd(a * 0.5, d * 1e18), w = _mm_set_pd(-2.5, c + 0.5);
    __m128 f = fk(a, b, c, d);
    __m128d g = dk(a, b, c, d);
    return fh(_mm_castsi128_ps(_mm_cvtps_epi32(x))) ^ hash(_mm_cvtps_epi32(t)) * 3 ^ hash(_mm_cvtps_epi32(f)) * 5 ^
           hash(_mm_cvtpd_epi32(u)) * 7 ^ hash(_mm_cvtpd_epi32(w)) * 11 ^ hash(_mm_cvtpd_epi32(g)) * 13 ^
           _mm_cvtss_si32(x) ^ _mm_cvtss_si32(t) * 3 ^ _mm_cvtss_si32(f) * 5 ^ _mm_cvtsd_si32(u) ^ _mm_cvtsd_si32(w) * 3 ^
           _mm_cvtsd_si32(g) * 5 ^ (int)_mm_cvtss_si64(x) ^ (int)_mm_cvtss_si64(t) * 3 ^ (int)(_mm_cvtsd_si64(u) >> 7) ^
           (int)_mm_cvtsd_si64(w) * 3 ^ (int)(_mm_cvtss_si64(f) >> 3) ^ (int)(_mm_cvtsd_si64(g) >> 5);
}

int mm_stream(int a, int b, int c, int d)
{
    static int xi[4];
    static long long xl[2];
    static float xf[4] __attribute__((aligned(16)));
    static double xd[2] __attribute__((aligned(16)));
    static __m128i xv[2];
    int s;
    _mm_stream_si32(&xi[a & 3], b);
    _mm_stream_si64(&xl[c & 1], (long long)b * d);
    _mm_stream_ps(xf, fk(a, b, c, d));
    _mm_stream_pd(xd, dk(d, c, b, a));
    _mm_stream_si128(&xv[0], _mm_set_epi32(a, b, c, d));
    _mm_sfence();
    _mm_lfence();
    _mm_mfence();
    _mm_pause();
    _mm_clflush(xi);
    s = xi[0] + xi[1] * 3 + xi[2] * 5 + xi[3] * 7 + (int)xl[0] + (int)(xl[1] >> 3) + hash(xv[0]);
    return s ^ fh(_mm_load_ps(xf)) ^ dh(_mm_load_pd(xd));
}

/* loadl/loadh/storel/storeh (_pi), and the MXCSR: a converted value follows the rounding mode */
int mm_halves(int a, int b, int c, int d)
{
    float m[4] = {a / 3.0f, b * 0.5f, c * 7.0f, d / 5.0f};
    float o[4] = {0, 0, 0, 0}, p[4] = {1, 2, 3, 4};
    __m128 x = fk(a, b, c, d);
    x = _mm_loadl_pi(x, (const __m64 *)m);
    x = _mm_loadh_pi(x, (const __m64 *)(m + 1));
    _mm_storel_pi((__m64 *)o, x);
    _mm_storeh_pi((__m64 *)p, _mm_loadu_ps(m));
    return fh(x) ^ fh(_mm_loadu_ps(o)) * 3 ^ fh(_mm_loadu_ps(p)) * 5;
}

int mm_csr(int a, int b, int c, int d)
{
    __m128 x = _mm_set_ps(a / 2.0f, b * 0.5f + 0.25f, c * 1.5f, d * 0.75f - 0.5f);
    __m128d u = _mm_set_pd(a * 0.5 + 0.3, d * -0.5 - 0.3);
    unsigned old = _mm_getcsr();
    int s = (int)(old & 0x1f80);
    int mode;
    for (mode = 0; mode < 4; mode++) {
        _mm_setcsr((old & ~0x6000u) | (unsigned)mode << 13);
        s = s * 5 + hash(_mm_cvtps_epi32(x)) + _mm_cvtss_si32(x) + _mm_cvtsd_si32(u) + (int)_mm_cvtsd_si64(u) + hash(_mm_cvtpd_epi32(u));
    }
    _mm_setcsr(old);
    return s + (int)((_mm_getcsr() ^ old) & 0xffff);
}

/* vectors and scalars of the same size cast bit for bit; 8-byte vectors (__m64) */
typedef long long v1di __attribute__((vector_size(8)));

int vec_bits8(int a, int b, int c, int d)
{
    v2si x = {a, b};
    long long l = (long long)x, m = (long long)c << 32 | (unsigned)d;
    v2si y = (v2si)m;
    v1di z = (v1di)l;
    __m128i s = _mm_set_epi64((__m64)m, (__m64)l), t = _mm_loadl_epi64((const __m128i *)&m);
    return (int)(l >> 5) ^ y[0] * 3 ^ y[1] * 5 ^ (int)z[0] ^ hash(s) * 13 ^ hash(t) * 17 ^ hash(_mm_movpi64_epi64((__m64)l));
}

#include <xmmintrin.h>
static int h64(__m64 v)
{
    long long l = (long long)v;
    return (int)l * 31 + (int)(l >> 32);
}

int mm_mmx(int a, int b, int c, int d)
{
    __m64 x = (__m64)(((long long)a << 32) | (unsigned)b), y = (__m64)(((long long)c << 32) | (unsigned)d);
    __m64 z = _mm_set_pi16((short)a, (short)b, (short)c, (short)d);
    __m128 f = _mm_set_ps((a & 1023) / 8.0f - 60, (b & 1023) * 0.5f - 100, (c & 255) * 1.5f - 30, (d & 63) / 4.0f + 0.5f);
    __m128d u = _mm_set_pd((a & 255) * 0.5 - 20, (d & 255) * 0.75 - 30.5);
    int s = h64(_mm_max_pi16(x, y)) ^ h64(_mm_min_pi16(x, y)) * 3 ^ h64(_mm_max_pu8(x, y)) * 5 ^ h64(_mm_min_pu8(x, y)) * 7 ^
            h64(_mm_avg_pu8(x, y)) * 11 ^ h64(_mm_avg_pu16(x, y)) * 13 ^ h64(_mm_mulhi_pu16(x, y)) * 17 ^ h64(_mm_sad_pu8(x, y)) * 19 ^
            _mm_movemask_pi8(x) * 23 ^ _mm_movemask_pi8(y) * 29 ^ h64(_mm_mul_su32(x, y)) * 31;
    s ^= h64(_mm_cvtps_pi32(f)) ^ h64(_mm_cvttps_pi32(f)) * 3 ^ h64(_mm_cvtpd_pi32(u)) * 5 ^ h64(_mm_cvttpd_pi32(u)) * 7 ^
         h64(_mm_cvtps_pi16(f)) * 11 ^ h64(_mm_cvtps_pi8(f)) * 13;
    s ^= fh(_mm_cvtpi32_ps(f, x)) ^ fh(_mm_cvtpi16_ps(z)) * 3 ^ fh(_mm_cvtpu16_ps(z)) * 5 ^ fh(_mm_cvtpi8_ps(x)) * 7 ^
         fh(_mm_cvtpu8_ps(y)) * 11 ^ fh(_mm_cvtpi32x2_ps(x, y)) * 13 ^ dh(_mm_cvtpi32_pd(x)) * 17;
    return s;
}

int mm_maskmove(int a, int b, int c, int d)
{
    char buf[48] = {0}, buf2[48] = {0};
    __m128i v = _mm_set_epi32(a, b, c, d), m = _mm_set_epi32(d, c, a, b);
    static long long sink[2];
    int i, s = 0;
    _mm_maskmoveu_si128(v, m, buf + (a & 7));
    _mm_maskmove_si64((__m64)(((long long)a << 32) | (unsigned)b), (__m64)(((long long)c << 32) | (unsigned)d), buf2 + (b & 15));
    _mm_stream_pi((__m64 *)&sink[a & 1], (__m64)(((long long)c << 32) | (unsigned)d));
    for (i = 0; i < 48; i++)
        s = s * 31 + buf[i] + buf2[i] * 7;
    return s ^ (int)sink[0] ^ (int)(sink[1] >> 3);
}

int sqrt_scalar(int a, int b, int c, int d)       /* __builtin_sqrt[f] is the instruction */
{
    double x = (a & 1023) + 0.5, y = (double)b;
    float f = (float)(c & 1023) + 2.0f, g = (float)d;
    union { double d; long long l; } u = {__builtin_sqrt(y)};
    union { float f; int i; } v = {__builtin_sqrtf(g)};
    return (int)__builtin_sqrt(x * 3) ^ (int)(__builtin_sqrtf(f) * 16) ^ (int)u.l ^ (int)(u.l >> 32) * 3 ^ v.i * 5;
}

int mm_rcp(int a, int b, int c, int d)      /* the CPU's approximations: gcc emits the same instructions */
{
    __m128 x = fk(a, b, c, d), y = _mm_set_ps(a * 0.37f, b / 7.0f, c + 0.5f, d * 1e5f);
    return fh(_mm_rcp_ps(x)) ^ fh(_mm_rcp_ss(x)) * 3 ^ fh(_mm_rsqrt_ps(x)) * 5 ^ fh(_mm_rsqrt_ss(x)) * 7 ^
           fh(_mm_rcp_ps(y)) * 11 ^ fh(_mm_rsqrt_ps(y)) * 13 ^ fh(_mm_rsqrt_ss(y)) * 17;
}

int incdec(int a, int b, int c, int d)      /* ++ and -- on vectors: every lane plus or minus 1 */
{
    v4si x = {a, b, c, d}, y, z;
    v4sf f = {a & 1023, b & 1023, c & 1023, d & 1023};
    y = x++;
    x++;
    ++x;
    z = --x;
    x--;
    f++;
    f++;
    --f;
    return fold4(x) ^ fold4(y) * 3 ^ fold4(z) * 5 ^ (int)(f[0] + f[1] * 3 + f[2] * 5 + f[3] * 7);
}
