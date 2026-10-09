#define EMPTY
#define F(x,...) #__VA_OPT__(a x __VA_ARGS__ "q" ) | #__VA_OPT__()
#define G(x,...) [#__VA_OPT__(x  ,  __VA_ARGS__)]
F(p) F(p,) F(p,E) F(p,1) G(1) G(2,E)
#define H(x,...) x ## __VA_OPT__(b c) ## d
H(a) H(a,1) H(,1)
#define I(...) __VA_OPT__(x ## y) z __VA_OPT__(__VA_ARGS__ ## 1)
I() I(a) 
#define F(a,...) <a|__VA_OPT__(x ## a, __VA_ARGS__ y)|z>
F(1) F(1,) F(1,2) F(1,EMPTY) F(1,EMPTY EMPTY) F(1,,)
#define G(...) [__VA_OPT__(A B)] [a ## __VA_OPT__(c d)] [__VA_OPT__(c d) ## e]
G() G(1) G(EMPTY)
#define H(x,...) #x __VA_OPT__(#__VA_ARGS__) 
H(a) H(a,b c)
#define I(...) __VA_OPT__(a ## __VA_ARGS__ ## b) end
I() I(m) I(m n)
#define J(x,...) f(x __VA_OPT__(,) __VA_ARGS__)
J(1) J(1,2)
#define K(...) __VA_OPT__( ( __VA_ARGS__ ) )
K(1) K()
#define L(x,...) #__VA_OPT__(x y)
L(1) L(1,2)
#define J(x) x z
#define K(x,...) __VA_OPT__(x) z
#define L(x,...) a __VA_OPT__(x) z
[J()][K(1)][K(1,2)][L(1)]
[J() J()][K(1) K(1)]
#define J2(x,...) f(x __VA_ARGS__)
#define J3(x,...) f(x __VA_OPT__(,) __VA_ARGS__)
J2(1) J3(1) J3(1,2)
#define J4(x,y) f(x y)
#define J5(x,y) f(x y z)
#define J6(x,y) f(x y)g
#define J7(x,y) f(x y )
J4(1,) J5(1,) J6(1,) J7(1,)
