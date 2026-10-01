void f(int *a, int n)
{
  int i;
#pragma GCC unroll 4
  for (i = 0; ; ++i) a[i] = 0;
#pragma GCC ivdep
  while (n--) a[n] = 1;
#pragma GCC ivdep
  a[0] = 2;
#pragma GCC ivdep
#pragma GCC unroll 2
  for (;;) ;
#pragma GCC ivdep
  do { n++; } while (n < 3);
#pragma GCC ivdep
}
