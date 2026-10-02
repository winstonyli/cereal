// flags: -std=gnu99 -Wall
typedef union { int *i; long *l; } U2 __attribute__((transparent_union));
extern void f2 (U2);
extern void f2 (int *);
extern void h (int *);
extern void h (U2);
long l;
void g(void){ f2(&l); h(&l); }
void k(int *p, long *q);
void k2(void){ k(__extension__ (char *) 0, 0); }
