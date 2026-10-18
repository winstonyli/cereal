int callee(int v);
typedef struct { int len; } S;
#define WRAP(x) callee(x)
#define LEN(p) ((p)->len)
#define TWICE(x) (callee(x) + callee(x))
#define NEST(x) WRAP(x)
int f(S *s) { return WRAP(1) + LEN(s) + TWICE(2) + NEST(3); }
int g(S *s) { return LEN(s); }
