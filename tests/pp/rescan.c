/* hide-set corner cases */
#define f(a) a*g
#define g(a) f(a)
f(2)(9)
#define AA BB
#define BB AA
AA BB
#define lparen (
#define F(x) [x]
F lparen 1)
#define EMPTY
#define G(x) x
G(EMPTY)G(1) a EMPTY b
#define NIL(x) x
#define GG(x) NIL(x) GG
GG(1)(2)(3)
#define obj(x) x
#define call obj
call(call)(4)
#define foo() bar
foo()baz
#define cat(a,b) a##b
cat(1,e)+2 cat(-,>) cat(<,<=) cat(%:,%:) cat(L,'a') cat(L, "s")
#define s2(x) #x
s2( a   "b\n"   'c'  \\ ) s2() s2(   leading) s2(@)
#define VA(fmt, ...) printf(fmt, ## __VA_ARGS__)
VA("a") VA("a", 1, 2) VA("a",)
#define PM 1
#pragma push_macro("PM")
#undef PM
#define PM 2
PM
#pragma pop_macro("PM")
PM
#undef PM
#pragma pop_macro("PM")
