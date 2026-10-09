/* declarations: tentative and real definitions, block-scope extern,
 * prototypes and definitions with parameters, K&R, typedef shadowing,
 * implicit declaration */
int t;
int t;
int d;
int d = 1;
extern int e;
int f(int a, int b);
int f(int a, int b) { extern int e; return a + b + e + t; }
int kr(x, y) int x; { return x + y; }
typedef int T;
int g(void) { T T = 1; return T; }
int h(void) { return later(1); }
int later(int v) { return v; }
