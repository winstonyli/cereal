// flags: -Wall -Wextra
struct P { int x, y; };
struct N { struct P p; int z; union { int a; float b; }; struct { int c; int d; }; };
struct N n1 = { { 1, 2 }, 3, 4, 5, 6 };
struct N n2 = { 1, 2, 3, 4, 5, 6 };
struct N n3 = { .a = 1, .c = 2 };
struct N n4 = { .b = 1, .a = 2 };
struct N n5 = { .p.x = 1, .p = { 2, 3 } };
struct N n6 = { .d = 1, 2 };
struct N n7 = { .p = 1 };
struct N n8 = { 0 };
struct N n9 = { { 0 } };
struct N n10 = { .z = 0, 0, 0 };
int f(int x) {
  int a[3] = { [0] = x, [0] = 2 };
  int b[3] = { x, [0] = 2 };
  int c[3] = { [0 ... 1] = 1, [1] = 2 };
  struct P p = { .x = f(1), .x = 2 };
  struct P q = { .x = 1, .x = f(2) };
  int d[2][2] = { [0] = { 1, 2 }, [0][1] = 3 };
  int e[] = { [1] = 1, [0] = 2, 3 };
  return 0;
}
struct B { unsigned a : 3; int b : 4; int c; };
struct B b1 = { 1, 2, 3 };
struct B b2 = { 9, 9 };
struct B b3 = { 1.5, 2, 3 };
struct B b4 = { .c = 1 };
struct S { int a[2]; struct P p[2]; };
struct S s1 = { 1, 2, 3, 4, 5, 6 };
struct S s2 = { { 1, 2 }, { { 3, 4 }, { 5, 6 } } };
struct S s3 = { .p[1].y = 1, .a[0] = 2 };
struct S s4 = { .p = { 1, 2, 3 } };
struct S s5 = { .a = { 1, 2, 3 } };
struct S s6 = { { 1, 2, 3 } };
struct S s7 = { .a[2] = 1 };
struct S s8 = { .a.x = 1 };
struct S s9 = { .p[0].z = 1 };
