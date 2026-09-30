// flags: -Wall -Wextra
struct B { int a : 3; unsigned b : 2; } s;
void f(void) {
  switch (s.b) { case -1: ; }
  switch (s.a) { case 9: ; }
}
