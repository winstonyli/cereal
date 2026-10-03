typedef __SIZE_TYPE__ size_t;
extern int c1[(((size_t)-1 >> 1) + 1) / sizeof(int)];
extern int c2[(size_t)-1 / sizeof(int)];
extern int c3[(size_t)-1 / sizeof(int)][4];
extern int c4[100][(size_t)-1 / sizeof(int)];
void f(void){ int x[((size_t)-1 >> 1) / 2][4]; (void)sizeof(int[(size_t)-1/2][4]); }
struct S { int m[(((size_t)-1 >> 1) + 1) / sizeof(int)]; };
typedef int T[(((size_t)-1 >> 1) + 1) / sizeof(int)];
