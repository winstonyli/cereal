// flags: -std=c11 -pedantic-errors
typedef void F(int);
typedef void F(int);
typedef int IA[];
typedef int A2[2];
typedef IA A2;
void f(void)
{
  int a = 1, b = 2;
  typedef void FN(int (*p)[a]);
  typedef void FN(int (*p)[b]);
  typedef void FN(int (*p)[*]);
  typedef void FN(int (*p)[1]);
  typedef void FN2(int (*p)[a]);
  typedef void FN2(int (*p)[]);
  typedef int AV[a];
  typedef int AV[b - 1];
}
