typedef struct { int *a; char b[64]; } A __attribute__((aligned (64)));
struct B { int x; A d[4]; } b;
struct B2 {  A d[4]; } b2;
struct B3 { A d[4]; int y; int z; } b3;
struct B4 {
 int y; A d[4]; int z; };
struct B { A d[4]; } b;
struct C {
  A d[4];
};
