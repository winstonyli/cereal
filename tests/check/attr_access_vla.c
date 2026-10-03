// flags: -Wall
#define RW(...) __attribute__ ((access (read_write, __VA_ARGS__)))

void f1 (int n, int[n], int);
RW (2, 1) void f1 (int n, int[n], int);
RW (2, 3) void f1 (int n, int[n], int);

          void f2 (int, int[*], int);
RW (2)    void f2 (int, int[*], int);
RW (2, 3) void f2 (int, int[*], int);

       void f7 (int n, int[n]);
RW (2) void f7 (int n, int[n]);

          void f9 (int, char[]);
RW (2)    void f9 (int n, char a[n])
{ (void)&n; (void)&a; }

typedef void G1 (int n, int[n], int);
G1 g1;
RW (2, 3) void g1 (int n, int[n], int);

RW (2, 1) void f11 (int n, char a[*], int m);
RW (2, 3) void f11 (int n, char a[*], int m);
