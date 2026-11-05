// flags: -Wall
/* docs/ATTR_IDS_DESIGN.md golden (d): an attribute dropped by an exclusion goes into Checker.ign, and
 * aset_add must not record it: has_attribute sees only the survivor.  The differing alloc_size
 * redeclaration is also pinned as it behaves today: both are recorded (ign is reset before the
 * set is built), so has_attribute finds either. */
#include <stddef.h>
int f1(void) __attribute__((pure, noreturn));
int f3(void) __attribute__((always_inline, noinline));
int v1 __attribute__((common, nocommon));
#define YES(n, e, a) extern char n[__builtin_has_attribute (e, a) ? 1 : -1]
#define NO(n, e, a) extern char n[__builtin_has_attribute (e, a) ? -1 : 1]
NO(a1, f1, noreturn); YES(a2, f1, pure);
YES(c1, f3, always_inline); NO(c2, f3, noinline);
YES(d1, v1, common); NO(d2, v1, nocommon);
void *g(size_t, size_t) __attribute__((alloc_size(1)));
void *g(size_t, size_t) __attribute__((alloc_size(2)));
YES(e1, g, alloc_size(1));
YES(e2, g, alloc_size(2));
