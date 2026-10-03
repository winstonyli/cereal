extern void bar (int);
void test (void)
{
  #pragma GCC unroll 4+4
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
  #pragma GCC unroll -1
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
  #pragma GCC unroll 20000000000
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
  #pragma GCC unroll  4.2
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
  #pragma GCC unroll (2*3)-7
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
  #pragma GCC unroll 65534
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
  #pragma GCC unroll 65535
  for (unsigned long i = 1; i <= 8; ++i)
    bar(i);
}
