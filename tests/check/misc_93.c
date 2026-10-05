#define R __attribute__((scalar_storage_order("big-endian")))
struct R A { int i; };
struct R S { int *pa[2]; char c[3]; struct A ia[2]; _Complex float z; double d; };
typedef int I2[2];
struct R T { I2 t; I2 tt[2]; };
void *f(struct S *s, struct T *t)
{
  void *v;
  v = s->pa;
  v = &s->pa[0];
  v = s->c;
  v = &s->c;
  v = s->ia;
  v = &s->z;
  v = &s->d;
  v = t->t;
  v = t->tt;
  v = &t->tt[1];
  v = t->tt[1];
  v = &t->t[1];
  v = &((s)->d);
  v = &(s->c)[0];
  (s->c);
  s->c, 1;
  v = (1, s->c);
  v = 1 ? s->c : s->c;
  return s->c + 1;
}
