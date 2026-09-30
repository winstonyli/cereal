// flags: -Wall -Wextra
struct S { int x; };
int d[1] = { (struct S){1} };
int e[1] = { (int[1]){1} };
struct T { int a; } t = { (struct S){1} };
