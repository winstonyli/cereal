// flags: -Wc++-compat
struct two { const int a; int m; const int b; };
struct nest { int x; struct two t; const int z; };
struct arr { const int a[2]; };
typedef const int CI;
CI g1;
struct two g2;
static struct nest g3;
extern struct two g4;
void f(void) {
  struct two v1;
  struct nest v2;
  struct arr v3;
  CI v4;
  const int v5[3];
  static struct two v6;
  const struct two v7;
  extern const int e1;
  register const int r1;
  struct two v8 = { 1 };
}
