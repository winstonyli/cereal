// flags: -Wpacked
struct A1 { int a; int b __attribute__((packed)); };
struct A2 { char a; int b __attribute__((packed)); };
struct A3 { char a; char b __attribute__((packed)); };
struct A4 { int a __attribute__((packed)); };
struct A5 { int a; int b __attribute__((packed, aligned(4))); };
struct A6 { char a; int b __attribute__((packed)); int c __attribute__((packed)); };
struct A7 { int a : 3 __attribute__((packed)); };
struct A8 { char a; short b __attribute__((packed)); short c __attribute__((packed)); };
struct A9 { int a; double b __attribute__((packed)); };
struct B1 { char a[4]; int b __attribute__((packed)); };
union U1 { int a __attribute__((packed)); char b; };
struct B2 { int a; struct { int x; } s __attribute__((packed)); };
struct B3 { int a; int b __attribute__((packed)); } __attribute__((packed));
