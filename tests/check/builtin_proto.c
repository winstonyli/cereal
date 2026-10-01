_Atomic int a; int b; unsigned u;
double d; float f; int n; char *s;
void t(void)
{
  d = __builtin_pow(d, 2);
  f = __builtin_powf(f, f);
  n = __builtin_abs(d);
  __builtin_memcpy(s, s, 3);
  __builtin_strlen(n);
  __builtin_pow(1);
  __builtin_printf("%d", 1);
  n = __builtin_pow;
  __typeof(__builtin_pow) *pp;
  d = __builtin_fabs(f) + __builtin_sqrt(2);
}
void f2(void) {
  __builtin_sadd_overflow(1, 2, &a);
  __builtin_sadd_overflow(1, 2, &u);
  __builtin_uadd_overflow(1, 2, &b);
  __builtin_saddl_overflow(1, 2, &b);
  __builtin_sadd_overflow(1, 2, 0);
  __builtin_sadd_overflow(1, 2);
  __builtin_ssubll_overflow(1.5, b, &b);
  b = __builtin_umul_overflow(1, 2, &u);
}
