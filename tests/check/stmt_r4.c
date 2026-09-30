// flags: -Wall -Wextra
void g(void);
void f(int a) {
  if (a) g(); else ;
}
