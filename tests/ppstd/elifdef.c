#define A
#undef B
#if 0
#elifdef A
int a1;
#elifdef A
int a2;
#endif
#if 0
#elifdef B
int b1;
#elifndef B
int b2;
#else
int b3;
#endif
#if 1
#elifndef A
int c1;
#endif
