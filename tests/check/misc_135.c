// flags: -O2 -Wconversion
void foo();
void foo() { void foo(struct S); }
void h() {}
void h(struct S);
static const int ZERO = 0;
int *a;
long *c;
int b1[(int)(1.0 + 2.0)];
struct { int x : (int)(1.0 + 2.0); } s;
void f(void) {
  c = (1 ? a : (void *)(__SIZE_TYPE__)(ZERO + 0));
  c = (1 ? a : (void *)(__SIZE_TYPE__)0);
# 40 "sys.h" 3
  char ch = 300;
# 45 "misc_135.c"
  ch = 300;
}
