// flags: -Wpacked-not-aligned -Wall
typedef unsigned long long __u64
  __attribute__((aligned(4),warn_if_not_aligned(8)));
struct foo1 { int i1, i2, i3; __u64 x; };
struct foo2 { int i1, i2, i3; __u64 x; } __attribute__((aligned(8)));
struct foo4 { int i1, i2; __u64 x; } __attribute__((aligned(8)));
struct foo5 {
  int i1;
  int x __attribute__((warn_if_not_aligned(16)));
};
union bar1 { int i1; __u64 x; };
struct __attribute__ ((aligned (8))) S8 { char a[8]; };
struct __attribute__ ((packed)) S1 {
  struct S8 s8;
};
struct __attribute__ ((packed, aligned (8))) S3 {
  int i1;
  struct S8 s8;
};
struct bf { __u64 i : 2; };

extern unsigned long strlen (const char *);
const char b[][5] = { "12", "123", "1234", "54321" };
unsigned long f1(int v)
{
  return strlen(b[3]) + strlen(&b[3][1]) + strlen(v ? "" : b[3]) +
         strlen(b[2]) + strlen(v ? b[3] : b[1]);
}
