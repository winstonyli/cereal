// flags: -Wall -Wextra
typedef int *ip;
typedef void (*fpt)(int);
typedef const int *cip;
void r1(ip); void r2(fpt); void r3(cip);
void f(double d, float *fl) { r1(d); r1(fl); r2(d); r2(fl); r3(fl); }
