_Static_assert(_Alignof(undef1) == 0, "a0");
_Static_assert(_Alignof(undef2) == 1, "a1");
_Static_assert(sizeof(undef3) == 0, "s0");
_Static_assert(sizeof(undef4) == 1, "s1");
_Static_assert(sizeof(int) == 1 + undef5, "s2");
_Static_assert(_Alignof(undef6) >= 2, "a2");
