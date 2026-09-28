/* macro hygiene
   cereal-flags: -Wmacro-unused-param -Wmacro-self-reference -Wmacro-multi-eval-call */
#define SQUARE(x) x * x /* expect: macro-unparenthesized-param, macro-unparenthesized-param, macro-unparenthesized-body */
#define SQUARE_OK(x) ((x) * (x))
#define NEG(x) -x /* expect: macro-unparenthesized-param */
#define FIELD(s) s.field /* expect: macro-unparenthesized-param */
#define MEMBER(s, m) ((s).m)
#define CAST(x) (long)x /* expect: macro-unparenthesized-param */
#define SUM 1 + 2 /* expect: macro-unparenthesized-body */
#define SUM_OK (1 + 2)
#define MINUS_ONE -1
#define CALL(f, x) f(x)
#define TWO_STMTS(a, b) a = 1; b = 2 /* expect: macro-multi-statement */
#define BLOCK(a) { a = 1; } /* expect: macro-multi-statement */
#define SAFE_BLOCK(a) do { a = 1; } while (0)
#define SAFE_SEMI(a) do { a = 1; } while (0); /* expect: macro-trailing-semicolon */
#define CHECK(c) if (!(c)) abort() /* expect: macro-dangling-else */
#define CHECK_OK(c) if (!(c)) abort(); else (void)0
#define TEN 10; /* expect: macro-trailing-semicolon */
#define DECL(n) static int n;
#define __my_reserved 1 /* expect: macro-reserved-name */
#define _Reserved 1 /* expect: macro-reserved-name */
#define _GNU_SOURCE_LIKE_SOURCE 1
#define inline __inline__ /* expect: macro-reserved-name, unused-macros */
#define UNUSED_P(a, b) (a) /* expect: macro-unused-param */
#define SELF SELF /* expect: macro-self-reference */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define FIRST(a, b) (a) /* expect: macro-unused-param */
#define STR(x) #x
#define SIZE(x) sizeof(x)

int f(int);
void use(void)
{
    int i = 0, j = 0, k;
    k = SQUARE(i) + SQUARE_OK(j) + NEG(i) + SUM + SUM_OK + MINUS_ONE + CALL(f, 1) + TEN
    k = MAX(i++, j); /* expect: macro-multi-eval */
    k = MAX(i, j) + MAX(f(i), 2); /* expect: macro-multi-eval-call */
    k = FIRST(i, j++); /* expect: macro-discarded-side-effect */
    k = STR(i++)[0] + SIZE(i++) + UNUSED_P(i, j); /* expect: macro-discarded-side-effect */
    k = MAX(k, FIRST(i, j)) + CAST(i) + SELF;
    TWO_STMTS(i, j); BLOCK(i); SAFE_BLOCK(i); SAFE_SEMI(i) CHECK(i); CHECK_OK(i);
    k = FIELD(k) + MEMBER(k, x) + __my_reserved + _Reserved + _GNU_SOURCE_LIKE_SOURCE;
}
DECL(z)
/* parameters that are types or names, not expressions */
#define NEW(A, T) ((T *)alloc((A), sizeof(T))) /* expect: unused-macros */
#define VEC(T) struct { T *data; int len; } /* expect: unused-macros */
#define FOREACH(arr, n, it) for (int it = 0; it < (n); it++) /* expect: unused-macros, macro-unused-param */
#define DECLARE_PTR(T, name) T *name = 0; /* expect: unused-macros */
#define CAST_TO(T, x) ((T)(x)) /* expect: unused-macros */
#define MUL(a, b) (a * b) /* expect: macro-unparenthesized-param, macro-unparenthesized-param, unused-macros */
#define SCALE(v) (factor * v) /* expect: macro-unparenthesized-param, unused-macros */
#define XLIST(X) X(a) X(b) X(c) /* expect: unused-macros */
#define DECLS(T) struct { T *first; T *last; } /* expect: unused-macros */
