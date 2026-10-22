/* Built-ins evaluated in place (src/c/eval.c ev_intrin, cintrin.h bi_int): the bit scans,
 * byte swaps, overflow arithmetic, atomics on one thread, and the rest of the evaluated rows. */
static unsigned long long vals[] = {
    1, 2, 3, 0x80, 0xff, 0x1234, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL, 0x123456789abcdefULL,
    0x8000000000000000ULL, 0xffffffffffffffffULL, 0xfffffffffffffff0ULL, 0x0000ffff00000000ULL};

static unsigned long long h;
static void mix(unsigned long long x) { h = h * 1000003ULL + x; }

static void bits(void)
{
    unsigned i;
    for (i = 0; i < sizeof vals / sizeof *vals; i++) {
        unsigned long long x = vals[i];
        unsigned u = (unsigned)x;
        unsigned short s = (unsigned short)x;
        if (u) {
            mix(__builtin_clz(u));
            mix(__builtin_ctz(u));
        }
        mix(__builtin_clzll(x));
        mix(__builtin_ctzll(x));
        mix(__builtin_clzl(x));
        mix(__builtin_ffs((int)u));
        mix(__builtin_ffsll((long long)x));
        mix(__builtin_clrsb((int)u));
        mix(__builtin_clrsbll((long long)x));
        mix(__builtin_popcount(u));
        mix(__builtin_popcountll(x));
        mix(__builtin_parity(u));
        mix(__builtin_parityl(x));
        mix(__builtin_bswap16(s));
        mix(__builtin_bswap32(u));
        mix(__builtin_bswap64(x));
    }
    mix(__builtin_ffs(0));
    mix(__builtin_clrsb(0));
    mix(__builtin_clrsb(-1));
    mix(__builtin_clrsbll(0x4000000000000000LL));
}

static void overflow(void)
{
    int i, ir;
    unsigned u, ur;
    long long l, lr;
    unsigned long long ul, ulr;
    signed char c;
    int ia[] = {0, 1, -1, 2147483647, -2147483647 - 1, 65536, -65536, 46341};
    unsigned ua[] = {0, 1, 4294967295u, 65536, 2147483648u};
    for (i = 0; i < 8; i++) {
        int j;
        for (j = 0; j < 8; j++) {
            mix(__builtin_add_overflow(ia[i], ia[j], &ir)); mix(ir);
            mix(__builtin_sub_overflow(ia[i], ia[j], &ir)); mix(ir);
            mix(__builtin_mul_overflow(ia[i], ia[j], &ir)); mix(ir);
            mix(__builtin_add_overflow(ia[i], ia[j], &c)); mix(c);
            mix(__builtin_mul_overflow(ia[i], ia[j], &ur)); mix(ur);
            mix(__builtin_mul_overflow(ia[i], ia[j], &lr)); mix(lr);
        }
    }
    for (i = 0; i < 5; i++) {
        int j;
        for (j = 0; j < 5; j++) {
            mix(__builtin_uadd_overflow(ua[i], ua[j], &ur)); mix(ur);
            mix(__builtin_usub_overflow(ua[i], ua[j], &ur)); mix(ur);
            mix(__builtin_umul_overflow(ua[i], ua[j], &ur)); mix(ur);
            mix(__builtin_umulll_overflow(ua[i], ua[j], &ulr)); mix(ulr);
            mix(__builtin_smul_overflow((int)ua[i], (int)ua[j], &ir)); mix(ir);
        }
    }
    l = 0x7fffffffffffffffLL;
    mix(__builtin_add_overflow(l, 1LL, &lr)); mix(lr);
    mix(__builtin_sub_overflow(-l, 2LL, &lr)); mix(lr);
    mix(__builtin_mul_overflow(l, 2LL, &lr)); mix(lr);
    ul = 0xffffffffffffffffULL;
    mix(__builtin_add_overflow(ul, 1ULL, &ulr)); mix(ulr);
    mix(__builtin_sub_overflow(0ULL, 1ULL, &ulr)); mix(ulr);
    mix(__builtin_mul_overflow(ul, 2ULL, &ulr)); mix(ulr);
    mix(__builtin_mul_overflow(1ULL << 32, 1ULL << 31, &ulr)); mix(ulr);
    u = 5;
    mix(__builtin_add_overflow(u, 250, &c)); mix(c);
}

static void atomics(void)
{
    int v = 10, e, old;
    unsigned char b = 250;
    long long w = 1LL << 40;
    mix(__atomic_fetch_add(&v, 3, 5)); mix(v);
    mix(__atomic_add_fetch(&v, 3, 5)); mix(v);
    mix(__atomic_fetch_sub(&v, 20, 5)); mix(v);
    mix(__atomic_fetch_and(&v, 0xf, 5)); mix(v);
    mix(__atomic_or_fetch(&v, 0x30, 5)); mix(v);
    mix(__atomic_fetch_xor(&v, 0xff, 5)); mix(v);
    mix(__atomic_nand_fetch(&v, 0x0f, 5)); mix(v);
    mix(__atomic_exchange_n(&v, 77, 5)); mix(v);
    mix(__atomic_load_n(&v, 5));
    __atomic_store_n(&v, 5, 5); mix(v);
    e = 5;
    mix(__atomic_compare_exchange_n(&v, &e, 9, 0, 5, 5)); mix(v); mix(e);
    e = 5;
    mix(__atomic_compare_exchange_n(&v, &e, 11, 0, 5, 5)); mix(v); mix(e);
    mix(__sync_val_compare_and_swap(&v, 9, 12)); mix(v);
    mix(__sync_bool_compare_and_swap(&v, 1, 2)); mix(v);
    mix(__sync_fetch_and_add(&v, 1)); mix(__sync_add_and_fetch(&v, 1)); mix(v);
    mix(__sync_lock_test_and_set(&v, 3)); mix(v);
    __sync_lock_release(&v); mix(v);
    mix(__atomic_fetch_add(&b, 10, 5)); mix(b);
    mix(__atomic_sub_fetch(&b, 10, 5)); mix(b);
    mix(__atomic_fetch_add(&w, 1, 5)); mix((unsigned long long)w);
    e = 40;
    __atomic_load(&e, &old, 5); mix(old);
    __atomic_store(&e, &v, 5); mix(e);
    old = 123;
    v = 99;
    __atomic_exchange(&e, &old, &v, 5); mix(e); mix(v);
    e = 1;
    old = 2;
    v = 3;
    mix(__atomic_compare_exchange(&e, &old, &v, 0, 5, 5)); mix(e); mix(old);
}

static void others(void)
{
    volatile int k = 7;
    mix(__builtin_expect(k, 7));
    mix(__builtin_expect(k > 5, 1));
    mix(__builtin_inf() > 1e308);
    mix(__builtin_inff() > 1e38f);
    mix((unsigned long long)__builtin_sqrt(1521.0));
    mix((unsigned long long)(__builtin_sqrtf(2.25f) * 100.0f));
    __builtin_prefetch(&k);
    __builtin_ia32_pause();
    __builtin_ia32_mfence();
    mix(__builtin_strlen("hello"));
    mix(__builtin_memcmp("abc", "abd", 3) < 0);
    mix(__builtin_abs((int)-k));
    mix(__builtin_labs(-1234567890123L));
}

int main(void)
{
    bits();
    overflow();
    atomics();
    others();
    return (int)(h & 0x7fffffff);
}
