/* bit-fields: signed and unsigned, straddling bytes, truncation on store, ++, compound assignment, init lists */
struct S {
    int a : 3;
    unsigned b : 5;
    int c : 12;
    unsigned d : 1;
    long long e : 40;
    unsigned long long f : 63;
    _Bool g : 1;
};

struct T { unsigned char x; unsigned y : 4, z : 4; short w; };

int main(void)
{
    struct S s = {0};
    struct S i = {-2, 31, -1000, 1, -5000000000ll, 123456789012345ull, 1};
    struct T t = {7, 9, 6, -3};
    int r = 0;
    s.a = 5;                    /* truncates to -3 */
    s.b = 33;                   /* truncates to 1 */
    s.c = 2047;                 /* truncates to -1 */
    s.d = 3;
    s.e = 0x1234567890ll;
    s.f = ~0ull;
    s.g = 7;
    r += s.a + 10 * s.b + 100 * s.c + 1000 * s.d;
    r += (int)(s.e >> 20) + (int)(s.f >> 40) + s.g;
    r += i.a + i.b + i.c + i.d + (int)(i.e / 1000000) + (int)(i.f % 1000) + i.g;
    s.a++;
    s.a += 5;                   /* -3 + 1 + 5 = 3 */
    ++s.b;
    s.c -= 2000;                /* wraps in the field */
    s.d ^= 1;
    r += s.a + s.b + s.c + s.d;
    r += (s.a = 9);             /* the value of an assignment is the stored field: 1 */
    r += (s.b = 40) * 3;        /* 8 */
    r += t.y * 16 + t.z + t.x + t.w;
    t.y = t.z + t.y;            /* 15 */
    t.z = t.y;
    r += t.y * t.z + t.w;
    return r;
}
