// flags: -std=gnu99
// __builtin_shufflevector
typedef int v4si __attribute__((vector_size(16)));
typedef int v2si __attribute__((vector_size(8)));
typedef int v8si __attribute__((vector_size(32)));
typedef const int cv4 __attribute__((vector_size(16)));
v4si a, b; v2si c; v8si d; cv4 cc; int i; float fl; struct S{int x;} st;
void f(void){
  _Static_assert(_Generic(__builtin_shufflevector(a, b, 0, 1), v2si: 1, default: 0), "r1");
  _Static_assert(_Generic(__builtin_shufflevector(a, b, 0, 1, 2, 3, 4, 5, 6, 7), v8si: 1, default: 0), "r2");
  __builtin_shufflevector(a, c, 0, 1);
  __builtin_shufflevector(a, d, 0, 1);
  __builtin_shufflevector(a, b, -1, 1);
  __builtin_shufflevector(a, b, i, 1);
  __builtin_shufflevector(a, b, 1.0, 1);
  __builtin_shufflevector(a, b);
  __builtin_shufflevector(a, b, 1);
  __builtin_shufflevector(a, cc, 1, 2);
  __builtin_shufflevector(a);
  __builtin_shufflevector();
  __builtin_shufflevector(a, b, 0, 1, 2);
  __builtin_shufflevector(i, i, 0, 1);
  __builtin_shufflevector(st, st, 0, 1);
  int x = __builtin_shufflevector(a, b, 0, 1);
  __builtin_shufflevector(a, b, 7, 8);
  __builtin_shufflevector(a, b, 1 + 1, 2 * 3);
  __builtin_shufflevector(a, b, (char)1, 2UL);
}
void mixed(void) {
  __builtin_shufflevector(c, a, 5);     /* 2 + 4 elements */
  __builtin_shufflevector(c, a, 6);
  __builtin_shufflevector(a, c, 5);
  __builtin_shufflevector(a, c, 6);
}
