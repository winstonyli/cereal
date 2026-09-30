typedef unsigned long size_t;
struct S { int a; char b; };
struct P;
enum E { E0, E1 = 5 };
typedef struct { long x; } T;
int g = 3;
extern int h;
static int sq(int x) { return x * x; }
int f(int a, struct S *s) { return sq(a) + s->a + E1 + g + (int)sizeof(struct S) + (int)sizeof(T); }
struct P { struct S s; };
int h;
int k(void) { return undefined_fn(1); }
