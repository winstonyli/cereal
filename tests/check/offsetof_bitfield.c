#include <stddef.h>
struct s { int a : 1; int b; };
typedef struct s T;
int a = offsetof (struct s, a);
int b = __builtin_offsetof (T, a);
int c = offsetof (struct
  s, a);
int d = offsetof (struct { int q : 3; }, q);
int e = offsetof (struct s, b);
union u { int a : 1; };
int z1 = __builtin_offsetof (const struct s, a);
int z2 = __builtin_offsetof (struct s const, a);
int z3 = __builtin_offsetof (union u, a);
int z4 = __builtin_offsetof (struct t { int a : 1; }, a);
int z5 = __builtin_offsetof (union { int a : 1; }, a);
int z6 = __builtin_offsetof (const T, a);
int z7 = __builtin_offsetof (__typeof__ (struct s), a);
int z8 = __builtin_offsetof (struct s
  , a);
int z9 = __builtin_offsetof (T
  , a);
