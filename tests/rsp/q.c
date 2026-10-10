#define S(x) #x
#define XS(x) S(x)
#ifdef A
const char *a = XS(A);
#endif
#ifdef B
const char *b = XS(B);
#endif
