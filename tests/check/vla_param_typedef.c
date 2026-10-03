// flags: -Wall -Wvla-parameter
extern int m, n;
void local(void)
{
  typedef int IAm[m];
  void f_IAm (IAm);
  void f_IAm (int[m]);
  void f_IAm (IAm);
  void f_iam (int[m]);
  void f_iam (IAm);
}
void foo (int, int[*]);
foo (int x, int y)
{
  return (x >= 0) != (y < 0);
}
