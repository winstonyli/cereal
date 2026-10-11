struct S { int x; };
int f(struct S *p, struct S *q) { return p.x + q.x; }
