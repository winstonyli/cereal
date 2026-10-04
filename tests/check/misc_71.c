// flags: -Wall
extern unsigned long strlen (const char *);
struct A { char a[5], b[5]; };
const struct A s = { "1234", "12345" };
struct B { struct A a[2]; };
const struct B ba[] = {
  { { { "123", "12345" }, { "12345", "123" } } },
  { { { "1", "12" },      { "123", "1234" } } }
};
int v0;
unsigned long f (void)
{
  unsigned long n = 0;
  n += strlen (s.a);
  n += strlen (s.b);
  n += strlen (&s.b[1] + v0);
  n += strlen (&s.a[1]);
  n += strlen (ba[0].a[0].b);
  n += strlen (ba[0].a[1].a);
  n += strlen (ba[1].a[1].b);
  n += strlen (&ba[0].a[1].a[1]);
  return n;
}
