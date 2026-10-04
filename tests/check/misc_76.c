int b;
void fnone(void);
int i;
void t(void)
{
  b = __builtin_has_attribute(fnone, align);
  b = __builtin_has_attribute(fnone, aligned (3));
  b = __builtin_has_attribute(i, alloc_size (1));
  b = __builtin_has_attribute(int, alloc_size (1));
  b = __builtin_has_attribute(1, 2, 3);
  b = __builtin_has_attribute(2, "aligned");
}
