/* File-scope compound literals are anonymous static objects (gcc: __compound_literal.N). */
struct N { int u; };
struct T { long a; struct N n; int v[2]; };
struct P { int *p; struct T *t; };
int *sp = (int[]){ 1, 2 };
struct T *tp = &(struct T){ .a = 7, .n = { 3 }, .v = { 8, 9 } };
struct P pp = { (int[]){ 5, 6 }, &(struct T){ .a = 1 } };
char *cs = (char[]){ "hi" };
int *off = (int[]){ 1, 2, 3 } + 1;
int *in = &(int[]){ 4, 5 }[1];
/* a literal taken as a value in a static initializer: gcc folds its elements in */
struct A { long a; };
struct B { struct A a; int k; };
struct A sav = (struct A){ 3 };
struct B bq = { (struct A){ 2 }, 3 };
struct B bo = { (struct A){ 2 }, 3, .a = { 7 } };
struct B bp = { (struct A){ 2 }, 3, .a.a = 8 };
struct B bv[2] = { (struct B){ { 1 }, 2 }, [0].k = 5, (struct B){ { 3 }, 4 } };
int scl(void) { static struct A s = (struct A){ 5 }; return s.a; }
