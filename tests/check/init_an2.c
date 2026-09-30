// flags: -Wall -Wextra
struct A {
  int a;
  union tag_x_y
  {
 int u; } ;
  int z; };
struct A s = {1};
struct C { int a; struct { int q; } ; };
struct C c={1};
