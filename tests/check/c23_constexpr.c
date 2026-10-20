// flags: -std=c23
constexpr int a = 1;
constexpr unsigned char uc = 255, uc2 = 256;
constexpr int sh = 1 << 31;
constexpr long long ll = 0x7fffffffffffffffull;
constexpr long long ll2 = 0x8000000000000000ull;
constexpr char ch = 300;
constexpr unsigned u = -1;
constexpr _Bool bb = 2;
constexpr int m = 1.5;
constexpr int n = (int)1.5;
constexpr float fl = 0.1;
constexpr float fl2 = 0.5;
constexpr float fl3 = 0.1f;
constexpr double d1 = 0.1;
constexpr double d2 = 0.1f;
constexpr float f4 = 1.0 / 3;
constexpr float ff = 1e39;
constexpr long double ld = 0.1L;
constexpr int *np = 0;
constexpr int *np2 = &a;
constexpr char *cs = "x";
constexpr int *pp = (int *)4;
int nc = 3;
const int k = 3;
constexpr int bad1 = nc;
constexpr int bad2 = k;
constexpr int bad3 = k + 1;
constexpr int bad4 = (int)(&nc != 0);
constexpr double bad5 = nc;
constexpr int ok1 = a + 2;
constexpr int ok2 = sizeof(int) + 1;
int arr[a + ok1];
int ptr_read(void) { return np == 0; }
_Static_assert(ok1 == 3, "folds");
_Static_assert(sh < 0, "folds");
void f(void)
{
    constexpr int x = 1;
    static constexpr int y = 2;
    register constexpr int z = 3;
    int v[x + y + z];
    int w = x;
    switch (w) { case x: case y: break; }
    x = 2;
    x++;
    --x;
    (void)&x;
    (void)sizeof x;
    extern constexpr int e;
    auto constexpr int aa = 1;
    constexpr auto int bb = 1;
}
extern constexpr int b1 = 2;
constexpr extern int b2 = 2;
typedef constexpr int t1;
constexpr typedef int t2;
_Thread_local constexpr int c1 = 3;
constexpr _Thread_local int c2 = 3;
constexpr constexpr int dd = 4;
constexpr volatile int q1 = 5;
constexpr int *restrict q2 = 0;
constexpr _Atomic int q3 = 6;
constexpr volatile int *q4 = 0;
constexpr int *volatile q5 = 0;
typedef volatile int VI;
constexpr VI q6 = 1;
constexpr const int q7 = 1;
constexpr int h;
constexpr int i1 = 1, j1 = 2;
constexpr int fn(void) { return 1; }
constexpr int fp(void);
void g(constexpr int p);
