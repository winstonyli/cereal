// flags: -std=gnu99
#include <stdarg.h>
void foo(int s, ...) { va_list ap; int e; e = va_arg (ap, int (void)) (); int a[2]; (void)va_arg(ap, int[2]); (void)va_arg(ap, void); (void)va_arg(ap, struct S); }
int a;
void *a[] = {};
int b[] = {};
struct S {int q;} c;
int c[] = {};
int d;
int d[] = {1,2};
void f(int c){ struct s { int x[c]; struct { int z; } nest; } v = { 1, 2 }; int a[c] = {1,2}; int b[c] = {}; struct t { int n; int y[c]; } w = {1,2,3}; }
extern void xxx (int) __attribute__((noreturn));
typedef void voidfn (int);
__volatile extern voidfn xxx;
extern void yyy (int);
extern void __attribute__((noreturn)) yyy (int);
void __attribute__((noreturn)) zzz (int);
typedef void vfn(int);
volatile vfn zzz;
void f2 (void)
{
   void *p;
L:
   p = &&L;
   *p;
   *&&L;
}
