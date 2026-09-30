// flags: -Wall -Wextra
enum { A = 1 };
void g1(int x) { switch (x) { case A: } }
void g2(int x) { switch (x) { case 1+2: } }
void g3(int x) { switch (x) { case (3): } }
void g4(int x) { switch (x) { case 1 ... 3: } }
void g5(int x) { switch (x) { case 'a': } }
void g6(int x) {
    switch (x) {
    case
      1:
    }
}
void g7(int x) {
    switch (x) {
    case 1: case 2: }
}
void g8(int x) {
    switch (x) {
    default : }
    a
    :
    }
void g9(int x) { l: __attribute__((unused)) }
