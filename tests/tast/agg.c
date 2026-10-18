/* TAST T2: structs, unions, members, by-value copies, calls with aggregates. */
struct P { int x; char c; long l; };
struct Q { struct P p; short s[3]; struct P *next; };
union U { int i; char c[8]; };
struct A { int a; union { int b; char c; }; };
struct P gp;
int getx(struct P *p) { return p->x; }
int getx2(struct P p) { return p.x; }
long getl(struct Q *q) { return q->p.l + q->next->l; }
short gets(struct Q *q, int i) { return q->s[i]; }
void setc(struct P *p) { p->c = 3; p->x += 2; p->l++; }
void copy(struct P *d, struct P *s) { *d = *s; }
struct P ret(struct P *s) { return *s; }
struct P pass(struct P s) { return ret(&s); }
void use(void) { struct P a = gp; struct P b; b = a; gp = b; }
int un(union U *u) { u->c[1] = 1; return u->i; }
int anon(struct A *a) { return a->b + a->c; }
int *memaddr(struct P *p) { return &p->x; }
char arrmem(struct Q *q) { return ((char *)q->s)[1]; }
int local_arr(void) { int a[3]; a[0] = 1; a[1] = a[0] + 1; return a[1]; }
int nested_dot(void) { struct Q q; q.p.x = 1; q.s[2] = 2; return q.p.x + q.s[2]; }
