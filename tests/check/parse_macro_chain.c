#define BAD int x = ;
#define OUTER BAD
#define TWO(a) a OUTER
void f(void) {
  OUTER
}
void g(void) {
  TWO(int y;)
}
