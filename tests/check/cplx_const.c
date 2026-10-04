// flags: -std=c99 -Wconversion -Wsign-conversion
#include <limits.h>
double d;
_Complex double a = 1.0i;
_Complex double b = 1.0 + 2.0i;
_Complex double c = __builtin_complex(1.0, 2.0);
_Complex double e = -__builtin_complex(1.0, 2.0);
_Complex double f = __builtin_complex(d, 2.0);
_Complex double g = d + 1.0i;
_Complex float h = 3;
_Complex int i = 1 + 2i;
double j = __real__ (1.0 + 2.0i);
int k = (int)__real__ (1.0 + 2.0i);
static _Complex double l = (_Complex double)2.5;
_Complex double m = (1.0 + 2.0i) * (3.0 - 1.0i);
unsigned vui; int vsi; float vf; double vd; unsigned char vuc;
_Complex float vfc; _Complex int vic; _Complex unsigned vuic; _Complex double vdc;
void fn1(void)
{
  vui = __builtin_complex(-1., 1.);
  vui = -1 + 0i;
  vui = -1 + 1i;
  vsi = 3000000000u + 0i;
  vsi = __builtin_complex(0.5, 1.);
  vf = __builtin_complex(16777217., 1.);
  vuc = 300 + 0i;
  vuc = __builtin_complex(300., 0.);
  vdc = __builtin_complex(1.5f, 0.f);
  vfc = __builtin_complex(1.1, 2.2);
  vic = __builtin_complex(1.5, 2.5);
  vuic = -1 + 0i;
  vuic = __builtin_complex(-1.f, 0.f);
  vf = 2 * 1.5i;
  vsi = 1.5i * 1.5i;
  vsi = (1. + 2.i) / (3. - 1.i);
  vsi = -(2.5 + 0i);
  vsi = ~(2.5 + 0i);
  vsi = 5 + 0i;
  vsi = __real__ (2.5 + 1i);
  vf = __imag__ (2.5 + 1i);
  vsi = (int)(2.5 + 0i);
  vdc = (_Complex double)3;
  vsi = (vsi ? 2.5 + 0i : 1.5 + 0i);
}
void fsi(int); void fuc(unsigned char); void fuic(_Complex unsigned);
void fn2(void)
{
  fsi (1.5 + 2.0);
  fsi (1.5 * 3);
  fuc (250 + 50);
  fuc ((250 + 50));
  vsi = 1.5 + 2.0;
  vuc = 250 + 50;
  fsi (0.5 + 0.i);
  fsi ((0.5 + 0.i));
  vsi = 0.5 + 0.i;
  vsi = (0.5 + 0.i);
  fuic (4294967296ull + 1i);
  fuic (0.5 * 2.i);
}
void fn3(void)
{
  __builtin_complex(1.);
  __builtin_complex(1.,2.,3.);
  __builtin_complex(1,2);
  __builtin_complex(1.f,2.);
  __builtin_complex();
  __builtin_complex(1.f, 2.f) = 1;
  &__builtin_complex(1.f, 2.f);
  int x = __builtin_complex(1.f, 2.f);
}
void (*pp)(void) = __builtin_complex;
void fn4(__complex__ int i) { i == 0u; i == ~1u; }
