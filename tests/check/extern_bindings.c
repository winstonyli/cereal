// Block-scope extern declarations see the composite of the visible type only.
typedef int IA[];
typedef int IA5[5];
typedef IA *IAP;
typedef IA5 *IA5P;
extern IAP a[];
void f(void)
{
  {
    extern IA5P a[];
    sizeof(*a[0]);
  }
  extern IAP a[];
  extern IAP a[5];
  sizeof(*a[0]);
}

int b[1] = { 0 };
void g(void)
{
  int b;
  {
    extern int b[];
    sizeof(b);
  }
}

int x[];
void h(void)
{
  extern int x[2];
}
int y[];
void k(void)
{
  extern int y[1];
}
int z[];
void m(void)
{
  extern int z[3];
}
int z[3];
