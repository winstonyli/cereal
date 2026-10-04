// flags: -Wtraditional
struct S { int a; };
union U { int i; long l; };
void f(void)
{
  int a[2]
     = { 1, 2 };
  int
    b[2],
      c[2] = { 1, 2 };
  int d[2] =
      { 1, 2 };
  struct
    S
  x = { 1 };
  struct S
  y
  = { 1 };
  union U u = { 1 };
  union U v =
     { 1 };
  union U
    w = { 0 };
  struct { union U q; int r; } an = { { 1 }, 2 };
}
