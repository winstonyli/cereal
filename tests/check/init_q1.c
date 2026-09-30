// flags: -Wall -Wextra
int a = 1;
static int b[] = {a};
struct S { int x; struct { int y; } in; };
struct S s = { 1, { a } };
static int *p = (int[]){1,2};
struct T { int *q; };
struct T t = { (int[]){1,2} };
void f(void){ int l; static int *pp = &l; static struct S z[] = { { 1, {2} }, {3}, }; 
 struct T t2 = { (int[]){1,2} }; static struct T t3 = { (int[]){1,2} }; }
