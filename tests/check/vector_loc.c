typedef int i4 __attribute__((vector_size(16)));
i4 a; float x;
#define ADD(p, q) p + q
#define ID(p) p
#define TWO a + x
void t(void)
{
  ADD(a, x);
  ADD(x, a);
  ID(a + x);
  ID(a) + x;
  TWO;
  a + ID(x);
  (a + ID(x));
  ID(ID(a) + x);
  ADD(a,
      x);
}
i4 r;

void t2(void)
{
  r = a + x;
  r = (a + x);
  r = 1 + (a + x);
  r = ((a) + x);
  r = (a) + x;
  r = ID(a) + x;
  r = a + ID(x);
  r = (a + ID(x));
  r = 1 + (a + ID(x));
  r = 1 + ID(a + x);
  r = ID(1) + (a + x);
  r = ID(1) + (ID(a) + x);
}
