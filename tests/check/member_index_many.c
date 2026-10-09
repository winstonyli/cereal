// flags: -std=c11 -Wc++-compat
/* Member lookup in records past the member-name index threshold: by access
 * and by designator, through anonymous members (nested, const, one of
 * thousands of members), a member named both directly and in an anonymous
 * member (the first wins), misses with a hint; and -Wc++-compat over
 * thousands of typedef-typed members: a field named like a typedef, and a
 * typedef used while such a field is open, also from a nested record. */
typedef int T;
#define D(p) T p##0, p##1, p##2, p##3, p##4, p##5, p##6, p##7, p##8, p##9;
#define C(p) D(p##0) D(p##1) D(p##2) D(p##3) D(p##4) D(p##5) D(p##6) D(p##7) D(p##8) D(p##9)
#define M(p) C(p##0) C(p##1) C(p##2) C(p##3) C(p##4) C(p##5) C(p##6) C(p##7) C(p##8) C(p##9)
#define K(p) M(p##0) M(p##1) M(p##2) M(p##3)
struct Small { int T; T u; };
struct Small2 { int v; T w; };
struct Big {
  K(f)
  struct { int a1; union { int a2; long a3; }; };
  const struct { int q; };
  int a6;
  struct { long a6; int a7; };
  struct { long a8; };
  int a8;
  struct { K(g) };
  int T;
  struct Inner { T i1; int i2; } in;
  T after;
};
struct Again { int x; T y; };
struct Big b = { .f0000 = 1, .f3999 = 2, .a1 = 3, .a3 = (void *)0, .g2000 = 4,
                 .a7 = 5, .in.i2 = 6, .zz = 7 };
_Static_assert(sizeof b.a6 == sizeof(int), "a6: the named member");
_Static_assert(sizeof b.a8 == sizeof(long), "a8: the anonymous one");
_Static_assert(__builtin_offsetof(struct Big, a2) ==
               __builtin_offsetof(struct Big, a3), "a2, a3: one union");
_Static_assert(__builtin_offsetof(struct Big, g3999) >
               __builtin_offsetof(struct Big, a8), "g3999 after a8");
int use(struct Big *p)
{
    p->q = 1;
    return p->f0000 + p->f3999 + p->a2 + (int)p->a3 + p->g0000 + p->in.i1 +
           p->nope + p->f40000;
}
