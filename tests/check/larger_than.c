// flags: -Wlarger-than=64kB
int g[131072];
static int s[131072];
extern int e[131072];
int tent[131072];
int tent[131072];
struct S { int m[131072]; };
struct S gs;
struct S ret(void) { struct S x; return x; }
void f(struct S p) {}
void h(void) { int loc[131072]; static int st[131072]; struct S ls; }
typedef int T[131072];
T tt;
int *pp = 0;
int small[100];
char c[65536];
char c2[65537];
