// flags: -std=gnu11 -Wpedantic -Wall
struct S { char c[3]; };
int *p; void *vp; char *cp; struct S *sp; long long *ll;
_Static_assert (__atomic_always_lock_free (1, p), "1");
_Static_assert (__atomic_always_lock_free (4, p), "2");
_Static_assert (__atomic_always_lock_free (8, ll), "3");
_Static_assert (!__atomic_always_lock_free (4, vp), "4");
_Static_assert (!__atomic_always_lock_free (4, cp), "5");
_Static_assert (__atomic_always_lock_free (1, vp), "6");
_Static_assert (__atomic_always_lock_free (4, 0), "7");
_Static_assert (!__atomic_always_lock_free (16, 0), "8");
_Static_assert (!__atomic_always_lock_free (3, 0), "9");
_Static_assert (!__atomic_always_lock_free (4, sp), "10");
_Static_assert (__atomic_always_lock_free (1, sp), "11");
_Static_assert (__atomic_is_lock_free (4, p), "12");
_Static_assert (__atomic_is_lock_free (16, p), "13");
_Static_assert (__atomic_always_lock_free (4, 8), "14");
_Static_assert (!__atomic_always_lock_free (4, 2), "15");
_Static_assert (1 + __atomic_always_lock_free (1, p), "");
_Static_assert ((__atomic_always_lock_free (1, p)), "");
void g(void) { __atomic_always_lock_free (1, p); __atomic_is_lock_free (16, p); }
