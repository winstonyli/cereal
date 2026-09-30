struct S {
  int a;
  int a;
  void v;
  int f(void);
  struct S self;
  struct Inc inc;
  int arr[];
  int after;
};
struct Flex { int n; int d[]; };
struct OnlyFlex { int d[]; };
struct Z { };
union Flex2 { int d[]; int e; };
struct Nested { struct Flex f; int g; };
struct Neg { int n; char c[-1]; };
struct Fn { int (*fp)(void); int x(int); };
