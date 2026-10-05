#define R __attribute__((scalar_storage_order("big-endian")))
#define L __attribute__((scalar_storage_order("little-endian")))
struct R A { int i; };
struct L B { int i; };
struct N { int i; };
union R UA { int x; struct B b; };
union UB { int x; struct A a; };
union R UC { struct A a; struct N n; int *p; int arr[2]; };
struct R SA { struct N n; struct B b; };
extern void fv(void *);
extern void fa(struct A *);
void t(struct A *a, struct B *b, struct N *n, void *v, int *ip, char *cp)
{
  v = a;
  v = b;
  v = n;
  a = v;
  ip = a;
  cp = a;
  fv(n);
  fa(v);
  fa(b);
  a = n;
  (void)(struct A *)b;
  (void)(void *)a;
  v = (void *)a;
}
struct R S1 { int a[2]; int *p; char c; struct A in; struct A ia[2]; };
void u(struct S1 *s, struct S1 t)
{
  int *q = (int*)s->a;
  q = &s->a[1];
  q = s->p;
  q = &s->in.i;
  q = &t.c;
  struct A (*pa) = &s->in;
  pa = s->ia;
  pa = &s->ia[0];
  int x = s->a[0] + sizeof(s->a);
}
