// flags: -std=gnu99
// __builtin_FILE/FUNCTION/LINE are constants; equal address constants subtract
// to a (pedantic) constant; __builtin_constant_p is a constant at -O0;
// __float128 builtins are typed; copy(packed) packs the field.
const char *const file = __builtin_FILE ();
int g;
enum E {
  e0 = __FILE__ - __FILE__,
  e1 = __builtin_FILE () - __builtin_FILE (),
  e2 = __FUNCTION__ - __FUNCTION__,
  e3 = __builtin_FUNCTION () - __builtin_FUNCTION (),
  e4 = "a" - "b"
};
int c1[__builtin_constant_p (__builtin_FILE ()) ? 1 : -1];
int c2[__builtin_constant_p ("abc") ? 1 : -1];
int c3[__builtin_constant_p (g) ? -1 : 1];
#line 20
int l1[__builtin_LINE () == 20 ? 1 : -1];
int l2[__builtin_LINE () == 20 ? 1 : -1];
struct S { unsigned bf: __builtin_LINE (); };
_Static_assert (_Generic (__builtin_fabsq (0), __float128: 1, default: 0), "q");
_Static_assert (_Generic (__builtin_infq (), double: 1, default: 0), "q2");
struct A { char c; } __attribute__ ((packed)) av;
struct C { char c; __attribute__ ((copy (av))) int i; };
_Static_assert (__builtin_offsetof (struct C, i) == 1, "copy packed");
