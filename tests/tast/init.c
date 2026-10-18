/* T3b: initializers.  Static objects print as data units, automatic ones as
 * local nodes with elem kids. */
struct P { int a; char b; short c[2]; };
struct B { unsigned x : 3, y : 5; int z; };
union U { int i; char c[4]; };
struct N { struct P p; int q[3]; };
typedef struct { const char *s; int n; } Ent;

int g0 = 5;
int g1[4] = { 1, 2 };
int g2[] = { 1, 2, 3 };
struct P g3 = { 1, 'x', { 2, 3 } };
struct P g4 = { .c = { [1] = 9 }, .a = 7 };
struct B g5 = { 1, 2, 3 };
union U g6 = { .c = { 1, 2 } };
struct N g7 = { { 1 }, { [2] = 4 } };
char g8[] = "hello";
char g9[8] = "abc";
const char *g10 = "hi";
int *g11 = &g0;
int *g12 = &g1[2];
long g13 = (long)&g0;
Ent g14[] = { { "a", 1 }, { "bc", 2 } };
struct P g15 = { 1, 2, 3, 4 };
int g17[3] = { [0] = 1, [0] = 2, 3 };
struct P g18 = { .a = 1, .a = 2 };
static int g19;

void f(int k)
{
    int a[3] = { 1, k };
    struct P p = { k, 'y' };
    struct P q = { .c[1] = k, .a = 3 };
    char s[] = "xyz";
    char t[2] = "ab";
    struct B b = { 1, k };
    union U u = { k };
    int m[2][2] = { { 1 }, { k, 2 } };
    static int sa = 3;
    static int sb[2] = { 1 };
    static const char *sc = "ss";
    extern int ext;
    static int sn;
    int z = { 4 };
    ext = sa + sb[0] + sn + z;
}
