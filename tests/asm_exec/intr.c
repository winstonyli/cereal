/* Built-ins for tests/asm_exec.py: expect, infinity, libc calls, atomics. */
int slot = 5;
long long wide = 1000;
short narrow = 7;

int likely(int k)
{
    if (__builtin_expect(k > 3, 1))
        return k * 2;
    return -k;
}

double huge(int k)
{
    double h = __builtin_huge_val();
    return k > 0 ? h : -1.0;
}

float hugef(int k)
{
    return k > 0 ? __builtin_inff() : 2.0f;
}

int fill(int k)
{
    char b[8];
    int i, s = 0;
    __builtin_memset(b, k, 8);
    for (i = 0; i < 8; i++)
        s += b[i];
    return s + (int)__builtin_strlen("hello");
}

int copy(int k)
{
    char a[6], b[6];
    int i;
    for (i = 0; i < 6; i++)
        a[i] = (char)(k + i);
    __builtin_memcpy(b, a, 6);
    return b[5] + __builtin_memcmp(a, b, 6) + __builtin_abs(k);
}

int atoms(int k)
{
    int old, r, v = k;
    old = __atomic_fetch_add(&slot, k, 5);
    __atomic_store(&slot, &v, 5);
    __atomic_load(&slot, &r, 5);
    __atomic_exchange(&slot, &old, &v, 5);
    return old + r + v + slot;
}

long long atoms8(int k)
{
    long long v = k, r;
    long long old = __atomic_fetch_sub(&wide, k, 5);
    __atomic_load(&wide, &r, 5);
    __atomic_store(&wide, &v, 5);
    return old + r + wide;
}

int atoms2(int k)
{
    short v = (short)k, r;
    __atomic_store(&narrow, &v, 5);
    __atomic_load(&narrow, &r, 5);
    return r + __atomic_fetch_add(&narrow, 3, 5) + narrow;
}

int cas(int k)
{
    long long want = (k & 1) ? wide : wide + 1, to = k * 3LL, got;
    int ok1 = __atomic_compare_exchange(&wide, &want, &to, 0, 5, 5);
    short sw = (k & 2) ? narrow : (short)(narrow + 1);
    int ok2 = __atomic_compare_exchange_n(&narrow, &sw, (short)k, 0, 5, 5);
    __atomic_load(&wide, &got, 5);
    return ok1 * 1000 + ok2 * 100 + (int)(want & 0xff) + (int)(sw & 0xff) + (int)(got & 0xf) + narrow;
}

int relax(int k)
{
    __asm__ __volatile__ ("rep; nop" ::: "memory");
    __asm__ __volatile__ ("" ::: "memory");
    __asm__ volatile ("pause");
    return k + 1;
}

/* bit built-ins (zero is undefined for clz and ctz: callers pass nonzero) */
unsigned bs32(unsigned x) { return __builtin_bswap32(x); }
unsigned long long bs64(unsigned long long x) { return __builtin_bswap64(x); }
unsigned bs16(unsigned short x) { return __builtin_bswap16(x); }
int lz32(unsigned x) { return __builtin_clz(x | 1); }
int lz64(unsigned long long x) { return __builtin_clzll(x | 1) + __builtin_clzl(x | 2); }
int tz32(unsigned x) { return __builtin_ctz(x | 0x80000000u); }
int tz64(unsigned long long x) { return __builtin_ctzll(x | (1ull << 63)) + __builtin_ctzl(x | (1ull << 62)); }

/* asm with operands: cpuid (leaf 0 gives the vendor's largest leaf; its value is
 * the same in both builds), and an empty guard */
unsigned cpu0(int k)
{
    unsigned n = 7, a = 0, c = 0, d = 0;
    __asm__("cpuid" : "=a"(n) : "a"(0) : "ebx", "ecx", "edx");
    __asm__("cpuid" : "=a"(a), "=c"(c), "=d"(d) : "a"(1) : "ebx");
    __asm__("" : "+r"(k));
    return (n > 0) + (a != 0) * 2 + ((c ^ d) != 0) * 4 + (unsigned)k;
}

int fall(int k)
{
    int r = 0;
    switch ((unsigned)k % 3) {
    case 0: r += 1; [[fallthrough]];
    case 1: r += 2; __attribute__((fallthrough));
    case 2: r += 4; break;
    default: __builtin_unreachable();
    }
    return r;
}

unsigned cpu7(int k)
{
    unsigned a = 1, b = 2, c = 3;
    __asm__("cpuid" : "=a"(a), "=b"(b), "=c"(c) : "a"(0), "c"(0) : "edx");
    return (a > 0) + (b != 0) * 2 + (unsigned)k;
}

/* register operands: a conditional move through a template, as zstd's ZSTD_selectAddr */
const char *selpick(unsigned index, unsigned low, const char *cand, const char *back)
{
    __asm__("cmp %1, %2\n"
            "cmova %3, %0\n"
            : "+r"(cand)
            : "r"(index), "r"(low), "r"(back));
    return cand;
}

long addmix(long x, long y)
{
    long r;
    __asm__("mov %1, %0; add %2, %0; sub $3, %0" : "=r"(r) : "r"(x), "r"(y));
    return r;
}

int selp(unsigned index, unsigned low) { return selpick(index, low, "aaaa", "bbbb")[0] + (int)addmix(index, (long)low); }

int pf(const int *p, int k)
{
    __builtin_prefetch(p + k);
    __builtin_prefetch(p, 0, 3);
    return p[k & 1] + k;
}
