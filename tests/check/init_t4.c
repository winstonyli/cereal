// flags: -Wall -Wextra
int d[] = 5;
void f(void) {
  int e[] = 5;
  int g[] = {1, undecl};
  int h[] = {undecl2, 3};
  static int i[] = 5;
  int j[] = (int[]){1,2};
}
