// flags: -std=c11 -pedantic
struct S;
struct U { int x; };
int f2(struct U *a, struct S *b) { return a < (struct U *)b; }
struct T { int x; } t = {};
_Static_assert(1);
_Static_assert(1, "msg");
