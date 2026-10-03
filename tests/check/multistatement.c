// flags: -Wmultistatement-macros
#define SWAP(X, Y) tmp = X; X = Y; Y = tmp
#define STUFF2 if (0) x = y; x++
#define STUFF3 if (x) SWAP(x, y)
#define SET(X, Y) (X) = (Y)
#define STUFF4 if (x) SET(x, y); SET(x, y)
#define FOO0 if (1) { } else
#define M(N) L ## N: x++; x++
int x, y, tmp;
void f(void)
{
  if (x) SWAP(x, y);
  while (x) SWAP(x, y);
  for (;x;) SWAP(x, y);
  if (x) ; else SWAP(x, y);
  switch (x) case 1: SWAP(x, y);
  if (x) M(0);
  if (x) { SWAP(x, y); }
  STUFF2;
  STUFF3;
  STUFF4;
  { FOO0 { } }
  if (x) x++;;
}
