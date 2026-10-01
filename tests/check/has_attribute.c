// flags: -Wall
/* __builtin_has_attribute and the attribute names that copy carries */
#define ATTR(...) __attribute__ ((__VA_ARGS__))
#define YES(e, a) extern char chk[__builtin_has_attribute (e, a) ? 1 : -1]
#define NO(e, a) extern char chk[__builtin_has_attribute (e, a) ? -1 : 1]

ATTR (const, nothrow) int fc (int);
ATTR (copy (fc)) int fcc (int);
YES (fc, const); YES (fc, __nothrow__); YES (fcc, const); YES (fcc, nothrow);
NO (fc, pure);

ATTR (noinline, deprecated, pure) int fd (void);
ATTR (copy (fd)) int fdc (void);
YES (fd, noinline);
typedef int Tfd[__builtin_has_attribute (fd, deprecated) ? 1 : -1];
NO (fdc, noinline);
YES (fdc, pure);

_Noreturn void fnr (void);
ATTR (copy (fnr)) void fnrc (void);
YES (fnr, noreturn); YES (fnrc, noreturn);

ATTR (aligned (8)) int va;
ATTR (copy (va)) int vb;
YES (va, aligned); YES (vb, aligned); NO (vb, packed);

struct ATTR (packed) P { char c; int i; };
struct S { char c; int i ATTR (aligned (8)); };
YES (struct P, packed);
NO (struct S, packed);
extern struct P p;
extern struct S *ps;
YES (p, packed);
YES (ps->i, aligned);
NO (ps->c, aligned);
YES (int ATTR (aligned), aligned);
NO (int, aligned);

struct C {
  ATTR (copy (*ps)) int a;
  ATTR (copy (p)) int b;
  ATTR (copy ((struct P *) 0)) int d;
  ATTR (copy (ps->i)) int e;
};
NO (((struct C *) 0)->a, packed);
YES (((struct C *) 0)->b, packed);
YES (((struct C *) 0)->d, packed);
YES (((struct C *) 0)->e, aligned);
_Static_assert (__alignof__ (((struct S *) 0)->i) == 8, "member alignof");
_Static_assert (__alignof__ (((struct P *) 0)->i) == 1, "packed alignof");

ATTR (leaf) int fl (void);
static ATTR (leaf) int fsl (void);
YES (fl, leaf); NO (fsl, leaf);

static int fn1 (void) { __builtin_abort (); }
static int fn2 (void) { __builtin_unreachable (); }
