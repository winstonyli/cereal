struct A { int n; int d[]; };
struct B { int d[]; };
struct C { int n; int d[]; int m; };
union U { int d[]; };
struct D { struct A a; int x; };
int f[];
void g(int n) { int v[n]; static int w[n]; }
