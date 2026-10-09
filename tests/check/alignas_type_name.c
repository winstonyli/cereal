// flags: -std=c11 -pedantic-errors
void f (void)
{
  _Generic (1, int: 1, _Alignas (8) long: 2);
  sizeof (_Alignas (8) long);
  sizeof (int _Alignas (int));
  _Alignof (_Alignas (8) long);
  (_Alignas (8) long) 0;
  _Atomic (_Alignas (8) long) x;
  _Alignas (_Alignas (8) long) long y;
  __typeof (long double _Alignas (0)) e;
  __builtin_types_compatible_p (signed _Alignas (0), unsigned);
}
