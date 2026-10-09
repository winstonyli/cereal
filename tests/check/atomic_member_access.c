// flags: -std=c11 -pedantic-errors
struct S { int x; };
union U { int x; };
int f1(_Atomic struct S p) { int e = 0 && p.x; return p.x + e; }
int f2(_Atomic struct S *p) { return p->x; }
void f3(_Atomic union U p, _Atomic union U *q, int x) { p.x = x; q->x = x; }
