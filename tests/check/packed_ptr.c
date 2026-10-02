struct B { int i; };
struct P { char c; int i; char a[8]; struct B b; } __attribute__ ((packed));
struct N { char c; int i; };
struct Q { char c; int i __attribute__ ((packed)); int j; };
struct P gp, *pp, ga[2];
extern struct P *g (void);
void use (long *, int *, char *, void *);
long *f1 (struct P *p)
{
  int *a = &p->i;
  char *b = &p->c;
  void *v = &p->i;
  int *d = p->a;
  int *e = &p->a[1];
  int *f = &p->b.i;
  int *h = (int *) &gp;
  long *k = (long *) g ();
  int *m = &ga[1].i;
  int *n = &(*p).i;
  struct N nn, *np = &nn;
  int *o = &np->i;
  int *q = &((struct Q *) 0)->j;
  int *r = &((struct Q *) 0)->i;
  use (&p->i, &p->i, &p->c, p);
  use ((void *) 0, pp ? &p->i : &pp->i, 0, 0);
  return p;
}
