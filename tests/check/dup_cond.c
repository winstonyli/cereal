// flags: -Wduplicated-cond
int foo(void);
struct S { int *p; };
int f1(int n, struct S *s, int i, int j)
{
  if (n == 1) return 1;
  else if (n == 2) return 2;
  else if ((n == 1)) return 3;
  else if (foo()) return 7;
  else if (foo()) return 8;
  else if (n) { if (i) return 1; else if (i) return 2; }
  else if (!s->p) return 4;
  else if (!s->p) return 4;
  else if (i > 0 && j > 0 && i) return 1;
  else
  if (i > 0 && j > 0 && i) return 9;
  else if ((long)i) return 1;
  else if ((long)i) return 1;
  return 0;
}
int f2(int n)
{
  if (4) return 1;
  else if (4) return 2;
  if (n) return 1;
  if (n) return 2;
  return 0;
}
