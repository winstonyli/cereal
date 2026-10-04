// flags: -Wtraditional
struct S { int a; };
struct S *gp = &(struct S){ 1 };
int *gq = (int[2]){ 1, 2 };
void f(void)
{
  struct S *p = &(struct S){ 1 };
  int *q = (int[2]){ 1, 2 };
  int r = (int){ 3 };
  static struct S *sp = &(struct S){ 1 };
  struct S s;
  s = (struct S){ 1 };
}
