// flags: -O2 -Wall
/* -Wstrict-aliasing=3 (-Wall): a dereference of (T *)&object whose alias set
   conflicts with nothing in the object's.  Only at -O2 and above. */
struct S1 { int a; float b; };
struct S2 { int a; float b; };
struct S3 { int a; char c; };
struct G { long a; };
union UU { int i; float f; } uu;
struct S1 s1, sa[3];
struct S3 s3;
int ai[4];
long lg;
void *vp;
typedef int __attribute__((may_alias)) ia;
typedef long __attribute__((may_alias)) la;
struct M { int x; } __attribute__((may_alias));

int d1(float x) { return *(int *)&x; }
int d2(float x) { int *p = (int *)&x; return *p; }       /* not a dereference of the cast */
int d3(float x) { return ((int *)&x)[0]; }
int d4(float x) { return ((int *)&x)[1]; }               /* gcc folds only [0] */
int d5(struct G *a) { return *(int *)&a->a; }
int d6(struct G *a) { return *(unsigned long *)&a->a; }   /* same set */
int d7(void) { return *(float *)&s1.a; }
int d8(void) { return *(long *)&uu.f; }
int d9(void) { return *(int *)&uu; }                      /* the union holds int */
int d10(void) { return *(long *)ai; }                     /* an array decays */
int d11(void) { return *(long *)&sa[1]; }
int d12(void) { return ((struct S2 *)&s1)->a; }
int d13(void) { return (*(struct S2 *)&s1).a; }
int d14(void) { return *(int *)&s1; }                     /* S1 holds int */
int d15(void) { return ((struct S1 *)&s3.a)->a; }         /* int is held by S1 */
int d16(void) { return ((struct S3 *)&s1)->a; }           /* a char member as the target */
int d17(void) { return *(float *)&s3; }                   /* ... and as the object */
int d18(float x) { return *(int *)(void *)&x; }
int d19(float x) { return *(int *)(char *)&x; }
int d20(float x) { return *(char *)&x; }                  /* character types alias all */
int d21(void) { return *(ia *)&lg; }                      /* may_alias */
int d22(void) { return *(la *)&s1; }
int d23(void) { return ((struct M *)&lg)->x; }
int d24(void) { return *(volatile int *)&lg; }
int d25(float *x) { return *(int *)&*x; }                 /* &*x is x */
int d26(float *x) { return *(int *)&x[1]; }
int d27(void) { *(int *)&lg = 1; return 0; }
int d28(void) { return sizeof(*(long *)&s1.a); }
int d29(void) { return *(unsigned *)&lg + (long)*(long **)&vp; }
int d30(void) { return *(int *)&vp; }
