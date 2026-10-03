// flags: -Wall
/* gcc: a redeclared local typedef counts as used; a variable named in
   __builtin_has_attribute counts as used (a function does not). */
static int sf (void);
void f (void)
{
  int v;
  static int ls;
  typedef int T[1];
  typedef int T[1];
  typedef int U[1];
  (void)__builtin_has_attribute (v, aligned);
  (void)__builtin_has_attribute (ls, aligned);
  (void)__builtin_has_attribute (sf, aligned);
}
