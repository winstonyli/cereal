/* built-in functions must be called; #pragma GCC diagnostic */
typedef void (F)(void);
int g(F *p, ...);
int i;
void t(int n) {
  F *p = __builtin_trap;
  void *q = (__builtin_trap);
  n = -__builtin_trap;
  n = __builtin_trap * 2;
  n = n * __builtin_trap;
  n = __builtin_trap && n;
  n = n || __builtin_trap;
  n += __builtin_trap;
  p = &__builtin_trap;
  p = *__builtin_trap;
  (*__builtin_trap)();
  (__builtin_trap)();
  (void)__builtin_trap;
  __builtin_trap;
  n = (__builtin_trap, 1);
  g(__builtin_trap);
  n = n ? __builtin_trap : 0;
  (void)p; (void)q;
}
long a = _Alignof i;
void u(int n) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
  n = foo(n);
#pragma GCC diagnostic pop
  n = bar(n);
}
