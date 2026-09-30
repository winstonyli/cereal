// flags: -Wall -Wextra
struct A { int a; struct { int x; int y; }; union { int u; char c; }; int z; };
struct A s1 = { 1, { 2 } };
struct A s2 = { .a = 1, .z = 2 };
struct A s3 = { 1, 2, 3, 4, 5 };
struct B { int a; struct { int x, y; } in; };
struct B b = { 1, { 2 } };
int e[3] = {};
struct A s4 = { .x = 1, .u = 2, .c = 3 };
