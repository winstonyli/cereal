// flags: -Wno-unused -ftrack-macro-expansion=0
#define A(x, v, a) _Static_assert (__builtin_has_attribute (v, a) == x, #v)
int v1;
char v2 __attribute__ ((aligned (4)));
void f (void)
{
  A (0, v1, aligned (0));
  A (0, v2, aligned (0));
  A (1, v2, aligned (4));
  int i8 __attribute__ ((mode (QI)));
  (void)__builtin_has_attribute (i8, mode);
}
