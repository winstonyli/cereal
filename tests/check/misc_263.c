// flags: -Wall -Wpedantic
/* gcc folds a difference of two addresses off the same pointer variable
 * (&sp->a[0] - sp) and __builtin_copysign/fabs of constants, so these array
 * bounds are constants: zero-size arrays under -Wpedantic, and -Warray-parameter
 * (not -Wvla-parameter) for the mismatch. */
typedef long intptr_t;
struct S { int a[1]; };
extern struct S *sp;
void f0 (double[!__builtin_copysign (~2, 3)]);
void f1 (double[!__builtin_copysign (~2, 3)]);
void f1 (double[1]);
void f3 (int[(intptr_t)((char*)sp->a - (char*)sp)]);
void f3 (int[(intptr_t)((char*)&sp->a[0] - (char*)sp)]);
void f3 (int[(intptr_t)((char*)&sp->a[1] - (char*)sp)]);
void f4 (int[(int)__builtin_fabs (-4.0)]);
void f4 (int[4]);
