// flags: -Wc++-compat
typedef struct { int a; } T;
T g0;
struct { int a; } *p1;
struct { int a; } a1[2];
extern struct { int a; } e1;
const struct { int a; } c1 = {1};
static struct { int a; } s1;
void f(void) { struct { int a; } l1; static struct { int a; } l2; extern struct { int b; } l3; }
enum { Q } en;
typeof(en) en2;
struct { int a; } fn(void);
