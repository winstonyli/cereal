// flags: -Wrestrict
int foo (char *__restrict buf, const char *__restrict fmt, ...);
int bar (char *__restrict a, char *__restrict b, char *c, char *d);
void f(char *p, char *q)
{
  foo (p, "%s", p, p);
  foo (p, "%s", 1, p);
  bar (p, q, p, q);
  bar (p, p, p, q);
  bar (q, p, q, p);
}
