/* header-guard fix-its (docs/C_DESIGN.md 4.2, Q9): the #define name becomes the
   #ifndef name; SAFE, but MAYBE when the misspelt name is tested anywhere else
   in the unit.  The edits are in the headers. */
#include "inc/guard_safe.h" /* expect-fixit: inc/guard_safe.h 2:9-2:22 "GUARD_SAFE_H" safe */
#include "inc/guard_maybe.h" /* expect-fixit: inc/guard_maybe.h 2:9-2:23 "GUARD_MAYBE_H" maybe */
#ifdef GUARD_MAYBE_HH
int tested;
#endif
