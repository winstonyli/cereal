// flags: -Wall -Wvla-parameter
extern int m, n;
void f(void)
{
  typedef int IAm[m];
  typedef int IAn[n];
  void* g_IAm (IAm);
  void* g_IAm (int[n]);
  void* g_iam (int[m]);
  void* g_iam (IAn);
}
void f2(void)
{
  void* g_iam (int[m]);
  void* g_iam (int[n]);
}
