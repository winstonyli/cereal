// flags: -std=gnu99 -Wall
/* An old-style definition is checked against a prior prototype (an enum as
 * wide as int does not promote) or against a library built-in (a warning).
 * A built-in with a FILE * parameter takes the first type declared; the
 * "declared in header" note is at the declaration. */
enum e1 {a, b}; enum e2 {c, d};
void f(enum e1);
void f(x) enum e2 x; { }
void g(int, int);
void g(x) int x; { }
void h(int);
void h(x) char x; { }
void i(double);
void i(x) float x; { }
void j(int *);
void j(x) long *x; { }
int k(int);
int k(x, y) int x, y; { return 0; }
char *strchr(a) const char *a; { return 0; }
char *rindex(a, b) register char *a, b; { return 0; }
#pragma GCC diagnostic warning "-Wextra"
int g(int a, unsigned b) { return a < b; }
struct FooFile; struct BarFile;
int fputc (int, struct FooFile*);
int fputs (const char*, struct BarFile*);
