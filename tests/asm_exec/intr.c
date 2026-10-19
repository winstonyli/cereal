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
