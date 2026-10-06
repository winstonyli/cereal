struct S { int a; char b[__INTPTR_MAX__]; };
struct T { char b[__INTPTR_MAX__]; };
struct U { char b[__INTPTR_MAX__]; char c; };
char x[__INTPTR_MAX__];
void foo(void) { struct S s; struct T t; char y[__INTPTR_MAX__]; struct U u; }
