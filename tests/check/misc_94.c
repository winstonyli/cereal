#define R __attribute__((scalar_storage_order("big-endian")))
struct R S { char c[3]; signed char sc[2]; unsigned char uc[2]; _Bool b[2]; short s[2]; long long ll[2]; float f[2]; char cc[2][2]; int v __attribute__((vector_size(16))); int *p; _Complex int zz[2]; int e[0]; };
enum E { X };
struct R U { enum E en[2]; char *sp[2]; struct { char x; } an[2]; };
void *f(struct S *s, struct U *u)
{
  void *v;
  v = s->sc;
  v = s->uc;
  v = s->b;
  v = s->s;
  v = s->ll;
  v = s->f;
  v = s->cc;
  v = s->zz;
  v = s->e;
  v = &s->v;
  v = &s->p;
  v = u->en;
  v = u->sp;
  v = u->an;
  return v;
}
