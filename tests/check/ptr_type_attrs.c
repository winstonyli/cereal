// flags: -Wall
#define A(expr) do { int a[1 - 2 * !(expr)]; (void)&a; } while (0)
struct S
{
  int* __attribute__ ((aligned (16))) paligned;
  int* __attribute__ ((packed)) ppacked;
  int* __attribute__ ((aligned (16), packed)) qaligned;
  int* __attribute__ ((packed, aligned (16))) qpacked;
} s;
void test (void)
{
  A (__alignof__ (s.paligned) == 16);
  A (__alignof__ (s.ppacked) < 16);
  A (__alignof__ (s.qaligned) == 16);
  A (__alignof__ (s.qpacked) == __alignof__ (s.paligned));
}
#define Assert(expr)   typedef char AssertExpr[2 * !!(expr) - 1]
struct __attribute__((packed)) PackedA { int i; char c; };
Assert (__alignof (struct PackedA) == 1);
struct __attribute__ ((copy ((struct PackedA*)0))) PackedB { long i; char c; };
Assert (__alignof (struct PackedA) == __alignof (struct PackedB));
struct Unpacked { int i; char c; };
Assert (__alignof (struct Unpacked) > 1);
