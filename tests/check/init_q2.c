// flags: -Wall -Wextra
int a[4] = { [undecl] = 1, 2 };
int b[4] = { [1.5] = 1 };
int c2[2][2] = { 1, 2, 3, 4, 5 };
int d[2][2] = { {1, 2}, {3, 4}, 5 };
int e[2] = { 1, 2, 3 };
struct S { int x; char y[]; };
struct S s1 = { 1, "abc" };
struct S s2[] = { { 1, "abc" } };
struct S s3 = { 1, {2,3} };
typedef struct { int a; struct { int b; struct S s; } in; } N;
N n = { 1, { 2, { 3, {4,5} } } };
int f(void) { struct S z = { 1, "ab" }; return z.x; }
