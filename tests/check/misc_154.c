// flags: -Wpadded
struct S { __UINT32_TYPE__ i; char c; } __attribute__((aligned(4)));
struct A {
  char a;
  int b;
  char c;
};
struct B {
  char a;
  double d;
  short s;
  struct A *p;
};
union U { char c; int i; };
struct C { int a; char b[3]; };
struct D { char a; struct { char x; long y; } in; };
struct E { char a; int bf : 3; long l; };
struct F { char a; } __attribute__((packed));
void f(void) { struct L { char a; long b; } l; (void)l; }
struct C2 { int a;
  char b[3]; };
struct
C3 { int a; char b[3]; };
struct C4 { int a; char b[3];
};
struct C5 {
  int a;
  char b[3];
}
;
struct { int a; char b[3]; } v6;
typedef struct { int a; char b[3]; } T7;
struct C8 { int a; char b[3]; } v8;
struct C9 { int a; char b[3]; }
  v9;
union UU { long l; char b[9]; };
struct G { int a:3; char c; long l; int z[]; };
struct H { char a; struct { int q; }; };
