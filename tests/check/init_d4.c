// flags: -Wall -Wextra
struct S { int x; } s;
int a = s;
int b = (struct S){1};
int c[] = { s };
int d[] = { (struct S){1} };
void f(void) { int e = (struct S){1}; int g[] = { (struct S){1} }; int h = s; }
