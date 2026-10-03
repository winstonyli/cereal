// flags: -O2 -Wstrict-aliasing=2
/* -Wstrict-aliasing=2: the cast itself, "will" when the sets are disjoint,
   "might" when one holds the other. */
struct S1 { int a; float b; };
struct S2 { int a; float b; };
struct INC;
union UU { int i; float f; } uu;
struct S1 s1;
int ai[4];
long lg;
_Complex double x;
void *vp;
typedef int __attribute__((may_alias)) ia;

double *c1(void) { return (double *)ai; }
double *c2(void) { return (double *)&ai[0]; }
int *c3(void) { return (int *)&__imag x; }
void *c4(void) { return (long *)&s1; }
void *c5(void) { return (int *)&s1; }                     /* held: might */
void *c6(void) { return (struct S1 *)&ai[1]; }
void *c7(void) { return (struct S2 *)&s1; }
void *c8(void) { return (int *)&uu; }
void *c9(void) { return (char *)&lg; }
void *c10(void) { return (void *)&lg; }
void *c11(void) { return (struct INC *)&lg; }             /* incomplete target */
void *c12(void) { return (ia *)&lg; }
void *c13(void) { return (long *)(void *)&s1.b; }
void *c14(void) { return (long *)(char *)&s1.b; }
void *c15(int *p) { return (long *)p; }                   /* not an address */
void *c16(int *p) { return (long *)&p[1]; }
void *c17(int *p) { return (long *)&*p; }
void *c18(struct S1 *p) { return (long *)&p->b; }
void *c19(void) { return (float *)&lg; }
void *c20(void) { return (_Complex float *)&lg; }
void *c21(void) { return (void **)&vp; }
