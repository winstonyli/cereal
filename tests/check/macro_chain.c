// flags: -Wmultistatement-macros
#define SWAP(X, Y) tmp = X; X = Y; Y = tmp
#define M1 if (x) SWAP (x, y)
#define M2() M1
int x, y, tmp;
void f(void) {
  M1;
  M2();
}
