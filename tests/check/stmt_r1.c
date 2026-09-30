// flags: -Wall -Wextra
void f(char c) {
  switch (c) {
  case 100 ... 300: ;
  case -300 ... 10: ;
  case -400 ... -300: ;
  case 400 ... 500: ;
  case -300 ... 300: ;
  case 0 ... 1: ;
  }
}
