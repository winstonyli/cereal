// flags: -Wall -Wextra
struct S { int x, y, z; };
struct S a =
   { 1,
     2
   }
   ;
struct S b[] = { { 1 },
   { 2,
     3 }
   , { 4 } };
void f(void) {
  struct S c = { 1 },
    d = {
      1
   }; int q;
  struct S e = {1}; struct S g = { 1 };
  int sc = {
     1
  };
  int sd =
     {
       1, 2 };
}
