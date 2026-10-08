// flags: -Wall -Wextra
/* An erroneous subscript operand: gcc's build_array_ref returns before it
 * marks a plain identifier operand read, so the parameter is "set but not
 * used".  Compound operands, other operators and the other parameters are
 * read as usual. */
struct S { int a; };
void f1(struct S *p, int i) { p->nope[i] = 1; }
int f2(struct S *p, int i) { return p->nope + i; }
int f4(struct S *p, int i, int j) { return p->nope[i][j]; }
int f6(struct S *p, int i) { return i[p->nope]; }
int f7(struct S *p, int i, int k) { return p->nope[i + k]; }
int f8(struct S *p, int i) { return undefd[i]; }
int f9(struct S *p, int i) { return -p->nope[(i)]; }
