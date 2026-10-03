// flags: -Wpacked
struct D { int i; int j; } __attribute__((packed));
struct M1 {
  int i;
  int j;
} __attribute__((packed));
struct M2 {
  struct D d;
  int j;
} __attribute__((packed));
struct M3 { struct D d;
  int j; } __attribute__((packed));
struct M4 { struct D d; int j; }
  __attribute__((packed));
struct M5 { char c; } __attribute__((packed))
  x;
struct M6 { char c; struct { char q; } __attribute__((packed)) in; } __attribute__((packed));
struct __attribute__((packed)) M7 { char c; };
struct M8 { int a; } __attribute__((packed, aligned(4)));
