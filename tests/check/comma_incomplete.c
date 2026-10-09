// flags: -std=c11 -pedantic-errors
struct S;
extern struct S var;
extern struct S *vp;
void f(void) { var, (void)0; *vp, (void)0; (void)0, var; (void)(var, 1); var; }
