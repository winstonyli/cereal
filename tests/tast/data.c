/* T3b: static data against gcc (tests/tast_data.py): bytes and relocations. */
enum E { A, B = 5, C };
struct S { char n[6]; int v; const char *p; };
struct Bf { int a : 4; unsigned b : 12; signed char c; long long d : 40; };
struct In { struct { int x, y; } pt; union { int i; char c; } u; int arr[2]; };
extern int ext;
int f(void);
int h(int);

signed char sc = -3;
unsigned char uc = 300;
short sh = -1;
unsigned long long ull = 0xfedcba9876543210ull;
_Bool bo = 7;
enum E en = C;
int neg[3] = { -1, ~0, 1 << 4 };
int rng[6] = { [1 ... 3] = 8, [5] = 2 };
char words[3][4] = { "ab", "cde", { 'x' } };
struct S ss[] = { { "hi", 1, "p" }, { .v = 2, .n = "zz" }, [3] = { "q" } };
struct Bf bf = { -2, 4095, -1, -5 };
struct Bf bf2 = { .d = 1099511627775ll, .a = 7 };
struct In in = { { 1, 2 }, { .c = 3 }, { 4 } };
struct In in2 = { .pt.y = 9, .arr[1] = 8 };
int (*fp)(void) = f;
int (*fps[2])(void) = { f, 0 };
int *ip = &ext;
int *ip2 = &in.arr[1];
char *cp = (char *)&in + 4;
const char *str[] = { "one", "two", 0 };
const char *mid = "abcdef" + 2;
int *nul = 0;
unsigned long ul = sizeof(struct In) * 2;
static int priv = 12;
int *pp = &priv;
static const char names[][3] = { "a", "bb" };
const char (*pn)[3] = &names[1];

void g(void)
{
    static int a = -1;
    static char b[5] = "xy";
    static struct S s = { "k", 4, "kk" };
    static int *p = &a;
    static int (*h2)(int) = h;
    static int c[3];
    (void)a; (void)b; (void)s; (void)p; (void)h2; (void)c;
}
