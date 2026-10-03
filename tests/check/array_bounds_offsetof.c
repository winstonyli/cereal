// flags: -Wall
#define vector __attribute__((vector_size(16)))
struct A { int i; char p[1]; };
struct B { struct A a; int i; };
union D { char p[1]; struct B b; };
struct M { char m[3][4]; char t[0]; int z; };
typedef struct M TM;
int f (void)
{
  vector int v;
  int k = 1;
  int a[] = {
    __builtin_offsetof (struct A, p[4]),       /* trailing: ok */
    __builtin_offsetof (struct B, a.p[4]),     /* warns */
    __builtin_offsetof (union D, b.a.p[4]),    /* warns */
    __builtin_offsetof (union D, p[4]),        /* ok */
    __builtin_offsetof (struct M, m[3]),       /* one past: ok */
    __builtin_offsetof (struct M, m[4]),       /* warns */
    __builtin_offsetof (struct M, m[2][4]),    /* ok */
    __builtin_offsetof (TM, m[3][0]),          /* warns, typedef location */
    __builtin_offsetof (struct M, m[1][5]),    /* warns */
    __builtin_offsetof (struct M, m[k][5]),    /* variable index: no check */
    __builtin_offsetof (struct M, t[9]),       /* zero-length: ok */
  };
  return a[0] + v[3] + v[4] + v[-1] + v[k];
}
