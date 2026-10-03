// flags:
struct blah { int n; char a[]; };
extern int ea[];
void f(struct blah *p, struct blah b, int *q) {
  b.a = "hi";
  p->a = "x";
  ea = q;
  ea++;
  b.a++;
  (void)sizeof(b.a);
  b.a += 1;
}
