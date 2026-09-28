/* GCC behavior at the edges of argument collection. */
#define L __LINE__
#define B(x) x
#define D(x) B(x)
#define S(x) #x x ## 1 x
B(
L

)
B(
__LINE__
)
D(
L)
S(
L)
#define C B(L)
C
/* a macro redefined inside its own arguments stays disabled by name */
#define R(a) (a) * R(a)
R(
#define R )
x ) after )
/* a #line inside arguments does not move tokens read before it */
#line 230
B(L
#line 219
L) L
/* outermost macro object-like: __LINE__ is its call site's line */
#define OBJ , B(
OBJ
__LINE__ )
