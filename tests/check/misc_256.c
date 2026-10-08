// flags: -Wall
union GC { int b; };
union GC *p; union GC gg;
void rm(void);
#define ID(t) t
#define AND(t) ((t) && 1)
#define A2(t) (t)
#define AM(x) (&(x)->b)
#define FIRST(t) { if ((t) && 1) rm(); }
void c1(void) { if (ID(&p->b) && 1) rm(); }
void c2(void) { if (1 && ID(&p->b)) rm(); }
void c3(void) { if (AND(&p->b)) rm(); }
void c4(void) { if (A2(AM(p))) rm(); }
void c5(void) { if (ID(AM(p))) rm(); }
void c6(void) { FIRST(AM(p)); }
void c7(void) { if (!A2(&p->b)) rm(); }
void c8(void) { int x = ID(&p->b) ? 1 : 2; (void)x; }
void c9(void) { if (1 && A2(&p->b)) rm(); }
void d1(void) { if (ID((&p->b))) rm(); }
void d2(void) { if (ID(&p->b) || 0) rm(); }
void d3(void) { if (ID(ID(&p->b))) rm(); }
