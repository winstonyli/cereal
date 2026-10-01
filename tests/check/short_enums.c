// flags: -fshort-enums
enum E { A, B };
enum F { C = 300 };
int a[sizeof(enum E) == 1 ? 1 : -1];
int b[sizeof(enum F) == 2 ? 1 : -1];
enum G { N = -1, M = 1 };
int c[sizeof(enum G) == 1 ? 1 : -1];
enum H { X = 70000 };
int d[sizeof(enum H) == 4 ? 1 : -1];
