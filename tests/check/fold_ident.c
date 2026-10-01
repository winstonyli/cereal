int n;
enum { A = n * 0 + 1 };
enum { B = 0 * n };
enum { C = 0 & n, D = n - n, E = n * 0 };
struct s { int a : (n * 0 + 1); };
int v[2] = { [(n * 0 + 1)] = 1 };
void f(int x) { switch (x) { case n * 0 + 1: ; } }
int g[n * 0 + 1];
