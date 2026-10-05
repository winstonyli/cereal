// flags: -std=gnu99 -Wc++-compat -Wextra -Wno-ignored-qualifiers
#include <limits.h>
typedef __SIZE_TYPE__ size_t;
struct FILE;
struct tm;
struct fenv_t;
struct fexcept_t;
typedef struct FILE FILE;
typedef struct fenv_t fenv_t;
typedef struct fexcept_t fexcept_t;
typedef const int cint;
size_t strftime (char *__restrict, const size_t, const char *__restrict,	/* { dg-bogus "mismatch in argument 1 type of built-in function" } */
                 const struct tm *__restrict) __attribute__((nothrow));
int fprintf (struct FILE *, const char *const, ...);				/* { dg-bogus "mismatch in argument 2 type of built-in function" } */
cint putc (int, struct FILE *);							/* { dg-bogus "mismatch in return type of built-in function" } */
cint fegetenv (fenv_t *);							/* { dg-bogus "mismatch in argument 1 type of built-in function" } */
cint fesetenv (const fenv_t *);							/* { dg-bogus "mismatch in return type of built-in function" } */
int fegetexceptflag (fexcept_t *, const int);					/* { dg-bogus "mismatch in argument 1 type of built-in function" } */
extern const int foo = 42;
extern int bar = 1;
void f(void){ extern int z; }
typedef const int CI;
extern CI a = 1;
extern const int *p = 0;
extern int *const q = 0;
volatile int extern v = 2;
int ca, cb, cc;
void cf(void){
  ca = __builtin_choose_expr (0 * (INT_MAX + 1), cb, cc);
  cb = 1 +
    __builtin_choose_expr (0 * (INT_MAX + 1), cb, cc);
  if (cc) __builtin_choose_expr (0 * (INT_MAX + 1), cb, cc);
}
struct s { enum A a : 8; enum B b : 32; enum C c : 31; };
enum D { d1 }; struct t { enum D d : 1; enum D e: 0+0; };
