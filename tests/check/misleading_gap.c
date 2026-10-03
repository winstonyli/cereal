// flags: -Wmisleading-indentation
void foo(int);
int a, b;
#define EMPTY
#define engine_ref_debug(X, Y, Z)
void t1(void)
{
  if (a)
    foo (0);
  EMPTY foo (1);
  if (a)
    foo (0);
#if 0
  if (b)
#endif
    foo (1);
  if (a)
    foo (2);
    engine_ref_debug(1, 2, 3)
    foo (3);
  if (a)
    foo (4);
  engine_ref_debug(1, 2, 3)
    foo (5);
  while (a)
    /* blah */;
    foo (6);
  while (a)
    ;
    foo (7);
}
