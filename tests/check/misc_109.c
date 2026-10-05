// flags: -std=gnu99 -Wwrite-strings
/* -Wwrite-strings: string literals are arrays of const char. */
typedef char T[1];
typedef const char CT[1];
T *p = &"";
CT *q = &"";
T x; CT cx;
T *p2 = &cx;
CT *q2 = &x;
void f(T *a); void g(CT *a);
void h(void) { f(&cx); g(&x); T *r; r = &cx; }
char *s2 = "";
char (*p3)[1] = &"";
const char (*q3)[1] = &"";
void f5(void){ char *t = "a"; (void)sizeof(""); }
