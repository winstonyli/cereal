// flags: -Wall -Wextra
enum E { A, B, C };
enum F { X = 1, Y = 2 };
void g(void);
void f1(enum E e, enum F f, int n) {
  switch (e) { case A: g(); break; }
  switch (e) { case A: case B: case C: break; default: break; }
  switch (e) { case A: case 7: break; }
  switch (f) { case X: break; case 5: break; }
  switch (n) { case A: break; }
  switch ((enum E)n) { case A: break; case B: break; }
  switch (e) { }
  switch (n) { }
  switch (n) { default: default: break; }
  switch (n) { case 1: case 1: break; case 2 ... 1: break; }
  switch (1.0) { case 1: break; }
  switch (n == 1) { case 1: case 2: break; }
  switch (!n) { case 1: break; }
  switch ((_Bool)n) { case 1: break; }
  switch (n) { case 1.0: break; case "a": break; case n: break; }
  case 3: break;
  default: break;
  break;
  continue;
}
void f2(int n) {
  for (;;) { break; continue; }
  while (n) { switch (n) { case 1: continue; default: break; } }
  do { break; } while (0);
  if (n) break;
}
int f3(void) { return; }
void f4(void) { return 1; }
void f5(void) { return f4(); }
int f6(int *p) { int x; return &x; }
int *f7(void) { int x; return &x; }
char *f8(void) { char b[4]; return b; }
char *f9(void) { static char b[4]; return b; }
int f10(void) { return "a"; }
int *f11(void) { return 1; }
struct S { int a; };
struct S f12(void) { return 1; }
int f13(struct S s) { return s; }
