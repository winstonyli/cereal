// flags: -Wall
/* -Wmultistatement-macros: gcc compares macro maps.  An argument token
 * belongs to the macro it was substituted into, so a nested macro's body
 * followed by an argument token of the outer macro is two maps (silent),
 * while a body token of M followed by an argument of M is one (warns). */
struct T { int c; };
void foo(void); void bar(void); int c, a;
#define COL(x) (x)->c
#define V(o, t) if (o) COL (o) = 1; t->c = 2;
void f1(struct T *o, struct T *t) { V(o, t) }
#define W(o) if (o) COL (o) = 1; o->c = 2;
void f2(struct T *o) { W(o) }
#define M(x) foo (); x
void g1(void) { if (c) M(bar ()); }
#define N(x) foo (); x;
void g2(void) { if (c) N(a++) }
#define IN(x) foo ()
#define P(x) IN (1) x
void g3(void) { if (c) P(a++); }
#define Q(x) IN (1) ; x
void g4(void) { if (c) Q(a++) }
#define R(x) bar (); IN (x)
void g5(void) { if (c) R(a) ; }
#define S(x, y) IN (x) y ;
void g6(void) { if (c) S(1, bar ()) }
