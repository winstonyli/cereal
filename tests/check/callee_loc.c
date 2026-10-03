// flags:
struct A {};
void f(void) {
  (struct A){}();
  ({ int i; i; })();
}
