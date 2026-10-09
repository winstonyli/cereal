// flags: -std=c11 -pedantic-errors -Wc99-c11-compat -Wc11-c2x-compat
struct S;
struct U { int x; };
int f2(struct U *a, struct S *b) { return a < (struct U *)b; }
int (*pa)[2];
const int (*pb)[2];
int f3(void) { return pa == pb; }
void g(const int (*q)[2]);
void h(void) { g(pa); }
struct T { int x; } t = {};
enum E { A = 0x100000000LL };
[[deprecated]] int dd;
int *cl = &(static int){1};
void v(...);
_Static_assert(1);
_Static_assert(1, "msg");
enum E2 : int { A2 };
_Decimal32 d32;
double dd2 = 1.0df;
int bin = 0b11;
void k(void) { goto l; l: int y; (void)y; }
void np(int) {}
#warning hi
