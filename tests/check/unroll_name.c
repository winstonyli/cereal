const int k = 4;
enum { E = 3 };
int f(void);
int j;
void t(void)
{
#pragma GCC unroll k
 for (int i = 0; i < 8; i++) f();
#pragma GCC unroll E
 for (int i = 0; i < 8; i++) f();
#pragma GCC unroll f
 for (int i = 0; i < 8; i++) f();
#pragma GCC unroll E + 1
 for (int i = 0; i < 8; i++) f();
#pragma GCC unroll k + 1
 for (int i = 0; i < 8; i++) f();
#pragma GCC unroll j
 for (int i = 0; i < 8; i++) f();
}
