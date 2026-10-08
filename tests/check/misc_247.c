#pragma STDC FLOAT_CONST_DECIMAL64 ON
#pragma STDC FLOAT_CONST_DECIMAL64 BAD
#if L'\U1234abcd' != 1
#endif
typedef int __attribute__((vector_size(12))) V3;
typedef void (*fp)(void);
extern char *bar(void *a1, int a2);
void foo(void)
{
  ((char *(*)(void *, char **))((fp)bar))(0, 0);
}
long double big = 1.18973149535723176508575932662800702e+4932L;
