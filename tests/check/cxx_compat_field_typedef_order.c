// flags: -std=c11 -Wc++-compat -Wpadded
/* -Wc++-compat "using 'T' as both field and typedef name" comes last in
 * finish_struct (warn_cxx_compat_finish_struct): after duplicate members
 * (whose names are gone by then), -Wpadded and the transparent_union check;
 * erroneous members still count; only typedefs used in the body itself. */
typedef int T;
struct A { int a; int a; T x; int T; };
struct B { T x; int T; int b; int b; };
struct C { int c; T y; struct D { int T; T z; int d; int d; } in; int c; };
struct E { int T; int e; int e; };
struct F { T x; int T; int T; };
struct G { T f; int T; char g; long h; };
union __attribute__((transparent_union)) U { T T; long l; char cc[9]; };
struct K { T x; int T[]; int y; };
union L { T x; int T[]; };
struct N { T x; int T : 0; };
struct O { int T; struct P { T z; } p; int o; int o; };
struct S { T a; struct { int T; int u; int u; }; int v; int v; };
struct M { };
