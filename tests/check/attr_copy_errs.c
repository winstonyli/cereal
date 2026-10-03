// flags: -Wall -Wextra
void v1 (void) __attribute__ ((alloc_size (1)));
void v2 (void) __attribute__ ((copy (v1)));
int v3 (void) __attribute__ ((__const));
extern int a;
__attribute__ ((copy (foobar))) void f1 (void);
__attribute__ ((copy ("foobar"))) void f2 (void);
__attribute__ ((copy (123))) void f3 (void);
void *al (int) __attribute__ ((alloc_size (1)));
__attribute__ ((copy ((a, al)))) void f4 (void);
