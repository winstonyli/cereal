/* signed shifts and compares, argument order, whole-struct copies */
struct S { int a, b, c; long d; };
static int nx(int *p) { return ++*p; }
static int sub(int a, int b) { return a * 10 - b; }
static int cmpall(int a, int b, unsigned ua, unsigned ub)
{
    return (a < b) + 2 * (a > b) + 4 * (a <= b) + 8 * (a >= b) + 16 * (ua < ub) + 32 * (ua >= ub) + 64 * (a == b);
}
static struct S mk(int k) { struct S s = { k, k + 1, k + 2, 1000L * k }; return s; }
static long total(struct S s) { return s.a + s.b + s.c + s.d; }
int main(void)
{
    int c = 0, r = 0;
    struct S x = mk(3), y;
    y = x;
    r += -64 >> 3;
    long big = -1L;
    int sh = 40;
    r += (int)(big >> sh);
    r += (int)(0x80000000u >> 31);
    r += cmpall(-1, 1, 4000000000u, 5u) + cmpall(5, 2, 1u, 1u) + cmpall(7, 7, 0u, 9u);
    r += sub(nx(&c), nx(&c));
    r += (int)total(y) + (int)total(mk(9)) + y.c;
    return r;
}
