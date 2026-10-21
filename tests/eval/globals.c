/* globals and block-scope statics: initializers with addresses, tentative definitions, state across calls */
struct P { int x, y; const char *name; };
struct B { unsigned a : 3, b : 9; int c; };

int g = 5;
int tent;
int tent;
static int sg = -7;
int arr[5] = {1, 2, 3};
const char msg[] = "hello";
const char *pm = "world";
struct P pt = {3, 4, "pt"};
struct P pts[2] = {{1, 2, "a"}, {5, 6, "b"}};
int *pg = &g;
int *pa = &arr[2];
int **ppg = &pg;
struct P *pp = &pts[1];
struct B bf = {5, 300, -2};
double dg = 2.5;
float fg = 0.25f;
long long big = 0x123456789abcdefll;
int *self = (int *)&self;
static int sq(int x) { return x * x; }
static int cube(int x) { return x * x * x; }
int (*fns[2])(int) = {sq, cube};
int later_user(void);
int late = 40;
int dual = 11;
int dual;
int dual2;
int dual2 = 12;

static int counter(void)
{
    static int n;
    static int k = 10;
    n += 1;
    k += n;
    return n * 100 + k;
}

static int other(void)
{
    static int n = 1000;      /* a different object from counter's n */
    return ++n;
}

int main(void)
{
    int r = 0, i;
    r += g + tent + sg + arr[0] + arr[2] + arr[4];
    tent = 9;
    g += 2;
    r += tent + *pg + *pa + **ppg + msg[1] + pm[4] + pt.x * pt.y + pts[1].y + pp->name[0] + pts[0].name[0];
    r += bf.a + bf.b + bf.c;
    bf.b = 7;
    r += bf.b;
    r += (int)(dg * 4) + (int)(fg * 8) + (int)(big >> 40);
    r += (int *)self == (int *)&self;
    for (i = 0; i < 4; i++)
        r += counter() + other();
    r += fns[0](5) + fns[1](3);
    r += later_user() + dual + dual2;
    return r;
}

int later_user(void) { return late + sizeof arr / sizeof arr[0]; }
