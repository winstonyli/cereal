#undef __DATE__
#undef __LINE__
#undef __STDC__
#undef __COUNTER__
#if 2:
#endif
#if (1 : 2)
#endif
#error don't
#pragma foo 'x
#define a!
#define b"x
#define c '
#define d(x) x "y
#define e 1.0e ## -1
int z = e;
e
