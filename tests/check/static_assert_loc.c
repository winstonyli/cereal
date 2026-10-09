// flags: -std=gnu11
int f(void); int x;
_Static_assert (f (), "");
_Static_assert ((f ()), "");
_Static_assert ((f () + 1), "");
_Static_assert ((x), "");
_Static_assert (((x)), "");
_Static_assert ((x ? 1 : 2), "");
_Static_assert (f () ? 1 : 2, "");
_Static_assert ((f ()) + 1, "");
