// flags: -pedantic-errors
#define a!
#define foo(X			/* eol */
#define foo2(X,)
#define foo3(, X)
#define foo4(X, X)
#define foo5(X Y)
#define foo6(..., X)
#define foo7(X ...) X
#define goo(__VA_ARGS__) 1
#define one(x, ...) x
int a1 = one(1);
