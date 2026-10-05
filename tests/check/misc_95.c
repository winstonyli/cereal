#define R __attribute__((scalar_storage_order("big-endian")))
struct R A { int i; };
struct N { int i; };
union R U1 { struct N n[2]; };
union U2 { struct A a[2]; };
union R U3 { struct { int q; } an; struct N;  };
struct R S4 { union { struct N n; int i; }; };
union U5 { struct A a; struct N n; };
