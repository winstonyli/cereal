/* Output details that must match GCC. */
#define A , x ## ## x a
A
#define J(a, b) a ## ## ## b
J(1, 2)
#ident "kept"
#define I "expanded"
#ident I
#sccs "as ident"
#pragma pack(1)
#pragma GCC visibility push(default)
#pragma GCC poison consumed_not_printed
#pragma GCC warning "consumed too"
#pragma STDC FP_CONTRACT ON
#define E
_Pragma(E "expanded operand")
#define P(x) _Pragma(x)
P("from a macro") after
#define S(x) x ## #x
#define W(x) L ## #x
S() W(q)
