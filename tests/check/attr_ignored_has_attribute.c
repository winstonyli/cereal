// flags: -Wall
/* An attribute the declaration ignores (inapplicable to its kind) is not in its set, so
 * __builtin_has_attribute is false for it, as in gcc; a kept one stays. */
int v1 __attribute__((pure));
int v2 __attribute__((noreturn));
int v3 __attribute__((used));
void f(void) __attribute__((used, common));
_Static_assert(__builtin_has_attribute(v1, pure) == 0, "v1");
_Static_assert(__builtin_has_attribute(v2, noreturn) == 0, "v2");
_Static_assert(__builtin_has_attribute(v3, used) == 1, "v3");
_Static_assert(__builtin_has_attribute(f, common) == 0, "f");
_Static_assert(__builtin_has_attribute(f, used) == 1, "f used");
int g1 __attribute__((aligned(8), packed));
_Static_assert(__builtin_has_attribute(g1, aligned) == 1, "g1");
int n1(int) __attribute__((aligned(8)));
int n2(int) __attribute__((packed, copy(n1)));
