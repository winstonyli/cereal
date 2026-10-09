// flags: -Wextra
struct A; struct B;
int fegetenv (struct A *);
int feholdexcept (struct B *);
int fesetenv (const struct A *);
int feupdateenv (const struct B *);
int fegetexceptflag (struct A *, int);
int fesetexceptflag (struct B *, int);
int fesetexceptflag (const struct B *, int);
