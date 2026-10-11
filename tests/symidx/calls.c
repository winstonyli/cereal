/* CIX_CALL (docs/B4_DESIGN.md 4): the flag, the dedupe, and xcalls. */
#define CALL_LEAF(v) leaf(v, 0)
#define OUTER() CALL_LEAF(1)
#define PASTE(g) g##_impl(1)
#define ID(x) x
#define M(f) f(1); g_impl(1)
#define DEFINE_FN(name) int name(void) { return leaf(5, 6); }

int leaf(int a, int b);
int g_impl(int x);
int knr();
int plain(int n) { return n; }
struct ops { int (*cb)(int); };
int (*fp)(int, int) = leaf;
int init = sizeof(leaf(1, 2));
int a1 = sizeof(leaf(3, 4)), b1 = sizeof(g_impl(5)) + sizeof(leaf(5, 6));
_Static_assert(sizeof(leaf(1, 2)) == 4, "");
DEFINE_FN(made)

int knrdef(a, b) int a, b; { return a + b; }

int user(struct ops *o, int (*param)(int))
{
    int t = leaf(1, 2);               /* f(x) */
    t += (leaf)(3, 4);                /* (f)(x) */
    t += (*fp)(5, 6);                 /* (*fp)(x): not flagged */
    t += (&plain)(7);                 /* (&f)(x): not flagged */
    t += fp(8, 9);                    /* object callee */
    t += param(10);                   /* parameter callee */
    t += o->cb(11);                   /* field callee */
    t += ID(plain(12));               /* callee in a macro argument */
    t += PASTE(g);                    /* name formed by ## */
    t += CALL_LEAF(2);                /* callee spelled in a #define body */
    t += OUTER();                     /* nested expansion */
    t += ID(CALL_LEAF(2));            /* inside a macro argument */
    t += __builtin_abs(t);            /* no symbol */
    t += undeclared_fn(9);            /* implicit declaration */
    t += ({ int u = plain(1); u; });  /* statement expression */
    t += knr(1) + knrdef(1, 2);       /* old-style declarations */
    t += user(o, param);              /* recursion */
    M(plain);                         /* two events at one spelling */
    return t;
}

int local_define(void)
{
#define IN_BODY() leaf(7, 8)
    return IN_BODY() + IN_BODY();
}

int init2 = sizeof(made());
